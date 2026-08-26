// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_ocean_renderer.h"

#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <cstring>

static bool t3dRdOceanAdopt( Terrain3DRdContext &rContext, RenderingDevice *const pRd, RID &rRid,
		const String &rName, const Terrain3DRdContext::ResourceType type,
		const Terrain3DRdContext::MemoryCategory memory = Terrain3DRdContext::MEMORY_NONE, const uint64_t nBytes = 0u ) {
	if ( rContext.registerResource( rRid, rName, type, memory, nBytes ) ) return true;
	if ( pRd && rRid.is_valid() ) pRd->free_rid( rRid );
	rRid = RID();
	return false;
}

static Ref<RDUniform> t3dRdOceanBufferUniform( const int nBinding, const RenderingDevice::UniformType type, const RID &rRid ) {
	Ref<RDUniform> uniform; uniform.instantiate();
	uniform->set_uniform_type( type ); uniform->set_binding( nBinding ); uniform->add_id( rRid );
	return uniform;
}

Terrain3DRdOceanRenderer::Terrain3DRdOceanRenderer( void ) : m_context( Terrain3DRdContext::getShared() ) {}
Terrain3DRdOceanRenderer::~Terrain3DRdOceanRenderer( void ) { shutdown(); }

void Terrain3DRdOceanRenderer::setError( const String &rError ) {
	m_lastError = rError;
	if ( !m_context.hasFailure() ) m_context.recordFailure( Terrain3DRdContext::FAILURE_RENDER_PASS, rError );
}

bool Terrain3DRdOceanRenderer::ensureShaders( const Ref<RDShaderSPIRV> &rColor, const Ref<RDShaderSPIRV> &rShadow ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || rColor.is_null() || rShadow.is_null() ) return false;
	if ( !m_hColorShader.is_valid() ) {
		m_hColorShader = rd->shader_create_from_spirv( rColor );
		if ( !m_hColorShader.is_valid() || !t3dRdOceanAdopt( m_context, rd, m_hColorShader, "T3D.Ocean.Shader.Color", Terrain3DRdContext::RESOURCE_SHADER ) ) return false;
	}
	if ( !m_hShadowShader.is_valid() ) {
		m_hShadowShader = rd->shader_create_from_spirv( rShadow );
		if ( !m_hShadowShader.is_valid() || !t3dRdOceanAdopt( m_context, rd, m_hShadowShader, "T3D.Ocean.Shader.Shadow", Terrain3DRdContext::RESOURCE_SHADER ) ) return false;
	}
	return true;
}

bool Terrain3DRdOceanRenderer::ensureBuffers( void ) {
	RenderingDevice *const rd = m_context.getDevice(); if ( !rd ) return false;
	const uint32_t frame_bytes = sizeof( t3dRdOceanFrame_t );
	const uint32_t patch_bytes = sizeof( t3dRdPatch_t ) * s_nMaxClipmapLevels;
	if ( !m_hFrameBuffer.is_valid() ) {
		m_hFrameBuffer = rd->uniform_buffer_create( frame_bytes );
		if ( !m_hFrameBuffer.is_valid() || !t3dRdOceanAdopt( m_context, rd, m_hFrameBuffer, "T3D.Ocean.Frame", Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER, Terrain3DRdContext::MEMORY_UNIFORM, frame_bytes ) ) return false;
	}
	if ( !m_hPatchBuffer.is_valid() ) {
		m_hPatchBuffer = rd->storage_buffer_create( patch_bytes );
		if ( !m_hPatchBuffer.is_valid() || !t3dRdOceanAdopt( m_context, rd, m_hPatchBuffer, "T3D.Ocean.Patches", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER, Terrain3DRdContext::MEMORY_PATCH, patch_bytes ) ) return false;
	}
	m_frameBytes.resize( frame_bytes ); m_patchBytes.resize( patch_bytes ); m_pushBytes.resize( 16u );
	return true;
}

