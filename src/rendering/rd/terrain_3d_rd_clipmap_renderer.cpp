// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_clipmap_renderer.h"

#include "core/logger.h"

#include <cstring>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/render_scene_buffers.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>

CLASS_NAME_STATIC( "Terrain3DRdClipmapRenderer" );

static Ref<RDUniform>
t3dRdBufferUniform( const int nBinding, const RenderingDevice::UniformType type, const RID &rRid ) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_uniform_type( type );
	uniform->set_binding( nBinding );
	uniform->add_id( rRid );
	return uniform;
}

static Ref<RDUniform> t3dRdTextureUniform( const int nBinding, const RID &rSampler, const RID &rTexture ) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE );
	uniform->set_binding( nBinding );
	uniform->add_id( rSampler );
	uniform->add_id( rTexture );
	return uniform;
}

static bool t3dRdAdoptResource( Terrain3DRdContext &rContext,
		RenderingDevice *const pRd,
		RID &rRid,
		const String &rName,
		const Terrain3DRdContext::ResourceType type,
		const Terrain3DRdContext::MemoryCategory memoryCategory = Terrain3DRdContext::MEMORY_NONE,
		const uint64_t nBytes = 0u ) {
	if ( rContext.registerResource( rRid, rName, type, memoryCategory, nBytes ) ) {
		return true;
	}
	if ( pRd && rRid.is_valid() ) {
		pRd->free_rid( rRid );
	}
	rRid = RID();
	return false;
}

Terrain3DRdClipmapRenderer::Terrain3DRdClipmapRenderer( void ) :
		m_context( Terrain3DRdContext::getShared() ), m_forwardLighting( m_context ) {}

Terrain3DRdClipmapRenderer::~Terrain3DRdClipmapRenderer( void ) {
	shutdown();
}

void Terrain3DRdClipmapRenderer::setError( const String &rError ) {
	{
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_lastError = rError;
	}
	m_bHasError.store( true, std::memory_order_release );
	if ( !m_context.hasFailure() ) {
		m_context.recordFailure( Terrain3DRdContext::FAILURE_RENDER_PASS, rError );
	}
	LOG( ERROR, "[RD alpha] ", rError );
}

void Terrain3DRdClipmapRenderer::clearError( void ) {
	m_bHasError.store( false, std::memory_order_release );
}

String Terrain3DRdClipmapRenderer::getLastError( void ) const {
	if ( !m_bHasError.load( std::memory_order_acquire ) ) {
		return String();
	}
	std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
	return m_lastError;
}

bool Terrain3DRdClipmapRenderer::prepareTopologies( const t3dClipmapGeometry_t &rLod0,
		const t3dClipmapGeometry_t &rOuter ) {
	std::lock_guard<std::mutex> state_lock( m_resourceStateMutex );
	return m_szTopology[LOD0_MESH].buildCpu( rLod0, "Lod0" ) &&
			m_szTopology[OUTER_RING_MESH].buildCpu( rOuter, "Outer" );
}

bool Terrain3DRdClipmapRenderer::ensureDevice( void ) {
	if ( m_context.isReady() ) {
		return true;
	}
	if ( !m_context.initialize() ) {
		setError( "Global RenderingDevice is unavailable; Forward+ or Mobile is required" );
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
		m_hszShaders[i] = rd->shader_create_from_spirv( shaders[i] );
		if ( !m_hszShaders[i].is_valid() ) {
			m_context.recordResourceCreationFailure( names[i], Terrain3DRdContext::RESOURCE_SHADER );
			setError( "Failed to create Terrain3D RD raster shader" );
			return false;
		}
		if ( !t3dRdAdoptResource( m_context, rd, m_hszShaders[i], names[i], Terrain3DRdContext::RESOURCE_SHADER ) ) {
			setError( "Failed to register Terrain3D RD raster shader ownership" );
			return false;
		}
	}
	if ( !m_hCullShader.is_valid() && rCull.is_valid() ) {
		m_hCullShader = rd->shader_create_from_spirv( rCull );
		if ( !m_hCullShader.is_valid() ) {
			m_context.recordResourceCreationFailure( "T3D.Shader.Cull",
					Terrain3DRdContext::RESOURCE_SHADER,
					"Failed to create Terrain3D culling shader" );
			setError( "Failed to create Terrain3D culling shader" );
			return false;
		}
		if ( !t3dRdAdoptResource(
					 m_context, rd, m_hCullShader, "T3D.Shader.Cull", Terrain3DRdContext::RESOURCE_SHADER ) ) {
			return false;
		}
		m_hCullPipeline = rd->compute_pipeline_create( m_hCullShader );
		if ( !m_hCullPipeline.is_valid() ) {
			m_context.recordResourceCreationFailure( "T3D.Pipeline.Cull",
					Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE,
					"Failed to create Terrain3D culling pipeline" );
			setError( "Failed to create Terrain3D culling pipeline" );
			return false;
		}
		if ( !t3dRdAdoptResource( m_context,
					 rd,
					 m_hCullPipeline,
					 "T3D.Pipeline.Cull",
					 Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE ) ) {
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
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
			if ( m_hszForwardShaders[tier][variant].is_valid() ) {
				continue;
			}
			const Ref<RDShaderSPIRV> spirv = rShaders.m_szTerrain[tier][variant];
			if ( spirv.is_null() ) {
				setError( "Missing Terrain3D Forward+ terrain shader variant" );
				return false;
			}
			RID shader = rd->shader_create_from_spirv( spirv );
			const String name = String( "T3D.Forward.Shader.Terrain." ) + String::num_int64( tier ) + "." +
					String::num_int64( variant );
			if ( !shader.is_valid() ||
					!t3dRdAdoptResource( m_context, rd, shader, name, Terrain3DRdContext::RESOURCE_SHADER ) ) {
				setError( "Failed to create Terrain3D Forward+ terrain shader variant" );
				return false;
			}
			m_hszForwardShaders[tier][variant] = shader;
		}
	}
	m_forwardPipelines.reserve( 24u );
	return true;
}

void Terrain3DRdClipmapRenderer::freeForwardPipelines( void ) {
	for ( t3dRdForwardPipelineEntry_t &entry : m_forwardPipelines ) {
		for ( RID &pipeline : entry.m_hszPipelines ) {
			m_context.freeResource( pipeline );
		}
	}
	m_forwardPipelines.clear();
	m_bLastForwardPipelineKeyValid = false;
}

bool Terrain3DRdClipmapRenderer::selectForwardVariant( const t3dRdForwardPassContext_t &rContext,
		t3dRdForwardShaderVariant_t &rVariant ) const {
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
	if ( ( rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION ) ||
			rContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_COLOR ) {
		return false;
	}
	const bool specular = ( rContext.m_nColorPassFlags & ( 1u << 1u ) ) != 0u;
	const bool motion =
			rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || ( rContext.m_nColorPassFlags & ( 1u << 3u ) ) != 0u;
	const bool radiance_array = ( rContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY ) != 0u;
	if ( radiance_array ) {
		rVariant = specular
				? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION_RADIANCE_ARRAY : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_RADIANCE_ARRAY )
				: ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION_RADIANCE_ARRAY : T3D_RD_FORWARD_SHADER_COLOR_RADIANCE_ARRAY );
	} else {
		rVariant = specular
				? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR )
				: ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION : T3D_RD_FORWARD_SHADER_COLOR );
	}
	return true;
}

const Terrain3DRdClipmapRenderer::t3dRdForwardPipelineEntry_t *Terrain3DRdClipmapRenderer::findForwardPipeline(
		const t3dRdForwardPipelineKey_t &rKey ) const {
	for ( const t3dRdForwardPipelineEntry_t &entry : m_forwardPipelines ) {
		if ( entry.m_key == rKey ) {
			return &entry;
		}
	}
	return nullptr;
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
			if ( !enabled || limit <= 0 || patch.m_nLod >= (uint32_t)( limit ) ) {
				continue;
			}
			rResult.m_szVisibleIndices[group * s_nMaxClipmapLevels + write++] = patch_index;
			++visible_count;
		}
		command.m_nInstanceCount = write;
	}
	rResult.m_nVisibleCount = visible_count;
}

