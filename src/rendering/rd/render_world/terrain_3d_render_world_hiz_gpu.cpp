// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_hiz_gpu.h"

#include "rendering/rd/render_world/terrain_3d_render_world_rd_owner.h"

#include <godot_cpp/classes/rd_pipeline_specialization_constant.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <string.h>

using namespace godot;

Terrain3DRenderWorldHiZGpu::Terrain3DRenderWorldHiZGpu() {
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	memset( &m_Stats, 0, sizeof( m_Stats ) );
	m_nHotCreateEpoch = 0u;
	m_nHotFreeEpoch = 0u;
	m_nMaximumWidth = 0u;
	m_nMaximumHeight = 0u;
	m_nMaximumMipCount = 0u;
	m_nPreviousDepthWidth = 0u;
	m_nPreviousDepthHeight = 0u;
	m_bInitialized = false;
	m_bHotSealed = false;
}

Terrain3DRenderWorldHiZGpu::~Terrain3DRenderWorldHiZGpu() {
	// Explicit shutdown_cold() owns teardown.
}

bool Terrain3DRenderWorldHiZGpu::initialize_cold( const Terrain3DRenderWorldResourceImage *pResources,
        const T3DRenderWorldGpuProfile &pProfile,
        const RID &pBorrowedPreviousDepth,
        const t3dRdRenderWorldShaderBundle_t &rShaders ) {
	Terrain3DRdContext *const pContext = pResources ? pResources->get_rd_context() : NULL;
	RenderingDevice *const pRenderingDevice = pContext ? pContext->getDevice() : NULL;
	if ( m_bInitialized || pRenderingDevice == NULL || pResources == NULL || !pResources->is_initialized() ) {
		return false;
	}
	if ( !pBorrowedPreviousDepth.is_valid() || !pRenderingDevice->texture_is_valid( pBorrowedPreviousDepth ) ) {
		return false;
	}
	if ( pProfile.nHiZMipCount == 0u || pProfile.nHiZMipCount > T3D_RW_HIZ_MAX_MIPS ) {
		return false;
	}
	const Ref<RDTextureFormat> PreviousFormat = pRenderingDevice->texture_get_format( pBorrowedPreviousDepth );
	if ( PreviousFormat.is_null() ) {
		return false;
	}

	m_pRenderingDevice = pRenderingDevice;
	m_pResources = pResources;
	m_hBorrowedPreviousDepth = pBorrowedPreviousDepth;
	m_nMaximumWidth = pProfile.nHiZWidth;
	m_nMaximumHeight = pProfile.nHiZHeight;
	m_nMaximumMipCount = pProfile.nHiZMipCount;
	m_nPreviousDepthWidth = PreviousFormat->get_width();
	m_nPreviousDepthHeight = PreviousFormat->get_height();
	if ( m_nPreviousDepthWidth == 0u || m_nPreviousDepthHeight == 0u ) {
		shutdown_cold();
		return false;
	}
	if ( !create_sampler_cold() || !create_mip_views_cold()
	        || !create_reduce_shader_and_pipelines_cold( rShaders.m_szHiZ[T3D_RD_RW_HIZ_REDUCE] )
	        || !create_cluster_shader_and_pipeline_cold( rShaders.m_szHiZ[T3D_RD_RW_HIZ_CLUSTER] ) || !create_reduce_uniform_sets_cold()
	        || !create_cluster_uniform_set_cold() ) {
		shutdown_cold();
		return false;
	}
	if ( m_pRenderingDevice->buffer_clear( m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VISIBILITY_HISTORY ),
	             0u,
	             ( uint32_t )m_pResources->get_plan().nBufferBytes[T3D_RW_GPU_BUFFER_VISIBILITY_HISTORY] )
	        != OK ) {
		shutdown_cold();
		return false;
	}
	++m_Stats.nHistoryClearCount;
	m_bInitialized = true;
	return true;
}

