// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_gpu_visibility.h"

#include "rendering/rd/render_world/terrain_3d_render_world_rd_owner.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <string.h>

using namespace godot;

Terrain3DRenderWorldGpuVisibility::Terrain3DRenderWorldGpuVisibility() {
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	memset( &m_Stats, 0, sizeof( m_Stats ) );
	m_nHotCreateEpoch = 0u;
	m_nHotFreeEpoch = 0u;
	m_bInitialized = false;
	m_bHotSealed = false;
}

Terrain3DRenderWorldGpuVisibility::~Terrain3DRenderWorldGpuVisibility() {
	// Explicit cold shutdown is required by the ownership contract.
}

bool Terrain3DRenderWorldGpuVisibility::initialize_cold(
        const Terrain3DRenderWorldResourceImage *pResources, const t3dRdRenderWorldShaderBundle_t &rShaders ) {
	Terrain3DRdContext *const pContext = pResources ? pResources->get_rd_context() : NULL;
	RenderingDevice *const pRenderingDevice = pContext ? pContext->getDevice() : NULL;
	if ( m_bInitialized || pRenderingDevice == NULL || pResources == NULL || !pResources->is_initialized() ) {
		return false;
	}
	static_assert( ( int )( PIPELINE_COUNT ) == ( int )( T3D_RD_RW_VISIBILITY_COUNT ), "RenderWorld visibility shader bundle mismatch" );
	m_pRenderingDevice = pRenderingDevice;
	m_pResources = pResources;
	for ( uint32_t i = 0u; i < PIPELINE_COUNT; ++i ) {
		if ( !create_pipeline_cold( ( PipelineId )i, rShaders.m_szVisibility[i] ) ) {
			shutdown_cold();
			return false;
		}
	}
	if ( !create_uniform_sets_cold() ) {
		shutdown_cold();
		return false;
	}
	m_FrameUpload.resize( ( int64_t )sizeof( T3DVisibilityFrame ) );
	if ( m_FrameUpload.size() != ( int64_t )sizeof( T3DVisibilityFrame ) ) {
		shutdown_cold();
		return false;
	}
	m_bInitialized = true;
	return true;
}

void Terrain3DRenderWorldGpuVisibility::shutdown_cold() {
	m_bHotSealed = false;
	if ( m_pRenderingDevice != NULL ) {
		for ( int32_t i = ( int32_t )PIPELINE_COUNT - 1; i >= 0; --i ) {
			free_rid_cold( m_hUniformSet[i] );
		}
		for ( int32_t i = ( int32_t )PIPELINE_COUNT - 1; i >= 0; --i ) {
			free_rid_cold( m_hPipeline[i] );
		}
		for ( int32_t i = ( int32_t )PIPELINE_COUNT - 1; i >= 0; --i ) {
			free_rid_cold( m_hShader[i] );
		}
	}
	m_FrameUpload.resize( 0 );
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	m_bInitialized = false;
}

void Terrain3DRenderWorldGpuVisibility::seal_hot_runtime() {
	if ( !m_bInitialized ) {
		return;
	}
	m_nHotCreateEpoch = m_Stats.nGpuCreateCount;
	m_nHotFreeEpoch = m_Stats.nGpuFreeCount;
	m_bHotSealed = true;
}

bool Terrain3DRenderWorldGpuVisibility::upload_frame_internal( const T3DVisibilityFrame &pFrame ) {
	if ( !m_bInitialized || m_pRenderingDevice == NULL || m_pResources == NULL ) {
		return false;
	}
	if ( m_FrameUpload.size() != ( int64_t )sizeof( T3DVisibilityFrame ) ) {
		return false;
	}
	memcpy( m_FrameUpload.ptrw(), &pFrame, sizeof( pFrame ) );
	const Error Result = m_pRenderingDevice->buffer_update(
	        m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VISIBILITY_FRAME ), 0u, ( uint32_t )sizeof( pFrame ), m_FrameUpload );
	if ( Result != OK ) {
		return false;
	}
	Terrain3DRdContext *const pContext = m_pResources->get_rd_context();
	if ( pContext ) {
		pContext->recordBufferUpdate( sizeof( pFrame ) );
	}
	++m_Stats.nFrameUploadCount;
	m_Stats.nFrameUploadBytes += sizeof( pFrame );
	return true;
}

bool Terrain3DRenderWorldGpuVisibility::upload_frame_reference_debug( const T3DVisibilityFrame &pFrame ) {
	return upload_frame_internal( pFrame );
}

