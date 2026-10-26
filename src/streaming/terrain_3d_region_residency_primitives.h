// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_REGION_RESIDENCY_PRIMITIVES_H
#define TERRAIN3D_REGION_RESIDENCY_PRIMITIVES_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"
#include "data/terrain_3d_region_repository.h"

#include <EASTL/numeric_limits.h>
#include <stdint.h>

template <typename TMapEntry, typename TSlotPredicate>
EA_FORCE_INLINE int t3dRegionResidencyFindMappedSlot( const Vector2i &rLocation,
        const TMapEntry *const pRegionMap,
        const t3dRegionCoord_t *const pLocations,
        const int nCapacity,
        TSlotPredicate slotPredicate ) {
	if ( !pRegionMap || !pLocations || nCapacity <= 0 ) {
		return -1;
	}
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int slot = ( int )( pRegionMap[map_index] ) - 1;
	if ( slot < 0 || slot >= nCapacity || !slotPredicate( slot ) || !t3dRegionCoordEquals( pLocations[slot], rLocation ) ) {
		return -1;
	}
	return slot;
}

template <typename TSlotPredicate>
EA_FORCE_INLINE int t3dRegionResidencyFindFirstSlot( const int nCapacity, TSlotPredicate slotPredicate ) {
	for ( int slot = 0; slot < nCapacity; ++slot ) {
		if ( slotPredicate( slot ) ) {
			return slot;
		}
	}
	return -1;
}

EA_FORCE_INLINE void t3dRegionResidencyBuildDesiredMap(
        const Vector2i *const pDesired, const int nDesiredCount, uint8_t *const pMap, const size_t nMapCount ) {
	if ( !pMap || nMapCount == 0u ) {
		return;
	}
	for ( size_t index = 0u; index < nMapCount; ++index ) {
		pMap[index] = 0u;
	}
	if ( !pDesired || nDesiredCount <= 0 ) {
		return;
	}
	for ( int index = 0; index < nDesiredCount; ++index ) {
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( pDesired[index] );
		if ( map_index >= 0 && ( size_t )( map_index ) < nMapCount ) {
			pMap[map_index] = 1u;
		}
	}
}

EA_FORCE_INLINE int t3dRegionResidencyDistance( const Vector2i &rA, const Vector2i &rB ) {
	const int64_t dx = ( int64_t )( rA.x ) - ( int64_t )( rB.x );
	const int64_t dy = ( int64_t )( rA.y ) - ( int64_t )( rB.y );
	const uint64_t abs_x = dx < 0 ? ( uint64_t )( -dx ) : ( uint64_t )( dx );
	const uint64_t abs_y = dy < 0 ? ( uint64_t )( -dy ) : ( uint64_t )( dy );
	const uint64_t distance = abs_x > abs_y ? abs_x : abs_y;
	return distance > ( uint64_t )( eastl::numeric_limits<int>::max() ) ? eastl::numeric_limits<int>::max() : ( int )( distance );
}

template <typename TSlotPredicate>
EA_FORCE_INLINE int t3dRegionResidencySelectFarthestSlot( const t3dRegionCoord_t *const pLocations,
        const int nCapacity,
        const Vector2i &rCenter,
        TSlotPredicate slotPredicate ) {
	if ( !pLocations || nCapacity <= 0 ) {
		return -1;
	}
	int farthest_slot = -1;
	int farthest_distance = -1;
	for ( int slot = 0; slot < nCapacity; ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( pLocations[slot] );
		if ( !slotPredicate( slot, location ) ) {
			continue;
		}
		const int distance = t3dRegionResidencyDistance( location, rCenter );
		if ( distance > farthest_distance ) {
			farthest_distance = distance;
			farthest_slot = slot;
		}
	}
	return farthest_slot;
}

#endif // TERRAIN3D_REGION_RESIDENCY_PRIMITIVES_H
