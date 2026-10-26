// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/visibility/terrain_3d_world_raster.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "rendering/rd/shaders/terrain_3d_rd_material_vt_contract.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_variant.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_binding_builder.h"
#include "Terrain3DConstants.h"
#include "rendering/rd/vt/gpu/terrain_3d_vt_gpu.h"

#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <string.h>

using namespace godot;

static void t3dWorldMarkDynamicBuffers(
	const Ref< RDShaderSPIRV > &shader,
	bool,
	bool,
	bool ) {
	Terrain3DRdContext::getShared().markHotShader( shader );
}

Terrain3DWorldRaster::Terrain3DWorldRaster(
	Terrain3DRdContext &rContext ) :
	m_context( rContext ),
	m_forwardLighting( rContext ),
	m_pipelines( rContext, T3D_RD_CONTEXT_OWNER_WORLD ),
    m_environmentSetCache( rContext, T3D_RD_CONTEXT_OWNER_WORLD ),
	m_set0Cache( rContext, T3D_RD_CONTEXT_OWNER_WORLD ),
	m_set1Cache( rContext, T3D_RD_CONTEXT_OWNER_WORLD ) {
	memset( m_nSubmitCount, 0, sizeof( m_nSubmitCount ) );
}
Terrain3DWorldRaster::~Terrain3DWorldRaster(
	void ) {
}

bool Terrain3DWorldRaster::uploadSuperTerrainCandidateCold(
    const Terrain3DStPublicationImage &rCandidate, uint32_t nFinePageSlots )
{
    if ( !m_bInitialized || m_stGpuImage.valid() || !rCandidate.valid() || !m_context.getDevice() )
    {
        return false;
    }
    Terrain3DStGpuImage candidate;
    if ( !candidate.uploadCandidateCold( m_context, rCandidate, nFinePageSlots,
                                        m_context.getDeviceGeneration() ) )
    {
        return false;
    }
    // Invalidate cached sets while they still refer to the preceding RID set.
    // The caller is responsible for the render-thread cold publication boundary.
    m_bCurrentValid = false;
    freeSetsCold();
    m_stGpuImage.swap( candidate );
    candidate.releaseCold( m_context );
    return true;
}

void Terrain3DWorldRaster::discardSuperTerrainCandidateCold( void )
{
    // Caller first clears all borrowed clipmap RIDs. Set caches must release
    // descriptors before their backing GPU resources are destroyed.
    m_bCurrentValid = false;
    freeSetsCold();
    m_stGpuImage.releaseCold( m_context );
}

bool Terrain3DWorldRaster::configureComposedExperimentalCold( const bool bEnable ) {
	if ( m_bInitialized || bEnable ) return false;
	m_bExperimentalComposed = bEnable;
	return true;
}

bool Terrain3DWorldRaster::initializeCold(
	const bool bPersistentFrameBuffer,
	const bool bPersistentCpuBuffers ) {
	if ( m_bInitialized ) {
		return true;
	}
	if ( !m_context.getDevice() ) {
		return false;
	}
	m_bPersistentFrameBuffer = m_context.arena( Terrain3DRdContext::HOT ).isPersistent();
	m_hotPushBytes.resize( sizeof( t3dRdDrawPushConstants_t ) );
	m_bPersistentCpuBuffers = bPersistentCpuBuffers;
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		const t3dRdShaderId_t	   shadow_id = primitive == T3D_PRIMITIVE_TERRAIN_STRIP
												   ? T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW
												   : T3D_RD_SHADER_WORLD_RASTER_MESH_SHADOW;
		const Ref< RDShaderSPIRV > shadow_spirv = Terrain3DRdShaderLibrary::borrowSpirv( shadow_id );
		if ( shadow_spirv.is_null() ) {
			shutdownCold();
			return false;
		}
		t3dWorldMarkDynamicBuffers( shadow_spirv,
									bPersistentFrameBuffer,
									bPersistentCpuBuffers,
									primitive == T3D_PRIMITIVE_TERRAIN_STRIP );
		const String class_name =
			primitive == T3D_PRIMITIVE_TERRAIN_STRIP ? String( "TerrainStrip" ) : String( "Mesh" );
		const String shadow_name = String( "T3D.World.Raster." ) + class_name + String( ".ShadowShader" );
		if ( !m_hShadowShader[primitive].createTracked( m_context,
														T3D_RD_CONTEXT_OWNER_WORLD,
														shadow_name,
														shadow_id ) ) {
			shutdownCold();
			return false;
		}
	}
    if ( Terrain3DRdShaderLibrary::isEnvironmentFieldsEnabledCold() ) {
        m_context.beginRecoverableFailureScope();
        const bool created = m_hEnvironmentMeshShadowShader.createTracked( m_context,
            T3D_RD_CONTEXT_OWNER_WORLD, "T3D.World.Environment.Mesh.Shadow",
            T3D_RD_SHADER_WORLD_MESH_ENV_SHADOW );
        m_context.endRecoverableFailureScope();
        if ( !created ) {
            // Keep the existing WORLD shadow path when an optional variant is unavailable.
            m_hEnvironmentMeshShadowShader.releaseTracked( m_context );
        }
    }
	m_bInitialized = true;
	return true;
}

void Terrain3DWorldRaster::invalidateMaterialShadersCold( void ) {
	m_bCurrentValid = false;
	m_pipelines.clear();
	freeSetsCold();
	m_forwardLighting.clearShaderSetsCold();
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
			m_hShader[primitive][variant].releaseTracked( m_context );
		}
	}
	memset( m_bMeshFeedback, 0, sizeof( m_bMeshFeedback ) );
	memset( m_bCompactShader, 0, sizeof( m_bCompactShader ) );
	m_hCurrentFeedbackSet = RID();
}