bool Terrain3DRenderWorldGpuVisibility::publish_frame_hot( const T3DVisibilityFrame &pFrame ) {
	if ( !m_bHotSealed || !hot_epoch_unchanged() ) {
		++m_Stats.nHotViolationCount;
		return false;
	}
	if ( !upload_frame_internal( pFrame ) ) {
		return false;
	}
	if ( !hot_epoch_unchanged() ) {
		++m_Stats.nHotViolationCount;
		return false;
	}
	return true;
}

bool Terrain3DRenderWorldGpuVisibility::dispatch_m04_hot( const T3DRenderWorldLimits &pLimits, const T3DVisibilityFrame &pFrame ) {
	return dispatch_internal_hot( pLimits, pFrame, m_hPipeline[PIPELINE_CLUSTER], m_hUniformSet[PIPELINE_CLUSTER] );
}

bool Terrain3DRenderWorldGpuVisibility::dispatch_with_cluster_override_hot( const T3DRenderWorldLimits &pLimits,
        const T3DVisibilityFrame &pFrame,
        const RID &pClusterPipeline,
        const RID &pClusterUniformSet ) {
	if ( !pClusterPipeline.is_valid() || !pClusterUniformSet.is_valid() ) {
		return false;
	}
	return dispatch_internal_hot( pLimits, pFrame, pClusterPipeline, pClusterUniformSet );
}

bool Terrain3DRenderWorldGpuVisibility::dispatch_internal_hot( const T3DRenderWorldLimits &pLimits,
        const T3DVisibilityFrame &pFrame,
        const RID &pClusterPipeline,
        const RID &pClusterUniformSet ) {
	if ( !m_bInitialized || !m_bHotSealed || m_pRenderingDevice == NULL || m_pResources == NULL ) {
		return false;
	}
	if ( !hot_epoch_unchanged() ) {
		++m_Stats.nHotViolationCount;
		return false;
	}
	const uint32_t nLiveCells = pFrame.nCellCount < pLimits.nMaxCells ? pFrame.nCellCount : pLimits.nMaxCells;
	const uint32_t nCellGroups = ( nLiveCells + 63u ) / 64u;
	const RID &hIndirect = m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_INDIRECT_ARGS );
	if ( !hIndirect.is_valid() ) {
		return false;
	}

	const int64_t nList = m_pRenderingDevice->compute_list_begin();
	m_pRenderingDevice->compute_list_bind_compute_pipeline( nList, m_hPipeline[PIPELINE_RESET] );
	m_pRenderingDevice->compute_list_bind_uniform_set( nList, m_hUniformSet[PIPELINE_RESET], 0u );
	m_pRenderingDevice->compute_list_dispatch( nList, 1u, 1u, 1u );
	++m_Stats.nDispatchCount;
	m_pRenderingDevice->compute_list_add_barrier( nList );
	++m_Stats.nBarrierCount;

	if ( nCellGroups != 0u ) {
		m_pRenderingDevice->compute_list_bind_compute_pipeline( nList, m_hPipeline[PIPELINE_CELL] );
		m_pRenderingDevice->compute_list_bind_uniform_set( nList, m_hUniformSet[PIPELINE_CELL], 0u );
		m_pRenderingDevice->compute_list_dispatch( nList, nCellGroups, 1u, 1u );
		++m_Stats.nDispatchCount;
		m_Stats.nLaunchedVariableGroups += nCellGroups;
		m_Stats.nRequiredVariableGroups += nCellGroups;
		m_pRenderingDevice->compute_list_add_barrier( nList );
		++m_Stats.nBarrierCount;
	}

	// Materialize live GPU counters into compute-indirect args before every
	// counter-dependent stage. The one-thread args pass is deliberately tiny.
	m_pRenderingDevice->compute_list_bind_compute_pipeline( nList, m_hPipeline[PIPELINE_ARGS] );
	m_pRenderingDevice->compute_list_bind_uniform_set( nList, m_hUniformSet[PIPELINE_ARGS], 0u );
	m_pRenderingDevice->compute_list_dispatch( nList, 1u, 1u, 1u );
	++m_Stats.nDispatchCount;
	m_pRenderingDevice->compute_list_add_barrier( nList );
	++m_Stats.nBarrierCount;

	m_pRenderingDevice->compute_list_bind_compute_pipeline( nList, m_hPipeline[PIPELINE_INSTANCE] );
	m_pRenderingDevice->compute_list_bind_uniform_set( nList, m_hUniformSet[PIPELINE_INSTANCE], 0u );
	m_pRenderingDevice->compute_list_dispatch_indirect( nList, hIndirect, T3D_RW_INDIRECT_VIS_INSTANCE_OFFSET );
	++m_Stats.nDispatchCount;
	++m_Stats.nIndirectDispatchCount;
	m_pRenderingDevice->compute_list_add_barrier( nList );
	++m_Stats.nBarrierCount;

	m_pRenderingDevice->compute_list_bind_compute_pipeline( nList, m_hPipeline[PIPELINE_ARGS] );
	m_pRenderingDevice->compute_list_bind_uniform_set( nList, m_hUniformSet[PIPELINE_ARGS], 0u );
	m_pRenderingDevice->compute_list_dispatch( nList, 1u, 1u, 1u );
	++m_Stats.nDispatchCount;
	m_pRenderingDevice->compute_list_add_barrier( nList );
	++m_Stats.nBarrierCount;

	m_pRenderingDevice->compute_list_bind_compute_pipeline( nList, pClusterPipeline );
	m_pRenderingDevice->compute_list_bind_uniform_set( nList, pClusterUniformSet, 0u );
	m_pRenderingDevice->compute_list_dispatch_indirect( nList, hIndirect, T3D_RW_INDIRECT_VIS_CLUSTER_OFFSET );
	++m_Stats.nDispatchCount;
	++m_Stats.nIndirectDispatchCount;
	m_pRenderingDevice->compute_list_add_barrier( nList );
	++m_Stats.nBarrierCount;

	// Terminal args generation also prepares draw args and all three VT tier
	// dispatch triplets from final live visible-cluster counters.
	m_pRenderingDevice->compute_list_bind_compute_pipeline( nList, m_hPipeline[PIPELINE_ARGS] );
	m_pRenderingDevice->compute_list_bind_uniform_set( nList, m_hUniformSet[PIPELINE_ARGS], 0u );
	m_pRenderingDevice->compute_list_dispatch( nList, 1u, 1u, 1u );
	++m_Stats.nDispatchCount;
	m_pRenderingDevice->compute_list_add_barrier( nList );
	++m_Stats.nBarrierCount;
	m_pRenderingDevice->compute_list_end();
	return hot_epoch_unchanged();
}

