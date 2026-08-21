// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_GPU_CACHE_POLICY_H
#define TERRAIN3D_GPU_CACHE_POLICY_H

#include "core/terrain_3d_compiler.h"
#include "streaming/terrain_3d_gpu_cache_constants.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

 // R16 physical page table plus R8 logical-presence table.

typedef struct t3dGpuCacheCoord_s {
	int x;
	int y;
} t3dGpuCacheCoord_t;

typedef struct t3dGpuCacheSelectionStats_s {
	int required = 0;
	int guard = 0;
	int prefetch = 0;
	int written = 0;
} t3dGpuCacheSelectionStats_t;

typedef bool ( *T3DGpuCacheActiveFn )( int iX, int iY, const void *pContext );

EA_FORCE_INLINE bool terrain3dGpuCacheOptionalChannelNeedsUpdate( const uint8_t nSourceFlags, const uint8_t nPhysicalFlags, const uint8_t nChannelMask ) { return ( ( nSourceFlags | nPhysicalFlags ) & nChannelMask ) != 0u; }

inline size_t terrain3dGpuCacheTexelCount( const int nRegionSize, const int nDivisor ) {
	if ( nRegionSize <= 0 || nDivisor <= 0 ) {
		return 0u;
	}
	const size_t side = ( size_t ) ( ( nRegionSize + nDivisor - 1 ) / nDivisor );
	if ( side > std::numeric_limits<size_t>::max() / side ) {
		return 0u;
	}
	return side * side;
}

inline size_t terrain3dGpuCacheColorBytes( const int nRegionSize, const int nDivisor ) {
	const size_t texels = terrain3dGpuCacheTexelCount( nRegionSize, nDivisor );
	if ( texels == 0u || texels > ( std::numeric_limits<size_t>::max() - 2u ) / 16u ) {
		return 0u;
	}
	// RGBA8 plus a complete mip chain: 4 bytes * 4/3.
	return ( texels * 16u + 2u ) / 3u;
}

inline size_t terrain3dGpuCacheEstimateLayerBytes(
	const int nRegionSize,
	const bool bControlImplicit, const bool bColorImplicit, const bool bDerivedEnabled,
	const int nColorDivisor, const int nDerivedDivisor
) {
	const size_t full_texels = terrain3dGpuCacheTexelCount( nRegionSize, 1 );
	if ( full_texels == 0u ) {
		return 0u;
	}
	if ( full_texels > std::numeric_limits<size_t>::max() / 8u ) {
		return 0u;
	}
	size_t bytes = full_texels * 2u; // R16 height.
	if ( !bControlImplicit ) {
		bytes += full_texels * 4u; // RF packed control.
	}
	if ( !bColorImplicit ) {
		const size_t color_bytes = terrain3dGpuCacheColorBytes( nRegionSize, nColorDivisor );
		if ( color_bytes == 0u || bytes > std::numeric_limits<size_t>::max() - color_bytes ) {
			return 0u;
		}
		bytes += color_bytes;
	}
	if ( bDerivedEnabled ) {
		const size_t derived_texels = terrain3dGpuCacheTexelCount( nRegionSize, nDerivedDivisor );
		if ( derived_texels == 0u || derived_texels > ( std::numeric_limits<size_t>::max() - bytes ) / 2u ) {
			return 0u;
		}
		bytes += derived_texels * 2u;
	}
	return bytes;
}

