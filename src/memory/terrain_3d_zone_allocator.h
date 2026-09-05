// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_ZONE_ALLOCATOR_H
#define TERRAIN3D_ZONE_ALLOCATOR_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "core/terrain_3d_compiler.h"
#include "memory/terrain_3d_memory_zone.h"

#include <stdint.h>
typedef enum t3dAllocatorAllocationOrigin_e : uint8_t {
	TERRAIN3D_ALLOCATION_BOOTSTRAP = 0,
	TERRAIN3D_ALLOCATION_ZONE,
	TERRAIN3D_ALLOCATION_OVERFLOW,
} t3dAllocatorAllocationOrigin_t;

typedef enum t3dAllocatorOverflowPolicy_e : uint8_t {
	TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT = 0,
	TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP,
} t3dAllocatorOverflowPolicy_t;

// Allocator routing is explicit per Terrain3DEastlAllocator instance.
// This header retains only allocation-origin and overflow-policy enums.

#endif // TERRAIN3D_ZONE_ALLOCATOR_H
