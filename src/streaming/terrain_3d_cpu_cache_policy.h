// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_CPU_CACHE_POLICY_H
#define TERRAIN3D_CPU_CACHE_POLICY_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"

#include <EASTL/numeric_limits.h>
#include <stddef.h>
#include <stdint.h>

typedef struct t3dCpuCacheCoord_s {
	int m_nX = 0;
	int m_nY = 0;
} t3dCpuCacheCoord_t;

static inline bool t3dCpuCacheMultiply( const size_t nA, const size_t nB, size_t &nResult ) {
	if ( nA != 0u && nB > eastl::numeric_limits<size_t>::max() / nA ) {
		return false;
	}
	nResult = nA * nB;
	return true;
}

static inline size_t t3dCpuCacheEstimateLayerBytes( const int nRegionSize ) {
	if ( nRegionSize <= 0 ) {
		return 0u;
	}
	size_t texels = 0u;
	size_t bytes = 0u;
	if ( !t3dCpuCacheMultiply( ( size_t )( nRegionSize ), ( size_t )( nRegionSize ), texels )
	        || !t3dCpuCacheMultiply( texels, s_nCpuCacheBytesPerTexel, bytes ) ) {
		return 0u;
	}
	return bytes;
}

static inline int t3dCpuCacheCalculateCapacity( const int nRegionSize, const int nBudgetMb, const int nMaxLayers ) {
	const size_t layer_bytes = t3dCpuCacheEstimateLayerBytes( nRegionSize );
	if ( layer_bytes == 0u || nBudgetMb <= 0 || nMaxLayers <= 0 ) {
		return 0;
	}
	size_t budget_bytes = 0u;
	if ( !t3dCpuCacheMultiply( ( size_t )( nBudgetMb ), s_nCpuCacheMib, budget_bytes ) ) {
		return 0;
	}
	const size_t raw_capacity = budget_bytes / layer_bytes;
	const size_t minimum_capacity = ( size_t )( s_nCpuCacheMinSampleLayers );
	const size_t legal_capacity = raw_capacity < minimum_capacity ? minimum_capacity : raw_capacity;
	return ( int )( legal_capacity > ( size_t )( nMaxLayers ) ? ( size_t )( nMaxLayers ) : legal_capacity );
}

EA_FORCE_INLINE static int t3dCpuCacheDistance( const t3dCpuCacheCoord_t &rA, const t3dCpuCacheCoord_t &rB ) {
	const int dx = rA.m_nX >= rB.m_nX ? rA.m_nX - rB.m_nX : rB.m_nX - rA.m_nX;
	const int dy = rA.m_nY >= rB.m_nY ? rA.m_nY - rB.m_nY : rB.m_nY - rA.m_nY;
	return dx > dy ? dx : dy;
}

#endif // TERRAIN3D_CPU_CACHE_POLICY_H
