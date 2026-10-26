// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/infrastructure/terrain_3d_rd_binding_builder.h"
#include "rendering/rd/terrain_3d_rd_ocean_renderer.h"

#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <string.h>
#include "rendering/rd/infrastructure/terrain_3d_rd_gpu_buffer_batch.h"

Terrain3DRdOceanRenderer::Terrain3DRdOceanRenderer( void ) :
        m_context( Terrain3DRdContext::getShared() ),
        m_pipelines( m_context, Terrain3DRdContext::OWNER_OCEAN ),
        m_descriptors( m_context, Terrain3DRdContext::OWNER_OCEAN ) {
}
Terrain3DRdOceanRenderer::~Terrain3DRdOceanRenderer( void ) {
	shutdown();
}

void Terrain3DRdOceanRenderer::setError( const String &rError ) {
	// Ocean is a companion lane. A pass-local failure must not transition the
	// shared RenderingDevice context into STATE_FAILED.
	m_lastError = rError;
}

bool Terrain3DRdOceanRenderer::ensureShaders( const Ref<RDShaderSPIRV> &rColor, const Ref<RDShaderSPIRV> &rShadow ) {
	if ( !m_context.getDevice() || rColor.is_null() || rShadow.is_null() ) {
		return false;
	}
	if ( !m_hColorShader.isValid()
	        && !m_hColorShader.createTracked( m_context, Terrain3DRdContext::OWNER_OCEAN, "T3D.Ocean.Shader.Color", rColor ) ) {
		return false;
	}
	if ( !m_hShadowShader.isValid()
	        && !m_hShadowShader.createTracked( m_context, Terrain3DRdContext::OWNER_OCEAN, "T3D.Ocean.Shader.Shadow", rShadow ) ) {
		return false;
	}
	return true;
}

bool Terrain3DRdOceanRenderer::ensureBuffers( void ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	const uint32_t frame_bytes = sizeof( t3dRdOceanFrame_t );
	const uint32_t patch_bytes = sizeof( t3dRdPatch_t ) * s_nMaxClipmapLevels;
	const uint32_t strip_part_bytes = sizeof( t3dClipmapStripPart_t ) * MESH_TYPE_COUNT * T3D_CLIPMAP_STRIP_PART_CAPACITY;
	const uint32_t strip_indirect_bytes = sizeof( t3dRdDrawIndirectCommand_t ) * MESH_TYPE_COUNT * T3D_CLIPMAP_STRIP_PART_CAPACITY;
	if ( strip_indirect_bytes > Terrain3DRdContext::fixedIndirectCapacity( Terrain3DRdContext::FIXED_INDIRECT_OCEAN ) ) {
		return false;
	}
	if ( !m_hFrameBuffer.isValid() ) {
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.Ocean.Frame";
		spec.m_owner = Terrain3DRdContext::OWNER_OCEAN;
		spec.m_type = Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER;
		spec.m_memory = Terrain3DRdContext::MEMORY_UNIFORM;
		if ( !m_hFrameBuffer.configure( spec ) || !m_hFrameBuffer.createTracked( m_context, frame_bytes ) ) {
			return false;
		}
	}
	if ( !m_hPatchBuffer.isValid() ) {
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.Ocean.Patches";
		spec.m_owner = Terrain3DRdContext::OWNER_OCEAN;
		spec.m_type = Terrain3DRdContext::RESOURCE_STORAGE_BUFFER;
		spec.m_memory = Terrain3DRdContext::MEMORY_PATCH;
		if ( !m_hPatchBuffer.configure( spec ) || !m_hPatchBuffer.createTracked( m_context, patch_bytes ) ) {
			return false;
		}
	}
	if ( !m_hStripPartBuffer.isValid() ) {
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.Ocean.StripParts";
		spec.m_owner = Terrain3DRdContext::OWNER_OCEAN;
		spec.m_type = Terrain3DRdContext::RESOURCE_STORAGE_BUFFER;
		spec.m_memory = Terrain3DRdContext::MEMORY_STORAGE;
		if ( !m_hStripPartBuffer.configure( spec ) || !m_hStripPartBuffer.createTracked( m_context, strip_part_bytes ) ) {
			return false;
		}
	}
	if ( !m_pStripIndirectBuffer || !m_pStripIndirectBuffer->ownsSlice( m_stripIndirectSlice ) ) {
		if ( m_pStripIndirectBuffer ) {
			m_context.releaseFixedIndirect( Terrain3DRdContext::FIXED_INDIRECT_OCEAN, m_pStripIndirectBuffer );
		}
		m_stripIndirectSlice = t3dRdGpuBufferSlice_t{};
		uint32_t offset = 0u;
		const uint32_t capacity = Terrain3DRdContext::fixedIndirectCapacity( Terrain3DRdContext::FIXED_INDIRECT_OCEAN );
		if ( !m_context.acquireFixedIndirect( Terrain3DRdContext::FIXED_INDIRECT_OCEAN,
	             strip_indirect_bytes, m_pStripIndirectBuffer, offset )
		        || offset != Terrain3DRdContext::fixedIndirectOffset( Terrain3DRdContext::FIXED_INDIRECT_OCEAN )
		        || !m_pStripIndirectBuffer->slice( offset, capacity, 4u, m_stripIndirectSlice ) ) {
			m_context.releaseFixedIndirect( Terrain3DRdContext::FIXED_INDIRECT_OCEAN, m_pStripIndirectBuffer );
			m_stripIndirectSlice = t3dRdGpuBufferSlice_t{};
			return false;
		}
	}
	m_frameBytes.resize( frame_bytes );
	m_patchBytes.resize( patch_bytes );
	m_pushBytes.resize( 16u );
	m_stripPartBytes.resize( strip_part_bytes );
	m_stripIndirectBytes.resize( strip_indirect_bytes );
	return true;
}