bool Terrain3DWorldRaster::ensureShaderCold(
	const t3dPrimitiveClass_t		  primitive,
	const t3dRdForwardShaderVariant_t variant ) {
	if ( primitive >= T3D_PRIMITIVE_CLASS_COUNT || variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX ) {
		return false;
	}
	Terrain3DRdShader &shader = m_hShader[primitive][variant];
    if ( shader.isValid() ) {
        return true;
    }
	bool feedback = primitive == T3D_PRIMITIVE_MESH && variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT &&
					m_shaderCapabilities.m_bFeedbackReady;
	const bool composed_variant = m_bExperimentalComposed &&
		primitive == T3D_PRIMITIVE_TERRAIN_STRIP && ( uint32_t ) variant != 8u;
	const t3dRdShaderId_t fallback_id =
		( t3dRdShaderId_t ) ( ( primitive == T3D_PRIMITIVE_TERRAIN_STRIP ? T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE
																		 : T3D_RD_SHADER_WORLD_RASTER_MESH_BASE ) +
							  variant );
	t3dRdShaderId_t id = composed_variant
		? ( t3dRdShaderId_t ) ( T3D_RD_SHADER_WORLD_CS_BASE + ( uint32_t ) variant - ( ( uint32_t ) variant > 8u ? 1u : 0u ) )
		: feedback ? ( t3dRdShaderId_t ) ( T3D_RD_SHADER_WORLD_MESH_FEEDBACK_BASE + variant ) : fallback_id;
	const bool compact = m_forwardLighting.compactRequested() &&
		variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT && !feedback && !composed_variant;
	if ( compact )
		id = ( t3dRdShaderId_t ) ( ( primitive == T3D_PRIMITIVE_TERRAIN_STRIP
			? T3D_RD_SHADER_WORLD_STRIP_COMPACT_BASE : T3D_RD_SHADER_WORLD_MESH_COMPACT_BASE ) + variant );
    const t3dRdShaderId_t original_id = id;
    if ( primitive == T3D_PRIMITIVE_TERRAIN_STRIP && m_bEnvironmentReady && !composed_variant &&
         ( uint32_t ) variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT ) {
        id = ( t3dRdShaderId_t ) ( ( compact ? T3D_RD_SHADER_WORLD_STRIP_ENV_COMPACT_BASE :
                                    T3D_RD_SHADER_WORLD_STRIP_ENV_BASE ) + variant );
        m_bEnvironmentShader[T3D_PRIMITIVE_TERRAIN_STRIP][variant] = true;
    }
    if ( primitive == T3D_PRIMITIVE_MESH && m_bEnvironmentReady ) {
        if ( feedback ) {
            id = ( t3dRdShaderId_t ) ( T3D_RD_SHADER_WORLD_MESH_ENV_FEEDBACK_BASE + variant );
        } else if ( compact ) {
            id = ( t3dRdShaderId_t ) ( T3D_RD_SHADER_WORLD_MESH_ENV_COMPACT_BASE + variant );
        } else {
            id = ( t3dRdShaderId_t ) ( T3D_RD_SHADER_WORLD_MESH_ENV_BASE + variant );
        }
        m_bEnvironmentShader[primitive][variant] = true;
    }
    Ref< RDShaderSPIRV > spirv = Terrain3DRdShaderLibrary::borrowSpirv( id );
    if ( m_bEnvironmentShader[primitive][variant] && spirv.is_null() ) {
        // A missing optional variant cannot remove the baseline WORLD shader.
        m_bEnvironmentShader[primitive][variant] = false;
        id = original_id;
        spirv = Terrain3DRdShaderLibrary::borrowSpirv( id );
    }
    if ( feedback && spirv.is_null() ) {
		feedback = false;
        m_bEnvironmentShader[primitive][variant] = false;
		id = fallback_id;
		spirv = Terrain3DRdShaderLibrary::borrowSpirv( id );
	}
	if ( spirv.is_null() ) {
		return false;
	}
	t3dWorldMarkDynamicBuffers( spirv,
								m_bPersistentFrameBuffer,
								m_bPersistentCpuBuffers,
								primitive == T3D_PRIMITIVE_TERRAIN_STRIP );
	const String class_name = primitive == T3D_PRIMITIVE_TERRAIN_STRIP ? String( "TerrainStrip" ) : String( "Mesh" );
	const String name =
		String( "T3D.World.Raster." ) + class_name + String( ".Shader." ) + String::num_uint64( variant );
    const bool environmentVariant = m_bEnvironmentShader[primitive][variant];
    if ( feedback || environmentVariant ) {
        m_context.beginRecoverableFailureScope();
    }
    const bool created = shader.createTracked( m_context, T3D_RD_CONTEXT_OWNER_WORLD, name, id );
    if ( feedback || environmentVariant ) {
        m_context.endRecoverableFailureScope();
    }
    if ( feedback ) {
        m_bMeshFeedback[variant] = created;
    }
    if ( !created && ( feedback || environmentVariant ) ) {
        // Fall back to the selected original shader for non-feedback variants.
        // Feedback failures take the established no-feedback baseline path.
        m_bEnvironmentShader[primitive][variant] = false;
        m_bMeshFeedback[variant] = false;
        const t3dRdShaderId_t fallback = feedback ? fallback_id : original_id;
        const Ref< RDShaderSPIRV > baselineSpirv = Terrain3DRdShaderLibrary::borrowSpirv( fallback );
        if ( baselineSpirv.is_null() ) {
            return false;
        }
        t3dWorldMarkDynamicBuffers( baselineSpirv, m_bPersistentFrameBuffer, m_bPersistentCpuBuffers,
                                    primitive == T3D_PRIMITIVE_TERRAIN_STRIP );
        const bool fallbackCreated = shader.createTracked( m_context, T3D_RD_CONTEXT_OWNER_WORLD, name, fallback );
        if ( fallbackCreated ) {
            m_bCompactShader[primitive][variant] = !feedback && compact;
        }
        return fallbackCreated;
    }
    if ( created ) {
        m_bCompactShader[primitive][variant] = compact;
    }
    return created;
}

