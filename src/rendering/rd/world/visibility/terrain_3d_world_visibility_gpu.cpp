// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/visibility/terrain_3d_world_hiz.h"
#include "rendering/rd/world/visibility/terrain_3d_world_visibility_gpu.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_binding_builder.h"

#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <math.h>
#include <string.h>

using namespace godot;

Terrain3DWorldVisibilityGpu::Terrain3DWorldVisibilityGpu(
	Terrain3DRdContext &rContext ) :
	m_context( rContext ),
	m_pipelineCache( rContext, T3D_RD_CONTEXT_OWNER_WORLD ),
	m_descriptorCache( rContext, T3D_RD_CONTEXT_OWNER_WORLD ) {
}

Terrain3DWorldVisibilityGpu::~Terrain3DWorldVisibilityGpu(
	void ) {
	shutdownCold();
}

bool Terrain3DWorldVisibilityGpu::createPipelineCold(
	const t3dVisibilityPipeline_t ePipeline ) {
	if ( !m_context.getDevice() || ePipeline >= PIPELINE_COUNT ) {
		return false;
	}
	const String shader_name = String( "T3D.World.Visibility." ) + String::num_uint64( ePipeline );
	const String registry_name = String( "T3D.World.Visibility.Shader." ) + String::num_uint64( ePipeline );
	static const t3dRdShaderId_t s_ids[PIPELINE_COUNT] = { T3D_RD_SHADER_WORLD_VISIBILITY_RESET,
														   T3D_RD_SHADER_WORLD_VISIBILITY_CULL,
														   T3D_RD_SHADER_WORLD_VISIBILITY_FINALIZE,
														   T3D_RD_SHADER_WORLD_VISIBILITY_CULL_SMALL,
                                                           T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_CLASSIFY,
                                                           T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_PREFIX,
                                                           T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_SCATTER,
                                                           T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_FINALIZE };
	const t3dRdShaderId_t shader_id =
		ePipeline == PIPELINE_CULL && Terrain3DRdShaderLibrary::isGroupedCullEnabledCold()
			? T3D_RD_SHADER_WORLD_VISIBILITY_CULL_GROUPED
			: s_ids[ePipeline];
	if ( !m_context.initializeSharedArenas() ) {
		return false;
	}
	m_context.markHotShader( Terrain3DRdShaderLibrary::borrowSpirv( shader_id ),
							 ( ePipeline == PIPELINE_RESET || ePipeline == PIPELINE_FINALIZE ||
                               ePipeline == PIPELINE_SCAN_FINALIZE ) ? 2 : 0 );
	if ( !m_hShader[ePipeline]
			  .createTracked( m_context, T3D_RD_CONTEXT_OWNER_WORLD, shader_name, shader_id, registry_name ) ) {
		return false;
	}
	m_hPipeline[ePipeline] = m_pipelineCache.getOrCreateCompute( m_hShader[ePipeline].rid(),
																 String( "T3D.World.Visibility.Pipeline." ) +
																	 String::num_uint64( ePipeline ),
																 m_hShader[ePipeline].rid() );
	return m_hPipeline[ePipeline].is_valid();
}

bool Terrain3DWorldVisibilityGpu::initializeCold(
	void ) {
	if ( m_bInitialized ) {
		return true;
	}
	if ( !createPipelineCold( PIPELINE_RESET ) || !createPipelineCold( PIPELINE_CULL ) ||
		 !createPipelineCold( PIPELINE_FINALIZE ) ||
		 ( Terrain3DRdShaderLibrary::isWorldSmallCullEnabledCold() && !createPipelineCold( PIPELINE_CULL_SMALL ) ) ||
         ( Terrain3DRdShaderLibrary::isWorldScanEnabledCold() &&
           ( !createPipelineCold( PIPELINE_SCAN_CLASSIFY ) || !createPipelineCold( PIPELINE_SCAN_PREFIX ) ||
             !createPipelineCold( PIPELINE_SCAN_SCATTER ) || !createPipelineCold( PIPELINE_SCAN_FINALIZE ) ) ) ) {
		shutdownCold();
		return false;
	}
    if ( Terrain3DRdShaderLibrary::isWorldScanEnabledCold() ) {
        t3dRdGpuBufferSpec_t spec;
        spec.m_pName = "T3D.World.Visibility.ScanScratch";
        spec.m_owner = T3D_RD_CONTEXT_OWNER_WORLD;
        spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
        spec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
        if ( !m_scanScratch.configure( spec ) ||
             !m_scanScratch.createTracked( m_context, SCAN_WORDS * sizeof( uint32_t ) ) ) {
            shutdownCold();
            return false;
        }
    }
	m_pushBytes.resize( sizeof( t3dVisibilityPush_t ) );
	m_bInitialized = true;
	return true;
}

