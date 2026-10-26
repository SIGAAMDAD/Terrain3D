// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/terrain_3d_world_visibility_gpu.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_binding_builder.h"

#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <math.h>
#include <string.h>

using namespace godot;

Terrain3DWorldVisibilityGpu::Terrain3DWorldVisibilityGpu( Terrain3DRdContext &rContext ) :
        m_context( rContext ), m_pipelineCache( rContext, Terrain3DRdContext::OWNER_WORLD ),
        m_descriptorCache( rContext, Terrain3DRdContext::OWNER_WORLD ) {
}

Terrain3DWorldVisibilityGpu::~Terrain3DWorldVisibilityGpu( void ) {
	shutdownCold();
}

bool Terrain3DWorldVisibilityGpu::createPipelineCold( const t3dPipeline_t ePipeline ) {
	if ( !m_context.getDevice() || ePipeline >= PIPELINE_COUNT || m_source[ePipeline].is_null() ) {
		return false;
	}
	const String shader_name = String( "T3D.World.Visibility." ) + String::num_uint64( ePipeline );
	const String registry_name = String( "T3D.World.Visibility.Shader." ) + String::num_uint64( ePipeline );
	if ( !m_hShader[ePipeline].createTracked(
	             m_context, Terrain3DRdContext::OWNER_WORLD, shader_name, m_source[ePipeline], registry_name ) ) {
		return false;
	}
	m_hPipeline[ePipeline] = m_pipelineCache.getOrCreateCompute( m_hShader[ePipeline].rid(),
	        String( "T3D.World.Visibility.Pipeline." ) + String::num_uint64( ePipeline ),
	        m_hShader[ePipeline].rid() );
	return m_hPipeline[ePipeline].is_valid();
}

bool Terrain3DWorldVisibilityGpu::initializeCold(
        const Ref<RDShaderSPIRV> &rReset, const Ref<RDShaderSPIRV> &rCull, const Ref<RDShaderSPIRV> &rFinalize ) {
	if ( m_bInitialized ) {
		return true;
	}
	m_source[PIPELINE_RESET] = rReset;
	m_source[PIPELINE_CULL] = rCull;
	m_source[PIPELINE_FINALIZE] = rFinalize;
	if ( rReset.is_null() || rCull.is_null() || rFinalize.is_null() || !createPipelineCold( PIPELINE_RESET )
	        || !createPipelineCold( PIPELINE_CULL ) || !createPipelineCold( PIPELINE_FINALIZE ) ) {
		shutdownCold();
		return false;
	}
	const uint32_t patch_bytes = sizeof( t3dWorldTerrainClusterBounds_t ) * s_nMaxClipmapLevels;
	t3dRdGpuBufferSpec_t patch_spec;
	patch_spec.m_pName = "T3D.World.Visibility.PatchBounds";
	patch_spec.m_owner = Terrain3DRdContext::OWNER_WORLD;
	patch_spec.m_type = Terrain3DRdContext::RESOURCE_STORAGE_BUFFER;
	patch_spec.m_memory = Terrain3DRdContext::MEMORY_PATCH;
	if ( !m_hPatchBoundsBuffer.configure( patch_spec ) || !m_hPatchBoundsBuffer.createTracked( m_context, patch_bytes ) ) {
		shutdownCold();
		return false;
	}
	m_patchBoundsBytes.resize( patch_bytes );
	m_pushBytes.resize( sizeof( t3dVisibilityPush_t ) );
	m_bInitialized = true;
	return true;
}

void Terrain3DWorldVisibilityGpu::freeSetsCold( void ) {
	m_descriptorCache.clear();
	for ( uint32_t i = 0u; i < PIPELINE_COUNT; ++i ) {
		m_hSet[i] = RID();
	}
	m_boundSignature = {};
}

