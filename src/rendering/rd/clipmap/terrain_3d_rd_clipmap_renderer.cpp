// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/forward/terrain_3d_rd_forward_pipeline_base.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_binding_builder.h"
#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_sync.h"
#include "rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_variant.h"

#include "core/utility/logger.h"
#include "rendering/rd/vt/core/terrain_3d_vt_contract.h"

#include <math.h>
#include <string.h>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_pipeline_specialization_constant.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/render_scene_buffers.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer_batch.h"

CLASS_NAME_STATIC( "Terrain3DRdClipmapRenderer" );

#define T3D_TERRAIN_VT_ALLOCATION_BINDING 26u
#define T3D_TERRAIN_VT_PAGE_TABLE_BINDING 27u
#define T3D_TERRAIN_VT_PHYSICAL_BINDING_BASE 28u
#define T3D_TERRAIN_VT_TAIL_METADATA_BINDING 32u
#define T3D_TERRAIN_VT_TAIL_TEXTURE_BINDING_BASE 33u
#define T3D_TERRAIN_VT_MIP_LAYOUT_BINDING 37u

static bool t3dRdSupportsDynamicBufferMetadata( const Ref<RDShaderSPIRV> &rShader ) {
	return rShader.is_valid() && rShader->has_method( StringName( "set_dynamic_buffers" ) );
}

static void t3dRdMarkDynamicBuffers( const Ref<RDShaderSPIRV> &rShader, const PackedInt64Array &rDynamicBuffers ) {
	if ( rShader.is_valid() ) {
		rShader->call( StringName( "set_dynamic_buffers" ), rDynamicBuffers );
	}
}


Terrain3DRdClipmapRenderer::Terrain3DRdClipmapRenderer( void ) :
        m_context( Terrain3DRdContext::getShared() ), m_forwardLighting( m_context ),
        m_cullPipelines( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN ),
        m_viewPipelines( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN ),
        m_descriptorCache( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN ),
        m_forwardPipelines( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN ) {
}

Terrain3DRdClipmapRenderer::~Terrain3DRdClipmapRenderer( void ) {
	shutdown();
}

void Terrain3DRdClipmapRenderer::setError( const String &rError ) {
	bool bReport = true;
	{
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		if ( m_lastError == rError ) {
			++m_nRepeatedErrorCount;
			bReport = m_nRepeatedErrorCount == 1u || ( m_nRepeatedErrorCount % 120u ) == 0u;
		} else {
			m_lastError = rError;
			m_nRepeatedErrorCount = 1u;
		}
	}
	m_bHasError.store( true, T3D_MEMORY_ORDER_RELEASE );
	// A terrain pass failure is lane-local. The shared RD context is reserved for
	// actual device/resource ownership failures; poisoning it here turns a
	// recoverable descriptor/pass error into a fake device-loss cascade.
	if ( bReport ) {
		LOG( ERROR, "[RD alpha] ", rError );
	}
}

void Terrain3DRdClipmapRenderer::clearError( void ) {
	m_bHasError.store( false, T3D_MEMORY_ORDER_RELEASE );
}

String Terrain3DRdClipmapRenderer::getLastError( void ) const {
	if ( !m_bHasError.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return String();
	}
	Terrain3DScopedLock lock( m_diagnosticsMutex );
	return m_lastError;
}

bool Terrain3DRdClipmapRenderer::prepareStripParts( const t3dClipmapStripPart_t *const pLod0Parts,
        const uint16_t nLod0PartCount,
        const t3dClipmapStripPart_t *const pOuterParts,
        const uint16_t nOuterPartCount ) {
	Terrain3DScopedLock state_lock( m_resourceStateMutex );
	if ( !pLod0Parts || !pOuterParts || nLod0PartCount == 0u || nOuterPartCount == 0u
	        || nLod0PartCount > T3D_CLIPMAP_STRIP_PART_CAPACITY || nOuterPartCount > T3D_CLIPMAP_STRIP_PART_CAPACITY ) {
		return false;
	}
	memset( m_szStripParts, 0, sizeof( m_szStripParts ) );
	m_szStripPartCounts[LOD0_MESH] = nLod0PartCount;
	m_szStripPartCounts[OUTER_RING_MESH] = nOuterPartCount;
	memcpy( m_szStripParts[LOD0_MESH], pLod0Parts, sizeof( t3dClipmapStripPart_t ) * nLod0PartCount );
	memcpy( m_szStripParts[OUTER_RING_MESH], pOuterParts, sizeof( t3dClipmapStripPart_t ) * nOuterPartCount );
	for ( uint32_t type = 0u; type < MESH_TYPE_COUNT; ++type ) {
		for ( uint32_t part = 0u; part < m_szStripPartCounts[type]; ++part ) {
			const t3dClipmapStripPart_t &descriptor = m_szStripParts[type][part];
			if ( descriptor.m_nPartCode >= T3D_CLIPMAP_PART_CODE_COUNT
			        || ( descriptor.m_nFlags & ~T3D_CLIPMAP_STRIP_KNOWN_FLAG_MASK ) != 0u
			        || !t3dClipmapStripPartFitsVirtualVertexRange( descriptor ) ) {
				return false;
			}
		}
	}
	m_stripPartBytes.resize( sizeof( m_szStripParts ) );
	if ( m_stripPartBytes.size() != ( int64_t )( sizeof( m_szStripParts ) ) ) {
		return false;
	}
	memcpy( m_stripPartBytes.ptrw(), m_szStripParts, sizeof( m_szStripParts ) );
	return true;
}

uint32_t Terrain3DRdClipmapRenderer::getStripPartCountForGroup( const uint32_t nGroup ) const {
	return nGroup == T3D_RD_DRAW_GROUP_LOD0_NEAR ? m_szStripPartCounts[LOD0_MESH] : m_szStripPartCounts[OUTER_RING_MESH];
}

uint32_t Terrain3DRdClipmapRenderer::getStripDescriptorBaseForGroup( const uint32_t nGroup ) const {
	return nGroup == T3D_RD_DRAW_GROUP_LOD0_NEAR ? 0u : T3D_CLIPMAP_STRIP_PART_CAPACITY;
}

