// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_GPU_CACHE_POLICY_H
#define TERRAIN3D_GPU_CACHE_POLICY_H

#include "core/terrain_3d_compiler.h"
#include "streaming/terrain_3d_gpu_cache_constants.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

// R16 physical page table plus R8 logical-presence table.

typedef struct t3dGpuCacheCoord_s {
	int m_nX;
	int m_nY;
} t3dGpuCacheCoord_t;

typedef struct t3dGpuCacheSelectionStats_s {
	int m_nRequired = 0;
	int m_nGuard = 0;
	int m_nPrefetch = 0;
	int m_nWritten = 0;
} t3dGpuCacheSelectionStats_t;

typedef bool ( *T3DGpuCacheActiveFn )( int iX, int iY, const void *pContext );

EA_FORCE_INLINE bool t3dGpuCacheOptionalChannelNeedsUpdate(
        const uint8_t nSourceFlags, const uint8_t nPhysicalFlags, const uint8_t nChannelMask ) {
	return ( ( nSourceFlags | nPhysicalFlags ) & nChannelMask ) != 0u;
}

inline size_t t3dGpuCacheTexelCount( const int nDivisor ) {
	if ( nDivisor <= 0 ) {
		return 0u;
	}
	const size_t side = ( size_t )( ( s_nRegionSize + nDivisor - 1 ) / nDivisor );
	if ( side > SIZE_MAX / side ) {
		return 0u;
	}
	return side * side;
}

inline size_t t3dGpuCacheColorBytes( const int nDivisor ) {
	if ( nDivisor <= 0 ) {
		return 0u;
	}
	size_t side = ( size_t )( ( s_nRegionSize + nDivisor - 1 ) / nDivisor );
	size_t bytes = 0u;
	for ( ;; ) {
		if ( side > SIZE_MAX / side ) {
			return 0u;
		}
		const size_t texels = side * side;
		if ( texels > ( SIZE_MAX - bytes ) / 4u ) {
			return 0u;
		}
		bytes += texels * 4u;
		if ( side == 1u ) {
			break;
		}
		side >>= 1u;
	}
	return bytes;
}

inline size_t t3dGpuCacheShadowBytes( const int nDivisor ) {
	if ( nDivisor <= 0 ) {
		return 0u;
	}
	size_t side = ( size_t )( ( s_nRegionSize + nDivisor - 1 ) / nDivisor );
	size_t bytes = 0u;
	for ( ;; ) {
		if ( side > SIZE_MAX / side ) {
			return 0u;
		}
		const size_t texels = side * side;
		if ( texels > SIZE_MAX - bytes ) {
			return 0u;
		}
		bytes += texels;
		if ( side == 1u ) {
			break;
		}
		side >>= 1u;
	}
	return bytes;
}

inline size_t t3dGpuCacheEstimateLayerBytes( const bool bControlImplicit,
        const bool bColorImplicit,
        const bool bDerivedEnabled,
        const int nColorDivisor,
        const int nDerivedDivisor ) {
	const size_t full_texels = t3dGpuCacheTexelCount( 1 );
	if ( full_texels == 0u ) {
		return 0u;
	}
	if ( full_texels > SIZE_MAX / 8u ) {
		return 0u;
	}
	size_t bytes = full_texels * 2u; // R16 height.
	if ( !bControlImplicit ) {
		bytes += full_texels * 4u; // RF packed control.
	}
	if ( !bColorImplicit ) {
		const size_t color_bytes = t3dGpuCacheColorBytes( nColorDivisor );
		if ( color_bytes == 0u || bytes > SIZE_MAX - color_bytes ) {
			return 0u;
		}
		bytes += color_bytes;
	}
	if ( bDerivedEnabled ) {
		const size_t derived_texels = t3dGpuCacheTexelCount( nDerivedDivisor );
		if ( derived_texels == 0u || derived_texels > ( SIZE_MAX - bytes ) / 2u ) {
			return 0u;
		}
		bytes += derived_texels * 2u;
	}
	return bytes;
}