void Terrain3DWorldRaster::freeSetsCold(
	void ) {
	m_set0Cache.clear();
	m_set1Cache.clear();
    m_environmentSetCache.clear();
    for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
        m_hCurrentEnvironmentSet[primitive] = RID();
        m_bCurrentEnvironmentShader[primitive] = false;
        for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
            m_bEnvironmentShader[primitive][variant] = false;
        }
        for ( uint32_t variant = 0u; variant <= ENV_SHADOW_SLOT; ++variant ) {
            m_hEnvironmentSet[primitive][variant][0] = RID();
            m_hEnvironmentSet[primitive][variant][1] = RID();
        }
    }
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		for ( uint32_t slot = 0u; slot < T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT; ++slot ) {
			m_hSet0[primitive][slot] = RID();
			m_hSet1[primitive][slot] = RID();
			m_set0Signature[primitive][slot] = t3dSetSignature_t{};
			m_set1Signature[primitive][slot] = t3dSetSignature_t{};
		}
		m_hCurrentSet0[primitive] = RID();
		m_hCurrentSet1[primitive] = RID();
		m_hLightingSet[primitive] = RID();
		m_hCompactSet[primitive] = RID();
	}
}

bool Terrain3DWorldRaster::ensureSetsCold(
	Terrain3DWorldResources					&rResources,
	const t3dRdWorldTerrainRasterBindings_t &rTerrain,
	const t3dSharedVTBindings_t				&rVT,
	const RID								&rShader,
	t3dPrimitiveClass_t						 primitiveClass,
	uint32_t								 nDescriptorSlot,
	const t3dComposedSurfaceGpuBindings_t *pComposed ) {
	const uint32_t primitive = ( uint32_t ) primitiveClass;
	const uint64_t world_generation = rResources.getResourceGeneration();
	if ( primitive >= T3D_PRIMITIVE_CLASS_COUNT || nDescriptorSlot >= T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT ) {
		return false;
	}
	RID					   &hSet0 = m_hSet0[primitive][nDescriptorSlot];
	RID					   &hSet1 = m_hSet1[primitive][nDescriptorSlot];
	const t3dSetSignature_t set0_key{ rShader, world_generation, rTerrain.m_nGeneration };
	// Layout identity includes composed pool replacement even when source V1 is unchanged.
	const uint64_t composed_dependency = pComposed ?
		( uint64_t ) pComposed->m_hPageTable.get_id() ^
		( ( uint64_t ) pComposed->m_hChannels[0].get_id() << 1u ) ^
		( ( uint64_t ) pComposed->m_hChannels[1].get_id() << 2u ) ^
		( ( uint64_t ) pComposed->m_hChannels[2].get_id() << 3u ) ^
		( ( uint64_t ) pComposed->m_nDeviceGeneration << 32u ) : 0u;
    const t3dStGpuBindings_t st = m_stGpuImage.bindings();
    const uint64_t st_dependency = st.valid() ?
        (st.m_nSignature ^ ((uint64_t) st.m_nDeviceGeneration << 32u) ^
         (uint64_t) st.m_hVhmPages.get_id() ^ ((uint64_t) st.m_hMaskWords.get_id() << 1u)) : 0u;
	const t3dSetSignature_t set1_key{ rShader, world_generation,
		rVT.nBindingGeneration ^ composed_dependency ^ st_dependency };
	if ( m_set0Signature[primitive][nDescriptorSlot] == set0_key &&
		 m_set1Signature[primitive][nDescriptorSlot] == set1_key ) {
		const RID liveSet0 = m_set0Cache.resolve( hSet0 );
		const RID liveSet1 = m_set1Cache.resolve( hSet1 );
		if ( liveSet0.is_valid() && liveSet1.is_valid() ) {
			m_hCurrentSet0[primitive] = liveSet0;
			m_hCurrentSet1[primitive] = liveSet1;
			return true;
		}
	}
	m_set0Cache.erase( hSet0 );
	m_set1Cache.erase( hSet1 );
	hSet0 = RID();
	hSet1 = RID();
	m_hLightingSet[primitive] = RID();
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !rShader.is_valid() ) {
		return false;
	}
	const RenderingDevice::UniformType frame_type = rTerrain.m_bPersistentFrameBuffer
														? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC
														: RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
	const RenderingDevice::UniformType cpu_type = rTerrain.m_bPersistentCpuBuffers
													  ? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC
													  : RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
	TypedArray< Ref< RDUniform > >	   set0;
	set0.push_back( Terrain3DRdBindingBuilder::single( 0, frame_type, rTerrain.m_hFrameBuffer ) );
	if ( primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ) {
		// Unified terrain-strip schema explicitly owns the standalone terrain resource surface.
		set0.push_back( Terrain3DRdBindingBuilder::single( 1, cpu_type, rTerrain.m_hRegionMapBuffer ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 2, rTerrain.m_hTerrainSampler, rTerrain.m_hHeightTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 3, rTerrain.m_hTerrainSampler, rTerrain.m_hControlTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 4, rTerrain.m_hMapSampler, rTerrain.m_hColorTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 5, rTerrain.m_hMaterialSampler, rTerrain.m_hAlbedoTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 6, rTerrain.m_hMaterialSampler, rTerrain.m_hNormalTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 7, rTerrain.m_hMaterialSampler, rTerrain.m_hSurfaceTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 8, cpu_type, rTerrain.m_hMaterialBuffer ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 10,
														   RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
														   rTerrain.m_hVisibleBuffer ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 11,
																   rTerrain.m_hMaterialSampler,
																   rTerrain.m_hMaterialDisplacementTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 12,
																   rTerrain.m_hTerrainSampler,
																   rTerrain.m_hDisplacementTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 13, cpu_type, rTerrain.m_hMaterialConfigBuffer ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 14, rTerrain.m_hMapSampler, rTerrain.m_hDerivedTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 15,
																   rTerrain.m_hMapSampler,
																   rTerrain.m_hMiddleCompositeTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 16,
																   rTerrain.m_hTerrainSampler,
																   rTerrain.m_hMiddleHeightTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 17,
																   rTerrain.m_hTerrainSampler,
																   rTerrain.m_hMiddleHeightRegionMapTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 20,
																   rTerrain.m_hMaterialSampler,
																   rTerrain.m_hFarAlbedoTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 21,
																   rTerrain.m_hMaterialSampler,
																   rTerrain.m_hWorldNoiseTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 22,
																   rTerrain.m_hMaterialSampler,
																   rTerrain.m_hMacroNoiseTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 24, rTerrain.m_hMapSampler, rTerrain.m_hEditorBrushTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 25,
																   rTerrain.m_hMapSampler,
																   rTerrain.m_hTerrainShadowMaskTexture ) );
	}
	static const struct
	{
		int				   binding;
		t3dWorldBufferId_t section;
		bool			   terrain;
	} worldBindings[] = { { 39, WORLD_BUFFER_VERTICES, false },
						  { 40, WORLD_BUFFER_CLUSTER_VERTEX_INDICES, false },
						  { 41, WORLD_BUFFER_TRIANGLES, false },
						  { 42, WORLD_BUFFER_CLUSTERS, false },
						  { 46, WORLD_BUFFER_TRANSFORMS_CURRENT, false },
						  { 47, WORLD_BUFFER_TRANSFORMS_PREVIOUS, false },
						  { 48, WORLD_BUFFER_INSTANCES, false },
						  { 50, WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS, true } };
	set0.push_back( Terrain3DRdBindingBuilder::storage( 49, rResources.getBuffer( WORLD_BUFFER_VISIBLE_ITEMS ) ) );
	for ( const auto &binding : worldBindings ) {
		if ( binding.terrain == ( primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ) ) {
			set0.push_back(
				Terrain3DRdBindingBuilder::storage( binding.binding, rResources.getBuffer( binding.section ) ) );
		}
	}
	const String class_name =
		primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ? String( "TerrainStrip" ) : String( "Mesh" );
	hSet0 = m_set0Cache.getOrCreate( set0_key,
									 String( "T3D.World.Raster." ) + class_name + String( ".Set0." ) +
										 String::num_uint64( nDescriptorSlot ),
									 set0,
									 rShader,
									 0u );
	if ( !hSet0.is_valid() ) {
		return false;
	}

	if ( rVT.nBankCount[0] != 1u || rVT.nBankCount[1] != 1u || rVT.nBankCount[2] != 1u ||
         rVT.nBankCount[3] != 1u || rVT.nBankCount[T3D_VT_CHANNEL_HEIGHT16] != 1u ||
         !rVT.hPhysical[T3D_VT_CHANNEL_HEIGHT16][0].is_valid() || !rVT.hTail[T3D_VT_CHANNEL_HEIGHT16].is_valid() ) {
		return false;
	}
	TypedArray< Ref< RDUniform > > set1;
	if ( primitiveClass == T3D_PRIMITIVE_MESH ) {
		set1.push_back( Terrain3DRdBindingBuilder::single( 0,
														   RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
														   rResources.getBuffer( WORLD_BUFFER_MATERIALS ) ) );
	}
	set1.push_back(
		Terrain3DRdBindingBuilder::single( T3D_RD_WORLD_VT_ALLOCATION_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rVT.hAllocationTable ) );
	set1.push_back(
		Terrain3DRdBindingBuilder::single( T3D_RD_WORLD_VT_PAGE_TABLE_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rVT.hPageTable ) );
	for ( uint32_t channel = 0u; channel < 4u; ++channel ) {
		set1.push_back( Terrain3DRdBindingBuilder::samplerTexture( T3D_RD_WORLD_VT_PHYSICAL_BINDING_BASE + ( int ) channel,
																   rTerrain.m_hMaterialSampler,
																   rVT.hPhysical[channel][0] ) );
	}
	set1.push_back( Terrain3DRdBindingBuilder::samplerTexture(
		T3D_RD_WORLD_VT_HEIGHT16_BINDING, rTerrain.m_hMaterialSampler,
		rVT.hPhysical[T3D_VT_CHANNEL_HEIGHT16][0] ) );
	set1.push_back(
		Terrain3DRdBindingBuilder::single( T3D_RD_WORLD_VT_TAIL_METADATA_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rVT.hTailMetadata ) );
	for ( uint32_t channel = 0u; channel < 4u; ++channel ) {
		set1.push_back( Terrain3DRdBindingBuilder::samplerTexture( T3D_RD_WORLD_VT_TAIL_TEXTURE_BINDING_BASE + ( int ) channel,
																   rTerrain.m_hMapSampler,
																   rVT.hTail[channel] ) );
	}
	set1.push_back( Terrain3DRdBindingBuilder::samplerTexture(
		T3D_RD_WORLD_VT_HEIGHT16_TAIL_BINDING, rTerrain.m_hMapSampler,
		rVT.hTail[T3D_VT_CHANNEL_HEIGHT16] ) );
	set1.push_back(
		Terrain3DRdBindingBuilder::single( T3D_RD_WORLD_VT_MIP_LAYOUT_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rVT.hMipLayoutTable ) );
    if ( primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP )
    {
        // Legacy-only frames borrow well-typed live resources; no additional
        // allocation is made until an ST05 candidate has been committed.
        const RID hVhmPages = st.valid() ? st.m_hVhmPages : rVT.hPageTable;
        const RID hVhmTails = st.valid() ? st.m_hVhmTails : rVT.hTailMetadata;
        const RID hVhmFine = st.valid() ? st.m_hVhmFine : rVT.hPhysical[T3D_VT_CHANNEL_HEIGHT16][0];
        const RID hVhmRoot = st.valid() ? st.m_hVhmRoot : rVT.hTail[T3D_VT_CHANNEL_HEIGHT16];
        const RID hMaskEntries = st.valid() ? st.m_hMaskEntries : rVT.hPageTable;
        const RID hMaskWords = st.valid() ? st.m_hMaskWords : rVT.hAllocationTable;
        set1.push_back( Terrain3DRdBindingBuilder::single( T3D_RD_WORLD_ST_VHM_PAGES_BINDING,
            RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, hVhmPages ) );
        set1.push_back( Terrain3DRdBindingBuilder::single( T3D_RD_WORLD_ST_VHM_TAILS_BINDING,
            RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, hVhmTails ) );
        set1.push_back( Terrain3DRdBindingBuilder::samplerTexture( T3D_RD_WORLD_ST_VHM_FINE_BINDING,
            rTerrain.m_hTerrainSampler, hVhmFine ) );
        set1.push_back( Terrain3DRdBindingBuilder::samplerTexture( T3D_RD_WORLD_ST_VHM_ROOT_BINDING,
            rTerrain.m_hMapSampler, hVhmRoot ) );
        set1.push_back( Terrain3DRdBindingBuilder::single( T3D_RD_WORLD_ST_MASK_ENTRIES_BINDING,
            RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, hMaskEntries ) );
        set1.push_back( Terrain3DRdBindingBuilder::single( T3D_RD_WORLD_ST_MASK_WORDS_BINDING,
            RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, hMaskWords ) );
    }