bool Terrain3DRdOceanRenderer::ensureUniformSets( void ) {
	if ( !m_context.getDevice() ) {
		return false;
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hPatchBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hStripPartBuffer ) );
	const t3dRdOceanSetKey_t color { m_hColorShader.rid(), m_hFrameBuffer, m_hPatchBuffer, m_hStripPartBuffer };
	const t3dRdOceanSetKey_t shadow { m_hShadowShader.rid(), m_hFrameBuffer, m_hPatchBuffer, m_hStripPartBuffer };
	return m_descriptors.getOrCreate( color, "T3D.Ocean.Set.Color", uniforms, m_hColorShader.rid(), 0u ).is_valid()
	        && m_descriptors.getOrCreate( shadow, "T3D.Ocean.Set.Shadow", uniforms, m_hShadowShader.rid(), 0u ).is_valid();
}

bool Terrain3DRdOceanRenderer::ensureStripParts( const t3dRdOceanStripSnapshot_t *const pStrip ) {
	if ( !pStrip || !m_hStripPartBuffer.is_valid() ) {
		return false;
	}
	if ( m_nStripGeneration == pStrip->m_nGeneration ) {
		return true;
	}
	for ( uint32_t type = 0u; type < MESH_TYPE_COUNT; ++type ) {
		const uint32_t count = pStrip->m_szStripPartCounts[type];
		if ( count == 0u || count > T3D_CLIPMAP_STRIP_PART_CAPACITY ) {
			setError( "Invalid RD ocean procedural strip descriptor count" );
			return false;
		}
		for ( uint32_t part = 0u; part < count; ++part ) {
			const t3dClipmapStripPart_t &descriptor = pStrip->m_szStripParts[type][part];
			if ( descriptor.m_nPartCode >= T3D_CLIPMAP_PART_CODE_COUNT
			        || ( descriptor.m_nFlags & ~T3D_CLIPMAP_STRIP_KNOWN_FLAG_MASK ) != 0u
			        || !t3dClipmapStripPartFitsVirtualVertexRange( descriptor ) ) {
				setError( "Invalid RD ocean procedural strip descriptor" );
				return false;
			}
		}
	}
	memcpy( m_szStripPartCounts, pStrip->m_szStripPartCounts, sizeof( m_szStripPartCounts ) );
	memcpy( m_szStripParts, pStrip->m_szStripParts, sizeof( m_szStripParts ) );
	const uint32_t bytes = sizeof( m_szStripParts );
	if ( m_stripPartBytes.size() != bytes ) {
		return false;
	}
	memcpy( m_stripPartBytes.ptrw(), m_szStripParts, bytes );
	if ( !m_hStripPartBuffer.writeTracked( m_context, 0u, m_stripPartBytes.ptr(), bytes,
	        m_stripPartBytes, T3D_GPU_PUBLICATION ) ) {
		setError( "Failed to upload RD ocean procedural strip descriptors" );
		return false;
	}
	m_nStripGeneration = pStrip->m_nGeneration;
	++m_nStripDescriptorUploads;
	return true;
}

