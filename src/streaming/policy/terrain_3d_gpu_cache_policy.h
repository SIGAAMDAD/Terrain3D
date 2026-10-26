// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_GPU_CACHE_POLICY_H
#define TERRAIN3D_GPU_CACHE_POLICY_H

#include "Terrain3DTypes.h"

#include "core/config/terrain_3d_compiler.h"
#include "Terrain3DConstants.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

// R16 physical page table plus R8 logical-presence table.

// t3dGpuCacheCoord_s centralized in Terrain3DTypes.h.

// t3dGpuCacheSelectionStats_s centralized in Terrain3DTypes.h.

typedef bool ( *t3dGpuCacheActiveFn_t )( int iX, int iY, const void *pContext );

bool t3dGpuCacheOptionalChannelNeedsUpdate(
        const uint8_t nSourceFlags, const uint8_t nPhysicalFlags, const uint8_t nChannelMask );

size_t t3dGpuCacheTexelCount( const int nDivisor );

size_t t3dGpuCacheColorBytes( const int nDivisor );

size_t t3dGpuCacheShadowBytes( const int nDivisor );

size_t t3dGpuCacheEstimateLayerBytes( const bool bControlImplicit,
        const bool bColorImplicit,
        const bool bDerivedEnabled,
        const int nColorDivisor,
        const int nDerivedDivisor );

size_t t3dGpuCacheEstimateResidentLayerBytes( const bool bControlImplicit,
        const bool bColorImplicit,
        const bool bDerivedEnabled,
        const int nColorDivisor,
        const int nDerivedDivisor );

size_t t3dGpuCacheEstimateSharedBytes( const bool bControlImplicit, const bool bColorImplicit, const int nColorDivisor );

int t3dGpuCacheCalculateBudgetPhysicalLayers( const uint64_t nBudgetBytes,
        const int nMaxLayers,
        const bool bControlImplicit,
        const bool bColorImplicit,
        const bool bDerivedEnabled,
        const int nColorDivisor,
        const int nDerivedDivisor );

int t3dGpuCacheCalculateFootprintRadiusRegions( const int nMeshSize, const int nDetailLods );

int t3dGpuCacheCalculateRequestedPages( const int nActiveRegions,
        const int nRequiredRadius,
        const int nGuardRegions,
        const int nPrefetchRows,
        const int nHysteresisReserve );

int t3dGpuCacheCalculateCapacity( const uint64_t nBudgetBytes,
        const int nMaxLayers,
        const int nRequestedPages,
        const bool bControlImplicit,
        const bool bColorImplicit,
        const bool bDerivedEnabled,
        const int nColorDivisor,
        const int nDerivedDivisor,
        const int nRequestedSpareLayers );

int t3dGpuCacheCalculatePinnedCapacity( const uint64_t nBudgetBytes,
        const int nMaxLayers,
        const int nRequestedPages,
        const bool bControlImplicit,
        const bool bColorImplicit,
        const bool bDerivedEnabled,
        const int nColorDivisor,
        const int nDerivedDivisor );

int t3dGpuCacheCalculatePhysicalLayers(
        const int nMappedCapacity, const int nRequestedSpareLayers, const int nMaxLayers, const int nBudgetPhysicalLayers );

t3dGpuCacheCoord_t t3dGpuCacheClampCenter( const t3dGpuCacheCoord_t center );

bool t3dGpuCacheCoordValid( const t3dGpuCacheCoord_t coord );

bool t3dGpuCacheAppend( const t3dGpuCacheCoord_t coord,
        t3dGpuCacheCoord_t *pLocations,
        const int nCapacity,
        int &nWritten,
        t3dGpuCacheActiveFn_t active,
        const void *pContext );

void t3dGpuCacheCollectRing( const t3dGpuCacheCoord_t center,
        const int nRadius,
        t3dGpuCacheCoord_t *pLocations,
        const int nCapacity,
        int &nWritten,
        int &nCount,
        t3dGpuCacheActiveFn_t active,
        const void *pContext );

int t3dGpuCacheCollectFootprint( const t3dGpuCacheCoord_t center,
        t3dGpuCacheCoord_t *pLocations,
        const int nCapacity,
        t3dGpuCacheActiveFn_t active,
        const void *pContext,
        const int nRequiredRadius,
        const int nGuardRegions,
        const t3dGpuCacheCoord_t motionDirection,
        const int nPrefetchRows,
        t3dGpuCacheSelectionStats_t *pStats );

// Compatibility collector retained for CPU/instance caches. GPU residency uses
// t3dGpuCacheCollectFootprint().
int t3dGpuCacheCollectDesired( const t3dGpuCacheCoord_t center,
        t3dGpuCacheCoord_t *pLocations,
        const int nCapacity,
        t3dGpuCacheActiveFn_t active,
        const void *pContext );

#endif // TERRAIN3D_GPU_CACHE_POLICY_H