inline size_t t3dGpuCacheEstimateResidentLayerBytes( const bool bControlImplicit,
        const bool bColorImplicit,
        const bool bDerivedEnabled,
        const int nColorDivisor,
        const int nDerivedDivisor ) {
	const size_t base_bytes =
	        t3dGpuCacheEstimateLayerBytes( bControlImplicit, bColorImplicit, bDerivedEnabled, nColorDivisor, nDerivedDivisor );
	const size_t middle_composite_bytes = t3dGpuCacheColorBytes( nColorDivisor );
	const size_t shadow_bytes = t3dGpuCacheShadowBytes( 1 );
	if ( base_bytes == 0u || middle_composite_bytes == 0u || shadow_bytes == 0u ) {
		return 0u;
	}
	if ( base_bytes > SIZE_MAX - middle_composite_bytes ) {
		return 0u;
	}
	const size_t with_middle = base_bytes + middle_composite_bytes;
	if ( with_middle > SIZE_MAX - shadow_bytes ) {
		return 0u;
	}
	return with_middle + shadow_bytes;
}

inline size_t t3dGpuCacheEstimateSharedBytes( const bool bControlImplicit, const bool bColorImplicit, const int nColorDivisor ) {
	const size_t full_texels = t3dGpuCacheTexelCount( 1 );
	if ( full_texels == 0u ) {
		return 0u;
	}
	size_t bytes = bControlImplicit ? full_texels * 4u : 0u;
	if ( bColorImplicit ) {
		const size_t color_bytes = t3dGpuCacheColorBytes( nColorDivisor );
		if ( color_bytes == 0u || bytes > SIZE_MAX - color_bytes ) {
			return 0u;
		}
		bytes += color_bytes;
	}
	// The aggregate memory policy owns far-atlas residency as fixed terrain
	// memory. This helper only accounts for storage owned by the managed GPU page
	// cache itself: implicit default maps plus its bounded page-table allocation.
	const size_t page_cache_owned_fixed_metadata_bytes = s_nGpuCacheMaxPageTableBytes;
	if ( bytes > SIZE_MAX - page_cache_owned_fixed_metadata_bytes ) {
		return 0u;
	}
	return bytes + page_cache_owned_fixed_metadata_bytes;
}

inline int t3dGpuCacheCalculateBudgetPhysicalLayers( const uint64_t nBudgetBytes,
        const int nMaxLayers,
        const bool bControlImplicit,
        const bool bColorImplicit,
        const bool bDerivedEnabled,
        const int nColorDivisor,
        const int nDerivedDivisor ) {
	if ( nBudgetBytes == 0u || nMaxLayers <= 0 || nBudgetBytes > ( uint64_t )( SIZE_MAX ) ) {
		return 0;
	}
	const size_t layer_bytes =
	        t3dGpuCacheEstimateResidentLayerBytes( bControlImplicit, bColorImplicit, bDerivedEnabled, nColorDivisor, nDerivedDivisor );
	const size_t shared_bytes = t3dGpuCacheEstimateSharedBytes( bControlImplicit, bColorImplicit, nColorDivisor );
	const size_t budget_bytes = ( size_t )( nBudgetBytes );
	if ( layer_bytes == 0u || budget_bytes <= shared_bytes ) {
		return 0;
	}
	const size_t budget_layers = ( budget_bytes - shared_bytes ) / layer_bytes;
	const size_t bounded_layers = budget_layers < ( size_t )( nMaxLayers ) ? budget_layers : ( size_t )( nMaxLayers );
	return bounded_layers > ( size_t )( INT_MAX ) ? INT_MAX : ( int )( bounded_layers );
}


inline int t3dGpuCacheCalculateFootprintRadiusRegions( const int nMeshSize, const int nDetailLods ) {
	if ( nMeshSize <= 0 || nDetailLods <= 0 ) {
		return 0;
	}
	const int shift = nDetailLods - 1 > 30 ? 30 : nDetailLods - 1;
	const double fScale = ldexp( 1.0, shift );
	const double fRadius = ( double )( nMeshSize * 2 + 8 ) * fScale;
	const double fRegionWorldSize = ( double )( s_nRegionWorldSizeMeters );
	if ( !isfinite( fRadius ) || !isfinite( fRegionWorldSize ) || fRegionWorldSize <= 0.0 ) {
		return 0;
	}
	const double fRegionRadius = ceil( fRadius / fRegionWorldSize );
	if ( fRegionRadius >= ( double )( INT_MAX ) ) {
		return INT_MAX;
	}
	return ( int )( fRegionRadius );
}

