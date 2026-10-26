// Copyright (c) 2023-2026 Terrain3D contributors.
// Reusable, allocation-free spatial key for 64m HLOD instance culling cells.
#ifndef TERRAIN3D_HLOD_SPATIAL_BUCKET_H
#define TERRAIN3D_HLOD_SPATIAL_BUCKET_H

#include <math.h>
#include <stdint.h>

static inline int16_t t3dHlodSpatialBucket( const float fLocalMetres )
{
    const float nBucket = floorf( fLocalMetres * ( 1.0f / 64.0f ) );
    if ( !( nBucket >= -32768.0f ) ) { return INT16_MIN; }
    if ( nBucket >= 32767.0f ) { return INT16_MAX; }
    return ( int16_t ) nBucket;
}

#endif // TERRAIN3D_HLOD_SPATIAL_BUCKET_H
