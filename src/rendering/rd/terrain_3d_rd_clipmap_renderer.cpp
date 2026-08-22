// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_clipmap_renderer.h"

#include "core/logger.h"

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
#include <godot_cpp/classes/rendering_server.hpp>

#include <cstring>

CLASS_NAME_STATIC( "Terrain3DRdClipmapRenderer" );

static Ref<RDUniform> t3dRdBufferUniform( const int nBinding, const RenderingDevice::UniformType type, const RID &rRid ) {
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

Terrain3DRdClipmapRenderer::~Terrain3DRdClipmapRenderer( void ) { shutdown(); }

void Terrain3DRdClipmapRenderer::setError( const String &rError ) {
	{
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_lastError = rError;
	}
	LOG( ERROR, "[RD alpha] ", rError );
}

void Terrain3DRdClipmapRenderer::clearError( void ) {
	std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
	m_lastError = String();
}

String Terrain3DRdClipmapRenderer::getLastError( void ) const {
	std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
	return m_lastError;
}

bool Terrain3DRdClipmapRenderer::prepareTopologies( const t3dClipmapGeometry_t &rLod0, const t3dClipmapGeometry_t &rOuter ) {
	return m_szTopology[LOD0_MESH].buildCpu( rLod0, "Lod0" ) &&
			m_szTopology[OUTER_RING_MESH].buildCpu( rOuter, "Outer" );
}

bool Terrain3DRdClipmapRenderer::ensureDevice( void ) {
	if ( m_context.isReady() ) return true;
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
	if ( !rd ) return false;
	const Ref<RDShaderSPIRV> shaders[3] = { rNear, rMiddle, rFar };
	const char *names[3] = { "T3D.PipelineShader.Near", "T3D.PipelineShader.Middle", "T3D.PipelineShader.Far" };
	for ( int i = 0; i < 3; ++i ) {
		if ( m_hszShaders[i].is_valid() ) continue;
		if ( shaders[i].is_null() ) {
			setError( "Missing Terrain3D RD raster shader SPIR-V" );
			return false;
		}
		m_hszShaders[i] = rd->shader_create_from_spirv( shaders[i] );
		if ( !m_hszShaders[i].is_valid() ) {
			setError( "Failed to create Terrain3D RD raster shader" );
			return false;
		}
		m_context.registerResource( m_hszShaders[i], names[i] );
	}
	if ( !m_hCullShader.is_valid() && rCull.is_valid() ) {
		m_hCullShader = rd->shader_create_from_spirv( rCull );
		if ( m_hCullShader.is_valid() ) {
			m_context.registerResource( m_hCullShader, "T3D.Shader.Cull" );
			m_hCullPipeline = rd->compute_pipeline_create( m_hCullShader );
			if ( m_hCullPipeline.is_valid() ) m_context.registerResource( m_hCullPipeline, "T3D.Pipeline.Cull" );
		}
	}
	m_bGpuCullingReady = m_hCullShader.is_valid() && m_hCullPipeline.is_valid();
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureSamplers( void ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) return false;
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
		if ( !m_hTerrainSampler.is_valid() ) { setError( "Failed to create terrain data sampler" ); return false; }
		m_context.registerResource( m_hTerrainSampler, "T3D.Sampler.Terrain" );
	}
	if ( !m_hMaterialSampler.is_valid() ) {
		Ref<RDSamplerState> state;
		state.instantiate();
		state->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_mip_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT );
		state->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT );
		state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_hMaterialSampler = rd->sampler_create( state );
		if ( !m_hMaterialSampler.is_valid() ) { setError( "Failed to create material sampler" ); return false; }
		m_context.registerResource( m_hMaterialSampler, "T3D.Sampler.Material" );
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureFallbackTextures( void ) {
	if ( m_hFallbackDisplacementTexture.is_valid() ) return true;
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) return false;
	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_width( 1u );
	format->set_height( 1u );
	format->set_depth( 1u );
	format->set_array_layers( 1u );
	format->set_mipmaps( 1u );
	format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
	format->set_format( RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT );
	format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT );
	Ref<RDTextureView> view;
	view.instantiate();
	PackedByteArray texel;
	texel.resize( 4 * sizeof( float ) );
	const float neutral[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
	std::memcpy( texel.ptrw(), neutral, sizeof( neutral ) );
	TypedArray<PackedByteArray> initial_data;
	initial_data.push_back( texel );
	m_hFallbackDisplacementTexture = rd->texture_create( format, view, initial_data );
	if ( !m_hFallbackDisplacementTexture.is_valid() ) {
		setError( "Failed to create neutral Terrain3D displacement texture" );
		return false;
	}
	m_context.registerTextureResource( m_hFallbackDisplacementTexture, "T3D.Texture.DisplacementFallback", sizeof( neutral ) );
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureBuffers( void ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) return false;
	const uint32_t frame_bytes = ( uint32_t )( sizeof( t3dRdFrameUniforms_t ) );
	const uint32_t material_bytes = ( uint32_t )( sizeof( t3dRdMaterialLayer_t ) * s_nRdMaterialLayerCount );
	const uint32_t patch_bytes = ( uint32_t )( sizeof( t3dRdPatch_t ) * s_nMaxClipmapLevels );
	const uint32_t visible_bytes = ( uint32_t )( sizeof( uint32_t ) * s_nRdDrawGroupCount * s_nMaxClipmapLevels );
	const uint32_t indirect_bytes = ( uint32_t )( sizeof( t3dRdIndexedIndirectCommand_t ) * s_nRdDrawGroupCount );
	if ( !m_hFrameUniformBuffer.is_valid() ) {
		m_hFrameUniformBuffer = rd->uniform_buffer_create( frame_bytes );
		if ( !m_hFrameUniformBuffer.is_valid() ) return false;
		m_context.registerResource( m_hFrameUniformBuffer, "T3D.Frame.UBO", frame_bytes );
	}
	if ( !m_hMaterialBuffer.is_valid() ) {
		m_hMaterialBuffer = rd->storage_buffer_create( material_bytes );
		if ( !m_hMaterialBuffer.is_valid() ) return false;
		m_context.registerResource( m_hMaterialBuffer, "T3D.Materials", material_bytes );
	}
	if ( !m_hPatchBuffer.is_valid() ) {
		m_hPatchBuffer = rd->storage_buffer_create( patch_bytes );
		if ( !m_hPatchBuffer.is_valid() ) return false;
		m_context.registerResource( m_hPatchBuffer, "T3D.Clipmap.Patches", patch_bytes );
	}
	if ( !m_hVisibleBuffer.is_valid() ) {
		m_hVisibleBuffer = rd->storage_buffer_create( visible_bytes );
		if ( !m_hVisibleBuffer.is_valid() ) return false;
		m_context.registerResource( m_hVisibleBuffer, "T3D.Clipmap.Visible", visible_bytes );
	}
	if ( !m_hIndirectBuffer.is_valid() ) {
		m_hIndirectBuffer = rd->storage_buffer_create( indirect_bytes,
				PackedByteArray(),
				RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT );
		if ( !m_hIndirectBuffer.is_valid() ) return false;
		m_context.registerResource( m_hIndirectBuffer, "T3D.Clipmap.Indirect", indirect_bytes );
	}
	m_frameUploadBytes.resize( frame_bytes );
	m_materialUploadBytes.resize( material_bytes );
	m_patchUploadBytes.resize( patch_bytes );
	m_cullPushBytes.resize( sizeof( t3dRdCullPushConstants_t ) );
	m_cpuVisibleBytes.resize( visible_bytes );
	m_cpuIndirectBytes.resize( indirect_bytes );
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureRegionMapBuffer( const uint32_t nRequiredBytes ) {
	if ( nRequiredBytes == 0u ) return false;
	if ( m_hRegionMapBuffer.is_valid() && nRequiredBytes <= m_nRegionMapCapacityBytes ) return true;
	freeUniformSets();
	m_context.freeResource( m_hRegionMapBuffer, m_nRegionMapCapacityBytes );
	m_nRegionMapCapacityBytes = nRequiredBytes;
	m_hRegionMapBuffer = m_context.getDevice()->storage_buffer_create( m_nRegionMapCapacityBytes );
	if ( !m_hRegionMapBuffer.is_valid() ) {
		m_nRegionMapCapacityBytes = 0u;
		setError( "Failed to allocate RD region page-table buffer" );
		return false;
	}
	m_context.registerResource( m_hRegionMapBuffer, "T3D.RegionMap", m_nRegionMapCapacityBytes );
	m_nRegionMapGeneration = 0u;
	return true;
}

void Terrain3DRdClipmapRenderer::freeUniformSets( void ) {
	for ( RID &rid : m_hszRasterUniformSets ) m_context.freeResource( rid );
	m_context.freeResource( m_hCullUniformSet );
	m_hBoundHeightTexture = RID();
	m_hBoundControlTexture = RID();
	m_hBoundColorTexture = RID();
	m_hBoundAlbedoTexture = RID();
	m_hBoundNormalTexture = RID();
	m_hBoundSurfaceTexture = RID();
	m_hBoundDisplacementTexture = RID();
}

bool Terrain3DRdClipmapRenderer::ensureUniformSets( const RID &rHeight,
		const RID &rControl,
		const RID &rColor,
		const RID &rAlbedo,
		const RID &rNormal,
		const RID &rSurface,
		const RID &rDisplacement ) {
	if ( m_hszRasterUniformSets[0].is_valid() && m_hszRasterUniformSets[1].is_valid() &&
			m_hszRasterUniformSets[2].is_valid() && ( !m_bGpuCullingReady || m_hCullUniformSet.is_valid() ) &&
			m_hBoundHeightTexture == rHeight && m_hBoundControlTexture == rControl &&
			m_hBoundColorTexture == rColor && m_hBoundAlbedoTexture == rAlbedo &&
			m_hBoundNormalTexture == rNormal && m_hBoundSurfaceTexture == rSurface &&
			m_hBoundDisplacementTexture == rDisplacement ) return true;
	freeUniformSets();
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !m_hRegionMapBuffer.is_valid() ) return false;
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( t3dRdBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameUniformBuffer ) );
	uniforms.push_back( t3dRdBufferUniform( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hRegionMapBuffer ) );
	uniforms.push_back( t3dRdTextureUniform( 2, m_hTerrainSampler, rHeight ) );
	uniforms.push_back( t3dRdTextureUniform( 3, m_hTerrainSampler, rControl ) );
	uniforms.push_back( t3dRdTextureUniform( 4, m_hTerrainSampler, rColor ) );
	uniforms.push_back( t3dRdTextureUniform( 5, m_hMaterialSampler, rAlbedo ) );
	uniforms.push_back( t3dRdTextureUniform( 6, m_hMaterialSampler, rNormal ) );
	uniforms.push_back( t3dRdTextureUniform( 7, m_hMaterialSampler, rSurface ) );
	uniforms.push_back( t3dRdBufferUniform( 8, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hMaterialBuffer ) );
	uniforms.push_back( t3dRdBufferUniform( 9, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hPatchBuffer ) );
	uniforms.push_back( t3dRdBufferUniform( 10, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hVisibleBuffer ) );
	uniforms.push_back( t3dRdTextureUniform( 12, m_hTerrainSampler, rDisplacement ) );
	for ( int i = 0; i < 3; ++i ) {
		m_hszRasterUniformSets[i] = rd->uniform_set_create( uniforms, m_hszShaders[i], 0u );
		if ( !m_hszRasterUniformSets[i].is_valid() ) { setError( "Failed to create Terrain3D raster uniform set" ); return false; }
		m_context.registerResource( m_hszRasterUniformSets[i], String( "T3D.UniformSet.Raster." ) + String::num_int64( i ) );
	}
	if ( m_bGpuCullingReady ) {
		TypedArray<Ref<RDUniform>> cull_uniforms;
		cull_uniforms.push_back( t3dRdBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameUniformBuffer ) );
		cull_uniforms.push_back( t3dRdBufferUniform( 9, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hPatchBuffer ) );
		cull_uniforms.push_back( t3dRdBufferUniform( 10, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hVisibleBuffer ) );
		cull_uniforms.push_back( t3dRdBufferUniform( 11, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hIndirectBuffer ) );
		m_hCullUniformSet = rd->uniform_set_create( cull_uniforms, m_hCullShader, 0u );
		if ( !m_hCullUniformSet.is_valid() ) m_bGpuCullingReady = false;
	}
	m_hBoundHeightTexture = rHeight;
	m_hBoundControlTexture = rControl;
	m_hBoundColorTexture = rColor;
	m_hBoundAlbedoTexture = rAlbedo;
	m_hBoundNormalTexture = rNormal;
	m_hBoundSurfaceTexture = rSurface;
	m_hBoundDisplacementTexture = rDisplacement;
	return true;
}