bool Terrain3DRdOceanRenderer::ensureUniformSets( void ) {
	RenderingDevice *const rd = m_context.getDevice(); if ( !rd ) return false;
	if ( !m_hColorUniformSet.is_valid() ) {
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back( t3dRdOceanBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameBuffer ) );
		uniforms.push_back( t3dRdOceanBufferUniform( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hPatchBuffer ) );
		m_hColorUniformSet = rd->uniform_set_create( uniforms, m_hColorShader, 0u );
		if ( !m_hColorUniformSet.is_valid() || !t3dRdOceanAdopt( m_context, rd, m_hColorUniformSet, "T3D.Ocean.Set.Color", Terrain3DRdContext::RESOURCE_UNIFORM_SET ) ) return false;
	}
	if ( !m_hShadowUniformSet.is_valid() ) {
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back( t3dRdOceanBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameBuffer ) );
		uniforms.push_back( t3dRdOceanBufferUniform( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hPatchBuffer ) );
		m_hShadowUniformSet = rd->uniform_set_create( uniforms, m_hShadowShader, 0u );
		if ( !m_hShadowUniformSet.is_valid() || !t3dRdOceanAdopt( m_context, rd, m_hShadowUniformSet, "T3D.Ocean.Set.Shadow", Terrain3DRdContext::RESOURCE_UNIFORM_SET ) ) return false;
	}
	return true;
}

bool Terrain3DRdOceanRenderer::ensureTopology( const t3dRdOceanTopologySnapshot_t *const pTopology ) {
	if ( !pTopology ) return false;
	if ( m_nTopologyGeneration == pTopology->m_nGeneration && m_szTopology[0].isGpuReady() && m_szTopology[1].isGpuReady() ) return true;
	for ( Terrain3DRdTopology &topology : m_szTopology ) topology.releaseGpu( m_context );
	if ( !m_szTopology[LOD0_MESH].buildCpu( pTopology->m_szGeometry[LOD0_MESH], "Ocean.LOD0" ) ||
			!m_szTopology[OUTER_RING_MESH].buildCpu( pTopology->m_szGeometry[OUTER_RING_MESH], "Ocean.Outer" ) ||
			!m_szTopology[LOD0_MESH].upload( m_context, m_nVertexFormat ) ||
			!m_szTopology[OUTER_RING_MESH].upload( m_context, m_nVertexFormat ) ) {
		setError( "Failed to upload RD ocean clipmap topology" ); return false;
	}
	m_nTopologyGeneration = pTopology->m_nGeneration; ++m_nTopologyUploads; return true;
}

static void t3dRdOceanCopyProjection( const Projection &rProjection, float *const pDestination ) {
	for ( uint32_t column = 0u; column < 4u; ++column ) for ( uint32_t row = 0u; row < 4u; ++row ) pDestination[column * 4u + row] = ( float )( rProjection.columns[column][row] );
}

