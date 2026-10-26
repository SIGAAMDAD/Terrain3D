// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/infrastructure/terrain_3d_rd_binding_builder.h"
#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "rendering/rd/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/infrastructure/terrain_3d_gpu_upload_service.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"

#include "core/logger.h"
#include "rendering/rd/terrain_3d_rd_persistent.h"
#include "rendering/rd/vt/terrain_3d_vt_contract.h"
#include "rendering/rd/terrain_3d_rd_shader_compile_status.h"

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
        m_cullPipelines( m_context, Terrain3DRdContext::OWNER_TERRAIN ),
        m_viewPipelines( m_context, Terrain3DRdContext::OWNER_TERRAIN ),
        m_descriptorCache( m_context, Terrain3DRdContext::OWNER_TERRAIN ),
        m_forwardPipelines( m_context, Terrain3DRdContext::OWNER_TERRAIN ) {
}

Terrain3DRdClipmapRenderer::~Terrain3DRdClipmapRenderer( void ) {
	shutdown();
}

void Terrain3DRdClipmapRenderer::setError( const String &rError ) {
	bool bReport = true;
	{
		t3dScopedLock_t lock( m_diagnosticsMutex );
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
	t3dScopedLock_t lock( m_diagnosticsMutex );
	return m_lastError;
}

bool Terrain3DRdClipmapRenderer::prepareStripParts( const t3dClipmapStripPart_t *const pLod0Parts,
        const uint16_t nLod0PartCount,
        const t3dClipmapStripPart_t *const pOuterParts,
        const uint16_t nOuterPartCount ) {
	t3dScopedLock_t state_lock( m_resourceStateMutex );
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
	const uint32_t required_bytes = sizeof( t3dRdDrawIndirectCommand_t ) * s_nStripCommandCapacity;
	if ( m_cpuStripIndirectBytes.size() != required_bytes ) {
		return false;
	}
	t3dRdDrawIndirectCommand_t *const commands = ( t3dRdDrawIndirectCommand_t * )( m_cpuStripIndirectBytes.ptrw() );
	memset( commands, 0, required_bytes );
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
		const t3dMeshType_t type = group == T3D_RD_DRAW_GROUP_LOD0_NEAR ? LOD0_MESH : OUTER_RING_MESH;
		const uint32_t part_count = m_szStripPartCounts[type];
		for ( uint32_t part = 0u; part < part_count; ++part ) {
			t3dRdDrawIndirectCommand_t &command = commands[group * T3D_CLIPMAP_STRIP_PART_CAPACITY + part];
			command.m_nVertexCount = t3dClipmapStripActualVertexCount( m_szStripParts[type][part] );
			command.m_nInstanceCount = rResult.m_szCommands[group].m_nInstanceCount;
			command.m_nFirstVertex = t3dClipmapStripVirtualFirstVertex( part );
			command.m_nFirstInstance = group * s_nMaxClipmapLevels;
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
		} else if ( m_context.getState() == Terrain3DRdContext::STATE_FAILED ) {
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

bool Terrain3DRdClipmapRenderer::ensureShaders( const Ref<RDShaderSPIRV> &rNear,
        const Ref<RDShaderSPIRV> &rMiddle,
        const Ref<RDShaderSPIRV> &rFar,
        const Ref<RDShaderSPIRV> &rCull ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	const Ref<RDShaderSPIRV> shaders[3] = { rNear, rMiddle, rFar };
	const char *names[3] = { "T3D.PipelineShader.Near", "T3D.PipelineShader.Middle", "T3D.PipelineShader.Far" };
	for ( int i = 0; i < 3; ++i ) {
		if ( m_hszShaders[i].is_valid() ) {
			continue;
		}
		if ( shaders[i].is_null() ) {
			setError( "Missing Terrain3D RD raster shader SPIR-V" );
			return false;
		}
		m_hszShaders[i] = t3dRdCreateShaderWithStatus( rd, shaders[i], String( names[i] ) );
		if ( !m_hszShaders[i].is_valid() ) {
			m_context.recordResourceCreationFailure( names[i], Terrain3DRdContext::RESOURCE_SHADER );
			setError( "Failed to create Terrain3D RD raster shader" );
			return false;
		}
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN ).adopt( rd, m_hszShaders[i], names[i], Terrain3DRdContext::RESOURCE_SHADER ) ) {
			setError( "Failed to register Terrain3D RD raster shader ownership" );
			return false;
		}
	}
	if ( !m_hCullShader.is_valid() && rCull.is_valid() ) {
		m_hCullShader = t3dRdCreateShaderWithStatus( rd, rCull, "T3D.Shader.Cull" );
		if ( !m_hCullShader.is_valid() ) {
			m_context.recordResourceCreationFailure(
			        "T3D.Shader.Cull", Terrain3DRdContext::RESOURCE_SHADER, "Failed to create Terrain3D culling shader" );
			setError( "Failed to create Terrain3D culling shader" );
			return false;
		}
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN ).adopt( rd, m_hCullShader, "T3D.Shader.Cull", Terrain3DRdContext::RESOURCE_SHADER ) ) {
			return false;
		}
		m_hCullPipeline = m_cullPipelines.getOrCreateCompute( m_hCullShader, "T3D.Pipeline.Cull", m_hCullShader );
		if ( !m_hCullPipeline.is_valid() ) {
			m_context.recordResourceCreationFailure(
			        "T3D.Pipeline.Cull", Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE, "Failed to create Terrain3D culling pipeline" );
			setError( "Failed to create Terrain3D culling pipeline" );
			return false;
		}
	}
	m_bGpuCullingReady = m_hCullShader.is_valid() && m_hCullPipeline.is_valid();
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureForwardShaders( const t3dRdForwardShaderBundle_t &rShaders ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	for ( uint32_t path = 0u; path < T3D_TERRAIN_SHADER_PATH_COUNT; ++path ) {
		for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
			for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
				if ( m_hszForwardShaders[path][tier][variant].is_valid() ) {
					continue;
				}
				const Ref<RDShaderSPIRV> spirv = rShaders.m_szTerrain[path][tier][variant];
				if ( spirv.is_null() ) {
					setError( "Missing Terrain3D Forward+ terrain shader path/variant" );
					return false;
				}
				const String name = String( "T3D.Forward.Shader.Terrain." ) + String::num_int64( path ) + "."
				        + String::num_int64( tier ) + "." + String::num_int64( variant );
				RID shader = t3dRdCreateShaderWithStatus( rd, spirv, name );
				if ( !shader.is_valid() || !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN ).adopt( rd, shader, name, Terrain3DRdContext::RESOURCE_SHADER ) ) {
					setError( "Failed to create Terrain3D Forward+ terrain shader path/variant" );
					return false;
				}
				m_hszForwardShaders[path][tier][variant] = shader;
			}
			for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT; ++variant ) {
				if ( m_hszForwardOneSunShaders[path][tier][variant].is_valid() ) {
					continue;
				}
				const Ref<RDShaderSPIRV> spirv = rShaders.m_szTerrainOneSun[path][tier][variant];
				if ( spirv.is_null() ) {
					setError( "Missing Terrain3D one-sun Forward+ terrain shader path/variant" );
					return false;
				}
				const String name = String( "T3D.Forward.Shader.TerrainOneSun." ) + String::num_int64( path ) + "."
				        + String::num_int64( tier ) + "." + String::num_int64( variant );
				RID shader = t3dRdCreateShaderWithStatus( rd, spirv, name );
				if ( !shader.is_valid() || !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN ).adopt( rd, shader, name, Terrain3DRdContext::RESOURCE_SHADER ) ) {
					setError( "Failed to create Terrain3D one-sun Forward+ terrain shader path/variant" );
					return false;
				}
				m_hszForwardOneSunShaders[path][tier][variant] = shader;
			}
		}
	}
	return true;
}

void Terrain3DRdClipmapRenderer::freeForwardPipelines( void ) {
	m_forwardPipelines.clear();
	m_currentForwardPipelineEntry = t3dRdForwardPipelineEntry_t{};
	m_bLastForwardPipelineKeyValid = false;
}

bool Terrain3DRdClipmapRenderer::selectForwardVariant(
        const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t &rVariant ) const {
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ) {
		if ( rContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_SHADOW ) {
			return false;
		}
		rVariant = T3D_RD_FORWARD_SHADER_DEPTH;
		return true;
	}
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH ) {
		switch ( rContext.m_nativePassMode ) {
			case T3D_RD_FORWARD_NATIVE_PASS_DEPTH:
				rVariant = T3D_RD_FORWARD_SHADER_DEPTH;
				return true;
			case T3D_RD_FORWARD_NATIVE_PASS_DEPTH_NORMAL_ROUGHNESS:
				rVariant = T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS;
				return true;
			case T3D_RD_FORWARD_NATIVE_PASS_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI:
				rVariant = T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI;
				return true;
			default:
				return false;
		}
	}
	if ( ( rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION )
	        || rContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_COLOR ) {
		return false;
	}
	const bool specular = ( rContext.m_nColorPassFlags & ( 1u << 1u ) ) != 0u;
	const bool motion = rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || ( rContext.m_nColorPassFlags & ( 1u << 3u ) ) != 0u;
	const bool radiance_array = ( rContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY ) != 0u;
	if ( radiance_array ) {
		rVariant = specular ? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION_RADIANCE_ARRAY
		                               : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_RADIANCE_ARRAY )
		                    : ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION_RADIANCE_ARRAY : T3D_RD_FORWARD_SHADER_COLOR_RADIANCE_ARRAY );
	} else {
		rVariant = specular ? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR )
		                    : ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION : T3D_RD_FORWARD_SHADER_COLOR );
	}
	return true;
}