inline int t3dGpuCacheCalculateRequestedPages( const int nActiveRegions,
        const int nRequiredRadius,
        const int nGuardRegions,
        const int nPrefetchRows,
        const int nHysteresisReserve ) {
	if ( s_nRegionMapSize <= 0 || nActiveRegions <= 0 ) {
		return 0;
	}
	const int maximum_radius = s_nRegionMapSize - 1;
	const int required_radius = nRequiredRadius < 0 ? 0 : ( nRequiredRadius > maximum_radius ? maximum_radius : nRequiredRadius );
	const int guard = nGuardRegions < 0 ? 0 : nGuardRegions;
	const int outer_radius = required_radius + guard > maximum_radius ? maximum_radius : required_radius + guard;
	const size_t side = ( size_t )( outer_radius ) * 2u + 1u;
	if ( side > SIZE_MAX / side ) {
		return nActiveRegions;
	}
	size_t requested = side * side;
	if ( nPrefetchRows > 0 && side <= SIZE_MAX / ( size_t )( nPrefetchRows ) ) {
		requested += side * ( size_t )( nPrefetchRows );
	}
	if ( nHysteresisReserve > 0 && requested <= SIZE_MAX - ( size_t )( nHysteresisReserve ) ) {
		requested += ( size_t )( nHysteresisReserve );
	}
	const size_t logical_capacity = ( size_t )( s_nRegionMapSize ) * ( size_t )( s_nRegionMapSize );
	if ( requested > logical_capacity ) {
		requested = logical_capacity;
	}
	if ( requested > ( size_t )( nActiveRegions ) ) {
		requested = ( size_t )( nActiveRegions );
	}
	return requested > ( size_t )( INT_MAX ) ? INT_MAX : ( int )( requested );
}

inline int t3dGpuCacheCalculateCapacity( const uint64_t nBudgetBytes,
        const int nMaxLayers,
        const int nRequestedPages,
        const bool bControlImplicit,
        const bool bColorImplicit,
        const bool bDerivedEnabled,
        const int nColorDivisor,
        const int nDerivedDivisor,
        const int nRequestedSpareLayers ) {
	if ( s_nRegionMapSize <= 0 || nBudgetBytes == 0u || nMaxLayers <= 0 || nRequestedPages <= 0 ) {
		return 0;
	}
	const size_t layer_bytes =
	        t3dGpuCacheEstimateResidentLayerBytes( bControlImplicit, bColorImplicit, bDerivedEnabled, nColorDivisor, nDerivedDivisor );
	const size_t shared_bytes = t3dGpuCacheEstimateSharedBytes( bControlImplicit, bColorImplicit, nColorDivisor );
	if ( layer_bytes == 0u || nBudgetBytes > ( uint64_t )( SIZE_MAX ) ) {
		return 0;
	}
	const size_t budget_bytes = ( size_t )( nBudgetBytes );
	if ( budget_bytes <= shared_bytes ) {
		return 0;
	}
	const size_t available_bytes = budget_bytes > shared_bytes ? budget_bytes - shared_bytes : 0u;
	const size_t budget_layers = available_bytes / layer_bytes;
	const size_t maximum_physical_layers = ( size_t )( nMaxLayers );
	const size_t physical_layers = budget_layers < maximum_physical_layers ? budget_layers : maximum_physical_layers;
	const size_t minimum_spares = ( size_t )( s_nGpuCacheMinSpareLayers );
	( void )( nRequestedSpareLayers );
	// Mapped terrain is functional residency; configured spare/retirement pages are
	// a performance target. Capacity therefore reserves only the legal minimum spare
	// set and lets physical-layer selection spend remaining grant on extra spares.
	if ( physical_layers <= minimum_spares ) {
		return 0;
	}
	const size_t maximum_mapped_layers = physical_layers - minimum_spares;
	size_t capacity = ( size_t )( nRequestedPages );
	if ( capacity > maximum_mapped_layers ) {
		capacity = maximum_mapped_layers;
	}
	const size_t logical_capacity = ( size_t )( s_nRegionMapSize ) * ( size_t )( s_nRegionMapSize );
	if ( capacity > logical_capacity ) {
		capacity = logical_capacity;
	}
	return ( int )( capacity );
}

