#include "rendering/rd/render_world/terrain_3d_render_world_contract.h"
#include "rendering/rd/render_world/terrain_3d_render_world_resource_plan.h"

static bool t3d_rw_nonzero_u32( uint32_t pValue ) {
	return pValue != 0u;
}

T3DRenderWorldLimits t3d_render_world_make_prototype_limits() {
	T3DRenderWorldLimits limits = {};
	limits.nMaxCells = 1024u;
	limits.nMaxInstances = 32768u;
	limits.nMaxMeshes = 4096u;
	limits.nMaxMeshLods = 16384u;
	limits.nMaxClusters = 131072u;
	limits.nMaxMaterials = 4096u;
	limits.nMaxVisibleCells = 1024u;
	limits.nMaxVisibleInstances = 32768u;
	limits.nMaxVisibleClusters[T3D_RW_TIER_WORLD] = 65536u;
	limits.nMaxVisibleClusters[T3D_RW_TIER_CLUTTER] = 65536u;
	limits.nMaxVisibleClusters[T3D_RW_TIER_FOLIAGE] = 65536u;
	limits.nMaxVtVirtualPages = 4096u;
	limits.nMaxVtPhysicalAlbedoPages = 32u;
	limits.nMaxVtPhysicalNormalPages = 32u;
	limits.nMaxVtPhysicalSurfacePages = 32u;
	limits.nMaxVtPhysicalHeightPages = 32u;
	limits.nMaxVtRequests = 2048u;
	limits.nMaxStreamJobs = 128u;
	limits.nStreamScratchLaneCount = 4u;
	limits.nMaxStreamPayloadBytes = 256u * 1024u;
	limits.nMaxUploadBytesPerFrame = 4u * 1024u * 1024u;
	limits.nMaxReadbackBytesPerFrame = 64u * 1024u;
	limits.nGeometryVertexHeapBytes = 64u * 1024u * 1024u;
	limits.nGeometryClusterVertexIndexHeapBytes = 16u * 1024u * 1024u;
	limits.nGeometryTriangleHeapBytes = 16u * 1024u * 1024u;
	limits.nSnapshotBytesPerFrame = 2u * 1024u * 1024u;
	limits.nFrameScratchBytesPerFrame = 2u * 1024u * 1024u;
	limits.nStreamScratchBytesPerLane = 256u * 1024u;
	limits.nOperationEventCapacity = 4096u;
	return limits;
}

uint64_t t3d_render_world_limits_signature( const T3DRenderWorldLimits &pLimits ) {
	const uint8_t *pBytes = ( const uint8_t * )&pLimits;
	uint64_t nHash = 1469598103934665603ull;
	for ( uint32_t iByte = 0u; iByte < sizeof( T3DRenderWorldLimits ); ++iByte ) {
		nHash ^= pBytes[iByte];
		nHash *= 1099511628211ull;
	}
	return nHash;
}

bool t3d_render_world_validate_limits( const T3DRenderWorldLimits &pLimits ) {
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxCells ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxInstances ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxMeshes ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxMeshLods ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxClusters ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxMaterials ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVisibleCells ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVisibleInstances ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVisibleClusters[T3D_RW_TIER_WORLD] ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVisibleClusters[T3D_RW_TIER_CLUTTER] ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVisibleClusters[T3D_RW_TIER_FOLIAGE] ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVtVirtualPages ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVtPhysicalAlbedoPages ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVtPhysicalNormalPages ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVtPhysicalSurfacePages ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVtPhysicalHeightPages ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVtRequests ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxStreamJobs ) || pLimits.nMaxStreamJobs > 0xffffu ) {
		return false;
	}
	if ( pLimits.nMaxVtPhysicalAlbedoPages > 0xffffu || pLimits.nMaxVtPhysicalNormalPages > 0xffffu
	        || pLimits.nMaxVtPhysicalSurfacePages > 0xffffu || pLimits.nMaxVtPhysicalHeightPages > 0xffffu ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nStreamScratchLaneCount ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nOperationEventCapacity ) ) {
		return false;
	}
	if ( pLimits.nMaxStreamPayloadBytes == 0u ) {
		return false;
	}
	if ( pLimits.nMaxUploadBytesPerFrame == 0u ) {
		return false;
	}
	if ( pLimits.nMaxReadbackBytesPerFrame == 0u ) {
		return false;
	}
	if ( pLimits.nGeometryVertexHeapBytes == 0u ) {
		return false;
	}
	if ( pLimits.nGeometryClusterVertexIndexHeapBytes == 0u ) {
		return false;
	}
	if ( pLimits.nGeometryTriangleHeapBytes == 0u ) {
		return false;
	}
	if ( pLimits.nSnapshotBytesPerFrame == 0u ) {
		return false;
	}
	if ( pLimits.nFrameScratchBytesPerFrame == 0u ) {
		return false;
	}
	if ( pLimits.nStreamScratchBytesPerLane == 0u ) {
		return false;
	}
	return true;
}

