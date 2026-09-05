// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_CPU_CACHE_POLICY_H
#define TERRAIN3D_CPU_CACHE_POLICY_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"

#include <stddef.h>
#include <stdint.h>

typedef struct t3dCpuCacheCoord_s {
	int m_nX = 0;
	int m_nY = 0;
} t3dCpuCacheCoord_t;

EA_FORCE_INLINE static size_t t3dCpuCacheEstimateLayerBytes( void ) {
	return ( size_t )( s_nRegionTexels )*s_nCpuCacheBytesPerTexel;
}

EA_FORCE_INLINE static int t3dCpuCacheCalculateCapacity( const int nBudgetMb, const int nMaxLayers ) {
	if ( nBudgetMb <= 0 || nMaxLayers <= 0 ) {
		return 0;
	}
	const size_t budget_bytes = ( size_t )( nBudgetMb )*s_nCpuCacheMib;
	const size_t raw_capacity = budget_bytes / t3dCpuCacheEstimateLayerBytes();
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