inline int t3dGpuCacheCalculatePinnedCapacity( const uint64_t nBudgetBytes,
        const int nMaxLayers,
        const int nRequestedPages,
        const bool bControlImplicit,
        const bool bColorImplicit,
        const bool bDerivedEnabled,
        const int nColorDivisor,
        const int nDerivedDivisor ) {
	if ( s_nRegionMapSize <= 0 || nBudgetBytes == 0u || nMaxLayers <= 0 || nRequestedPages <= 0 ) {
		return 0;
	}
	const size_t layer_bytes =
	        t3dGpuCacheEstimateResidentLayerBytes( bControlImplicit, bColorImplicit, bDerivedEnabled, nColorDivisor, nDerivedDivisor );
	const size_t shared_bytes = t3dGpuCacheEstimateSharedBytes( bControlImplicit, bColorImplicit, nColorDivisor );
	if ( layer_bytes == 0u || nBudgetBytes > ( uint64_t )( SIZE_MAX ) ) {
		return 0;
	}
	const size_t budget_bytes = ( size_t )( nBudgetBytes );
	if ( budget_bytes <= shared_bytes ) {
		return 0;
	}
	const size_t budget_layers = ( budget_bytes - shared_bytes ) / layer_bytes;
	const size_t logical_capacity = ( size_t )( s_nRegionMapSize ) * ( size_t )( s_nRegionMapSize );
	size_t capacity = ( size_t )( nRequestedPages );
	capacity = capacity < budget_layers ? capacity : budget_layers;
	capacity = capacity < ( size_t )( nMaxLayers ) ? capacity : ( size_t )( nMaxLayers );
	capacity = capacity < logical_capacity ? capacity : logical_capacity;
	return capacity > ( size_t )( INT_MAX ) ? INT_MAX : ( int )( capacity );
}

inline int t3dGpuCacheCalculatePhysicalLayers(
        const int nMappedCapacity, const int nRequestedSpareLayers, const int nMaxLayers, const int nBudgetPhysicalLayers ) {
	if ( nMappedCapacity <= 0 || nMaxLayers <= 0 || nBudgetPhysicalLayers < nMappedCapacity ) {
		return 0;
	}
	const int requested_spares = nRequestedSpareLayers < s_nGpuCacheMinSpareLayers ? s_nGpuCacheMinSpareLayers : nRequestedSpareLayers;
	const int maximum_physical = nBudgetPhysicalLayers < nMaxLayers ? nBudgetPhysicalLayers : nMaxLayers;
	const int available_spares = maximum_physical - nMappedCapacity;
	const int spare_layers = available_spares < requested_spares ? available_spares : requested_spares;
	return nMappedCapacity + ( spare_layers > 0 ? spare_layers : 0 );
}

inline t3dGpuCacheCoord_t t3dGpuCacheClampCenter( const t3dGpuCacheCoord_t center ) {
	t3dGpuCacheCoord_t result = center;
	if ( s_nRegionMapSize <= 0 ) {
		result.m_nX = 0;
		result.m_nY = 0;
		return result;
	}
	const int minimum = -( s_nRegionMapSize / 2 );
	const int maximum = minimum + s_nRegionMapSize - 1;
	if ( result.m_nX < minimum ) {
		result.m_nX = minimum;
	} else if ( result.m_nX > maximum ) {
		result.m_nX = maximum;
	}
	if ( result.m_nY < minimum ) {
		result.m_nY = minimum;
	} else if ( result.m_nY > maximum ) {
		result.m_nY = maximum;
	}
	return result;
}

inline bool t3dGpuCacheCoordValid( const t3dGpuCacheCoord_t coord ) {
	if ( s_nRegionMapSize <= 0 ) {
		return false;
	}
	const int minimum = -( s_nRegionMapSize / 2 );
	const int maximum = minimum + s_nRegionMapSize;
	return coord.m_nX >= minimum && coord.m_nX < maximum && coord.m_nY >= minimum && coord.m_nY < maximum;
}

inline bool t3dGpuCacheAppend( const t3dGpuCacheCoord_t coord,
        t3dGpuCacheCoord_t *pLocations,
        const int nCapacity,
        int &nWritten,
        T3DGpuCacheActiveFn active,
        const void *pContext ) {
	if ( !t3dGpuCacheCoordValid( coord ) || !active( coord.m_nX, coord.m_nY, pContext ) ) {
		return false;
	}
	if ( pLocations && nWritten < nCapacity ) {
		pLocations[nWritten] = coord;
		++nWritten;
	}
	return true;
}