// BR20: this consumer keeps its original descriptor ABI until a hint-specific shader version is selected.
	if ( pComposed && primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ) {
		if ( !pComposed->m_hPageTable.is_valid() || !pComposed->m_nTableCapacity ||
			!pComposed->m_nDeviceGeneration ) return false;
		for ( uint32_t i = 0u; i < T3D_CS_CHANNEL_COUNT; ++i )
			if ( !pComposed->m_hChannels[i].is_valid() ) return false;
		set1.push_back( Terrain3DRdBindingBuilder::single( T3D_RD_WORLD_VT_COMPOSED_PAGE_TABLE_BINDING,
			RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, pComposed->m_hPageTable ) );
		for ( uint32_t i = 0u; i < T3D_CS_CHANNEL_COUNT; ++i )
			set1.push_back( Terrain3DRdBindingBuilder::samplerTexture( T3D_RD_WORLD_VT_COMPOSED_TEXTURE_BINDING_BASE + ( int ) i,
				rTerrain.m_hMaterialSampler, pComposed->m_hChannels[i] ) );
	}
	hSet1 = m_set1Cache.getOrCreate( set1_key,
									 String( "T3D.World.Raster." ) + class_name + String( ".Set1." ) +
										 String::num_uint64( nDescriptorSlot ),
									 set1,
									 rShader,
									 T3D_RD_WORLD_VT_DESCRIPTOR_SET );
	if ( !hSet1.is_valid() ) {
		return false;
	}
	m_set0Signature[primitive][nDescriptorSlot] = set0_key;
	m_set1Signature[primitive][nDescriptorSlot] = set1_key;
	m_hCurrentSet0[primitive] = hSet0;
	m_hCurrentSet1[primitive] = hSet1;
	return true;
}

