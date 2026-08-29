// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_aux.h"

#include "rendering/rd/render_world/terrain_3d_render_world_rd_owner.h"

#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <string.h>

using namespace godot;

Terrain3DRenderWorldAuxRaster::Terrain3DRenderWorldAuxRaster() {
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	memset( &m_Telemetry, 0, sizeof( m_Telemetry ) );
	m_nGpuCreateCount = 0u;
	m_nGpuFreeCount = 0u;
	m_nHotCreateEpoch = 0u;
	m_nHotFreeEpoch = 0u;
	m_bInitialized = false;
	m_bHotSealed = false;
}

Terrain3DRenderWorldAuxRaster::~Terrain3DRenderWorldAuxRaster() {
}

bool Terrain3DRenderWorldAuxRaster::initialize_cold( const Terrain3DRenderWorldResourceImage *const pResources,
        const T3DRenderWorldAuxFormats &rFormats,
        const t3dRdRenderWorldShaderBundle_t &rShaders ) {
	Terrain3DRdContext *const pContext = pResources ? pResources->get_rd_context() : NULL;
	RenderingDevice *const pRenderingDevice = pContext ? pContext->getDevice() : NULL;
	if ( m_bInitialized || !pRenderingDevice || !pResources || !pResources->is_initialized() ) {
		return false;
	}
	if ( rFormats.nShadowFramebufferFormat < 0 || rFormats.nMotionFramebufferFormat < 0 || rFormats.nAuxFramebufferFormat < 0 ) {
		return false;
	}

	m_pRenderingDevice = pRenderingDevice;
	m_pResources = pResources;

	if ( !create_shaders_cold( rShaders ) || !create_pipelines_cold( rFormats ) || !create_uniform_sets_cold() ) {
		shutdown_cold();
		return false;
	}

	m_bInitialized = true;
	return true;
}

void Terrain3DRenderWorldAuxRaster::shutdown_cold() {
	m_bHotSealed = false;

	if ( m_pRenderingDevice ) {
		for ( int32_t iPass = ( int32_t )( T3D_RW_AUX_PASS_COUNT )-1; iPass >= 0; --iPass ) {
			for ( int32_t iTier = ( int32_t )( T3D_RW_TIER_COUNT )-1; iTier >= 0; --iTier ) {
				free_rid_cold( m_hUniformSet[iPass][iTier] );
			}
			for ( int32_t iTier = ( int32_t )( T3D_RW_TIER_COUNT )-1; iTier >= 0; --iTier ) {
				free_rid_cold( m_hPipeline[iPass][iTier] );
			}
			free_rid_cold( m_hShader[iPass] );
		}
	}

	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	m_bInitialized = false;
}

void Terrain3DRenderWorldAuxRaster::seal_hot_runtime() {
	if ( !m_bInitialized ) {
		return;
	}
	m_nHotCreateEpoch = m_nGpuCreateCount;
	m_nHotFreeEpoch = m_nGpuFreeCount;
	m_bHotSealed = true;
}

bool Terrain3DRenderWorldAuxRaster::get_bindings_hot( T3DRenderWorldAuxBindings &rBindings ) {
	if ( !m_bInitialized || !m_bHotSealed || !hot_epoch_unchanged() ) {
		return false;
	}

	for ( uint32_t iPass = 0u; iPass < T3D_RW_AUX_PASS_COUNT; ++iPass ) {
		for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
			rBindings.hPipeline[iPass][iTier] = m_hPipeline[iPass][iTier];
			rBindings.hUniformSet[iPass][iTier] = m_hUniformSet[iPass][iTier];
		}
	}
	rBindings.nUniformSetIndex = 0u;
	return true;
}

bool Terrain3DRenderWorldAuxRaster::record_shadow_visibility_hot( const uint32_t nShadowViewCount, const uint32_t nVisibleClusterCount ) {
	if ( !m_bInitialized || !m_bHotSealed || !hot_epoch_unchanged() ) {
		return false;
	}
	m_Telemetry.nLastShadowViewCount = nShadowViewCount;
	m_Telemetry.nShadowVisibleClusters += nVisibleClusterCount;
	return true;
}

bool Terrain3DRenderWorldAuxRaster::record_pass_draws_hot( const T3DRenderWorldAuxPass pass, const uint32_t nDrawCount ) {
	if ( !m_bInitialized || !m_bHotSealed || pass >= T3D_RW_AUX_PASS_COUNT || !hot_epoch_unchanged() ) {
		++m_Telemetry.nSubmissionFailureCount;
		return false;
	}

	switch ( pass ) {
		case T3D_RW_AUX_PASS_SHADOW:
			m_Telemetry.nShadowDrawCount += nDrawCount;
			break;
		case T3D_RW_AUX_PASS_MOTION:
			m_Telemetry.nMotionDrawCount += nDrawCount;
			break;
		default:
			m_Telemetry.nAuxDrawCount += nDrawCount;
			break;
	}
	return true;
}