bool Terrain3DRenderWorldGpuVisibility::is_initialized() const {
	return m_bInitialized;
}

const T3DGpuVisibilityStats &Terrain3DRenderWorldGpuVisibility::get_stats() const {
	return m_Stats;
}

const RID &Terrain3DRenderWorldGpuVisibility::get_cluster_pipeline_rid() const {
	return m_hPipeline[PIPELINE_CLUSTER];
}

const RID &Terrain3DRenderWorldGpuVisibility::get_cluster_uniform_set_rid() const {
	return m_hUniformSet[PIPELINE_CLUSTER];
}

bool Terrain3DRenderWorldGpuVisibility::create_pipeline_cold( PipelineId pId, const Ref<RDShaderSPIRV> &rSpirv ) {
	if ( m_pRenderingDevice == NULL || rSpirv.is_null() || ( uint32_t )pId >= PIPELINE_COUNT ) {
		return false;
	}
	m_hShader[pId] = m_pRenderingDevice->shader_create_from_spirv( rSpirv );
	if ( !m_hShader[pId].is_valid() ) {
		return false;
	}
	if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
	             m_hShader[pId],
	             String( "RenderWorld visibility shader " ) + String::num_uint64( ( uint64_t )pId ),
	             Terrain3DRdContext::RESOURCE_SHADER,
	             Terrain3DRdContext::OWNER_RENDER_WORLD_VISIBILITY ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hShader[pId] );
		return false;
	}
	++m_Stats.nGpuCreateCount;
	m_hPipeline[pId] = m_pRenderingDevice->compute_pipeline_create( m_hShader[pId] );
	if ( !m_hPipeline[pId].is_valid() ) {
		return false;
	}
	if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
	             m_hPipeline[pId],
	             String( "RenderWorld visibility pipeline " ) + String::num_uint64( ( uint64_t )pId ),
	             Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE,
	             Terrain3DRdContext::OWNER_RENDER_WORLD_VISIBILITY ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hPipeline[pId] );
		return false;
	}
	++m_Stats.nGpuCreateCount;
	return true;
}