bool Terrain3DRdOceanRenderer::updateFrame( const t3dRdForwardPassContext_t &rContext, const t3dRdFrameSnapshot_t &rSnapshot ) {
	const t3dRdOceanState_t &rState = rSnapshot.m_ocean;
	t3dRdOceanFrame_t frame{};
	for ( uint32_t view = 0u; view < s_nRdForwardMaxViews; ++view ) t3dRdOceanCopyProjection( rContext.m_szViewProjection[MIN( view, rContext.m_nViewCount - 1u )], frame.m_mViewProjection[view] );
	t3dRdOceanCopyProjection( Projection( rContext.m_cameraTransform.affine_inverse() ), frame.m_mView );
	frame.m_szCameraTime[0] = ( float )( rContext.m_cameraTransform.origin.x ); frame.m_szCameraTime[1] = ( float )( rContext.m_cameraTransform.origin.y ); frame.m_szCameraTime[2] = ( float )( rContext.m_cameraTransform.origin.z ); frame.m_szCameraTime[3] = rState.m_fTimeSeconds;
	frame.m_szLightDirection[0] = ( float )( rSnapshot.m_lightDirection.x ); frame.m_szLightDirection[1] = ( float )( rSnapshot.m_lightDirection.y ); frame.m_szLightDirection[2] = ( float )( rSnapshot.m_lightDirection.z ); frame.m_szLightDirection[3] = rSnapshot.m_fAmbient;
	frame.m_szLightColor[0] = ( float )( rSnapshot.m_lightColor.r ); frame.m_szLightColor[1] = ( float )( rSnapshot.m_lightColor.g ); frame.m_szLightColor[2] = ( float )( rSnapshot.m_lightColor.b ); frame.m_szLightColor[3] = ( float )( rSnapshot.m_lightColor.a );
	frame.m_szMeshParams[0] = rState.m_fMeshSize; frame.m_szMeshParams[1] = rState.m_fVertexSpacing; frame.m_szMeshParams[2] = rState.m_fSubdiv; frame.m_szMeshParams[3] = rState.m_fSeaLevel;
	frame.m_szTargetTime[0] = rState.m_fTargetX; frame.m_szTargetTime[1] = rState.m_fTargetY; frame.m_szTargetTime[2] = rState.m_fTargetZ; frame.m_szTargetTime[3] = rState.m_fTimeSeconds;
	std::memcpy( frame.m_szWaterColor, rState.m_szWaterColor, sizeof( frame.m_szWaterColor ) );
	frame.m_szDepthParams[0] = rState.m_fVisibleDepth; frame.m_szDepthParams[1] = rState.m_fDepthCurve; frame.m_szDepthParams[2] = rState.m_fShoreBlend; frame.m_szDepthParams[3] = rState.m_fRefractionStrength;
	frame.m_szWaveParams[0] = rState.m_fTimeScale; frame.m_szWaveParams[1] = rState.m_fHeightScale; frame.m_szWaveParams[2] = rState.m_fOceanScale; frame.m_szWaveParams[3] = rState.m_fFresnelScale;
	frame.m_szSurfaceParams[0] = rState.m_fFresnelPower; frame.m_szSurfaceParams[1] = rState.m_fSpecular; frame.m_szSurfaceParams[2] = rState.m_fFoamHeight; frame.m_szSurfaceParams[3] = rState.m_fFoamTopIntensity;
	frame.m_szFoamParams0[0] = rState.m_fFoamDistance; frame.m_szFoamParams0[1] = rState.m_fFoamShoreIntensity; frame.m_szFoamParams0[2] = rState.m_fFoamNoiseScale; frame.m_szFoamParams0[3] = rState.m_fHeightScattering;
	frame.m_szFoamParams1[0] = rState.m_fHeightThreshold; frame.m_szFoamParams1[1] = rState.m_fHeightAngleThreshold; frame.m_szFoamParams1[2] = rState.m_fFresnelScattering; frame.m_szFoamParams1[3] = rState.m_fDirectScattering;
	frame.m_szScatterParams[0] = rState.m_fHeightScattering; frame.m_szScatterParams[1] = rState.m_fHeightAngleThreshold; frame.m_szScatterParams[2] = rState.m_fFresnelScattering; frame.m_szScatterParams[3] = rState.m_fDirectScattering;
	frame.m_szModeParams[0] = ( float )( rState.m_nVertexIterations ); frame.m_szModeParams[1] = ( float )( rState.m_nFragmentIterations ); frame.m_szModeParams[2] = ( float )( rState.m_nRefractionMode ); frame.m_szModeParams[3] = ( float )( rState.m_nFoamMode + 4u * rState.m_nScatteringMode + 16u * rState.m_nShadingMode );
	std::memcpy( m_frameBytes.ptrw(), &frame, sizeof( frame ) );
	std::memset( m_patchBytes.ptrw(), 0, m_patchBytes.size() );
	std::memcpy( m_patchBytes.ptrw(), rState.m_szPatches, sizeof( t3dRdPatch_t ) * MIN( rState.m_nPatchCount, ( uint32_t )( s_nMaxClipmapLevels ) ) );
	RenderingDevice *const rd = m_context.getDevice();
	if ( rd->buffer_update( m_hFrameBuffer, 0u, sizeof( frame ), m_frameBytes ) != OK || rd->buffer_update( m_hPatchBuffer, 0u, m_patchBytes.size(), m_patchBytes ) != OK ) { setError( "Failed to update RD ocean frame/patch buffers" ); return false; }
	m_context.recordBufferUpdate( sizeof( frame ) + m_patchBytes.size() ); m_nStateGeneration = rState.m_nGeneration; return true;
}

