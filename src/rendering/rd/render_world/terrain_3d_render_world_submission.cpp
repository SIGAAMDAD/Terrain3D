// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_submission.h"

#include <godot_cpp/variant/packed_byte_array.hpp>

#include <string.h>

using namespace godot;

static bool t3d_rw_upload_bytes_debug( Terrain3DRdContext &rContext,
        const RID &pBuffer,
        uint32_t pOffset,
        const void *pSource,
        uint32_t pBytes,
        T3DThreeTierUploadStats &rStats ) {
	RenderingDevice *const pRenderingDevice = rContext.getDevice();
	if ( pRenderingDevice == NULL || !pBuffer.is_valid() || pSource == NULL || pBytes == 0u ) {
		return false;
	}
	PackedByteArray Data;
	Data.resize( ( int64_t )pBytes );
	memcpy( Data.ptrw(), pSource, pBytes );
	if ( pRenderingDevice->buffer_update( pBuffer, pOffset, pBytes, Data ) != OK ) {
		return false;
	}
	rContext.recordBufferUpdate( pBytes );
	++rStats.nUploadCallCount;
	rStats.nUploadBytes += pBytes;
	return true;
}

T3DThreeTierDrawPlan t3d_render_world_build_three_tier_draw_plan_reference_debug( const T3DVisibilityReferenceResult &pResult ) {
	T3DThreeTierDrawPlan Plan = {};
	for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
		Plan.nIndirectOffset[iTier] = iTier * ( uint32_t )sizeof( T3DDrawIndirectArgs );
		Plan.nEnabled[iTier] = pResult.DrawArgs[iTier].nInstanceCount != 0u ? 1u : 0u;
		Plan.nDrawCount += Plan.nEnabled[iTier];
	}
	return Plan;
}

bool t3d_render_world_upload_scene_database_warm( const Terrain3DRenderWorldResourceImage &pResources,
        const T3DGeometryDatabase &pGeometry,
        const T3DSceneCell *pCells,
        uint32_t pCellCount,
        const T3DInstanceRecord *pInstances,
        uint32_t pInstanceCount,
        const T3DMaterialRecord *pMaterials,
        uint32_t pMaterialCount,
        T3DThreeTierUploadStats &rStats ) {
	memset( &rStats, 0, sizeof( rStats ) );
	Terrain3DRdContext *const pContext = pResources.get_rd_context();
	if ( pContext == NULL || pContext->getDevice() == NULL || !pResources.is_initialized() ) {
		return false;
	}
	if ( pGeometry.Buffers.pVertexHeap == NULL || pGeometry.Buffers.pClusterVertexIndexHeap == NULL
	        || pGeometry.Buffers.pTriangleHeap == NULL || pGeometry.Buffers.pMeshRecords == NULL || pGeometry.Buffers.pLodRecords == NULL
	        || pGeometry.Buffers.pClusterRecords == NULL ) {
		return false;
	}
	if ( ( pCellCount != 0u && pCells == NULL ) || ( pInstanceCount != 0u && pInstances == NULL )
	        || ( pMaterialCount != 0u && pMaterials == NULL ) ) {
		return false;
	}

	struct UploadSpan {
		T3DRenderWorldGpuBufferId eBuffer;
		const void *pData;
		uint64_t nBytes;
	};
	const UploadSpan aUploads[] = { { T3D_RW_GPU_BUFFER_GEOMETRY_VERTEX,
		                                    pGeometry.Buffers.pVertexHeap,
		                                    ( uint64_t )pGeometry.nVertexCount * sizeof( T3DPrototypeVertex ) },
		{ T3D_RW_GPU_BUFFER_CLUSTER_VERTEX_INDEX,
		        pGeometry.Buffers.pClusterVertexIndexHeap,
		        ( uint64_t )pGeometry.nClusterVertexIndexCount * sizeof( uint32_t ) },
		{ T3D_RW_GPU_BUFFER_CLUSTER_TRIANGLE,
		        pGeometry.Buffers.pTriangleHeap,
		        ( uint64_t )pGeometry.nTriangleCount * sizeof( T3DPackedClusterTriangle ) },
		{ T3D_RW_GPU_BUFFER_MESH_RECORDS, pGeometry.Buffers.pMeshRecords, ( uint64_t )pGeometry.nMeshCount * sizeof( T3DMeshRecord ) },
		{ T3D_RW_GPU_BUFFER_MESH_LOD_RECORDS, pGeometry.Buffers.pLodRecords, ( uint64_t )pGeometry.nLodCount * sizeof( T3DMeshLodRecord ) },
		{ T3D_RW_GPU_BUFFER_CLUSTER_RECORDS,
		        pGeometry.Buffers.pClusterRecords,
		        ( uint64_t )pGeometry.nClusterCount * sizeof( T3DClusterRecord ) },
		{ T3D_RW_GPU_BUFFER_CELL_RECORDS, pCells, ( uint64_t )pCellCount * sizeof( T3DSceneCell ) },
		{ T3D_RW_GPU_BUFFER_INSTANCE_RECORDS, pInstances, ( uint64_t )pInstanceCount * sizeof( T3DInstanceRecord ) },
		{ T3D_RW_GPU_BUFFER_MATERIAL_RECORDS, pMaterials, ( uint64_t )pMaterialCount * sizeof( T3DMaterialRecord ) } };
	for ( uint32_t i = 0u; i < sizeof( aUploads ) / sizeof( aUploads[0] ); ++i ) {
		if ( aUploads[i].nBytes == 0u ) {
			continue;
		}
		if ( aUploads[i].nBytes > UINT32_MAX || aUploads[i].nBytes > pResources.get_plan().nBufferBytes[aUploads[i].eBuffer] ) {
			return false;
		}
		if ( !t3d_rw_upload_bytes_debug( *pContext,
		             pResources.get_buffer_rid( aUploads[i].eBuffer ),
		             0u,
		             aUploads[i].pData,
		             ( uint32_t )aUploads[i].nBytes,
		             rStats ) ) {
			return false;
		}
	}
	return true;
}

