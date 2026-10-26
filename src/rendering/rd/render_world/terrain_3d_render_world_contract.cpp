#include "rendering/rd/render_world/terrain_3d_render_world_contract.h"

static const uint32_t T3D_RW_INDIRECT_BUFFER_BYTES = 108u;

static bool t3d_rw_nonzero_u32( uint32_t pValue ) {
	return pValue != 0u;
}

static uint32_t t3d_rw_max_u32( const uint32_t nA, const uint32_t nB ) {
	return nA > nB ? nA : nB;
}

static uint64_t t3d_rw_max_u64( const uint64_t nA, const uint64_t nB ) {
	return nA > nB ? nA : nB;
}

static uint32_t t3d_rw_min_u32( const uint32_t nA, const uint32_t nB ) {
	return nA < nB ? nA : nB;
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
	limits.nMaxVtVirtualPages = 32768u;
	limits.nMaxVtPhysicalPageSets = 256u;
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

T3DRenderWorldLimits t3d_render_world_make_runtime_limits( const t3dWorldResourceLimits_t &rWorldLimits ) {
	T3DRenderWorldLimits limits = {};
	limits.nMaxCells = t3d_rw_max_u32( 1u, rWorldLimits.m_nMaxVisibleItems / 64u );
	limits.nMaxInstances = t3d_rw_max_u32( 1u, rWorldLimits.m_nMaxTransforms );
	limits.nMaxMeshes = t3d_rw_max_u32( 1u, rWorldLimits.m_nMaxClusters / 8u );
	limits.nMaxMeshLods = t3d_rw_max_u32( 1u, rWorldLimits.m_nMaxClusters / 4u );
	limits.nMaxClusters = t3d_rw_max_u32( 1u, rWorldLimits.m_nMaxClusters );
	limits.nMaxMaterials = t3d_rw_max_u32( 1u, rWorldLimits.m_nMaxMaterials );
	limits.nMaxVisibleCells = limits.nMaxCells;
	limits.nMaxVisibleInstances = t3d_rw_max_u32( 1u, rWorldLimits.m_nMaxVisibleItems );
	for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
		limits.nMaxVisibleClusters[iTier] = t3d_rw_max_u32( 1u, rWorldLimits.m_nMaxVisibleItems );
	}
	limits.nMaxVtRequests = t3d_rw_max_u32( 1u, rWorldLimits.m_nVtPageRequestBudget );
	limits.nMaxStreamJobs = t3d_rw_max_u32( 1u, rWorldLimits.m_nVtPageAdmissionBudget );
	limits.nMaxVtVirtualPages = t3d_rw_max_u32( 4096u, limits.nMaxVtRequests * 16u );
	limits.nMaxVtPhysicalPageSets = t3d_rw_max_u32( 64u, limits.nMaxStreamJobs * 4u );
	limits.nStreamScratchLaneCount = t3d_rw_min_u32( 4u, limits.nMaxStreamJobs );
	limits.nMaxStreamPayloadBytes = 256u * 1024u;
	limits.nMaxUploadBytesPerFrame = t3d_rw_max_u64( 64ull * 1024ull, rWorldLimits.m_nWorldUploadBytesPerFrame );
	limits.nMaxReadbackBytesPerFrame = 64u * 1024u;
	limits.nGeometryVertexHeapBytes = t3d_rw_max_u64( 4ull * 1024ull, rWorldLimits.m_nGeometryHeapBytes );
	limits.nGeometryClusterVertexIndexHeapBytes =
	        t3d_rw_max_u64( 4ull * 1024ull, ( uint64_t )rWorldLimits.m_nMaxClusterVertexIndices * sizeof( uint32_t ) );
	limits.nGeometryTriangleHeapBytes = t3d_rw_max_u64( 4ull * 1024ull, rWorldLimits.m_nTriangleHeapBytes );
	limits.nSnapshotBytesPerFrame = t3d_rw_max_u64( 64ull * 1024ull, rWorldLimits.m_nPublicationScratchBytes );
	limits.nFrameScratchBytesPerFrame = t3d_rw_max_u64( 64ull * 1024ull, rWorldLimits.m_nVisibilityBytes );
	limits.nStreamScratchBytesPerLane = 256u * 1024u;
	limits.nOperationEventCapacity = 4096u;
	return limits;
}