void Terrain3DRenderWorldHiZGpu::shutdown_cold() {
	m_bHotSealed = false;
	if ( m_pRenderingDevice != NULL ) {
		free_rid_cold( m_hClusterUniformSet );
		free_rid_cold( m_hClusterPipeline );
		free_rid_cold( m_hClusterShader );
		for ( int32_t i = ( int32_t )m_nMaximumMipCount - 1; i >= 0; --i ) {
			free_rid_cold( m_hReduceUniformSet[i] );
		}
		for ( int32_t i = ( int32_t )m_nMaximumMipCount - 1; i >= 0; --i ) {
			free_rid_cold( m_hReducePipeline[i] );
		}
		free_rid_cold( m_hReduceShader );
		for ( int32_t i = ( int32_t )m_nMaximumMipCount - 1; i >= 0; --i ) {
			free_rid_cold( m_hMipView[i] );
		}
		free_rid_cold( m_hSampler );
	}
	m_hBorrowedPreviousDepth = RID();
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	m_nMaximumWidth = 0u;
	m_nMaximumHeight = 0u;
	m_nMaximumMipCount = 0u;
	m_nPreviousDepthWidth = 0u;
	m_nPreviousDepthHeight = 0u;
	m_bInitialized = false;
}

void Terrain3DRenderWorldHiZGpu::seal_hot_runtime() {
	if ( !m_bInitialized ) {
		return;
	}
	m_nHotCreateEpoch = m_Stats.nGpuCreateCount;
	m_nHotFreeEpoch = m_Stats.nGpuFreeCount;
	m_bHotSealed = true;
}

bool Terrain3DRenderWorldHiZGpu::dispatch_hiz_hot( uint32_t pActiveWidth, uint32_t pActiveHeight, uint32_t pActiveMipCount ) {
	if ( !m_bInitialized || !m_bHotSealed || m_pRenderingDevice == NULL ) {
		return false;
	}
	if ( !hot_epoch_unchanged() ) {
		++m_Stats.nHotViolationCount;
		return false;
	}
	if ( pActiveWidth == 0u || pActiveHeight == 0u || pActiveMipCount == 0u || pActiveMipCount > m_nMaximumMipCount ) {
		return false;
	}
	if ( pActiveWidth > m_nMaximumWidth || pActiveHeight > m_nMaximumHeight ) {
		return false;
	}
	if ( pActiveWidth > m_nPreviousDepthWidth || pActiveHeight > m_nPreviousDepthHeight ) {
		return false;
	}

	const int64_t nList = m_pRenderingDevice->compute_list_begin();
	uint32_t nWidth = pActiveWidth;
	uint32_t nHeight = pActiveHeight;
	for ( uint32_t iMip = 0u; iMip < pActiveMipCount; ++iMip ) {
		const uint32_t nGroupsX = ( nWidth + 7u ) / 8u;
		const uint32_t nGroupsY = ( nHeight + 7u ) / 8u;
		m_pRenderingDevice->compute_list_bind_compute_pipeline( nList, m_hReducePipeline[iMip] );
		m_pRenderingDevice->compute_list_bind_uniform_set( nList, m_hReduceUniformSet[iMip], 0u );
		m_pRenderingDevice->compute_list_dispatch( nList, nGroupsX, nGroupsY, 1u );
		++m_Stats.nDispatchCount;
		m_pRenderingDevice->compute_list_add_barrier( nList );
		++m_Stats.nBarrierCount;
		if ( nWidth > 1u ) {
			nWidth = ( nWidth + 1u ) >> 1u;
		}
		if ( nHeight > 1u ) {
			nHeight = ( nHeight + 1u ) >> 1u;
		}
	}
	m_pRenderingDevice->compute_list_end();
	return hot_epoch_unchanged();
}

bool Terrain3DRenderWorldHiZGpu::dispatch_visibility_m05_hot(
        Terrain3DRenderWorldGpuVisibility &rVisibility, const T3DRenderWorldLimits &pLimits ) {
	if ( !m_bInitialized || !m_bHotSealed || !hot_epoch_unchanged() ) {
		return false;
	}
	return rVisibility.dispatch_with_cluster_override_hot( pLimits, m_hClusterPipeline, m_hClusterUniformSet );
}