const Terrain3DRdOceanRenderer::t3dRdOceanPipelineEntry_t *Terrain3DRdOceanRenderer::findPipeline( const t3dRdOceanPipelineKey_t &rKey ) const { for ( const t3dRdOceanPipelineEntry_t &entry : m_pipelines ) if ( entry.m_key == rKey ) return &entry; return nullptr; }
void Terrain3DRdOceanRenderer::freePipelines( void ) { for ( t3dRdOceanPipelineEntry_t &entry : m_pipelines ) m_context.freeResource( entry.m_hPipeline ); m_pipelines.clear(); }

bool Terrain3DRdOceanRenderer::ensurePipeline( const t3dRdForwardPassContext_t &rContext ) {
	RenderingDevice *const rd = m_context.getDevice(); if ( !rd || rContext.m_nFramebufferFormat < 0 ) return false;
	t3dRdOceanPipelineKey_t key; key.m_nFramebufferFormat = rContext.m_nFramebufferFormat; key.m_passKind = rContext.m_passKind; key.m_samples = rd->framebuffer_format_get_texture_samples( rContext.m_nFramebufferFormat, 0u ); key.m_nViewCount = rContext.m_nViewCount; key.m_bReverseCull = rContext.m_bReverseCull;
	if ( findPipeline( key ) ) return true;
	if ( m_pipelines.size() >= 32u ) freePipelines();
	Ref<RDPipelineRasterizationState> raster; raster.instantiate(); raster->set_front_face( RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE ); raster->set_cull_mode( rContext.m_bReverseCull ? RenderingDevice::POLYGON_CULL_FRONT : RenderingDevice::POLYGON_CULL_BACK );
	Ref<RDPipelineMultisampleState> multisample; multisample.instantiate(); multisample->set_sample_count( key.m_samples );
	Ref<RDPipelineDepthStencilState> depth; depth.instantiate(); depth->set_enable_depth_test( true ); depth->set_enable_depth_write( true ); depth->set_depth_compare_operator( RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL );
	Ref<RDPipelineColorBlendState> blend; blend.instantiate();
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT ) {
		TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments; Ref<RDPipelineColorBlendStateAttachment> attachment; attachment.instantiate(); attachment->set_enable_blend( true ); attachment->set_src_color_blend_factor( RenderingDevice::BLEND_FACTOR_SRC_ALPHA ); attachment->set_dst_color_blend_factor( RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ); attachment->set_src_alpha_blend_factor( RenderingDevice::BLEND_FACTOR_ONE ); attachment->set_dst_alpha_blend_factor( RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ); attachments.push_back( attachment ); blend->set_attachments( attachments );
	}
	t3dRdOceanPipelineEntry_t entry; entry.m_key = key;
	const RID shader = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ? m_hShadowShader : m_hColorShader;
	entry.m_hPipeline = rd->render_pipeline_create( shader, rContext.m_nFramebufferFormat, m_nVertexFormat, RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, raster, multisample, depth, blend );
	if ( !entry.m_hPipeline.is_valid() || !t3dRdOceanAdopt( m_context, rd, entry.m_hPipeline, "T3D.Ocean.ForwardPipeline", Terrain3DRdContext::RESOURCE_RASTER_PIPELINE ) ) { setError( "Failed to create native Forward+ ocean pipeline" ); return false; }
	m_pipelines.push_back( entry ); ++m_nPipelineBuilds; return true;
}

