// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_CPU_CACHE_POLICY_H
#define TERRAIN3D_CPU_CACHE_POLICY_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"

#include <stddef.h>
#include <stdint.h>

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

#endif // TERRAIN3D_CPU_CACHE_POLICY_H