inline size_t terrain3dGpuCacheEstimateSharedBytes(
	const int nRegionSize,
	const bool bControlImplicit, const bool bColorImplicit,
	const int nColorDivisor, const int nFarQualityDivisor,
	const int nFarHeightSide = TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE
) {
	const size_t full_texels = terrain3dGpuCacheTexelCount( nRegionSize, 1 );
	if ( full_texels == 0u ) {
		return 0u;
	}
	size_t bytes = bControlImplicit ? full_texels * 4u : 0u;
	if ( bColorImplicit ) {
		const size_t color_bytes = terrain3dGpuCacheColorBytes( nRegionSize, nColorDivisor );
		if ( color_bytes == 0u || bytes > std::numeric_limits<size_t>::max() - color_bytes ) {
			return 0u;
		}
		bytes += color_bytes;
	}
	const int far_divisor = nFarQualityDivisor <= 1 ? 1 :
		( nFarQualityDivisor <= 2 ? 2 : ( nFarQualityDivisor <= 4 ? 4 : 8 ) );
	const size_t far_surface_side = ( size_t ) ( TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE / far_divisor );
	const size_t far_height_side = ( size_t ) ( nFarHeightSide > 0 ? nFarHeightSide : 1 );
	if ( far_surface_side == 0u || far_surface_side > std::numeric_limits<size_t>::max() / far_surface_side ||
		far_height_side > std::numeric_limits<size_t>::max() / far_height_side ) {
		return 0u;
	}
	const size_t far_surface_texels = far_surface_side * far_surface_side;
	const size_t far_height_texels = far_height_side * far_height_side;
	if ( far_height_texels > std::numeric_limits<size_t>::max() / 2u ||
		far_surface_texels > std::numeric_limits<size_t>::max() / 4u ) {
		return 0u;
	}
	const size_t far_height_bytes = far_height_texels * 2u;
	const size_t far_surface_bytes = far_surface_texels * 4u;
	if ( far_height_bytes > std::numeric_limits<size_t>::max() - far_surface_bytes - TERRAIN3D_GPU_CACHE_MAX_PAGE_TABLE_BYTES ) {
		return 0u;
	}
	const size_t fixed_bytes = far_height_bytes + far_surface_bytes + TERRAIN3D_GPU_CACHE_MAX_PAGE_TABLE_BYTES;
	if ( bytes > std::numeric_limits<size_t>::max() - fixed_bytes ) {
		return 0u;
	}
	return bytes + fixed_bytes;
}

inline int terrain3dGpuCacheCalculateFootprintRadiusRegions(
	const int nMeshSize,
	const int nDetailLods, const double fVertexSpacing, const int nRegionSize
) {
	if ( nMeshSize <= 0 || nDetailLods <= 0 || fVertexSpacing <= 0.0 || nRegionSize <= 0 ) {
		return 0;
	}
	const int shift = nDetailLods - 1 > 30 ? 30 : nDetailLods - 1;
	const double fScale = std::ldexp( fVertexSpacing, shift );
	const double fRadius = ( double ) ( nMeshSize * 2 + 8 ) * fScale;
	const double fRegionWorldSize = ( double ) ( nRegionSize ) * fVertexSpacing;
	if ( !std::isfinite( fRadius ) || !std::isfinite( fRegionWorldSize ) || fRegionWorldSize <= 0.0 ) {
		return 0;
	}
	const double fRegionRadius = std::ceil( fRadius / fRegionWorldSize );
	if ( fRegionRadius >= ( double ) ( std::numeric_limits<int>::max() ) ) {
		return std::numeric_limits<int>::max();
	}
	return ( int ) ( fRegionRadius );
}

inline int terrain3dGpuCacheCalculateRequestedPages(
	const int nMapSize, const int nActiveRegions,
	const int nRequiredRadius, const int nGuardRegions, const int nPrefetchRows,
	const int nHysteresisReserve
) {
	if ( nMapSize <= 0 || nActiveRegions <= 0 ) {
		return 0;
	}
	const int maximum_radius = nMapSize - 1;
	const int required_radius = nRequiredRadius < 0 ? 0 :
		( nRequiredRadius > maximum_radius ? maximum_radius : nRequiredRadius );
	const int guard = nGuardRegions < 0 ? 0 : nGuardRegions;
	const int outer_radius = required_radius + guard > maximum_radius ? maximum_radius : required_radius + guard;
	const size_t side = ( size_t ) ( outer_radius ) * 2u + 1u;
	if ( side > std::numeric_limits<size_t>::max() / side ) {
		return nActiveRegions;
	}
	size_t requested = side * side;
	if ( nPrefetchRows > 0 && side <= std::numeric_limits<size_t>::max() / ( size_t ) ( nPrefetchRows ) ) {
		requested += side * ( size_t ) ( nPrefetchRows );
	}
	if ( nHysteresisReserve > 0 && requested <= std::numeric_limits<size_t>::max() - ( size_t ) ( nHysteresisReserve ) ) {
		requested += ( size_t ) ( nHysteresisReserve );
	}
	const size_t logical_capacity = ( size_t ) ( nMapSize ) * ( size_t ) ( nMapSize );
	if ( requested > logical_capacity ) {
		requested = logical_capacity;
	}
	if ( requested > ( size_t ) ( nActiveRegions ) ) {
		requested = ( size_t ) ( nActiveRegions );
	}
	return requested > ( size_t ) ( std::numeric_limits<int>::max() ) ? std::numeric_limits<int>::max() : ( int ) ( requested );
}