const Terrain3DWorldRaster::t3dPipelineEntry_t *Terrain3DWorldRaster::findPipeline(
	const t3dPipelineKey_t &rKey ) const {
	return m_pipelines.find( rKey );
}

bool Terrain3DWorldRaster::ensureEnvironmentSetsCold(
    const RID &rShader, const uint32_t nPrimitive, const uint32_t nVariant,
    const t3dRdEnvironmentBindings_t &rBindings ) {
    if ( nPrimitive >= T3D_PRIMITIVE_CLASS_COUNT || nVariant > ENV_SHADOW_SLOT ||
         !rBindings.m_bAvailable || !rBindings.m_hParams.is_valid() || !rBindings.m_hSampler.is_valid() ) {
        return false;
    }
    for ( uint32_t ping = 0u; ping < 2u; ++ping ) {
        if ( !rBindings.m_hStatePair[ping].is_valid() || !rBindings.m_hInteractionPair[ping].is_valid() ) {
            return false;
        }
        RID &cached = m_hEnvironmentSet[nPrimitive][nVariant][ping];
        if ( m_environmentSetCache.resolve( cached ).is_valid() ) {
            continue;
        }
        const uint64_t fieldId = ( uint64_t ) rBindings.m_hStatePair[ping].get_id() ^
            ( nPrimitive == T3D_PRIMITIVE_MESH
                  ? ( ( uint64_t ) rBindings.m_hInteractionPair[ping].get_id() << 1u ) : 0u );
        const uint64_t previousId = nPrimitive == T3D_PRIMITIVE_TERRAIN_STRIP ? 0u :
            ( uint64_t ) rBindings.m_hInteractionPair[ping ^ 1u].get_id() ^
            ( ( uint64_t ) rBindings.m_hHeightPair[ping].get_id() << 3u ) ^
            ( ( uint64_t ) rBindings.m_hHeightPair[ping ^ 1u].get_id() << 7u );
        const t3dSetSignature_t key{ rShader, fieldId ^ ( previousId << 1u ),
            ( uint64_t ) rBindings.m_hParams.get_id() };
        TypedArray< Ref< RDUniform > > uniforms;
        // Terrain and world meshes both consume wetness; world materials additionally
        // sample the same state to preserve rock/shoreline surface continuity.
        if ( nPrimitive == T3D_PRIMITIVE_TERRAIN_STRIP ||
             ( nVariant != 8u && nVariant != ENV_SHADOW_SLOT ) ) {
            uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 0,
                rBindings.m_hSampler, rBindings.m_hStatePair[ping] ) );
        }
        uniforms.push_back( Terrain3DRdBindingBuilder::single( 1,
            RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, rBindings.m_hParams ) );
        if ( nPrimitive == T3D_PRIMITIVE_MESH ) {
            uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 2,
                rBindings.m_hSampler, rBindings.m_hInteractionPair[ping] ) );
            uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 4,
                rBindings.m_hHeightSampler, rBindings.m_hHeightPair[ping] ) );
            if ( nVariant != ENV_SHADOW_SLOT && ( nVariant == 2u || nVariant == 3u ||
                nVariant == 6u || nVariant == 7u ) ) {
                uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 3,
                    rBindings.m_hSampler, rBindings.m_hInteractionPair[ping ^ 1u] ) );
                uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 5,
                    rBindings.m_hHeightSampler, rBindings.m_hHeightPair[ping ^ 1u] ) );
            }
        }
        cached = m_environmentSetCache.getOrCreate( key,
            "T3D.World.Environment." + String::num_uint64( nPrimitive ) + "." +
            String::num_uint64( nVariant ) + "." + String::num_uint64( ping ),
            uniforms, rShader, 5u );
        if ( !cached.is_valid() ) {
            return false;
        }
    }
    m_hCurrentEnvironmentSet[nPrimitive] = m_hEnvironmentSet[nPrimitive][nVariant][rBindings.m_nPing & 1u];
    return m_hCurrentEnvironmentSet[nPrimitive].is_valid();
}

