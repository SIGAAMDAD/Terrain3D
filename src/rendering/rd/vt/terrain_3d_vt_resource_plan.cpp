// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/vt/terrain_3d_vt_resource_plan.h"
#include "rendering/rd/vt/terrain_3d_vt_contract.h"

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

T3DVtGpuProfile t3d_vt_make_development_gpu_profile() {
	T3DVtGpuProfile profile = {};
	profile.nVtAllocationRecordBytes = sizeof( t3dVTAllocation_t );
	profile.nVtPageTableEntryBytes = sizeof( t3dVTPageEntry_t );
	profile.nVtPhysicalTileSize = 136u;
	return profile;
}

T3DVtGpuProfile t3d_vt_make_runtime_gpu_profile( const t3dWorldResourceLimits_t & ) {
	return t3d_vt_make_development_gpu_profile();
}

T3DVtGpuProfile t3d_vt_make_preview_gpu_profile() {
	return t3d_vt_make_development_gpu_profile();
}

bool t3d_vt_validate_gpu_profile( const T3DVtGpuProfile &pProfile ) {
	return pProfile.nVtAllocationRecordBytes == sizeof( t3dVTAllocation_t )
	        && pProfile.nVtPageTableEntryBytes == sizeof( t3dVTPageEntry_t )
	        && pProfile.nVtPhysicalTileSize != 0u;
}

bool t3d_vt_compute_bank_layout( uint32_t pPageCount, uint32_t pMaxArrayLayers, T3DVtBankLayout &rLayout ) {
	memset( &rLayout, 0, sizeof( rLayout ) );
	if ( pPageCount == 0u || pMaxArrayLayers == 0u || pPageCount > pMaxArrayLayers ) return false;
	rLayout.nBankCount = 1u;
	rLayout.nLayers[0] = pPageCount;
	return true;
}