void Terrain3DRdClipmapRenderer::freeViewState( t3dRdViewState_t &rState ) {
	for ( RID &pipeline : rState.m_hszPipelines ) m_context.freeResource( pipeline );
	m_context.freeResource( rState.m_hFramebuffer );
	rState = t3dRdViewState_t{};
}

bool Terrain3DRdClipmapRenderer::ensureViewState( const uint32_t nView,
		const RID &rColor,
		const RID &rDepth,
		const RenderingDevice::TextureSamples samples,
		const bool bColorWrite ) {
	if ( nView >= s_nMaxViews ) return false;
	t3dRdViewState_t &state = m_szViewStates[nView];
	if ( state.m_hFramebuffer.is_valid() && state.m_hColor == rColor && state.m_hDepth == rDepth &&
			state.m_samples == samples && state.m_bColorWrite == bColorWrite ) return true;
	freeViewState( state );
	RenderingDevice *const rd = m_context.getDevice();
	TypedArray<RID> attachments;
	attachments.push_back( rColor );
	attachments.push_back( rDepth );
	state.m_hFramebuffer = rd->framebuffer_create( attachments, -1, 1u );
	if ( !state.m_hFramebuffer.is_valid() ) { setError( "Failed to create Terrain3D per-view framebuffer" ); return false; }
	m_context.registerResource( state.m_hFramebuffer, String( "T3D.Framebuffer.View" ) + String::num_int64( nView ) );
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
		if ( !state.m_hszPipelines[i].is_valid() ) { setError( "Failed to create Terrain3D raster pipeline" ); freeViewState( state ); return false; }
		m_context.registerResource( state.m_hszPipelines[i], String( "T3D.Pipeline.View" ) + String::num_int64( nView ) + "." + String::num_int64( i ) );
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
		const Ref<RDShaderSPIRV> &rCull ) {
	if ( isReady() ) return true;
	if ( !ensureDevice() || !ensureShaders( rNear, rMiddle, rFar, rCull ) || !ensureSamplers() ||
			!ensureFallbackTextures() || !ensureBuffers() ) return false;
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
	if ( m_nVertexFormat == RenderingDevice::INVALID_FORMAT_ID ) { setError( "Failed to create packed Terrain3D vertex format" ); return false; }
	if ( !m_szTopology[LOD0_MESH].upload( m_context, m_nVertexFormat ) ||
			!m_szTopology[OUTER_RING_MESH].upload( m_context, m_nVertexFormat ) ) {
		setError( "Failed to upload packed Terrain3D topology" );
		return false;
	}
	return true;
}

