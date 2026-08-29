// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_resource_plan.h"

#include <limits.h>
#include <string.h>

static bool t3d_rw_checked_add_u64( uint64_t pA, uint64_t pB, uint64_t &rValue ) {
	if ( pA > UINT64_MAX - pB ) {
		return false;
	}

	rValue = pA + pB;
	return true;
}

static bool t3d_rw_checked_mul_u64( uint64_t pA, uint64_t pB, uint64_t &rValue ) {
	if ( pA != 0u && pB > UINT64_MAX / pA ) {
		return false;
	}

	rValue = pA * pB;
	return true;
}

static bool t3d_rw_add_plan_bytes( uint64_t pBytes, uint64_t &rAccumulator ) {
	uint64_t nNext = 0u;
	if ( !t3d_rw_checked_add_u64( rAccumulator, pBytes, nNext ) ) {
		return false;
	}

	rAccumulator = nNext;
	return true;
}

static bool t3d_rw_compute_block_texture_bytes( uint32_t pWidth,
        uint32_t pHeight,
        uint32_t pLayers,
        uint32_t pBlockWidth,
        uint32_t pBlockHeight,
        uint32_t pBlockBytes,
        uint64_t &rBytes ) {
	if ( pWidth == 0u || pHeight == 0u || pLayers == 0u || pBlockWidth == 0u || pBlockHeight == 0u || pBlockBytes == 0u ) {
		return false;
	}

	const uint64_t nBlocksX = ( ( uint64_t )pWidth + pBlockWidth - 1u ) / pBlockWidth;
	const uint64_t nBlocksY = ( ( uint64_t )pHeight + pBlockHeight - 1u ) / pBlockHeight;
	uint64_t nBlocks = 0u;
	uint64_t nLayerBytes = 0u;
	if ( !t3d_rw_checked_mul_u64( nBlocksX, nBlocksY, nBlocks ) ) {
		return false;
	}
	if ( !t3d_rw_checked_mul_u64( nBlocks, pBlockBytes, nLayerBytes ) ) {
		return false;
	}
	return t3d_rw_checked_mul_u64( nLayerBytes, pLayers, rBytes );
}

static bool t3d_rw_compute_hiz_bytes( const T3DRenderWorldGpuProfile &pProfile, uint64_t &rBytes ) {
	uint32_t nWidth = pProfile.nHiZWidth;
	uint32_t nHeight = pProfile.nHiZHeight;
	uint64_t nTotal = 0u;

	for ( uint32_t iMip = 0u; iMip < pProfile.nHiZMipCount; ++iMip ) {
		uint64_t nTexels = 0u;
		uint64_t nMipBytes = 0u;
		if ( !t3d_rw_checked_mul_u64( nWidth, nHeight, nTexels ) ) {
			return false;
		}
		if ( !t3d_rw_checked_mul_u64( nTexels, 4u, nMipBytes ) ) {
			return false;
		}
		if ( !t3d_rw_add_plan_bytes( nMipBytes, nTotal ) ) {
			return false;
		}

		if ( nWidth > 1u ) {
			nWidth >>= 1u;
		}
		if ( nHeight > 1u ) {
			nHeight >>= 1u;
		}
	}

	rBytes = nTotal;
	return true;
}

T3DRenderWorldGpuProfile t3d_render_world_make_development_gpu_profile() {
	T3DRenderWorldGpuProfile Profile = {};

	// M06 finalizes the VT table PODs. Changing any stride is an explicit cold-rebuild ABI change.
	Profile.nMeshLodRecordBytes = sizeof( T3DMeshLodRecord );
	Profile.nVtAllocationRecordBytes = sizeof( T3DVTAllocationRecord );
	Profile.nVtPageTableEntryBytes = sizeof( T3DVTPageTableEntry );
	Profile.nVtRequestHashEntryBytes = 16u;
	Profile.nGpuCounterBytes = 256u;

	// Packet prototype VT page geometry: 128 texels + 4-texel gutter on both sides.
	Profile.nVtPhysicalTileSize = 136u;

	// Fixed prototype HZB maximum. Actual viewport use may address a prefix only.
	Profile.nHiZWidth = 2048u;
	Profile.nHiZHeight = 2048u;
	Profile.nHiZMipCount = 12u;
	return Profile;
}