bool Terrain3DRenderWorldAuxRaster::is_initialized() const {
	return m_bInitialized;
}

const T3DRenderWorldAuxTelemetry &Terrain3DRenderWorldAuxRaster::get_telemetry() const {
	return m_Telemetry;
}

bool Terrain3DRenderWorldAuxRaster::create_shaders_cold( const t3dRdRenderWorldShaderBundle_t &rShaders ) {
	static_assert( ( int )( T3D_RW_AUX_PASS_COUNT ) == ( int )( T3D_RD_RW_AUX_COUNT ), "RenderWorld aux shader bundle mismatch" );
	Terrain3DRdContext *const pContext = m_pResources->get_rd_context();
	for ( uint32_t iPass = 0u; iPass < T3D_RW_AUX_PASS_COUNT; ++iPass ) {
		const Ref<RDShaderSPIRV> &spirv = rShaders.m_szAux[iPass];
		if ( spirv.is_null() ) {
			return false;
		}
		m_hShader[iPass] = m_pRenderingDevice->shader_create_from_spirv( spirv );
		if ( !m_hShader[iPass].is_valid() ) {
			return false;
		}
		if ( !t3dRenderWorldRegisterRid( pContext,
		             m_hShader[iPass],
		             String( "RenderWorld aux shader " ) + String::num_uint64( iPass ),
		             Terrain3DRdContext::RESOURCE_SHADER,
		             Terrain3DRdContext::OWNER_RENDER_WORLD_AUX_RASTER ) ) {
			t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hShader[iPass] );
			return false;
		}
		++m_nGpuCreateCount;
	}
	return true;
}

bool Terrain3DRenderWorldAuxRaster::create_pipelines_cold( const T3DRenderWorldAuxFormats &rFormats ) {
	const int64_t nFormats[T3D_RW_AUX_PASS_COUNT] = {
		rFormats.nShadowFramebufferFormat,
		rFormats.nMotionFramebufferFormat,
		rFormats.nAuxFramebufferFormat,
	};

	Terrain3DRdContext *const pContext = m_pResources->get_rd_context();
	for ( uint32_t iPass = 0u; iPass < T3D_RW_AUX_PASS_COUNT; ++iPass ) {
		for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
			Ref<RDPipelineRasterizationState> raster;
			Ref<RDPipelineMultisampleState> multisample;
			Ref<RDPipelineDepthStencilState> depth;
			Ref<RDPipelineColorBlendState> blend;
			Ref<RDPipelineColorBlendStateAttachment> attachment;
			raster.instantiate();
			multisample.instantiate();
			depth.instantiate();
			blend.instantiate();
			attachment.instantiate();
			if ( raster.is_null() || multisample.is_null() || depth.is_null() || blend.is_null() || attachment.is_null() ) {
				return false;
			}

			raster->set_cull_mode(
			        iTier == T3D_RW_TIER_FOLIAGE ? RenderingDevice::POLYGON_CULL_DISABLED : RenderingDevice::POLYGON_CULL_BACK );
			depth->set_enable_depth_test( true );
			depth->set_enable_depth_write( iPass == T3D_RW_AUX_PASS_SHADOW );
			depth->set_depth_compare_operator( RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL );
			attachment->set_enable_blend( false );
			attachment->set_write_r( iPass != T3D_RW_AUX_PASS_SHADOW );
			attachment->set_write_g( iPass != T3D_RW_AUX_PASS_SHADOW );
			attachment->set_write_b( iPass != T3D_RW_AUX_PASS_SHADOW );
			attachment->set_write_a( iPass != T3D_RW_AUX_PASS_SHADOW );

			TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
			attachments.push_back( attachment );
			blend->set_attachments( attachments );

			m_hPipeline[iPass][iTier] = m_pRenderingDevice->render_pipeline_create(
			        m_hShader[iPass], nFormats[iPass], -1, RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, raster, multisample, depth, blend );
			if ( !m_hPipeline[iPass][iTier].is_valid() ) {
				return false;
			}
			if ( !t3dRenderWorldRegisterRid( pContext,
			             m_hPipeline[iPass][iTier],
			             String( "RenderWorld aux pipeline " ) + String::num_uint64( iPass ) + "." + String::num_uint64( iTier ),
			             Terrain3DRdContext::RESOURCE_RASTER_PIPELINE,
			             Terrain3DRdContext::OWNER_RENDER_WORLD_AUX_RASTER ) ) {
				t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hPipeline[iPass][iTier] );
				return false;
			}
			++m_nGpuCreateCount;
		}
	}
	return true;
}