bool Terrain3DRdOceanRenderer::updateStripIndirect( const uint32_t nPatchCount ) {
	if ( !m_pStripIndirectBuffer || !m_pStripIndirectBuffer->ownsSlice( m_stripIndirectSlice ) || m_stripIndirectBytes.is_empty() ) {
		return false;
	}
	t3dRdDrawIndirectCommand_t *const commands = ( t3dRdDrawIndirectCommand_t * )( m_stripIndirectBytes.ptrw() );
	memset( commands, 0, ( size_t )( m_stripIndirectBytes.size() ) );
	for ( uint32_t type = 0u; type < MESH_TYPE_COUNT; ++type ) {
		const uint32_t instance_count = type == LOD0_MESH ? ( nPatchCount > 0u ? 1u : 0u ) : ( nPatchCount > 1u ? nPatchCount - 1u : 0u );
		const uint32_t part_count = m_szStripPartCounts[type];
		for ( uint32_t part = 0u; part < part_count; ++part ) {
			t3dRdDrawIndirectCommand_t &command = commands[type * T3D_CLIPMAP_STRIP_PART_CAPACITY + part];
			command.m_nVertexCount = t3dClipmapStripActualVertexCount( m_szStripParts[type][part] );
			command.m_nInstanceCount = instance_count;
			command.m_nFirstVertex = t3dClipmapStripVirtualFirstVertex( part );
			command.m_nFirstInstance = 0u;
		}
	}
	const uint32_t indirect_bytes = ( uint32_t )( m_stripIndirectBytes.size() );
	if ( !m_pStripIndirectBuffer->writePackedSliceTracked( m_context, m_stripIndirectSlice, 0u, m_stripIndirectBytes, indirect_bytes,
	        T3D_GPU_INDIRECT, Terrain3DRdContext::OWNER_OCEAN )
	        || !m_pStripIndirectBuffer->setSliceUsedBytes( m_stripIndirectSlice, indirect_bytes ) ) {
		setError( "Failed to update RD ocean procedural strip indirect commands" );
		return false;
	}
	return true;
}

static void t3dRdOceanCopyProjection( const Projection &rProjection, float *const pDestination ) {
	for ( uint32_t column = 0u; column < 4u; ++column ) {
		for ( uint32_t row = 0u; row < 4u; ++row ) {
			pDestination[column * 4u + row] = ( float )( rProjection.columns[column][row] );
		}
	}
}

