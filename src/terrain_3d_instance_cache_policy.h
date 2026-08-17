// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_INSTANCE_CACHE_POLICY_H
#define TERRAIN3D_INSTANCE_CACHE_POLICY_H

#include "Terrain3DConstants.h"

#include <cstddef>
#include <cstdint>
#include <limits>


typedef struct t3dinstanceCacheCoord_s {
	int x;
	int y;
} t3dinstanceCacheCoord_t;

inline int terrain3dInstanceCacheDistance(
	const t3dinstanceCacheCoord_t a,
	const t3dinstanceCacheCoord_t b
) {
	const int64_t dx = ( int64_t ) ( a.x ) - ( int64_t ) ( b.x );
	const int64_t dy = ( int64_t ) ( a.y ) - ( int64_t ) ( b.y );
	const uint64_t abs_x = dx < 0 ? ( uint64_t ) ( -dx ) : ( uint64_t ) ( dx );
	const uint64_t abs_y = dy < 0 ? ( uint64_t ) ( -dy ) : ( uint64_t ) ( dy );
	const uint64_t distance = abs_x > abs_y ? abs_x : abs_y;
	return distance > ( uint64_t ) ( std::numeric_limits<int>::max() ) ? std::numeric_limits<int>::max() : ( int ) ( distance );
}

inline bool terrain3dInstanceCacheCanStream(
	const size_t nResidentBytes, const size_t nIncomingBytes,
	const size_t nBudgetBytes, const int nResidentPages
) {
	if ( nResidentPages <= 0 ) {
		return true;
	}
	if ( nIncomingBytes > nBudgetBytes || nResidentBytes > nBudgetBytes ) {
		return false;
	}
	return nIncomingBytes <= nBudgetBytes - nResidentBytes;
}

inline bool terrain3dInstanceCacheIndexValid( const int iIndex, const int nCapacity ) {
	return iIndex >= 0 && nCapacity > 0 && nCapacity <= TERRAIN3D_INSTANCE_CACHE_MAX_PAGES && iIndex < nCapacity;
}

#endif // TERRAIN3D_INSTANCE_CACHE_POLICY_H