bool Terrain3DRenderWorldHiZGpu::is_initialized() const {
	return m_bInitialized;
}

const RID &Terrain3DRenderWorldHiZGpu::get_borrowed_previous_depth_rid() const {
	return m_hBorrowedPreviousDepth;
}

const RID &Terrain3DRenderWorldHiZGpu::get_cluster_pipeline_rid() const {
	return m_hClusterPipeline;
}

const RID &Terrain3DRenderWorldHiZGpu::get_cluster_uniform_set_rid() const {
	return m_hClusterUniformSet;
}

const T3DHiZGpuStats &Terrain3DRenderWorldHiZGpu::get_stats() const {
	return m_Stats;
}

bool Terrain3DRenderWorldHiZGpu::create_sampler_cold() {
	Ref<RDSamplerState> Sampler;
	Sampler.instantiate();
	if ( Sampler.is_null() ) {
		return false;
	}
	Sampler->set_mag_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
	Sampler->set_min_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
	Sampler->set_mip_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
	Sampler->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
	Sampler->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
	Sampler->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
	m_hSampler = m_pRenderingDevice->sampler_create( Sampler );
	if ( !m_hSampler.is_valid() ) {
		return false;
	}
	if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
	             m_hSampler,
	             "RenderWorld Hi-Z sampler",
	             Terrain3DRdContext::RESOURCE_SAMPLER,
	             Terrain3DRdContext::OWNER_RENDER_WORLD_HIZ ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hSampler );
		return false;
	}
	++m_Stats.nGpuCreateCount;
	return true;
}

bool Terrain3DRenderWorldHiZGpu::create_mip_views_cold() {
	Ref<RDTextureView> View;
	View.instantiate();
	if ( View.is_null() ) {
		return false;
	}
	for ( uint32_t iMip = 0u; iMip < m_nMaximumMipCount; ++iMip ) {
		m_hMipView[iMip] = m_pRenderingDevice->texture_create_shared_from_slice(
		        View, m_pResources->get_hiz_rid(), 0u, iMip, 1u, RenderingDevice::TEXTURE_SLICE_2D );
		if ( !m_hMipView[iMip].is_valid() ) {
			return false;
		}
		if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
		             m_hMipView[iMip],
		             String( "RenderWorld Hi-Z mip view " ) + String::num_uint64( ( uint64_t )iMip ),
		             Terrain3DRdContext::RESOURCE_TEXTURE,
		             Terrain3DRdContext::OWNER_RENDER_WORLD_HIZ ) ) {
			t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hMipView[iMip] );
			return false;
		}
		++m_Stats.nGpuCreateCount;
	}
	return true;
}

bool Terrain3DRenderWorldHiZGpu::create_reduce_shader_and_pipelines_cold( const Ref<RDShaderSPIRV> &rSpirv ) {
	if ( rSpirv.is_null() ) {
		return false;
	}
	m_hReduceShader = m_pRenderingDevice->shader_create_from_spirv( rSpirv );
	if ( !m_hReduceShader.is_valid() ) {
		return false;
	}
	if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
	             m_hReduceShader,
	             "RenderWorld Hi-Z reduce shader",
	             Terrain3DRdContext::RESOURCE_SHADER,
	             Terrain3DRdContext::OWNER_RENDER_WORLD_HIZ ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hReduceShader );
		return false;
	}
	++m_Stats.nGpuCreateCount;
	for ( uint32_t iMip = 0u; iMip < m_nMaximumMipCount; ++iMip ) {
		Ref<RDPipelineSpecializationConstant> Constant;
		Constant.instantiate();
		if ( Constant.is_null() ) {
			return false;
		}
		Constant->set_constant_id( 0u );
		Constant->set_value( Variant( ( int64_t )iMip ) );
		TypedArray<Ref<RDPipelineSpecializationConstant>> Constants;
		Constants.push_back( Constant );
		m_hReducePipeline[iMip] = m_pRenderingDevice->compute_pipeline_create( m_hReduceShader, Constants );
		if ( !m_hReducePipeline[iMip].is_valid() ) {
			return false;
		}
		if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
		             m_hReducePipeline[iMip],
		             String( "RenderWorld Hi-Z reduce pipeline " ) + String::num_uint64( ( uint64_t )iMip ),
		             Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE,
		             Terrain3DRdContext::OWNER_RENDER_WORLD_HIZ ) ) {
			t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hReducePipeline[iMip] );
			return false;
		}
		++m_Stats.nGpuCreateCount;
	}
	return true;
}