bool Terrain3DRdOceanRenderer::updateFrame( const t3dRdForwardPassContext_t &rContext, const t3dRdFrameSnapshot_t &rSnapshot ) {
	const t3dRdOceanState_t &rState = rSnapshot.m_ocean;
	t3dRdOceanFrame_t frame;
	memset( &frame, 0, sizeof( frame ) );
	for ( uint32_t view = 0u; view < s_nRdForwardMaxViews; ++view ) {
		t3dRdOceanCopyProjection( rContext.m_szViewProjection[MIN( view, rContext.m_nViewCount - 1u )], frame.m_mViewProjection[view] );
	}
	t3dRdOceanCopyProjection( Projection( rContext.m_cameraTransform.affine_inverse() ), frame.m_mView );
	frame.m_szCameraTime[0] = ( float )( rContext.m_cameraTransform.origin.x );
	frame.m_szCameraTime[1] = ( float )( rContext.m_cameraTransform.origin.y );
	frame.m_szCameraTime[2] = ( float )( rContext.m_cameraTransform.origin.z );
	frame.m_szCameraTime[3] = rState.m_fTimeSeconds;
	frame.m_szLightDirection[0] = ( float )( rSnapshot.m_lightDirection.x );
	frame.m_szLightDirection[1] = ( float )( rSnapshot.m_lightDirection.y );
	frame.m_szLightDirection[2] = ( float )( rSnapshot.m_lightDirection.z );
	frame.m_szLightDirection[3] = rSnapshot.m_fAmbient;
	frame.m_szLightColor[0] = ( float )( rSnapshot.m_lightColor.r );
	frame.m_szLightColor[1] = ( float )( rSnapshot.m_lightColor.g );
	frame.m_szLightColor[2] = ( float )( rSnapshot.m_lightColor.b );
	frame.m_szLightColor[3] = ( float )( rSnapshot.m_lightColor.a );
	frame.m_szMeshParams[0] = rState.m_fMeshSize;
	frame.m_szMeshParams[1] = rState.m_fVertexSpacing;
	frame.m_szMeshParams[2] = rState.m_fSubdiv;
	frame.m_szMeshParams[3] = rState.m_fSeaLevel;
	frame.m_szTargetTime[0] = rState.m_fTargetX;
	frame.m_szTargetTime[1] = rState.m_fTargetY;
	frame.m_szTargetTime[2] = rState.m_fTargetZ;
	frame.m_szTargetTime[3] = rState.m_fTimeSeconds;
	memcpy( frame.m_szWaterColor, rState.m_szWaterColor, sizeof( frame.m_szWaterColor ) );
	frame.m_szDepthParams[0] = rState.m_fVisibleDepth;
	frame.m_szDepthParams[1] = rState.m_fDepthCurve;
	frame.m_szDepthParams[2] = rState.m_fShoreBlend;
	frame.m_szDepthParams[3] = rState.m_fRefractionStrength;
	frame.m_szWaveParams[0] = rState.m_fTimeScale;
	frame.m_szWaveParams[1] = rState.m_fHeightScale;
	frame.m_szWaveParams[2] = rState.m_fOceanScale;
	frame.m_szWaveParams[3] = rState.m_fFresnelScale;
	frame.m_szSurfaceParams[0] = rState.m_fFresnelPower;
	frame.m_szSurfaceParams[1] = rState.m_fSpecular;
	frame.m_szSurfaceParams[2] = rState.m_fFoamHeight;
	frame.m_szSurfaceParams[3] = rState.m_fFoamTopIntensity;
	frame.m_szFoamParams0[0] = rState.m_fFoamDistance;
	frame.m_szFoamParams0[1] = rState.m_fFoamShoreIntensity;
	frame.m_szFoamParams0[2] = rState.m_fFoamNoiseScale;
	frame.m_szFoamParams0[3] = rState.m_fHeightScattering;
	frame.m_szFoamParams1[0] = rState.m_fHeightThreshold;
	frame.m_szFoamParams1[1] = rState.m_fHeightAngleThreshold;
	frame.m_szFoamParams1[2] = rState.m_fFresnelScattering;
	frame.m_szFoamParams1[3] = rState.m_fDirectScattering;
	frame.m_szScatterParams[0] = rState.m_fHeightScattering;
	frame.m_szScatterParams[1] = rState.m_fHeightAngleThreshold;
	frame.m_szScatterParams[2] = rState.m_fFresnelScattering;
	frame.m_szScatterParams[3] = rState.m_fDirectScattering;
	frame.m_szModeParams[0] = ( float )( rState.m_nVertexIterations );
	frame.m_szModeParams[1] = ( float )( rState.m_nFragmentIterations );
	frame.m_szModeParams[2] = ( float )( rState.m_nRefractionMode );
	frame.m_szModeParams[3] = ( float )( rState.m_nFoamMode + 4u * rState.m_nScatteringMode + 16u * rState.m_nShadingMode );
	memcpy( m_frameBytes.ptrw(), &frame, sizeof( frame ) );
	memset( m_patchBytes.ptrw(), 0, m_patchBytes.size() );
	memcpy( m_patchBytes.ptrw(),
	        rState.m_szPatches,
	        sizeof( t3dRdPatch_t ) * MIN( rState.m_nPatchCount, ( uint32_t )( s_nMaxClipmapLevels ) ) );
	Terrain3DRdGpuBufferBatch uploads( m_context );
	if ( !uploads.queueFullWrite( m_hFrameBuffer, m_frameBytes.ptr(), sizeof( frame ), m_frameBytes,
	                    T3D_GPU_HOT_FRAME )
	        || !uploads.queueFullWrite( m_hPatchBuffer,
	                m_patchBytes.ptr(),
	                ( uint32_t )( m_patchBytes.size() ),
	                m_patchBytes, T3D_GPU_DIRTY_UPLOAD )
	        || !uploads.flush() ) {
		setError( "Failed to update RD ocean frame/patch buffers" );
		return false;
	}
	m_nStateGeneration = rState.m_nGeneration;
	return true;
}

