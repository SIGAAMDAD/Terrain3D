// Copyright (c) 2026 Terrain3D contributors.
// Pure 4-slot completeness rule; no Godot or per-frame allocations.
#ifndef TERRAIN3D_HLOD_SUBCELL_GROUP_H
#define TERRAIN3D_HLOD_SUBCELL_GROUP_H

#include <stdint.h>

static inline bool t3dHlodSubcellMaskComplete( uint8_t nExpectedMask, uint8_t nReadyMask )
{
    // Empty slots do not need resources. Every authored quadrant must be ready.
    return nExpectedMask != 0u && ( nExpectedMask & nReadyMask ) == nExpectedMask;
}

#endif // TERRAIN3D_HLOD_SUBCELL_GROUP_H