bool Terrain3DWorldRaster::ensurePipelineCold(
	const t3dRdForwardPassContext_t &rContext,
	t3dRdForwardShaderVariant_t		 variant,
	t3dPrimitiveClass_t				 primitiveClass ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || rContext.m_nFramebufferFormat < 0 ) {
		return false;
	}
	t3dPipelineKey_t key;
	if ( !t3dRdCompileForwardPipelineBaseKey( rd, rContext, variant, false, key ) ) {
		return false;
	}
	key.m_nPrimitiveClass = ( uint32_t ) primitiveClass;
    key.m_bEnvironmentVariant = m_bCurrentEnvironmentShader[( uint32_t ) primitiveClass];
	if ( findPipeline( key ) ) {
		m_currentKey[( uint32_t ) primitiveClass] = key;
		return true;
	}
	t3dRdForwardPipelineStateOptions_t state_options;
	state_options.m_frontFace = primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP
									? RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE
									: RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE;
	state_options.m_cullMode =
		key.m_bReverseCull ? RenderingDevice::POLYGON_CULL_FRONT : RenderingDevice::POLYGON_CULL_BACK;
	state_options.m_nBlendAttachmentCount = t3dRdForwardBlendAttachmentCount( variant, false );
	state_options.m_bEnableDepthWrite = !key.m_bDepthPrepassActive;
	state_options.m_depthCompare =
		key.m_bDepthPrepassActive ? RenderingDevice::COMPARE_OP_EQUAL : RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
	t3dRdForwardPipelineStates_t pipeline_states;
	if ( !t3dRdBuildForwardPipelineStates( key, state_options, pipeline_states ) ) {
		return false;
	}
	Ref< RDPipelineRasterizationState > raster = pipeline_states.m_raster;
	Ref< RDPipelineMultisampleState >	multisample = pipeline_states.m_multisample;
	Ref< RDPipelineDepthStencilState >	depth = pipeline_states.m_depth;
	Ref< RDPipelineColorBlendState >	blend = pipeline_states.m_blend;
	const uint32_t						primitive = ( uint32_t ) primitiveClass;
	const RID shader = key.m_passKind == T3D_RD_FORWARD_PASS_SHADOW
        ? ( primitiveClass == T3D_PRIMITIVE_MESH && key.m_bEnvironmentVariant
            ? m_hEnvironmentMeshShadowShader.rid() : m_hShadowShader[primitive].rid() )
        : m_hShader[primitive][( uint32_t ) variant].rid();
	if ( !shader.is_valid() ) {
		return false;
	}
	const RenderingDevice::RenderPrimitive render_primitive = primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP
																  ? RenderingDevice::RENDER_PRIMITIVE_TRIANGLE_STRIPS
																  : RenderingDevice::RENDER_PRIMITIVE_TRIANGLES;
	const String						   class_name =
		  primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ? String( "TerrainStrip" ) : String( "Mesh" );
	const RID pipeline =
		m_pipelines.getOrCreateRaster( key,
									   String( "T3D.World.Raster." ) + class_name + String( ".Pipeline" ),
									   shader,
									   rContext.m_nFramebufferFormat,
									   -1,
									   render_primitive,
									   raster,
									   multisample,
									   depth,
									   blend,
									   0u );
	if ( !pipeline.is_valid() ) {
		return false;
	}
	m_currentKey[primitive] = key;
	return true;
}

