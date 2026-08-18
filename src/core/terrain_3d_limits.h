// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_LIMITS_H
#define TERRAIN3D_LIMITS_H

#include "Terrain3DConstants.h"

#include <cstddef>
#include <cstdint>

// Terrain world resolution and region partitioning are independent. The fixed
// logical world contains 4096x4096 texels; region size only changes how many
// region records divide that world.
static_assert( TERRAIN3D_MAX_REGION_MAP_SIZE == 64,
	"The fixed logical terrain must retain a 64x64 maximum region grid" );
static_assert( TERRAIN3D_MAX_REGION_SLOTS == 4096,
	"The fixed logical terrain must retain 4096 region slots" );
static_assert( TERRAIN3D_MAX_REGION_SLOTS <= UINT16_MAX,
	"Region slot indices must fit in uint16_t" );

#endif // TERRAIN3D_LIMITS_H
