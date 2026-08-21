// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_CPU_CACHE_POLICY_H
#define TERRAIN3D_CPU_CACHE_POLICY_H

#include "core/terrain_3d_compiler.h"
#include "Terrain3DConstants.h"

#include <cstddef>
#include <cstdint>
#include <limits>


typedef struct t3dCpuCacheCoord_s {
	int x = 0;
	int y = 0;
} t3dCpuCacheCoord_t;

static inline bool terrain3dCpuCacheMultiply( const size_t nA, const size_t nB, size_t &nResult ) {
	if ( nA != 0u && nB > std::numeric_limits<size_t>::max() / nA ) {
		return false;
	}
	nResult = nA * nB;
	return true;
}

static inline size_t terrain3dCpuCacheEstimateLayerBytes( const int nRegionSize ) {
	if ( nRegionSize <= 0 ) {
		return 0u;
	}
	size_t texels = 0u;
	size_t bytes = 0u;
	if ( !terrain3dCpuCacheMultiply( ( size_t ) ( nRegionSize ), ( size_t ) ( nRegionSize ), texels ) ||
		!terrain3dCpuCacheMultiply( texels, TERRAIN3D_CPU_CACHE_BYTES_PER_TEXEL, bytes ) ) {
		return 0u;
	}
	return bytes;
}

static inline int terrain3dCpuCacheCalculateCapacity(
	const int nRegionSize, const int nBudgetMb,
	const int nMaxLayers
) {
	const size_t layer_bytes = terrain3dCpuCacheEstimateLayerBytes( nRegionSize );
	if ( layer_bytes == 0u || nBudgetMb <= 0 || nMaxLayers <= 0 ) {
		return 0;
	}
	size_t budget_bytes = 0u;
	if ( !terrain3dCpuCacheMultiply( ( size_t ) ( nBudgetMb ), TERRAIN3D_CPU_CACHE_MIB, budget_bytes ) ) {
		return 0;
	}
	const size_t raw_capacity = budget_bytes / layer_bytes;
	const size_t minimum_capacity = ( size_t ) ( TERRAIN3D_CPU_CACHE_MIN_SAMPLE_LAYERS );
	const size_t legal_capacity = raw_capacity < minimum_capacity ? minimum_capacity : raw_capacity;
	return ( int ) ( legal_capacity > ( size_t ) ( nMaxLayers ) ? ( size_t ) ( nMaxLayers ) : legal_capacity );
}

EA_FORCE_INLINE static int terrain3dCpuCacheDistance( const t3dCpuCacheCoord_t &rA, const t3dCpuCacheCoord_t &rB ) { const int dx = rA.x >= rB.x ? rA.x - rB.x : rB.x - rA.x; const int dy = rA.y >= rB.y ? rA.y - rB.y : rB.y - rA.y; return dx > dy ? dx : dy; }

#endif // TERRAIN3D_CPU_CACHE_POLICY_H