bool Terrain3DWorldRaster::prepareCold(
	const t3dRdForwardPassContext_t			&rContext,
	Terrain3DWorldResources					&rResources,
	const t3dRdWorldTerrainRasterBindings_t &rTerrain,
	const t3dSharedVTBindings_t				&rVT,
	Terrain3DVTGpu							*pFeedbackOwner,
	const t3dComposedSurfaceGpuBindings_t *pComposed,
    const t3dRdEnvironmentBindings_t *pEnvironment ) {
	if ( !m_bInitialized || rContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE || rContext.m_nViewCount == 0u ) {
		return false;
	}
	m_bCurrentValid = false;
	if ( m_bExperimentalComposed && !pComposed ) return false;
	const t3dWorldShaderCapabilityKey_t capabilities{
		pFeedbackOwner,
		pFeedbackOwner ? pFeedbackOwner->get_fragment_feedback_shader_generation() : 0u,
		pFeedbackOwner && pFeedbackOwner->is_fragment_feedback_shader_ready(),
		pEnvironment && pEnvironment->m_bAvailable,
		m_forwardLighting.compactRequested()
	};
	if ( m_bShaderCapabilitiesValid && !( m_shaderCapabilities == capabilities ) ) {
		// The first no-feedback material shader must not survive VT readiness or
		// coarse-grid ABI changes. Retire dependent pipelines/sets before its RID.
		invalidateMaterialShadersCold();
	}
	m_shaderCapabilities = capabilities;
	m_bShaderCapabilitiesValid = true;
	m_pFeedbackOwner = pFeedbackOwner;
	m_bEnvironmentReady = capabilities.m_bEnvironmentReady;
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		m_hCurrentEnvironmentSet[primitive] = RID();
		m_bCurrentEnvironmentShader[primitive] = false;
	}
	m_hCurrentFeedbackSet = RID();
	++m_nPrepareCount;
	t3dRdForwardShaderVariant_t variant;
	if ( !t3dRdSelectForwardVariant( rContext, T3D_RD_FORWARD_VARIANT_SHADOW_ANY_NATIVE_MODE, variant ) ) {
		return false;
	}
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW &&
		 ( !m_hShadowShader[T3D_PRIMITIVE_TERRAIN_STRIP].isValid() ||
		   !m_hShadowShader[T3D_PRIMITIVE_MESH].isValid() ) ) {
		return false;
	}
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		m_hLightingSet[primitive] = RID();
		m_hCompactSet[primitive] = RID();
	}
	const t3dPrimitiveClass_t active_classes[2] = { T3D_PRIMITIVE_TERRAIN_STRIP, T3D_PRIMITIVE_MESH };
	const uint32_t			  descriptor_slot =
		   rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ? T3D_WORLD_SHADOW_DESCRIPTOR_SLOT : ( uint32_t ) variant;
	for ( uint32_t active = 0u; active < 2u; ++active ) {
		const t3dPrimitiveClass_t primitive_class = active_classes[active];
		const uint32_t			  primitive = ( uint32_t ) primitive_class;
		if ( rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW && !ensureShaderCold( primitive_class, variant ) ) {
			return false;
		}
		if ( primitive_class == T3D_PRIMITIVE_MESH && variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT &&
			 m_bMeshFeedback[variant] ) {
			m_hCurrentFeedbackSet = m_pFeedbackOwner->get_fragment_feedback_set_cold(
				m_hShader[primitive][variant].rid(),
				T3D_RD_FORWARD_COLOR_VARIANT_COUNT + ( uint32_t ) variant );
			if ( !m_hCurrentFeedbackSet.is_valid() ) {
				return false;
			}
		}
		if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW &&
             primitive_class == T3D_PRIMITIVE_MESH && m_bEnvironmentReady &&
             m_hEnvironmentMeshShadowShader.isValid() ) {
            m_bCurrentEnvironmentShader[primitive] = true;
        } else if ( rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW &&
                   m_bEnvironmentShader[primitive][variant] ) {
            m_bCurrentEnvironmentShader[primitive] = true;
        }
        const RID shader = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW
            ? ( m_bCurrentEnvironmentShader[primitive] ? m_hEnvironmentMeshShadowShader.rid()
                : m_hShadowShader[primitive].rid() )
            : m_hShader[primitive][( uint32_t ) variant].rid();
		const t3dComposedSurfaceGpuBindings_t *pActiveComposed =
			m_bExperimentalComposed && primitive_class == T3D_PRIMITIVE_TERRAIN_STRIP &&
			rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW && ( uint32_t ) variant != 8u
				? pComposed : NULL;
        if ( !ensureSetsCold( rResources, rTerrain, rVT, shader, primitive_class, descriptor_slot, pActiveComposed ) ||
             !ensurePipelineCold( rContext, variant, primitive_class ) ) {
            return false;
        }
        if ( m_bCurrentEnvironmentShader[primitive] ) {
            const uint32_t environmentVariant = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW
                ? ENV_SHADOW_SLOT : ( uint32_t ) variant;
            if ( !pEnvironment || !ensureEnvironmentSetsCold( shader, primitive, environmentVariant, *pEnvironment ) ) {
                ++m_nStaleEnvironmentSetMisses;
                return false;
            }
        }
	}
	if ( variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT ) {
		if ( !m_forwardLighting.prepare( rContext, UINT32_MAX ) ) {
			return false;
		}
		for ( uint32_t active = 0u; active < 2u; ++active ) {
			const uint32_t primitive = ( uint32_t ) active_classes[active];
			const RID	   shader = m_hShader[primitive][( uint32_t ) variant].rid();
			m_hLightingSet[primitive] = m_forwardLighting.ensureSet( shader );
			if ( !m_hLightingSet[primitive].is_valid() ) return false;
			if ( m_bCompactShader[primitive][variant] ) {
				m_hCompactSet[primitive] = m_forwardLighting.ensureCompactSet( shader );
				if ( !m_hCompactSet[primitive].is_valid() ) return false;
			}
		}
	}
	m_bCurrentValid = true;
	return true;
}

