// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_PLATEAU_H
#define TERRAIN3D_RENDER_WORLD_PLATEAU_H

#include "rendering/rd/render_world/terrain_3d_render_world_contract.h"

#include <stdint.h>

struct T3DRenderWorldPlateauSnapshot {
	uint64_t nLimitsSignature;
	uint64_t nGpuCreateCount;
	uint64_t nGpuFreeCount;
	uint64_t nGpuBufferBytes;
	uint64_t nGpuTextureBytes;
	uint64_t nVTPhysicalBytes;
	uint64_t nVisibilityCapacity;
	uint64_t nCpuAllocationCount;
	uint64_t nSyncWaitCount;
	uint64_t nSyncReadbackCount;
	uint32_t nLiveResourceCount;
	uint32_t nHotViolationCount;
};

bool t3d_render_world_plateau_snapshot_equal_reference_debug(
        const T3DRenderWorldPlateauSnapshot &pBaseline, const T3DRenderWorldPlateauSnapshot &pCurrent );

static_assert( sizeof( T3DRenderWorldPlateauSnapshot ) == 88, "T3DRenderWorldPlateauSnapshot ABI mismatch" );

#endif // TERRAIN3D_RENDER_WORLD_PLATEAU_H
