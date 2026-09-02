// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_GPU_CACHE_CONSTANTS_H
#define TERRAIN3D_GPU_CACHE_CONSTANTS_H

#include "Terrain3DConstants.h"

#include <stdint.h>

typedef enum t3dGpuRegionSlotState_e : uint8_t {
	TERRAIN3D_GPU_SLOT_FREE = 0u,
	TERRAIN3D_GPU_SLOT_STAGING = 1u,
	TERRAIN3D_GPU_SLOT_READY = 2u,
	TERRAIN3D_GPU_SLOT_MAPPED = 3u,
	TERRAIN3D_GPU_SLOT_RETIRED = 4u,
} t3dGpuRegionSlotState_t;

#endif // TERRAIN3D_GPU_CACHE_CONSTANTS_H