bool Terrain3DRenderWorldHiZGpu::create_cluster_shader_and_pipeline_cold( const Ref<RDShaderSPIRV> &rSpirv ) {
	if ( rSpirv.is_null() ) {
		return false;
	}
	m_hClusterShader = m_pRenderingDevice->shader_create_from_spirv( rSpirv );
	if ( !m_hClusterShader.is_valid() ) {
		return false;
	}
	if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
	             m_hClusterShader,
	             "RenderWorld Hi-Z cluster shader",
	             Terrain3DRdContext::RESOURCE_SHADER,
	             Terrain3DRdContext::OWNER_RENDER_WORLD_HIZ ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hClusterShader );
		return false;
	}
	++m_Stats.nGpuCreateCount;
	m_hClusterPipeline = m_pRenderingDevice->compute_pipeline_create( m_hClusterShader );
	if ( !m_hClusterPipeline.is_valid() ) {
		return false;
	}
	if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
	             m_hClusterPipeline,
	             "RenderWorld Hi-Z cluster pipeline",
	             Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE,
	             Terrain3DRdContext::OWNER_RENDER_WORLD_HIZ ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hClusterPipeline );
		return false;
	}
	++m_Stats.nGpuCreateCount;
	return true;
}

bool Terrain3DRenderWorldHiZGpu::add_storage_uniform_cold( TypedArray<Ref<RDUniform>> &rUniforms, int32_t pBinding, const RID &pRid ) {
	if ( !pRid.is_valid() ) {
		return false;
	}
	Ref<RDUniform> Uniform;
	Uniform.instantiate();
	if ( Uniform.is_null() ) {
		return false;
	}
	Uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
	Uniform->set_binding( pBinding );
	Uniform->add_id( pRid );
	rUniforms.push_back( Uniform );
	return true;
}

bool Terrain3DRenderWorldHiZGpu::add_sampler_texture_uniform_cold(
        TypedArray<Ref<RDUniform>> &rUniforms, int32_t pBinding, const RID &pTexture ) {
	if ( !m_hSampler.is_valid() || !pTexture.is_valid() ) {
		return false;
	}
	Ref<RDUniform> Uniform;
	Uniform.instantiate();
	if ( Uniform.is_null() ) {
		return false;
	}
	Uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE );
	Uniform->set_binding( pBinding );
	Uniform->add_id( m_hSampler );
	Uniform->add_id( pTexture );
	rUniforms.push_back( Uniform );
	return true;
}

bool Terrain3DRenderWorldHiZGpu::add_image_uniform_cold( TypedArray<Ref<RDUniform>> &rUniforms, int32_t pBinding, const RID &pTexture ) {
	if ( !pTexture.is_valid() ) {
		return false;
	}
	Ref<RDUniform> Uniform;
	Uniform.instantiate();
	if ( Uniform.is_null() ) {
		return false;
	}
	Uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_IMAGE );
	Uniform->set_binding( pBinding );
	Uniform->add_id( pTexture );
	rUniforms.push_back( Uniform );
	return true;
}