T3DRenderWorldMemoryPlan t3d_render_world_compute_minimum_memory_plan( const T3DRenderWorldLimits &pLimits ) {
	T3DRenderWorldMemoryPlan Plan = {};

	Plan.nCpuPermanentBytes = ( uint64_t )pLimits.nMaxCells * sizeof( T3DSceneCell )
	        + ( uint64_t )pLimits.nMaxMeshes * sizeof( T3DMeshRecord ) + ( uint64_t )pLimits.nMaxMeshLods * sizeof( T3DMeshLodRecord )
	        + ( uint64_t )pLimits.nMaxClusters * sizeof( T3DClusterRecord )
	        + ( uint64_t )pLimits.nMaxMaterials * sizeof( T3DMaterialRecord );

	Plan.nCpuResidentBytes = ( uint64_t )pLimits.nMaxInstances * sizeof( T3DInstanceRecord );

	Plan.nCpuSnapshotBytes = pLimits.nSnapshotBytesPerFrame * 3u;
	Plan.nCpuFrameScratchBytes = pLimits.nFrameScratchBytesPerFrame * 3u;
	Plan.nCpuStreamingScratchBytes = pLimits.nStreamScratchBytesPerLane * pLimits.nStreamScratchLaneCount;

	Plan.nGpuGeometryBytes =
	        pLimits.nGeometryVertexHeapBytes + pLimits.nGeometryClusterVertexIndexHeapBytes + pLimits.nGeometryTriangleHeapBytes;

	Plan.nGpuSceneRecordBytes = ( uint64_t )pLimits.nMaxCells * sizeof( T3DSceneCell )
	        + ( uint64_t )pLimits.nMaxInstances * sizeof( T3DInstanceRecord ) + ( uint64_t )pLimits.nMaxMeshes * sizeof( T3DMeshRecord )
	        + ( uint64_t )pLimits.nMaxMeshLods * sizeof( T3DMeshLodRecord ) + ( uint64_t )pLimits.nMaxClusters * sizeof( T3DClusterRecord )
	        + ( uint64_t )pLimits.nMaxMaterials * sizeof( T3DMaterialRecord );

	Plan.nGpuVisibilityBytes = ( uint64_t )pLimits.nMaxVisibleCells * sizeof( uint32_t )
	        + ( uint64_t )pLimits.nMaxVisibleInstances * sizeof( uint32_t )
	        + ( uint64_t )( pLimits.nMaxVisibleClusters[0] + pLimits.nMaxVisibleClusters[1] + pLimits.nMaxVisibleClusters[2] )
	                * sizeof( T3DVisibleCluster )
	        + T3D_RW_INDIRECT_BUFFER_BYTES;

	// VT physical bytes and Hi-Z bytes depend on selected texture formats/resolution.
	// They must be added by the profile/device-specific planner before initialization.
	Plan.nGpuVTPhysicalBytes = 0u;
	Plan.nGpuHiZBytes = 0u;
	Plan.nGpuMiscBytes = 0u;

	return Plan;
}