bool Terrain3DRdClipmapRenderer::buildStripIndirectCommands( const t3dRdCpuCullResult_t &rResult ) {
	const uint32_t required_bytes = sizeof( t3dRdDrawIndirectCommand_t ) * T3D_CLIPMAP_STRIP_COMMAND_CAPACITY;
	if ( m_cpuStripIndirectBytes.size() != required_bytes ) {
		return false;
	}
	t3dRdDrawIndirectCommand_t *const commands = ( t3dRdDrawIndirectCommand_t * )( m_cpuStripIndirectBytes.ptrw() );
	memset( commands, 0, required_bytes );
	for ( uint32_t group = 0u; group < T3D_RD_DRAW_GROUP_COUNT; ++group ) {
		const t3dMeshType_t type = group == T3D_RD_DRAW_GROUP_LOD0_NEAR ? LOD0_MESH : OUTER_RING_MESH;
		const uint32_t part_count = m_szStripPartCounts[type];
		for ( uint32_t part = 0u; part < part_count; ++part ) {
			t3dRdDrawIndirectCommand_t &command = commands[group * T3D_CLIPMAP_STRIP_PART_CAPACITY + part];
			command.m_nVertexCount = t3dClipmapStripActualVertexCount( m_szStripParts[type][part] );
			command.m_nInstanceCount = rResult.m_szCommands[group].m_nInstanceCount;
			command.m_nFirstVertex = t3dClipmapStripVirtualFirstVertex( part );
			command.m_nFirstInstance = group * T3D_MAX_CLIPMAP_LEVELS;
		}
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureDevice( void ) {
	if ( m_context.isReady() ) {
		return true;
	}
	if ( !m_context.initialize() ) {
		if ( Terrain3DRdContext::getGlobalDevice() == NULL ) {
			setError( "Global RenderingDevice is unavailable" );
		} else if ( m_context.getState() == T3D_RD_CONTEXT_STATE_FAILED ) {
			const Dictionary diagnostics = m_context.getDiagnostics();
			setError( String( "Terrain3D shared RD context is failed: " )
			        + String( diagnostics.get( "last_failure", "unknown RD context failure" ) ) );
		} else {
			setError( "Terrain3D shared RD context could not initialize" );
		}
		return false;
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureShaders( void ) {
	if ( !m_context.getDevice() ) { return false; }
	static const t3dRdShaderId_t s_ids[3] = { T3D_RD_SHADER_NEAR_ALIAS, T3D_RD_SHADER_MIDDLE_ALIAS, T3D_RD_SHADER_FAR_ALIAS };
	static const char *const s_names[3] = { "T3D.PipelineShader.Near", "T3D.PipelineShader.Middle", "T3D.PipelineShader.Far" };
	for ( uint32_t i = 0u; i < 3u; ++i ) {
		if ( !m_hszShaders[i].isValid() && !m_hszShaders[i].createTracked( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN, s_names[i], s_ids[i] ) ) {
			setError( "Failed to acquire Terrain3D catalog raster shader" ); return false;
		}
	}
	if ( !m_hCullShader.isValid() && !m_hCullShader.createTracked( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN, "T3D.Shader.Cull", T3D_RD_SHADER_TERRAIN_CULL ) ) {
		setError( "Failed to acquire Terrain3D catalog cull shader" ); return false;
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureForwardShaders( const t3dTerrainShaderPath_t path,
                                                       const t3dRdForwardShaderVariant_t variant, const bool bOneSunLighting )
{
	if ( !m_context.getDevice() ) { return false; }
	if ( path >= T3D_TERRAIN_SHADER_PATH_COUNT || variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX ) {
		return false;
	}
	const bool one_sun = bOneSunLighting && variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT;
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		Terrain3DRdShader& shader = one_sun ? m_hszForwardOneSunShaders[ path ][ tier ][ variant ] : m_hszForwardShaders[ path ][ tier ][ variant ];
		if ( shader.isValid() ) {
			continue;
		}
		const String name = String( one_sun ? "T3D.Forward.Shader.TerrainOneSun." : "T3D.Forward.Shader.Terrain." ) + String::num_int64( path ) + "." + String::num_int64( tier ) + "." + String::num_int64( variant );
		const t3dRdShaderId_t id = one_sun ? t3dRdTerrainOneSunId( path, tier, variant ) : t3dRdTerrainForwardId( path, tier, variant );
		if ( !shader.createTracked( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN, name, id ) ) {
			return false;
		}
	}
	return true;
}

void Terrain3DRdClipmapRenderer::freeForwardPipelines( void ) {
	m_forwardPipelines.clear();
	m_currentForwardPipelineEntry = t3dRdForwardPipelineEntry_t{};
	m_bLastForwardPipelineKeyValid = false;
}



const Terrain3DRdClipmapRenderer::t3dRdForwardPipelineEntry_t *Terrain3DRdClipmapRenderer::findForwardPipeline(
        const t3dRdForwardPipelineKey_t &rKey ) const {
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		const auto *const entry = m_forwardPipelines.find( { rKey, tier } );
		if ( !entry ) {
			return NULL;
		}
		m_currentForwardPipelineEntry.m_hszPipelines[tier] = entry->m_pipeline.rid();
	}
	m_currentForwardPipelineEntry.m_key = rKey;
	return &m_currentForwardPipelineEntry;
}

static void t3dRdApplyShadowCasterPolicy( const t3dRdFrameSnapshot_t &rSnapshot, t3dRdCpuCullResult_t &rResult ) {
	uint32_t visible_count = 0u;
	for ( uint32_t group = 0u; group < T3D_RD_DRAW_GROUP_COUNT; ++group ) {
		t3dRdIndexedIndirectCommand_t &command = rResult.m_szCommands[group];
		uint32_t write = 0u;
		for ( uint32_t read = 0u; read < command.m_nInstanceCount; ++read ) {
			const uint32_t patch_index = rResult.m_szVisibleIndices[group * T3D_MAX_CLIPMAP_LEVELS + read];
			if ( patch_index >= rSnapshot.m_nPatchCount ) {
				continue;
			}
			const t3dRdPatch_t &patch = rSnapshot.m_szPatches[patch_index];
			bool enabled = true;
			int32_t limit = rSnapshot.m_nShadowLodCount;
			if ( patch.m_nTier == 0u ) {
				enabled = rSnapshot.m_bNearShadowEnabled;
				if ( rSnapshot.m_nNearShadowLodCount >= 0 ) {
					limit = rSnapshot.m_nNearShadowLodCount;
				}
			} else if ( patch.m_nTier == 1u ) {
				enabled = rSnapshot.m_bMiddleShadowEnabled;
				if ( rSnapshot.m_nMiddleShadowLodCount >= 0 ) {
					limit = rSnapshot.m_nMiddleShadowLodCount;
				}
			} else {
				enabled = rSnapshot.m_bFarShadowEnabled;
				if ( rSnapshot.m_nFarShadowLodCount >= 0 ) {
					limit = rSnapshot.m_nFarShadowLodCount;
				}
			}
			if ( !enabled || limit <= 0 || patch.m_nLod >= ( uint32_t )( limit ) ) {
				continue;
			}
			rResult.m_szVisibleIndices[group * T3D_MAX_CLIPMAP_LEVELS + write++] = patch_index;
			++visible_count;
		}
		command.m_nInstanceCount = write;
	}
	rResult.m_nVisibleCount = visible_count;
}

t3dTerrainShaderPath_t Terrain3DRdClipmapRenderer::selectTerrainShaderPath( const t3dRdFrameSnapshot_t &rSnapshot ) const {
	bool editor_debug = rSnapshot.m_materialConfig.m_nDebugFlags != 0u;
	for ( uint32_t i = 0u; !editor_debug && i < T3D_ARRAY_COUNT( rSnapshot.m_editorOverlayConfig.m_szDecals ); ++i ) {
		editor_debug = rSnapshot.m_editorOverlayConfig.m_szDecals[i].m_nVisible != 0u;
	}
	if ( editor_debug ) {
		return T3D_TERRAIN_SHADER_PATH_EDITOR_DEBUG_MIXED;
	}
	return rSnapshot.m_nTerrainMaterialBackend == T3D_TERRAIN_MATERIAL_BACKEND_CANONICAL_VT_V3
	        ? T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL
	        : T3D_TERRAIN_SHADER_PATH_COMPATIBILITY_MIXED;
}

static uint8_t t3dRdTerrainShippingFeatureClass( const t3dRdFrameSnapshot_t &rSnapshot, const t3dTerrainShaderPath_t shaderPath ) {
	if ( shaderPath != T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL ) {
		return T3D_TERRAIN_SHIPPING_FEATURE_RUNTIME;
	}
	const uint32_t flags = rSnapshot.m_materialConfig.m_nFeatureFlags;
	uint8_t feature_class = 0u;
	if ( ( flags & T3D_RD_MATERIAL_FEATURE_AUTO_SHADER ) != 0u ) {
		feature_class |= T3D_TERRAIN_SHIPPING_FEATURE_AUTO_SHADER;
	}
	if ( ( flags & T3D_RD_MATERIAL_FEATURE_DUAL_SCALING ) != 0u ) {
		feature_class |= T3D_TERRAIN_SHIPPING_FEATURE_DUAL_SCALE;
	}
	if ( ( flags & T3D_RD_MATERIAL_FEATURE_MEGAVT ) != 0u ) {
		feature_class |= T3D_TERRAIN_SHIPPING_FEATURE_MEGAVT;
	}
	return feature_class;
}

static uint8_t t3dRdTerrainLightingModelClass( const t3dRdMaterialConfig_t &rConfig, const t3dTerrainShaderPath_t shaderPath,
        const t3dRdForwardShaderVariant_t variant, const uint32_t nTier ) {
	if ( shaderPath != T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL || variant >= T3D_RD_FORWARD_COLOR_VARIANT_COUNT || nTier >= 3u ) {
		return T3D_TERRAIN_LIGHTING_MODEL_RUNTIME;
	}
	// Runtime values above PBR already fall through to the PBR branch in the
	// existing shader. Clamp to that semantically identical bounded class.
	return ( uint8_t )( MIN( rConfig.m_szTier[nTier].m_nLightingModel, ( uint32_t )( T3D_TERRAIN_LIGHTING_MODEL_PBR ) ) );
}

static uint8_t t3dRdTerrainSceneIntegrationClass( const t3dRdForwardPassContext_t &rContext,
        const t3dTerrainShaderPath_t shaderPath, const t3dRdForwardShaderVariant_t variant, const bool bOneSunLighting ) {
	if ( shaderPath != T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL || variant >= T3D_RD_FORWARD_COLOR_VARIANT_COUNT ) {
		return T3D_TERRAIN_SCENE_INTEGRATION_RUNTIME;
	}
	if ( bOneSunLighting ) {
		return T3D_TERRAIN_SCENE_INTEGRATION_NONE;
	}
	const bool bReflections = rContext.m_hReflectionProbeBuffer.is_valid() && rContext.m_hReflectionAtlasTexture.is_valid();
	const bool bDecals = rContext.m_hDecalBuffer.is_valid() && rContext.m_hDecalAtlasTexture.is_valid()
	        && rContext.m_hDecalAtlasSrgbTexture.is_valid();
	return !bReflections && !bDecals ? T3D_TERRAIN_SCENE_INTEGRATION_NONE : T3D_TERRAIN_SCENE_INTEGRATION_DYNAMIC;
}

static uint8_t t3dRdTerrainDnrOutputClass( const t3dRdMaterialConfig_t &rConfig, const t3dTerrainShaderPath_t shaderPath,
        const t3dRdForwardShaderVariant_t variant, const uint32_t nTier ) {
	if ( shaderPath != T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL || nTier >= 3u
	        || ( variant != T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS
	                && variant != T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI ) ) {
		return T3D_TERRAIN_DNR_OUTPUT_RUNTIME;
	}
	uint8_t output_class = 0u;
	if ( ( rConfig.m_nOutputFlags & T3D_RD_MATERIAL_OUTPUT_NORMAL ) != 0u && rConfig.m_szTier[nTier].m_nSurfaceResponse != 0u ) {
		output_class |= T3D_TERRAIN_DNR_OUTPUT_NORMAL;
	}
	if ( ( rConfig.m_nOutputFlags & T3D_RD_MATERIAL_OUTPUT_ROUGHNESS ) != 0u ) {
		output_class |= T3D_TERRAIN_DNR_OUTPUT_ROUGHNESS;
	}
	return output_class;
}

static uint8_t t3dRdTerrainMiddleSurfaceClass( const t3dRdMaterialConfig_t &rConfig, const t3dTerrainShaderPath_t shaderPath ) {
	if ( shaderPath != T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL ) {
		return T3D_TERRAIN_MIDDLE_SURFACE_RUNTIME;
	}
	// Sampling mode 2 is the serialized TERRAIN_SAMPLING_SINGLE_SAMPLE value
	// selected by MATERIAL_SAMPLING_FAST / MIDDLE_SURFACE_BAKED_COMPOSITE.
	const bool bComposite = rConfig.m_szTier[1].m_nSamplingMode == 2u
	        && ( rConfig.m_nFeatureFlags & T3D_RD_MATERIAL_FEATURE_MIDDLE_COMPOSITE ) != 0u;
	return bComposite ? T3D_TERRAIN_MIDDLE_SURFACE_COMPOSITE : T3D_TERRAIN_MIDDLE_SURFACE_DETAILED;
}

RID Terrain3DRdClipmapRenderer::getForwardShader( const uint32_t nTier, const t3dRdForwardShaderVariant_t variant,
        const bool bOneSunLighting, const t3dTerrainShaderPath_t shaderPath ) const {
	if ( shaderPath >= T3D_TERRAIN_SHADER_PATH_COUNT || nTier >= 3u || variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX ) {
		return RID();
	}
	if ( bOneSunLighting && variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT ) {
		return m_hszForwardOneSunShaders[shaderPath][nTier][variant].rid();
	}
	return m_hszForwardShaders[shaderPath][nTier][variant].rid();
}

static t3dTerrainShaderPassClass_t t3dRdTerrainShaderPassClass( const t3dRdForwardShaderVariant_t variant, const bool bOneSunLighting ) {
	if ( variant == T3D_RD_FORWARD_SHADER_DEPTH ) {
		return T3D_TERRAIN_SHADER_PASS_DEPTH_ONLY;
	}
	if ( variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS || variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI ) {
		return T3D_TERRAIN_SHADER_PASS_DEPTH_NORMAL_ROUGHNESS;
	}
	return bOneSunLighting ? T3D_TERRAIN_SHADER_PASS_COLOR_ONE_SUN : T3D_TERRAIN_SHADER_PASS_COLOR_GENERAL;
}

bool Terrain3DRdClipmapRenderer::ensureForwardPipelines( const t3dRdForwardPassContext_t &rContext,
        const t3dRdForwardShaderVariant_t variant, const bool bOneSunLighting, const t3dTerrainShaderPath_t shaderPath,
        const uint8_t nShippingFeatureClass, const uint8_t *const pLightingModelClasses, const uint8_t nSceneIntegrationClass,
        const uint8_t *const pDnrOutputClasses, const uint8_t nMiddleSurfaceClass ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || rContext.m_nFramebufferFormat < 0 ) {
		return false;
	}
	t3dRdForwardPipelineKey_t key;
	if ( !t3dRdCompileForwardPipelineBaseKey( rd, rContext, variant, true, key ) ) {
		setError( "Terrain3D Forward+ sample-count metadata does not match native framebuffer format" );
		return false;
	}
	key.m_bOneSunLighting = bOneSunLighting && variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT;
	key.m_shaderPath = shaderPath;
	key.m_nShippingFeatureClass = nShippingFeatureClass;
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		key.m_nszLightingModelClass[tier] = pLightingModelClasses ? pLightingModelClasses[tier] : T3D_TERRAIN_LIGHTING_MODEL_RUNTIME;
	}
	key.m_nSceneIntegrationClass = nSceneIntegrationClass;
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		key.m_nszDnrOutputClass[tier] = pDnrOutputClasses ? pDnrOutputClasses[tier] : T3D_TERRAIN_DNR_OUTPUT_RUNTIME;
	}
	key.m_nMiddleSurfaceClass = nMiddleSurfaceClass;
	if ( findForwardPipeline( key ) ) {
		m_lastForwardPipelineKey = key;
		m_bLastForwardPipelineKeyValid = true;
		return true;
	}
	t3dRdForwardPipelineStateOptions_t state_options;
	state_options.m_frontFace = RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
	state_options.m_cullMode = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW
	        ? ( key.m_bReverseCull ? RenderingDevice::POLYGON_CULL_FRONT : RenderingDevice::POLYGON_CULL_BACK )
	        : RenderingDevice::POLYGON_CULL_DISABLED;
	state_options.m_nBlendAttachmentCount = t3dRdForwardBlendAttachmentCount( variant, false );
	state_options.m_bEnableDepthWrite = !key.m_bDepthPrepassActive;
	state_options.m_depthCompare =
	        key.m_bDepthPrepassActive ? RenderingDevice::COMPARE_OP_EQUAL : RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
	t3dRdForwardPipelineStates_t pipeline_states;
	if ( !t3dRdBuildForwardPipelineStates( key, state_options, pipeline_states ) ) {
		setError( "Failed to allocate Terrain3D Forward+ pipeline state" );
		return false;
	}
	Ref<RDPipelineRasterizationState> raster = pipeline_states.m_raster;
	Ref<RDPipelineMultisampleState> multisample = pipeline_states.m_multisample;
	Ref<RDPipelineDepthStencilState> depth = pipeline_states.m_depth;
	Ref<RDPipelineColorBlendState> blend = pipeline_states.m_blend;
	m_context.beginResourceRebuild();
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		TypedArray<Ref<RDPipelineSpecializationConstant>> specialization_constants;
		if ( key.m_shaderPath == T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL ) {
			Ref<RDPipelineSpecializationConstant> feature_class_constant;
			feature_class_constant.instantiate();
			if ( feature_class_constant.is_null() ) {
				m_context.endResourceRebuild();
				setError( "Failed to allocate Terrain3D material-feature specialization constant" );
				return false;
			}
			feature_class_constant->set_constant_id( T3D_TERRAIN_SPEC_CONSTANT_SHIPPING_FEATURE_CLASS );
			feature_class_constant->set_value( Variant( ( int64_t )( key.m_nShippingFeatureClass ) ) );
			specialization_constants.push_back( feature_class_constant );
			if ( variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT ) {
				Ref<RDPipelineSpecializationConstant> lighting_model_constant;
				lighting_model_constant.instantiate();
				Ref<RDPipelineSpecializationConstant> scene_integration_constant;
				scene_integration_constant.instantiate();
				if ( lighting_model_constant.is_null() || scene_integration_constant.is_null() ) {
					m_context.endResourceRebuild();
					setError( "Failed to allocate Terrain3D lighting specialization constants" );
					return false;
				}
				lighting_model_constant->set_constant_id( T3D_TERRAIN_SPEC_CONSTANT_LIGHTING_MODEL_CLASS );
				lighting_model_constant->set_value( Variant( ( int64_t )( key.m_nszLightingModelClass[tier] ) ) );
				scene_integration_constant->set_constant_id( T3D_TERRAIN_SPEC_CONSTANT_SCENE_INTEGRATION_CLASS );
				scene_integration_constant->set_value( Variant( ( int64_t )( key.m_nSceneIntegrationClass ) ) );
				specialization_constants.push_back( lighting_model_constant );
				specialization_constants.push_back( scene_integration_constant );
			}
			if ( variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS
			        || variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI ) {
				Ref<RDPipelineSpecializationConstant> dnr_output_constant;
				dnr_output_constant.instantiate();
				if ( dnr_output_constant.is_null() ) {
					m_context.endResourceRebuild();
					setError( "Failed to allocate Terrain3D DNR specialization constant" );
					return false;
				}
				dnr_output_constant->set_constant_id( T3D_TERRAIN_SPEC_CONSTANT_DNR_OUTPUT_CLASS );
				dnr_output_constant->set_value( Variant( ( int64_t )( key.m_nszDnrOutputClass[tier] ) ) );
				specialization_constants.push_back( dnr_output_constant );
			}
			if ( tier == 1u ) {
				Ref<RDPipelineSpecializationConstant> middle_surface_constant;
				middle_surface_constant.instantiate();
				if ( middle_surface_constant.is_null() ) {
					m_context.endResourceRebuild();
					setError( "Failed to allocate Terrain3D middle-surface specialization constant" );
					return false;
				}
				middle_surface_constant->set_constant_id( T3D_TERRAIN_SPEC_CONSTANT_MIDDLE_SURFACE_CLASS );
				middle_surface_constant->set_value( Variant( ( int64_t )( key.m_nMiddleSurfaceClass ) ) );
				specialization_constants.push_back( middle_surface_constant );
			}
		}
		t3dTerrainShaderPipelineKey_t shader_key;
		shader_key.m_nPassClass = ( uint8_t )( t3dRdTerrainShaderPassClass( variant, key.m_bOneSunLighting ) );
		shader_key.m_nTier = ( uint8_t )( tier );
		shader_key.m_nShaderPath = ( uint8_t )( key.m_shaderPath );
		shader_key.m_nMaterialBackend = key.m_shaderPath == T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL
		        ? T3D_TERRAIN_MATERIAL_BACKEND_CANONICAL_VT_V3
		        : T3D_TERRAIN_MATERIAL_BACKEND_COMPATIBILITY_MIXED;
		shader_key.m_nShippingFeatureClass = key.m_nShippingFeatureClass;
		if ( variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT ) {
			shader_key.m_nLightingModelClass = key.m_nszLightingModelClass[tier];
			shader_key.m_nSceneIntegrationClass = key.m_nSceneIntegrationClass;
		}
		if ( variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS
		        || variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI ) {
			shader_key.m_nDnrOutputClass = key.m_nszDnrOutputClass[tier];
		}
		if ( tier == 1u ) {
			shader_key.m_nMiddleSurfaceClass = key.m_nMiddleSurfaceClass;
		}
		if ( !t3dTerrainShaderPipelineKeyIsValid( shader_key ) ) {
			m_context.endResourceRebuild();
			setError( "Invalid bounded Terrain3D shader pipeline key" );
			return false;
		}
		const RID shader = getForwardShader( tier, variant, key.m_bOneSunLighting, key.m_shaderPath );
		const String name = String( key.m_bOneSunLighting ? "T3D.Forward.Pipeline.TerrainOneSun." : "T3D.Forward.Pipeline.Terrain." )
		        + String::num_int64( variant ) + "." + String::num_int64( tier ) + ".P" + String::num_int64( key.m_shaderPath ) + ".F"
		        + String::num_int64( key.m_nShippingFeatureClass ) + ".L" + String::num_int64( key.m_nszLightingModelClass[tier] ) + ".S"
		        + String::num_int64( key.m_nSceneIntegrationClass ) + ".D" + String::num_int64( key.m_nszDnrOutputClass[tier] )
		        + ".M" + String::num_int64( key.m_nMiddleSurfaceClass );
		const RID pipeline = m_forwardPipelines.getOrCreateRaster( { key, tier }, name, shader,
		        rContext.m_nFramebufferFormat,
		        -1,
		        RenderingDevice::RENDER_PRIMITIVE_TRIANGLE_STRIPS,
		        raster,
		        multisample,
		        depth,
		        blend,
		        0,
		        0u,
		        specialization_constants );
		if ( !pipeline.is_valid() ) {
			m_forwardPipelines.clear();
			m_context.endResourceRebuild();
			setError( "Failed to create framebuffer-compatible Terrain3D Forward+ terrain pipeline" );
			return false;
		}
	}
	m_context.endResourceRebuild();
	m_lastForwardPipelineKey = key;
	m_bLastForwardPipelineKeyValid = true;
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureSamplers( void ) {
	if ( !m_context.getDevice() ) { return false; }
	if ( !m_hTerrainSampler.isValid() ) {
		Ref<RDSamplerState> state;
		state.instantiate();
		state->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_mip_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		state->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		if ( !m_hTerrainSampler.createTracked( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN, "T3D.Sampler.Terrain", state ) ) {
			setError( "Failed to create terrain data sampler" );
			return false;
		}
	}
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		const bool bNearest = i >= 2u;
		const bool bAnisotropic = i == 0u || i == 2u;
		if ( !m_hszMaterialSamplers[i].isValid() ) {
			Ref<RDSamplerState> state;
			state.instantiate();
			state->set_mag_filter( bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			state->set_min_filter( bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			state->set_mip_filter( bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			state->set_use_anisotropy( bAnisotropic );
			state->set_anisotropy_max( bAnisotropic ? 16.0f : 1.0f );
			state->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT );
			state->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT );
			state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
			const String name = String( "T3D.Sampler.Material." ) + String::num_int64( i );
			if ( !m_hszMaterialSamplers[i].createTracked( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN, name, state ) ) {
				setError( "Failed to create material sampler" );
				return false;
			}
		}
		if ( !m_hszMapSamplers[i].isValid() ) {
			Ref<RDSamplerState> map_state;
			map_state.instantiate();
			map_state->set_mag_filter( bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			map_state->set_min_filter( bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			map_state->set_mip_filter( bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			map_state->set_use_anisotropy( bAnisotropic );
			map_state->set_anisotropy_max( bAnisotropic ? 16.0f : 1.0f );
			map_state->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
			map_state->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
			map_state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
			const String map_name = String( "T3D.Sampler.Map." ) + String::num_int64( i );
			if ( !m_hszMapSamplers[i].createTracked( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN, map_name, map_state ) ) {
				setError( "Failed to create terrain map sampler" );
				return false;
			}
		}
		if ( !m_hszVtSamplers[i].isValid() ) {
			Ref<RDSamplerState> vt_state;
			vt_state.instantiate();
			vt_state->set_mag_filter( bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			vt_state->set_min_filter( bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			vt_state->set_mip_filter( bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			vt_state->set_use_anisotropy( bAnisotropic );
			vt_state->set_anisotropy_max( bAnisotropic ? ( float )( T3D_VT_MAX_ANISOTROPY ) : 1.0f );
			vt_state->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
			vt_state->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
			vt_state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
			const String vt_name = String( "T3D.Sampler.VT." ) + String::num_int64( i );
			if ( !m_hszVtSamplers[i].createTracked( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN, vt_name, vt_state ) ) {
				setError( "Failed to create Sparse VT sampler" );
				return false;
			}
		}
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureFallbackTextures( void ) {
	if ( m_hFallbackDisplacementTexture.isValid() && m_hFallbackArrayTexture.isValid() ) { return true; }
	PackedByteArray texel; texel.resize( 4 * sizeof( float ) );
	const float neutral[4] = { 0.5f, 0.5f, 0.5f, 1.0f }; memcpy( texel.ptrw(), neutral, sizeof( neutral ) );
	TypedArray<PackedByteArray> initialData; initialData.push_back( texel );
	auto createFallback = [&]( Terrain3DRdTexture &rTexture, const char *pName, RenderingDevice::TextureType type ) -> bool {
		if ( rTexture.isValid() ) { return true; }
		t3dRdTextureSpec_t spec; spec.m_name = pName; spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE; spec.m_layout.m_format = RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT;
		spec.m_layout.m_type = type; spec.m_layout.m_nWidth = 1u; spec.m_layout.m_nHeight = 1u; spec.m_layout.m_nLayers = 1u; spec.m_layout.m_nMipmaps = 1u;
		spec.m_layout.m_nUsageBits = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT;
		spec.m_layout.m_nBytes = sizeof( neutral ); return rTexture.createTracked( m_context, spec, initialData );
	};
	if ( !createFallback( m_hFallbackDisplacementTexture, "T3D.Texture.DisplacementFallback", RenderingDevice::TEXTURE_TYPE_2D ) ) {
		setError( "Failed to create neutral Terrain3D displacement texture" ); return false;
	}
	if ( !createFallback( m_hFallbackArrayTexture, "T3D.Texture.ArrayFallback", RenderingDevice::TEXTURE_TYPE_2D_ARRAY ) ) {
		setError( "Failed to create neutral Terrain3D array fallback texture" ); return false;
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureBuffers( void ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	const uint32_t frame_bytes = ( uint32_t )( sizeof( t3dRdFrameUniforms_t ) );
	const uint32_t material_bytes = ( uint32_t )( sizeof( t3dRdMaterialLayer_t ) * T3D_RD_MATERIAL_LAYER_COUNT );
	const uint32_t material_config_bytes = ( uint32_t )( sizeof( t3dRdMaterialConfig_t ) );
	const uint32_t editor_overlay_bytes = ( uint32_t )( sizeof( t3dRdEditorOverlayConfig_t ) );
	const uint32_t patch_bytes = ( uint32_t )( sizeof( t3dRdPatch_t ) * T3D_MAX_CLIPMAP_LEVELS );
	const uint32_t visible_bytes = ( uint32_t )( sizeof( uint32_t ) * T3D_RD_DRAW_GROUP_COUNT * T3D_MAX_CLIPMAP_LEVELS );
	const uint32_t indirect_bytes = T3D_CLIPMAP_STRIP_INDIRECT_OFFSET_BYTES;
	const uint32_t strip_indirect_bytes = T3D_CLIPMAP_STRIP_INDIRECT_BYTES;
	const uint32_t strip_part_bytes = ( uint32_t )( sizeof( m_szStripParts ) );
	if ( m_stripPartBytes.size() != strip_part_bytes ) {
		setError( "Terrain3D procedural strip descriptors were not prepared before RD initialization" );
		return false;
	}
	const BitField<RenderingDevice::BufferCreationBits> persistent_bits =
	        m_bPersistentCpuBuffers ? Terrain3DRdGpuBuffer::persistentCreationBits() : BitField<RenderingDevice::BufferCreationBits>( 0 );
	if ( !m_hFrameUniformBuffer.isValid() ) {
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.Frame.UBO";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		spec.m_type = T3D_RD_CONTEXT_RESOURCE_UNIFORM_BUFFER;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_UNIFORM;
		spec.m_creationBits = persistent_bits;
		if ( !m_hFrameUniformBuffer.configure( spec ) || !m_hFrameUniformBuffer.createTracked( m_context, frame_bytes ) ) {
			m_context.recordResourceCreationFailure( "T3D.Frame.UBO", T3D_RD_CONTEXT_RESOURCE_UNIFORM_BUFFER );
			return false;
		}
	}
	if ( !m_hMaterialBuffer.isValid() ) {
		m_bMaterialUploadInitialized = false;
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.Materials";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_MATERIAL_TABLE;
		spec.m_creationBits = persistent_bits;
		if ( !m_hMaterialBuffer.configure( spec ) || !m_hMaterialBuffer.createTracked( m_context, material_bytes ) ) {
			m_context.recordResourceCreationFailure( "T3D.Materials", T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER );
			return false;
		}
	}
	if ( !m_hMaterialConfigBuffer.isValid() ) {
		m_bMaterialUploadInitialized = false;
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.MaterialConfig";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_MATERIAL_TABLE;
		spec.m_creationBits = persistent_bits;
		if ( !m_hMaterialConfigBuffer.configure( spec ) || !m_hMaterialConfigBuffer.createTracked( m_context, material_config_bytes ) ) {
			m_context.recordResourceCreationFailure( "T3D.MaterialConfig", T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER );
			return false;
		}
	}
	if ( !m_hEditorOverlayBuffer.isValid() ) {
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.EditorOverlay";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_MATERIAL_TABLE;
		spec.m_creationBits = persistent_bits;
		if ( !m_hEditorOverlayBuffer.configure( spec ) || !m_hEditorOverlayBuffer.createTracked( m_context, editor_overlay_bytes ) ) {
			m_context.recordResourceCreationFailure( "T3D.EditorOverlay", T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER );
			return false;
		}
	}
	if ( !m_hPatchBuffer.isValid() ) {
		m_bPatchUploadInitialized = false;
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.Clipmap.Patches";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_PATCH;
		spec.m_creationBits = persistent_bits;
		if ( !m_hPatchBuffer.configure( spec ) || !m_hPatchBuffer.createTracked( m_context, patch_bytes ) ) {
			m_context.recordResourceCreationFailure( "T3D.Clipmap.Patches", T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER );
			return false;
		}
	}
	if ( !m_hVisibleBuffer.isValid() ) {
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.Clipmap.Visible";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		spec.m_type = T3D_RD_CONTEXT_RESOURCE_CULLING_BUFFER;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_SCRATCH;
		if ( !m_hVisibleBuffer.configure( spec ) || !m_hVisibleBuffer.createTracked( m_context, visible_bytes ) ) {
			m_context.recordResourceCreationFailure( "T3D.Clipmap.Visible", T3D_RD_CONTEXT_RESOURCE_CULLING_BUFFER );
			return false;
		}
	}
	if ( !m_hStripPartBuffer.isValid() ) {
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.Clipmap.StripParts";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
		if ( !m_hStripPartBuffer.configure( spec ) || !m_hStripPartBuffer.createTracked( m_context, strip_part_bytes, m_stripPartBytes ) ) {
			m_context.recordResourceCreationFailure( "T3D.Clipmap.StripParts", T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER );
			return false;
		}
	}
	if ( !m_pIndirectBuffer || !m_pIndirectBuffer->ownsSlice( m_indexedIndirectSlice )
	        || !m_pIndirectBuffer->ownsSlice( m_stripIndirectSlice ) ) {
		if ( m_pIndirectBuffer ) {
			m_context.releaseFixedIndirect( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP, m_pIndirectBuffer );
		}
		m_indexedIndirectSlice = t3dRdGpuBufferSlice_t{};
		m_stripIndirectSlice = t3dRdGpuBufferSlice_t{};
		uint32_t indirect_offset = 0u;
		if ( !m_context.acquireFixedIndirect( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP,
		             T3D_CLIPMAP_INDIRECT_ARENA_BYTES, m_pIndirectBuffer, indirect_offset )
		        || indirect_offset != t3d_rd_context_contract::fixedIndirectOffset( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP )
		        || !m_pIndirectBuffer->slice( indirect_offset, T3D_CLIPMAP_STRIP_INDIRECT_OFFSET_BYTES, 4u, m_indexedIndirectSlice )
		        || !m_pIndirectBuffer->slice( indirect_offset + T3D_CLIPMAP_STRIP_INDIRECT_OFFSET_BYTES, T3D_CLIPMAP_STRIP_INDIRECT_BYTES, 16u, m_stripIndirectSlice ) ) {
			m_context.releaseFixedIndirect( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP, m_pIndirectBuffer );
			m_indexedIndirectSlice = t3dRdGpuBufferSlice_t{};
			m_stripIndirectSlice = t3dRdGpuBufferSlice_t{};
			m_context.recordResourceCreationFailure( "T3D.Clipmap.IndirectArena", T3D_RD_CONTEXT_RESOURCE_INDIRECT_BUFFER );
			return false;
		}
	}
	m_frameUploadBytes.resize( frame_bytes );
	m_materialUploadBytes.resize( material_bytes );
	m_materialConfigUploadBytes.resize( material_config_bytes );
	m_editorOverlayUploadBytes.resize( editor_overlay_bytes );
	m_patchUploadBytes.resize( patch_bytes );
	m_cullPushBytes.resize( sizeof( t3dRdCullPushConstants_t ) );
	m_cpuVisibleBytes.resize( visible_bytes );
	m_cpuIndirectBytes.resize( indirect_bytes );
	m_cpuStripIndirectBytes.resize( strip_indirect_bytes );
	m_drawPushBytes.resize( sizeof( t3dRdDrawPushConstants_t ) );
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureRegionMapBuffer( const uint32_t nRequiredBytes ) {
	if ( nRequiredBytes == 0u ) {
		return false;
	}
	if ( !m_hRegionMapBuffer.needsCapacity( nRequiredBytes ) ) {
		return true;
	}
	// Descriptor ownership remains here: invalidate before the physical RID can change.
	freeUniformSets();
	t3dRdGpuBufferSpec_t spec;
	spec.m_pName = "T3D.RegionMap";
	spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
	spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
	spec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
	spec.m_creationBits = m_bPersistentCpuBuffers ? Terrain3DRdGpuBuffer::persistentCreationBits()
	                                               : BitField<RenderingDevice::BufferCreationBits>( 0 );
	if ( ( !m_hRegionMapBuffer.isConfigured() && !m_hRegionMapBuffer.configure( spec ) )
	        || !m_hRegionMapBuffer.replaceTracked( m_context, nRequiredBytes ) ) {
		m_context.recordResourceCreationFailure( "T3D.RegionMap", T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER );
		setError( "Failed to allocate RD region page-table buffer" );
		return false;
	}
	m_nRegionMapGeneration = 0u;
	m_bRegionMapUploadInitialized = false;
	return true;
}

void Terrain3DRdClipmapRenderer::freeUniformSets( void ) {
	m_descriptorCache.clear();
	for ( size_t iSet = 0u; iSet < T3D_ARRAY_COUNT( m_hszRasterUniformSets ); iSet++ ) {
		RID &rid = ( m_hszRasterUniformSets )[iSet];
		rid = RID();
	}
	m_hCullUniformSet = RID();
	m_boundDescriptorKey = t3dRdClipmapDescriptorKey_t{};
	m_bBoundDescriptorKeyValid = false;
}

Terrain3DRdClipmapRenderer::t3dRdClipmapDescriptorKey_t Terrain3DRdClipmapRenderer::buildUniformSetDescriptorKey(
        const t3dRdTerrainBindings_t &rBindings,
        const t3dRdMaterialConfig_t &rMaterialConfig,
        const t3dSharedVTBindings_t *const pSharedVTBindings ) const {
	t3dRdClipmapDescriptorKey_t key{};
	key.m_hBoundHeightTexture = rBindings.m_hHeightTextureRd;
	key.m_hBoundDerivedTexture = rBindings.m_hDerivedTextureRd;
	key.m_hBoundControlTexture = rBindings.m_hControlTextureRd;
	key.m_hBoundColorTexture = rBindings.m_hColorTextureRd;
	key.m_hBoundMiddleCompositeTexture = rBindings.m_hMiddleCompositeTextureRd;
	key.m_hBoundTerrainShadowMaskTexture = rBindings.m_hTerrainShadowMaskTextureRd;
	key.m_hBoundMiddleHeightTexture = rBindings.m_hMiddleHeightTextureRd;
	key.m_hBoundMiddleHeightRegionMapTexture = rBindings.m_hMiddleHeightRegionMapTextureRd;
	key.m_hBoundFarHeightTexture = rBindings.m_hFarHeightTextureRd;
	key.m_hBoundFarColorTexture = rBindings.m_hFarColorTextureRd;
	key.m_hBoundAlbedoTexture = rBindings.m_hAlbedoTextureRd;
	key.m_hBoundFarAlbedoTexture = rBindings.m_hFarAlbedoTextureRd;
	key.m_hBoundNormalTexture = rBindings.m_hNormalTextureRd;
	key.m_hBoundSurfaceTexture = rBindings.m_hSurfaceTextureRd;
	key.m_hBoundMaterialDisplacementTexture = rBindings.m_hMaterialDisplacementTextureRd;
	key.m_hBoundDisplacementTexture = rBindings.m_hDisplacementTextureRd;
	key.m_hBoundWorldNoiseTexture = rBindings.m_hWorldNoiseTextureRd;
	key.m_hBoundMacroNoiseTexture = rBindings.m_hMacroNoiseTextureRd;
	key.m_hBoundEditorBrushTexture = rBindings.m_hEditorBrushTextureRd;
	key.m_nBoundSharedVTResourceGeneration = pSharedVTBindings ? pSharedVTBindings->nResourceGeneration : 0u;
	for ( uint32_t i = 0u; i < 3u; ++i ) {
		key.m_nszBoundTierFiltering[i] = rMaterialConfig.m_szTier[i].m_nFiltering;
	}
	return key;
}

bool Terrain3DRdClipmapRenderer::hasCompatibleUniformSetDescriptorKey( const t3dRdClipmapDescriptorKey_t &rKey ) const {
	return m_bBoundDescriptorKeyValid && m_boundDescriptorKey == rKey;
}

bool Terrain3DRdClipmapRenderer::ensureUniformSets( const t3dRdTerrainBindings_t &rBindings,
        const t3dRdMaterialConfig_t &rMaterialConfig,
        const t3dSharedVTBindings_t *const pSharedVTBindings ) {
	const RID &rHeight = rBindings.m_hHeightTextureRd;
	const RID &rDerived = rBindings.m_hDerivedTextureRd;
	const RID &rControl = rBindings.m_hControlTextureRd;
	const RID &rColor = rBindings.m_hColorTextureRd;
	const RID &rMiddleComposite = rBindings.m_hMiddleCompositeTextureRd;
	const RID &rTerrainShadowMask = rBindings.m_hTerrainShadowMaskTextureRd;
	const RID &rMiddleHeight = rBindings.m_hMiddleHeightTextureRd;
	const RID &rMiddleHeightRegionMap = rBindings.m_hMiddleHeightRegionMapTextureRd;
	const RID &rFarHeight = rBindings.m_hFarHeightTextureRd;
	const RID &rFarColor = rBindings.m_hFarColorTextureRd;
	const RID &rAlbedo = rBindings.m_hAlbedoTextureRd;
	const RID &rFarAlbedo = rBindings.m_hFarAlbedoTextureRd;
	const RID &rNormal = rBindings.m_hNormalTextureRd;
	const RID &rSurface = rBindings.m_hSurfaceTextureRd;
	const RID &rMaterialDisplacement = rBindings.m_hMaterialDisplacementTextureRd;
	const RID &rDisplacement = rBindings.m_hDisplacementTextureRd;
	const RID &rWorldNoise = rBindings.m_hWorldNoiseTextureRd;
	const RID &rMacroNoise = rBindings.m_hMacroNoiseTextureRd;
	const RID &rEditorBrush = rBindings.m_hEditorBrushTextureRd;
	const t3dRdClipmapDescriptorKey_t descriptor_key = buildUniformSetDescriptorKey( rBindings, rMaterialConfig, pSharedVTBindings );
	const bool bUniformSetsReady =
	        m_hszRasterUniformSets[0].is_valid() && m_hszRasterUniformSets[1].is_valid() && m_hszRasterUniformSets[2].is_valid();
	const bool bCullUniformSetReady = !m_bGpuCullingReady.load( T3D_MEMORY_ORDER_ACQUIRE ) || m_hCullUniformSet.is_valid();
	if ( bUniformSetsReady && bCullUniformSetReady && hasCompatibleUniformSetDescriptorKey( descriptor_key ) ) {
		return true;
	}
	freeUniformSets();
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !m_hRegionMapBuffer.is_valid() ) {
		return false;
	}
	const RenderingDevice::UniformType frame_uniform_type = m_bPersistentFrameUniformBuffer
	        ? RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC
	        : RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER;
	const RenderingDevice::UniformType cpu_storage_type =
	        m_bPersistentCpuBuffers ? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC : RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
	for ( int i = 0; i < 3; ++i ) {
		const uint32_t nFiltering = MIN( rMaterialConfig.m_szTier[i].m_nFiltering, 3u );
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 0, frame_uniform_type, m_hFrameUniformBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 1, cpu_storage_type, m_hRegionMapBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 2, m_hTerrainSampler.rid(), rHeight ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 3, m_hTerrainSampler.rid(), rControl ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 4, m_hszMapSamplers[nFiltering].rid(), rColor ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 5, m_hszMaterialSamplers[nFiltering].rid(), rAlbedo ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 6, m_hszMaterialSamplers[nFiltering].rid(), rNormal ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 7, m_hszMaterialSamplers[nFiltering].rid(), rSurface ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 8, cpu_storage_type, m_hMaterialBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 9, cpu_storage_type, m_hPatchBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 10, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hVisibleBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 11, m_hszMaterialSamplers[nFiltering].rid(), rMaterialDisplacement ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 12, m_hTerrainSampler.rid(), rDisplacement ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 13, cpu_storage_type, m_hMaterialConfigBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 14, m_hszMapSamplers[nFiltering].rid(), rDerived ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 15, m_hszMapSamplers[3].rid(), rMiddleComposite ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 16, m_hTerrainSampler.rid(), rMiddleHeight ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 17, m_hTerrainSampler.rid(), rMiddleHeightRegionMap ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 18, m_hTerrainSampler.rid(), rFarHeight ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 19, m_hTerrainSampler.rid(), rFarColor ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 20, m_hszMaterialSamplers[nFiltering].rid(), rFarAlbedo ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 21, m_hszMaterialSamplers[1].rid(), rWorldNoise ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 22, m_hszMaterialSamplers[nFiltering].rid(), rMacroNoise ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 23, cpu_storage_type, m_hEditorOverlayBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 24, m_hszMapSamplers[1].rid(), rEditorBrush ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 25, m_hszMapSamplers[1].rid(), rTerrainShadowMask ) );
		const RID hVTAllocation = pSharedVTBindings && pSharedVTBindings->hAllocationTable.is_valid() ? pSharedVTBindings->hAllocationTable
		                                                                                              : m_hMaterialBuffer.rid();
		const RID hVTPageTable =
		        pSharedVTBindings && pSharedVTBindings->hPageTable.is_valid() ? pSharedVTBindings->hPageTable : m_hRegionMapBuffer.rid();
		uniforms.push_back(
		        Terrain3DRdBindingBuilder::single( T3D_TERRAIN_VT_ALLOCATION_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, hVTAllocation ) );
		uniforms.push_back(
		        Terrain3DRdBindingBuilder::single( T3D_TERRAIN_VT_PAGE_TABLE_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, hVTPageTable ) );
		for ( uint32_t channel = 0u; channel < T3D_SHARED_VT_CHANNEL_COUNT; ++channel ) {
			RID hPhysical = m_hFallbackArrayTexture.rid();
			if ( pSharedVTBindings ) {
				if ( pSharedVTBindings->nBankCount[channel] == 0u ) {
					setError( String( "Terrain VT physical bank is unavailable for active channel " ) + String::num_uint64( channel ) );
					return false;
				}
				if ( pSharedVTBindings->nBankCount[channel] > 1u ) {
					setError( String( "Terrain VT single-bank contract violated for channel " ) + String::num_uint64( channel ) );
					return false;
				}
				if ( pSharedVTBindings->hPhysical[channel][0].is_valid() ) {
					hPhysical = pSharedVTBindings->hPhysical[channel][0];
				}
			}
			const uint32_t nBinding = T3D_TERRAIN_VT_PHYSICAL_BINDING_BASE + channel;
			uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( ( int )( nBinding ), m_hszVtSamplers[nFiltering].rid(), hPhysical ) );
		}
		const RID hVTTailMetadata =
		        pSharedVTBindings && pSharedVTBindings->hTailMetadata.is_valid() ? pSharedVTBindings->hTailMetadata : m_hMaterialBuffer.rid();
		const RID hVTMipLayout =
		        pSharedVTBindings && pSharedVTBindings->hMipLayoutTable.is_valid() ? pSharedVTBindings->hMipLayoutTable : m_hMaterialBuffer.rid();
		uniforms.push_back(
		        Terrain3DRdBindingBuilder::single( T3D_TERRAIN_VT_TAIL_METADATA_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, hVTTailMetadata ) );
		for ( uint32_t channel = 0u; channel < T3D_SHARED_VT_CHANNEL_COUNT; ++channel ) {
			const RID hTail = pSharedVTBindings && pSharedVTBindings->hTail[channel].is_valid() ? pSharedVTBindings->hTail[channel]
			                                                                                    : m_hFallbackDisplacementTexture.rid();
			uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture(
			        ( int )( T3D_TERRAIN_VT_TAIL_TEXTURE_BINDING_BASE + channel ), m_hszVtSamplers[nFiltering].rid(), hTail ) );
		}
		uniforms.push_back(
		        Terrain3DRdBindingBuilder::single( T3D_TERRAIN_VT_MIP_LAYOUT_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, hVTMipLayout ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 38, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hStripPartBuffer ) );
		const String name = String( "T3D.UniformSet.Raster." ) + String::num_int64( i );
		m_hszRasterUniformSets[i] = m_descriptorCache.getOrCreate(
		        { m_hszShaders[i].rid(), descriptor_key }, name, uniforms, m_hszShaders[i].rid(), 0u );
		if ( !m_hszRasterUniformSets[i].is_valid() ) {
			m_context.recordResourceCreationFailure( name, T3D_RD_CONTEXT_RESOURCE_UNIFORM_SET );
			setError( "Failed to create Terrain3D raster uniform set" );
			return false;
		}
	}
	if ( m_bGpuCullingReady.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		TypedArray<Ref<RDUniform>> cull_uniforms;
		cull_uniforms.push_back( Terrain3DRdBindingBuilder::single( 0, frame_uniform_type, m_hFrameUniformBuffer ) );
		cull_uniforms.push_back( Terrain3DRdBindingBuilder::single( 9, cpu_storage_type, m_hPatchBuffer ) );
		cull_uniforms.push_back( Terrain3DRdBindingBuilder::single( 10, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hVisibleBuffer ) );
		cull_uniforms.push_back( Terrain3DRdBindingBuilder::single( 11, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_pIndirectBuffer->rid() ) );
		cull_uniforms.push_back( Terrain3DRdBindingBuilder::single( 12, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_pIndirectBuffer->rid() ) );
		m_hCullUniformSet = m_descriptorCache.getOrCreate(
		        { m_hCullShader.rid(), descriptor_key }, "T3D.UniformSet.Cull", cull_uniforms, m_hCullShader.rid(), 0u );
		if ( !m_hCullUniformSet.is_valid() ) {
			m_context.recordResourceCreationFailure( "T3D.UniformSet.Cull", T3D_RD_CONTEXT_RESOURCE_UNIFORM_SET );
			setError( "Failed to create Terrain3D culling uniform set" );
			return false;
		}
	}
	m_boundDescriptorKey = descriptor_key;
	m_bBoundDescriptorKeyValid = true;
	return true;
}

void Terrain3DRdClipmapRenderer::freeViewState( t3dRdViewState_t &rState ) {
	for ( size_t iPipeline = 0u; iPipeline < T3D_ARRAY_COUNT( rState.m_hszPipelines ); iPipeline++ ) {
		RID &pipeline = ( rState.m_hszPipelines )[iPipeline];
		m_viewPipelines.erase( pipeline );
		pipeline = RID();
	}
	rState.m_hFramebuffer.releaseTracked( m_context );
	rState.m_hColor = RID();
	rState.m_hDepth = RID();
	rState.m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
	rState.m_bColorWrite = true;
}

bool Terrain3DRdClipmapRenderer::ensureViewState( const uint32_t nView,
        const RID &rColor,
        const RID &rDepth,
        const RenderingDevice::TextureSamples samples,
        const bool bColorWrite ) {
	if ( nView >= T3D_MAX_VIEWS ) {
		return false;
	}
	t3dRdViewState_t &state = m_szViewStates[nView];
	if ( state.m_hFramebuffer.isValid() && state.m_hColor == rColor && state.m_hDepth == rDepth && state.m_samples == samples
	        && state.m_bColorWrite == bColorWrite ) {
		return true;
	}
	freeViewState( state );
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	TypedArray<RID> attachments;
	attachments.push_back( rColor );
	attachments.push_back( rDepth );
	const String framebuffer_name = String( "T3D.Framebuffer.View" ) + String::num_int64( nView );
	if ( !state.m_hFramebuffer.createTracked( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN, framebuffer_name, attachments ) ) {
		setError( "Failed to create Terrain3D per-view framebuffer" );
		return false;
	}
	t3dRdForwardPipelineBaseKey_t pipeline_key;
	pipeline_key.m_samples = samples;
	t3dRdForwardPipelineStateOptions_t options;
	options.m_depthCompare = RenderingDevice::COMPARE_OP_LESS_OR_EQUAL;
	options.m_nBlendAttachmentCount = 1u;
	options.m_bColorWrite = bColorWrite;
	t3dRdForwardPipelineStates_t pipeline_states;
	if ( !t3dRdBuildForwardPipelineStates( pipeline_key, options, pipeline_states ) ) {
		freeViewState( state );
		setError( "Failed to create Terrain3D per-view pipeline states" );
		return false;
	}
	const int64_t framebuffer_format = state.m_hFramebuffer.format( rd );
	for ( int i = 0; i < 3; ++i ) {
		const String pipeline_name = String( "T3D.Pipeline.View" ) + String::num_int64( nView ) + "." + String::num_int64( i );
		state.m_hszPipelines[ i ] = m_viewPipelines.getOrCreateRaster( nView * 3u + (uint32_t)i,
		                                                               pipeline_name, m_hszShaders[ i ].rid(),
		                                                               framebuffer_format,
		                                                               -1,
		                                                               RenderingDevice::RENDER_PRIMITIVE_TRIANGLE_STRIPS,
		                                                               pipeline_states.m_raster,
		                                                               pipeline_states.m_multisample,
		                                                               pipeline_states.m_depth,
		                                                               pipeline_states.m_blend );
		if ( !state.m_hszPipelines[i].is_valid() ) {
			m_context.recordResourceCreationFailure( pipeline_name, T3D_RD_CONTEXT_RESOURCE_RASTER_PIPELINE );
			setError( "Failed to create Terrain3D raster pipeline" );
			freeViewState( state );
			return false;
		}
	}
	state.m_hColor = rColor;
	state.m_hDepth = rDepth;
	state.m_samples = samples;
	state.m_bColorWrite = bColorWrite;
	return true;
}

bool Terrain3DRdClipmapRenderer::initialize( void ) {
	const Ref<RDShaderSPIRV> rNear = Terrain3DRdShaderLibrary::borrowSpirv( T3D_RD_SHADER_NEAR_ALIAS );
	const Ref<RDShaderSPIRV> rMiddle = Terrain3DRdShaderLibrary::borrowSpirv( T3D_RD_SHADER_MIDDLE_ALIAS );
	const Ref<RDShaderSPIRV> rFar = Terrain3DRdShaderLibrary::borrowSpirv( T3D_RD_SHADER_FAR_ALIAS );
	const Ref<RDShaderSPIRV> rCull = Terrain3DRdShaderLibrary::borrowSpirv( T3D_RD_SHADER_TERRAIN_CULL );
	Terrain3DScopedLock state_lock( m_resourceStateMutex );
	if ( isReady() ) {
		return true;
	}

	// Persistent mappings are only enabled when both sides of the contract exist:
	// the RenderingDevice raw-map/flush methods and dynamic-buffer shader metadata.
	// This keeps stock Godot builds on the staging path rather than attempting an
	// unsafe partial implementation.
	if ( !ensureDevice() ) {
		return false;
	}
	RenderingDevice *const persistent_rd = m_context.getDevice();
	m_bPersistentCpuBuffers = Terrain3DRdGpuBuffer::supportsPersistentMapping( persistent_rd ) && t3dRdSupportsDynamicBufferMetadata( rNear )
	        && t3dRdSupportsDynamicBufferMetadata( rMiddle ) && t3dRdSupportsDynamicBufferMetadata( rFar )
	        && ( rCull.is_null() || t3dRdSupportsDynamicBufferMetadata( rCull ) );
	if ( m_bPersistentCpuBuffers ) {
		for ( uint32_t path = 0u; path < T3D_TERRAIN_SHADER_PATH_COUNT && m_bPersistentCpuBuffers; ++path ) {
			for ( uint32_t tier = 0u; tier < 3u && m_bPersistentCpuBuffers; ++tier ) {
				for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
					if ( !t3dRdSupportsDynamicBufferMetadata( Terrain3DRdShaderLibrary::borrowSpirv( t3dRdTerrainForwardId( path, tier, variant ) ) ) ) {
						m_bPersistentCpuBuffers = false;
						break;
					}
				}
			}
		}
	}
	m_bPersistentFrameUniformBuffer = m_bPersistentCpuBuffers;
	m_forwardLighting.setPersistentFrameBufferEnabled( m_bPersistentCpuBuffers );
	if ( m_bPersistentCpuBuffers ) {
		PackedInt64Array raster_dynamic_buffers;
		raster_dynamic_buffers.push_back( 0 );                          // set 0, binding 0: frame UBO
		raster_dynamic_buffers.push_back( 1 );                          // set 0, binding 1: region page table
		raster_dynamic_buffers.push_back( 8 );                          // set 0, binding 8: material table
		raster_dynamic_buffers.push_back( 9 );                          // set 0, binding 9: clipmap patches
		raster_dynamic_buffers.push_back( 13 );                         // set 0, binding 13: material config
		raster_dynamic_buffers.push_back( 23 );                         // set 0, binding 23: editor overlay
		raster_dynamic_buffers.push_back( ( int64_t )( 2ULL << 32u ) ); // set 2, binding 0: Forward+ frame UBO
		t3dRdMarkDynamicBuffers( rNear, raster_dynamic_buffers );
		t3dRdMarkDynamicBuffers( rMiddle, raster_dynamic_buffers );
		t3dRdMarkDynamicBuffers( rFar, raster_dynamic_buffers );

		if ( rCull.is_valid() ) {
			PackedInt64Array cull_dynamic_buffers;
			cull_dynamic_buffers.push_back( 0 );
			cull_dynamic_buffers.push_back( 9 );
			t3dRdMarkDynamicBuffers( rCull, cull_dynamic_buffers );
		}
		for ( uint32_t path = 0u; path < T3D_TERRAIN_SHADER_PATH_COUNT; ++path ) {
			for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
				for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
					t3dRdMarkDynamicBuffers( Terrain3DRdShaderLibrary::borrowSpirv( t3dRdTerrainForwardId( path, tier, variant ) ), raster_dynamic_buffers );
				}
			}
		}
	}

	if ( !ensureShaders() || !ensureSamplers() || !ensureFallbackTextures() || !ensureBuffers() ) {
		return false;
	}
	return true;
}

void Terrain3DRdClipmapRenderer::shutdown( void ) {
	Terrain3DScopedLock state_lock( m_resourceStateMutex );
	for ( size_t iView = 0u; iView < T3D_ARRAY_COUNT( m_szViewStates ); iView++ ) {
		t3dRdViewState_t &state = ( m_szViewStates )[iView];
		freeViewState( state );
	}
	m_forwardLighting.shutdown();
	freeForwardPipelines();
	freeUniformSets();
	m_hStripPartBuffer.releaseTracked( m_context );
	m_context.releaseFixedIndirect( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP, m_pIndirectBuffer );
	m_hVisibleBuffer.releaseTracked( m_context );
	m_hPatchBuffer.releaseTracked( m_context );
	m_hEditorOverlayBuffer.releaseTracked( m_context );
	m_hMaterialConfigBuffer.releaseTracked( m_context );
	m_hMaterialBuffer.releaseTracked( m_context );
	m_hRegionMapBuffer.releaseTracked( m_context );
	m_hFrameUniformBuffer.releaseTracked( m_context );
	m_hFallbackDisplacementTexture.releaseTracked( m_context );
	m_hFallbackArrayTexture.releaseTracked( m_context );
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		m_hszMaterialSamplers[i].releaseTracked( m_context );
		m_hszMapSamplers[i].releaseTracked( m_context );
		m_hszVtSamplers[i].releaseTracked( m_context );
	}
	m_hTerrainSampler.releaseTracked( m_context );
	m_cullPipelines.clear();
	m_hCullShader.releaseTracked( m_context );
	for ( uint32_t path = 0u; path < T3D_TERRAIN_SHADER_PATH_COUNT; ++path ) {
		for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
			for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
				m_hszForwardShaders[path][tier][variant].releaseTracked( m_context );
			}
			for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT; ++variant ) {
				m_hszForwardOneSunShaders[path][tier][variant].releaseTracked( m_context );
			}
		}
	}
	for ( size_t iShader = 0u; iShader < T3D_ARRAY_COUNT( m_hszShaders ); iShader++ ) {
		m_hszShaders[iShader].releaseTracked( m_context );
	}
	m_indexedIndirectSlice = {};
	m_stripIndirectSlice = {};
	m_hFallbackDisplacementTexture.abandon();
	m_hFallbackArrayTexture.abandon();
	m_hCullPipeline = RID();
	m_nForwardPreparedScenePass = 0u;
	m_nForwardPreparedViewportKey = 0u;
	m_bForwardHistoryValid = false;
	m_nForwardPreparedSnapshotGeneration = 0u;
	m_forwardPreparedCullPassKind = T3D_RD_FORWARD_PASS_INVALID;
	m_bLastForwardPipelineKeyValid = false;
	m_nRegionMapGeneration = 0u;
	m_nMaterialGeneration = 0u;
	m_nPatchGeneration = 0u;
	m_bRegionMapUploadInitialized = false;
	m_bMaterialUploadInitialized = false;
	m_nUploadedPatchCount = 0u;
	m_bPatchUploadInitialized = false;
	m_bPersistentFrameUniformBuffer = false;
	m_bPersistentCpuBuffers = false;
	m_forwardLighting.setPersistentFrameBufferEnabled( false );
	m_bEditorOverlayUploadInitialized = false;
	m_uploadedEditorOverlay = t3dRdEditorOverlayConfig_t{};
	memset( m_szUploadedPatches, 0, sizeof( m_szUploadedPatches ) );
	m_bGpuCullingReady = false;
	m_frameUploadBytes.clear();
	m_materialUploadBytes.clear();
	m_materialConfigUploadBytes.clear();
	m_editorOverlayUploadBytes.clear();
	m_patchUploadBytes.clear();
	m_cullPushBytes.clear();
	m_cpuVisibleBytes.clear();
	m_cpuIndirectBytes.clear();
	m_cpuStripIndirectBytes.clear();
	m_drawPushBytes.clear();
}