bool t3d_render_world_validate_gpu_profile( const T3DRenderWorldGpuProfile &pProfile ) {
	if ( pProfile.nMeshLodRecordBytes == 0u ) {
		return false;
	}
	if ( pProfile.nVtAllocationRecordBytes == 0u ) {
		return false;
	}
	if ( pProfile.nVtPageTableEntryBytes == 0u ) {
		return false;
	}
	if ( pProfile.nVtRequestHashEntryBytes == 0u ) {
		return false;
	}
	if ( pProfile.nGpuCounterBytes < sizeof( T3DVisibilityCounters ) ) {
		return false;
	}
	if ( pProfile.nVtPhysicalTileSize == 0u ) {
		return false;
	}
	if ( pProfile.nHiZWidth == 0u || pProfile.nHiZHeight == 0u ) {
		return false;
	}
	if ( pProfile.nHiZMipCount == 0u ) {
		return false;
	}

	uint32_t nWidth = pProfile.nHiZWidth;
	uint32_t nHeight = pProfile.nHiZHeight;
	uint32_t nMaximumMipCount = 1u;
	while ( nWidth > 1u || nHeight > 1u ) {
		if ( nWidth > 1u ) {
			nWidth >>= 1u;
		}
		if ( nHeight > 1u ) {
			nHeight >>= 1u;
		}
		++nMaximumMipCount;
	}

	return pProfile.nHiZMipCount <= nMaximumMipCount;
}

bool t3d_render_world_compute_vt_bank_layout( uint32_t pPageCount, uint32_t pMaxArrayLayers, T3DRenderWorldVTBankLayout &rLayout ) {
	memset( &rLayout, 0, sizeof( rLayout ) );
	if ( pPageCount == 0u || pMaxArrayLayers == 0u ) {
		return false;
	}

	uint32_t nRemaining = pPageCount;
	while ( nRemaining != 0u ) {
		if ( rLayout.nBankCount >= T3D_RW_MAX_VT_TEXTURE_BANKS ) {
			return false;
		}
		const uint32_t nLayers = nRemaining < pMaxArrayLayers ? nRemaining : pMaxArrayLayers;
		rLayout.nLayers[rLayout.nBankCount] = nLayers;
		++rLayout.nBankCount;
		nRemaining -= nLayers;
	}

	return true;
}