bool Terrain3DRdClipmapRenderer::ensureForwardPipelines( const t3dRdForwardPassContext_t &rContext,
		const t3dRdForwardShaderVariant_t variant ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || rContext.m_nFramebufferFormat < 0 ) {
		return false;
	}
	t3dRdForwardPipelineKey_t key;
	key.m_nFramebufferFormat = rContext.m_nFramebufferFormat;
	key.m_variant = variant;
	key.m_passKind = rContext.m_passKind;
	key.m_samples = rd->framebuffer_format_get_texture_samples( rContext.m_nFramebufferFormat, 0u );
	if ( ( 1u << (uint32_t)( key.m_samples ) ) != rContext.m_nSampleCount ) {
		setError( "Terrain3D Forward+ sample-count metadata does not match native framebuffer format" );
		return false;
	}
	key.m_nViewCount = rContext.m_nViewCount;
	key.m_bReverseCull = rContext.m_bReverseCull;
	key.m_bDepthPrepassActive = ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ||
										rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) &&
			rContext.m_bDepthPrepassActive;
	if ( findForwardPipeline( key ) ) {
		m_lastForwardPipelineKey = key;
		m_bLastForwardPipelineKeyValid = true;
		return true;
	}
	if ( m_forwardPipelines.size() >= 64u ) {
		freeForwardPipelines();
	}

	t3dRdForwardPipelineEntry_t entry;
	entry.m_key = key;
	Ref<RDPipelineRasterizationState> raster;
	raster.instantiate();
	raster->set_cull_mode( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW
					? ( rContext.m_bReverseCull ? RenderingDevice::POLYGON_CULL_FRONT
												: RenderingDevice::POLYGON_CULL_BACK )
					: RenderingDevice::POLYGON_CULL_DISABLED );
	raster->set_front_face( RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE );
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count( key.m_samples );
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	depth->set_enable_depth_test( true );
	depth->set_enable_depth_write( !key.m_bDepthPrepassActive );
	depth->set_depth_compare_operator( key.m_bDepthPrepassActive ? RenderingDevice::COMPARE_OP_EQUAL
																 : RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL );
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
		entry.m_hszPipelines[tier] = rd->render_pipeline_create( m_hszForwardShaders[tier][variant],
				rContext.m_nFramebufferFormat,
				m_nVertexFormat,
				RenderingDevice::RENDER_PRIMITIVE_TRIANGLES,
				raster,
				multisample,
				depth,
				blend );
		const String name = String( "T3D.Forward.Pipeline.Terrain." ) + String::num_int64( variant ) + "." +
				String::num_int64( tier );
		if ( !entry.m_hszPipelines[tier].is_valid() ||
				!t3dRdAdoptResource( m_context,
						rd,
						entry.m_hszPipelines[tier],
						name,
						Terrain3DRdContext::RESOURCE_RASTER_PIPELINE ) ) {
			for ( RID &pipeline : entry.m_hszPipelines ) {
				m_context.freeResource( pipeline );
			}
			m_context.endResourceRebuild();
			setError( "Failed to create framebuffer-compatible Terrain3D Forward+ terrain pipeline" );
			return false;
		}
	}
	m_context.endResourceRebuild();
	m_forwardPipelines.push_back( entry );
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
		if ( !t3dRdAdoptResource(
					 m_context, rd, m_hTerrainSampler, "T3D.Sampler.Terrain", Terrain3DRdContext::RESOURCE_SAMPLER ) ) {
			return false;
		}
	}
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		const bool bNearest = i >= 2u;
		const bool bAnisotropic = i == 0u || i == 2u;
		if ( !m_hszMaterialSamplers[i].is_valid() ) {
			Ref<RDSamplerState> state;
			state.instantiate();
			state->set_mag_filter(
					bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			state->set_min_filter(
					bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			state->set_mip_filter(
					bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
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
			if ( !t3dRdAdoptResource(
						 m_context, rd, m_hszMaterialSamplers[i], name, Terrain3DRdContext::RESOURCE_SAMPLER ) ) {
				return false;
			}
		}
		if ( !m_hszMapSamplers[i].is_valid() ) {
			Ref<RDSamplerState> map_state;
			map_state.instantiate();
			map_state->set_mag_filter(
					bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			map_state->set_min_filter(
					bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
			map_state->set_mip_filter(
					bNearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
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
			if ( !t3dRdAdoptResource(
						 m_context, rd, m_hszMapSamplers[i], map_name, Terrain3DRdContext::RESOURCE_SAMPLER ) ) {
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
	format2d->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT );
	Ref<RDTextureView> view;
	view.instantiate();
	PackedByteArray texel;
	texel.resize( 4 * sizeof( float ) );
	const float neutral[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
	std::memcpy( texel.ptrw(), neutral, sizeof( neutral ) );
	TypedArray<PackedByteArray> initial_data;
	initial_data.push_back( texel );
	m_hFallbackDisplacementTexture = rd->texture_create( format2d, view, initial_data );
	if ( !m_hFallbackDisplacementTexture.is_valid() ) {
		m_context.recordResourceCreationFailure(
				"T3D.Texture.DisplacementFallback", Terrain3DRdContext::RESOURCE_TEXTURE );
		setError( "Failed to create neutral Terrain3D displacement texture" );
		return false;
	}
	if ( !t3dRdAdoptResource( m_context,
				 rd,
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
	format_array->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT );
	m_hFallbackArrayTexture = rd->texture_create( format_array, view, initial_data );
	if ( !m_hFallbackArrayTexture.is_valid() ) {
		m_context.recordResourceCreationFailure( "T3D.Texture.ArrayFallback", Terrain3DRdContext::RESOURCE_TEXTURE );
		setError( "Failed to create neutral Terrain3D array fallback texture" );
		return false;
	}
	if ( !t3dRdAdoptResource( m_context,
				 rd,
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
	const uint32_t frame_bytes = (uint32_t)( sizeof( t3dRdFrameUniforms_t ) );
	const uint32_t material_bytes = (uint32_t)( sizeof( t3dRdMaterialLayer_t ) * s_nRdMaterialLayerCount );
	const uint32_t material_config_bytes = (uint32_t)( sizeof( t3dRdMaterialConfig_t ) );
	const uint32_t editor_overlay_bytes = (uint32_t)( sizeof( t3dRdEditorOverlayConfig_t ) );
	const uint32_t patch_bytes = (uint32_t)( sizeof( t3dRdPatch_t ) * s_nMaxClipmapLevels );
	const uint32_t visible_bytes = (uint32_t)( sizeof( uint32_t ) * s_nRdDrawGroupCount * s_nMaxClipmapLevels );
	const uint32_t indirect_bytes = (uint32_t)( sizeof( t3dRdIndexedIndirectCommand_t ) * s_nRdDrawGroupCount );
	if ( !m_hFrameUniformBuffer.is_valid() ) {
		m_hFrameUniformBuffer = rd->uniform_buffer_create( frame_bytes );
		if ( !m_hFrameUniformBuffer.is_valid() ) {
			m_context.recordResourceCreationFailure( "T3D.Frame.UBO", Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER );
			return false;
		}
		if ( !t3dRdAdoptResource( m_context,
					 rd,
					 m_hFrameUniformBuffer,
					 "T3D.Frame.UBO",
					 Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER,
					 Terrain3DRdContext::MEMORY_UNIFORM,
					 frame_bytes ) ) {
			return false;
		}
	}
	if ( !m_hMaterialBuffer.is_valid() ) {
		m_hMaterialBuffer = rd->storage_buffer_create( material_bytes );
		if ( !m_hMaterialBuffer.is_valid() ) {
			m_context.recordResourceCreationFailure( "T3D.Materials", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER );
			return false;
		}
		if ( !t3dRdAdoptResource( m_context,
					 rd,
					 m_hMaterialBuffer,
					 "T3D.Materials",
					 Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
					 Terrain3DRdContext::MEMORY_MATERIAL_TABLE,
					 material_bytes ) ) {
			return false;
		}
	}
	if ( !m_hMaterialConfigBuffer.is_valid() ) {
		m_hMaterialConfigBuffer = rd->storage_buffer_create( material_config_bytes );
		if ( !m_hMaterialConfigBuffer.is_valid() ) {
			m_context.recordResourceCreationFailure(
					"T3D.MaterialConfig", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER );
			return false;
		}
		if ( !t3dRdAdoptResource( m_context,
					 rd,
					 m_hMaterialConfigBuffer,
					 "T3D.MaterialConfig",
					 Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
					 Terrain3DRdContext::MEMORY_MATERIAL_TABLE,
					 material_config_bytes ) ) {
			return false;
		}
	}
	if ( !m_hEditorOverlayBuffer.is_valid() ) {
		m_hEditorOverlayBuffer = rd->storage_buffer_create( editor_overlay_bytes );
		if ( !m_hEditorOverlayBuffer.is_valid() ) {
			m_context.recordResourceCreationFailure( "T3D.EditorOverlay", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER );
			return false;
		}
		if ( !t3dRdAdoptResource( m_context,
					 rd,
					 m_hEditorOverlayBuffer,
					 "T3D.EditorOverlay",
					 Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
					 Terrain3DRdContext::MEMORY_MATERIAL_TABLE,
					 editor_overlay_bytes ) ) {
			return false;
		}
	}
	if ( !m_hPatchBuffer.is_valid() ) {
		m_hPatchBuffer = rd->storage_buffer_create( patch_bytes );
		if ( !m_hPatchBuffer.is_valid() ) {
			m_context.recordResourceCreationFailure(
					"T3D.Clipmap.Patches", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER );
			return false;
		}
		if ( !t3dRdAdoptResource( m_context,
					 rd,
					 m_hPatchBuffer,
					 "T3D.Clipmap.Patches",
					 Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
					 Terrain3DRdContext::MEMORY_PATCH,
					 patch_bytes ) ) {
			return false;
		}
	}
	if ( !m_hVisibleBuffer.is_valid() ) {
		m_hVisibleBuffer = rd->storage_buffer_create( visible_bytes );
		if ( !m_hVisibleBuffer.is_valid() ) {
			m_context.recordResourceCreationFailure(
					"T3D.Clipmap.Visible", Terrain3DRdContext::RESOURCE_CULLING_BUFFER );
			return false;
		}
		if ( !t3dRdAdoptResource( m_context,
					 rd,
					 m_hVisibleBuffer,
					 "T3D.Clipmap.Visible",
					 Terrain3DRdContext::RESOURCE_CULLING_BUFFER,
					 Terrain3DRdContext::MEMORY_SCRATCH,
					 visible_bytes ) ) {
			return false;
		}
	}
	if ( !m_hIndirectBuffer.is_valid() ) {
		m_hIndirectBuffer = rd->storage_buffer_create(
				indirect_bytes, PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT );
		if ( !m_hIndirectBuffer.is_valid() ) {
			m_context.recordResourceCreationFailure(
					"T3D.Clipmap.Indirect", Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER );
			return false;
		}
		if ( !t3dRdAdoptResource( m_context,
					 rd,
					 m_hIndirectBuffer,
					 "T3D.Clipmap.Indirect",
					 Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER,
					 Terrain3DRdContext::MEMORY_INDIRECT,
					 indirect_bytes ) ) {
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
	m_hRegionMapBuffer = m_context.getDevice()->storage_buffer_create( m_nRegionMapCapacityBytes );
	if ( !m_hRegionMapBuffer.is_valid() ) {
		m_nRegionMapCapacityBytes = 0u;
		m_context.recordResourceCreationFailure( "T3D.RegionMap", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER );
		setError( "Failed to allocate RD region page-table buffer" );
		return false;
	}
	if ( !t3dRdAdoptResource( m_context,
				 m_context.getDevice(),
				 m_hRegionMapBuffer,
				 "T3D.RegionMap",
				 Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
				 Terrain3DRdContext::MEMORY_STORAGE,
				 m_nRegionMapCapacityBytes ) ) {
		m_nRegionMapCapacityBytes = 0u;
		return false;
	}
	m_nRegionMapGeneration = 0u;
	return true;
}

void Terrain3DRdClipmapRenderer::freeUniformSets( void ) {
	for ( RID &rid : m_hszRasterUniformSets ) {
		m_context.freeResource( rid );
		rid = RID();
	}
	m_context.freeResource( m_hCullUniformSet );
	m_hCullUniformSet = RID();
	m_hBoundHeightTexture = RID();
	m_hBoundDerivedTexture = RID();
	m_hBoundControlTexture = RID();
	m_hBoundColorTexture = RID();
	m_hBoundMiddleCompositeTexture = RID();
	m_hBoundTerrainShadowMaskTexture = RID();
	m_hBoundMiddleHeightTexture = RID();
	m_hBoundMiddleHeightRegionMapTexture = RID();
	m_hBoundFarHeightTexture = RID();
	m_hBoundFarColorTexture = RID();
	m_hBoundAlbedoTexture = RID();
	m_hBoundFarAlbedoTexture = RID();
	m_hBoundNormalTexture = RID();
	m_hBoundSurfaceTexture = RID();
	m_hBoundDisplacementTexture = RID();
	m_hBoundWorldNoiseTexture = RID();
	m_hBoundMacroNoiseTexture = RID();
	m_hBoundEditorBrushTexture = RID();
	for ( uint32_t i = 0u; i < 3u; ++i ) {
		m_nszBoundTierFiltering[i] = 0xFFFFFFFFu;
	}
}

bool Terrain3DRdClipmapRenderer::ensureUniformSets( const RID &rHeight,
		const RID &rDerived,
		const RID &rControl,
		const RID &rColor,
		const RID &rMiddleComposite,
		const RID &rTerrainShadowMask,
		const RID &rMiddleHeight,
		const RID &rMiddleHeightRegionMap,
		const RID &rFarHeight,
		const RID &rFarColor,
		const RID &rAlbedo,
		const RID &rFarAlbedo,
		const RID &rNormal,
		const RID &rSurface,
		const RID &rDisplacement,
		const RID &rWorldNoise,
		const RID &rMacroNoise,
		const RID &rEditorBrush,
		const t3dRdMaterialConfig_t &rMaterialConfig ) {
	if ( m_hszRasterUniformSets[0].is_valid() && m_hszRasterUniformSets[1].is_valid() &&
			m_hszRasterUniformSets[2].is_valid() &&
			( !m_bGpuCullingReady.load( std::memory_order_acquire ) || m_hCullUniformSet.is_valid() ) &&
			m_hBoundHeightTexture == rHeight && m_hBoundDerivedTexture == rDerived &&
			m_hBoundControlTexture == rControl && m_hBoundColorTexture == rColor &&
			m_hBoundMiddleCompositeTexture == rMiddleComposite &&
			m_hBoundTerrainShadowMaskTexture == rTerrainShadowMask && m_hBoundMiddleHeightTexture == rMiddleHeight &&
			m_hBoundMiddleHeightRegionMapTexture == rMiddleHeightRegionMap && m_hBoundFarHeightTexture == rFarHeight &&
			m_hBoundFarColorTexture == rFarColor && m_hBoundAlbedoTexture == rAlbedo &&
			m_hBoundFarAlbedoTexture == rFarAlbedo && m_hBoundNormalTexture == rNormal &&
			m_hBoundSurfaceTexture == rSurface && m_hBoundDisplacementTexture == rDisplacement &&
			m_hBoundWorldNoiseTexture == rWorldNoise && m_hBoundMacroNoiseTexture == rMacroNoise &&
			m_hBoundEditorBrushTexture == rEditorBrush &&
			m_nszBoundTierFiltering[0] == rMaterialConfig.m_szTier[0].m_nFiltering &&
			m_nszBoundTierFiltering[1] == rMaterialConfig.m_szTier[1].m_nFiltering &&
			m_nszBoundTierFiltering[2] == rMaterialConfig.m_szTier[2].m_nFiltering ) {
		return true;
	}
	freeUniformSets();
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !m_hRegionMapBuffer.is_valid() ) {
		return false;
	}
	for ( int i = 0; i < 3; ++i ) {
		const uint32_t nFiltering = MIN( rMaterialConfig.m_szTier[i].m_nFiltering, 3u );
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back(
				t3dRdBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameUniformBuffer ) );
		uniforms.push_back( t3dRdBufferUniform( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hRegionMapBuffer ) );
		uniforms.push_back( t3dRdTextureUniform( 2, m_hTerrainSampler, rHeight ) );
		uniforms.push_back( t3dRdTextureUniform( 3, m_hTerrainSampler, rControl ) );
		uniforms.push_back( t3dRdTextureUniform( 4, m_hszMapSamplers[nFiltering], rColor ) );
		uniforms.push_back( t3dRdTextureUniform( 5, m_hszMaterialSamplers[nFiltering], rAlbedo ) );
		uniforms.push_back( t3dRdTextureUniform( 6, m_hszMaterialSamplers[nFiltering], rNormal ) );
		uniforms.push_back( t3dRdTextureUniform( 7, m_hszMaterialSamplers[nFiltering], rSurface ) );
		uniforms.push_back( t3dRdBufferUniform( 8, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hMaterialBuffer ) );
		uniforms.push_back( t3dRdBufferUniform( 9, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hPatchBuffer ) );
		uniforms.push_back( t3dRdBufferUniform( 10, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hVisibleBuffer ) );
		uniforms.push_back( t3dRdTextureUniform( 12, m_hTerrainSampler, rDisplacement ) );
		uniforms.push_back(
				t3dRdBufferUniform( 13, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hMaterialConfigBuffer ) );
		uniforms.push_back( t3dRdTextureUniform( 14, m_hszMapSamplers[nFiltering], rDerived ) );
		uniforms.push_back( t3dRdTextureUniform( 15, m_hszMapSamplers[3], rMiddleComposite ) );
		uniforms.push_back( t3dRdTextureUniform( 16, m_hTerrainSampler, rMiddleHeight ) );
		uniforms.push_back( t3dRdTextureUniform( 17, m_hTerrainSampler, rMiddleHeightRegionMap ) );
		uniforms.push_back( t3dRdTextureUniform( 18, m_hTerrainSampler, rFarHeight ) );
		uniforms.push_back( t3dRdTextureUniform( 19, m_hTerrainSampler, rFarColor ) );
		uniforms.push_back( t3dRdTextureUniform( 20, m_hszMaterialSamplers[nFiltering], rFarAlbedo ) );
		uniforms.push_back( t3dRdTextureUniform( 21, m_hszMaterialSamplers[1], rWorldNoise ) );
		uniforms.push_back( t3dRdTextureUniform( 22, m_hszMaterialSamplers[nFiltering], rMacroNoise ) );
		uniforms.push_back(
				t3dRdBufferUniform( 23, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hEditorOverlayBuffer ) );
		uniforms.push_back( t3dRdTextureUniform( 24, m_hszMapSamplers[1], rEditorBrush ) );
		uniforms.push_back( t3dRdTextureUniform( 25, m_hszMapSamplers[1], rTerrainShadowMask ) );
		m_hszRasterUniformSets[i] = rd->uniform_set_create( uniforms, m_hszShaders[i], 0u );
		const String name = String( "T3D.UniformSet.Raster." ) + String::num_int64( i );
		if ( !m_hszRasterUniformSets[i].is_valid() ) {
			m_context.recordResourceCreationFailure( name, Terrain3DRdContext::RESOURCE_UNIFORM_SET );
			setError( "Failed to create Terrain3D raster uniform set" );
			return false;
		}
		if ( !t3dRdAdoptResource(
					 m_context, rd, m_hszRasterUniformSets[i], name, Terrain3DRdContext::RESOURCE_UNIFORM_SET ) ) {
			return false;
		}
		m_nszBoundTierFiltering[i] = rMaterialConfig.m_szTier[i].m_nFiltering;
	}
	if ( m_bGpuCullingReady.load( std::memory_order_acquire ) ) {
		TypedArray<Ref<RDUniform>> cull_uniforms;
		cull_uniforms.push_back(
				t3dRdBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameUniformBuffer ) );
		cull_uniforms.push_back(
				t3dRdBufferUniform( 9, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hPatchBuffer ) );
		cull_uniforms.push_back(
				t3dRdBufferUniform( 10, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hVisibleBuffer ) );
		cull_uniforms.push_back(
				t3dRdBufferUniform( 11, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hIndirectBuffer ) );
		m_hCullUniformSet = rd->uniform_set_create( cull_uniforms, m_hCullShader, 0u );
		if ( !m_hCullUniformSet.is_valid() ) {
			m_context.recordResourceCreationFailure( "T3D.UniformSet.Cull", Terrain3DRdContext::RESOURCE_UNIFORM_SET );
			setError( "Failed to create Terrain3D culling uniform set" );
			return false;
		}
		if ( !t3dRdAdoptResource( m_context,
					 rd,
					 m_hCullUniformSet,
					 "T3D.UniformSet.Cull",
					 Terrain3DRdContext::RESOURCE_UNIFORM_SET ) ) {
			return false;
		}
	}
	m_hBoundHeightTexture = rHeight;
	m_hBoundDerivedTexture = rDerived;
	m_hBoundControlTexture = rControl;
	m_hBoundColorTexture = rColor;
	m_hBoundMiddleCompositeTexture = rMiddleComposite;
	m_hBoundTerrainShadowMaskTexture = rTerrainShadowMask;
	m_hBoundMiddleHeightTexture = rMiddleHeight;
	m_hBoundMiddleHeightRegionMapTexture = rMiddleHeightRegionMap;
	m_hBoundFarHeightTexture = rFarHeight;
	m_hBoundFarColorTexture = rFarColor;
	m_hBoundAlbedoTexture = rAlbedo;
	m_hBoundFarAlbedoTexture = rFarAlbedo;
	m_hBoundNormalTexture = rNormal;
	m_hBoundSurfaceTexture = rSurface;
	m_hBoundDisplacementTexture = rDisplacement;
	m_hBoundWorldNoiseTexture = rWorldNoise;
	m_hBoundMacroNoiseTexture = rMacroNoise;
	m_hBoundEditorBrushTexture = rEditorBrush;
	return true;
}

void Terrain3DRdClipmapRenderer::freeViewState( t3dRdViewState_t &rState ) {
	for ( RID &pipeline : rState.m_hszPipelines ) {
		m_context.freeResource( pipeline );
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
	if ( state.m_hFramebuffer.is_valid() && state.m_hColor == rColor && state.m_hDepth == rDepth &&
			state.m_samples == samples && state.m_bColorWrite == bColorWrite ) {
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
	if ( !t3dRdAdoptResource(
				 m_context, rd, state.m_hFramebuffer, framebuffer_name, Terrain3DRdContext::RESOURCE_FRAMEBUFFER ) ) {
		return false;
	}
	Ref<RDPipelineRasterizationState> raster;
	raster.instantiate();
	raster->set_cull_mode( RenderingDevice::POLYGON_CULL_DISABLED );
	raster->set_front_face( RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE );
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
		state.m_hszPipelines[i] = rd->render_pipeline_create( m_hszShaders[i],
				framebuffer_format,
				m_nVertexFormat,
				RenderingDevice::RENDER_PRIMITIVE_TRIANGLES,
				raster,
				multisample,
				depth,
				blend );
		const String pipeline_name =
				String( "T3D.Pipeline.View" ) + String::num_int64( nView ) + "." + String::num_int64( i );
		if ( !state.m_hszPipelines[i].is_valid() ) {
			m_context.recordResourceCreationFailure( pipeline_name, Terrain3DRdContext::RESOURCE_RASTER_PIPELINE );
			setError( "Failed to create Terrain3D raster pipeline" );
			freeViewState( state );
			return false;
		}
		if ( !t3dRdAdoptResource( m_context,
					 rd,
					 state.m_hszPipelines[i],
					 pipeline_name,
					 Terrain3DRdContext::RESOURCE_RASTER_PIPELINE ) ) {
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
	std::lock_guard<std::mutex> state_lock( m_resourceStateMutex );
	if ( isReady() ) {
		return true;
	}
	if ( !ensureDevice() || !ensureShaders( rNear, rMiddle, rFar, rCull ) || !ensureForwardShaders( rForwardShaders ) ||
			!ensureSamplers() || !ensureFallbackTextures() || !ensureBuffers() ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	TypedArray<Ref<RDVertexAttribute>> attributes;
	Ref<RDVertexAttribute> grid;
	grid.instantiate();
	grid->set_location( 0u );
	grid->set_binding( 0u );
	grid->set_format( RenderingDevice::DATA_FORMAT_R16G16_SINT );
	grid->set_offset( 0u );
	grid->set_stride( sizeof( t3dRdClipmapVertex_t ) );
	grid->set_frequency( RenderingDevice::VERTEX_FREQUENCY_VERTEX );
	attributes.push_back( grid );
	Ref<RDVertexAttribute> meta;
	meta.instantiate();
	meta->set_location( 1u );
	meta->set_binding( 0u );
	meta->set_format( RenderingDevice::DATA_FORMAT_R16G16_UINT );
	meta->set_offset( 4u );
	meta->set_stride( sizeof( t3dRdClipmapVertex_t ) );
	meta->set_frequency( RenderingDevice::VERTEX_FREQUENCY_VERTEX );
	attributes.push_back( meta );
	m_nVertexFormat = rd->vertex_format_create( attributes );
	if ( m_nVertexFormat == RenderingDevice::INVALID_FORMAT_ID ) {
		m_context.recordResourceCreationFailure(
				"T3D.VertexFormat.ClipmapPacked8", Terrain3DRdContext::RESOURCE_VERTEX_ARRAY );
		setError( "Failed to create packed Terrain3D vertex format" );
		return false;
	}
	if ( !m_context.registerVertexFormat( m_nVertexFormat, "T3D.VertexFormat.ClipmapPacked8" ) ) {
		m_nVertexFormat = RenderingDevice::INVALID_FORMAT_ID;
		setError( "Failed to register packed Terrain3D vertex format ownership" );
		return false;
	}
	if ( !m_szTopology[LOD0_MESH].upload( m_context, m_nVertexFormat ) ||
			!m_szTopology[OUTER_RING_MESH].upload( m_context, m_nVertexFormat ) ) {
		setError( "Failed to upload packed Terrain3D topology" );
		return false;
	}
	return true;
}

void Terrain3DRdClipmapRenderer::shutdown( void ) {
	std::lock_guard<std::mutex> state_lock( m_resourceStateMutex );
	for ( t3dRdViewState_t &state : m_szViewStates ) {
		freeViewState( state );
	}
	m_forwardLighting.shutdown();
	freeForwardPipelines();
	freeUniformSets();
	m_szTopology[LOD0_MESH].releaseGpu( m_context );
	m_szTopology[OUTER_RING_MESH].releaseGpu( m_context );
	m_context.freeResource( m_hIndirectBuffer, sizeof( t3dRdIndexedIndirectCommand_t ) * s_nRdDrawGroupCount );
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
	}
	m_context.freeResource( m_hTerrainSampler );
	m_context.freeResource( m_hCullPipeline );
	m_context.freeResource( m_hCullShader );
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
			m_context.freeResource( m_hszForwardShaders[tier][variant] );
		}
	}
	for ( RID &shader : m_hszShaders ) {
		m_context.freeResource( shader );
	}
	m_context.releaseVertexFormat( m_nVertexFormat );
	m_hIndirectBuffer = RID();
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
	}
	m_hTerrainSampler = RID();
	m_hCullPipeline = RID();
	m_hCullShader = RID();
	for ( RID &shader : m_hszShaders ) {
		shader = RID();
	}
	for ( auto &tier : m_hszForwardShaders ) {
		for ( RID &shader : tier ) {
			shader = RID();
		}
	}
	m_nVertexFormat = RenderingDevice::INVALID_FORMAT_ID;
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
	m_nUploadedPatchCount = 0u;
	m_bPatchUploadInitialized = false;
	std::memset( m_szUploadedPatches, 0, sizeof( m_szUploadedPatches ) );
	m_bGpuCullingReady = false;
	m_frameUploadBytes.clear();
	m_materialUploadBytes.clear();
	m_materialConfigUploadBytes.clear();
	m_editorOverlayUploadBytes.clear();
	m_patchUploadBytes.clear();
	m_cullPushBytes.clear();
	m_cpuVisibleBytes.clear();
	m_cpuIndirectBytes.clear();
	m_drawPushBytes.clear();
}

void Terrain3DRdClipmapRenderer::copyProjection( const Projection &rProjection, float *const pDestination ) const {
	for ( int column = 0; column < 4; ++column ) {
		const Vector4 value = rProjection[column];
		pDestination[column * 4 + 0] = (float)( value.x );
		pDestination[column * 4 + 1] = (float)( value.y );
		pDestination[column * 4 + 2] = (float)( value.z );
		pDestination[column * 4 + 3] = (float)( value.w );
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
		const bool bHistoryValid ) {
	if ( !pViewProjections || !pCameras || nViewCount == 0u || nViewCount > s_nRdForwardMaxViews ) {
		return false;
	}
	t3dRdFrameUniforms_t frame{};
	for ( uint32_t view = 0u; view < s_nRdForwardMaxViews; ++view ) {
		const uint32_t source = MIN( view, nViewCount - 1u );
		copyProjection( pViewProjections[source], frame.m_mViewProjection[view] );
		const Projection &previous =
				bHistoryValid && pPreviousViewProjections ? pPreviousViewProjections[source] : pViewProjections[source];
		copyProjection( previous, frame.m_mPreviousViewProjection[view] );
		frame.m_szCamera[view][0] = (float)( pCameras[source].x );
		frame.m_szCamera[view][1] = (float)( pCameras[source].y );
		frame.m_szCamera[view][2] = (float)( pCameras[source].z );
	}
	copyProjection( rView, frame.m_mView );
	frame.m_szMotionJitter[0] = (float)( rTaaJitter.x );
	frame.m_szMotionJitter[1] = (float)( rTaaJitter.y );
	frame.m_szMotionJitter[2] = (float)( bHistoryValid ? rPreviousTaaJitter.x : rTaaJitter.x );
	frame.m_szMotionJitter[3] = (float)( bHistoryValid ? rPreviousTaaJitter.y : rTaaJitter.y );
	frame.m_szMotionParams[0] = bHistoryValid ? 1.0f : 0.0f;
	frame.m_szMotionParams[1] = m_bMotionDebugEnabled.load( std::memory_order_acquire ) ? 1.0f : 0.0f;
	frame.m_fLightDirectionX = (float)( rSnapshot.m_lightDirection.x );
	frame.m_fLightDirectionY = (float)( rSnapshot.m_lightDirection.y );
	frame.m_fLightDirectionZ = (float)( rSnapshot.m_lightDirection.z );
	frame.m_fAmbient = rSnapshot.m_fAmbient;
	frame.m_fLightColorR = (float)( rSnapshot.m_lightColor.r );
	frame.m_fLightColorG = (float)( rSnapshot.m_lightColor.g );
	frame.m_fLightColorB = (float)( rSnapshot.m_lightColor.b );
	frame.m_fLightColorA = (float)( rSnapshot.m_lightColor.a );
	frame.m_fRegionSize = (float)( rSnapshot.m_nRegionSize );
	frame.m_fRegionMapSize = (float)( rSnapshot.m_nRegionMapSize );
	frame.m_fHeightMin = rSnapshot.m_fHeightMin;
	frame.m_fHeightRange = rSnapshot.m_fHeightRange;
	frame.m_fVertexSpacing = rSnapshot.m_fVertexSpacing;
	frame.m_fMeshSize = (float)( rSnapshot.m_nMeshSize );
	frame.m_fMaterialLayerCount = (float)( MAX( rSnapshot.m_nMaterialLayerCount, 1u ) );
	frame.m_fTargetX = (float)( rSnapshot.m_targetPosition.x );
	frame.m_fTargetZ = (float)( rSnapshot.m_targetPosition.z );
	frame.m_fSubdiv = MAX( rSnapshot.m_fSubdiv, 1.0f );
	frame.m_fFogR = (float)( rSnapshot.m_fogColor.r );
	frame.m_fFogG = (float)( rSnapshot.m_fogColor.g );
	frame.m_fFogB = (float)( rSnapshot.m_fogColor.b );
	frame.m_fFogDensity = rSnapshot.m_fFogDensity;
	frame.m_fFogStart = rSnapshot.m_fFogStart;
	frame.m_fFogEnd = rSnapshot.m_fFogEnd;
	frame.m_fFogCurve = rSnapshot.m_fFogCurve;
	frame.m_fFogMode = (float)( rSnapshot.m_nFogMode );
	frame.m_fDisplacementScale = rSnapshot.m_fDisplacementScale;
	frame.m_fDisplacementEnabled = rSnapshot.m_bDisplacementEnabled ? 1.0f : 0.0f;
	std::memcpy( m_frameUploadBytes.ptrw(), &frame, sizeof( frame ) );
	if ( m_context.getDevice()->buffer_update( m_hFrameUniformBuffer, 0u, sizeof( frame ), m_frameUploadBytes ) !=
			OK ) {
		return false;
	}
	m_context.recordBufferUpdate( sizeof( frame ) );
	return true;
}

bool Terrain3DRdClipmapRenderer::uploadSnapshotBuffers( const t3dRdFrameSnapshot_t &rSnapshot,
		const PackedByteArray &rRegionMapBytes,
		const uint64_t nRegionMapGeneration ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !ensureRegionMapBuffer( (uint32_t)( rRegionMapBytes.size() ) ) ) {
		return false;
	}
	if ( nRegionMapGeneration != m_nRegionMapGeneration ) {
		if ( rd->buffer_update( m_hRegionMapBuffer, 0u, (uint32_t)( rRegionMapBytes.size() ), rRegionMapBytes ) !=
				OK ) {
			return false;
		}
		m_context.recordBufferUpdate( (uint64_t)( rRegionMapBytes.size() ) );
		m_nRegionMapGeneration = nRegionMapGeneration;
	}
	if ( rSnapshot.m_nMaterialGeneration != m_nMaterialGeneration ) {
		std::memcpy( m_materialUploadBytes.ptrw(), rSnapshot.m_szMaterials, sizeof( rSnapshot.m_szMaterials ) );
		if ( rd->buffer_update( m_hMaterialBuffer, 0u, sizeof( rSnapshot.m_szMaterials ), m_materialUploadBytes ) !=
				OK ) {
			return false;
		}
		std::memcpy(
				m_materialConfigUploadBytes.ptrw(), &rSnapshot.m_materialConfig, sizeof( rSnapshot.m_materialConfig ) );
		if ( rd->buffer_update(
					 m_hMaterialConfigBuffer, 0u, sizeof( rSnapshot.m_materialConfig ), m_materialConfigUploadBytes ) !=
				OK ) {
			return false;
		}
		m_context.recordBufferUpdate( sizeof( rSnapshot.m_szMaterials ) + sizeof( rSnapshot.m_materialConfig ) );
		m_nMaterialGeneration = rSnapshot.m_nMaterialGeneration;
	}
	std::memcpy( m_editorOverlayUploadBytes.ptrw(),
			&rSnapshot.m_editorOverlayConfig,
			sizeof( rSnapshot.m_editorOverlayConfig ) );
	if ( rd->buffer_update(
				 m_hEditorOverlayBuffer, 0u, sizeof( rSnapshot.m_editorOverlayConfig ), m_editorOverlayUploadBytes ) !=
			OK ) {
		return false;
	}
	m_context.recordBufferUpdate( sizeof( rSnapshot.m_editorOverlayConfig ) );
	if ( rSnapshot.m_nGeneration != m_nPatchGeneration ) {
		if ( rSnapshot.m_nPatchCount > s_nMaxClipmapLevels ) {
			return false;
		}
		uint32_t first_dirty = 0u;
		while ( first_dirty < rSnapshot.m_nPatchCount ) {
			while ( first_dirty < rSnapshot.m_nPatchCount && m_bPatchUploadInitialized &&
					first_dirty < m_nUploadedPatchCount &&
					std::memcmp( &m_szUploadedPatches[first_dirty],
							&rSnapshot.m_szPatches[first_dirty],
							sizeof( t3dRdPatch_t ) ) == 0 ) {
				++first_dirty;
			}
			if ( first_dirty >= rSnapshot.m_nPatchCount ) {
				break;
			}
			uint32_t end_dirty = first_dirty + 1u;
			while ( end_dirty < rSnapshot.m_nPatchCount &&
					( !m_bPatchUploadInitialized || end_dirty >= m_nUploadedPatchCount ||
							std::memcmp( &m_szUploadedPatches[end_dirty],
									&rSnapshot.m_szPatches[end_dirty],
									sizeof( t3dRdPatch_t ) ) != 0 ) ) {
				++end_dirty;
			}
			const uint32_t dirty_count = end_dirty - first_dirty;
			const uint32_t dirty_bytes = dirty_count * sizeof( t3dRdPatch_t );
			std::memcpy( m_patchUploadBytes.ptrw(), &rSnapshot.m_szPatches[first_dirty], dirty_bytes );
			if ( rd->buffer_update(
						 m_hPatchBuffer, first_dirty * sizeof( t3dRdPatch_t ), dirty_bytes, m_patchUploadBytes ) !=
					OK ) {
				return false;
			}
			std::memcpy( &m_szUploadedPatches[first_dirty], &rSnapshot.m_szPatches[first_dirty], dirty_bytes );
			m_context.recordBufferUpdate( dirty_bytes );
			++m_nPatchDirtyRanges;
			first_dirty = end_dirty;
		}
		m_nUploadedPatchCount = rSnapshot.m_nPatchCount;
		m_bPatchUploadInitialized = true;
		m_nPatchGeneration = rSnapshot.m_nGeneration;
		++m_nPatchUploads;
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::runGpuCulling( const t3dRdFrameSnapshot_t &rSnapshot ) {
	if ( !m_bGpuCullingReady.load( std::memory_order_acquire ) || !m_hCullUniformSet.is_valid() ) {
		return false;
	}
	t3dRdIndexedIndirectCommand_t *const commands = (t3dRdIndexedIndirectCommand_t *)( m_cpuIndirectBytes.ptrw() );
	std::memset( commands, 0, (size_t)( m_cpuIndirectBytes.size() ) );
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
		commands[group].m_nIndexCount = group == T3D_RD_DRAW_GROUP_LOD0_NEAR
				? m_szTopology[LOD0_MESH].getIndexCount()
				: m_szTopology[OUTER_RING_MESH].getIndexCount();
		commands[group].m_nFirstInstance = group * s_nMaxClipmapLevels;
	}
	t3dRdCullPushConstants_t constants{};
	constants.m_nPatchCount = MIN( rSnapshot.m_nPatchCount, (uint32_t)( s_nMaxClipmapLevels ) );
	constants.m_nLod0IndexCount = m_szTopology[LOD0_MESH].getIndexCount();
	constants.m_nOuterIndexCount = m_szTopology[OUTER_RING_MESH].getIndexCount();
	constants.m_nMaxPatches = s_nMaxClipmapLevels;
	std::memcpy( m_cullPushBytes.ptrw(), &constants, sizeof( constants ) );
	RenderingDevice *const rd = m_context.getDevice();
	if ( rd->buffer_update( m_hIndirectBuffer, 0u, (uint32_t)( m_cpuIndirectBytes.size() ), m_cpuIndirectBytes ) !=
			OK ) {
		return false;
	}
	m_context.recordBufferUpdate( (uint64_t)( m_cpuIndirectBytes.size() ) );
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
	std::memcpy( m_cpuVisibleBytes.ptrw(), rResult.m_szVisibleIndices, sizeof( rResult.m_szVisibleIndices ) );
	std::memcpy( m_cpuIndirectBytes.ptrw(), rResult.m_szCommands, sizeof( rResult.m_szCommands ) );
	RenderingDevice *const rd = m_context.getDevice();
	if ( rd->buffer_update( m_hVisibleBuffer, 0u, (uint32_t)( m_cpuVisibleBytes.size() ), m_cpuVisibleBytes ) != OK ) {
		return false;
	}
	if ( rd->buffer_update( m_hIndirectBuffer, 0u, (uint32_t)( m_cpuIndirectBytes.size() ), m_cpuIndirectBytes ) !=
			OK ) {
		return false;
	}
	m_context.recordBufferUpdate( (uint64_t)( m_cpuVisibleBytes.size() + m_cpuIndirectBytes.size() ) );
	return true;
}

bool Terrain3DRdClipmapRenderer::compareGpuCpuCulling( const t3dRdFrameSnapshot_t &rSnapshot,
		const Projection &rViewProjection,
		const t3dRdCpuCullResult_t &rExpected ) {
	const uint64_t comparison_index = m_nCullComparisons.fetch_add( 1u, std::memory_order_acq_rel );
	RenderingDevice *const rd = m_context.getDevice();
	const PackedByteArray gpu_visible =
			rd->buffer_get_data( m_hVisibleBuffer, 0u, (uint32_t)( sizeof( rExpected.m_szVisibleIndices ) ) );
	const PackedByteArray gpu_indirect =
			rd->buffer_get_data( m_hIndirectBuffer, 0u, (uint32_t)( sizeof( rExpected.m_szCommands ) ) );
	if ( gpu_visible.size() != (int64_t)( sizeof( rExpected.m_szVisibleIndices ) ) ||
			gpu_indirect.size() != (int64_t)( sizeof( rExpected.m_szCommands ) ) ) {
		++m_nCullMismatches;
		return false;
	}
	t3dRdCpuCullResult_t actual{};
	std::memcpy( actual.m_szVisibleIndices, gpu_visible.ptr(), sizeof( actual.m_szVisibleIndices ) );
	std::memcpy( actual.m_szCommands, gpu_indirect.ptr(), sizeof( actual.m_szCommands ) );
	for ( const t3dRdIndexedIndirectCommand_t &command : actual.m_szCommands ) {
		actual.m_nVisibleCount += command.m_nInstanceCount;
	}
	if ( !Terrain3DRdCpuCuller::validate( rSnapshot,
				 rViewProjection,
				 m_szTopology[LOD0_MESH].getIndexCount(),
				 m_szTopology[OUTER_RING_MESH].getIndexCount(),
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
		bool expected_set[s_nMaxClipmapLevels]{};
		bool actual_set[s_nMaxClipmapLevels]{};
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
	const t3dRdDrawPushConstants_t indirect_constants{};
	std::memcpy( m_drawPushBytes.ptrw(), &indirect_constants, sizeof( indirect_constants ) );
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
		constexpr uint32_t indirect_buffer_bytes = sizeof( t3dRdIndexedIndirectCommand_t ) * s_nRdDrawGroupCount;
		if ( !Terrain3DRdCpuCuller::isCommandRangeValid( group, indirect_buffer_bytes ) ) {
			++m_nIndirectValidationFailures;
			rd->draw_list_end();
			return false;
		}
		const int tier = group <= 1u ? 0 : ( group == 2u ? 1 : 2 );
		const Terrain3DRdTopology &topology = group == 0u ? m_szTopology[LOD0_MESH] : m_szTopology[OUTER_RING_MESH];
		rd->draw_list_bind_render_pipeline( draw_list, state.m_hszPipelines[tier] );
		rd->draw_list_set_push_constant( draw_list, m_drawPushBytes, sizeof( indirect_constants ) );
		rd->draw_list_bind_uniform_set( draw_list, m_hszRasterUniformSets[tier], 0u );
		rd->draw_list_bind_vertex_array( draw_list, topology.getVertexArray() );
		rd->draw_list_bind_index_array( draw_list, topology.getIndexArray() );
		rd->draw_list_draw_indirect( draw_list,
				true,
				m_hIndirectBuffer,
				group * sizeof( t3dRdIndexedIndirectCommand_t ),
				1u,
				sizeof( t3dRdIndexedIndirectCommand_t ) );
		++m_nIndirectDraws;
	}
	rd->draw_list_end();
	m_nDrawsRendered += s_nRdDrawGroupCount;
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
		const Terrain3DRdTopology &topology =
				group == T3D_RD_DRAW_GROUP_LOD0_NEAR ? m_szTopology[LOD0_MESH] : m_szTopology[OUTER_RING_MESH];
		rd->draw_list_bind_render_pipeline( draw_list, state.m_hszPipelines[tier] );
		rd->draw_list_bind_uniform_set( draw_list, m_hszRasterUniformSets[tier], 0u );
		rd->draw_list_bind_vertex_array( draw_list, topology.getVertexArray() );
		rd->draw_list_bind_index_array( draw_list, topology.getIndexArray() );
		const uint32_t count = rCullResult.m_szCommands[group].m_nInstanceCount;
		for ( uint32_t slot = 0u; slot < count; ++slot ) {
			t3dRdDrawPushConstants_t constants{};
			constants.m_nDirectPatchIndex = rCullResult.m_szVisibleIndices[group * s_nMaxClipmapLevels + slot];
			constants.m_nDirectMode = 1u;
			std::memcpy( m_drawPushBytes.ptrw(), &constants, sizeof( constants ) );
			rd->draw_list_set_push_constant( draw_list, m_drawPushBytes, sizeof( constants ) );
			rd->draw_list_draw( draw_list, true, 1u, 0u );
			++m_nDirectDebugDraws;
		}
	}
	rd->draw_list_end();
	m_nDrawsRendered += rCullResult.m_nVisibleCount;
	return true;
}

bool Terrain3DRdClipmapRenderer::render( RenderData *const pRenderData,
		const t3dRdFrameSnapshot_t &rSnapshot,
		const PackedByteArray &rRegionMapBytes,
		const uint64_t nRegionMapGeneration,
		const RID &rHeightTextureRd,
		const RID &rDerivedTextureRd,
		const RID &rControlTextureRd,
		const RID &rColorTextureRd,
		const RID &rMiddleCompositeTextureRd,
		const RID &rTerrainShadowMaskTextureRd,
		const RID &rMiddleHeightTextureRd,
		const RID &rMiddleHeightRegionMapTextureRd,
		const RID &rFarHeightTextureRd,
		const RID &rFarColorTextureRd,
		const RID &rAlbedoTextureRd,
		const RID &rFarAlbedoTextureRd,
		const RID &rNormalTextureRd,
		const RID &rSurfaceTextureRd,
		const RID &rDisplacementTextureRd,
		const RID &rWorldNoiseTextureRd,
		const RID &rMacroNoiseTextureRd,
		const RID &rEditorBrushTextureRd,
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
	if ( view_count == 0u || view_count > s_nMaxViews ) {
		setError( "Unsupported Terrain3D RD view count" );
		return false;
	}
	const RID height = rHeightTextureRd;
	const RID control = rControlTextureRd;
	const RID color_map = rColorTextureRd;
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
	RID displacement = rDisplacementTextureRd;
	RID world_noise = rWorldNoiseTextureRd;
	RID macro_noise = rMacroNoiseTextureRd;
	RID editor_brush = rEditorBrushTextureRd;
	if ( !albedo.is_valid() ) {
		albedo = color_map;
	}
	if ( !surface.is_valid() ) {
		surface = normal.is_valid() ? normal : albedo;
	}
	if ( !normal.is_valid() ) {
		normal = surface;
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
	if ( !height.is_valid() || !control.is_valid() || !color_map.is_valid() || !albedo.is_valid() ||
			!normal.is_valid() || !surface.is_valid() || !displacement.is_valid() ) {
		setError( "Terrain3D direct RenderingDevice textures are incomplete" );
		return false;
	}
	const uint64_t gpu_frame_id = m_nFramesRendered.load( std::memory_order_acquire ) + 1u;
	m_context.beginFrame( gpu_frame_id, rSnapshot.m_nPublicationGeneration );
	if ( !uploadSnapshotBuffers( rSnapshot, rRegionMapBytes, nRegionMapGeneration ) ) {
		setError( "Failed to upload Terrain3D RD frame data" );
		return false;
	}
	if ( !ensureUniformSets( height,
				 derived,
				 control,
				 color_map,
				 middle_composite,
				 terrain_shadow_mask,
				 middle_height,
				 middle_height_region_map,
				 far_height,
				 far_color,
				 albedo,
				 far_albedo,
				 normal,
				 surface,
				 displacement,
				 world_noise,
				 macro_noise,
				 editor_brush,
				 rSnapshot.m_materialConfig ) ) {
		return false;
	}
	const RenderingDevice::TextureSamples samples = buffers->get_texture_samples();
	const bool bMsaa = samples != RenderingDevice::TEXTURE_SAMPLES_1;
	const bool bDirectDebug = m_bDirectDrawDebugEnabled.load( std::memory_order_acquire );
	const bool bGpuCulling = m_bGpuCullingReady.load( std::memory_order_acquire );
	const bool bCompareCulling = m_bCullingComparisonEnabled.load( std::memory_order_acquire );
	const Transform3D camera_transform = scene_data->get_cam_transform();
	for ( uint32_t view = 0u; view < view_count; ++view ) {
		RID target_color =
				view_count > 1u ? buffers->get_color_layer( view, bMsaa ) : buffers->get_color_texture( bMsaa );
		RID target_depth =
				view_count > 1u ? buffers->get_depth_layer( view, bMsaa ) : buffers->get_depth_texture( bMsaa );
		if ( !target_color.is_valid() || !target_depth.is_valid() ||
				!ensureViewState( view, target_color, target_depth, samples, bColorWrite ) ) {
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
		if ( !updateFrameUniforms( rSnapshot,
					 direct_views,
					 1u,
					 view_matrix,
					 direct_cameras,
					 nullptr,
					 Vector2(),
					 Vector2(),
					 false ) ) {
			setError( "Failed to update Terrain3D frame uniforms" );
			return false;
		}
		if ( !Terrain3DRdCpuCuller::build( rSnapshot,
					 view_projection,
					 m_szTopology[LOD0_MESH].getIndexCount(),
					 m_szTopology[OUTER_RING_MESH].getIndexCount(),
					 m_cpuCullResult ) ) {
			++m_nIndirectValidationFailures;
			setError( "Terrain3D CPU reference culler rejected patch or indirect bounds" );
			return false;
		}
		m_nVisiblePatches.store( m_cpuCullResult.m_nVisibleCount, std::memory_order_release );
		m_context.recordSceneCounts( rSnapshot.m_nPatchCount, m_cpuCullResult.m_nVisibleCount, 0u, 0u, 0u, 0u );
		if ( bDirectDebug ) {
			if ( !drawViewDirect( view, m_cpuCullResult ) ) {
				setError( "Failed to submit Terrain3D debug direct draws" );
				return false;
			}
			continue;
		}
		if ( bGpuCulling ) {
			if ( !runGpuCulling( rSnapshot ) ) {
				setError( "Failed to submit Terrain3D compute culling" );
				return false;
			}
			if ( bCompareCulling && !compareGpuCpuCulling( rSnapshot, view_projection, m_cpuCullResult ) ) {
				setError( "Terrain3D GPU/CPU culling visible-set mismatch" );
				return false;
			}
		} else if ( !uploadCpuIndirect( m_cpuCullResult ) ) {
			setError( "Failed to upload Terrain3D CPU reference culling output" );
			return false;
		}
		if ( !drawView( view ) ) {
			setError( "Failed to submit Terrain3D indexed indirect draws" );
			return false;
		}
	}
	++m_nFramesRendered;
	m_context.recordSubmissionCounts(
			bDirectDebug ? 0u : view_count * s_nRdDrawGroupCount, bDirectDebug || !bGpuCulling ? 0u : view_count );
	m_context.recordSuccessfulPass( Terrain3DRdContext::PASS_TERRAIN_CLIPMAP, gpu_frame_id );
	m_context.markWarmupComplete();
	clearError();
	return true;
}

bool Terrain3DRdClipmapRenderer::prepareForwardPass( const t3dRdForwardPassContext_t &rContext,
		const t3dRdFrameSnapshot_t &rSnapshot,
		const PackedByteArray &rRegionMapBytes,
		const uint64_t nRegionMapGeneration,
		const RID &rHeightTextureRd,
		const RID &rDerivedTextureRd,
		const RID &rControlTextureRd,
		const RID &rColorTextureRd,
		const RID &rMiddleCompositeTextureRd,
		const RID &rTerrainShadowMaskTextureRd,
		const RID &rMiddleHeightTextureRd,
		const RID &rMiddleHeightRegionMapTextureRd,
		const RID &rFarHeightTextureRd,
		const RID &rFarColorTextureRd,
		const RID &rAlbedoTextureRd,
		const RID &rFarAlbedoTextureRd,
		const RID &rNormalTextureRd,
		const RID &rSurfaceTextureRd,
		const RID &rDisplacementTextureRd,
		const RID &rWorldNoiseTextureRd,
		const RID &rMacroNoiseTextureRd,
		const RID &rEditorBrushTextureRd,
		const bool bTemporalHistoryValid,
		const bool bViewportReconfigured ) {
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

	const RID height = rHeightTextureRd;
	const RID control = rControlTextureRd;
	const RID color_map = rColorTextureRd;
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
	RID displacement = rDisplacementTextureRd;
	RID world_noise = rWorldNoiseTextureRd;
	RID macro_noise = rMacroNoiseTextureRd;
	RID editor_brush = rEditorBrushTextureRd;
	if ( !albedo.is_valid() ) {
		albedo = color_map;
	}
	if ( !surface.is_valid() ) {
		surface = normal.is_valid() ? normal : albedo;
	}
	if ( !normal.is_valid() ) {
		normal = surface;
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
	if ( !height.is_valid() || !control.is_valid() || !color_map.is_valid() || !albedo.is_valid() ||
			!normal.is_valid() || !surface.is_valid() || !displacement.is_valid() ) {
		setError( "Terrain3D direct RenderingDevice textures are incomplete" );
		return false;
	}

	for ( uint32_t view = 0u; view < s_nRdForwardMaxViews; ++view ) {
		const uint32_t source = MIN( view, rContext.m_nViewCount - 1u );
		m_szForwardEyePositions[view] = rContext.m_cameraTransform.origin;
		if ( rContext.m_nViewCount > 1u ) {
			m_szForwardEyePositions[view] += rContext.m_cameraTransform.basis.xform( rContext.m_szEyeOffset[source] );
		}
	}
	m_bForwardHistoryValid = bTemporalHistoryValid;
	if ( m_nForwardPreparedScenePass != rContext.m_nScenePass ||
			m_nForwardPreparedViewportKey != rContext.m_nViewportKey ) {
		m_context.beginFrame( rContext.m_nScenePass, rSnapshot.m_nPublicationGeneration );
	}
	if ( !uploadSnapshotBuffers( rSnapshot, rRegionMapBytes, nRegionMapGeneration ) ||
			!ensureUniformSets( height,
					derived,
					control,
					color_map,
					middle_composite,
					terrain_shadow_mask,
					middle_height,
					middle_height_region_map,
					far_height,
					far_color,
					albedo,
					far_albedo,
					normal,
					surface,
					displacement,
					world_noise,
					macro_noise,
					editor_brush,
					rSnapshot.m_materialConfig ) ||
			!updateFrameUniforms( rSnapshot,
					rContext.m_szViewProjection,
					rContext.m_nViewCount,
					Projection( rContext.m_cameraTransform.affine_inverse() ),
					m_szForwardEyePositions,
					rContext.m_szPreviousViewProjection,
					rContext.m_taaJitter,
					rContext.m_previousTaaJitter,
					rContext.m_bMotionVectorsEnabled && m_bForwardHistoryValid ) ) {
		setError( "Failed to prepare Terrain3D native Forward+ frame state" );
		return false;
	}

	const bool shadow_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
	const bool recull = shadow_pass || m_nForwardPreparedScenePass != rContext.m_nScenePass ||
			m_nForwardPreparedViewportKey != rContext.m_nViewportKey ||
			m_nForwardPreparedSnapshotGeneration != rSnapshot.m_nPublicationGeneration ||
			m_forwardPreparedCullPassKind != rContext.m_passKind;
	if ( recull ) {
		if ( !Terrain3DRdCpuCuller::buildMultiview( rSnapshot,
					 rContext.m_szViewProjection,
					 rContext.m_nViewCount,
					 m_szTopology[LOD0_MESH].getIndexCount(),
					 m_szTopology[OUTER_RING_MESH].getIndexCount(),
					 m_cpuCullResult ) ) {
			++m_nIndirectValidationFailures;
			setError( "Terrain3D CPU reference culler rejected native Forward+ patch bounds" );
			return false;
		}
		if ( shadow_pass ) {
			t3dRdApplyShadowCasterPolicy( rSnapshot, m_cpuCullResult );
		}
		m_nVisiblePatches.store( m_cpuCullResult.m_nVisibleCount, std::memory_order_release );
		if ( !m_bDirectDrawDebugEnabled.load( std::memory_order_acquire ) ) {
			if ( !shadow_pass && rContext.m_nViewCount == 1u && m_bGpuCullingReady.load( std::memory_order_acquire ) ) {
				if ( !runGpuCulling( rSnapshot ) ) {
					return false;
				}
			} else if ( !uploadCpuIndirect( m_cpuCullResult ) ) {
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
		if ( !m_forwardLighting.prepare( rContext, rSnapshot.m_nRenderLayers ) ) {
			setError( String( "Failed to prepare Terrain3D Forward+ native lighting: " ) +
					String( m_forwardLighting.getDiagnostics().get( "forward_light_last_error", "unknown" ) ) );
			return false;
		}
		for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
			if ( !m_forwardLighting.ensureSet( m_hszForwardShaders[tier][variant] ).is_valid() ) {
				setError( "Failed to cache Terrain3D Forward+ terrain lighting descriptor" );
				return false;
			}
		}
	}
	if ( !ensureForwardPipelines( rContext, variant ) ) {
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
	if ( !selectForwardVariant( rContext, variant ) || variant != m_lastForwardPipelineKey.m_variant ||
			rContext.m_nFramebufferFormat != m_lastForwardPipelineKey.m_nFramebufferFormat ||
			rContext.m_nViewCount != m_lastForwardPipelineKey.m_nViewCount ||
			rContext.m_passKind != m_lastForwardPipelineKey.m_passKind ||
			rContext.m_bReverseCull != m_lastForwardPipelineKey.m_bReverseCull ||
			( ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ||
					  rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) &&
					rContext.m_bDepthPrepassActive ) != m_lastForwardPipelineKey.m_bDepthPrepassActive ) {
		return false;
	}
	const t3dRdForwardPipelineEntry_t *const entry = findForwardPipeline( m_lastForwardPipelineKey );
	if ( !entry ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	const int64_t draw_list = rContext.m_nDrawList;
	const bool direct = m_bDirectDrawDebugEnabled.load( std::memory_order_acquire );
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
		const uint32_t tier = group <= 1u ? 0u : ( group == 2u ? 1u : 2u );
		const Terrain3DRdTopology &topology =
				group == T3D_RD_DRAW_GROUP_LOD0_NEAR ? m_szTopology[LOD0_MESH] : m_szTopology[OUTER_RING_MESH];
		rd->draw_list_bind_render_pipeline( draw_list, entry->m_hszPipelines[tier] );
		rd->draw_list_bind_uniform_set( draw_list, m_hszRasterUniformSets[tier], 0u );
		if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) {
			const RID lighting_set = m_forwardLighting.getSet( m_hszForwardShaders[tier][variant] );
			if ( !lighting_set.is_valid() ) {
				return false;
			}
			rd->draw_list_bind_uniform_set( draw_list, lighting_set, 2u );
		}
		rd->draw_list_bind_vertex_array( draw_list, topology.getVertexArray() );
		rd->draw_list_bind_index_array( draw_list, topology.getIndexArray() );
		if ( direct ) {
			const uint32_t count = m_cpuCullResult.m_szCommands[group].m_nInstanceCount;
			for ( uint32_t slot = 0u; slot < count; ++slot ) {
				t3dRdDrawPushConstants_t constants{};
				constants.m_nDirectPatchIndex = m_cpuCullResult.m_szVisibleIndices[group * s_nMaxClipmapLevels + slot];
				constants.m_nDirectMode = 1u;
				std::memcpy( m_drawPushBytes.ptrw(), &constants, sizeof( constants ) );
				rd->draw_list_set_push_constant( draw_list, m_drawPushBytes, sizeof( constants ) );
				rd->draw_list_draw( draw_list, true, 1u, 0u );
				++m_nDirectDebugDraws;
			}
			m_nDrawsRendered += count;
		} else {
			constexpr uint32_t indirect_buffer_bytes = sizeof( t3dRdIndexedIndirectCommand_t ) * s_nRdDrawGroupCount;
			if ( !Terrain3DRdCpuCuller::isCommandRangeValid( group, indirect_buffer_bytes ) ) {
				return false;
			}
			const t3dRdDrawPushConstants_t constants{};
			std::memcpy( m_drawPushBytes.ptrw(), &constants, sizeof( constants ) );
			rd->draw_list_set_push_constant( draw_list, m_drawPushBytes, sizeof( constants ) );
			rd->draw_list_draw_indirect( draw_list,
					true,
					m_hIndirectBuffer,
					group * sizeof( t3dRdIndexedIndirectCommand_t ),
					1u,
					sizeof( t3dRdIndexedIndirectCommand_t ) );
			++m_nIndirectDraws;
			++m_nDrawsRendered;
		}
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::renderForwardPass( const t3dRdForwardPassContext_t &rContext ) {
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT ) {
		return false;
	}
	if ( m_nForwardPreparedScenePass != rContext.m_nScenePass ||
			m_nForwardPreparedViewportKey != rContext.m_nViewportKey ) {
		return false;
	}
	if ( !drawForwardView( rContext ) ) {
		setError( "Failed to submit Terrain3D terrain onto native Forward+ draw list" );
		return false;
	}
	++m_nFramesRendered;
	m_context.recordSubmissionCounts(
			m_bDirectDrawDebugEnabled.load( std::memory_order_acquire ) ? 0u : s_nRdDrawGroupCount, 0u );
	m_context.markWarmupComplete();
	clearError();
	return true;
}

Dictionary Terrain3DRdClipmapRenderer::getDiagnostics( void ) const {
	std::lock_guard<std::mutex> state_lock( m_resourceStateMutex );
	Dictionary result = m_context.getDiagnostics();
	result["frames_rendered"] = (int64_t)( m_nFramesRendered.load( std::memory_order_acquire ) );
	result["draws_rendered"] = (int64_t)( m_nDrawsRendered.load( std::memory_order_acquire ) );
	result["indirect_draws"] = (int64_t)( m_nIndirectDraws.load( std::memory_order_acquire ) );
	result["cull_dispatches"] = (int64_t)( m_nCullDispatches.load( std::memory_order_acquire ) );
	result["patch_uploads"] = (int64_t)( m_nPatchUploads.load( std::memory_order_acquire ) );
	result["patch_dirty_ranges"] = (int64_t)( m_nPatchDirtyRanges.load( std::memory_order_acquire ) );
	result["visible_terrain_patches"] = (int64_t)( m_nVisiblePatches.load( std::memory_order_acquire ) );
	result["cull_comparison_enabled"] = m_bCullingComparisonEnabled.load( std::memory_order_acquire );
	result["cull_comparisons"] = (int64_t)( m_nCullComparisons.load( std::memory_order_acquire ) );
	result["cull_mismatches"] = (int64_t)( m_nCullMismatches.load( std::memory_order_acquire ) );
	result["indirect_validation_failures"] =
			(int64_t)( m_nIndirectValidationFailures.load( std::memory_order_acquire ) );
	result["direct_draw_debug_enabled"] = m_bDirectDrawDebugEnabled.load( std::memory_order_acquire );
	result["direct_debug_draws"] = (int64_t)( m_nDirectDebugDraws.load( std::memory_order_acquire ) );
	result["topology_bytes"] = (int64_t)( getTopologyBytes() );
	result["lod0_index_bits"] =
			m_szTopology[LOD0_MESH].getIndexFormat() == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ? 16 : 32;
	result["outer_index_bits"] =
			m_szTopology[OUTER_RING_MESH].getIndexFormat() == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ? 16 : 32;
	result["lod0_topology_uploads"] = (int64_t)( m_szTopology[LOD0_MESH].getUploadCount() );
	result["outer_topology_uploads"] = (int64_t)( m_szTopology[OUTER_RING_MESH].getUploadCount() );
	result["lod0_vertex_checksum"] = String::num_uint64( m_szTopology[LOD0_MESH].getVertexChecksum(), 16, true );
	result["lod0_uploaded_vertex_checksum"] =
			String::num_uint64( m_szTopology[LOD0_MESH].getUploadedVertexChecksum(), 16, true );
	result["lod0_index_checksum"] = String::num_uint64( m_szTopology[LOD0_MESH].getIndexChecksum(), 16, true );
	result["lod0_uploaded_index_checksum"] =
			String::num_uint64( m_szTopology[LOD0_MESH].getUploadedIndexChecksum(), 16, true );
	result["outer_vertex_checksum"] = String::num_uint64( m_szTopology[OUTER_RING_MESH].getVertexChecksum(), 16, true );
	result["outer_uploaded_vertex_checksum"] =
			String::num_uint64( m_szTopology[OUTER_RING_MESH].getUploadedVertexChecksum(), 16, true );
	result["outer_index_checksum"] = String::num_uint64( m_szTopology[OUTER_RING_MESH].getIndexChecksum(), 16, true );
	result["outer_uploaded_index_checksum"] =
			String::num_uint64( m_szTopology[OUTER_RING_MESH].getUploadedIndexChecksum(), 16, true );
	result["lod0_grid_min"] = m_szTopology[LOD0_MESH].getGridMinimum();
	result["lod0_grid_max"] = m_szTopology[LOD0_MESH].getGridMaximum();
	result["outer_grid_min"] = m_szTopology[OUTER_RING_MESH].getGridMinimum();
	result["outer_grid_max"] = m_szTopology[OUTER_RING_MESH].getGridMaximum();
	result["gpu_culling"] = m_bGpuCullingReady.load( std::memory_order_acquire );
	result["fallback_displacement_texture"] = m_hFallbackDisplacementTexture.is_valid();
	result["max_views"] = (int64_t)( s_nMaxViews );
	result["forward_lighting"] = m_forwardLighting.getDiagnostics();
	result["last_error"] = getLastError();
	return result;
}