const Terrain3DRdOceanRenderer::t3dRdOceanPipelineEntry_t *Terrain3DRdOceanRenderer::findPipeline(
        const t3dRdOceanPipelineKey_t &rKey ) const {
	return m_pipelines.find( rKey );
}
void Terrain3DRdOceanRenderer::freePipelines( void ) {
	m_pipelines.clear();
}

bool Terrain3DRdOceanRenderer::ensurePipeline( const t3dRdForwardPassContext_t &rContext ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || rContext.m_nFramebufferFormat < 0 ) {
		return false;
	}
	t3dRdOceanPipelineKey_t key;
	const t3dRdForwardShaderVariant_t variant =
	        rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ? T3D_RD_FORWARD_SHADER_DEPTH : T3D_RD_FORWARD_SHADER_COLOR;
	if ( !t3dRdCompileForwardPipelineBaseKey( rd, rContext, variant, false, key, false ) ) {
		return false;
	}
	if ( findPipeline( key ) ) {
		return true;
	}
	t3dRdForwardPipelineStateOptions_t state_options;
	state_options.m_frontFace = RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
	state_options.m_cullMode = key.m_bReverseCull ? RenderingDevice::POLYGON_CULL_FRONT : RenderingDevice::POLYGON_CULL_BACK;
	state_options.m_nBlendAttachmentCount = rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT ? 3u : 0u;
	state_options.m_bBlendFirstAttachment = rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
	t3dRdForwardPipelineStates_t pipeline_states;
	if ( !t3dRdBuildForwardPipelineStates( key, state_options, pipeline_states ) ) {
		setError( "Failed to allocate native Forward+ ocean pipeline state" );
		return false;
	}
	Ref<RDPipelineRasterizationState> raster = pipeline_states.m_raster;
	Ref<RDPipelineMultisampleState> multisample = pipeline_states.m_multisample;
	Ref<RDPipelineDepthStencilState> depth = pipeline_states.m_depth;
	Ref<RDPipelineColorBlendState> blend = pipeline_states.m_blend;
	const RID shader = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ? m_hShadowShader.rid() : m_hColorShader.rid();
	const RID pipeline = m_pipelines.getOrCreateRaster( key,
	        "T3D.Ocean.ForwardPipeline",
	        shader,
	        rContext.m_nFramebufferFormat,
	        static_cast<int64_t>( RenderingDevice::INVALID_FORMAT_ID ),
	        RenderingDevice::RENDER_PRIMITIVE_TRIANGLE_STRIPS,
	        raster,
	        multisample,
	        depth,
	        blend );
	if ( !pipeline.is_valid() ) {
		setError( "Failed to create native Forward+ ocean pipeline" );
		return false;
	}
	return true;
}