void Terrain3DRdClipmapRenderer::shutdown( void ) {
	if ( !m_context.isReady() ) return;
	for ( t3dRdViewState_t &state : m_szViewStates ) freeViewState( state );
	freeUniformSets();
	m_szTopology[LOD0_MESH].releaseGpu( m_context );
	m_szTopology[OUTER_RING_MESH].releaseGpu( m_context );
	m_context.freeResource( m_hIndirectBuffer, sizeof( t3dRdIndexedIndirectCommand_t ) * s_nRdDrawGroupCount );
	m_context.freeResource( m_hVisibleBuffer, sizeof( uint32_t ) * s_nRdDrawGroupCount * s_nMaxClipmapLevels );
	m_context.freeResource( m_hPatchBuffer, sizeof( t3dRdPatch_t ) * s_nMaxClipmapLevels );
	m_context.freeResource( m_hMaterialBuffer, sizeof( t3dRdMaterialLayer_t ) * s_nRdMaterialLayerCount );
	m_context.freeResource( m_hRegionMapBuffer, m_nRegionMapCapacityBytes );
	m_context.freeResource( m_hFrameUniformBuffer, sizeof( t3dRdFrameUniforms_t ) );
	m_context.freeTextureResource( m_hFallbackDisplacementTexture, 4u * sizeof( float ) );
	m_context.freeResource( m_hMaterialSampler );
	m_context.freeResource( m_hTerrainSampler );
	m_context.freeResource( m_hCullPipeline );
	m_context.freeResource( m_hCullShader );
	for ( RID &shader : m_hszShaders ) m_context.freeResource( shader );
	m_context.shutdown();
	m_nVertexFormat = RenderingDevice::INVALID_FORMAT_ID;
	m_nRegionMapCapacityBytes = 0u;
	m_nRegionMapGeneration = 0u;
	m_nMaterialGeneration = 0u;
	m_nPatchGeneration = 0u;
	m_bGpuCullingReady = false;
	m_frameUploadBytes.clear();
	m_materialUploadBytes.clear();
	m_patchUploadBytes.clear();
	m_cullPushBytes.clear();
	m_cpuVisibleBytes.clear();
	m_cpuIndirectBytes.clear();
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
		const Projection &rViewProjection,
		const Vector3 &rCamera ) {
	t3dRdFrameUniforms_t frame{};
	copyProjection( rViewProjection, frame.m_mViewProjection );
	frame.m_fLightDirectionX = ( float )( rSnapshot.m_lightDirection.x );
	frame.m_fLightDirectionY = ( float )( rSnapshot.m_lightDirection.y );
	frame.m_fLightDirectionZ = ( float )( rSnapshot.m_lightDirection.z );
	frame.m_fAmbient = rSnapshot.m_fAmbient;
	frame.m_fLightColorR = ( float )( rSnapshot.m_lightColor.r );
	frame.m_fLightColorG = ( float )( rSnapshot.m_lightColor.g );
	frame.m_fLightColorB = ( float )( rSnapshot.m_lightColor.b );
	frame.m_fLightColorA = ( float )( rSnapshot.m_lightColor.a );
	frame.m_fRegionSize = ( float )( rSnapshot.m_nRegionSize );
	frame.m_fRegionMapSize = ( float )( rSnapshot.m_nRegionMapSize );
	frame.m_fHeightMin = rSnapshot.m_fHeightMin;
	frame.m_fHeightRange = rSnapshot.m_fHeightRange;
	frame.m_fVertexSpacing = rSnapshot.m_fVertexSpacing;
	frame.m_fMeshSize = ( float )( rSnapshot.m_nMeshSize );
	frame.m_fMaterialLayerCount = ( float )( MAX( rSnapshot.m_nMaterialLayerCount, 1u ) );
	frame.m_fTargetX = ( float )( rSnapshot.m_targetPosition.x );
	frame.m_fTargetZ = ( float )( rSnapshot.m_targetPosition.z );
	frame.m_fSubdiv = MAX( rSnapshot.m_fSubdiv, 1.0f );
	frame.m_fCameraX = ( float )( rCamera.x );
	frame.m_fCameraY = ( float )( rCamera.y );
	frame.m_fCameraZ = ( float )( rCamera.z );
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
	std::memcpy( m_frameUploadBytes.ptrw(), &frame, sizeof( frame ) );
	if ( m_context.getDevice()->buffer_update( m_hFrameUniformBuffer, 0u, sizeof( frame ), m_frameUploadBytes ) != OK ) return false;
	m_context.recordBufferUpdate( sizeof( frame ) );
	return true;
}