bool Terrain3DWorldVisibilityGpu::createSetsCold( Terrain3DWorldResources &rResources, const Terrain3DWorldHiZ &rHiZ ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !rResources.isInitialized() || !rHiZ.getTexture().is_valid() || !rHiZ.getSampler().is_valid() ) {
		return false;
	}
	freeSetsCold();
	const t3dDescriptorKey_t::Signature signature { rResources.getResourceGeneration(), rHiZ.getResourceGeneration() };
	TypedArray<Ref<RDUniform>> reset;
	reset.push_back( Terrain3DRdBindingBuilder::storage( 0, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_VISIBILITY_COUNTERS ) ) );
	reset.push_back( Terrain3DRdBindingBuilder::storage( 1, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_INDIRECT ) ) );
	reset.push_back( Terrain3DRdBindingBuilder::storage( 2, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_FRAME ) ) );
	m_hSet[PIPELINE_RESET] = m_descriptorCache.getOrCreate( { m_hShader[PIPELINE_RESET].rid(), signature },
	        "T3D.World.Visibility.Set.Reset", reset, m_hShader[PIPELINE_RESET].rid(), 0u );

	TypedArray<Ref<RDUniform>> cull;
	cull.push_back( Terrain3DRdBindingBuilder::storage( 0, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_FRAME ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 1, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_VISIBILITY_CANDIDATES ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 2, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_CLUSTERS ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 3, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_TERRAIN_STRIP_CLUSTER_BOUNDS ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 4, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_TRANSFORMS_CURRENT ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 5, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_INSTANCES ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 6, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_VISIBLE_ITEMS ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 7, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_VISIBILITY_COUNTERS ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 8, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_INDIRECT ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::samplerTexture( 9, rHiZ.getSampler(), rHiZ.getTexture() ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 10, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_MATERIALS ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 11, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS ) ) );
	cull.push_back( Terrain3DRdBindingBuilder::storage( 12, m_hPatchBoundsBuffer ) );
	m_hSet[PIPELINE_CULL] = m_descriptorCache.getOrCreate( { m_hShader[PIPELINE_CULL].rid(), signature },
	        "T3D.World.Visibility.Set.Cull", cull, m_hShader[PIPELINE_CULL].rid(), 0u );

	TypedArray<Ref<RDUniform>> finalize;
	finalize.push_back( Terrain3DRdBindingBuilder::storage( 0, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_VISIBILITY_COUNTERS ) ) );
	finalize.push_back( Terrain3DRdBindingBuilder::storage( 1, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_INDIRECT ) ) );
	finalize.push_back( Terrain3DRdBindingBuilder::storage( 2, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_FRAME ) ) );
	m_hSet[PIPELINE_FINALIZE] = m_descriptorCache.getOrCreate( { m_hShader[PIPELINE_FINALIZE].rid(), signature },
	        "T3D.World.Visibility.Set.Finalize", finalize, m_hShader[PIPELINE_FINALIZE].rid(), 0u );
	for ( uint32_t i = 0u; i < PIPELINE_COUNT; ++i ) {
		if ( !m_hSet[i].is_valid() ) {
			freeSetsCold();
			return false;
		}
	}
	m_boundSignature = signature;
	++m_nSetRebuildCount;
	return true;
}

bool Terrain3DWorldVisibilityGpu::prepareBindingsCold( Terrain3DWorldResources &rResources, const Terrain3DWorldHiZ &rHiZ ) {
	if ( !m_bInitialized ) {
		return false;
	}
	if ( m_boundSignature == t3dDescriptorKey_t::Signature{ rResources.getResourceGeneration(), rHiZ.getResourceGeneration() }
	        && m_hSet[PIPELINE_CULL].is_valid() ) {
		return true;
	}
	return createSetsCold( rResources, rHiZ );
}