bool t3d_vt_compute_resource_plan(
        const T3DRenderWorldLimits &pLimits, const T3DVtGpuProfile &pProfile, T3DVtGpuResourcePlan &rPlan ) {
	memset( &rPlan, 0, sizeof( rPlan ) );
	if ( !t3d_vt_validate_gpu_profile( pProfile ) || pLimits.nMaxMaterials == 0u
	        || pLimits.nMaxVtVirtualPages == 0u || pLimits.nMaxVtPhysicalPageSets == 0u ) return false;
	uint64_t nBytes = 0u;
	uint64_t nCount = 0u;
	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxMaterials, 4u, nCount )
	        || !t3d_rw_checked_add_u64( nCount, T3D_TERRAIN_MEGAVT_ALLOCATION_CAPACITY, nCount )
	        || !t3d_rw_checked_mul_u64( nCount, pProfile.nVtAllocationRecordBytes, nBytes ) ) return false;
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE] = nBytes;
	uint64_t nMipLayoutEntries = 0u;
	if ( !t3d_rw_checked_mul_u64( nCount, T3D_VT_MAX_MIPS, nMipLayoutEntries )
	        || !t3d_rw_checked_mul_u64( nMipLayoutEntries, sizeof( t3dVTMipLayout_t ), nBytes ) ) return false;
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VT_MIP_LAYOUT_TABLE] = nBytes;
	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxVtVirtualPages, pProfile.nVtPageTableEntryBytes, nBytes ) ) return false;
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VT_PAGE_TABLE] = nBytes;

	uint64_t nTailEntries = 0u;
	if ( !t3d_rw_checked_mul_u64( pLimits.nMaxMaterials, 4u, nTailEntries ) || nTailEntries == 0u || nTailEntries > UINT32_MAX ) {
		return false;
	}
	if ( !t3d_rw_checked_mul_u64( nTailEntries, sizeof( t3dVTTailMetadata_t ), nBytes ) ) {
		return false;
	}
	rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_VT_TAIL_METADATA] = nBytes;
	rPlan.nVtTailEntryCapacity = ( uint32_t )nTailEntries;
	rPlan.nVtTailAtlasWidth = T3D_VT_TAIL_ATLAS_WIDTH;
	const uint64_t nLegacyTailRows64 = ( nTailEntries + T3D_VT_TAIL_TILES_PER_ROW - 1u ) / T3D_VT_TAIL_TILES_PER_ROW;
	const uint64_t nPackTailRows64 = ( ( uint64_t )pLimits.nMaxMaterials + T3D_VT_PACK_TAILS_PER_ROW - 1u ) / T3D_VT_PACK_TAILS_PER_ROW;
	if ( T3D_VT_PACK_TAILS_PER_ROW == 0u || nLegacyTailRows64 == 0u || nPackTailRows64 == 0u
	        || nLegacyTailRows64 > UINT32_MAX / T3D_VT_TAIL_TILE || nPackTailRows64 > UINT32_MAX / T3D_VT_PACK_TAIL_SLOT_HEIGHT ) {
		return false;
	}
	const uint32_t nLegacyTailHeight = ( uint32_t )nLegacyTailRows64 * T3D_VT_TAIL_TILE;
	const uint32_t nPackTailHeight = ( uint32_t )nPackTailRows64 * T3D_VT_PACK_TAIL_SLOT_HEIGHT;
	rPlan.nVtTailAtlasHeight = nLegacyTailHeight > nPackTailHeight ? nLegacyTailHeight : nPackTailHeight;

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
	             nTile, nTile, pLimits.nMaxVtPhysicalPageSets, 4u, 4u, 8u, rPlan.nVtPhysicalBytes[T3D_RW_VT_ALBEDO] ) ) {
		return false;
	}
	if ( !t3d_rw_compute_block_texture_bytes(
	             nTile, nTile, pLimits.nMaxVtPhysicalPageSets, 4u, 4u, 16u, rPlan.nVtPhysicalBytes[T3D_RW_VT_NORMAL] ) ) {
		return false;
	}
	if ( !t3d_rw_compute_block_texture_bytes(
	             nTile, nTile, pLimits.nMaxVtPhysicalPageSets, 4u, 4u, 16u, rPlan.nVtPhysicalBytes[T3D_RW_VT_SURFACE] ) ) {
		return false;
	}
	if ( !t3d_rw_compute_block_texture_bytes(
	             nTile, nTile, pLimits.nMaxVtPhysicalPageSets, 1u, 1u, 1u, rPlan.nVtPhysicalBytes[T3D_RW_VT_HEIGHT] ) ) {
		return false;
	}

	const uint32_t nTailWidth = rPlan.nVtTailAtlasWidth;
	const uint32_t nTailHeight = rPlan.nVtTailAtlasHeight;
	if ( !t3d_rw_compute_block_texture_bytes( nTailWidth, nTailHeight, 1u, 4u, 4u, 8u, rPlan.nVtTailBytes[T3D_RW_VT_ALBEDO] ) ) {
		return false;
	}
	if ( !t3d_rw_compute_block_texture_bytes( nTailWidth, nTailHeight, 1u, 4u, 4u, 16u, rPlan.nVtTailBytes[T3D_RW_VT_NORMAL] ) ) {
		return false;
	}
	if ( !t3d_rw_compute_block_texture_bytes( nTailWidth, nTailHeight, 1u, 4u, 4u, 16u, rPlan.nVtTailBytes[T3D_RW_VT_SURFACE] ) ) {
		return false;
	}
	if ( !t3d_rw_compute_block_texture_bytes( nTailWidth, nTailHeight, 1u, 1u, 1u, 1u, rPlan.nVtTailBytes[T3D_RW_VT_HEIGHT] ) ) {
		return false;
	}

	for ( uint32_t iChannel = 0u; iChannel < T3D_RW_VT_CHANNEL_COUNT; ++iChannel ) {
		if ( !t3d_rw_add_plan_bytes( rPlan.nVtPhysicalBytes[iChannel], rPlan.nTotalTextureBytes )
		        || !t3d_rw_add_plan_bytes( rPlan.nVtTailBytes[iChannel], rPlan.nTotalTextureBytes ) ) {
			return false;
		}
	}

	return t3d_rw_checked_add_u64( rPlan.nTotalBufferBytes, rPlan.nTotalTextureBytes, rPlan.nTotalBytes );
}