bool Terrain3DRenderWorldAuxRaster::create_uniform_sets_cold() {
	const T3DRenderWorldGpuBufferId commonBuffers[] = {
		T3D_RW_GPU_BUFFER_VISIBILITY_FRAME,
		T3D_RW_GPU_BUFFER_GEOMETRY_VERTEX,
		T3D_RW_GPU_BUFFER_CLUSTER_VERTEX_INDEX,
		T3D_RW_GPU_BUFFER_CLUSTER_TRIANGLE,
		T3D_RW_GPU_BUFFER_CLUSTER_RECORDS,
		T3D_RW_GPU_BUFFER_INSTANCE_RECORDS,
	};
	const T3DRenderWorldGpuBufferId visibleBuffers[] = {
		T3D_RW_GPU_BUFFER_VISIBLE_WORLD_CLUSTERS,
		T3D_RW_GPU_BUFFER_VISIBLE_CLUTTER_CLUSTERS,
		T3D_RW_GPU_BUFFER_VISIBLE_FOLIAGE_CLUSTERS,
	};

	Terrain3DRdContext *const pContext = m_pResources->get_rd_context();
	for ( uint32_t iPass = 0u; iPass < T3D_RW_AUX_PASS_COUNT; ++iPass ) {
		for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
			TypedArray<Ref<RDUniform>> uniforms;
			for ( uint32_t iBuffer = 0u; iBuffer < sizeof( commonBuffers ) / sizeof( commonBuffers[0] ); ++iBuffer ) {
				Ref<RDUniform> uniform;
				uniform.instantiate();
				if ( uniform.is_null() ) {
					return false;
				}
				uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
				uniform->set_binding( ( int32_t )( iBuffer ) );
				uniform->add_id( m_pResources->get_buffer_rid( commonBuffers[iBuffer] ) );
				uniforms.push_back( uniform );
			}

			Ref<RDUniform> visible;
			visible.instantiate();
			if ( visible.is_null() ) {
				return false;
			}
			visible->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
			visible->set_binding( 6 );
			visible->add_id( m_pResources->get_buffer_rid( visibleBuffers[iTier] ) );
			uniforms.push_back( visible );

			m_hUniformSet[iPass][iTier] = m_pRenderingDevice->uniform_set_create( uniforms, m_hShader[iPass], 0u );
			if ( !m_hUniformSet[iPass][iTier].is_valid() ) {
				return false;
			}
			if ( !t3dRenderWorldRegisterRid( pContext,
			             m_hUniformSet[iPass][iTier],
			             String( "RenderWorld aux uniform set " ) + String::num_uint64( iPass ) + "." + String::num_uint64( iTier ),
			             Terrain3DRdContext::RESOURCE_UNIFORM_SET,
			             Terrain3DRdContext::OWNER_RENDER_WORLD_AUX_RASTER ) ) {
				t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hUniformSet[iPass][iTier] );
				return false;
			}
			++m_nGpuCreateCount;
		}
	}
	return true;
}

void Terrain3DRenderWorldAuxRaster::free_rid_cold( RID &rRid ) {
	if ( !m_pRenderingDevice || !rRid.is_valid() ) {
		return;
	}
	t3dRenderWorldFreeRid( m_pResources ? m_pResources->get_rd_context() : NULL, rRid );
	++m_nGpuFreeCount;
}

bool Terrain3DRenderWorldAuxRaster::hot_epoch_unchanged() const {
	return m_nGpuCreateCount == m_nHotCreateEpoch && m_nGpuFreeCount == m_nHotFreeEpoch;
}

bool t3d_render_world_submit_aux_indirect_hot( const int64_t nDrawList,
        const T3DRenderWorldAuxPass pass,
        const Terrain3DRenderWorldResourceImage &rResources,
        const T3DRenderWorldAuxBindings &rBindings,
        uint32_t &rDrawCount ) {
	rDrawCount = 0u;
	Terrain3DRdContext *const pContext = rResources.get_rd_context();
	RenderingDevice *const pRenderingDevice = pContext ? pContext->getDevice() : NULL;
	if ( !pRenderingDevice || nDrawList < 0 || pass >= T3D_RW_AUX_PASS_COUNT || !rResources.is_initialized() ) {
		return false;
	}

	const RID &hIndirect = rResources.get_buffer_rid( T3D_RW_GPU_BUFFER_INDIRECT_ARGS );
	if ( !hIndirect.is_valid() ) {
		return false;
	}

	for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
		if ( !rBindings.hPipeline[pass][iTier].is_valid() ) {
			return false;
		}
		pRenderingDevice->draw_list_bind_render_pipeline( nDrawList, rBindings.hPipeline[pass][iTier] );
		if ( rBindings.hUniformSet[pass][iTier].is_valid() ) {
			pRenderingDevice->draw_list_bind_uniform_set( nDrawList, rBindings.hUniformSet[pass][iTier], rBindings.nUniformSetIndex );
		}
		pRenderingDevice->draw_list_draw_indirect(
		        nDrawList, false, hIndirect, iTier * ( uint32_t )( sizeof( T3DDrawIndirectArgs ) ), 1u, 0u );
		++rDrawCount;
	}
	return true;
}