bool Terrain3DWorldVisibilityGpu::dispatchHot( Terrain3DWorldResources &rResources,
        const Terrain3DWorldHiZ &rHiZ,
        const t3dRdFrameSnapshot_t &rSnapshot,
        const uint32_t nCandidateCount,
        const uint32_t nVisibilityMask,
        const uint32_t nPassKind,
        const bool bHiZValid ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !isReady()
	        || !( m_boundSignature == t3dDescriptorKey_t::Signature{ rResources.getResourceGeneration(), rHiZ.getResourceGeneration() } ) ) {
		++m_nFailureCount;
		return false;
	}
	if ( m_nPatchBoundsGeneration != rSnapshot.m_nGeneration ) {
		t3dWorldTerrainClusterBounds_t patch_bounds[s_nMaxClipmapLevels]{};
		for ( uint32_t i = 0u; i < rSnapshot.m_nPatchCount; ++i ) {
			const t3dRdPatch_t &patch = rSnapshot.m_szPatches[i];
			t3dWorldTerrainClusterBounds_t &bound = patch_bounds[i];
			bound.m_fCenter[0] = patch.m_fOriginX;
			bound.m_fCenter[1] = ( patch.m_fBoundsMinY + patch.m_fBoundsMaxY ) * 0.5f;
			bound.m_fCenter[2] = patch.m_fOriginZ;
			const float half_y = ( patch.m_fBoundsMaxY - patch.m_fBoundsMinY ) * 0.5f;
			bound.m_fRadius = sqrtf( patch.m_fBoundsHalfExtent * patch.m_fBoundsHalfExtent * 2.0f + half_y * half_y );
		}
		const uint32_t patch_bytes = rSnapshot.m_nPatchCount * sizeof( t3dWorldTerrainClusterBounds_t );
		if ( patch_bytes > 0u ) {
			if ( !m_hPatchBoundsBuffer.writeTracked( m_context, 0u, patch_bounds, patch_bytes,
			        m_patchBoundsBytes, T3D_GPU_PUBLICATION ) ) {
				++m_nFailureCount;
				return false;
			}
		}
		m_nPatchBoundsGeneration = rSnapshot.m_nGeneration;
	}
	const uint32_t capacity =
	        rResources.getCapacityBytes( Terrain3DWorldResources::WORLD_BUFFER_VISIBLE_ITEMS ) / sizeof( t3dWorldVisibleItem_t );
	const uint32_t candidate_count = MIN( nCandidateCount, capacity );
	t3dVisibilityPush_t push{};
	push.m_nCandidateCount = candidate_count;
	push.m_nVisibilityMask = nVisibilityMask;
	push.m_nFlags = bHiZValid ? 1u : 0u;
	push.m_nPassKind = nPassKind;
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
	rd->compute_list_bind_uniform_set( list, m_hSet[PIPELINE_RESET], 0u );
	rd->compute_list_set_push_constant( list, m_pushBytes, sizeof( push ) );
	rd->compute_list_dispatch( list, 1u, 1u, 1u );
	++m_nDispatchCount;
	rd->compute_list_add_barrier( list );
	if ( candidate_count > 0u ) {
		rd->compute_list_bind_compute_pipeline( list, m_hPipeline[PIPELINE_CULL] );
		rd->compute_list_bind_uniform_set( list, m_hSet[PIPELINE_CULL], 0u );
		rd->compute_list_set_push_constant( list, m_pushBytes, sizeof( push ) );
		rd->compute_list_dispatch( list, ( candidate_count + 63u ) / 64u, 1u, 1u );
		++m_nDispatchCount;
		rd->compute_list_add_barrier( list );
	}
	rd->compute_list_bind_compute_pipeline( list, m_hPipeline[PIPELINE_FINALIZE] );
	rd->compute_list_bind_uniform_set( list, m_hSet[PIPELINE_FINALIZE], 0u );
	rd->compute_list_set_push_constant( list, m_pushBytes, sizeof( push ) );
	rd->compute_list_dispatch( list, 1u, 1u, 1u );
	++m_nDispatchCount;
	rd->compute_list_add_barrier( list );
	rd->compute_list_end();
	m_nLastCandidateCount = candidate_count;
	++m_nPreparedFrameCount;
	return true;
}

void Terrain3DWorldVisibilityGpu::shutdownCold( void ) {
	freeSetsCold();
	m_pipelineCache.clear();
	for ( uint32_t i = 0u; i < PIPELINE_COUNT; ++i ) {
		m_hPipeline[i] = RID();
		m_hShader[i].releaseTracked( m_context );
		m_source[i].unref();
	}
	m_hPatchBoundsBuffer.releaseTracked( m_context );
	m_patchBoundsBytes.clear();
	m_pushBytes.clear();
	m_nPatchBoundsGeneration = 0u;
	m_bInitialized = false;
}

bool Terrain3DWorldVisibilityGpu::isReady( void ) const {
	return m_bInitialized && m_hPatchBoundsBuffer.is_valid() && m_hPipeline[PIPELINE_RESET].is_valid() && m_hPipeline[PIPELINE_CULL].is_valid()
	        && m_hPipeline[PIPELINE_FINALIZE].is_valid() && m_hSet[PIPELINE_RESET].is_valid() && m_hSet[PIPELINE_CULL].is_valid()
	        && m_hSet[PIPELINE_FINALIZE].is_valid();
}
uint64_t Terrain3DWorldVisibilityGpu::getDispatchCount( void ) const {
	return m_nDispatchCount;
}
uint64_t Terrain3DWorldVisibilityGpu::getPreparedFrameCount( void ) const {
	return m_nPreparedFrameCount;
}
uint64_t Terrain3DWorldVisibilityGpu::getSetRebuildCount( void ) const {
	return m_nSetRebuildCount;
}
uint64_t Terrain3DWorldVisibilityGpu::getFailureCount( void ) const {
	return m_nFailureCount;
}
uint32_t Terrain3DWorldVisibilityGpu::getLastCandidateCount( void ) const {
	return m_nLastCandidateCount;
}
