// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_clipmap_renderer.h"

#include "core/logger.h"

#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/render_scene_buffers.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/rendering_server.hpp>

#include <cstring>

CLASS_NAME_STATIC( "Terrain3DRdClipmapRenderer" );

static Ref<RDUniform> t3dRdMakeBufferUniform( const int nBinding,
		const RenderingDevice::UniformType type,
		const RID &rBuffer ) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_uniform_type( type );
	uniform->set_binding( nBinding );
	uniform->add_id( rBuffer );
	return uniform;
}

static Ref<RDUniform> t3dRdMakeSampledTextureUniform(
		const int nBinding, const RID &rSampler, const RID &rTexture ) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE );
	uniform->set_binding( nBinding );
	uniform->add_id( rSampler );
	uniform->add_id( rTexture );
	return uniform;
}

Terrain3DRdClipmapRenderer::~Terrain3DRdClipmapRenderer( void ) {
	shutdown();
}

void Terrain3DRdClipmapRenderer::setError( const String &rError ) {
	{
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_lastError = rError;
	}
	LOG( ERROR, "[RD experiment] ", rError );
}

String Terrain3DRdClipmapRenderer::getLastError( void ) const {
	std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
	return m_lastError;
}

void Terrain3DRdClipmapRenderer::freeRid( RID &rRid ) {
	if ( m_pRd && rRid.is_valid() ) {
		m_pRd->free_rid( rRid );
	}
	rRid = RID();
}

void Terrain3DRdClipmapRenderer::freeUniformSet( void ) {
	freeRid( m_hUniformSet );
	m_hBoundHeightTexture = RID();
	m_hBoundControlTexture = RID();
	m_hBoundColorTexture = RID();
	m_hBoundAlbedoTexture = RID();
}

void Terrain3DRdClipmapRenderer::freeFramebufferState( void ) {
	freeRid( m_hPipeline );
	freeRid( m_hFramebuffer );
	m_hFramebufferColor = RID();
	m_hFramebufferDepth = RID();
	m_framebufferSamples = RenderingDevice::TEXTURE_SAMPLES_1;
	m_nFramebufferViewCount = 0u;
}