inline int terrain3dGpuCacheCalculateCapacity(
	const int nRegionSize, const int nMapSize,
	const int nBudgetMb, const int nMaxLayers, const int nRequestedPages,
	const bool bControlImplicit, const bool bColorImplicit, const bool bDerivedEnabled,
	const int nColorDivisor, const int nDerivedDivisor, const int nFarQualityDivisor,
	const int nRequestedSpareLayers,
	const int nFarHeightSide = TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE
) {
	if ( nMapSize <= 0 || nBudgetMb <= 0 || nMaxLayers <= 0 || nRequestedPages <= 0 ) {
		return 0;
	}
	const size_t layer_bytes = terrain3dGpuCacheEstimateLayerBytes( nRegionSize,
		bControlImplicit, bColorImplicit, bDerivedEnabled, nColorDivisor, nDerivedDivisor );
	const size_t shared_bytes = terrain3dGpuCacheEstimateSharedBytes( nRegionSize,
		bControlImplicit, bColorImplicit, nColorDivisor, nFarQualityDivisor, nFarHeightSide );
	if ( layer_bytes == 0u || ( size_t ) ( nBudgetMb ) > std::numeric_limits<size_t>::max() / TERRAIN3D_GPU_CACHE_MIB ) {
		return 0;
	}
	const size_t budget_bytes = ( size_t ) ( nBudgetMb ) * TERRAIN3D_GPU_CACHE_MIB;
	const size_t available_bytes = budget_bytes > shared_bytes ? budget_bytes - shared_bytes : 0u;
	const size_t budget_layers = available_bytes / layer_bytes;
	const size_t maximum_physical_layers = ( size_t ) ( nMaxLayers );
	const size_t requested_spares = ( size_t ) ( nRequestedSpareLayers < TERRAIN3D_GPU_CACHE_MIN_SPARE_LAYERS ?
		TERRAIN3D_GPU_CACHE_MIN_SPARE_LAYERS : nRequestedSpareLayers );
	const size_t physical_layers = budget_layers < maximum_physical_layers ? budget_layers : maximum_physical_layers;
	// Mapped capacity is renderer demand constrained. Spare and retiring layers are
	// physical publication resources and must never be counted as mapped demand.
	// If one page already exceeds the configured byte ceiling, retain one mapped
	// page and the minimum legal spare set so failure remains explicit and bounded.
	const size_t effective_physical_layers = physical_layers > 0u ? physical_layers :
		( size_t ) ( TERRAIN3D_GPU_CACHE_MIN_SPARE_LAYERS + 1 );
	const size_t spare_layers = requested_spares < effective_physical_layers ? requested_spares :
		( effective_physical_layers > 1u ? effective_physical_layers - 1u : 0u );
	const size_t maximum_mapped_layers = effective_physical_layers > spare_layers ?
		effective_physical_layers - spare_layers : 1u;
	size_t capacity = ( size_t ) ( nRequestedPages );
	if ( capacity > maximum_mapped_layers ) {
		capacity = maximum_mapped_layers;
	}
	const size_t logical_capacity = ( size_t ) ( nMapSize ) * ( size_t ) ( nMapSize );
	if ( capacity > logical_capacity ) {
		capacity = logical_capacity;
	}
	return ( int ) ( capacity );
}