void Terrain3DWorldVisibilityGpu::freeSetsCold(
	void ) {
	m_descriptorCache.clear();
	for ( uint32_t i = 0u; i < PIPELINE_COUNT; ++i ) {
		m_hSet[i] = RID();
	}
	m_boundSignature = {};
}

bool Terrain3DWorldVisibilityGpu::createSetsCold(
	Terrain3DWorldResources &rResources,
	const Terrain3DWorldHiZ &rHiZ ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !rResources.isInitialized() || !rHiZ.getTexture().is_valid() || !rHiZ.getSampler().is_valid() ) {
		return false;
	}
	freeSetsCold();
	const t3dDescriptorKey_t::Signature signature{ rResources.getResourceGeneration(), rHiZ.getResourceGeneration() };
	TypedArray< Ref< RDUniform > >		reset;
	reset.push_back(
		Terrain3DRdBindingBuilder::storage( 0, rResources.getBuffer( WORLD_BUFFER_VISIBILITY_COUNTERS ) ) );
	reset.push_back( Terrain3DRdBindingBuilder::storage( 1, rResources.getBuffer( WORLD_BUFFER_INDIRECT ) ) );
	reset.push_back( Terrain3DRdBindingBuilder::single( 2,
														m_context.arena( Terrain3DRdContext::HOT ).isPersistent()
															? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC
															: RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
														rResources.getBuffer( WORLD_BUFFER_FRAME ) ) );
	m_hSet[PIPELINE_RESET] = m_descriptorCache.getOrCreate( { m_hShader[PIPELINE_RESET].rid(), signature },
															"T3D.World.Visibility.Set.Reset",
															reset,
															m_hShader[PIPELINE_RESET].rid(),
															0u );

	TypedArray< Ref< RDUniform > > cull;
	cull.push_back( Terrain3DRdBindingBuilder::single( 0,
													   m_context.arena( Terrain3DRdContext::HOT ).isPersistent()
														   ? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC
														   : RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
													   rResources.getBuffer( WORLD_BUFFER_FRAME ) ) );
	cull.push_back(
		Terrain3DRdBindingBuilder::storage( 1, rResources.getBuffer( WORLD_BUFFER_VISIBILITY_CANDIDATES ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 2, rResources.getBuffer( WORLD_BUFFER_CLUSTERS ) ) );
	cull.push_back(
		Terrain3DRdBindingBuilder::storage( 3, rResources.getBuffer( WORLD_BUFFER_TERRAIN_STRIP_CLUSTER_BOUNDS ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 4, rResources.getBuffer( WORLD_BUFFER_TRANSFORMS_CURRENT ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 5, rResources.getBuffer( WORLD_BUFFER_INSTANCES ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 6, rResources.getBuffer( WORLD_BUFFER_VISIBLE_ITEMS ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 7, rResources.getBuffer( WORLD_BUFFER_VISIBILITY_COUNTERS ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 8, rResources.getBuffer( WORLD_BUFFER_INDIRECT ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::samplerTexture( 9, rHiZ.getSampler(), rHiZ.getTexture() ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 10, rResources.getBuffer( WORLD_BUFFER_MATERIALS ) ) );
	cull.push_back(
		Terrain3DRdBindingBuilder::storage( 11, rResources.getBuffer( WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS ) ) );
	m_hSet[PIPELINE_CULL] = m_descriptorCache.getOrCreate( { m_hShader[PIPELINE_CULL].rid(), signature },
														   "T3D.World.Visibility.Set.Cull",
														   cull,
														   m_hShader[PIPELINE_CULL].rid(),
														   0u );

    if ( Terrain3DRdShaderLibrary::isWorldSmallCullEnabledCold() ) {
        m_hSet[PIPELINE_CULL_SMALL] = m_descriptorCache.getOrCreate(
            { m_hShader[PIPELINE_CULL_SMALL].rid(), signature },
            "T3D.World.Visibility.Set.CullSmall", cull,
            m_hShader[PIPELINE_CULL_SMALL].rid(), 0u );
    }

    if ( Terrain3DRdShaderLibrary::isWorldScanEnabledCold() ) {
        if ( !m_scanScratch.isValid() ) {
            freeSetsCold();
            return false;
        }
        TypedArray< Ref< RDUniform > > scan_classify;
        for ( int i = 0; i < cull.size(); ++i ) {
            scan_classify.push_back( cull[i] );
        }
        scan_classify.push_back( Terrain3DRdBindingBuilder::storage( 12, m_scanScratch.rid() ) );
        m_hSet[PIPELINE_SCAN_CLASSIFY] = m_descriptorCache.getOrCreate(
            { m_hShader[PIPELINE_SCAN_CLASSIFY].rid(), signature },
            "T3D.World.Visibility.Set.ScanClassify", scan_classify,
            m_hShader[PIPELINE_SCAN_CLASSIFY].rid(), 0u );
        TypedArray< Ref< RDUniform > > scan_prefix;
        scan_prefix.push_back( Terrain3DRdBindingBuilder::single( 0,
            m_context.arena( Terrain3DRdContext::HOT ).isPersistent()
                ? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC
                : RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
            rResources.getBuffer( WORLD_BUFFER_FRAME ) ) );
        scan_prefix.push_back( Terrain3DRdBindingBuilder::storage( 7, rResources.getBuffer( WORLD_BUFFER_VISIBILITY_COUNTERS ) ) );
        scan_prefix.push_back( Terrain3DRdBindingBuilder::storage( 12, m_scanScratch.rid() ) );
        m_hSet[PIPELINE_SCAN_PREFIX] = m_descriptorCache.getOrCreate(
            { m_hShader[PIPELINE_SCAN_PREFIX].rid(), signature },
            "T3D.World.Visibility.Set.ScanPrefix", scan_prefix,
            m_hShader[PIPELINE_SCAN_PREFIX].rid(), 0u );
        TypedArray< Ref< RDUniform > > scan_scatter;
        scan_scatter.push_back( Terrain3DRdBindingBuilder::single( 0,
            m_context.arena( Terrain3DRdContext::HOT ).isPersistent()
                ? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC
                : RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
            rResources.getBuffer( WORLD_BUFFER_FRAME ) ) );
        scan_scatter.push_back( Terrain3DRdBindingBuilder::storage( 1, rResources.getBuffer( WORLD_BUFFER_VISIBILITY_CANDIDATES ) ) );
        scan_scatter.push_back( Terrain3DRdBindingBuilder::storage( 6, rResources.getBuffer( WORLD_BUFFER_VISIBLE_ITEMS ) ) );
        scan_scatter.push_back( Terrain3DRdBindingBuilder::storage( 12, m_scanScratch.rid() ) );
        m_hSet[PIPELINE_SCAN_SCATTER] = m_descriptorCache.getOrCreate(
            { m_hShader[PIPELINE_SCAN_SCATTER].rid(), signature },
            "T3D.World.Visibility.Set.ScanScatter", scan_scatter,
            m_hShader[PIPELINE_SCAN_SCATTER].rid(), 0u );
    }

	TypedArray< Ref< RDUniform > > finalize;
	finalize.push_back(
		Terrain3DRdBindingBuilder::storage( 0, rResources.getBuffer( WORLD_BUFFER_VISIBILITY_COUNTERS ) ) );
	finalize.push_back( Terrain3DRdBindingBuilder::storage( 1, rResources.getBuffer( WORLD_BUFFER_INDIRECT ) ) );
	finalize.push_back( Terrain3DRdBindingBuilder::single( 2,
														   m_context.arena( Terrain3DRdContext::HOT ).isPersistent()
															   ? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC
															   : RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
														   rResources.getBuffer( WORLD_BUFFER_FRAME ) ) );
	m_hSet[PIPELINE_FINALIZE] = m_descriptorCache.getOrCreate( { m_hShader[PIPELINE_FINALIZE].rid(), signature },
															   "T3D.World.Visibility.Set.Finalize",
															   finalize,
															   m_hShader[PIPELINE_FINALIZE].rid(),
															   0u );
    if ( Terrain3DRdShaderLibrary::isWorldScanEnabledCold() ) {
        TypedArray< Ref< RDUniform > > scan_finalize;
        for ( int i = 0; i < finalize.size(); ++i ) {
            scan_finalize.push_back( finalize[i] );
        }
        scan_finalize.push_back( Terrain3DRdBindingBuilder::storage( 12, m_scanScratch.rid() ) );
        m_hSet[PIPELINE_SCAN_FINALIZE] = m_descriptorCache.getOrCreate(
            { m_hShader[PIPELINE_SCAN_FINALIZE].rid(), signature },
            "T3D.World.Visibility.Set.ScanFinalize", scan_finalize,
            m_hShader[PIPELINE_SCAN_FINALIZE].rid(), 0u );
    }
	for ( uint32_t i = 0u; i < PIPELINE_COUNT; ++i ) {
		if ( i >= PIPELINE_SCAN_CLASSIFY && !Terrain3DRdShaderLibrary::isWorldScanEnabledCold() ) {
            continue;
        }
		if ( i == PIPELINE_CULL_SMALL && !Terrain3DRdShaderLibrary::isWorldSmallCullEnabledCold() ) {
            continue;
        }
        if ( !m_hSet[i].is_valid() ) {
			freeSetsCold();
			return false;
		}
	}
	m_boundSignature = signature;
	++m_nSetRebuildCount;
	return true;
}

bool Terrain3DWorldVisibilityGpu::prepareBindingsCold(
	Terrain3DWorldResources &rResources,
	const Terrain3DWorldHiZ &rHiZ ) {
	if ( !m_bInitialized ) {
		return false;
	}
	if ( m_boundSignature ==
			 t3dDescriptorKey_t::Signature{ rResources.getResourceGeneration(), rHiZ.getResourceGeneration() } &&
		 m_hSet[PIPELINE_CULL].is_valid() &&
		 ( !Terrain3DRdShaderLibrary::isWorldSmallCullEnabledCold() || m_hSet[PIPELINE_CULL_SMALL].is_valid() ) &&
         ( !Terrain3DRdShaderLibrary::isWorldScanEnabledCold() ||
           ( m_scanScratch.isValid() && m_hSet[PIPELINE_SCAN_CLASSIFY].is_valid() &&
             m_hSet[PIPELINE_SCAN_PREFIX].is_valid() && m_hSet[PIPELINE_SCAN_SCATTER].is_valid() &&
             m_hSet[PIPELINE_SCAN_FINALIZE].is_valid() ) ) ) {
		return true;
	}
	return createSetsCold( rResources, rHiZ );
}

bool Terrain3DWorldVisibilityGpu::dispatchHot(
	Terrain3DWorldResources	   &rResources,
	const Terrain3DWorldHiZ	   &rHiZ,
	const t3dRdFrameSnapshot_t &rSnapshot,
	const uint32_t				nCandidateCount,
	const uint32_t				nVisibilityMask,
	const uint32_t				nPassKind,
	const bool					bHiZValid ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !isReady() || rSnapshot.m_nPatchCount > T3D_MAX_CLIPMAP_LEVELS ||
		 !( m_boundSignature ==
			t3dDescriptorKey_t::Signature{ rResources.getResourceGeneration(), rHiZ.getResourceGeneration() } ) ) {
		++m_nFailureCount;
		return false;
	}
	if ( m_nPatchBoundsGeneration != rSnapshot.m_nGeneration || m_nCachedPatchCount != rSnapshot.m_nPatchCount ) {
		memset( m_szCachedPatchBounds, 0, sizeof( m_szCachedPatchBounds ) );
		for ( uint32_t i = 0u; i < rSnapshot.m_nPatchCount; ++i ) {
			const t3dRdPatch_t			   &patch = rSnapshot.m_szPatches[i];
			t3dWorldTerrainClusterBounds_t &bound = m_szCachedPatchBounds[i];
			bound.m_fCenter[0] = patch.m_fOriginX;
			bound.m_fCenter[1] = ( patch.m_fBoundsMinY + patch.m_fBoundsMaxY ) * 0.5f;
			bound.m_fCenter[2] = patch.m_fOriginZ;
			const float half_y = ( patch.m_fBoundsMaxY - patch.m_fBoundsMinY ) * 0.5f;
			bound.m_fRadius = sqrtf( patch.m_fBoundsHalfExtent * patch.m_fBoundsHalfExtent * 2.0f + half_y * half_y );
		}
		m_nPatchBoundsGeneration = rSnapshot.m_nGeneration;
		m_nCachedPatchCount = rSnapshot.m_nPatchCount;
	}
	if ( !m_context.writeHotRecord( offsetof( Terrain3DRdHotRecord, bounds ),
									m_szCachedPatchBounds,
									sizeof( m_szCachedPatchBounds ) ) ) {
		++m_nFailureCount;
		return false;
	}
	const uint32_t capacity =
		rResources.getCapacityBytes( WORLD_BUFFER_VISIBLE_ITEMS ) / sizeof( t3dWorldVisibleItem_t );
	const uint32_t		candidate_count = MIN( nCandidateCount, capacity );
	t3dVisibilityPush_t push{};
	push.m_nCandidateCount = candidate_count;
	push.m_nVisibilityMask = nVisibilityMask;
	push.m_nFlags = ( bHiZValid ? 1u : 0u ) |
                    ( Terrain3DRdShaderLibrary::isEnvironmentFieldsEnabledCold() ? 2u : 0u );
	push.m_nPassKind = nPassKind;
	push.m_nHotIndex = rResources.hotIndex();
	for ( uint32_t nBucket = 0u; nBucket < T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT; ++nBucket ) {
		push.m_nTerrainBucketCounts[nBucket] = rResources.getTerrainStripBucketCount( nBucket );
	}
	memcpy( m_pushBytes.ptrw(), &push, sizeof( push ) );
	const int64_t list = rd->compute_list_begin();
	if ( list == RenderingDevice::INVALID_ID ) {
		++m_nFailureCount;
		return false;
	}
	rd->compute_list_bind_compute_pipeline( list, m_hPipeline[PIPELINE_RESET] );
	const RID descriptorRESET = m_descriptorCache.resolve( m_hSet[PIPELINE_RESET] );
	if ( !descriptorRESET.is_valid() ) {
		rd->compute_list_end();
		return false;
	}
	rd->compute_list_bind_uniform_set( list, descriptorRESET, 0u );
	rd->compute_list_set_push_constant( list, m_pushBytes, sizeof( push ) );
	rd->compute_list_dispatch( list, 1u, 1u, 1u );
	++m_nDispatchCount;
	rd->compute_list_add_barrier( list );
    if ( candidate_count > 0u ) {
        const bool use_small = Terrain3DRdShaderLibrary::isWorldSmallCullEnabledCold() && candidate_count <= 64u;
        const bool use_scan = !use_small && Terrain3DRdShaderLibrary::isWorldScanEnabledCold() &&
                              candidate_count <= SCAN_MAX_CANDIDATES && m_scanScratch.isValid();
        const t3dVisibilityPipeline_t passes[3] = { PIPELINE_SCAN_CLASSIFY, PIPELINE_SCAN_PREFIX, PIPELINE_SCAN_SCATTER };
        const uint32_t scan_pass_count = use_scan ? 3u : 1u;
        for ( uint32_t pass = 0u; pass < scan_pass_count; ++pass ) {
            const t3dVisibilityPipeline_t pipeline = use_scan ? passes[pass]
                : ( use_small ? PIPELINE_CULL_SMALL : PIPELINE_CULL );
            rd->compute_list_bind_compute_pipeline( list, m_hPipeline[pipeline] );
            const RID descriptor = m_descriptorCache.resolve( m_hSet[pipeline] );
            if ( !descriptor.is_valid() ) {
                rd->compute_list_end();
                ++m_nFailureCount;
                return false;
            }
            rd->compute_list_bind_uniform_set( list, descriptor, 0u );
            rd->compute_list_set_push_constant( list, m_pushBytes, sizeof( push ) );
            rd->compute_list_dispatch( list, use_scan && pass == 1u ? 1u : ( candidate_count + 63u ) / 64u, 1u, 1u );
            ++m_nDispatchCount;
            rd->compute_list_add_barrier( list );
        }
        // A distinct finalizer consumes per-group accepted-vertex/kind totals.
        if ( use_scan ) {
            rd->compute_list_bind_compute_pipeline( list, m_hPipeline[PIPELINE_SCAN_FINALIZE] );
            const RID descriptor = m_descriptorCache.resolve( m_hSet[PIPELINE_SCAN_FINALIZE] );
            if ( !descriptor.is_valid() ) {
                rd->compute_list_end();
                ++m_nFailureCount;
                return false;
            }
            rd->compute_list_bind_uniform_set( list, descriptor, 0u );
            rd->compute_list_set_push_constant( list, m_pushBytes, sizeof( push ) );
            rd->compute_list_dispatch( list, 1u, 1u, 1u );
            ++m_nDispatchCount;
            rd->compute_list_add_barrier( list );
            rd->compute_list_end();
            m_nLastCandidateCount = candidate_count;
            ++m_nPreparedFrameCount;
            return true;
        }
    }
	rd->compute_list_bind_compute_pipeline( list, m_hPipeline[PIPELINE_FINALIZE] );
	const RID descriptorFINALIZE = m_descriptorCache.resolve( m_hSet[PIPELINE_FINALIZE] );
	if ( !descriptorFINALIZE.is_valid() ) {
		rd->compute_list_end();
		return false;
	}
	rd->compute_list_bind_uniform_set( list, descriptorFINALIZE, 0u );
	rd->compute_list_set_push_constant( list, m_pushBytes, sizeof( push ) );
	rd->compute_list_dispatch( list, 1u, 1u, 1u );
	++m_nDispatchCount;
	rd->compute_list_add_barrier( list );
	rd->compute_list_end();
	m_nLastCandidateCount = candidate_count;
	++m_nPreparedFrameCount;
	return true;
}

void Terrain3DWorldVisibilityGpu::shutdownCold(
	void ) {
	freeSetsCold();
    m_scanScratch.releaseTracked( m_context );
	m_pipelineCache.clear();
	for ( uint32_t i = 0u; i < PIPELINE_COUNT; ++i ) {
		m_hPipeline[i] = RID();
		m_hShader[i].releaseTracked( m_context );
	}

	m_pushBytes.clear();
	m_nPatchBoundsGeneration = 0u;
	m_nCachedPatchCount = UINT32_MAX;
	m_bInitialized = false;
}

bool Terrain3DWorldVisibilityGpu::isReady(
	void ) const {
	return m_bInitialized && m_context.arena( Terrain3DRdContext::HOT ).is_valid() &&
		   m_hPipeline[PIPELINE_RESET].is_valid() && m_hPipeline[PIPELINE_CULL].is_valid() &&
		   m_hPipeline[PIPELINE_FINALIZE].is_valid() && m_hSet[PIPELINE_RESET].is_valid() &&
		   m_hSet[PIPELINE_CULL].is_valid() && m_hSet[PIPELINE_FINALIZE].is_valid() &&
           ( !Terrain3DRdShaderLibrary::isWorldSmallCullEnabledCold() ||
             ( m_hPipeline[PIPELINE_CULL_SMALL].is_valid() && m_hSet[PIPELINE_CULL_SMALL].is_valid() ) ) &&
           ( !Terrain3DRdShaderLibrary::isWorldScanEnabledCold() ||
             ( m_scanScratch.isValid() &&
               m_hPipeline[PIPELINE_SCAN_CLASSIFY].is_valid() && m_hSet[PIPELINE_SCAN_CLASSIFY].is_valid() &&
               m_hPipeline[PIPELINE_SCAN_PREFIX].is_valid() && m_hSet[PIPELINE_SCAN_PREFIX].is_valid() &&
               m_hPipeline[PIPELINE_SCAN_SCATTER].is_valid() && m_hSet[PIPELINE_SCAN_SCATTER].is_valid() &&
               m_hPipeline[PIPELINE_SCAN_FINALIZE].is_valid() && m_hSet[PIPELINE_SCAN_FINALIZE].is_valid() ) );
}
uint64_t Terrain3DWorldVisibilityGpu::getDispatchCount(
	void ) const {
	return m_nDispatchCount;
}
uint64_t Terrain3DWorldVisibilityGpu::getPreparedFrameCount(
	void ) const {
	return m_nPreparedFrameCount;
}
uint64_t Terrain3DWorldVisibilityGpu::getSetRebuildCount(
	void ) const {
	return m_nSetRebuildCount;
}
uint64_t Terrain3DWorldVisibilityGpu::getFailureCount(
	void ) const {
	return m_nFailureCount;
}
uint32_t Terrain3DWorldVisibilityGpu::getLastCandidateCount(
	void ) const {
	return m_nLastCandidateCount;
}

// -------- Consolidated from rendering/rd/world/visibility/terrain_3d_world_hiz.cpp --------
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
using namespace godot;

Terrain3DWorldHiZ::Terrain3DWorldHiZ(
	Terrain3DRdContext &rContext ) :
	m_context( rContext ),
	m_reducer( rContext, T3D_RD_CONTEXT_OWNER_WORLD ) {
}

Terrain3DWorldHiZ::~Terrain3DWorldHiZ(
	void ) {
	shutdownCold();
}

void Terrain3DWorldHiZ::writePush(
	void *,
	const t3dRdHiZDispatchInfo_t &rInfo,
	uint8_t *const				  pDestination,
	const uint32_t				  nDestinationBytes ) {
	if ( !pDestination || nDestinationBytes != sizeof( t3dWorldHiZReducePush_t ) ) {
		return;
	}
	const t3dWorldHiZReducePush_t push = { rInfo.m_nSourceWidth,
										   rInfo.m_nSourceHeight,
										   rInfo.m_nDestinationWidth,
										   rInfo.m_nDestinationHeight };
	memcpy( pDestination, &push, sizeof( push ) );
}

bool Terrain3DWorldHiZ::initializeCold(
	const t3dWorldResourceLimits_t &rLimits ) {
	const uint32_t max_width = MAX( rLimits.m_nHiZMaxWidth, 1u );
	const uint32_t max_height = MAX( rLimits.m_nHiZMaxHeight, 1u );
	const uint32_t max_mips = CLAMP( rLimits.m_nHiZMaxMipCount, 1u, T3D_RD_HIZ_ABSOLUTE_MAX_MIP_COUNT );
	if ( !m_reducer.initializeCold( T3D_RD_SHADER_WORLD_HIZ_REDUCE,
									"T3D.World.HiZ",
									max_width,
									max_height,
									max_mips,
									0u,
									0u,
									1u,
									false,
									sizeof( t3dWorldHiZReducePush_t ) ) ) {
		return false;
	}
	return true;
}

void Terrain3DWorldHiZ::shutdownCold(
	void ) {
	m_reducer.shutdownCold();
}

bool Terrain3DWorldHiZ::bindPreviousDepthCold(
	const RID &rDepth ) {
	bool rebound = false;
	if ( !m_reducer.bindSourceCold( rDepth, RID(), rebound ) ) {
		++m_nUnavailableFrames;
		return false;
	}
	if ( rebound ) {
		m_context.recordRenderEvent( T3D_RD_CONTEXT_OWNER_WORLD, EVENT_HIZ_REBIND );
	}
	return true;
}

bool Terrain3DWorldHiZ::buildHot(
	const Vector2i &rRenderSize ) {
	if ( !m_reducer.buildHot( rRenderSize, &Terrain3DWorldHiZ::writePush, this ) ) {
		++m_nUnavailableFrames;
		return false;
	}
	m_context.recordRenderEvent( T3D_RD_CONTEXT_OWNER_WORLD, EVENT_HIZ_BUILD );
	return true;
}

bool Terrain3DWorldHiZ::isReady(
	void ) const {
	return m_reducer.isReady();
}
const RID &Terrain3DWorldHiZ::getTexture(
	void ) const {
	return m_reducer.getTexture();
}
const RID &Terrain3DWorldHiZ::getSampler(
	void ) const {
	return m_reducer.getSampler();
}
uint32_t Terrain3DWorldHiZ::getWidth(
	void ) const {
	return m_reducer.getWidth();
}
uint32_t Terrain3DWorldHiZ::getHeight(
	void ) const {
	return m_reducer.getHeight();
}
uint32_t Terrain3DWorldHiZ::getMipCount(
	void ) const {
	return m_reducer.getMipCount();
}
uint64_t Terrain3DWorldHiZ::getResourceGeneration(
	void ) const {
	return m_reducer.getResourceGeneration();
}
uint64_t Terrain3DWorldHiZ::getBuildCount(
	void ) const {
	return m_context.getRenderEvent( T3D_RD_CONTEXT_OWNER_WORLD, EVENT_HIZ_BUILD );
}
uint64_t Terrain3DWorldHiZ::getBuildDispatches(
	void ) const {
	return m_reducer.getBuildDispatches();
}
uint64_t Terrain3DWorldHiZ::getSourceRebinds(
	void ) const {
	return m_context.getRenderEvent( T3D_RD_CONTEXT_OWNER_WORLD, EVENT_HIZ_REBIND );
}
uint64_t Terrain3DWorldHiZ::getUnavailableFrames(
	void ) const {
	return m_nUnavailableFrames;
}
uint64_t Terrain3DWorldHiZ::getGpuBytes(
	void ) const {
	return m_reducer.getGpuBytes();
}
