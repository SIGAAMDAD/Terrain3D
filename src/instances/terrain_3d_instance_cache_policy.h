// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_INSTANCE_CACHE_POLICY_H
#define TERRAIN3D_INSTANCE_CACHE_POLICY_H

#include "core/terrain_3d_compiler.h"
#include "instances/terrain_3d_instance_cache_constants.h"

#include <cstddef>
#include <cstdint>
#include <limits>


typedef struct t3dInstanceCacheCoord_s {
	int m_nX;
	int m_nY;
} t3dInstanceCacheCoord_t;

inline int t3dInstanceCacheDistance(
	const t3dInstanceCacheCoord_t a,
	const t3dInstanceCacheCoord_t b
) {
	const int64_t dx = ( int64_t ) ( a.m_nX ) - ( int64_t ) ( b.m_nX );
	const int64_t dy = ( int64_t ) ( a.m_nY ) - ( int64_t ) ( b.m_nY );
	const uint64_t abs_x = dx < 0 ? ( uint64_t ) ( -dx ) : ( uint64_t ) ( dx );
	const uint64_t abs_y = dy < 0 ? ( uint64_t ) ( -dy ) : ( uint64_t ) ( dy );
	const uint64_t distance = abs_x > abs_y ? abs_x : abs_y;
	return distance > ( uint64_t ) ( std::numeric_limits<int>::max() ) ? std::numeric_limits<int>::max() : ( int ) ( distance );
}

inline bool t3dInstanceCacheCanStream(
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

EA_FORCE_INLINE bool t3dInstanceCacheIndexValid( const int iIndex, const int nCapacity ) { return iIndex >= 0 && nCapacity > 0 && nCapacity <= s_nInstanceCacheMaxPages && iIndex < nCapacity; }

#endif // TERRAIN3D_INSTANCE_CACHE_POLICY_H