bool t3d_render_world_upload_reference_visibility_debug( const Terrain3DRenderWorldResourceImage &pResources,
        const T3DVisibilityOutputBuffers &pBuffers,
        uint32_t pInstanceCount,
        const T3DVisibilityReferenceResult &pResult,
        T3DThreeTierUploadStats &rStats ) {
	memset( &rStats, 0, sizeof( rStats ) );
	Terrain3DRdContext *const pContext = pResources.get_rd_context();
	if ( pContext == NULL || pContext->getDevice() == NULL || !pResources.is_initialized() ) {
		return false;
	}
	if ( pResult.Counters.nVisibleCellCount > pBuffers.nVisibleCellCapacity ) {
		return false;
	}
	if ( pResult.Counters.nVisibleInstanceCount > pBuffers.nVisibleInstanceCapacity ) {
		return false;
	}
	if ( pInstanceCount > pBuffers.nSelectedLodCapacity ) {
		return false;
	}

	if ( pResult.Counters.nVisibleCellCount != 0u
	        && !t3d_rw_upload_bytes_debug( *pContext,
	                pResources.get_buffer_rid( T3D_RW_GPU_BUFFER_VISIBLE_CELLS ),
	                0u,
	                pBuffers.pVisibleCells,
	                pResult.Counters.nVisibleCellCount * ( uint32_t )sizeof( uint32_t ),
	                rStats ) ) {
		return false;
	}

	if ( pResult.Counters.nVisibleInstanceCount != 0u
	        && !t3d_rw_upload_bytes_debug( *pContext,
	                pResources.get_buffer_rid( T3D_RW_GPU_BUFFER_VISIBLE_INSTANCES ),
	                0u,
	                pBuffers.pVisibleInstances,
	                pResult.Counters.nVisibleInstanceCount * ( uint32_t )sizeof( uint32_t ),
	                rStats ) ) {
		return false;
	}

	if ( pInstanceCount != 0u
	        && !t3d_rw_upload_bytes_debug( *pContext,
	                pResources.get_buffer_rid( T3D_RW_GPU_BUFFER_SELECTED_LOD ),
	                0u,
	                pBuffers.pSelectedLodByInstance,
	                pInstanceCount * ( uint32_t )sizeof( uint32_t ),
	                rStats ) ) {
		return false;
	}

	const T3DRenderWorldGpuBufferId aTierBuffers[T3D_RW_TIER_COUNT] = {
		T3D_RW_GPU_BUFFER_VISIBLE_WORLD_CLUSTERS, T3D_RW_GPU_BUFFER_VISIBLE_CLUTTER_CLUSTERS, T3D_RW_GPU_BUFFER_VISIBLE_FOLIAGE_CLUSTERS
	};
	for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
		const uint32_t nCount = pResult.Counters.nVisibleClusterCount[iTier];
		if ( nCount > pBuffers.nVisibleClusterCapacity[iTier] ) {
			return false;
		}
		if ( nCount != 0u
		        && !t3d_rw_upload_bytes_debug( *pContext,
		                pResources.get_buffer_rid( aTierBuffers[iTier] ),
		                0u,
		                pBuffers.pVisibleClusters[iTier],
		                nCount * ( uint32_t )sizeof( T3DVisibleCluster ),
		                rStats ) ) {
			return false;
		}
	}

	if ( !t3d_rw_upload_bytes_debug( *pContext,
	             pResources.get_buffer_rid( T3D_RW_GPU_BUFFER_INDIRECT_ARGS ),
	             0u,
	             pResult.DrawArgs,
	             ( uint32_t )sizeof( pResult.DrawArgs ),
	             rStats ) ) {
		return false;
	}

	if ( !t3d_rw_upload_bytes_debug( *pContext,
	             pResources.get_buffer_rid( T3D_RW_GPU_BUFFER_COUNTERS ),
	             0u,
	             &pResult.Counters,
	             ( uint32_t )sizeof( pResult.Counters ),
	             rStats ) ) {
		return false;
	}
	return true;
}

