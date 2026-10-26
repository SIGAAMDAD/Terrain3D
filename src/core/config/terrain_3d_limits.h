// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_LIMITS_H
#define TERRAIN3D_LIMITS_H

#include "Terrain3DConstants.h"

#include <stddef.h>
#include <stdint.h>

// Canonical runtime terrain specialization. The public Terrain3D contract is
// 128 samples at 1 m spacing, giving a 32x32 logical grid over 4096 texels.
static_assert( T3D_REGION_SIZE == 128, "Terrain3D runtime regions must remain 128 samples" );
static_assert( T3D_REGION_VERTEX_SPACING_METERS == 1u, "Terrain3D runtime vertex spacing must remain exactly 1 metre" );
static_assert( T3D_REGION_WORLD_SIZE_METERS == 128u, "Terrain3D runtime regions must remain exactly 128 metres wide" );
static_assert( T3D_REGION_SHIFT == 7 && T3D_REGION_MASK == 127,
			   "128-sample region addressing constants are inconsistent" );
static_assert( T3D_REGION_MAP_SIZE == 32, "Terrain3D runtime region map must remain 32x32" );
static_assert( T3D_REGION_MAP_SHIFT == 5 && T3D_REGION_MAP_MASK == 31,
			   "32-wide region-map addressing constants are inconsistent" );
static_assert( T3D_REGION_SLOTS == 1024, "Terrain3D runtime logical region capacity must remain 1024 slots" );
static_assert( T3D_REGION_MAP_HALF == 16, "Terrain3D runtime region-map half extent must remain 16" );
static_assert( T3D_REGION_TEXELS == T3D_REGION_SIZE * T3D_REGION_SIZE,
			   "Terrain3D runtime region texel count is inconsistent" );
static_assert( T3D_REGION_HEIGHT_BYTES == T3D_REGION_TEXELS * sizeof( uint16_t ),
			   "Terrain3D R16 height page size is inconsistent" );
static_assert( T3D_WORLD_TEXELS / T3D_REGION_SIZE == T3D_REGION_MAP_SIZE,
			   "Terrain3D world and region dimensions are inconsistent" );
static_assert( T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE == T3D_REGION_SLOTS,
			   "Terrain3D logical region capacity is inconsistent" );
static_assert( ( T3D_REGION_GEOMETRY_SIGNATURE & 0xffu ) == T3D_REGION_MAP_SIZE,
			   "Geometry signature must encode the 32-wide region map" );
static_assert( ( ( T3D_REGION_GEOMETRY_SIGNATURE >> 8u ) & 0xffu ) == T3D_REGION_VERTEX_SPACING_METERS,
			   "Geometry signature must encode 1 metre vertex spacing" );
static_assert( ( ( T3D_REGION_GEOMETRY_SIGNATURE >> 16u ) & 0xffu ) == T3D_REGION_SIZE,
			   "Geometry signature must encode 128 samples per region" );
static_assert( T3D_MAX_COLLISION_SHAPE_RECORDS == 4096,
			   "Dynamic collision must retain its independent 4096-shape ceiling" );

static_assert( T3D_RESIDENT_WORLD_SIZE == T3D_REGION_MAP_SIZE,
			   "Resident world map must use the specialized 32-wide domain" );
static_assert( T3D_RESIDENT_WORLD_SLOT_COUNT == T3D_REGION_SLOTS,
			   "Resident world slot capacity must use 1024 logical regions" );
static_assert( T3D_INSTANCE_REGION_TABLE_CAPACITY == ( size_t ) ( T3D_REGION_SLOTS ),
			   "Instance region table must use 1024 logical regions" );
static_assert( T3D_INSTANCE_MIN_REGION_SIZE == T3D_REGION_SIZE && T3D_INSTANCE_MAX_REGION_SIZE == T3D_REGION_SIZE,
			   "Instance layout must remain specialized to 128-sample regions" );
static_assert( T3D_MAX_COLLISION_AXIS_SAMPLES == T3D_REGION_SIZE + 1,
			   "Collision blend scratch must retain exactly 129 axis samples" );
static_assert( T3D_MAX_REGION_BLEND_SAMPLES == T3D_REGION_SIZE,
			   "Collision blend curve must retain exactly 128 region samples" );
static_assert( T3D_COLLISION_BAKED_CACHE_AXIS == 8 && T3D_BAKED_CACHE_CAPACITY == 64,
			   "Collision baked-region cache must remain 8x8 / 64 entries" );
static_assert( T3D_REGION_SLOTS <= UINT16_MAX, "Region slot indices must fit in uint16_t" );

#endif // TERRAIN3D_LIMITS_H
