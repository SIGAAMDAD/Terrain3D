#pragma once

#ifndef TERRAIN3D_SIMD_INTERNAL_H
#define TERRAIN3D_SIMD_INTERNAL_H

#include "core/terrain_3d_numeric_constants.h"
#include "simd/terrain_3d_simd.h"

void t3dSimdScalarHalfToFloat( const uint16_t *, float *, size_t );
void t3dSimdScalarPackedTransforms( const t3dPackedTransform_t *, const uint32_t *, size_t, int, float * );
void t3dSimdScalarDerivedNormals( const float *, int, int, int, float, uint8_t * );
void t3dSimdScalarUpdateCumulativeErosion( const uint8_t *, const float *, const float *, float *, size_t );

bool t3dSimdSse2BackendAvailableAtBuild( void );
void t3dSimdSse2HalfToFloat( const uint16_t *, float *, size_t );
void t3dSimdSse2PackedTransforms( const t3dPackedTransform_t *, const uint32_t *, size_t, int, float * );
void t3dSimdSse2DerivedNormals( const float *, int, int, int, float, uint8_t * );
void t3dSimdSse2UpdateCumulativeErosion( const uint8_t *, const float *, const float *, float *, size_t );

bool t3dSimdAvxBackendAvailableAtBuild( void );
void t3dSimdAvxHalfToFloat( const uint16_t *, float *, size_t );
void t3dSimdAvxPackedTransforms( const t3dPackedTransform_t *, const uint32_t *, size_t, int, float * );
void t3dSimdAvxDerivedNormals( const float *, int, int, int, float, uint8_t * );
void t3dSimdAvxUpdateCumulativeErosion( const uint8_t *, const float *, const float *, float *, size_t );

bool t3dSimdAvx2BackendAvailableAtBuild( void );
void t3dSimdAvx2HalfToFloat( const uint16_t *, float *, size_t );
void t3dSimdAvx2PackedTransforms( const t3dPackedTransform_t *, const uint32_t *, size_t, int, float * );
void t3dSimdAvx2DerivedNormals( const float *, int, int, int, float, uint8_t * );
void t3dSimdAvx2UpdateCumulativeErosion( const uint8_t *, const float *, const float *, float *, size_t );

bool t3dSimdNeonBackendAvailableAtBuild( void );
void t3dSimdNeonHalfToFloat( const uint16_t *, float *, size_t );
void t3dSimdNeonPackedTransforms( const t3dPackedTransform_t *, const uint32_t *, size_t, int, float * );
void t3dSimdNeonDerivedNormals( const float *, int, int, int, float, uint8_t * );
void t3dSimdNeonUpdateCumulativeErosion( const uint8_t *, const float *, const float *, float *, size_t );

#endif // TERRAIN3D_SIMD_INTERNAL_H