bool Terrain3DRdClipmapRenderer::ensureDevice( void ) {
	if ( m_pRd ) {
		return true;
	}
	RenderingServer *const server = RenderingServer::get_singleton();
	m_pRd = server ? server->get_rendering_device() : nullptr;
	if ( !m_pRd ) {
		setError( "Global RenderingDevice is unavailable; Forward+ or Mobile is required" );
		return false;
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureShader( const Ref<RDShaderSPIRV> &rSpirv ) {
	if ( m_hShader.is_valid() ) {
		return true;
	}
	if ( rSpirv.is_null() ) {
		setError( "RD clipmap shader has no SPIR-V" );
		return false;
	}
	m_hShader = m_pRd->shader_create_from_spirv( rSpirv );
	if ( !m_hShader.is_valid() ) {
		setError( "Failed to create RD clipmap shader" );
		return false;
	}
	m_pRd->set_resource_name( m_hShader, "Terrain3D.RDExperiment.ClipmapShader" );
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureSamplers( void ) {
	if ( !m_hTerrainSampler.is_valid() ) {
		Ref<RDSamplerState> terrain_state;
		terrain_state.instantiate();
		terrain_state->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		terrain_state->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		terrain_state->set_mip_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		terrain_state->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		terrain_state->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		terrain_state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_hTerrainSampler = m_pRd->sampler_create( terrain_state );
		if ( !m_hTerrainSampler.is_valid() ) {
			setError( "Failed to create RD terrain-data sampler" );
			return false;
		}
		m_pRd->set_resource_name( m_hTerrainSampler, "Terrain3D.RDExperiment.TerrainSampler" );
	}

	if ( !m_hMaterialSampler.is_valid() ) {
		Ref<RDSamplerState> material_state;
		material_state.instantiate();
		material_state->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		material_state->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		material_state->set_mip_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		material_state->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT );
		material_state->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT );
		material_state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_hMaterialSampler = m_pRd->sampler_create( material_state );
		if ( !m_hMaterialSampler.is_valid() ) {
			setError( "Failed to create RD terrain-material sampler" );
			return false;
		}
		m_pRd->set_resource_name( m_hMaterialSampler, "Terrain3D.RDExperiment.MaterialSampler" );
	}
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureFrameUniformBuffer( void ) {
	if ( m_hFrameUniformBuffer.is_valid() ) {
		return true;
	}
	m_hFrameUniformBuffer = m_pRd->uniform_buffer_create( sizeof( t3dRdFrameUniforms_t ) );
	if ( !m_hFrameUniformBuffer.is_valid() ) {
		setError( "Failed to create RD frame UBO" );
		return false;
	}
	m_pRd->set_resource_name( m_hFrameUniformBuffer, "Terrain3D.RDExperiment.FrameUBO" );
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureRegionMapBuffer( const uint32_t nRequiredBytes ) {
	if ( nRequiredBytes == 0u ) {
		return false;
	}
	if ( m_hRegionMapBuffer.is_valid() && nRequiredBytes <= m_nRegionMapCapacityBytes ) {
		return true;
	}
	freeUniformSet();
	freeRid( m_hRegionMapBuffer );
	m_nRegionMapCapacityBytes = MAX( 64u, nRequiredBytes );
	m_hRegionMapBuffer = m_pRd->storage_buffer_create( m_nRegionMapCapacityBytes );
	if ( !m_hRegionMapBuffer.is_valid() ) {
		setError( "Failed to create RD region page-table buffer" );
		m_nRegionMapCapacityBytes = 0u;
		return false;
	}
	m_pRd->set_resource_name( m_hRegionMapBuffer, "Terrain3D.RDExperiment.RegionMap" );
	m_nRegionMapGeneration = 0u;
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureUniformSet( const RID &rHeightTexture,
		const RID &rControlTexture,
		const RID &rColorTexture,
		const RID &rAlbedoTexture ) {
	if ( m_hUniformSet.is_valid() && m_hBoundHeightTexture == rHeightTexture &&
			m_hBoundControlTexture == rControlTexture && m_hBoundColorTexture == rColorTexture &&
			m_hBoundAlbedoTexture == rAlbedoTexture ) {
		return true;
	}
	freeUniformSet();
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( t3dRdMakeBufferUniform(
			0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameUniformBuffer ) );
	uniforms.push_back( t3dRdMakeBufferUniform(
			1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hRegionMapBuffer ) );
	uniforms.push_back( t3dRdMakeSampledTextureUniform( 2, m_hTerrainSampler, rHeightTexture ) );
	uniforms.push_back( t3dRdMakeSampledTextureUniform( 3, m_hTerrainSampler, rControlTexture ) );
	uniforms.push_back( t3dRdMakeSampledTextureUniform( 4, m_hTerrainSampler, rColorTexture ) );
	uniforms.push_back( t3dRdMakeSampledTextureUniform( 5, m_hMaterialSampler, rAlbedoTexture ) );
	m_hUniformSet = m_pRd->uniform_set_create( uniforms, m_hShader, 0u );
	if ( !m_hUniformSet.is_valid() ) {
		setError( "Failed to create RD terrain uniform set" );
		return false;
	}
	m_hBoundHeightTexture = rHeightTexture;
	m_hBoundControlTexture = rControlTexture;
	m_hBoundColorTexture = rColorTexture;
	m_hBoundAlbedoTexture = rAlbedoTexture;
	return true;
}

bool Terrain3DRdClipmapRenderer::ensureFramebufferAndPipeline( const RID &rColor,
		const RID &rDepth,
		const RenderingDevice::TextureSamples samples,
		const uint32_t nViewCount ) {
	if ( m_hFramebuffer.is_valid() && m_hPipeline.is_valid() && m_hFramebufferColor == rColor &&
			m_hFramebufferDepth == rDepth && m_framebufferSamples == samples &&
			m_nFramebufferViewCount == nViewCount ) {
		return true;
	}
	freeFramebufferState();
	TypedArray<RID> attachments;
	attachments.push_back( rColor );
	attachments.push_back( rDepth );
	m_hFramebuffer = m_pRd->framebuffer_create( attachments, -1, nViewCount );
	if ( !m_hFramebuffer.is_valid() ) {
		setError( "Failed to create RD terrain framebuffer for scene color/depth" );
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
	Ref<RDPipelineColorBlendStateAttachment> attachment;
	attachment.instantiate();
	attachment->set_enable_blend( false );
	Ref<RDPipelineColorBlendState> blend;
	blend.instantiate();
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> blend_attachments;
	blend_attachments.push_back( attachment );
	blend->set_attachments( blend_attachments );

	m_hPipeline = m_pRd->render_pipeline_create( m_hShader,
			m_pRd->framebuffer_get_format( m_hFramebuffer ),
			m_nVertexFormat,
			RenderingDevice::RENDER_PRIMITIVE_TRIANGLES,
			raster,
			multisample,
			depth,
			blend );
	if ( !m_hPipeline.is_valid() ) {
		setError( "Failed to create RD terrain raster pipeline" );
		freeFramebufferState();
		return false;
	}
	m_pRd->set_resource_name( m_hPipeline, "Terrain3D.RDExperiment.ClipmapPipeline" );
	m_hFramebufferColor = rColor;
	m_hFramebufferDepth = rDepth;
	m_framebufferSamples = samples;
	m_nFramebufferViewCount = nViewCount;
	return true;
}

bool Terrain3DRdClipmapRenderer::createTopology( const int nType,
		const PackedByteArray &rVertexBytes,
		const uint32_t nVertexCount,
		const PackedByteArray &rIndexBytes,
		const uint32_t nIndexCount ) {
	if ( nType < 0 || nType >= MESH_TYPE_COUNT || rVertexBytes.is_empty() || rIndexBytes.is_empty() ||
			nVertexCount == 0u || nIndexCount == 0u ) {
		return false;
	}
	t3dRdTopology_t &topology = m_szTopology[( size_t )( nType )];
	topology.m_hVertexBuffer = m_pRd->vertex_buffer_create( ( uint32_t )( rVertexBytes.size() ), rVertexBytes );
	topology.m_hIndexBuffer = m_pRd->index_buffer_create(
			nIndexCount, RenderingDevice::INDEX_BUFFER_FORMAT_UINT32, rIndexBytes );
	if ( !topology.m_hVertexBuffer.is_valid() || !topology.m_hIndexBuffer.is_valid() ) {
		setError( "Failed to upload RD clipmap topology" );
		return false;
	}
	TypedArray<RID> vertex_buffers;
	vertex_buffers.push_back( topology.m_hVertexBuffer );
	topology.m_hVertexArray = m_pRd->vertex_array_create( nVertexCount, m_nVertexFormat, vertex_buffers );
	topology.m_hIndexArray = m_pRd->index_array_create( topology.m_hIndexBuffer, 0u, nIndexCount );
	if ( !topology.m_hVertexArray.is_valid() || !topology.m_hIndexArray.is_valid() ) {
		setError( "Failed to create RD clipmap vertex/index arrays" );
		return false;
	}
	topology.m_nVertexCount = nVertexCount;
	topology.m_nIndexCount = nIndexCount;
	m_nTopologyBytes += ( uint64_t )( rVertexBytes.size() + rIndexBytes.size() );
	const String prefix = nType == LOD0_MESH ? "Terrain3D.RDExperiment.Lod0" : "Terrain3D.RDExperiment.Outer";
	m_pRd->set_resource_name( topology.m_hVertexBuffer, prefix + String( ".VB" ) );
	m_pRd->set_resource_name( topology.m_hIndexBuffer, prefix + String( ".IB" ) );
	return true;
}

bool Terrain3DRdClipmapRenderer::initialize( const Ref<RDShaderSPIRV> &rSpirv,
		const PackedByteArray &rLod0Vertices,
		const uint32_t nLod0VertexCount,
		const PackedByteArray &rLod0Indices,
		const uint32_t nLod0IndexCount,
		const PackedByteArray &rOuterVertices,
		const uint32_t nOuterVertexCount,
		const PackedByteArray &rOuterIndices,
		const uint32_t nOuterIndexCount ) {
	if ( isReady() ) {
		return true;
	}
	if ( !ensureDevice() || !ensureShader( rSpirv ) || !ensureSamplers() || !ensureFrameUniformBuffer() ) {
		return false;
	}
	TypedArray<Ref<RDVertexAttribute>> attributes;
	Ref<RDVertexAttribute> position;
	position.instantiate();
	position->set_location( 0u );
	position->set_binding( 0u );
	position->set_format( RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT );
	position->set_offset( 0u );
	position->set_stride( sizeof( t3dRdClipmapVertex_t ) );
	position->set_frequency( RenderingDevice::VERTEX_FREQUENCY_VERTEX );
	attributes.push_back( position );
	Ref<RDVertexAttribute> part;
	part.instantiate();
	part->set_location( 1u );
	part->set_binding( 0u );
	part->set_format( RenderingDevice::DATA_FORMAT_R32_SFLOAT );
	part->set_offset( 12u );
	part->set_stride( sizeof( t3dRdClipmapVertex_t ) );
	part->set_frequency( RenderingDevice::VERTEX_FREQUENCY_VERTEX );
	attributes.push_back( part );
	m_nVertexFormat = m_pRd->vertex_format_create( attributes );
	if ( m_nVertexFormat == RenderingDevice::INVALID_FORMAT_ID ) {
		setError( "Failed to create RD clipmap vertex format" );
		return false;
	}
	m_frameUploadBytes.resize( sizeof( t3dRdFrameUniforms_t ) );
	m_pushConstantBytes.resize( sizeof( t3dRdDrawPushConstants_t ) );
	return createTopology( LOD0_MESH, rLod0Vertices, nLod0VertexCount, rLod0Indices, nLod0IndexCount ) &&
			createTopology( OUTER_RING_MESH, rOuterVertices, nOuterVertexCount, rOuterIndices, nOuterIndexCount );
}

void Terrain3DRdClipmapRenderer::shutdown( void ) {
	if ( !m_pRd ) {
		return;
	}
	freeFramebufferState();
	freeUniformSet();
	for ( t3dRdTopology_t &topology : m_szTopology ) {
		freeRid( topology.m_hIndexArray );
		freeRid( topology.m_hVertexArray );
		freeRid( topology.m_hIndexBuffer );
		freeRid( topology.m_hVertexBuffer );
		topology = t3dRdTopology_t{};
	}
	freeRid( m_hRegionMapBuffer );
	freeRid( m_hFrameUniformBuffer );
	freeRid( m_hMaterialSampler );
	freeRid( m_hTerrainSampler );
	freeRid( m_hShader );
	m_pRd = nullptr;
	m_nVertexFormat = RenderingDevice::INVALID_FORMAT_ID;
	m_nRegionMapCapacityBytes = 0u;
	m_nRegionMapGeneration = 0u;
	m_nTopologyBytes = 0u;
	m_frameUploadBytes.clear();
	m_pushConstantBytes.clear();
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

bool Terrain3DRdClipmapRenderer::render( RenderData *const pRenderData,
		const t3dRdFrameSnapshot_t &rSnapshot,
		const PackedByteArray &rRegionMapBytes,
		const uint64_t nRegionMapGeneration,
		const RID &rHeightTextureRs,
		const RID &rControlTextureRs,
		const RID &rColorTextureRs,
		const RID &rAlbedoTextureRs ) {
	if ( !pRenderData || !isReady() || rSnapshot.m_nPatchCount == 0u || rRegionMapBytes.is_empty() ) {
		return false;
	}
	Ref<RenderSceneBuffers> scene_buffers = pRenderData->get_render_scene_buffers();
	Ref<RenderSceneBuffersRD> buffers = scene_buffers;
	RenderSceneData *const scene_data = pRenderData->get_render_scene_data();
	if ( buffers.is_null() || !scene_data || buffers->get_view_count() != 1u ) {
		setError( "RD experiment currently requires a single-view RenderSceneBuffersRD viewport" );
		return false;
	}
	RenderingServer *const server = RenderingServer::get_singleton();
	if ( !server ) {
		return false;
	}
	const RID height_texture = server->texture_get_rd_texture( rHeightTextureRs );
	const RID control_texture = server->texture_get_rd_texture( rControlTextureRs );
	const RID color_texture = server->texture_get_rd_texture( rColorTextureRs, true );
	RID albedo_texture = server->texture_get_rd_texture( rAlbedoTextureRs, true );
	if ( !albedo_texture.is_valid() ) {
		albedo_texture = color_texture;
	}
	if ( !height_texture.is_valid() || !control_texture.is_valid() || !color_texture.is_valid() ||
			!albedo_texture.is_valid() ) {
		setError( "Terrain source textures cannot be bridged to the global RenderingDevice" );
		return false;
	}
	if ( !ensureRegionMapBuffer( ( uint32_t )( rRegionMapBytes.size() ) ) ) {
		return false;
	}
	if ( nRegionMapGeneration != m_nRegionMapGeneration ) {
		if ( m_pRd->buffer_update( m_hRegionMapBuffer,
					0u,
					( uint32_t )( rRegionMapBytes.size() ),
					rRegionMapBytes ) != OK ) {
			setError( "Failed to upload RD region page table" );
			return false;
		}
		m_nRegionMapGeneration = nRegionMapGeneration;
	}
	if ( !ensureUniformSet( height_texture, control_texture, color_texture, albedo_texture ) ) {
		return false;
	}

	const RenderingDevice::TextureSamples samples = buffers->get_texture_samples();
	const bool bMsaa = samples != RenderingDevice::TEXTURE_SAMPLES_1;
	const RID color = buffers->get_color_texture( bMsaa );
	const RID depth = buffers->get_depth_texture( bMsaa );
	if ( !color.is_valid() || !depth.is_valid() ||
			!ensureFramebufferAndPipeline( color, depth, samples, buffers->get_view_count() ) ) {
		return false;
	}

	t3dRdFrameUniforms_t frame{};
	const Projection view = Projection( scene_data->get_cam_transform().affine_inverse() );
	const Projection view_projection = scene_data->get_view_projection( 0u ) * view;
	copyProjection( view_projection, frame.m_mViewProjection );
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
	if ( m_frameUploadBytes.size() != ( int64_t )( sizeof( frame ) ) ) {
		m_frameUploadBytes.resize( sizeof( frame ) );
	}
	std::memcpy( m_frameUploadBytes.ptrw(), &frame, sizeof( frame ) );
	if ( m_pRd->buffer_update( m_hFrameUniformBuffer, 0u, sizeof( frame ), m_frameUploadBytes ) != OK ) {
		setError( "Failed to update RD frame UBO" );
		return false;
	}

	const int64_t draw_list = m_pRd->draw_list_begin( m_hFramebuffer, RenderingDevice::DRAW_DEFAULT_ALL );
	if ( draw_list == RenderingDevice::INVALID_ID ) {
		setError( "Failed to begin RD terrain draw list" );
		return false;
	}
	m_pRd->draw_list_bind_render_pipeline( draw_list, m_hPipeline );
	m_pRd->draw_list_bind_uniform_set( draw_list, m_hUniformSet, 0u );
	int current_topology = -1;
	uint64_t frame_draws = 0u;
	if ( m_pushConstantBytes.size() != ( int64_t )( sizeof( t3dRdDrawPushConstants_t ) ) ) {
		m_pushConstantBytes.resize( sizeof( t3dRdDrawPushConstants_t ) );
	}
	const uint32_t patch_count = MIN( rSnapshot.m_nPatchCount, ( uint32_t )( s_nMaxClipmapLevels ) );
	for ( uint32_t i = 0u; i < patch_count; ++i ) {
		const t3dRdPatch_t &patch = rSnapshot.m_szPatches[i];
		const int topology_index = patch.m_nLod == 0u ? LOD0_MESH : OUTER_RING_MESH;
		const t3dRdTopology_t &topology = m_szTopology[( size_t )( topology_index )];
		if ( current_topology != topology_index ) {
			m_pRd->draw_list_bind_vertex_array( draw_list, topology.m_hVertexArray );
			m_pRd->draw_list_bind_index_array( draw_list, topology.m_hIndexArray );
			current_topology = topology_index;
		}
		t3dRdDrawPushConstants_t push{};
		push.m_fOriginX = patch.m_fOriginX;
		push.m_fOriginZ = patch.m_fOriginZ;
		push.m_fScale = patch.m_fScale;
		push.m_fLod = ( float )( patch.m_nLod );
		push.m_fTier = ( float )( patch.m_nTier );
		push.m_fStitchX = ( float )( patch.m_nStitchX );
		push.m_fStitchZ = ( float )( patch.m_nStitchZ );
		push.m_fFlags = ( float )( patch.m_nFlags );
		std::memcpy( m_pushConstantBytes.ptrw(), &push, sizeof( push ) );
		m_pRd->draw_list_set_push_constant( draw_list, m_pushConstantBytes, sizeof( push ) );
		m_pRd->draw_list_draw( draw_list, true, 1u );
		++frame_draws;
	}
	m_pRd->draw_list_end();
	++m_nFramesRendered;
	m_nDrawsRendered += frame_draws;
	{
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_lastError = String();
	}
	return true;
}