bool Terrain3DRdClipmapRenderer::uploadSnapshotBuffers( const t3dRdFrameSnapshot_t &rSnapshot,
		const PackedByteArray &rRegionMapBytes,
		const uint64_t nRegionMapGeneration ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !ensureRegionMapBuffer( ( uint32_t )( rRegionMapBytes.size() ) ) ) return false;
	if ( nRegionMapGeneration != m_nRegionMapGeneration ) {
		if ( rd->buffer_update( m_hRegionMapBuffer, 0u, ( uint32_t )( rRegionMapBytes.size() ), rRegionMapBytes ) != OK ) return false;
		m_context.recordBufferUpdate( ( uint64_t )( rRegionMapBytes.size() ) );
		m_nRegionMapGeneration = nRegionMapGeneration;
	}
	if ( rSnapshot.m_nMaterialGeneration != m_nMaterialGeneration ) {
		std::memcpy( m_materialUploadBytes.ptrw(), rSnapshot.m_szMaterials, sizeof( rSnapshot.m_szMaterials ) );
		if ( rd->buffer_update( m_hMaterialBuffer, 0u, sizeof( rSnapshot.m_szMaterials ), m_materialUploadBytes ) != OK ) return false;
		m_context.recordBufferUpdate( sizeof( rSnapshot.m_szMaterials ) );
		m_nMaterialGeneration = rSnapshot.m_nMaterialGeneration;
	}
	if ( rSnapshot.m_nGeneration != m_nPatchGeneration ) {
		std::memcpy( m_patchUploadBytes.ptrw(), rSnapshot.m_szPatches, sizeof( rSnapshot.m_szPatches ) );
		if ( rd->buffer_update( m_hPatchBuffer, 0u, sizeof( rSnapshot.m_szPatches ), m_patchUploadBytes ) != OK ) return false;
		m_context.recordBufferUpdate( sizeof( rSnapshot.m_szPatches ) );
		m_nPatchGeneration = rSnapshot.m_nGeneration;
		++m_nPatchUploads;
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::runGpuCulling( const t3dRdFrameSnapshot_t &rSnapshot ) {
	if ( !m_bGpuCullingReady || !m_hCullUniformSet.is_valid() ) return false;
	t3dRdCullPushConstants_t constants{};
	constants.m_nPatchCount = MIN( rSnapshot.m_nPatchCount, ( uint32_t )( s_nMaxClipmapLevels ) );
	constants.m_nLod0IndexCount = m_szTopology[LOD0_MESH].getIndexCount();
	constants.m_nOuterIndexCount = m_szTopology[OUTER_RING_MESH].getIndexCount();
	constants.m_nMaxPatches = s_nMaxClipmapLevels;
	std::memcpy( m_cullPushBytes.ptrw(), &constants, sizeof( constants ) );
	RenderingDevice *const rd = m_context.getDevice();
	const int64_t compute_list = rd->compute_list_begin();
	if ( compute_list == RenderingDevice::INVALID_ID ) return false;
	rd->compute_list_bind_compute_pipeline( compute_list, m_hCullPipeline );
	rd->compute_list_bind_uniform_set( compute_list, m_hCullUniformSet, 0u );
	rd->compute_list_set_push_constant( compute_list, m_cullPushBytes, sizeof( constants ) );
	rd->compute_list_dispatch( compute_list, 1u, 1u, 1u );
	rd->compute_list_add_barrier( compute_list );
	rd->compute_list_end();
	++m_nCullDispatches;
	return true;
}

bool Terrain3DRdClipmapRenderer::buildCpuIndirect( const t3dRdFrameSnapshot_t &rSnapshot ) {
	uint32_t *const visible = ( uint32_t * )( m_cpuVisibleBytes.ptrw() );
	t3dRdIndexedIndirectCommand_t *const commands = ( t3dRdIndexedIndirectCommand_t * )( m_cpuIndirectBytes.ptrw() );
	std::memset( visible, 0, ( size_t )( m_cpuVisibleBytes.size() ) );
	std::memset( commands, 0, ( size_t )( m_cpuIndirectBytes.size() ) );
	commands[0].m_nIndexCount = m_szTopology[LOD0_MESH].getIndexCount();
	commands[1].m_nIndexCount = m_szTopology[OUTER_RING_MESH].getIndexCount();
	commands[2].m_nIndexCount = m_szTopology[OUTER_RING_MESH].getIndexCount();
	commands[3].m_nIndexCount = m_szTopology[OUTER_RING_MESH].getIndexCount();
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) commands[group].m_nFirstInstance = group * s_nMaxClipmapLevels;
	const uint32_t count = MIN( rSnapshot.m_nPatchCount, ( uint32_t )( s_nMaxClipmapLevels ) );
	for ( uint32_t i = 0u; i < count; ++i ) {
		const t3dRdPatch_t &patch = rSnapshot.m_szPatches[i];
		uint32_t group = patch.m_nLod == 0u ? 0u : ( patch.m_nTier == 0u ? 1u : ( patch.m_nTier == 1u ? 2u : 3u ) );
		const uint32_t slot = commands[group].m_nInstanceCount++;
		if ( slot < s_nMaxClipmapLevels ) visible[group * s_nMaxClipmapLevels + slot] = i;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( rd->buffer_update( m_hVisibleBuffer, 0u, ( uint32_t )( m_cpuVisibleBytes.size() ), m_cpuVisibleBytes ) != OK ) return false;
	if ( rd->buffer_update( m_hIndirectBuffer, 0u, ( uint32_t )( m_cpuIndirectBytes.size() ), m_cpuIndirectBytes ) != OK ) return false;
	m_context.recordBufferUpdate( ( uint64_t )( m_cpuVisibleBytes.size() + m_cpuIndirectBytes.size() ) );
	return true;
}

bool Terrain3DRdClipmapRenderer::drawView( const uint32_t nView ) {
	if ( nView >= s_nMaxViews ) return false;
	RenderingDevice *const rd = m_context.getDevice();
	t3dRdViewState_t &state = m_szViewStates[nView];
	const int64_t draw_list = rd->draw_list_begin( state.m_hFramebuffer, RenderingDevice::DRAW_DEFAULT_ALL );
	if ( draw_list == RenderingDevice::INVALID_ID ) return false;
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
		const int tier = group <= 1u ? 0 : ( group == 2u ? 1 : 2 );
		const Terrain3DRdTopology &topology = group == 0u ? m_szTopology[LOD0_MESH] : m_szTopology[OUTER_RING_MESH];
		rd->draw_list_bind_render_pipeline( draw_list, state.m_hszPipelines[tier] );
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

bool Terrain3DRdClipmapRenderer::render( RenderData *const pRenderData,
		const t3dRdFrameSnapshot_t &rSnapshot,
		const PackedByteArray &rRegionMapBytes,
		const uint64_t nRegionMapGeneration,
		const RID &rHeightTextureRs,
		const RID &rControlTextureRs,
		const RID &rColorTextureRs,
		const RID &rAlbedoTextureRs,
		const RID &rNormalTextureRs,
		const RID &rSurfaceTextureRs,
		const RID &rDisplacementTextureRs,
		const bool bColorWrite ) {
	if ( !pRenderData || !isReady() || rSnapshot.m_nPatchCount == 0u || rRegionMapBytes.is_empty() ) return false;
	Ref<RenderSceneBuffers> scene_buffers = pRenderData->get_render_scene_buffers();
	Ref<RenderSceneBuffersRD> buffers = scene_buffers;
	RenderSceneData *const scene_data = pRenderData->get_render_scene_data();
	if ( buffers.is_null() || !scene_data ) { setError( "RenderSceneBuffersRD/RenderSceneData unavailable" ); return false; }
	const uint32_t view_count = buffers->get_view_count();
	if ( view_count == 0u || view_count > s_nMaxViews ) { setError( "Unsupported Terrain3D RD view count" ); return false; }
	RenderingServer *const server = RenderingServer::get_singleton();
	if ( !server ) return false;
	const RID height = server->texture_get_rd_texture( rHeightTextureRs );
	const RID control = server->texture_get_rd_texture( rControlTextureRs );
	const RID color_map = server->texture_get_rd_texture( rColorTextureRs, true );
	RID albedo = server->texture_get_rd_texture( rAlbedoTextureRs, true );
	RID normal = server->texture_get_rd_texture( rNormalTextureRs, true );
	RID surface = server->texture_get_rd_texture( rSurfaceTextureRs, true );
	RID displacement = rDisplacementTextureRs.is_valid() ? server->texture_get_rd_texture( rDisplacementTextureRs ) : RID();
	if ( !albedo.is_valid() ) albedo = color_map;
	if ( !surface.is_valid() ) surface = normal.is_valid() ? normal : albedo;
	if ( !normal.is_valid() ) normal = surface;
	if ( !displacement.is_valid() ) displacement = m_hFallbackDisplacementTexture;
	if ( !height.is_valid() || !control.is_valid() || !color_map.is_valid() || !albedo.is_valid() || !normal.is_valid() || !surface.is_valid() || !displacement.is_valid() ) {
		setError( "Terrain textures could not be bridged to RenderingDevice" );
		return false;
	}
	if ( !uploadSnapshotBuffers( rSnapshot, rRegionMapBytes, nRegionMapGeneration ) ) { setError( "Failed to upload Terrain3D RD frame data" ); return false; }
	if ( !ensureUniformSets( height, control, color_map, albedo, normal, surface, displacement ) ) return false;
	const RenderingDevice::TextureSamples samples = buffers->get_texture_samples();
	const bool bMsaa = samples != RenderingDevice::TEXTURE_SAMPLES_1;
	const Transform3D camera_transform = scene_data->get_cam_transform();
	for ( uint32_t view = 0u; view < view_count; ++view ) {
		RID target_color = view_count > 1u ? buffers->get_color_layer( view, bMsaa ) : buffers->get_color_texture( bMsaa );
		RID target_depth = view_count > 1u ? buffers->get_depth_layer( view, bMsaa ) : buffers->get_depth_texture( bMsaa );
		if ( !target_color.is_valid() || !target_depth.is_valid() ||
				!ensureViewState( view, target_color, target_depth, samples, bColorWrite ) ) return false;
		const Projection view_matrix = Projection( camera_transform.affine_inverse() );
		const Projection view_projection = scene_data->get_view_projection( view ) * view_matrix;
		Vector3 eye_position = camera_transform.origin;
		if ( view_count > 1u ) eye_position += camera_transform.basis.xform( scene_data->get_view_eye_offset( view ) );
		if ( !updateFrameUniforms( rSnapshot, view_projection, eye_position ) ) return false;
		if ( !runGpuCulling( rSnapshot ) && !buildCpuIndirect( rSnapshot ) ) return false;
		if ( !drawView( view ) ) return false;
	}
	++m_nFramesRendered;
	clearError();
	return true;
}

Dictionary Terrain3DRdClipmapRenderer::getDiagnostics( void ) const {
	Dictionary result = m_context.getDiagnostics();
	result["frames_rendered"] = ( int64_t )( m_nFramesRendered.load( std::memory_order_acquire ) );
	result["draws_rendered"] = ( int64_t )( m_nDrawsRendered.load( std::memory_order_acquire ) );
	result["indirect_draws"] = ( int64_t )( m_nIndirectDraws.load( std::memory_order_acquire ) );
	result["cull_dispatches"] = ( int64_t )( m_nCullDispatches.load( std::memory_order_acquire ) );
	result["patch_uploads"] = ( int64_t )( m_nPatchUploads.load( std::memory_order_acquire ) );
	result["topology_bytes"] = ( int64_t )( getTopologyBytes() );
	result["lod0_index_bits"] = m_szTopology[LOD0_MESH].getIndexFormat() == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ? 16 : 32;
	result["outer_index_bits"] = m_szTopology[OUTER_RING_MESH].getIndexFormat() == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ? 16 : 32;
	result["gpu_culling"] = m_bGpuCullingReady;
	result["fallback_displacement_texture"] = m_hFallbackDisplacementTexture.is_valid();
	result["max_views"] = ( int64_t )( s_nMaxViews );
	result["last_error"] = getLastError();
	return result;
}
