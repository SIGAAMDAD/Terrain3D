// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_SYSTEM_LIMITS_H
#define TERRAIN3D_WORLD_SYSTEM_LIMITS_H

#include "rendering/rd/world/terrain_3d_world_contract.h"

#include <stdint.h>

struct T3DWorldLimits {
	uint32_t nMaxCells;
	uint32_t nMaxInstances;
	uint32_t nMaxMeshes;
	uint32_t nMaxMeshLods;
	uint32_t nMaxClusters;
	uint32_t nMaxMaterials;
	uint32_t nMaxVisibleCells;
	uint32_t nMaxVisibleInstances;
	uint32_t nMaxVisibleClusters[T3D_WORLD_TIER_COUNT];
	uint32_t nMaxVtVirtualPages;
	uint32_t nMaxVtPhysicalPageSets;
	uint32_t nMaxVtRequests;
	uint32_t nMaxStreamJobs;
	uint32_t nStreamScratchLaneCount;
	uint64_t nMaxStreamPayloadBytes;
	uint64_t nMaxUploadBytesPerFrame;
	uint64_t nMaxReadbackBytesPerFrame;
	uint64_t nGeometryVertexHeapBytes;
	uint64_t nGeometryClusterVertexIndexHeapBytes;
	uint64_t nGeometryTriangleHeapBytes;
	uint64_t nSnapshotBytesPerFrame;
	uint64_t nFrameScratchBytesPerFrame;
	uint64_t nStreamScratchBytesPerLane;
	uint32_t nOperationEventCapacity;
	uint32_t nReserved;
};

T3DWorldLimits t3d_world_make_prototype_limits();
T3DWorldLimits t3d_world_make_runtime_limits( const t3dWorldResourceLimits_t &rWorldLimits );
T3DWorldLimits t3d_world_make_preview_limits( uint32_t nMaterialCapacity, uint32_t nVirtualPageCapacity );
bool t3d_world_validate_limits( const T3DWorldLimits &pLimits );
uint64_t t3d_world_limits_signature( const T3DWorldLimits &pLimits );

#endif // TERRAIN3D_WORLD_SYSTEM_LIMITS_H