bool Terrain3DWorldRaster::submitHot(
	const t3dRdForwardPassContext_t &rContext,
	Terrain3DWorldResources			&rResources ) {
	if ( !m_bCurrentValid || rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT || rContext.m_nDrawList < 0 ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	const RID			   indirect = rResources.getBuffer( WORLD_BUFFER_INDIRECT );
	if ( !rd || !indirect.is_valid() ) {
		return false;
	}
	const t3dPrimitiveClass_t active_classes[2] = { T3D_PRIMITIVE_TERRAIN_STRIP, T3D_PRIMITIVE_MESH };
	for ( uint32_t active = 0u; active < 2u; ++active ) {
		const uint32_t					primitive = ( uint32_t ) active_classes[active];
		const t3dPipelineEntry_t *const entry = findPipeline( m_currentKey[primitive] );
		const RID						currentSet0 = m_set0Cache.resolve( m_hCurrentSet0[primitive] );
		const RID						currentSet1 = m_set1Cache.resolve( m_hCurrentSet1[primitive] );
		if ( !entry || !currentSet0.is_valid() || !currentSet1.is_valid() ) {
			return false;
		}
		rd->draw_list_bind_render_pipeline( rContext.m_nDrawList, entry->m_pipeline.rid() );
		t3dRdDrawPushConstants_t selection{};
		selection.m_nHotIndex = rResources.hotIndex();
		memcpy( m_hotPushBytes.ptrw(), &selection, sizeof( selection ) );
		rd->draw_list_set_push_constant( rContext.m_nDrawList, m_hotPushBytes, sizeof( selection ) );
		rd->draw_list_bind_uniform_set( rContext.m_nDrawList, currentSet0, 0u );
		rd->draw_list_bind_uniform_set( rContext.m_nDrawList, currentSet1, 1u );
        if ( m_bCurrentEnvironmentShader[primitive] ) {
            const RID environmentSet = m_environmentSetCache.resolve( m_hCurrentEnvironmentSet[primitive] );
            if ( !environmentSet.is_valid() ) { ++m_nStaleEnvironmentSetMisses; return false; }
            rd->draw_list_bind_uniform_set( rContext.m_nDrawList, environmentSet, 5u );
        }
		if ( m_hLightingSet[primitive].is_valid() ) {
			const RID lightingSet = m_forwardLighting.resolveSet( m_hLightingSet[primitive] );
			if ( !lightingSet.is_valid() ) {
				return false;
			}
			rd->draw_list_bind_uniform_set( rContext.m_nDrawList, lightingSet, 2u );
			if ( m_hCompactSet[primitive].is_valid() )
				rd->draw_list_bind_uniform_set( rContext.m_nDrawList, m_hCompactSet[primitive], 4u );
		}
		if ( primitive == T3D_PRIMITIVE_MESH && m_hCurrentFeedbackSet.is_valid() ) {
			const RID feedbackSet =
				m_pFeedbackOwner ? m_pFeedbackOwner->resolve_fragment_feedback_set_hot( m_hCurrentFeedbackSet ) : RID();
			if ( !feedbackSet.is_valid() ) {
				return false;
			}
			rd->draw_list_bind_uniform_set( rContext.m_nDrawList, feedbackSet, 3u );
		}
		if ( active == 0u ) {
			rd->draw_list_draw_indirect( rContext.m_nDrawList,
										 false,
										 indirect,
										 rResources.bufferOffset( WORLD_BUFFER_INDIRECT ) +
											 t3dWorldIndirectOffsetBytes( T3D_WORLD_INDIRECT_TERRAIN_STRIP_INDEX ),
										 T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT,
										 T3D_WORLD_DRAW_INDIRECT_ARGS_SIZE_BYTES );
			m_context.recordRenderIndirectCommandsSubmitted( T3D_RD_CONTEXT_OWNER_WORLD,
															 T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT );
		} else {
			rd->draw_list_draw_indirect( rContext.m_nDrawList,
										 false,
										 indirect,
										 rResources.bufferOffset( WORLD_BUFFER_INDIRECT ) +
											 t3dWorldIndirectOffsetBytes( T3D_WORLD_INDIRECT_NONTERRAIN_INDEX ),
										 1u,
										 0u );
			m_context.recordRenderIndirectCommandsSubmitted( T3D_RD_CONTEXT_OWNER_WORLD, 1u );
		}
	}
	if ( rContext.m_passKind > T3D_RD_FORWARD_PASS_INVALID && rContext.m_passKind < T3D_RD_FORWARD_PASS_MAX ) {
		++m_nSubmitCount[( uint32_t ) rContext.m_passKind];
	}
	return true;
}

void Terrain3DWorldRaster::shutdownCold(
	void ) {
	m_bCurrentValid = false;
	freeSetsCold();
    m_stGpuImage.releaseCold( m_context );
	m_forwardLighting.shutdown();
	m_pipelines.clear();
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		m_hShadowShader[primitive].releaseTracked( m_context );
		for ( uint32_t i = 0u; i < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++i ) {
			m_hShader[primitive][i].releaseTracked( m_context );
		}
	}
	memset( m_bMeshFeedback, 0, sizeof( m_bMeshFeedback ) );
	memset( m_bCompactShader, 0, sizeof( m_bCompactShader ) );
	m_hCurrentFeedbackSet = RID();
    m_bEnvironmentReady = false;
	m_pFeedbackOwner = NULL;
	m_shaderCapabilities = t3dWorldShaderCapabilityKey_t{};
	m_bShaderCapabilitiesValid = false;
    m_hEnvironmentMeshShadowShader.releaseTracked( m_context );
	m_bInitialized = false;
}

bool Terrain3DWorldRaster::isReady(
	void ) const {
	return m_bInitialized && m_bCurrentValid;
}
uint64_t Terrain3DWorldRaster::getSubmitCount(
	t3dRdForwardPassKind_t pass ) const {
	return pass > T3D_RD_FORWARD_PASS_INVALID && pass < T3D_RD_FORWARD_PASS_MAX ? m_nSubmitCount[( uint32_t ) pass]
																				: 0u;
}
uint64_t Terrain3DWorldRaster::getPrepareCount(
	void ) const {
	return m_nPrepareCount;
}