bool t3d_render_world_submit_three_tier_indirect_hot( int64_t pDrawList,
        const Terrain3DRenderWorldResourceImage &pResources,
        const T3DThreeTierRasterBindings &pBindings,
        T3DThreeTierUploadStats &rStats ) {
	Terrain3DRdContext *const pContext = pResources.get_rd_context();
	RenderingDevice *const pRenderingDevice = pContext ? pContext->getDevice() : NULL;
	if ( pRenderingDevice == NULL || pDrawList < 0 || !pResources.is_initialized() ) {
		return false;
	}
	const RID &hIndirect = pResources.get_buffer_rid( T3D_RW_GPU_BUFFER_INDIRECT_ARGS );
	if ( !hIndirect.is_valid() ) {
		return false;
	}

	for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
		if ( !pBindings.hPipeline[iTier].is_valid() ) {
			return false;
		}
		pRenderingDevice->draw_list_bind_render_pipeline( pDrawList, pBindings.hPipeline[iTier] );
		if ( pBindings.hUniformSet[iTier].is_valid() ) {
			pRenderingDevice->draw_list_bind_uniform_set( pDrawList, pBindings.hUniformSet[iTier], pBindings.nUniformSetIndex );
		}
		if ( !pBindings.hLightingSet.is_valid() ) {
			return false;
		}
		pRenderingDevice->draw_list_bind_uniform_set( pDrawList, pBindings.hLightingSet, pBindings.nLightingSetIndex );
		pRenderingDevice->draw_list_draw_indirect( pDrawList, false, hIndirect, iTier * ( uint32_t )sizeof( T3DDrawIndirectArgs ), 1u, 0u );
		++rStats.nDrawCount;
	}
	return true;
}