T3DRenderWorldLimits t3d_render_world_make_preview_limits( const uint32_t nMaterialCapacity, const uint32_t nVirtualPageCapacity ) {
	T3DRenderWorldLimits limits = {};
	limits.nMaxCells = 1u;
	limits.nMaxInstances = 1u;
	limits.nMaxMeshes = 1u;
	limits.nMaxMeshLods = 1u;
	limits.nMaxClusters = 1u;
	limits.nMaxMaterials = nMaterialCapacity > 0u ? nMaterialCapacity : 1u;
	limits.nMaxVisibleCells = 1u;
	limits.nMaxVisibleInstances = 1u;
	limits.nMaxVisibleClusters[T3D_RW_TIER_WORLD] = 1u;
	limits.nMaxVisibleClusters[T3D_RW_TIER_CLUTTER] = 1u;
	limits.nMaxVisibleClusters[T3D_RW_TIER_FOLIAGE] = 1u;
	limits.nMaxVtVirtualPages = nVirtualPageCapacity > 0u ? nVirtualPageCapacity : 1u;
	limits.nMaxVtPhysicalPageSets = 4u;
	limits.nMaxVtRequests = 32u;
	limits.nMaxStreamJobs = 8u;
	limits.nStreamScratchLaneCount = 1u;
	limits.nMaxStreamPayloadBytes = 256u * 1024u;
	limits.nMaxUploadBytesPerFrame = 512u * 1024u;
	limits.nMaxReadbackBytesPerFrame = 4u * 1024u;
	limits.nGeometryVertexHeapBytes = 4u * 1024u;
	limits.nGeometryClusterVertexIndexHeapBytes = 4u * 1024u;
	limits.nGeometryTriangleHeapBytes = 4u * 1024u;
	limits.nSnapshotBytesPerFrame = 4u * 1024u;
	limits.nFrameScratchBytesPerFrame = 4u * 1024u;
	limits.nStreamScratchBytesPerLane = 256u * 1024u;
	limits.nOperationEventCapacity = 64u;
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
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVtPhysicalPageSets ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxVtRequests ) ) {
		return false;
	}
	if ( !t3d_rw_nonzero_u32( pLimits.nMaxStreamJobs ) || pLimits.nMaxStreamJobs > 0xffffu ) {
		return false;
	}
	if ( pLimits.nMaxVtPhysicalPageSets > 0xffffu ) {
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
	        + ( uint64_t )pLimits.nMaxMaterials * sizeof( t3dRenderingMaterialGpu_t );

	Plan.nCpuResidentBytes = ( uint64_t )pLimits.nMaxInstances * sizeof( T3DInstanceRecord );

	Plan.nCpuSnapshotBytes = pLimits.nSnapshotBytesPerFrame * 3u;
	Plan.nCpuFrameScratchBytes = pLimits.nFrameScratchBytesPerFrame * 3u;
	Plan.nCpuStreamingScratchBytes = pLimits.nStreamScratchBytesPerLane * pLimits.nStreamScratchLaneCount;

	Plan.nGpuGeometryBytes =
	        pLimits.nGeometryVertexHeapBytes + pLimits.nGeometryClusterVertexIndexHeapBytes + pLimits.nGeometryTriangleHeapBytes;

	Plan.nGpuSceneRecordBytes = ( uint64_t )pLimits.nMaxCells * sizeof( T3DSceneCell )
	        + ( uint64_t )pLimits.nMaxInstances * sizeof( T3DInstanceRecord ) + ( uint64_t )pLimits.nMaxMeshes * sizeof( T3DMeshRecord )
	        + ( uint64_t )pLimits.nMaxMeshLods * sizeof( T3DMeshLodRecord ) + ( uint64_t )pLimits.nMaxClusters * sizeof( T3DClusterRecord )
	        + ( uint64_t )pLimits.nMaxMaterials * sizeof( t3dRenderingMaterialGpu_t );

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