bool Terrain3DRdOceanRenderer::initialize( const Ref<RDShaderSPIRV> &rColor, const Ref<RDShaderSPIRV> &rShadow ) {
	shutdown();
	if ( !m_context.isReady() && !m_context.initialize() ) {
		setError( "RenderingDevice unavailable for ocean renderer" );
		return false;
	}
	if ( !ensureShaders( rColor, rShadow ) || !ensureBuffers() || !ensureUniformSets() ) {
		setError( "Failed to initialize RD ocean renderer" );
		shutdown();
		return false;
	}
	m_lastError = String();
	return true;
}

void Terrain3DRdOceanRenderer::shutdown( void ) {
	freePipelines();
	m_descriptors.clear();
	m_hFrameBuffer.releaseTracked( m_context );
	m_hPatchBuffer.releaseTracked( m_context );
	m_hStripPartBuffer.releaseTracked( m_context );
	m_context.releaseFixedIndirect( Terrain3DRdContext::FIXED_INDIRECT_OCEAN, m_pStripIndirectBuffer );
	m_stripIndirectSlice = t3dRdGpuBufferSlice_t{};
	m_hColorShader.releaseTracked( m_context );
	m_hShadowShader.releaseTracked( m_context );
	m_nStripGeneration = m_nStateGeneration = 0u;
	m_nPreparedScenePass = m_nPreparedViewportKey = 0u;
	m_preparedPassKind = T3D_RD_FORWARD_PASS_INVALID;
	m_nPreparedPatchCount = 0u;
	m_bPreparedHasDraws = false;
	memset( m_szStripPartCounts, 0, sizeof( m_szStripPartCounts ) );
	memset( m_szStripParts, 0, sizeof( m_szStripParts ) );
	m_stripPartBytes.clear();
	m_stripIndirectBytes.clear();
}

bool Terrain3DRdOceanRenderer::prepareForwardPass( const t3dRdForwardPassContext_t &rContext,
        const t3dRdFrameSnapshot_t &rSnapshot,
        const t3dRdOceanStripSnapshot_t *const pStrip,
        const uint32_t nVisibilityMask ) {
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE ) {
		return false;
	}
	const t3dRdOceanState_t &rState = rSnapshot.m_ocean;
	m_bPreparedHasDraws = false;
	m_nPreparedPatchCount = 0u;
	m_nPreparedScenePass = rContext.m_nScenePass;
	m_nPreparedViewportKey = rContext.m_nViewportKey;
	m_preparedPassKind = rContext.m_passKind;
	if ( rState.m_nEnabled == 0u || rState.m_nPatchCount == 0u || ( rState.m_nRenderLayers & nVisibilityMask ) == 0u ) {
		return true;
	}
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW && rState.m_nCastShadows == 0u ) {
		return true;
	}
	if ( rContext.m_passKind != T3D_RD_FORWARD_PASS_TRANSPARENT && rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW ) {
		return true;
	}
	if ( !m_context.validateDevice() || !ensureStripParts( pStrip ) || !updateFrame( rContext, rSnapshot )
	        || !ensurePipeline( rContext ) ) {
		return false;
	}
	m_nPreparedPatchCount = MIN( rState.m_nPatchCount, ( uint32_t )( s_nMaxClipmapLevels ) );
	if ( !updateStripIndirect( m_nPreparedPatchCount ) ) {
		return false;
	}
	m_bPreparedHasDraws = m_nPreparedPatchCount > 0u;
	++m_nFramesPrepared;
	m_lastError = String();
	return true;
}