bool t3d_render_world_compute_gpu_resource_plan(
        const T3DRenderWorldLimits &pLimits, const T3DRenderWorldGpuProfile &pProfile, T3DRenderWorldGpuResourcePlan &rPlan ) {
	memset( &rPlan, 0, sizeof( rPlan ) );
	if ( !t3d_render_world_validate_limits( pLimits ) ) {
		return false;
	}
	if ( !t3d_render_world_validate_gpu_profile( pProfile ) ) {
		return false;
	}

	uint64_t nBytes = 0u;
	uint64_t nCount = 0u;

	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_GEOMETRY_VERTEX] = pLimits.nGeometryVertexHeapBytes;
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_CLUSTER_VERTEX_INDEX] = pLimits.nGeometryClusterVertexIndexHeapBytes;
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_CLUSTER_TRIANGLE] = pLimits.nGeometryTriangleHeapBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxMeshes, sizeof( T3DMeshRecord ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_MESH_RECORDS] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxMeshLods, pProfile.nMeshLodRecordBytes, nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_MESH_LOD_RECORDS] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxClusters, sizeof( T3DClusterRecord ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_CLUSTER_RECORDS] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxInstances, sizeof( T3DInstanceRecord ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_INSTANCE_RECORDS] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxCells, sizeof( T3DSceneCell ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_CELL_RECORDS] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxMaterials, sizeof( T3DMaterialRecord ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_MATERIAL_RECORDS] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxMaterials, 4u, nCount ) ) {
		return false;
	}
	if ( !t3d_rw_checked_mul_u64( nCount, pProfile.nVtAllocationRecordBytes, nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxVtVirtualPages, pProfile.nVtPageTableEntryBytes, nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VT_PAGE_TABLE] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxVisibleCells, sizeof( uint32_t ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VISIBLE_CELLS] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxVisibleInstances, sizeof( uint32_t ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VISIBLE_INSTANCES] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxVisibleClusters[T3D_RW_TIER_WORLD], sizeof( T3DVisibleCluster ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VISIBLE_WORLD_CLUSTERS] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxVisibleClusters[T3D_RW_TIER_CLUTTER], sizeof( T3DVisibleCluster ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VISIBLE_CLUTTER_CLUSTERS] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxVisibleClusters[T3D_RW_TIER_FOLIAGE], sizeof( T3DVisibleCluster ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VISIBLE_FOLIAGE_CLUSTERS] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxInstances, sizeof( uint32_t ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_SELECTED_LOD] = nBytes;

	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_COUNTERS] = pProfile.nGpuCounterBytes;
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_INDIRECT_ARGS] = ( uint64_t )T3D_RW_TIER_COUNT * sizeof( T3DDrawIndirectArgs );

	// Two hash slots per compact request keeps the fixed table at <= 50% occupancy.
	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxVtRequests, 2u, nCount ) ) {
		return false;
	}
	if ( !t3d_rw_checked_mul_u64( nCount, pProfile.nVtRequestHashEntryBytes, nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VT_REQUEST_HASH] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxVtRequests, sizeof( T3DVTRequest ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VT_REQUEST_LIST] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxUploadBytesPerFrame, T3D_RW_GPU_FRAME_RING_COUNT, nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_UPLOAD_RING] = nBytes;

	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxReadbackBytesPerFrame, T3D_RW_GPU_FRAME_RING_COUNT, nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_READBACK_RING] = nBytes;

	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VISIBILITY_FRAME] = sizeof( T3DVisibilityFrame );
	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxClusters, sizeof( uint32_t ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VISIBILITY_HISTORY] = nBytes;

	for ( uint32_t iBuffer = 0u; iBuffer < T3D_RW_GPU_BUFFER_COUNT; ++iBuffer ) {
		if ( rPlan.nBufferBytes[iBuffer] == 0u ) {
			return false;
		}
		if ( !t3d_rw_add_plan_bytes( rPlan.nBufferBytes[iBuffer], rPlan.nTotalBufferBytes ) ) {
			return false;
		}
	}

	const uint32_t nTile = pProfile.nVtPhysicalTileSize;
	if ( !t3d_rw_compute_block_texture_bytes(
	             nTile, nTile, pLimits.nMaxVtPhysicalAlbedoPages, 4u, 4u, 8u, rPlan.nVtPhysicalBytes[T3D_RW_VT_ALBEDO] ) ) {
		return false;
	}
	if ( !t3d_rw_compute_block_texture_bytes(
	             nTile, nTile, pLimits.nMaxVtPhysicalNormalPages, 4u, 4u, 16u, rPlan.nVtPhysicalBytes[T3D_RW_VT_NORMAL] ) ) {
		return false;
	}
	if ( !t3d_rw_compute_block_texture_bytes(
	             nTile, nTile, pLimits.nMaxVtPhysicalSurfacePages, 4u, 4u, 16u, rPlan.nVtPhysicalBytes[T3D_RW_VT_SURFACE] ) ) {
		return false;
	}
	if ( !t3d_rw_compute_block_texture_bytes(
	             nTile, nTile, pLimits.nMaxVtPhysicalHeightPages, 1u, 1u, 1u, rPlan.nVtPhysicalBytes[T3D_RW_VT_HEIGHT] ) ) {
		return false;
	}

	for ( uint32_t iChannel = 0u; iChannel < T3D_RW_VT_CHANNEL_COUNT; ++iChannel ) {
		if ( !t3d_rw_add_plan_bytes( rPlan.nVtPhysicalBytes[iChannel], rPlan.nTotalTextureBytes ) ) {
			return false;
		}
	}

	if ( !t3d_rw_compute_hiz_bytes( pProfile, rPlan.nHiZBytes ) ) {
		return false;
	}
	if ( !t3d_rw_add_plan_bytes( rPlan.nHiZBytes, rPlan.nTotalTextureBytes ) ) {
		return false;
	}
	if ( !t3d_rw_checked_add_u64( rPlan.nTotalBufferBytes, rPlan.nTotalTextureBytes, rPlan.nTotalBytes ) ) {
		return false;
	}
	return true;
}