bool Terrain3DRenderWorldGpuVisibility::create_uniform_set_cold(
        PipelineId pId, const T3DRenderWorldGpuBufferId *pBuffers, uint32_t pBufferCount ) {
	if ( m_pRenderingDevice == NULL || m_pResources == NULL || pBuffers == NULL || pBufferCount == 0u ) {
		return false;
	}
	TypedArray<Ref<RDUniform>> Uniforms;
	for ( uint32_t i = 0u; i < pBufferCount; ++i ) {
		const RID &hBuffer = m_pResources->get_buffer_rid( pBuffers[i] );
		if ( !hBuffer.is_valid() ) {
			return false;
		}
		Ref<RDUniform> Uniform;
		Uniform.instantiate();
		if ( Uniform.is_null() ) {
			return false;
		}
		Uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
		Uniform->set_binding( ( int32_t )i );
		Uniform->add_id( hBuffer );
		Uniforms.push_back( Uniform );
	}
	m_hUniformSet[pId] = m_pRenderingDevice->uniform_set_create( Uniforms, m_hShader[pId], 0u );
	if ( !m_hUniformSet[pId].is_valid() ) {
		return false;
	}
	if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
	             m_hUniformSet[pId],
	             String( "RenderWorld visibility uniform set " ) + String::num_uint64( ( uint64_t )pId ),
	             Terrain3DRdContext::RESOURCE_UNIFORM_SET,
	             Terrain3DRdContext::OWNER_RENDER_WORLD_VISIBILITY ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hUniformSet[pId] );
		return false;
	}
	++m_Stats.nGpuCreateCount;
	return true;
}

bool Terrain3DRenderWorldGpuVisibility::create_uniform_sets_cold() {
	const T3DRenderWorldGpuBufferId aReset[] = { T3D_RW_GPU_BUFFER_COUNTERS, T3D_RW_GPU_BUFFER_INDIRECT_ARGS };
	const T3DRenderWorldGpuBufferId aCell[] = {
		T3D_RW_GPU_BUFFER_VISIBILITY_FRAME, T3D_RW_GPU_BUFFER_CELL_RECORDS, T3D_RW_GPU_BUFFER_VISIBLE_CELLS, T3D_RW_GPU_BUFFER_COUNTERS
	};
	const T3DRenderWorldGpuBufferId aInstance[] = { T3D_RW_GPU_BUFFER_VISIBILITY_FRAME,
		T3D_RW_GPU_BUFFER_CELL_RECORDS,
		T3D_RW_GPU_BUFFER_VISIBLE_CELLS,
		T3D_RW_GPU_BUFFER_INSTANCE_RECORDS,
		T3D_RW_GPU_BUFFER_MESH_RECORDS,
		T3D_RW_GPU_BUFFER_MESH_LOD_RECORDS,
		T3D_RW_GPU_BUFFER_VISIBLE_INSTANCES,
		T3D_RW_GPU_BUFFER_SELECTED_LOD,
		T3D_RW_GPU_BUFFER_COUNTERS,
		T3D_RW_GPU_BUFFER_CLUSTER_RECORDS };
	const T3DRenderWorldGpuBufferId aCluster[] = { T3D_RW_GPU_BUFFER_VISIBILITY_FRAME,
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
	const T3DRenderWorldGpuBufferId aArgs[] = {
		T3D_RW_GPU_BUFFER_VISIBILITY_FRAME, T3D_RW_GPU_BUFFER_COUNTERS, T3D_RW_GPU_BUFFER_INDIRECT_ARGS
	};
	return create_uniform_set_cold( PIPELINE_RESET, aReset, sizeof( aReset ) / sizeof( aReset[0] ) )
	        && create_uniform_set_cold( PIPELINE_CELL, aCell, sizeof( aCell ) / sizeof( aCell[0] ) )
	        && create_uniform_set_cold( PIPELINE_INSTANCE, aInstance, sizeof( aInstance ) / sizeof( aInstance[0] ) )
	        && create_uniform_set_cold( PIPELINE_CLUSTER, aCluster, sizeof( aCluster ) / sizeof( aCluster[0] ) )
	        && create_uniform_set_cold( PIPELINE_ARGS, aArgs, sizeof( aArgs ) / sizeof( aArgs[0] ) );
}

void Terrain3DRenderWorldGpuVisibility::free_rid_cold( RID &rRid ) {
	if ( m_pRenderingDevice == NULL || !rRid.is_valid() ) {
		return;
	}
	t3dRenderWorldFreeRid( m_pResources ? m_pResources->get_rd_context() : NULL, rRid );
	++m_Stats.nGpuFreeCount;
}

bool Terrain3DRenderWorldGpuVisibility::hot_epoch_unchanged() const {
	return m_Stats.nGpuCreateCount == m_nHotCreateEpoch && m_Stats.nGpuFreeCount == m_nHotFreeEpoch;
}