inline int terrain3dGpuCacheCalculatePinnedCapacity(
	const int nRegionSize, const int nMapSize,
	const int nBudgetMb, const int nMaxLayers, const int nRequestedPages,
	const bool bControlImplicit, const bool bColorImplicit, const bool bDerivedEnabled,
	const int nColorDivisor, const int nDerivedDivisor, const int nFarQualityDivisor,
	const int nFarHeightSide = TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE
) {
	if ( nMapSize <= 0 || nBudgetMb <= 0 || nMaxLayers <= 0 || nRequestedPages <= 0 ) {
		return 0;
	}
	const size_t layer_bytes = terrain3dGpuCacheEstimateLayerBytes( nRegionSize,
		bControlImplicit, bColorImplicit, bDerivedEnabled, nColorDivisor, nDerivedDivisor );
	const size_t shared_bytes = terrain3dGpuCacheEstimateSharedBytes( nRegionSize,
		bControlImplicit, bColorImplicit, nColorDivisor, nFarQualityDivisor, nFarHeightSide );
	if ( layer_bytes == 0u || ( size_t ) ( nBudgetMb ) >
		std::numeric_limits<size_t>::max() / TERRAIN3D_GPU_CACHE_MIB ) {
		return 0;
	}
	const size_t budget_bytes = ( size_t ) ( nBudgetMb ) * TERRAIN3D_GPU_CACHE_MIB;
	if ( budget_bytes <= shared_bytes ) {
		return 0;
	}
	const size_t budget_layers = ( budget_bytes - shared_bytes ) / layer_bytes;
	const size_t logical_capacity = ( size_t ) ( nMapSize ) * ( size_t ) ( nMapSize );
	size_t capacity = ( size_t ) ( nRequestedPages );
	capacity = capacity < budget_layers ? capacity : budget_layers;
	capacity = capacity < ( size_t ) ( nMaxLayers ) ? capacity : ( size_t ) ( nMaxLayers );
	capacity = capacity < logical_capacity ? capacity : logical_capacity;
	return capacity > ( size_t ) ( std::numeric_limits<int>::max() ) ?
		std::numeric_limits<int>::max() : ( int ) ( capacity );
}

inline int terrain3dGpuCacheCalculatePhysicalLayers(
	const int nMappedCapacity,
	const int nRequestedSpareLayers, const int nMaxLayers
) {
	if ( nMappedCapacity <= 0 || nMaxLayers <= 0 ) {
		return 0;
	}
	const int requested_spares = nRequestedSpareLayers < TERRAIN3D_GPU_CACHE_MIN_SPARE_LAYERS ?
		TERRAIN3D_GPU_CACHE_MIN_SPARE_LAYERS : nRequestedSpareLayers;
	const int physical = nMappedCapacity > nMaxLayers - requested_spares ? nMaxLayers :
		nMappedCapacity + requested_spares;
	return physical < 1 ? 1 : physical;
}

inline t3dGpuCacheCoord_t terrain3dGpuCacheClampCenter(
	const t3dGpuCacheCoord_t center,
	const int nMapSize
) {
	t3dGpuCacheCoord_t result = center;
	if ( nMapSize <= 0 ) {
		result.x = 0;
		result.y = 0;
		return result;
	}
	const int minimum = -( nMapSize / 2 );
	const int maximum = minimum + nMapSize - 1;
	if ( result.x < minimum ) {
		result.x = minimum;
	} else if ( result.x > maximum ) {
		result.x = maximum;
	}
	if ( result.y < minimum ) {
		result.y = minimum;
	} else if ( result.y > maximum ) {
		result.y = maximum;
	}
	return result;
}

inline bool terrain3dGpuCacheCoordValid( const t3dGpuCacheCoord_t coord, const int nMapSize ) {
	if ( nMapSize <= 0 ) {
		return false;
	}
	const int minimum = -( nMapSize / 2 );
	const int maximum = minimum + nMapSize;
	return coord.x >= minimum && coord.x < maximum && coord.y >= minimum && coord.y < maximum;
}

inline bool terrain3dGpuCacheAppend(
	const t3dGpuCacheCoord_t coord, const int nMapSize,
	t3dGpuCacheCoord_t *pLocations, const int nCapacity, int &nWritten,
	T3DGpuCacheActiveFn active, const void *pContext
) {
	if ( !terrain3dGpuCacheCoordValid( coord, nMapSize ) || !active( coord.x, coord.y, pContext ) ) {
		return false;
	}
	if ( pLocations && nWritten < nCapacity ) {
		pLocations[nWritten] = coord;
		++nWritten;
	}
	return true;
}

inline void terrain3dGpuCacheCollectRing(
	const t3dGpuCacheCoord_t center, const int nMapSize,
	const int nRadius, t3dGpuCacheCoord_t *pLocations, const int nCapacity, int &nWritten,
	int &nCount, T3DGpuCacheActiveFn active, const void *pContext
) {
	if ( nRadius == 0 ) {
		if ( terrain3dGpuCacheAppend( center, nMapSize, pLocations, nCapacity, nWritten, active, pContext ) ) {
			++nCount;
		}
		return;
	}
	for ( int x = -nRadius; x <= nRadius; ++x ) {
		if ( terrain3dGpuCacheAppend( {center.x + x, center.y - nRadius}, nMapSize,
			pLocations, nCapacity, nWritten, active, pContext ) ) {
			++nCount;
		}
	}
	for ( int y = -nRadius + 1; y <= nRadius - 1; ++y ) {
		if ( terrain3dGpuCacheAppend( {center.x + nRadius, center.y + y}, nMapSize,
			pLocations, nCapacity, nWritten, active, pContext ) ) {
			++nCount;
		}
	}
	for ( int x = nRadius; x >= -nRadius; --x ) {
		if ( terrain3dGpuCacheAppend( {center.x + x, center.y + nRadius}, nMapSize,
			pLocations, nCapacity, nWritten, active, pContext ) ) {
			++nCount;
		}
	}
	for ( int y = nRadius - 1; y >= -nRadius + 1; --y ) {
		if ( terrain3dGpuCacheAppend( {center.x - nRadius, center.y + y}, nMapSize,
			pLocations, nCapacity, nWritten, active, pContext ) ) {
			++nCount;
		}
	}
}