inline void t3dGpuCacheCollectRing( const t3dGpuCacheCoord_t center,
        const int nRadius,
        t3dGpuCacheCoord_t *pLocations,
        const int nCapacity,
        int &nWritten,
        int &nCount,
        T3DGpuCacheActiveFn active,
        const void *pContext ) {
	if ( nRadius == 0 ) {
		if ( t3dGpuCacheAppend( center, pLocations, nCapacity, nWritten, active, pContext ) ) {
			++nCount;
		}
		return;
	}
	for ( int x = -nRadius; x <= nRadius; ++x ) {
		if ( t3dGpuCacheAppend( { center.m_nX + x, center.m_nY - nRadius }, pLocations, nCapacity, nWritten, active, pContext ) ) {
			++nCount;
		}
	}
	for ( int y = -nRadius + 1; y <= nRadius - 1; ++y ) {
		if ( t3dGpuCacheAppend( { center.m_nX + nRadius, center.m_nY + y }, pLocations, nCapacity, nWritten, active, pContext ) ) {
			++nCount;
		}
	}
	for ( int x = nRadius; x >= -nRadius; --x ) {
		if ( t3dGpuCacheAppend( { center.m_nX + x, center.m_nY + nRadius }, pLocations, nCapacity, nWritten, active, pContext ) ) {
			++nCount;
		}
	}
	for ( int y = nRadius - 1; y >= -nRadius + 1; --y ) {
		if ( t3dGpuCacheAppend( { center.m_nX - nRadius, center.m_nY + y }, pLocations, nCapacity, nWritten, active, pContext ) ) {
			++nCount;
		}
	}
}

inline int t3dGpuCacheCollectFootprint( const t3dGpuCacheCoord_t center,
        t3dGpuCacheCoord_t *pLocations,
        const int nCapacity,
        T3DGpuCacheActiveFn active,
        const void *pContext,
        const int nRequiredRadius,
        const int nGuardRegions,
        const t3dGpuCacheCoord_t motionDirection,
        const int nPrefetchRows,
        t3dGpuCacheSelectionStats_t *pStats ) {
	t3dGpuCacheSelectionStats_t stats;
	if ( nCapacity < 0 || s_nRegionMapSize <= 0 || !active ) {
		if ( pStats ) {
			*pStats = stats;
		}
		return 0;
	}
	const t3dGpuCacheCoord_t clampedCenter = t3dGpuCacheClampCenter( center );
	const int maximum_radius = s_nRegionMapSize - 1;
	const int required_radius = nRequiredRadius < 0 ? 0 : ( nRequiredRadius > maximum_radius ? maximum_radius : nRequiredRadius );
	const int guard = nGuardRegions < 0 ? 0 : nGuardRegions;
	const int outer_radius = required_radius + guard > maximum_radius ? maximum_radius : required_radius + guard;
	int written = 0;
	for ( int radius = 0; radius <= required_radius; ++radius ) {
		t3dGpuCacheCollectRing( clampedCenter, radius, pLocations, nCapacity, written, stats.m_nRequired, active, pContext );
	}
	for ( int radius = required_radius + 1; radius <= outer_radius; ++radius ) {
		t3dGpuCacheCollectRing( clampedCenter, radius, pLocations, nCapacity, written, stats.m_nGuard, active, pContext );
	}
	const int rows = nPrefetchRows < 0 ? 0 : nPrefetchRows;
	if ( rows > 0 && ( motionDirection.m_nX != 0 || motionDirection.m_nY != 0 ) ) {
		for ( int row = 1; row <= rows; ++row ) {
			if ( motionDirection.m_nX != 0 ) {
				const int x = center.m_nX + motionDirection.m_nX * ( outer_radius + row );
				for ( int y = -outer_radius; y <= outer_radius; ++y ) {
					if ( t3dGpuCacheAppend( { x, center.m_nY + y }, pLocations, nCapacity, written, active, pContext ) ) {
						++stats.m_nPrefetch;
					}
				}
			} else {
				const int y = center.m_nY + motionDirection.m_nY * ( outer_radius + row );
				for ( int x = -outer_radius; x <= outer_radius; ++x ) {
					if ( t3dGpuCacheAppend( { center.m_nX + x, y }, pLocations, nCapacity, written, active, pContext ) ) {
						++stats.m_nPrefetch;
					}
				}
			}
		}
	}
	stats.m_nWritten = written;
	if ( pStats ) {
		*pStats = stats;
	}
	return written;
}

// Compatibility collector retained for CPU/instance caches. GPU residency uses
// t3dGpuCacheCollectFootprint().
inline int t3dGpuCacheCollectDesired( const t3dGpuCacheCoord_t center,
        t3dGpuCacheCoord_t *pLocations,
        const int nCapacity,
        T3DGpuCacheActiveFn active,
        const void *pContext ) {
	if ( !pLocations || nCapacity <= 0 ) {
		return 0;
	}
	t3dGpuCacheSelectionStats_t stats;
	return t3dGpuCacheCollectFootprint( center, pLocations, nCapacity, active, pContext, s_nRegionMapSize - 1, 0, { 0, 0 }, 0, &stats );
}

#endif // TERRAIN3D_GPU_CACHE_POLICY_H
