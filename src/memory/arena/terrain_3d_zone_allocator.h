// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_ZONE_ALLOCATOR_H
#define TERRAIN3D_ZONE_ALLOCATOR_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_sync.h"
#include "core/config/terrain_3d_compiler.h"
#include "memory/arena/terrain_3d_memory_zone.h"

#include <stdint.h>
// t3dAllocatorAllocationOrigin_e centralized in Terrain3DTypes.h.


// t3dAllocatorOverflowPolicy_e centralized in Terrain3DTypes.h.


// Allocator routing is explicit per Terrain3DEastlAllocator instance.
// This header retains only allocation-origin and overflow-policy enums.

#endif // TERRAIN3D_ZONE_ALLOCATOR_H