inline int terrain3dGpuCacheCollectFootprint(
	const t3dGpuCacheCoord_t center, const int nMapSize,
	t3dGpuCacheCoord_t *pLocations, const int nCapacity, T3DGpuCacheActiveFn active,
	const void *pContext, const int nRequiredRadius, const int nGuardRegions,
	const t3dGpuCacheCoord_t motionDirection, const int nPrefetchRows,
	t3dGpuCacheSelectionStats_t *pStats
) {
	t3dGpuCacheSelectionStats_t stats;
	if ( nCapacity < 0 || nMapSize <= 0 || !active ) {
		if ( pStats ) {
			*pStats = stats;
		}
		return 0;
	}
	const t3dGpuCacheCoord_t clampedCenter = terrain3dGpuCacheClampCenter( center, nMapSize );
	const int maximum_radius = nMapSize - 1;
	const int required_radius = nRequiredRadius < 0 ? 0 :
		( nRequiredRadius > maximum_radius ? maximum_radius : nRequiredRadius );
	const int guard = nGuardRegions < 0 ? 0 : nGuardRegions;
	const int outer_radius = required_radius + guard > maximum_radius ? maximum_radius : required_radius + guard;
	int written = 0;
	for ( int radius = 0; radius <= required_radius; ++radius ) {
		terrain3dGpuCacheCollectRing( clampedCenter, nMapSize, radius, pLocations, nCapacity, written,
			stats.required, active, pContext );
	}
	for ( int radius = required_radius + 1; radius <= outer_radius; ++radius ) {
		terrain3dGpuCacheCollectRing( clampedCenter, nMapSize, radius, pLocations, nCapacity, written,
			stats.guard, active, pContext );
	}
	const int rows = nPrefetchRows < 0 ? 0 : nPrefetchRows;
	if ( rows > 0 && ( motionDirection.x != 0 || motionDirection.y != 0 ) ) {
		for ( int row = 1; row <= rows; ++row ) {
			if ( motionDirection.x != 0 ) {
				const int x = center.x + motionDirection.x * ( outer_radius + row );
				for ( int y = -outer_radius; y <= outer_radius; ++y ) {
					if ( terrain3dGpuCacheAppend( {x, center.y + y}, nMapSize, pLocations,
						nCapacity, written, active, pContext ) ) {
						++stats.prefetch;
					}
				}
			} else {
				const int y = center.y + motionDirection.y * ( outer_radius + row );
				for ( int x = -outer_radius; x <= outer_radius; ++x ) {
					if ( terrain3dGpuCacheAppend( {center.x + x, y}, nMapSize, pLocations,
						nCapacity, written, active, pContext ) ) {
						++stats.prefetch;
					}
				}
			}
		}
	}
	stats.written = written;
	if ( pStats ) {
		*pStats = stats;
	}
	return written;
}

// Compatibility collector retained for CPU/instance caches. GPU residency uses
// terrain3d_gpu_cache_collect_footprint().
inline int terrain3dGpuCacheCollectDesired(
	const t3dGpuCacheCoord_t center, const int nMapSize,
	t3dGpuCacheCoord_t *pLocations, const int nCapacity, T3DGpuCacheActiveFn active,
	const void *pContext
) {
	if ( !pLocations || nCapacity <= 0 ) {
		return 0;
	}
	t3dGpuCacheSelectionStats_t stats;
	return terrain3dGpuCacheCollectFootprint( center, nMapSize, pLocations, nCapacity,
		active, pContext, nMapSize - 1, 0, {0, 0}, 0, &stats );
}

#endif // TERRAIN3D_GPU_CACHE_POLICY_H
