// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_LIMITS_H
#define TERRAIN3D_LIMITS_H

#include "Terrain3DConstants.h"

#include <stddef.h>
#include <stdint.h>

// Canonical runtime terrain specialization. The public Terrain3D contract is
// 128 samples at 1 m spacing, giving a 32x32 logical grid over 4096 texels.
static_assert( s_nRegionSize == 128, "Terrain3D runtime regions must remain 128 samples" );
static_assert( s_nRegionVertexSpacingMeters == 1u, "Terrain3D runtime vertex spacing must remain exactly 1 metre" );
static_assert( s_nRegionWorldSizeMeters == 128u, "Terrain3D runtime regions must remain exactly 128 metres wide" );
static_assert( s_nRegionShift == 7 && s_nRegionMask == 127, "128-sample region addressing constants are inconsistent" );
static_assert( s_nRegionMapSize == 32, "Terrain3D runtime region map must remain 32x32" );
static_assert( s_nRegionMapShift == 5 && s_nRegionMapMask == 31, "32-wide region-map addressing constants are inconsistent" );
static_assert( s_nRegionSlots == 1024, "Terrain3D runtime logical region capacity must remain 1024 slots" );
static_assert( s_nRegionMapHalf == 16, "Terrain3D runtime region-map half extent must remain 16" );
static_assert( s_nRegionTexels == s_nRegionSize * s_nRegionSize, "Terrain3D runtime region texel count is inconsistent" );
static_assert( s_nRegionHeightBytes == s_nRegionTexels * sizeof( uint16_t ), "Terrain3D R16 height page size is inconsistent" );
static_assert( s_nWorldTexels / s_nRegionSize == s_nRegionMapSize, "Terrain3D world and region dimensions are inconsistent" );
static_assert( s_nRegionMapSize * s_nRegionMapSize == s_nRegionSlots, "Terrain3D logical region capacity is inconsistent" );
static_assert( ( s_nRegionGeometrySignature & 0xffu ) == s_nRegionMapSize, "Geometry signature must encode the 32-wide region map" );
static_assert( ( ( s_nRegionGeometrySignature >> 8u ) & 0xffu ) == s_nRegionVertexSpacingMeters,
		"Geometry signature must encode 1 metre vertex spacing" );
static_assert( ( ( s_nRegionGeometrySignature >> 16u ) & 0xffu ) == s_nRegionSize,
		"Geometry signature must encode 128 samples per region" );
static_assert( s_nMaxCollisionShapeRecords == 4096, "Dynamic collision must retain its independent 4096-shape ceiling" );

static_assert( s_nResidentWorldSize == s_nRegionMapSize, "Resident world map must use the specialized 32-wide domain" );
static_assert( s_nResidentWorldSlotCount == s_nRegionSlots, "Resident world slot capacity must use 1024 logical regions" );
static_assert( s_nInstanceRegionTableCapacity == ( size_t )( s_nRegionSlots ), "Instance region table must use 1024 logical regions" );
static_assert( s_nInstanceMinRegionSize == s_nRegionSize && s_nInstanceMaxRegionSize == s_nRegionSize, "Instance layout must remain specialized to 128-sample regions" );
static_assert( s_nMaxCollisionAxisSamples == s_nRegionSize + 1, "Collision blend scratch must retain exactly 129 axis samples" );
static_assert( s_nMaxRegionBlendSamples == s_nRegionSize, "Collision blend curve must retain exactly 128 region samples" );
static_assert( s_nCollisionBakedCacheAxis == 8 && s_nBakedCacheCapacity == 64, "Collision baked-region cache must remain 8x8 / 64 entries" );
static_assert( s_nRegionSlots <= UINT16_MAX, "Region slot indices must fit in uint16_t" );



#endif // TERRAIN3D_LIMITS_H