void Terrain3DRdClipmapRenderer::copyProjection( const Projection &rProjection, float *const pDestination ) const {
	for ( int column = 0; column < 4; ++column ) {
		const Vector4 value = rProjection[column];
		pDestination[column * 4 + 0] = ( float )( value.x );
		pDestination[column * 4 + 1] = ( float )( value.y );
		pDestination[column * 4 + 2] = ( float )( value.z );
		pDestination[column * 4 + 3] = ( float )( value.w );
	}
}

bool Terrain3DRdClipmapRenderer::updateFrameUniforms( const t3dRdFrameSnapshot_t &rSnapshot,
        const Projection *const pViewProjections,
        const uint32_t nViewCount,
        const Projection &rView,
        const Vector3 *const pCameras,
        const Projection *const pPreviousViewProjections,
        const Vector2 &rTaaJitter,
        const Vector2 &rPreviousTaaJitter,
        const bool bHistoryValid,
        const t3dSharedVTBindings_t *const pSharedVTBindings ) {
	if ( !pViewProjections || !pCameras || nViewCount == 0u || nViewCount > T3D_RD_FORWARD_MAX_VIEWS ) {
		return false;
	}
	t3dRdFrameUniforms_t frame;
	t3dRdFrameUniformsInit( &frame );
	for ( uint32_t view = 0u; view < T3D_RD_FORWARD_MAX_VIEWS; ++view ) {
		const uint32_t source = MIN( view, nViewCount - 1u );
		copyProjection( pViewProjections[source], frame.m_mViewProjection[view] );
		const Projection &previous =
		        bHistoryValid && pPreviousViewProjections ? pPreviousViewProjections[source] : pViewProjections[source];
		copyProjection( previous, frame.m_mPreviousViewProjection[view] );
		frame.m_szCamera[view][0] = ( float )( pCameras[source].x );
		frame.m_szCamera[view][1] = ( float )( pCameras[source].y );
		frame.m_szCamera[view][2] = ( float )( pCameras[source].z );
	}
	copyProjection( rView, frame.m_mView );
	frame.m_szMotionJitter[0] = ( float )( rTaaJitter.x );
	frame.m_szMotionJitter[1] = ( float )( rTaaJitter.y );
	frame.m_szMotionJitter[2] = ( float )( bHistoryValid ? rPreviousTaaJitter.x : rTaaJitter.x );
	frame.m_szMotionJitter[3] = ( float )( bHistoryValid ? rPreviousTaaJitter.y : rTaaJitter.y );
	frame.m_szMotionParams[0] = bHistoryValid ? 1.0f : 0.0f;
	frame.m_szMotionParams[1] = m_bMotionDebugEnabled.load( T3D_MEMORY_ORDER_ACQUIRE ) ? 1.0f : 0.0f;
	frame.m_fLightDirectionX = ( float )( rSnapshot.m_lightDirection.x );
	frame.m_fLightDirectionY = ( float )( rSnapshot.m_lightDirection.y );
	frame.m_fLightDirectionZ = ( float )( rSnapshot.m_lightDirection.z );
	frame.m_fAmbient = rSnapshot.m_fAmbient;
	frame.m_fLightColorR = ( float )( rSnapshot.m_lightColor.r );
	frame.m_fLightColorG = ( float )( rSnapshot.m_lightColor.g );
	frame.m_fLightColorB = ( float )( rSnapshot.m_lightColor.b );
	frame.m_fLightColorA = ( float )( rSnapshot.m_lightColor.a );
	frame.m_fTerrainPadding0 = pSharedVTBindings ? ( float )( pSharedVTBindings->nAllocationCount ) : 0.0f;
	frame.m_fTerrainPadding1 = pSharedVTBindings ? ( float )( pSharedVTBindings->nVirtualPageCount ) : 0.0f;
	memcpy( frame.m_szInstanceTextureMipBias, rSnapshot.m_szInstanceTextureMipBias, sizeof( frame.m_szInstanceTextureMipBias ) );
	frame.m_fHeightMin = rSnapshot.m_fHeightMin;
	frame.m_fHeightRange = rSnapshot.m_fHeightRange;
	frame.m_fVertexSpacing = rSnapshot.m_fVertexSpacing;
	frame.m_fMeshSize = ( float )( rSnapshot.m_nMeshSize );
	// Experimental hot-path constants packed into previously unused std140 lanes.
	// Keep the ABI unchanged while moving invariant divides/log work off the GPU.
	frame.m_fDebugBlend = 1.0f / MAX( frame.m_fMeshSize - 4.0f, 1.0f );
	frame.m_fMaterialLayerCount = ( float )( MAX( rSnapshot.m_nMaterialLayerCount, 1u ) );
	frame.m_fTargetX = ( float )( rSnapshot.m_targetPosition.x );
	frame.m_fTargetZ = ( float )( rSnapshot.m_targetPosition.z );
	frame.m_fSubdiv = MAX( rSnapshot.m_fSubdiv, 1.0f );
	frame.m_fTargetPadding = 1.0f / MAX( frame.m_fVertexSpacing, 1e-6f );
	frame.m_fDisplacementPadding0 = 1.0f / MAX( frame.m_fMeshSize * 2.0f, 1.0f );
	frame.m_fDisplacementPadding1 = 1.0f / MAX( log2f( frame.m_fSubdiv ), 1.0f );
	frame.m_fFogR = ( float )( rSnapshot.m_fogColor.r );
	frame.m_fFogG = ( float )( rSnapshot.m_fogColor.g );
	frame.m_fFogB = ( float )( rSnapshot.m_fogColor.b );
	frame.m_fFogDensity = rSnapshot.m_fFogDensity;
	frame.m_fFogStart = rSnapshot.m_fFogStart;
	frame.m_fFogEnd = rSnapshot.m_fFogEnd;
	frame.m_fFogCurve = rSnapshot.m_fFogCurve;
	frame.m_fFogMode = ( float )( rSnapshot.m_nFogMode );
	frame.m_fDisplacementScale = rSnapshot.m_fDisplacementScale;
	frame.m_fDisplacementEnabled = rSnapshot.m_bDisplacementEnabled ? 1.0f : 0.0f;
	if ( !m_hFrameUniformBuffer.writeTracked( m_context,
	             0u,
	             &frame,
	             ( uint32_t )sizeof( frame ),
	             m_frameUploadBytes,
	             T3D_GPU_HOT_FRAME ) ) {
		return false;
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::uploadSnapshotBuffers(
        const t3dRdFrameSnapshot_t &rSnapshot, const PackedByteArray &rRegionMapBytes, const uint64_t nRegionMapGeneration ) {
	if ( !ensureRegionMapBuffer( ( uint32_t )( rRegionMapBytes.size() ) ) ) {
		return false;
	}
	if ( !m_bRegionMapUploadInitialized || nRegionMapGeneration != m_nRegionMapGeneration ) {
		if ( !m_hRegionMapBuffer.writePackedTracked( m_context,
		             0u,
		             rRegionMapBytes,
		             ( uint32_t )rRegionMapBytes.size(),
		             T3D_GPU_DIRTY_UPLOAD ) ) {
			return false;
		}
		m_nRegionMapGeneration = nRegionMapGeneration;
		m_bRegionMapUploadInitialized = true;
	}
	if ( !m_bMaterialUploadInitialized || rSnapshot.m_nMaterialGeneration != m_nMaterialGeneration ) {
		Terrain3DRdGpuBufferBatch uploads( m_context );
		if ( !uploads.queueFullWrite( m_hMaterialBuffer,
		                rSnapshot.m_szMaterials,
		                ( uint32_t )sizeof( rSnapshot.m_szMaterials ),
		                m_materialUploadBytes,
		                T3D_GPU_PUBLICATION )
		        || !uploads.queueFullWrite( m_hMaterialConfigBuffer,
		                &rSnapshot.m_materialConfig,
		                ( uint32_t )sizeof( rSnapshot.m_materialConfig ),
		                m_materialConfigUploadBytes,
		                T3D_GPU_PUBLICATION )
		        || !uploads.flush() ) {
			return false;
		}
		m_nMaterialGeneration = rSnapshot.m_nMaterialGeneration;
		m_bMaterialUploadInitialized = true;
	}
	if ( !m_bEditorOverlayUploadInitialized
	        || memcmp( &m_uploadedEditorOverlay, &rSnapshot.m_editorOverlayConfig, sizeof( rSnapshot.m_editorOverlayConfig ) ) != 0 ) {
		const bool bUploaded = m_hEditorOverlayBuffer.writeTracked( m_context,
		        0u,
		        &rSnapshot.m_editorOverlayConfig,
		        ( uint32_t )sizeof( rSnapshot.m_editorOverlayConfig ),
		        m_editorOverlayUploadBytes,
		        T3D_GPU_DIRTY_UPLOAD );
		if ( !bUploaded ) {
			return false;
		}
		++m_nEditorOverlayUploads;
		m_uploadedEditorOverlay = rSnapshot.m_editorOverlayConfig;
		m_bEditorOverlayUploadInitialized = true;
	}
	if ( !m_bPatchUploadInitialized || rSnapshot.m_nGeneration != m_nPatchGeneration || rSnapshot.m_nPatchCount != m_nUploadedPatchCount ) {
		if ( rSnapshot.m_nPatchCount > T3D_MAX_CLIPMAP_LEVELS ) {
			return false;
		}
		if ( m_bPersistentCpuBuffers ) {
			const uint32_t nPatchBytes = rSnapshot.m_nPatchCount * sizeof( t3dRdPatch_t );
			if ( nPatchBytes > 0u
			        && !m_hPatchBuffer.writeTracked( m_context,
			                0u,
			                rSnapshot.m_szPatches,
			                nPatchBytes,
			                m_patchUploadBytes,
			                T3D_GPU_DIRTY_UPLOAD ) ) {
				return false;
			}
			if ( nPatchBytes > 0u ) {
				memcpy( m_szUploadedPatches, rSnapshot.m_szPatches, nPatchBytes );
				++m_nPatchDirtyRanges;
			}
		} else {
			uint32_t first_dirty = 0u;
			while ( first_dirty < rSnapshot.m_nPatchCount ) {
				while ( first_dirty < rSnapshot.m_nPatchCount && m_bPatchUploadInitialized && first_dirty < m_nUploadedPatchCount
				        && memcmp( &m_szUploadedPatches[first_dirty], &rSnapshot.m_szPatches[first_dirty], sizeof( t3dRdPatch_t ) ) == 0 ) {
					++first_dirty;
				}
				if ( first_dirty >= rSnapshot.m_nPatchCount ) {
					break;
				}
				uint32_t end_dirty = first_dirty + 1u;
				while ( end_dirty < rSnapshot.m_nPatchCount
				        && ( !m_bPatchUploadInitialized || end_dirty >= m_nUploadedPatchCount
				                || memcmp( &m_szUploadedPatches[end_dirty], &rSnapshot.m_szPatches[end_dirty], sizeof( t3dRdPatch_t ) )
				                        != 0 ) ) {
					++end_dirty;
				}
				const uint32_t dirty_count = end_dirty - first_dirty;
				const uint32_t dirty_bytes = dirty_count * sizeof( t3dRdPatch_t );
				if ( !m_hPatchBuffer.writeTracked( m_context,
				             first_dirty * sizeof( t3dRdPatch_t ),
				             &rSnapshot.m_szPatches[first_dirty],
				             dirty_bytes,
				             m_patchUploadBytes,
				             T3D_GPU_DIRTY_UPLOAD ) ) {
					return false;
				}
				memcpy( &m_szUploadedPatches[first_dirty], &rSnapshot.m_szPatches[first_dirty], dirty_bytes );
				++m_nPatchDirtyRanges;
				first_dirty = end_dirty;
			}
		}
		m_nUploadedPatchCount = rSnapshot.m_nPatchCount;
		m_bPatchUploadInitialized = true;
		m_nPatchGeneration = rSnapshot.m_nGeneration;
		++m_nPatchUploads;
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::runGpuCulling( const t3dRdFrameSnapshot_t &rSnapshot ) {
	if ( !m_bGpuCullingReady.load( T3D_MEMORY_ORDER_ACQUIRE ) || !m_hCullUniformSet.is_valid() ) {
		return false;
	}
	t3dRdIndexedIndirectCommand_t *const commands = ( t3dRdIndexedIndirectCommand_t * )( m_cpuIndirectBytes.ptrw() );
	memset( commands, 0, ( size_t )( m_cpuIndirectBytes.size() ) );
	for ( uint32_t group = 0u; group < T3D_RD_DRAW_GROUP_COUNT; ++group ) {
		commands[group].m_nIndexCount = getStripPartCountForGroup( group );
		commands[group].m_nFirstInstance = group * T3D_MAX_CLIPMAP_LEVELS;
	}
	t3dRdCpuCullResult_t empty_result;
	t3dRdCpuCullResultReset( &empty_result );
	for ( uint32_t group = 0u; group < T3D_RD_DRAW_GROUP_COUNT; ++group ) {
		empty_result.m_szCommands[group].m_nIndexCount = getStripPartCountForGroup( group );
		empty_result.m_szCommands[group].m_nFirstInstance = group * T3D_MAX_CLIPMAP_LEVELS;
	}
	if ( !buildStripIndirectCommands( empty_result ) ) {
		return false;
	}
	t3dRdCullPushConstants_t constants;
	t3dRdCullPushConstantsInit( &constants );
	constants.m_nPatchCount = MIN( rSnapshot.m_nPatchCount, ( uint32_t )( T3D_MAX_CLIPMAP_LEVELS ) );
	constants.m_nLod0PartCount = m_szStripPartCounts[LOD0_MESH];
	constants.m_nOuterPartCount = m_szStripPartCounts[OUTER_RING_MESH];
	constants.m_nMaxPatches = T3D_MAX_CLIPMAP_LEVELS;
	constants.m_nStripCommandBase = m_stripIndirectSlice.m_nOffsetBytes / sizeof( t3dRdDrawIndirectCommand_t );
	constants.m_nIndexedCommandBase = m_indexedIndirectSlice.m_nOffsetBytes / sizeof( t3dRdIndexedIndirectCommand_t );
	memcpy( m_cullPushBytes.ptrw(), &constants, sizeof( constants ) );
	RenderingDevice *const rd = m_context.getDevice();
	if ( !m_pIndirectBuffer->writePackedSliceTracked( m_context, m_indexedIndirectSlice, 0u, m_cpuIndirectBytes,
	             T3D_CLIPMAP_STRIP_INDIRECT_OFFSET_BYTES, T3D_GPU_INDIRECT, T3D_RD_CONTEXT_OWNER_TERRAIN )
	        || !m_pIndirectBuffer->writePackedSliceTracked( m_context, m_stripIndirectSlice, 0u, m_cpuStripIndirectBytes,
	                T3D_CLIPMAP_STRIP_INDIRECT_BYTES, T3D_GPU_INDIRECT, T3D_RD_CONTEXT_OWNER_TERRAIN ) ) {
		return false;
	}
	const int64_t compute_list = rd->compute_list_begin();
	if ( compute_list == RenderingDevice::INVALID_ID ) {
		return false;
	}
	rd->compute_list_bind_compute_pipeline( compute_list, m_hCullPipeline );
	rd->compute_list_bind_uniform_set( compute_list, m_hCullUniformSet, 0u );
	rd->compute_list_set_push_constant( compute_list, m_cullPushBytes, sizeof( constants ) );
	const uint32_t workgroups = MAX( 1u, ( constants.m_nPatchCount + 31u ) / 32u );
	rd->compute_list_dispatch( compute_list, workgroups, 1u, 1u );
	rd->compute_list_add_barrier( compute_list );
	rd->compute_list_end();
	++m_nCullDispatches;
	return true;
}

bool Terrain3DRdClipmapRenderer::uploadCpuIndirect( const t3dRdCpuCullResult_t &rResult ) {
	memcpy( m_cpuVisibleBytes.ptrw(), rResult.m_szVisibleIndices, sizeof( rResult.m_szVisibleIndices ) );
	memcpy( m_cpuIndirectBytes.ptrw(), rResult.m_szCommands, sizeof( rResult.m_szCommands ) );
	if ( !buildStripIndirectCommands( rResult ) ) {
		return false;
	}
	if ( !m_hVisibleBuffer.writePackedTracked( m_context,
	             0u,
	             m_cpuVisibleBytes,
	             ( uint32_t )m_cpuVisibleBytes.size(),
	             T3D_GPU_INDIRECT ) ) {
		return false;
	}
	if ( !m_pIndirectBuffer->writePackedSliceTracked( m_context, m_indexedIndirectSlice, 0u, m_cpuIndirectBytes,
	             T3D_CLIPMAP_STRIP_INDIRECT_OFFSET_BYTES, T3D_GPU_INDIRECT, T3D_RD_CONTEXT_OWNER_TERRAIN )
	        || !m_pIndirectBuffer->writePackedSliceTracked( m_context, m_stripIndirectSlice, 0u, m_cpuStripIndirectBytes,
	                T3D_CLIPMAP_STRIP_INDIRECT_BYTES, T3D_GPU_INDIRECT, T3D_RD_CONTEXT_OWNER_TERRAIN ) ) {
		return false;
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::compareGpuCpuCulling(
        const t3dRdFrameSnapshot_t &rSnapshot, const Projection &rViewProjection, const t3dRdCpuCullResult_t &rExpected ) {
	const uint64_t comparison_index = m_nCullComparisons.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
	const PackedByteArray gpu_visible = m_hVisibleBuffer.readbackTracked( m_context,
	        0u, ( uint32_t )( sizeof( rExpected.m_szVisibleIndices ) ) );
	const PackedByteArray gpu_indirect = m_pIndirectBuffer->readbackSliceTracked( m_context,
	        m_indexedIndirectSlice, 0u, ( uint32_t )( sizeof( rExpected.m_szCommands ) ) );
	if ( gpu_visible.size() != ( int64_t )( sizeof( rExpected.m_szVisibleIndices ) )
	        || gpu_indirect.size() != ( int64_t )( sizeof( rExpected.m_szCommands ) ) ) {
		++m_nCullMismatches;
		return false;
	}
	t3dRdCpuCullResult_t actual;
	t3dRdCpuCullResultReset( &actual );
	memcpy( actual.m_szVisibleIndices, gpu_visible.ptr(), sizeof( actual.m_szVisibleIndices ) );
	memcpy( actual.m_szCommands, gpu_indirect.ptr(), sizeof( actual.m_szCommands ) );
	for ( size_t iCommand = 0u; iCommand < T3D_ARRAY_COUNT( actual.m_szCommands ); iCommand++ ) {
		const t3dRdIndexedIndirectCommand_t &command = ( actual.m_szCommands )[iCommand];
		actual.m_nVisibleCount += command.m_nInstanceCount;
	}
	if ( !Terrain3DRdCpuCuller::validate( rSnapshot,
	             rViewProjection,
	             m_szStripPartCounts[LOD0_MESH],
	             m_szStripPartCounts[OUTER_RING_MESH],
	             actual ) ) {
		++m_nCullMismatches;
		return false;
	}
	for ( uint32_t group = 0u; group < T3D_RD_DRAW_GROUP_COUNT; ++group ) {
		const uint32_t expected_count = rExpected.m_szCommands[group].m_nInstanceCount;
		const uint32_t actual_count = actual.m_szCommands[group].m_nInstanceCount;
		if ( expected_count != actual_count ) {
			++m_nCullMismatches;
			return false;
		}
		bool expected_set[T3D_MAX_CLIPMAP_LEVELS];
		memset( expected_set, 0, sizeof( expected_set ) );
		bool actual_set[T3D_MAX_CLIPMAP_LEVELS];
		memset( actual_set, 0, sizeof( actual_set ) );
		for ( uint32_t slot = 0u; slot < expected_count; ++slot ) {
			expected_set[rExpected.m_szVisibleIndices[group * T3D_MAX_CLIPMAP_LEVELS + slot]] = true;
			actual_set[actual.m_szVisibleIndices[group * T3D_MAX_CLIPMAP_LEVELS + slot]] = true;
		}
		for ( uint32_t patch = 0u; patch < rSnapshot.m_nPatchCount; ++patch ) {
			if ( expected_set[patch] != actual_set[patch] ) {
				++m_nCullMismatches;
				return false;
			}
		}
	}
	if ( comparison_index == 0u ) {
		LOG( INFO, "[RD validation] GPU/CPU culling comparison passed" );
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::drawView( const uint32_t nView ) {
	if ( nView >= T3D_MAX_VIEWS ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	t3dRdViewState_t &state = m_szViewStates[nView];
	const int64_t draw_list = rd->draw_list_begin( state.m_hFramebuffer.rid(), RenderingDevice::DRAW_DEFAULT_ALL );
	if ( draw_list == RenderingDevice::INVALID_ID ) {
		return false;
	}
	for ( uint32_t group = 0u; group < T3D_RD_DRAW_GROUP_COUNT; ++group ) {
		const uint32_t part_count = getStripPartCountForGroup( group );
		if ( part_count == 0u || part_count > T3D_CLIPMAP_STRIP_PART_CAPACITY ) {
			++m_nIndirectValidationFailures;
			rd->draw_list_end();
			return false;
		}
		const int tier = group <= 1u ? 0 : ( group == 2u ? 1 : 2 );
		t3dRdDrawPushConstants_t constants;
		t3dRdDrawPushConstantsInit( &constants );
		constants.m_nPadding1 = getStripDescriptorBaseForGroup( group );
		memcpy( m_drawPushBytes.ptrw(), &constants, sizeof( constants ) );
		rd->draw_list_bind_render_pipeline( draw_list, state.m_hszPipelines[tier] );
		rd->draw_list_set_push_constant( draw_list, m_drawPushBytes, sizeof( constants ) );
		rd->draw_list_bind_uniform_set( draw_list, m_hszRasterUniformSets[tier], 0u );
		rd->draw_list_draw_indirect( draw_list,
		        false,
		        m_pIndirectBuffer->rid(),
		        m_stripIndirectSlice.m_nOffsetBytes + group * T3D_CLIPMAP_STRIP_PART_CAPACITY * sizeof( t3dRdDrawIndirectCommand_t ),
		        part_count,
		        sizeof( t3dRdDrawIndirectCommand_t ) );
		m_context.recordRenderIndirectCommandsSubmitted( T3D_RD_CONTEXT_OWNER_TERRAIN, part_count );
		++m_nIndirectDraws;
		m_nDrawsRendered += part_count;
	}
	rd->draw_list_end();
	return true;
}

bool Terrain3DRdClipmapRenderer::drawViewDirect( const uint32_t nView, const t3dRdCpuCullResult_t &rCullResult ) {
	if ( nView >= T3D_MAX_VIEWS ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	t3dRdViewState_t &state = m_szViewStates[nView];
	const int64_t draw_list = rd->draw_list_begin( state.m_hFramebuffer.rid(), RenderingDevice::DRAW_DEFAULT_ALL );
	if ( draw_list == RenderingDevice::INVALID_ID ) {
		return false;
	}
	for ( uint32_t group = 0u; group < T3D_RD_DRAW_GROUP_COUNT; ++group ) {
		const int tier = group <= 1u ? 0 : ( group == 2u ? 1 : 2 );
		const t3dMeshType_t type = group == T3D_RD_DRAW_GROUP_LOD0_NEAR ? LOD0_MESH : OUTER_RING_MESH;
		const uint32_t part_count = m_szStripPartCounts[type];
		rd->draw_list_bind_render_pipeline( draw_list, state.m_hszPipelines[tier] );
		rd->draw_list_bind_uniform_set( draw_list, m_hszRasterUniformSets[tier], 0u );
		const uint32_t count = rCullResult.m_szCommands[group].m_nInstanceCount;
		for ( uint32_t slot = 0u; slot < count; ++slot ) {
			for ( uint32_t part = 0u; part < part_count; ++part ) {
				t3dRdDrawPushConstants_t constants;
				t3dRdDrawPushConstantsInit( &constants );
				constants.m_nDirectPatchIndex = rCullResult.m_szVisibleIndices[group * T3D_MAX_CLIPMAP_LEVELS + slot];
				constants.m_nDirectMode = 1u;
				constants.m_nPadding0 = part;
				constants.m_nPadding1 = getStripDescriptorBaseForGroup( group );
				memcpy( m_drawPushBytes.ptrw(), &constants, sizeof( constants ) );
				rd->draw_list_set_push_constant( draw_list, m_drawPushBytes, sizeof( constants ) );
				rd->draw_list_draw( draw_list, false, 1u, t3dClipmapStripActualVertexCount( m_szStripParts[type][part] ) );
				++m_nDirectDebugDraws;
				++m_nDrawsRendered;
			}
		}
	}
	rd->draw_list_end();
	return true;
}


t3dRdTerrainBindings_t Terrain3DRdClipmapRenderer::resolveTerrainBindings( const t3dRdTerrainBindings_t &rBindings ) const {
	t3dRdTerrainBindings_t resolved = rBindings;
	if ( !resolved.m_hControlTextureRd.is_valid() ) resolved.m_hControlTextureRd = m_hFallbackArrayTexture.rid();
	if ( !resolved.m_hColorTextureRd.is_valid() ) resolved.m_hColorTextureRd = m_hFallbackArrayTexture.rid();
	if ( !resolved.m_hAlbedoTextureRd.is_valid() ) {
		resolved.m_hAlbedoTextureRd = resolved.m_hColorTextureRd.is_valid() ? resolved.m_hColorTextureRd : m_hFallbackArrayTexture.rid();
	}
	if ( !resolved.m_hNormalTextureRd.is_valid() ) resolved.m_hNormalTextureRd = m_hFallbackArrayTexture.rid();
	if ( !resolved.m_hSurfaceTextureRd.is_valid() ) resolved.m_hSurfaceTextureRd = m_hFallbackArrayTexture.rid();
	if ( !resolved.m_hMaterialDisplacementTextureRd.is_valid() ) resolved.m_hMaterialDisplacementTextureRd = m_hFallbackArrayTexture.rid();
	if ( !resolved.m_hDisplacementTextureRd.is_valid() ) resolved.m_hDisplacementTextureRd = m_hFallbackDisplacementTexture.rid();
	if ( !resolved.m_hDerivedTextureRd.is_valid() ) resolved.m_hDerivedTextureRd = m_hFallbackArrayTexture.rid();
	if ( !resolved.m_hMiddleCompositeTextureRd.is_valid() ) resolved.m_hMiddleCompositeTextureRd = m_hFallbackArrayTexture.rid();
	if ( !resolved.m_hTerrainShadowMaskTextureRd.is_valid() ) resolved.m_hTerrainShadowMaskTextureRd = m_hFallbackArrayTexture.rid();
	if ( !resolved.m_hMiddleHeightTextureRd.is_valid() ) resolved.m_hMiddleHeightTextureRd = m_hFallbackArrayTexture.rid();
	if ( !resolved.m_hMiddleHeightRegionMapTextureRd.is_valid() ) resolved.m_hMiddleHeightRegionMapTextureRd = m_hFallbackDisplacementTexture.rid();
	if ( !resolved.m_hFarHeightTextureRd.is_valid() ) resolved.m_hFarHeightTextureRd = m_hFallbackDisplacementTexture.rid();
	if ( !resolved.m_hFarColorTextureRd.is_valid() ) resolved.m_hFarColorTextureRd = m_hFallbackDisplacementTexture.rid();
	if ( !resolved.m_hFarAlbedoTextureRd.is_valid() ) {
		resolved.m_hFarAlbedoTextureRd = resolved.m_hAlbedoTextureRd.is_valid() ? resolved.m_hAlbedoTextureRd : m_hFallbackArrayTexture.rid();
	}
	if ( !resolved.m_hWorldNoiseTextureRd.is_valid() ) resolved.m_hWorldNoiseTextureRd = m_hFallbackDisplacementTexture.rid();
	if ( !resolved.m_hMacroNoiseTextureRd.is_valid() ) resolved.m_hMacroNoiseTextureRd = m_hFallbackDisplacementTexture.rid();
	if ( !resolved.m_hEditorBrushTextureRd.is_valid() ) resolved.m_hEditorBrushTextureRd = m_hFallbackDisplacementTexture.rid();
	// Near height residency may converge after topology is live. Preserve the neutral
	// array fallback so zero physical layers continue down the Middle/Far path.
	if ( !resolved.m_hHeightTextureRd.is_valid() ) resolved.m_hHeightTextureRd = m_hFallbackArrayTexture.rid();
	return resolved;
}

bool Terrain3DRdClipmapRenderer::render( RenderData *const pRenderData,
        const t3dRdFrameSnapshot_t &rSnapshot,
        const PackedByteArray &rRegionMapBytes,
        const uint64_t nRegionMapGeneration,
        const t3dRdTerrainBindings_t &rBindings,
        const bool bColorWrite ) {
	if ( !pRenderData || !isReady() || rSnapshot.m_nPatchCount == 0u || rRegionMapBytes.is_empty() ) {
		return false;
	}
	if ( !m_context.validateDevice() ) {
		setError( "Terrain3D RenderingDevice generation was invalidated" );
		return false;
	}
	Ref<RenderSceneBuffers> scene_buffers = pRenderData->get_render_scene_buffers();
	Ref<RenderSceneBuffersRD> buffers = scene_buffers;
	RenderSceneData *const scene_data = pRenderData->get_render_scene_data();
	if ( buffers.is_null() || !scene_data ) {
		setError( "RenderSceneBuffersRD/RenderSceneData unavailable" );
		return false;
	}
	const uint32_t view_count = buffers->get_view_count();
	if ( view_count == 0u || view_count > T3D_MAX_VIEWS ) {
		setError( "Unsupported Terrain3D RD view count" );
		return false;
	}
	const t3dRdTerrainBindings_t resolved_bindings = resolveTerrainBindings( rBindings );
	const uint64_t gpu_frame_id = m_nFramesRendered.load( T3D_MEMORY_ORDER_ACQUIRE ) + 1u;
	m_context.beginFrame( gpu_frame_id, rSnapshot.m_nPublicationGeneration );
	if ( !uploadSnapshotBuffers( rSnapshot, rRegionMapBytes, nRegionMapGeneration ) ) {
		setError( "Failed to upload Terrain3D RD frame data" );
		return false;
	}
	if ( !ensureUniformSets( resolved_bindings, rSnapshot.m_materialConfig, NULL ) ) {
		return false;
	}
	const RenderingDevice::TextureSamples samples = buffers->get_texture_samples();
	const bool bMsaa = samples != RenderingDevice::TEXTURE_SAMPLES_1;
	const bool bDirectDebug = m_bDirectDrawDebugEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );
	const bool bGpuCulling = m_bGpuCullingReady.load( T3D_MEMORY_ORDER_ACQUIRE );
	const bool bCompareCulling = m_bCullingComparisonEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );
	const Transform3D camera_transform = scene_data->get_cam_transform();
	for ( uint32_t view = 0u; view < view_count; ++view ) {
		RID target_color = view_count > 1u ? buffers->get_color_layer( view, bMsaa ) : buffers->get_color_texture( bMsaa );
		RID target_depth = view_count > 1u ? buffers->get_depth_layer( view, bMsaa ) : buffers->get_depth_texture( bMsaa );
		if ( !target_color.is_valid() || !target_depth.is_valid()
		        || !ensureViewState( view, target_color, target_depth, samples, bColorWrite ) ) {
			setError( "Failed to prepare Terrain3D framebuffer-dependent resources" );
			return false;
		}
		const Projection view_matrix = Projection( camera_transform.affine_inverse() );
		const Projection view_projection = scene_data->get_view_projection( view ) * view_matrix;
		Vector3 eye_position = camera_transform.origin;
		if ( view_count > 1u ) {
			eye_position += camera_transform.basis.xform( scene_data->get_view_eye_offset( view ) );
		}
		const Projection direct_views[T3D_RD_FORWARD_MAX_VIEWS] = { view_projection, view_projection };
		const Vector3 direct_cameras[T3D_RD_FORWARD_MAX_VIEWS] = { eye_position, eye_position };
		if ( !updateFrameUniforms( rSnapshot, direct_views, 1u, view_matrix, direct_cameras, NULL, Vector2(), Vector2(), false, NULL ) ) {
			setError( "Failed to update Terrain3D frame uniforms" );
			return false;
		}
		if ( !Terrain3DRdCpuCuller::build( rSnapshot,
		             view_projection,
		             m_szStripPartCounts[LOD0_MESH],
		             m_szStripPartCounts[OUTER_RING_MESH],
		             m_cpuCullResult ) ) {
			++m_nIndirectValidationFailures;
			setError( "Terrain3D CPU reference culler rejected patch or indirect bounds" );
			return false;
		}
		m_nVisiblePatches.store( m_cpuCullResult.m_nVisibleCount, T3D_MEMORY_ORDER_RELEASE );
		m_context.recordSceneCounts( rSnapshot.m_nPatchCount, m_cpuCullResult.m_nVisibleCount, 0u, 0u, 0u, 0u );
		if ( bDirectDebug ) {
			if ( !drawViewDirect( view, m_cpuCullResult ) ) {
				setError( "Failed to submit Terrain3D debug direct draws" );
				return false;
			}
			continue;
		}
		if ( bGpuCulling && bCompareCulling ) {
			if ( !runGpuCulling( rSnapshot ) ) {
				setError( "Failed to submit Terrain3D compute culling validation" );
				return false;
			}
			if ( !compareGpuCpuCulling( rSnapshot, view_projection, m_cpuCullResult ) ) {
				setError( "Terrain3D GPU/CPU culling visible-set mismatch" );
				return false;
			}
		}
		if ( !uploadCpuIndirect( m_cpuCullResult ) ) {
			setError( "Failed to upload Terrain3D CPU culling output" );
			return false;
		}
		if ( !drawView( view ) ) {
			setError( "Failed to submit Terrain3D indexed indirect draws" );
			return false;
		}
	}
	++m_nFramesRendered;
	m_context.recordSubmissionCounts(
	        bDirectDebug ? 0u : view_count * T3D_RD_DRAW_GROUP_COUNT, bDirectDebug || !bGpuCulling || !bCompareCulling ? 0u : view_count );
	m_context.recordSuccessfulPass( T3D_RD_CONTEXT_PASS_TERRAIN_CLIPMAP, gpu_frame_id );
	m_context.markWarmupComplete();
	clearError();
	return true;
}

bool Terrain3DRdClipmapRenderer::prepareForwardPass( const t3dRdForwardPassContext_t& rContext,
                                                     const t3dRdFrameSnapshot_t& rSnapshot,
                                                     const PackedByteArray& rRegionMapBytes,
                                                     const uint64_t nRegionMapGeneration,
                                                     const t3dRdTerrainBindings_t& rBindings,
                                                     const t3dSharedVTBindings_t *const pSharedVTBindings,
                                                     const bool bTemporalHistoryValid,
                                                     const bool bViewportReconfigured,
                                                     const bool bOneSunLighting, const bool bPrepareRasterPipelines )
{
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE ) {
		return false;
	}
	if ( !isReady() || !m_context.validateDevice() || rSnapshot.m_nPatchCount == 0u || rRegionMapBytes.is_empty() ) {
		return false;
	}
	if ( rContext.m_nViewCount == 0u || rContext.m_nViewCount > T3D_RD_FORWARD_MAX_VIEWS ) {
		setError( "Terrain3D native terrain view count exceeds bridge bounds" );
		return false;
	}
	t3dRdForwardShaderVariant_t variant;
	if ( !t3dRdSelectForwardVariant( rContext, 0u, variant ) ) {
		return false;
	}
	const bool one_sun_lighting = bOneSunLighting && variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT;
	const t3dTerrainShaderPath_t shader_path = selectTerrainShaderPath( rSnapshot );
	if ( bPrepareRasterPipelines && !ensureForwardShaders( shader_path, variant, one_sun_lighting ) ) {
		setError( "Failed to acquire the selected Terrain3D Forward+ shader variant" );
		return false;
	}
	const uint8_t shipping_feature_class = t3dRdTerrainShippingFeatureClass( rSnapshot, shader_path );
	uint8_t lighting_model_classes[3];
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		lighting_model_classes[tier] = t3dRdTerrainLightingModelClass( rSnapshot.m_materialConfig, shader_path, variant, tier );
	}
	const uint8_t scene_integration_class =
	        t3dRdTerrainSceneIntegrationClass( rContext, shader_path, variant, one_sun_lighting );
	uint8_t dnr_output_classes[3];
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		dnr_output_classes[tier] = t3dRdTerrainDnrOutputClass( rSnapshot.m_materialConfig, shader_path, variant, tier );
	}
	const uint8_t middle_surface_class = t3dRdTerrainMiddleSurfaceClass( rSnapshot.m_materialConfig, shader_path );

	const t3dRdTerrainBindings_t resolved_bindings = resolveTerrainBindings( rBindings );

	for ( uint32_t view = 0u; view < T3D_RD_FORWARD_MAX_VIEWS; ++view ) {
		const uint32_t source = MIN( view, rContext.m_nViewCount - 1u );
		m_szForwardEyePositions[view] = rContext.m_cameraTransform.origin;
		if ( rContext.m_nViewCount > 1u ) {
			m_szForwardEyePositions[view] += rContext.m_cameraTransform.basis.xform( rContext.m_szEyeOffset[source] );
		}
	}
	m_bForwardHistoryValid = bTemporalHistoryValid;
	if ( m_nForwardPreparedScenePass != rContext.m_nScenePass || m_nForwardPreparedViewportKey != rContext.m_nViewportKey ) {
		m_context.beginFrame( rContext.m_nRenderFrame, rSnapshot.m_nPublicationGeneration );
	}
	clearError();
	if ( !uploadSnapshotBuffers( rSnapshot, rRegionMapBytes, nRegionMapGeneration )
	        || !ensureUniformSets( resolved_bindings, rSnapshot.m_materialConfig, pSharedVTBindings )
	        || !updateFrameUniforms( rSnapshot,
	                rContext.m_szViewProjection,
	                rContext.m_nViewCount,
	                Projection( rContext.m_cameraTransform.affine_inverse() ),
	                m_szForwardEyePositions,
	                rContext.m_szPreviousViewProjection,
	                rContext.m_taaJitter,
	                rContext.m_previousTaaJitter,
	                rContext.m_bMotionVectorsEnabled && m_bForwardHistoryValid,
	                pSharedVTBindings ) ) {
		if ( !m_bHasError.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
			setError( "Failed to prepare Terrain3D native Forward+ frame state" );
		}
		return false;
	}

	const bool shadow_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
	const bool previous_shadow_pass = m_forwardPreparedCullPassKind == T3D_RD_FORWARD_PASS_SHADOW;
	const bool recull = shadow_pass || previous_shadow_pass || m_nForwardPreparedScenePass != rContext.m_nScenePass
	        || m_nForwardPreparedViewportKey != rContext.m_nViewportKey
	        || m_nForwardPreparedSnapshotGeneration != rSnapshot.m_nPublicationGeneration;
	if ( recull ) {
		if ( !Terrain3DRdCpuCuller::buildMultiview( rSnapshot,
		             rContext.m_szViewProjection,
		             rContext.m_nViewCount,
		             m_szStripPartCounts[LOD0_MESH],
		             m_szStripPartCounts[OUTER_RING_MESH],
		             m_cpuCullResult ) ) {
			++m_nIndirectValidationFailures;
			setError( "Terrain3D CPU reference culler rejected native Forward+ patch bounds" );
			return false;
		}
		if ( shadow_pass ) {
			t3dRdApplyShadowCasterPolicy( rSnapshot, m_cpuCullResult );
		}
		m_nVisiblePatches.store( m_cpuCullResult.m_nVisibleCount, T3D_MEMORY_ORDER_RELEASE );
		if ( !m_bDirectDrawDebugEnabled.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
			const bool validate_gpu_cull = !shadow_pass && rContext.m_nViewCount == 1u
			        && m_bGpuCullingReady.load( T3D_MEMORY_ORDER_ACQUIRE ) && m_bCullingComparisonEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );
			if ( validate_gpu_cull ) {
				if ( !runGpuCulling( rSnapshot ) || !compareGpuCpuCulling( rSnapshot, rContext.m_szViewProjection[0], m_cpuCullResult ) ) {
					return false;
				}
			}
			if ( !uploadCpuIndirect( m_cpuCullResult ) ) {
				return false;
			}
		}
		m_nForwardPreparedScenePass = rContext.m_nScenePass;
		m_nForwardPreparedViewportKey = rContext.m_nViewportKey;
		m_nForwardPreparedSnapshotGeneration = rSnapshot.m_nPublicationGeneration;
		m_forwardPreparedCullPassKind = rContext.m_passKind;
	}
	if ( !bPrepareRasterPipelines ) {
		// Unified world raster borrows the uploaded terrain resources and performs
		// its own lighting and draws. Do not compile an unused legacy draw path.
		m_bLastForwardPipelineKeyValid = false;
		clearError();
		return true;
	}
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) {
		if ( bViewportReconfigured ) {
			m_forwardLighting.invalidateBorrowedState();
		}
		if ( !m_forwardLighting.prepare( rContext, rSnapshot.m_nRenderLayers, one_sun_lighting ) ) {
			setError( String( "Failed to prepare Terrain3D Forward+ native lighting: " )
			        + String( m_forwardLighting.getDiagnostics().get( "forward_light_last_error", "unknown" ) ) );
			return false;
		}
		for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
			if ( !m_forwardLighting.ensureSet( getForwardShader( tier, variant, one_sun_lighting, shader_path ) ).is_valid() ) {
				setError( "Failed to cache Terrain3D Forward+ terrain lighting descriptor" );
				return false;
			}
		}
	}
	if ( !ensureForwardPipelines( rContext,
	             variant,
	             one_sun_lighting,
	             shader_path,
	             shipping_feature_class,
	             lighting_model_classes,
	             scene_integration_class,
	             dnr_output_classes,
	             middle_surface_class ) ) {
		return false;
	}
	clearError();
	return true;
}

bool Terrain3DRdClipmapRenderer::drawForwardView( const t3dRdForwardPassContext_t &rContext ) {
	if ( rContext.m_nDrawList < 0 || !m_bLastForwardPipelineKeyValid ) {
		return false;
	}
	t3dRdForwardShaderVariant_t variant;
	if ( !t3dRdSelectForwardVariant( rContext, 0u, variant ) || variant != m_lastForwardPipelineKey.m_variant
	        || rContext.m_nFramebufferFormat != m_lastForwardPipelineKey.m_nFramebufferFormat
	        || rContext.m_nViewCount != m_lastForwardPipelineKey.m_nViewCount || rContext.m_passKind != m_lastForwardPipelineKey.m_passKind
	        || rContext.m_bReverseCull != m_lastForwardPipelineKey.m_bReverseCull
	        || ( ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION )
	                   && rContext.m_bDepthPrepassActive )
	                != m_lastForwardPipelineKey.m_bDepthPrepassActive ) {
		return false;
	}
	const t3dRdForwardPipelineEntry_t *const entry = findForwardPipeline( m_lastForwardPipelineKey );
	if ( !entry ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	const int64_t draw_list = rContext.m_nDrawList;
	const bool direct = m_bDirectDrawDebugEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );
	for ( uint32_t group = 0u; group < T3D_RD_DRAW_GROUP_COUNT; ++group ) {
		const uint32_t tier = group <= 1u ? 0u : ( group == 2u ? 1u : 2u );
		const t3dMeshType_t type = group == T3D_RD_DRAW_GROUP_LOD0_NEAR ? LOD0_MESH : OUTER_RING_MESH;
		const uint32_t part_count = m_szStripPartCounts[type];
		if ( part_count == 0u || part_count > T3D_CLIPMAP_STRIP_PART_CAPACITY ) {
			return false;
		}
		rd->draw_list_bind_render_pipeline( draw_list, entry->m_hszPipelines[tier] );
		rd->draw_list_bind_uniform_set( draw_list, m_hszRasterUniformSets[tier], 0u );
		if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) {
			const RID lighting_set = m_forwardLighting.getSet(
			        getForwardShader( tier, variant, m_lastForwardPipelineKey.m_bOneSunLighting, m_lastForwardPipelineKey.m_shaderPath ) );
			if ( !lighting_set.is_valid() ) {
				return false;
			}
			rd->draw_list_bind_uniform_set( draw_list, lighting_set, 2u );
		}
		if ( direct ) {
			const uint32_t count = m_cpuCullResult.m_szCommands[group].m_nInstanceCount;
			for ( uint32_t slot = 0u; slot < count; ++slot ) {
				for ( uint32_t part = 0u; part < part_count; ++part ) {
					t3dRdDrawPushConstants_t constants;
					t3dRdDrawPushConstantsInit( &constants );
					constants.m_nDirectPatchIndex = m_cpuCullResult.m_szVisibleIndices[group * T3D_MAX_CLIPMAP_LEVELS + slot];
					constants.m_nDirectMode = 1u;
					constants.m_nPadding0 = part;
					constants.m_nPadding1 = getStripDescriptorBaseForGroup( group );
					memcpy( m_drawPushBytes.ptrw(), &constants, sizeof( constants ) );
					rd->draw_list_set_push_constant( draw_list, m_drawPushBytes, sizeof( constants ) );
					rd->draw_list_draw( draw_list, false, 1u, t3dClipmapStripActualVertexCount( m_szStripParts[type][part] ) );
					++m_nDirectDebugDraws;
					++m_nDrawsRendered;
				}
			}
		} else {
			t3dRdDrawPushConstants_t constants;
			t3dRdDrawPushConstantsInit( &constants );
			constants.m_nPadding1 = getStripDescriptorBaseForGroup( group );
			memcpy( m_drawPushBytes.ptrw(), &constants, sizeof( constants ) );
			rd->draw_list_set_push_constant( draw_list, m_drawPushBytes, sizeof( constants ) );
			rd->draw_list_draw_indirect( draw_list,
			        false,
			        m_pIndirectBuffer->rid(),
			        m_stripIndirectSlice.m_nOffsetBytes + group * T3D_CLIPMAP_STRIP_PART_CAPACITY * sizeof( t3dRdDrawIndirectCommand_t ),
			        part_count,
			        sizeof( t3dRdDrawIndirectCommand_t ) );
			m_context.recordRenderIndirectCommandsSubmitted( T3D_RD_CONTEXT_OWNER_TERRAIN, part_count );
			++m_nIndirectDraws;
			m_nDrawsRendered += part_count;
		}
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::renderForwardPass( const t3dRdForwardPassContext_t &rContext ) {
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT ) {
		return false;
	}
	if ( m_nForwardPreparedScenePass != rContext.m_nScenePass || m_nForwardPreparedViewportKey != rContext.m_nViewportKey ) {
		return false;
	}
	if ( !drawForwardView( rContext ) ) {
		setError( "Failed to submit Terrain3D terrain onto native Forward+ draw list" );
		return false;
	}
	++m_nFramesRendered;
	m_context.recordSubmissionCounts( m_bDirectDrawDebugEnabled.load( T3D_MEMORY_ORDER_ACQUIRE ) ? 0u : T3D_RD_DRAW_GROUP_COUNT, 0u );
	m_context.markWarmupComplete();
	clearError();
	return true;
}

bool Terrain3DRdClipmapRenderer::snapshotWorldRasterBindingsHot( t3dRdWorldTerrainRasterBindings_t &rBindings ) const {
	if ( !m_bBoundDescriptorKeyValid || !m_hFrameUniformBuffer.is_valid() || !m_hRegionMapBuffer.is_valid()
	        || !m_hMaterialBuffer.is_valid() || !m_hMaterialConfigBuffer.is_valid() || !m_hEditorOverlayBuffer.is_valid() ) {
		return false;
	}
	rBindings = t3dRdWorldTerrainRasterBindings_t{};
	rBindings.m_hFrameBuffer = m_hFrameUniformBuffer.rid();
	rBindings.m_hRegionMapBuffer = m_hRegionMapBuffer.rid();
	rBindings.m_hMaterialBuffer = m_hMaterialBuffer.rid();
	rBindings.m_hPatchBuffer = m_hPatchBuffer.rid();
	rBindings.m_hVisibleBuffer = m_hVisibleBuffer.rid();
	rBindings.m_hMaterialConfigBuffer = m_hMaterialConfigBuffer.rid();
	rBindings.m_hEditorOverlayBuffer = m_hEditorOverlayBuffer.rid();
	rBindings.m_hTerrainSampler = m_hTerrainSampler.rid();
	rBindings.m_hMaterialSampler = m_hszMaterialSamplers[0].rid();
	rBindings.m_hMapSampler = m_hszMapSamplers[0].rid();
	rBindings.m_hHeightTexture = m_boundDescriptorKey.m_hBoundHeightTexture;
	rBindings.m_hControlTexture = m_boundDescriptorKey.m_hBoundControlTexture;
	rBindings.m_hColorTexture = m_boundDescriptorKey.m_hBoundColorTexture;
	rBindings.m_hAlbedoTexture = m_boundDescriptorKey.m_hBoundAlbedoTexture;
	rBindings.m_hNormalTexture = m_boundDescriptorKey.m_hBoundNormalTexture;
	rBindings.m_hSurfaceTexture = m_boundDescriptorKey.m_hBoundSurfaceTexture;
	rBindings.m_hMaterialDisplacementTexture = m_boundDescriptorKey.m_hBoundMaterialDisplacementTexture;
	rBindings.m_hDisplacementTexture = m_boundDescriptorKey.m_hBoundDisplacementTexture;
	rBindings.m_hDerivedTexture = m_boundDescriptorKey.m_hBoundDerivedTexture;
	rBindings.m_hMiddleCompositeTexture = m_boundDescriptorKey.m_hBoundMiddleCompositeTexture;
	rBindings.m_hMiddleHeightTexture = m_boundDescriptorKey.m_hBoundMiddleHeightTexture;
	rBindings.m_hMiddleHeightRegionMapTexture = m_boundDescriptorKey.m_hBoundMiddleHeightRegionMapTexture;
	rBindings.m_hFarHeightTexture = m_boundDescriptorKey.m_hBoundFarHeightTexture;
	rBindings.m_hFarColorTexture = m_boundDescriptorKey.m_hBoundFarColorTexture;
	rBindings.m_hFarAlbedoTexture = m_boundDescriptorKey.m_hBoundFarAlbedoTexture;
	rBindings.m_hWorldNoiseTexture = m_boundDescriptorKey.m_hBoundWorldNoiseTexture;
	rBindings.m_hMacroNoiseTexture = m_boundDescriptorKey.m_hBoundMacroNoiseTexture;
	rBindings.m_hEditorBrushTexture = m_boundDescriptorKey.m_hBoundEditorBrushTexture;
	rBindings.m_hTerrainShadowMaskTexture = m_boundDescriptorKey.m_hBoundTerrainShadowMaskTexture;
	rBindings.m_bPersistentFrameBuffer = m_bPersistentFrameUniformBuffer;
	rBindings.m_bPersistentCpuBuffers = m_bPersistentCpuBuffers;
	rBindings.m_nGeneration = m_nRegionMapGeneration ^ ( m_nMaterialGeneration << 1u ) ^ ( m_nPatchGeneration << 2u )
	        ^ m_boundDescriptorKey.m_nBoundSharedVTResourceGeneration;
	return true;
}

Dictionary Terrain3DRdClipmapRenderer::getDiagnostics( void ) const {
	Terrain3DScopedLock state_lock( m_resourceStateMutex );
	Dictionary result = m_context.getDiagnostics();
	result["frames_rendered"] = ( int64_t )( m_nFramesRendered.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["forward_one_sun_active"] = m_bLastForwardPipelineKeyValid && m_lastForwardPipelineKey.m_bOneSunLighting;
	result["forward_shader_path"] = m_bLastForwardPipelineKeyValid ? ( int64_t )( m_lastForwardPipelineKey.m_shaderPath ) : -1;
	result["forward_shipping_feature_class"] =
	        m_bLastForwardPipelineKeyValid ? ( int64_t )( m_lastForwardPipelineKey.m_nShippingFeatureClass ) : -1;
	result["forward_lighting_model_near"] =
	        m_bLastForwardPipelineKeyValid ? ( int64_t )( m_lastForwardPipelineKey.m_nszLightingModelClass[0] ) : -1;
	result["forward_lighting_model_middle"] =
	        m_bLastForwardPipelineKeyValid ? ( int64_t )( m_lastForwardPipelineKey.m_nszLightingModelClass[1] ) : -1;
	result["forward_lighting_model_far"] =
	        m_bLastForwardPipelineKeyValid ? ( int64_t )( m_lastForwardPipelineKey.m_nszLightingModelClass[2] ) : -1;
	result["forward_scene_integration_class"] =
	        m_bLastForwardPipelineKeyValid ? ( int64_t )( m_lastForwardPipelineKey.m_nSceneIntegrationClass ) : -1;
	result["draws_rendered"] = ( int64_t )( m_nDrawsRendered.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["indirect_draws"] = ( int64_t )( m_nIndirectDraws.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["cull_dispatches"] = ( int64_t )( m_nCullDispatches.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["clipmap_culling_mode"] = String( "cpu_indirect" );
	result["gpu_culling_validation_only"] = true;
	result["repeated_error_count"] = ( int64_t )( m_nRepeatedErrorCount );
	result["patch_uploads"] = ( int64_t )( m_nPatchUploads.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["patch_dirty_ranges"] = ( int64_t )( m_nPatchDirtyRanges.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["editor_overlay_uploads"] = ( int64_t )( m_nEditorOverlayUploads.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["persistent_frame_uniform_buffer"] = m_bPersistentFrameUniformBuffer;
	result["persistent_cpu_buffers"] = m_bPersistentCpuBuffers;
	result["visible_terrain_patches"] = ( int64_t )( m_nVisiblePatches.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["cull_comparison_enabled"] = m_bCullingComparisonEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["cull_comparisons"] = ( int64_t )( m_nCullComparisons.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["cull_mismatches"] = ( int64_t )( m_nCullMismatches.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["indirect_validation_failures"] = ( int64_t )( m_nIndirectValidationFailures.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["direct_draw_debug_enabled"] = m_bDirectDrawDebugEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["direct_debug_draws"] = ( int64_t )( m_nDirectDebugDraws.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["topology_bytes"] = ( int64_t )( getTopologyBytes() );
	result["strip_descriptor_bytes"] = ( int64_t )( getStripDescriptorBytes() );
	result["lod0_strip_parts"] = ( int64_t )( m_szStripPartCounts[LOD0_MESH] );
	result["outer_strip_parts"] = ( int64_t )( m_szStripPartCounts[OUTER_RING_MESH] );
	result["strip_indirect_bytes"] = ( int64_t )( sizeof( t3dRdDrawIndirectCommand_t ) * T3D_CLIPMAP_STRIP_COMMAND_CAPACITY );
	result["indirect_arena_bytes"] = ( int64_t )( T3D_RD_FIXED_INDIRECT_ARENA_BYTES );
	result["indexed_indirect_offset_bytes"] = ( int64_t )( m_indexedIndirectSlice.m_nOffsetBytes );
	result["strip_indirect_offset_bytes"] = ( int64_t )( m_stripIndirectSlice.m_nOffsetBytes );
	result["gpu_culling"] = m_bGpuCullingReady.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["fallback_displacement_texture"] = m_hFallbackDisplacementTexture.isValid();
	result["max_views"] = ( int64_t )( T3D_MAX_VIEWS );
	result["forward_lighting"] = m_forwardLighting.getDiagnostics();
	result["last_error"] = getLastError();
	return result;
}