const Terrain3DRdClipmapRenderer::t3dRdForwardPipelineEntry_t *Terrain3DRdClipmapRenderer::findForwardPipeline(
        const t3dRdForwardPipelineKey_t &rKey ) const {
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		const auto *const entry = m_forwardPipelines.find( { rKey, tier } );
		if ( !entry ) {
			return nullptr;
		}
		m_currentForwardPipelineEntry.m_hszPipelines[tier] = entry->m_hPipeline;
	}
	m_currentForwardPipelineEntry.m_key = rKey;
	return &m_currentForwardPipelineEntry;
}

static void t3dRdApplyShadowCasterPolicy( const t3dRdFrameSnapshot_t &rSnapshot, t3dRdCpuCullResult_t &rResult ) {
	uint32_t visible_count = 0u;
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
		t3dRdIndexedIndirectCommand_t &command = rResult.m_szCommands[group];
		uint32_t write = 0u;
		for ( uint32_t read = 0u; read < command.m_nInstanceCount; ++read ) {
			const uint32_t patch_index = rResult.m_szVisibleIndices[group * s_nMaxClipmapLevels + read];
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
			rResult.m_szVisibleIndices[group * s_nMaxClipmapLevels + write++] = patch_index;
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
		return m_hszForwardOneSunShaders[shaderPath][nTier][variant];
	}
	return m_hszForwardShaders[shaderPath][nTier][variant];
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
	key.m_nFramebufferFormat = rContext.m_nFramebufferFormat;
	key.m_variant = variant;
	key.m_passKind = rContext.m_passKind;
	key.m_samples = rd->framebuffer_format_get_texture_samples( rContext.m_nFramebufferFormat, 0u );
	if ( ( 1u << ( uint32_t )( key.m_samples ) ) != rContext.m_nSampleCount ) {
		setError( "Terrain3D Forward+ sample-count metadata does not match native framebuffer format" );
		return false;
	}
	key.m_nViewCount = rContext.m_nViewCount;
	key.m_bReverseCull = rContext.m_bReverseCull;
	key.m_bDepthPrepassActive = ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION )
	        && rContext.m_bDepthPrepassActive;
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
	Ref<RDPipelineRasterizationState> raster;
	raster.instantiate();
	raster->set_cull_mode( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW
	                ? ( rContext.m_bReverseCull ? RenderingDevice::POLYGON_CULL_FRONT : RenderingDevice::POLYGON_CULL_BACK )
	                : RenderingDevice::POLYGON_CULL_DISABLED );
	raster->set_front_face( RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE );
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count( key.m_samples );
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	depth->set_enable_depth_test( true );
	depth->set_enable_depth_write( !key.m_bDepthPrepassActive );
	depth->set_depth_compare_operator(
	        key.m_bDepthPrepassActive ? RenderingDevice::COMPARE_OP_EQUAL : RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL );
	Ref<RDPipelineColorBlendState> blend;
	blend.instantiate();
	uint32_t blend_count = 0u;
	if ( variant <= T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION_RADIANCE_ARRAY ) {
		blend_count = 3u;
	} else if ( variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS ) {
		blend_count = 1u;
	} else if ( variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI ) {
		blend_count = 2u;
	}
	if ( blend_count > 0u ) {
		TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
		for ( uint32_t i = 0u; i < blend_count; ++i ) {
			Ref<RDPipelineColorBlendStateAttachment> attachment;
			attachment.instantiate();
			attachment->set_enable_blend( false );
			attachments.push_back( attachment );
		}
		blend->set_attachments( attachments );
	}

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
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	if ( !m_hTerrainSampler.is_valid() ) {
		Ref<RDSamplerState> state;
		state.instantiate();
		state->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_mip_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		state->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_hTerrainSampler = rd->sampler_create( state );
		if ( !m_hTerrainSampler.is_valid() ) {
			m_context.recordResourceCreationFailure( "T3D.Sampler.Terrain", Terrain3DRdContext::RESOURCE_SAMPLER );
			setError( "Failed to create terrain data sampler" );
			return false;
		}
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN ).adopt( rd, m_hTerrainSampler, "T3D.Sampler.Terrain", Terrain3DRdContext::RESOURCE_SAMPLER ) ) {
			return false;
		}
	}
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		const bool bNearest = i >= 2u;
		const bool bAnisotropic = i == 0u || i == 2u;
		if ( !m_hszMaterialSamplers[i].is_valid() ) {
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
			m_hszMaterialSamplers[i] = rd->sampler_create( state );
			const String name = String( "T3D.Sampler.Material." ) + String::num_int64( i );
			if ( !m_hszMaterialSamplers[i].is_valid() ) {
				m_context.recordResourceCreationFailure( name, Terrain3DRdContext::RESOURCE_SAMPLER );
				setError( "Failed to create material sampler" );
				return false;
			}
			if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN ).adopt( rd, m_hszMaterialSamplers[i], name, Terrain3DRdContext::RESOURCE_SAMPLER ) ) {
				return false;
			}
		}
		if ( !m_hszMapSamplers[i].is_valid() ) {
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
			m_hszMapSamplers[i] = rd->sampler_create( map_state );
			const String map_name = String( "T3D.Sampler.Map." ) + String::num_int64( i );
			if ( !m_hszMapSamplers[i].is_valid() ) {
				m_context.recordResourceCreationFailure( map_name, Terrain3DRdContext::RESOURCE_SAMPLER );
				setError( "Failed to create terrain map sampler" );
				return false;
			}
			if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN ).adopt( rd, m_hszMapSamplers[i], map_name, Terrain3DRdContext::RESOURCE_SAMPLER ) ) {
				return false;
			}
		}
		if ( !m_hszVtSamplers[i].is_valid() ) {
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
			m_hszVtSamplers[i] = rd->sampler_create( vt_state );
			const String vt_name = String( "T3D.Sampler.VT." ) + String::num_int64( i );
			if ( !m_hszVtSamplers[i].is_valid() ) {
				m_context.recordResourceCreationFailure( vt_name, Terrain3DRdContext::RESOURCE_SAMPLER );
				setError( "Failed to create Sparse VT sampler" );
				return false;
			}
			if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN ).adopt( rd, m_hszVtSamplers[i], vt_name, Terrain3DRdContext::RESOURCE_SAMPLER ) ) {
				return false;
			}
		}
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureFallbackTextures( void ) {
	if ( m_hFallbackDisplacementTexture.is_valid() && m_hFallbackArrayTexture.is_valid() ) {
		return true;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	Ref<RDTextureFormat> format2d;
	format2d.instantiate();
	format2d->set_width( 1u );
	format2d->set_height( 1u );
	format2d->set_depth( 1u );
	format2d->set_array_layers( 1u );
	format2d->set_mipmaps( 1u );
	format2d->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
	format2d->set_format( RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT );
	format2d->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT );
	Ref<RDTextureView> view;
	view.instantiate();
	PackedByteArray texel;
	texel.resize( 4 * sizeof( float ) );
	const float neutral[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
	memcpy( texel.ptrw(), neutral, sizeof( neutral ) );
	TypedArray<PackedByteArray> initial_data;
	initial_data.push_back( texel );
	m_hFallbackDisplacementTexture = rd->texture_create( format2d, view, initial_data );
	if ( !m_hFallbackDisplacementTexture.is_valid() ) {
		m_context.recordResourceCreationFailure( "T3D.Texture.DisplacementFallback", Terrain3DRdContext::RESOURCE_TEXTURE );
		setError( "Failed to create neutral Terrain3D displacement texture" );
		return false;
	}
	if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN ).adopt( rd,
	             m_hFallbackDisplacementTexture,
	             "T3D.Texture.DisplacementFallback",
	             Terrain3DRdContext::RESOURCE_TEXTURE,
	             Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
	             sizeof( neutral ) ) ) {
		return false;
	}
	Ref<RDTextureFormat> format_array;
	format_array.instantiate();
	format_array->set_width( 1u );
	format_array->set_height( 1u );
	format_array->set_depth( 1u );
	format_array->set_array_layers( 1u );
	format_array->set_mipmaps( 1u );
	format_array->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D_ARRAY );
	format_array->set_format( RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT );
	format_array->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT );
	m_hFallbackArrayTexture = rd->texture_create( format_array, view, initial_data );
	if ( !m_hFallbackArrayTexture.is_valid() ) {
		m_context.recordResourceCreationFailure( "T3D.Texture.ArrayFallback", Terrain3DRdContext::RESOURCE_TEXTURE );
		setError( "Failed to create neutral Terrain3D array fallback texture" );
		return false;
	}
	if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN ).adopt( rd,
	             m_hFallbackArrayTexture,
	             "T3D.Texture.ArrayFallback",
	             Terrain3DRdContext::RESOURCE_TEXTURE,
	             Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
	             sizeof( neutral ) ) ) {
		return false;
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureBuffers( void ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	const uint32_t frame_bytes = ( uint32_t )( sizeof( t3dRdFrameUniforms_t ) );
	const uint32_t material_bytes = ( uint32_t )( sizeof( t3dRdMaterialLayer_t ) * s_nRdMaterialLayerCount );
	const uint32_t material_config_bytes = ( uint32_t )( sizeof( t3dRdMaterialConfig_t ) );
	const uint32_t editor_overlay_bytes = ( uint32_t )( sizeof( t3dRdEditorOverlayConfig_t ) );
	const uint32_t patch_bytes = ( uint32_t )( sizeof( t3dRdPatch_t ) * s_nMaxClipmapLevels );
	const uint32_t visible_bytes = ( uint32_t )( sizeof( uint32_t ) * s_nRdDrawGroupCount * s_nMaxClipmapLevels );
	const uint32_t indirect_bytes = s_nStripIndirectOffsetBytes;
	const uint32_t strip_indirect_bytes = s_nStripIndirectBytes;
	const uint32_t strip_part_bytes = ( uint32_t )( sizeof( m_szStripParts ) );
	if ( m_stripPartBytes.size() != strip_part_bytes ) {
		setError( "Terrain3D procedural strip descriptors were not prepared before RD initialization" );
		return false;
	}
	if ( !m_hFrameUniformBuffer.is_valid() ) {
		const BitField<RenderingDevice::BufferCreationBits> creation_bits =
		        m_bPersistentCpuBuffers ? t3dRdPersistentCreationBits() : BitField<RenderingDevice::BufferCreationBits>( 0 );
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN )
		                    .createUniformBuffer( m_hFrameUniformBuffer, "T3D.Frame.UBO", frame_bytes,
		                            Terrain3DRdContext::MEMORY_UNIFORM, creation_bits ) ) {
			m_context.recordResourceCreationFailure( "T3D.Frame.UBO", Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER );
			return false;
		}
	}
	if ( !m_hMaterialBuffer.is_valid() ) {
		m_bMaterialUploadInitialized = false;
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN )
		                    .createStorageBuffer( m_hMaterialBuffer, "T3D.Materials", material_bytes,
		                            Terrain3DRdContext::MEMORY_MATERIAL_TABLE,
		                            BitField<RenderingDevice::StorageBufferUsage>( 0 ),
		                            m_bPersistentCpuBuffers ? t3dRdPersistentCreationBits() : BitField<RenderingDevice::BufferCreationBits>( 0 ) ) ) {
			m_context.recordResourceCreationFailure( "T3D.Materials", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER );
			return false;
		}
	}
	if ( !m_hMaterialConfigBuffer.is_valid() ) {
		m_bMaterialUploadInitialized = false;
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN )
		                    .createStorageBuffer( m_hMaterialConfigBuffer, "T3D.MaterialConfig", material_config_bytes,
		                            Terrain3DRdContext::MEMORY_MATERIAL_TABLE,
		                            BitField<RenderingDevice::StorageBufferUsage>( 0 ),
		                            m_bPersistentCpuBuffers ? t3dRdPersistentCreationBits() : BitField<RenderingDevice::BufferCreationBits>( 0 ) ) ) {
			m_context.recordResourceCreationFailure( "T3D.MaterialConfig", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER );
			return false;
		}
	}
	if ( !m_hEditorOverlayBuffer.is_valid() ) {
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN )
		                    .createStorageBuffer( m_hEditorOverlayBuffer, "T3D.EditorOverlay", editor_overlay_bytes,
		                            Terrain3DRdContext::MEMORY_MATERIAL_TABLE,
		                            BitField<RenderingDevice::StorageBufferUsage>( 0 ),
		                            m_bPersistentCpuBuffers ? t3dRdPersistentCreationBits() : BitField<RenderingDevice::BufferCreationBits>( 0 ) ) ) {
			m_context.recordResourceCreationFailure( "T3D.EditorOverlay", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER );
			return false;
		}
	}
	if ( !m_hPatchBuffer.is_valid() ) {
		m_bPatchUploadInitialized = false;
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN )
		                    .createStorageBuffer( m_hPatchBuffer, "T3D.Clipmap.Patches", patch_bytes,
		                            Terrain3DRdContext::MEMORY_PATCH,
		                            BitField<RenderingDevice::StorageBufferUsage>( 0 ),
		                            m_bPersistentCpuBuffers ? t3dRdPersistentCreationBits() : BitField<RenderingDevice::BufferCreationBits>( 0 ) ) ) {
			m_context.recordResourceCreationFailure( "T3D.Clipmap.Patches", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER );
			return false;
		}
	}
	if ( !m_hVisibleBuffer.is_valid() ) {
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN )
		                    .createBuffer( m_hVisibleBuffer, "T3D.Clipmap.Visible", Terrain3DRdContext::RESOURCE_CULLING_BUFFER,
		                            Terrain3DRdContext::MEMORY_SCRATCH, visible_bytes ) ) {
			m_context.recordResourceCreationFailure( "T3D.Clipmap.Visible", Terrain3DRdContext::RESOURCE_CULLING_BUFFER );
			return false;
		}
	}
	if ( !m_hStripPartBuffer.is_valid() ) {
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN )
		                    .createBuffer( m_hStripPartBuffer, "T3D.Clipmap.StripParts", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
		                            Terrain3DRdContext::MEMORY_STORAGE, strip_part_bytes, m_stripPartBytes ) ) {
			m_context.recordResourceCreationFailure( "T3D.Clipmap.StripParts", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER );
			return false;
		}
	}
	if ( !m_hIndirectBuffer.is_valid() ) {
		uint32_t indirect_offset = 0u;
		if ( !m_context.acquireFixedIndirect( Terrain3DRdContext::FIXED_INDIRECT_CLIPMAP,
		             s_nIndirectArenaBytes, m_hIndirectBuffer, indirect_offset )
		        || indirect_offset != Terrain3DRdContext::fixedIndirectOffset( Terrain3DRdContext::FIXED_INDIRECT_CLIPMAP ) ) {
			m_context.recordResourceCreationFailure( "T3D.Clipmap.IndirectArena", Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER );
			return false;
		}
	}
	if ( !m_indexedIndirectSlice.m_hBuffer.is_valid() || !m_stripIndirectSlice.m_hBuffer.is_valid() ) {
		Terrain3DGpuBufferLayout layout;
		layout.beginLayout( m_hIndirectBuffer, Terrain3DRdContext::FIXED_INDIRECT_ARENA_BYTES );
		t3dGpuBufferSlice_t prefix;
		if ( !layout.reserve( Terrain3DRdContext::fixedIndirectOffset( Terrain3DRdContext::FIXED_INDIRECT_CLIPMAP ), 16u, prefix )
		        || !layout.reserve( s_nStripIndirectOffsetBytes, 4u, m_indexedIndirectSlice )
		        || !layout.reserve( s_nStripIndirectBytes, 16u, m_stripIndirectSlice )
		        || m_stripIndirectSlice.m_nOffsetBytes != Terrain3DRdContext::fixedIndirectOffset( Terrain3DRdContext::FIXED_INDIRECT_CLIPMAP ) + s_nStripIndirectOffsetBytes ) {
			setError( "Failed to lay out clipmap indirect command slices" );
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
	if ( m_hRegionMapBuffer.is_valid() && nRequiredBytes <= m_nRegionMapCapacityBytes ) {
		return true;
	}
	freeUniformSets();
	m_context.freeResource( m_hRegionMapBuffer, m_nRegionMapCapacityBytes );
	m_nRegionMapCapacityBytes = nRequiredBytes;
	if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN )
	                    .createStorageBuffer( m_hRegionMapBuffer, "T3D.RegionMap", m_nRegionMapCapacityBytes,
	                            Terrain3DRdContext::MEMORY_STORAGE,
	                            BitField<RenderingDevice::StorageBufferUsage>( 0 ),
	                            m_bPersistentCpuBuffers ? t3dRdPersistentCreationBits() : BitField<RenderingDevice::BufferCreationBits>( 0 ) ) ) {
		m_nRegionMapCapacityBytes = 0u;
		m_context.recordResourceCreationFailure( "T3D.RegionMap", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER );
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
        const T3DTerrainStageASharedVTBindings *const pSharedVTBindings ) const {
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
        const T3DTerrainStageASharedVTBindings *const pSharedVTBindings ) {
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
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 2, m_hTerrainSampler, rHeight ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 3, m_hTerrainSampler, rControl ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 4, m_hszMapSamplers[nFiltering], rColor ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 5, m_hszMaterialSamplers[nFiltering], rAlbedo ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 6, m_hszMaterialSamplers[nFiltering], rNormal ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 7, m_hszMaterialSamplers[nFiltering], rSurface ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 8, cpu_storage_type, m_hMaterialBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 9, cpu_storage_type, m_hPatchBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 10, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hVisibleBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 11, m_hszMaterialSamplers[nFiltering], rMaterialDisplacement ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 12, m_hTerrainSampler, rDisplacement ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 13, cpu_storage_type, m_hMaterialConfigBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 14, m_hszMapSamplers[nFiltering], rDerived ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 15, m_hszMapSamplers[3], rMiddleComposite ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 16, m_hTerrainSampler, rMiddleHeight ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 17, m_hTerrainSampler, rMiddleHeightRegionMap ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 18, m_hTerrainSampler, rFarHeight ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 19, m_hTerrainSampler, rFarColor ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 20, m_hszMaterialSamplers[nFiltering], rFarAlbedo ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 21, m_hszMaterialSamplers[1], rWorldNoise ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 22, m_hszMaterialSamplers[nFiltering], rMacroNoise ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 23, cpu_storage_type, m_hEditorOverlayBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 24, m_hszMapSamplers[1], rEditorBrush ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 25, m_hszMapSamplers[1], rTerrainShadowMask ) );
		const RID hVTAllocation = pSharedVTBindings && pSharedVTBindings->hAllocationTable.is_valid() ? pSharedVTBindings->hAllocationTable
		                                                                                              : m_hMaterialBuffer;
		const RID hVTPageTable =
		        pSharedVTBindings && pSharedVTBindings->hPageTable.is_valid() ? pSharedVTBindings->hPageTable : m_hRegionMapBuffer;
		uniforms.push_back(
		        Terrain3DRdBindingBuilder::single( T3D_TERRAIN_VT_ALLOCATION_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, hVTAllocation ) );
		uniforms.push_back(
		        Terrain3DRdBindingBuilder::single( T3D_TERRAIN_VT_PAGE_TABLE_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, hVTPageTable ) );
		for ( uint32_t channel = 0u; channel < T3D_SHARED_VT_CHANNEL_COUNT; ++channel ) {
			RID hPhysical = m_hFallbackArrayTexture;
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
			uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( ( int )( nBinding ), m_hszVtSamplers[nFiltering], hPhysical ) );
		}
		const RID hVTTailMetadata =
		        pSharedVTBindings && pSharedVTBindings->hTailMetadata.is_valid() ? pSharedVTBindings->hTailMetadata : m_hMaterialBuffer;
		const RID hVTMipLayout =
		        pSharedVTBindings && pSharedVTBindings->hMipLayoutTable.is_valid() ? pSharedVTBindings->hMipLayoutTable : m_hMaterialBuffer;
		uniforms.push_back(
		        Terrain3DRdBindingBuilder::single( T3D_TERRAIN_VT_TAIL_METADATA_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, hVTTailMetadata ) );
		for ( uint32_t channel = 0u; channel < T3D_SHARED_VT_CHANNEL_COUNT; ++channel ) {
			const RID hTail = pSharedVTBindings && pSharedVTBindings->hTail[channel].is_valid() ? pSharedVTBindings->hTail[channel]
			                                                                                    : m_hFallbackDisplacementTexture;
			uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture(
			        ( int )( T3D_TERRAIN_VT_TAIL_TEXTURE_BINDING_BASE + channel ), m_hszVtSamplers[nFiltering], hTail ) );
		}
		uniforms.push_back(
		        Terrain3DRdBindingBuilder::single( T3D_TERRAIN_VT_MIP_LAYOUT_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, hVTMipLayout ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 38, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hStripPartBuffer ) );
		const String name = String( "T3D.UniformSet.Raster." ) + String::num_int64( i );
		m_hszRasterUniformSets[i] = m_descriptorCache.getOrCreate(
		        { m_hszShaders[i], descriptor_key }, name, uniforms, m_hszShaders[i], 0u );
		if ( !m_hszRasterUniformSets[i].is_valid() ) {
			m_context.recordResourceCreationFailure( name, Terrain3DRdContext::RESOURCE_UNIFORM_SET );
			setError( "Failed to create Terrain3D raster uniform set" );
			return false;
		}
	}
	if ( m_bGpuCullingReady.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		TypedArray<Ref<RDUniform>> cull_uniforms;
		cull_uniforms.push_back( Terrain3DRdBindingBuilder::single( 0, frame_uniform_type, m_hFrameUniformBuffer ) );
		cull_uniforms.push_back( Terrain3DRdBindingBuilder::single( 9, cpu_storage_type, m_hPatchBuffer ) );
		cull_uniforms.push_back( Terrain3DRdBindingBuilder::single( 10, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hVisibleBuffer ) );
		cull_uniforms.push_back( Terrain3DRdBindingBuilder::single( 11, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hIndirectBuffer ) );
		cull_uniforms.push_back( Terrain3DRdBindingBuilder::single( 12, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hIndirectBuffer ) );
		m_hCullUniformSet = m_descriptorCache.getOrCreate(
		        { m_hCullShader, descriptor_key }, "T3D.UniformSet.Cull", cull_uniforms, m_hCullShader, 0u );
		if ( !m_hCullUniformSet.is_valid() ) {
			m_context.recordResourceCreationFailure( "T3D.UniformSet.Cull", Terrain3DRdContext::RESOURCE_UNIFORM_SET );
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
	m_context.freeResource( rState.m_hFramebuffer );
	rState = t3dRdViewState_t{};
}

bool Terrain3DRdClipmapRenderer::ensureViewState( const uint32_t nView,
        const RID &rColor,
        const RID &rDepth,
        const RenderingDevice::TextureSamples samples,
        const bool bColorWrite ) {
	if ( nView >= s_nMaxViews ) {
		return false;
	}
	t3dRdViewState_t &state = m_szViewStates[nView];
	if ( state.m_hFramebuffer.is_valid() && state.m_hColor == rColor && state.m_hDepth == rDepth && state.m_samples == samples
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
	state.m_hFramebuffer = rd->framebuffer_create( attachments, -1, 1u );
	const String framebuffer_name = String( "T3D.Framebuffer.View" ) + String::num_int64( nView );
	if ( !state.m_hFramebuffer.is_valid() ) {
		m_context.recordResourceCreationFailure( framebuffer_name, Terrain3DRdContext::RESOURCE_FRAMEBUFFER );
		setError( "Failed to create Terrain3D per-view framebuffer" );
		return false;
	}
	if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_TERRAIN ).adopt( rd, state.m_hFramebuffer, framebuffer_name, Terrain3DRdContext::RESOURCE_FRAMEBUFFER ) ) {
		return false;
	}
	Ref<RDPipelineRasterizationState> raster;
	raster.instantiate();
	raster->set_cull_mode( RenderingDevice::POLYGON_CULL_DISABLED );
	raster->set_front_face( RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE );
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count( samples );
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	depth->set_enable_depth_test( true );
	depth->set_enable_depth_write( true );
	depth->set_depth_compare_operator( RenderingDevice::COMPARE_OP_LESS_OR_EQUAL );
	Ref<RDPipelineColorBlendStateAttachment> blend_attachment;
	blend_attachment.instantiate();
	blend_attachment->set_enable_blend( false );
	blend_attachment->set_write_r( bColorWrite );
	blend_attachment->set_write_g( bColorWrite );
	blend_attachment->set_write_b( bColorWrite );
	blend_attachment->set_write_a( bColorWrite );
	Ref<RDPipelineColorBlendState> blend;
	blend.instantiate();
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> blend_attachments;
	blend_attachments.push_back( blend_attachment );
	blend->set_attachments( blend_attachments );
	const int64_t framebuffer_format = rd->framebuffer_get_format( state.m_hFramebuffer );
	for ( int i = 0; i < 3; ++i ) {
		const String pipeline_name = String( "T3D.Pipeline.View" ) + String::num_int64( nView ) + "." + String::num_int64( i );
		state.m_hszPipelines[i] = m_viewPipelines.getOrCreateRaster( nView * 3u + ( uint32_t )i,
		        pipeline_name, m_hszShaders[i],
		        framebuffer_format,
		        -1,
		        RenderingDevice::RENDER_PRIMITIVE_TRIANGLE_STRIPS,
		        raster,
		        multisample,
		        depth,
		        blend );
		if ( !state.m_hszPipelines[i].is_valid() ) {
			m_context.recordResourceCreationFailure( pipeline_name, Terrain3DRdContext::RESOURCE_RASTER_PIPELINE );
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

bool Terrain3DRdClipmapRenderer::initialize( const Ref<RDShaderSPIRV> &rNear,
        const Ref<RDShaderSPIRV> &rMiddle,
        const Ref<RDShaderSPIRV> &rFar,
        const Ref<RDShaderSPIRV> &rCull,
        const t3dRdForwardShaderBundle_t &rForwardShaders ) {
	t3dScopedLock_t state_lock( m_resourceStateMutex );
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
	m_bPersistentCpuBuffers = t3dRdPersistentMappingAvailable( persistent_rd ) && t3dRdSupportsDynamicBufferMetadata( rNear )
	        && t3dRdSupportsDynamicBufferMetadata( rMiddle ) && t3dRdSupportsDynamicBufferMetadata( rFar )
	        && ( rCull.is_null() || t3dRdSupportsDynamicBufferMetadata( rCull ) );
	if ( m_bPersistentCpuBuffers ) {
		for ( uint32_t path = 0u; path < T3D_TERRAIN_SHADER_PATH_COUNT && m_bPersistentCpuBuffers; ++path ) {
			for ( uint32_t tier = 0u; tier < 3u && m_bPersistentCpuBuffers; ++tier ) {
				for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
					if ( !t3dRdSupportsDynamicBufferMetadata( rForwardShaders.m_szTerrain[path][tier][variant] ) ) {
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
					t3dRdMarkDynamicBuffers( rForwardShaders.m_szTerrain[path][tier][variant], raster_dynamic_buffers );
				}
			}
		}
	}

	if ( !ensureShaders( rNear, rMiddle, rFar, rCull ) || !ensureForwardShaders( rForwardShaders ) || !ensureSamplers()
	        || !ensureFallbackTextures() || !ensureBuffers() ) {
		return false;
	}
	return true;
}

void Terrain3DRdClipmapRenderer::shutdown( void ) {
	t3dScopedLock_t state_lock( m_resourceStateMutex );
	for ( size_t iView = 0u; iView < T3D_ARRAY_COUNT( m_szViewStates ); iView++ ) {
		t3dRdViewState_t &state = ( m_szViewStates )[iView];
		freeViewState( state );
	}
	m_forwardLighting.shutdown();
	freeForwardPipelines();
	freeUniformSets();
	m_context.freeResource( m_hStripPartBuffer, sizeof( m_szStripParts ) );
	m_context.releaseFixedIndirect( Terrain3DRdContext::FIXED_INDIRECT_CLIPMAP, m_hIndirectBuffer );
	m_context.freeResource( m_hVisibleBuffer, sizeof( uint32_t ) * s_nRdDrawGroupCount * s_nMaxClipmapLevels );
	m_context.freeResource( m_hPatchBuffer, sizeof( t3dRdPatch_t ) * s_nMaxClipmapLevels );
	m_context.freeResource( m_hEditorOverlayBuffer, sizeof( t3dRdEditorOverlayConfig_t ) );
	m_context.freeResource( m_hMaterialConfigBuffer, sizeof( t3dRdMaterialConfig_t ) );
	m_context.freeResource( m_hMaterialBuffer, sizeof( t3dRdMaterialLayer_t ) * s_nRdMaterialLayerCount );
	m_context.freeResource( m_hRegionMapBuffer, m_nRegionMapCapacityBytes );
	m_context.freeResource( m_hFrameUniformBuffer, sizeof( t3dRdFrameUniforms_t ) );
	m_context.freeTextureResource( m_hFallbackDisplacementTexture, 4u * sizeof( float ) );
	m_context.freeTextureResource( m_hFallbackArrayTexture, 4u * sizeof( float ) );
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		m_context.freeResource( m_hszMaterialSamplers[i] );
		m_context.freeResource( m_hszMapSamplers[i] );
		m_context.freeResource( m_hszVtSamplers[i] );
	}
	m_context.freeResource( m_hTerrainSampler );
	m_cullPipelines.clear();
	m_context.freeResource( m_hCullShader );
	for ( uint32_t path = 0u; path < T3D_TERRAIN_SHADER_PATH_COUNT; ++path ) {
		for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
			for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
				m_context.freeResource( m_hszForwardShaders[path][tier][variant] );
			}
			for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT; ++variant ) {
				m_context.freeResource( m_hszForwardOneSunShaders[path][tier][variant] );
			}
		}
	}
	for ( size_t iShader = 0u; iShader < T3D_ARRAY_COUNT( m_hszShaders ); iShader++ ) {
		RID &shader = ( m_hszShaders )[iShader];
		m_context.freeResource( shader );
	}
	m_hStripPartBuffer = RID();
	m_hIndirectBuffer = RID();
	m_indexedIndirectSlice = {};
	m_stripIndirectSlice = {};
	m_hVisibleBuffer = RID();
	m_hPatchBuffer = RID();
	m_hEditorOverlayBuffer = RID();
	m_hMaterialConfigBuffer = RID();
	m_hMaterialBuffer = RID();
	m_hRegionMapBuffer = RID();
	m_hFrameUniformBuffer = RID();
	m_hFallbackDisplacementTexture = RID();
	m_hFallbackArrayTexture = RID();
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		m_hszMaterialSamplers[i] = RID();
		m_hszMapSamplers[i] = RID();
		m_hszVtSamplers[i] = RID();
	}
	m_hTerrainSampler = RID();
	m_hCullPipeline = RID();
	m_hCullShader = RID();
	for ( size_t iShader = 0u; iShader < T3D_ARRAY_COUNT( m_hszShaders ); iShader++ ) {
		RID &shader = ( m_hszShaders )[iShader];
		shader = RID();
	}
	for ( uint32_t path = 0u; path < T3D_TERRAIN_SHADER_PATH_COUNT; ++path ) {
		for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
			for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
				m_hszForwardShaders[path][tier][variant] = RID();
			}
			for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT; ++variant ) {
				m_hszForwardOneSunShaders[path][tier][variant] = RID();
			}
		}
	}
	m_nForwardPreparedScenePass = 0u;
	m_nForwardPreparedViewportKey = 0u;
	m_bForwardHistoryValid = false;
	m_nForwardPreparedSnapshotGeneration = 0u;
	m_forwardPreparedCullPassKind = T3D_RD_FORWARD_PASS_INVALID;
	m_bLastForwardPipelineKeyValid = false;
	m_nRegionMapCapacityBytes = 0u;
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
        const T3DTerrainStageASharedVTBindings *const pSharedVTBindings ) {
	if ( !pViewProjections || !pCameras || nViewCount == 0u || nViewCount > s_nRdForwardMaxViews ) {
		return false;
	}
	t3dRdFrameUniforms_t frame;
	t3dRdFrameUniformsInit( &frame );
	for ( uint32_t view = 0u; view < s_nRdForwardMaxViews; ++view ) {
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
	if ( !Terrain3DGpuUploadService( m_context, Terrain3DRdContext::OWNER_TERRAIN, T3D_GPU_HOT_FRAME )
	                        .writeWhole( m_hFrameUniformBuffer,
	                                sizeof( frame ),
	                                &frame,
	                                sizeof( frame ),
	                                m_frameUploadBytes,
	                                m_bPersistentCpuBuffers ) ) {
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
		const t3dGpuBufferSlice_t region_slice {
			m_hRegionMapBuffer, 0u, m_nRegionMapCapacityBytes, m_nRegionMapCapacityBytes, 1u, 0u
		};
		const bool bUploaded = Terrain3DGpuUploadService( m_context, Terrain3DRdContext::OWNER_TERRAIN, T3D_GPU_DIRTY_UPLOAD )
		                               .writePacked( region_slice,
		                                       0u,
		                                       rRegionMapBytes,
		                                       ( uint32_t )( rRegionMapBytes.size() ),
		                                       m_bPersistentCpuBuffers );
		if ( !bUploaded ) {
			return false;
		}
		m_nRegionMapGeneration = nRegionMapGeneration;
		m_bRegionMapUploadInitialized = true;
	}
	if ( !m_bMaterialUploadInitialized || rSnapshot.m_nMaterialGeneration != m_nMaterialGeneration ) {
		Terrain3DGpuUploadService uploads( m_context, Terrain3DRdContext::OWNER_TERRAIN, T3D_GPU_PUBLICATION );
		if ( !uploads.writeWhole( m_hMaterialBuffer,
		                sizeof( rSnapshot.m_szMaterials ),
		                rSnapshot.m_szMaterials,
		                sizeof( rSnapshot.m_szMaterials ),
		                m_materialUploadBytes,
		                m_bPersistentCpuBuffers )
		        || !uploads.writeWhole( m_hMaterialConfigBuffer,
		                sizeof( rSnapshot.m_materialConfig ),
		                &rSnapshot.m_materialConfig,
		                sizeof( rSnapshot.m_materialConfig ),
		                m_materialConfigUploadBytes,
		                m_bPersistentCpuBuffers ) ) {
			return false;
		}
		m_nMaterialGeneration = rSnapshot.m_nMaterialGeneration;
		m_bMaterialUploadInitialized = true;
	}
	if ( !m_bEditorOverlayUploadInitialized
	        || memcmp( &m_uploadedEditorOverlay, &rSnapshot.m_editorOverlayConfig, sizeof( rSnapshot.m_editorOverlayConfig ) ) != 0 ) {
		const bool bUploaded = Terrain3DGpuUploadService( m_context, Terrain3DRdContext::OWNER_TERRAIN, T3D_GPU_DIRTY_UPLOAD )
		                               .writeWhole( m_hEditorOverlayBuffer,
		                                       sizeof( rSnapshot.m_editorOverlayConfig ),
		                                       &rSnapshot.m_editorOverlayConfig,
		                                       sizeof( rSnapshot.m_editorOverlayConfig ),
		                                       m_editorOverlayUploadBytes,
		                                       m_bPersistentCpuBuffers );
		if ( !bUploaded ) {
			return false;
		}
		++m_nEditorOverlayUploads;
		m_uploadedEditorOverlay = rSnapshot.m_editorOverlayConfig;
		m_bEditorOverlayUploadInitialized = true;
	}
	if ( !m_bPatchUploadInitialized || rSnapshot.m_nGeneration != m_nPatchGeneration || rSnapshot.m_nPatchCount != m_nUploadedPatchCount ) {
		if ( rSnapshot.m_nPatchCount > s_nMaxClipmapLevels ) {
			return false;
		}
		if ( m_bPersistentCpuBuffers ) {
			const uint32_t nPatchBytes = rSnapshot.m_nPatchCount * sizeof( t3dRdPatch_t );
			if ( nPatchBytes > 0u
			        && !Terrain3DGpuUploadService( m_context, Terrain3DRdContext::OWNER_TERRAIN, T3D_GPU_DIRTY_UPLOAD )
			                    .writeWhole( m_hPatchBuffer,
			                            sizeof( t3dRdPatch_t ) * s_nMaxClipmapLevels,
			                            rSnapshot.m_szPatches,
			                            nPatchBytes,
			                            m_patchUploadBytes,
			                            true ) ) {
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
				const t3dGpuBufferSlice_t patch_slice { m_hPatchBuffer,
					0u,
					sizeof( t3dRdPatch_t ) * s_nMaxClipmapLevels,
					sizeof( t3dRdPatch_t ) * s_nMaxClipmapLevels,
					1u,
					0u };
				if ( !Terrain3DGpuUploadService( m_context, Terrain3DRdContext::OWNER_TERRAIN, T3D_GPU_DIRTY_UPLOAD )
				                    .write( patch_slice,
				                            first_dirty * sizeof( t3dRdPatch_t ),
				                            &rSnapshot.m_szPatches[first_dirty],
				                            dirty_bytes,
				                            m_patchUploadBytes,
				                            false ) ) {
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
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
		commands[group].m_nIndexCount = getStripPartCountForGroup( group );
		commands[group].m_nFirstInstance = group * s_nMaxClipmapLevels;
	}
	t3dRdCpuCullResult_t empty_result;
	t3dRdCpuCullResultReset( &empty_result );
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
		empty_result.m_szCommands[group].m_nIndexCount = getStripPartCountForGroup( group );
		empty_result.m_szCommands[group].m_nFirstInstance = group * s_nMaxClipmapLevels;
	}
	if ( !buildStripIndirectCommands( empty_result ) ) {
		return false;
	}
	t3dRdCullPushConstants_t constants;
	t3dRdCullPushConstantsInit( &constants );
	constants.m_nPatchCount = MIN( rSnapshot.m_nPatchCount, ( uint32_t )( s_nMaxClipmapLevels ) );
	constants.m_nLod0PartCount = m_szStripPartCounts[LOD0_MESH];
	constants.m_nOuterPartCount = m_szStripPartCounts[OUTER_RING_MESH];
	constants.m_nMaxPatches = s_nMaxClipmapLevels;
	constants.m_nStripCommandBase = m_stripIndirectSlice.m_nOffsetBytes / sizeof( t3dRdDrawIndirectCommand_t );
	constants.m_nIndexedCommandBase = m_indexedIndirectSlice.m_nOffsetBytes / sizeof( t3dRdIndexedIndirectCommand_t );
	memcpy( m_cullPushBytes.ptrw(), &constants, sizeof( constants ) );
	RenderingDevice *const rd = m_context.getDevice();
	Terrain3DGpuUploadService uploads( m_context, Terrain3DRdContext::OWNER_TERRAIN, T3D_GPU_INDIRECT );
	if ( !uploads.writePacked( m_indexedIndirectSlice, 0u, m_cpuIndirectBytes, s_nStripIndirectOffsetBytes )
	        || !uploads.writePacked( m_stripIndirectSlice, 0u, m_cpuStripIndirectBytes, s_nStripIndirectBytes ) ) {
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
	Terrain3DGpuUploadService uploads( m_context, Terrain3DRdContext::OWNER_TERRAIN, T3D_GPU_INDIRECT );
	if ( !uploads.writePackedWhole( m_hVisibleBuffer, m_cpuVisibleBytes ) ) {
		return false;
	}
	if ( !uploads.writePacked( m_indexedIndirectSlice, 0u, m_cpuIndirectBytes, s_nStripIndirectOffsetBytes )
	        || !uploads.writePacked( m_stripIndirectSlice, 0u, m_cpuStripIndirectBytes, s_nStripIndirectBytes ) ) {
		return false;
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::compareGpuCpuCulling(
        const t3dRdFrameSnapshot_t &rSnapshot, const Projection &rViewProjection, const t3dRdCpuCullResult_t &rExpected ) {
	const uint64_t comparison_index = m_nCullComparisons.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
	RenderingDevice *const rd = m_context.getDevice();
	const PackedByteArray gpu_visible = rd->buffer_get_data( m_hVisibleBuffer, 0u, ( uint32_t )( sizeof( rExpected.m_szVisibleIndices ) ) );
	const PackedByteArray gpu_indirect = rd->buffer_get_data( m_hIndirectBuffer, m_indexedIndirectSlice.m_nOffsetBytes,
	        ( uint32_t )( sizeof( rExpected.m_szCommands ) ) );
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
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
		const uint32_t expected_count = rExpected.m_szCommands[group].m_nInstanceCount;
		const uint32_t actual_count = actual.m_szCommands[group].m_nInstanceCount;
		if ( expected_count != actual_count ) {
			++m_nCullMismatches;
			return false;
		}
		bool expected_set[s_nMaxClipmapLevels];
		memset( expected_set, 0, sizeof( expected_set ) );
		bool actual_set[s_nMaxClipmapLevels];
		memset( actual_set, 0, sizeof( actual_set ) );
		for ( uint32_t slot = 0u; slot < expected_count; ++slot ) {
			expected_set[rExpected.m_szVisibleIndices[group * s_nMaxClipmapLevels + slot]] = true;
			actual_set[actual.m_szVisibleIndices[group * s_nMaxClipmapLevels + slot]] = true;
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
	if ( nView >= s_nMaxViews ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	t3dRdViewState_t &state = m_szViewStates[nView];
	const int64_t draw_list = rd->draw_list_begin( state.m_hFramebuffer, RenderingDevice::DRAW_DEFAULT_ALL );
	if ( draw_list == RenderingDevice::INVALID_ID ) {
		return false;
	}
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
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
		        m_hIndirectBuffer,
		        m_stripIndirectSlice.m_nOffsetBytes + group * T3D_CLIPMAP_STRIP_PART_CAPACITY * sizeof( t3dRdDrawIndirectCommand_t ),
		        part_count,
		        sizeof( t3dRdDrawIndirectCommand_t ) );
		m_context.recordRenderIndirectCommandsSubmitted( Terrain3DRdContext::OWNER_TERRAIN, part_count );
		++m_nIndirectDraws;
		m_nDrawsRendered += part_count;
	}
	rd->draw_list_end();
	return true;
}

bool Terrain3DRdClipmapRenderer::drawViewDirect( const uint32_t nView, const t3dRdCpuCullResult_t &rCullResult ) {
	if ( nView >= s_nMaxViews ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	t3dRdViewState_t &state = m_szViewStates[nView];
	const int64_t draw_list = rd->draw_list_begin( state.m_hFramebuffer, RenderingDevice::DRAW_DEFAULT_ALL );
	if ( draw_list == RenderingDevice::INVALID_ID ) {
		return false;
	}
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
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
				constants.m_nDirectPatchIndex = rCullResult.m_szVisibleIndices[group * s_nMaxClipmapLevels + slot];
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

bool Terrain3DRdClipmapRenderer::render( RenderData *const pRenderData,
        const t3dRdFrameSnapshot_t &rSnapshot,
        const PackedByteArray &rRegionMapBytes,
        const uint64_t nRegionMapGeneration,
        const t3dRdTerrainBindings_t &rBindings,
        const bool bColorWrite ) {
	const RID &rHeightTextureRd = rBindings.m_hHeightTextureRd;
	const RID &rDerivedTextureRd = rBindings.m_hDerivedTextureRd;
	const RID &rControlTextureRd = rBindings.m_hControlTextureRd;
	const RID &rColorTextureRd = rBindings.m_hColorTextureRd;
	const RID &rMiddleCompositeTextureRd = rBindings.m_hMiddleCompositeTextureRd;
	const RID &rTerrainShadowMaskTextureRd = rBindings.m_hTerrainShadowMaskTextureRd;
	const RID &rMiddleHeightTextureRd = rBindings.m_hMiddleHeightTextureRd;
	const RID &rMiddleHeightRegionMapTextureRd = rBindings.m_hMiddleHeightRegionMapTextureRd;
	const RID &rFarHeightTextureRd = rBindings.m_hFarHeightTextureRd;
	const RID &rFarColorTextureRd = rBindings.m_hFarColorTextureRd;
	const RID &rAlbedoTextureRd = rBindings.m_hAlbedoTextureRd;
	const RID &rFarAlbedoTextureRd = rBindings.m_hFarAlbedoTextureRd;
	const RID &rNormalTextureRd = rBindings.m_hNormalTextureRd;
	const RID &rSurfaceTextureRd = rBindings.m_hSurfaceTextureRd;
	const RID &rMaterialDisplacementTextureRd = rBindings.m_hMaterialDisplacementTextureRd;
	const RID &rDisplacementTextureRd = rBindings.m_hDisplacementTextureRd;
	const RID &rWorldNoiseTextureRd = rBindings.m_hWorldNoiseTextureRd;
	const RID &rMacroNoiseTextureRd = rBindings.m_hMacroNoiseTextureRd;
	const RID &rEditorBrushTextureRd = rBindings.m_hEditorBrushTextureRd;
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
	if ( view_count == 0u || view_count > s_nMaxViews ) {
		setError( "Unsupported Terrain3D RD view count" );
		return false;
	}
	RID height = rHeightTextureRd;
	RID control = rControlTextureRd;
	RID color_map = rColorTextureRd;
	RID derived = rDerivedTextureRd;
	RID middle_composite = rMiddleCompositeTextureRd;
	RID terrain_shadow_mask = rTerrainShadowMaskTextureRd;
	RID middle_height = rMiddleHeightTextureRd;
	RID middle_height_region_map = rMiddleHeightRegionMapTextureRd;
	RID far_height = rFarHeightTextureRd;
	RID far_color = rFarColorTextureRd;
	RID albedo = rAlbedoTextureRd;
	RID far_albedo = rFarAlbedoTextureRd;
	RID normal = rNormalTextureRd;
	RID surface = rSurfaceTextureRd;
	RID material_displacement = rMaterialDisplacementTextureRd;
	RID displacement = rDisplacementTextureRd;
	RID world_noise = rWorldNoiseTextureRd;
	RID macro_noise = rMacroNoiseTextureRd;
	RID editor_brush = rEditorBrushTextureRd;
	if ( !control.is_valid() ) {
		control = m_hFallbackArrayTexture;
	}
	if ( !color_map.is_valid() ) {
		color_map = m_hFallbackArrayTexture;
	}
	if ( !albedo.is_valid() ) {
		albedo = color_map.is_valid() ? color_map : m_hFallbackArrayTexture;
	}
	if ( !normal.is_valid() ) {
		normal = m_hFallbackArrayTexture;
	}
	if ( !surface.is_valid() ) {
		surface = m_hFallbackArrayTexture;
	}
	if ( !material_displacement.is_valid() ) {
		material_displacement = m_hFallbackArrayTexture;
	}
	if ( !displacement.is_valid() ) {
		displacement = m_hFallbackDisplacementTexture;
	}
	if ( !derived.is_valid() ) {
		derived = m_hFallbackArrayTexture;
	}
	if ( !middle_composite.is_valid() ) {
		middle_composite = m_hFallbackArrayTexture;
	}
	if ( !terrain_shadow_mask.is_valid() ) {
		terrain_shadow_mask = m_hFallbackArrayTexture;
	}
	if ( !middle_height.is_valid() ) {
		middle_height = m_hFallbackArrayTexture;
	}
	if ( !middle_height_region_map.is_valid() ) {
		middle_height_region_map = m_hFallbackDisplacementTexture;
	}
	if ( !far_height.is_valid() ) {
		far_height = m_hFallbackDisplacementTexture;
	}
	if ( !far_color.is_valid() ) {
		far_color = m_hFallbackDisplacementTexture;
	}
	if ( !far_albedo.is_valid() ) {
		far_albedo = albedo.is_valid() ? albedo : m_hFallbackArrayTexture;
	}
	if ( !world_noise.is_valid() ) {
		world_noise = m_hFallbackDisplacementTexture;
	}
	if ( !macro_noise.is_valid() ) {
		macro_noise = m_hFallbackDisplacementTexture;
	}
	if ( !editor_brush.is_valid() ) {
		editor_brush = m_hFallbackDisplacementTexture;
	}
	// Near height residency is allowed to converge after the render topology is
	// already live. The coordinator publishes zero physical region layers while
	// this RID is unavailable, so the shader cannot sample this one-layer neutral
	// descriptor and instead follows its Middle/Far/background height path. Do not
	// turn normal cache convergence into a renderer-fatal condition.
	if ( !height.is_valid() ) {
		height = m_hFallbackArrayTexture;
	}
	const uint64_t gpu_frame_id = m_nFramesRendered.load( T3D_MEMORY_ORDER_ACQUIRE ) + 1u;
	m_context.beginFrame( gpu_frame_id, rSnapshot.m_nPublicationGeneration );
	if ( !uploadSnapshotBuffers( rSnapshot, rRegionMapBytes, nRegionMapGeneration ) ) {
		setError( "Failed to upload Terrain3D RD frame data" );
		return false;
	}
	t3dRdTerrainBindings_t resolved_bindings = rBindings;
	resolved_bindings.m_hHeightTextureRd = height;
	resolved_bindings.m_hDerivedTextureRd = derived;
	resolved_bindings.m_hControlTextureRd = control;
	resolved_bindings.m_hColorTextureRd = color_map;
	resolved_bindings.m_hMiddleCompositeTextureRd = middle_composite;
	resolved_bindings.m_hTerrainShadowMaskTextureRd = terrain_shadow_mask;
	resolved_bindings.m_hMiddleHeightTextureRd = middle_height;
	resolved_bindings.m_hMiddleHeightRegionMapTextureRd = middle_height_region_map;
	resolved_bindings.m_hFarHeightTextureRd = far_height;
	resolved_bindings.m_hFarColorTextureRd = far_color;
	resolved_bindings.m_hAlbedoTextureRd = albedo;
	resolved_bindings.m_hFarAlbedoTextureRd = far_albedo;
	resolved_bindings.m_hNormalTextureRd = normal;
	resolved_bindings.m_hSurfaceTextureRd = surface;
	resolved_bindings.m_hMaterialDisplacementTextureRd = material_displacement;
	resolved_bindings.m_hDisplacementTextureRd = displacement;
	resolved_bindings.m_hWorldNoiseTextureRd = world_noise;
	resolved_bindings.m_hMacroNoiseTextureRd = macro_noise;
	resolved_bindings.m_hEditorBrushTextureRd = editor_brush;
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
		const Projection direct_views[s_nRdForwardMaxViews] = { view_projection, view_projection };
		const Vector3 direct_cameras[s_nRdForwardMaxViews] = { eye_position, eye_position };
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
	        bDirectDebug ? 0u : view_count * s_nRdDrawGroupCount, bDirectDebug || !bGpuCulling || !bCompareCulling ? 0u : view_count );
	m_context.recordSuccessfulPass( Terrain3DRdContext::PASS_TERRAIN_CLIPMAP, gpu_frame_id );
	m_context.markWarmupComplete();
	clearError();
	return true;
}

bool Terrain3DRdClipmapRenderer::prepareForwardPass( const t3dRdForwardPassContext_t &rContext,
        const t3dRdFrameSnapshot_t &rSnapshot,
        const PackedByteArray &rRegionMapBytes,
        const uint64_t nRegionMapGeneration,
        const t3dRdTerrainBindings_t &rBindings,
        const T3DTerrainStageASharedVTBindings *const pSharedVTBindings,
        const bool bTemporalHistoryValid,
        const bool bViewportReconfigured,
        const bool bOneSunLighting ) {
	const RID &rHeightTextureRd = rBindings.m_hHeightTextureRd;
	const RID &rDerivedTextureRd = rBindings.m_hDerivedTextureRd;
	const RID &rControlTextureRd = rBindings.m_hControlTextureRd;
	const RID &rColorTextureRd = rBindings.m_hColorTextureRd;
	const RID &rMiddleCompositeTextureRd = rBindings.m_hMiddleCompositeTextureRd;
	const RID &rTerrainShadowMaskTextureRd = rBindings.m_hTerrainShadowMaskTextureRd;
	const RID &rMiddleHeightTextureRd = rBindings.m_hMiddleHeightTextureRd;
	const RID &rMiddleHeightRegionMapTextureRd = rBindings.m_hMiddleHeightRegionMapTextureRd;
	const RID &rFarHeightTextureRd = rBindings.m_hFarHeightTextureRd;
	const RID &rFarColorTextureRd = rBindings.m_hFarColorTextureRd;
	const RID &rAlbedoTextureRd = rBindings.m_hAlbedoTextureRd;
	const RID &rFarAlbedoTextureRd = rBindings.m_hFarAlbedoTextureRd;
	const RID &rNormalTextureRd = rBindings.m_hNormalTextureRd;
	const RID &rSurfaceTextureRd = rBindings.m_hSurfaceTextureRd;
	const RID &rMaterialDisplacementTextureRd = rBindings.m_hMaterialDisplacementTextureRd;
	const RID &rDisplacementTextureRd = rBindings.m_hDisplacementTextureRd;
	const RID &rWorldNoiseTextureRd = rBindings.m_hWorldNoiseTextureRd;
	const RID &rMacroNoiseTextureRd = rBindings.m_hMacroNoiseTextureRd;
	const RID &rEditorBrushTextureRd = rBindings.m_hEditorBrushTextureRd;
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE ) {
		return false;
	}
	if ( !isReady() || !m_context.validateDevice() || rSnapshot.m_nPatchCount == 0u || rRegionMapBytes.is_empty() ) {
		return false;
	}
	if ( rContext.m_nViewCount == 0u || rContext.m_nViewCount > s_nRdForwardMaxViews ) {
		setError( "Terrain3D native terrain view count exceeds bridge bounds" );
		return false;
	}
	t3dRdForwardShaderVariant_t variant;
	if ( !selectForwardVariant( rContext, variant ) ) {
		return false;
	}
	const bool one_sun_lighting = bOneSunLighting && variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT;
	const t3dTerrainShaderPath_t shader_path = selectTerrainShaderPath( rSnapshot );
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

	RID height = rHeightTextureRd;
	RID control = rControlTextureRd;
	RID color_map = rColorTextureRd;
	RID derived = rDerivedTextureRd;
	RID middle_composite = rMiddleCompositeTextureRd;
	RID terrain_shadow_mask = rTerrainShadowMaskTextureRd;
	RID middle_height = rMiddleHeightTextureRd;
	RID middle_height_region_map = rMiddleHeightRegionMapTextureRd;
	RID far_height = rFarHeightTextureRd;
	RID far_color = rFarColorTextureRd;
	RID albedo = rAlbedoTextureRd;
	RID far_albedo = rFarAlbedoTextureRd;
	RID normal = rNormalTextureRd;
	RID surface = rSurfaceTextureRd;
	RID material_displacement = rMaterialDisplacementTextureRd;
	RID displacement = rDisplacementTextureRd;
	RID world_noise = rWorldNoiseTextureRd;
	RID macro_noise = rMacroNoiseTextureRd;
	RID editor_brush = rEditorBrushTextureRd;
	if ( !control.is_valid() ) {
		control = m_hFallbackArrayTexture;
	}
	if ( !color_map.is_valid() ) {
		color_map = m_hFallbackArrayTexture;
	}
	if ( !albedo.is_valid() ) {
		albedo = color_map.is_valid() ? color_map : m_hFallbackArrayTexture;
	}
	if ( !normal.is_valid() ) {
		normal = m_hFallbackArrayTexture;
	}
	if ( !surface.is_valid() ) {
		surface = m_hFallbackArrayTexture;
	}
	if ( !material_displacement.is_valid() ) {
		material_displacement = m_hFallbackArrayTexture;
	}
	if ( !displacement.is_valid() ) {
		displacement = m_hFallbackDisplacementTexture;
	}
	if ( !derived.is_valid() ) {
		derived = m_hFallbackArrayTexture;
	}
	if ( !middle_composite.is_valid() ) {
		middle_composite = m_hFallbackArrayTexture;
	}
	if ( !terrain_shadow_mask.is_valid() ) {
		terrain_shadow_mask = m_hFallbackArrayTexture;
	}
	if ( !middle_height.is_valid() ) {
		middle_height = m_hFallbackArrayTexture;
	}
	if ( !middle_height_region_map.is_valid() ) {
		middle_height_region_map = m_hFallbackDisplacementTexture;
	}
	if ( !far_height.is_valid() ) {
		far_height = m_hFallbackDisplacementTexture;
	}
	if ( !far_color.is_valid() ) {
		far_color = m_hFallbackDisplacementTexture;
	}
	if ( !far_albedo.is_valid() ) {
		far_albedo = albedo.is_valid() ? albedo : m_hFallbackArrayTexture;
	}
	if ( !world_noise.is_valid() ) {
		world_noise = m_hFallbackDisplacementTexture;
	}
	if ( !macro_noise.is_valid() ) {
		macro_noise = m_hFallbackDisplacementTexture;
	}
	if ( !editor_brush.is_valid() ) {
		editor_brush = m_hFallbackDisplacementTexture;
	}
	// Near height residency is allowed to converge after the render topology is
	// already live. The coordinator publishes zero physical region layers while
	// this RID is unavailable, so the shader cannot sample this one-layer neutral
	// descriptor and instead follows its Middle/Far/background height path. Do not
	// turn normal cache convergence into a renderer-fatal condition.
	if ( !height.is_valid() ) {
		height = m_hFallbackArrayTexture;
	}

	for ( uint32_t view = 0u; view < s_nRdForwardMaxViews; ++view ) {
		const uint32_t source = MIN( view, rContext.m_nViewCount - 1u );
		m_szForwardEyePositions[view] = rContext.m_cameraTransform.origin;
		if ( rContext.m_nViewCount > 1u ) {
			m_szForwardEyePositions[view] += rContext.m_cameraTransform.basis.xform( rContext.m_szEyeOffset[source] );
		}
	}
	m_bForwardHistoryValid = bTemporalHistoryValid;
	if ( m_nForwardPreparedScenePass != rContext.m_nScenePass || m_nForwardPreparedViewportKey != rContext.m_nViewportKey ) {
		m_context.beginFrame( rContext.m_nScenePass, rSnapshot.m_nPublicationGeneration );
	}
	t3dRdTerrainBindings_t resolved_bindings = rBindings;
	resolved_bindings.m_hHeightTextureRd = height;
	resolved_bindings.m_hDerivedTextureRd = derived;
	resolved_bindings.m_hControlTextureRd = control;
	resolved_bindings.m_hColorTextureRd = color_map;
	resolved_bindings.m_hMiddleCompositeTextureRd = middle_composite;
	resolved_bindings.m_hTerrainShadowMaskTextureRd = terrain_shadow_mask;
	resolved_bindings.m_hMiddleHeightTextureRd = middle_height;
	resolved_bindings.m_hMiddleHeightRegionMapTextureRd = middle_height_region_map;
	resolved_bindings.m_hFarHeightTextureRd = far_height;
	resolved_bindings.m_hFarColorTextureRd = far_color;
	resolved_bindings.m_hAlbedoTextureRd = albedo;
	resolved_bindings.m_hFarAlbedoTextureRd = far_albedo;
	resolved_bindings.m_hNormalTextureRd = normal;
	resolved_bindings.m_hSurfaceTextureRd = surface;
	resolved_bindings.m_hMaterialDisplacementTextureRd = material_displacement;
	resolved_bindings.m_hDisplacementTextureRd = displacement;
	resolved_bindings.m_hWorldNoiseTextureRd = world_noise;
	resolved_bindings.m_hMacroNoiseTextureRd = macro_noise;
	resolved_bindings.m_hEditorBrushTextureRd = editor_brush;
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
	if ( !selectForwardVariant( rContext, variant ) || variant != m_lastForwardPipelineKey.m_variant
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
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
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
					constants.m_nDirectPatchIndex = m_cpuCullResult.m_szVisibleIndices[group * s_nMaxClipmapLevels + slot];
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
			        m_hIndirectBuffer,
			        m_stripIndirectSlice.m_nOffsetBytes + group * T3D_CLIPMAP_STRIP_PART_CAPACITY * sizeof( t3dRdDrawIndirectCommand_t ),
			        part_count,
			        sizeof( t3dRdDrawIndirectCommand_t ) );
			m_context.recordRenderIndirectCommandsSubmitted( Terrain3DRdContext::OWNER_TERRAIN, part_count );
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
	m_context.recordSubmissionCounts( m_bDirectDrawDebugEnabled.load( T3D_MEMORY_ORDER_ACQUIRE ) ? 0u : s_nRdDrawGroupCount, 0u );
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
	rBindings.m_hFrameBuffer = m_hFrameUniformBuffer;
	rBindings.m_hRegionMapBuffer = m_hRegionMapBuffer;
	rBindings.m_hMaterialBuffer = m_hMaterialBuffer;
	rBindings.m_hPatchBuffer = m_hPatchBuffer;
	rBindings.m_hVisibleBuffer = m_hVisibleBuffer;
	rBindings.m_hMaterialConfigBuffer = m_hMaterialConfigBuffer;
	rBindings.m_hEditorOverlayBuffer = m_hEditorOverlayBuffer;
	rBindings.m_hTerrainSampler = m_hTerrainSampler;
	rBindings.m_hMaterialSampler = m_hszMaterialSamplers[0];
	rBindings.m_hMapSampler = m_hszMapSamplers[0];
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
	t3dScopedLock_t state_lock( m_resourceStateMutex );
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
	result["strip_indirect_bytes"] = ( int64_t )( sizeof( t3dRdDrawIndirectCommand_t ) * s_nStripCommandCapacity );
	result["indirect_arena_bytes"] = ( int64_t )( Terrain3DRdContext::FIXED_INDIRECT_ARENA_BYTES );
	result["indexed_indirect_offset_bytes"] = ( int64_t )( m_indexedIndirectSlice.m_nOffsetBytes );
	result["strip_indirect_offset_bytes"] = ( int64_t )( m_stripIndirectSlice.m_nOffsetBytes );
	result["gpu_culling"] = m_bGpuCullingReady.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["fallback_displacement_texture"] = m_hFallbackDisplacementTexture.is_valid();
	result["max_views"] = ( int64_t )( s_nMaxViews );
	result["forward_lighting"] = m_forwardLighting.getDiagnostics();
	result["last_error"] = getLastError();
	return result;
}