bool Terrain3DRenderWorldHiZGpu::create_reduce_uniform_sets_cold() {
	for ( uint32_t iMip = 0u; iMip < m_nMaximumMipCount; ++iMip ) {
		TypedArray<Ref<RDUniform>> Uniforms;
		if ( !add_storage_uniform_cold( Uniforms, 0, m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VISIBILITY_FRAME ) ) ) {
			return false;
		}
		const RID &hSource = iMip == 0u ? m_hBorrowedPreviousDepth : m_hMipView[iMip - 1u];
		if ( !add_sampler_texture_uniform_cold( Uniforms, 1, hSource ) ) {
			return false;
		}
		if ( !add_image_uniform_cold( Uniforms, 2, m_hMipView[iMip] ) ) {
			return false;
		}
		m_hReduceUniformSet[iMip] = m_pRenderingDevice->uniform_set_create( Uniforms, m_hReduceShader, 0u );
		if ( !m_hReduceUniformSet[iMip].is_valid() ) {
			return false;
		}
		if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
		             m_hReduceUniformSet[iMip],
		             String( "RenderWorld Hi-Z reduce uniform set " ) + String::num_uint64( ( uint64_t )iMip ),
		             Terrain3DRdContext::RESOURCE_UNIFORM_SET,
		             Terrain3DRdContext::OWNER_RENDER_WORLD_HIZ ) ) {
			t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hReduceUniformSet[iMip] );
			return false;
		}
		++m_Stats.nGpuCreateCount;
	}
	return true;
}

bool Terrain3DRenderWorldHiZGpu::create_cluster_uniform_set_cold() {
	const T3DRenderWorldGpuBufferId aBuffers[] = { T3D_RW_GPU_BUFFER_VISIBILITY_FRAME,
		T3D_RW_GPU_BUFFER_INSTANCE_RECORDS,
		T3D_RW_GPU_BUFFER_MESH_RECORDS,
		T3D_RW_GPU_BUFFER_MESH_LOD_RECORDS,
		T3D_RW_GPU_BUFFER_CLUSTER_RECORDS,
		T3D_RW_GPU_BUFFER_VISIBLE_INSTANCES,
		T3D_RW_GPU_BUFFER_SELECTED_LOD,
		T3D_RW_GPU_BUFFER_VISIBLE_WORLD_CLUSTERS,
		T3D_RW_GPU_BUFFER_VISIBLE_CLUTTER_CLUSTERS,
		T3D_RW_GPU_BUFFER_VISIBLE_FOLIAGE_CLUSTERS,
		T3D_RW_GPU_BUFFER_COUNTERS };
	TypedArray<Ref<RDUniform>> Uniforms;
	for ( uint32_t i = 0u; i < sizeof( aBuffers ) / sizeof( aBuffers[0] ); ++i ) {
		if ( !add_storage_uniform_cold( Uniforms, ( int32_t )i, m_pResources->get_buffer_rid( aBuffers[i] ) ) ) {
			return false;
		}
	}
	if ( !add_sampler_texture_uniform_cold( Uniforms, 11, m_pResources->get_hiz_rid() ) ) {
		return false;
	}
	if ( !add_storage_uniform_cold( Uniforms, 12, m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VISIBILITY_HISTORY ) ) ) {
		return false;
	}
	m_hClusterUniformSet = m_pRenderingDevice->uniform_set_create( Uniforms, m_hClusterShader, 0u );
	if ( !m_hClusterUniformSet.is_valid() ) {
		return false;
	}
	if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
	             m_hClusterUniformSet,
	             "RenderWorld Hi-Z cluster uniform set",
	             Terrain3DRdContext::RESOURCE_UNIFORM_SET,
	             Terrain3DRdContext::OWNER_RENDER_WORLD_HIZ ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hClusterUniformSet );
		return false;
	}
	++m_Stats.nGpuCreateCount;
	return true;
}

void Terrain3DRenderWorldHiZGpu::free_rid_cold( RID &rRid ) {
	if ( m_pRenderingDevice == NULL || !rRid.is_valid() ) {
		return;
	}
	t3dRenderWorldFreeRid( m_pResources ? m_pResources->get_rd_context() : NULL, rRid );
	++m_Stats.nGpuFreeCount;
}

bool Terrain3DRenderWorldHiZGpu::hot_epoch_unchanged() const {
	return m_Stats.nGpuCreateCount == m_nHotCreateEpoch && m_Stats.nGpuFreeCount == m_nHotFreeEpoch;
}