bool Terrain3DRdOceanRenderer::renderForwardPass( const t3dRdForwardPassContext_t &rContext ) {
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT || rContext.m_nDrawList < 0 || m_nPreparedScenePass != rContext.m_nScenePass
	        || m_nPreparedViewportKey != rContext.m_nViewportKey || m_preparedPassKind != rContext.m_passKind ) {
		return false;
	}
	if ( !m_bPreparedHasDraws ) {
		return true;
	}
	t3dRdOceanPipelineKey_t key;
	RenderingDevice *const rd = m_context.getDevice();
	const t3dRdForwardShaderVariant_t variant =
	        rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ? T3D_RD_FORWARD_SHADER_DEPTH : T3D_RD_FORWARD_SHADER_COLOR;
	if ( !t3dRdCompileForwardPipelineBaseKey( rd, rContext, variant, false, key, false ) ) {
		return false;
	}
	const t3dRdOceanPipelineEntry_t *const entry = findPipeline( key );
	if ( !entry ) {
		return false;
	}
	const RID shader = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ? m_hShadowShader.rid() : m_hColorShader.rid();
	const RID set = m_descriptors.find( { shader, m_hFrameBuffer, m_hPatchBuffer, m_hStripPartBuffer } );
	if ( !set.is_valid() ) {
		return false;
	}
	rd->draw_list_bind_render_pipeline( rContext.m_nDrawList, entry->m_hPipeline );
	rd->draw_list_bind_uniform_set( rContext.m_nDrawList, set, 0u );
	uint32_t push[4] = { 0u, 0u, 0u, 0u };
	memcpy( m_pushBytes.ptrw(), push, sizeof( push ) );
	rd->draw_list_set_push_constant( rContext.m_nDrawList, m_pushBytes, sizeof( push ) );
	rd->draw_list_draw_indirect( rContext.m_nDrawList,
	        false,
	        m_pStripIndirectBuffer->rid(),
	        m_stripIndirectSlice.m_nOffsetBytes,
	        m_szStripPartCounts[LOD0_MESH],
	        sizeof( t3dRdDrawIndirectCommand_t ) );
	m_context.recordRenderIndirectCommandsSubmitted( Terrain3DRdContext::OWNER_OCEAN, m_szStripPartCounts[LOD0_MESH] );
	++m_nDraws;
	if ( m_nPreparedPatchCount > 1u ) {
		push[0] = 1u;
		push[2] = T3D_CLIPMAP_STRIP_PART_CAPACITY;
		memcpy( m_pushBytes.ptrw(), push, sizeof( push ) );
		rd->draw_list_set_push_constant( rContext.m_nDrawList, m_pushBytes, sizeof( push ) );
		rd->draw_list_draw_indirect( rContext.m_nDrawList,
		        false,
		        m_pStripIndirectBuffer->rid(),
		        m_stripIndirectSlice.m_nOffsetBytes + T3D_CLIPMAP_STRIP_PART_CAPACITY * sizeof( t3dRdDrawIndirectCommand_t ),
		        m_szStripPartCounts[OUTER_RING_MESH],
		        sizeof( t3dRdDrawIndirectCommand_t ) );
		m_context.recordRenderIndirectCommandsSubmitted( Terrain3DRdContext::OWNER_OCEAN, m_szStripPartCounts[OUTER_RING_MESH] );
		++m_nDraws;
	}
	return true;
}

Dictionary Terrain3DRdOceanRenderer::getDiagnostics( void ) const {
	Dictionary result;
	result["ocean_rd_ready"] = m_hColorShader.isValid() && m_hShadowShader.isValid();
	result["ocean_strip_generation"] = ( int64_t )( m_nStripGeneration );
	result["ocean_strip_descriptor_uploads"] = ( int64_t )( m_nStripDescriptorUploads );
	result["ocean_strip_descriptor_bytes"] = ( int64_t )( sizeof( m_szStripParts ) );
	result["ocean_lod0_strip_parts"] = ( int64_t )( m_szStripPartCounts[LOD0_MESH] );
	result["ocean_outer_strip_parts"] = ( int64_t )( m_szStripPartCounts[OUTER_RING_MESH] );
	result["ocean_pipeline_builds"] = ( int64_t )m_context.getRenderEvent(
	        Terrain3DRdContext::OWNER_OCEAN, Terrain3DRdContext::RenderTelemetry::EVENT_PIPELINE_CREATE );
	result["ocean_frames_prepared"] = ( int64_t )( m_nFramesPrepared );
	result["ocean_draws"] = ( int64_t )( m_nDraws );
	result["ocean_indirect_offset_bytes"] = ( int64_t )m_stripIndirectSlice.m_nOffsetBytes;
	result["ocean_last_error"] = m_lastError;
	return result;
}
