// Copyright (c) 2026 Terrain3D contributors.
// Pure, allocation-free bounds/owner decision for 64m HLOD quadrants.
#ifndef TERRAIN3D_HLOD_SUBCELL_OWNERSHIP_H
#define TERRAIN3D_HLOD_SUBCELL_OWNERSHIP_H

#include <stdint.h>
#include <cmath>

static inline uint8_t t3dHlodSubcellOwnerForBounds(
    const float fMinX, const float fMinZ, const float fMaxX, const float fMaxZ )
{
    if ( !std::isfinite( fMinX ) || !std::isfinite( fMinZ ) || !std::isfinite( fMaxX ) || !std::isfinite( fMaxZ ) ||
         fMinX < 0.0f || fMinZ < 0.0f || fMaxX > 128.0f || fMaxZ > 128.0f ||
         fMaxX < fMinX || fMaxZ < fMinZ ) { return 0xffu; }
    const uint8_t x = fMinX >= 64.0f ? 1u : 0u;
    const uint8_t z = fMinZ >= 64.0f ? 1u : 0u;
    if ( fMaxX > ( x ? 128.0f : 64.0f ) ||
         fMaxZ > ( z ? 128.0f : 64.0f ) ) { return 0xffu; }
    return ( uint8_t ) ( z * 2u + x );
}

static inline bool t3dHlodSubcellOwnedAndReady(
    const uint8_t nAuthoredMask, const uint8_t nReadyMask, const uint8_t nSlot )
{
    return nSlot < 4u && ( nAuthoredMask & nReadyMask & (uint8_t)( 1u << nSlot ) ) != 0u;
}

#endif // TERRAIN3D_HLOD_SUBCELL_OWNERSHIP_H