bool Terrain3DRdOceanRenderer::initialize( const Ref<RDShaderSPIRV> &rColor, const Ref<RDShaderSPIRV> &rShadow ) {
	shutdown(); if ( !m_context.isReady() && !m_context.initialize() ) { setError( "RenderingDevice unavailable for ocean renderer" ); return false; }
	RenderingDevice *const rd = m_context.getDevice();
	TypedArray<Ref<RDVertexAttribute>> attributes;
	Ref<RDVertexAttribute> grid; grid.instantiate(); grid->set_location( 0u ); grid->set_format( RenderingDevice::DATA_FORMAT_R16G16_SINT ); grid->set_offset( 0u ); grid->set_stride( sizeof( t3dRdClipmapVertex_t ) ); attributes.push_back( grid );
	Ref<RDVertexAttribute> meta; meta.instantiate(); meta->set_location( 1u ); meta->set_format( RenderingDevice::DATA_FORMAT_R16G16_UINT ); meta->set_offset( 4u ); meta->set_stride( sizeof( t3dRdClipmapVertex_t ) ); attributes.push_back( meta );
	m_nVertexFormat = rd->vertex_format_create( attributes );
	if ( m_nVertexFormat == RenderingDevice::INVALID_FORMAT_ID ) {
		m_context.recordResourceCreationFailure( "T3D.VertexFormat.OceanPacked8", Terrain3DRdContext::RESOURCE_VERTEX_ARRAY );
		setError( "Failed to create packed RD ocean vertex format" );
		shutdown();
		return false;
	}
	if ( !m_context.registerVertexFormat( m_nVertexFormat, "T3D.VertexFormat.OceanPacked8" ) || !ensureShaders( rColor, rShadow ) || !ensureBuffers() || !ensureUniformSets() ) { setError( "Failed to initialize RD ocean renderer" ); shutdown(); return false; }
	m_lastError = String(); return true;
}

void Terrain3DRdOceanRenderer::shutdown( void ) {
	freePipelines();
	for ( Terrain3DRdTopology &topology : m_szTopology ) { topology.releaseGpu( m_context ); topology.clearCpu(); }
	m_context.freeResource( m_hColorUniformSet ); m_context.freeResource( m_hShadowUniformSet ); m_context.freeResource( m_hFrameBuffer ); m_context.freeResource( m_hPatchBuffer ); m_context.freeResource( m_hColorShader ); m_context.freeResource( m_hShadowShader );
	m_context.releaseVertexFormat( m_nVertexFormat );
	m_hColorUniformSet = m_hShadowUniformSet = m_hFrameBuffer = m_hPatchBuffer = m_hColorShader = m_hShadowShader = RID(); m_nTopologyGeneration = m_nStateGeneration = 0u; m_nPreparedScenePass = m_nPreparedViewportKey = 0u; m_preparedPassKind = T3D_RD_FORWARD_PASS_INVALID; m_nPreparedPatchCount = 0u; m_bPreparedHasDraws = false;
}

bool Terrain3DRdOceanRenderer::prepareForwardPass( const t3dRdForwardPassContext_t &rContext, const t3dRdFrameSnapshot_t &rSnapshot,
		const t3dRdOceanTopologySnapshot_t *const pTopology, const uint32_t nVisibilityMask ) {
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE ) return false;
	const t3dRdOceanState_t &rState = rSnapshot.m_ocean;
	m_bPreparedHasDraws = false; m_nPreparedPatchCount = 0u; m_nPreparedScenePass = rContext.m_nScenePass; m_nPreparedViewportKey = rContext.m_nViewportKey; m_preparedPassKind = rContext.m_passKind;
	if ( rState.m_nEnabled == 0u || rState.m_nPatchCount == 0u || ( rState.m_nRenderLayers & nVisibilityMask ) == 0u ) return true;
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW && rState.m_nCastShadows == 0u ) return true;
	if ( rContext.m_passKind != T3D_RD_FORWARD_PASS_TRANSPARENT && rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW ) return true;
	if ( !m_context.validateDevice() || !ensureTopology( pTopology ) || !updateFrame( rContext, rSnapshot ) || !ensurePipeline( rContext ) ) return false;
	m_nPreparedPatchCount = MIN( rState.m_nPatchCount, ( uint32_t )( s_nMaxClipmapLevels ) ); m_bPreparedHasDraws = m_nPreparedPatchCount > 0u; ++m_nFramesPrepared; m_lastError = String(); return true;
}

bool Terrain3DRdOceanRenderer::renderForwardPass( const t3dRdForwardPassContext_t &rContext ) {
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT || rContext.m_nDrawList < 0 || m_nPreparedScenePass != rContext.m_nScenePass || m_nPreparedViewportKey != rContext.m_nViewportKey || m_preparedPassKind != rContext.m_passKind ) return false;
	if ( !m_bPreparedHasDraws ) return true;
	t3dRdOceanPipelineKey_t key; RenderingDevice *const rd = m_context.getDevice(); key.m_nFramebufferFormat = rContext.m_nFramebufferFormat; key.m_passKind = rContext.m_passKind; key.m_samples = rd->framebuffer_format_get_texture_samples( rContext.m_nFramebufferFormat, 0u ); key.m_nViewCount = rContext.m_nViewCount; key.m_bReverseCull = rContext.m_bReverseCull;
	const t3dRdOceanPipelineEntry_t *const entry = findPipeline( key ); if ( !entry ) return false;
	const RID set = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ? m_hShadowUniformSet : m_hColorUniformSet;
	rd->draw_list_bind_render_pipeline( rContext.m_nDrawList, entry->m_hPipeline ); rd->draw_list_bind_uniform_set( rContext.m_nDrawList, set, 0u );
	uint32_t push[4] = { 0u, 0u, 0u, 0u }; std::memcpy( m_pushBytes.ptrw(), push, sizeof( push ) ); rd->draw_list_set_push_constant( rContext.m_nDrawList, m_pushBytes, sizeof( push ) ); rd->draw_list_bind_vertex_array( rContext.m_nDrawList, m_szTopology[LOD0_MESH].getVertexArray() ); rd->draw_list_bind_index_array( rContext.m_nDrawList, m_szTopology[LOD0_MESH].getIndexArray() ); rd->draw_list_draw( rContext.m_nDrawList, true, 1u, 0u ); ++m_nDraws;
	if ( m_nPreparedPatchCount > 1u ) { push[0] = 1u; std::memcpy( m_pushBytes.ptrw(), push, sizeof( push ) ); rd->draw_list_set_push_constant( rContext.m_nDrawList, m_pushBytes, sizeof( push ) ); rd->draw_list_bind_vertex_array( rContext.m_nDrawList, m_szTopology[OUTER_RING_MESH].getVertexArray() ); rd->draw_list_bind_index_array( rContext.m_nDrawList, m_szTopology[OUTER_RING_MESH].getIndexArray() ); rd->draw_list_draw( rContext.m_nDrawList, true, m_nPreparedPatchCount - 1u, 0u ); ++m_nDraws; }
	return true;
}

Dictionary Terrain3DRdOceanRenderer::getDiagnostics( void ) const { Dictionary result; result["ocean_rd_ready"] = m_hColorShader.is_valid() && m_hShadowShader.is_valid(); result["ocean_topology_generation"] = ( int64_t )( m_nTopologyGeneration ); result["ocean_topology_uploads"] = ( int64_t )( m_nTopologyUploads ); result["ocean_pipeline_builds"] = ( int64_t )( m_nPipelineBuilds ); result["ocean_frames_prepared"] = ( int64_t )( m_nFramesPrepared ); result["ocean_draws"] = ( int64_t )( m_nDraws ); result["ocean_last_error"] = m_lastError; return result; }
