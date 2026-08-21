#pragma once

#ifndef TERRAIN3D_SIMD_INTERNAL_H
#define TERRAIN3D_SIMD_INTERNAL_H

#include "core/terrain_3d_numeric_constants.h"
#include "simd/terrain_3d_simd.h"

namespace terrain3d_simd::detail {

void scalarHalfToFloat( const uint16_t *, float *, size_t );
void scalarPackedTransforms( const t3dPackedTransform_t *, const uint32_t *, size_t, int, float * );
void scalarDerivedNormals( const float *, int, int, int, float, uint8_t * );
void scalarUpdateCumulativeErosion( const uint8_t *, const float *, const float *, float *, size_t );

bool sse2BackendAvailableAtBuild( void );
void sse2HalfToFloat( const uint16_t *, float *, size_t );
void sse2PackedTransforms( const t3dPackedTransform_t *, const uint32_t *, size_t, int, float * );
void sse2DerivedNormals( const float *, int, int, int, float, uint8_t * );
void sse2UpdateCumulativeErosion( const uint8_t *, const float *, const float *, float *, size_t );

bool avxBackendAvailableAtBuild( void );
void avxHalfToFloat( const uint16_t *, float *, size_t );
void avxPackedTransforms( const t3dPackedTransform_t *, const uint32_t *, size_t, int, float * );
void avxDerivedNormals( const float *, int, int, int, float, uint8_t * );
void avxUpdateCumulativeErosion( const uint8_t *, const float *, const float *, float *, size_t );

bool avx2BackendAvailableAtBuild( void );
void avx2HalfToFloat( const uint16_t *, float *, size_t );
void avx2PackedTransforms( const t3dPackedTransform_t *, const uint32_t *, size_t, int, float * );
void avx2DerivedNormals( const float *, int, int, int, float, uint8_t * );
void avx2UpdateCumulativeErosion( const uint8_t *, const float *, const float *, float *, size_t );

bool neonBackendAvailableAtBuild( void );
void neonHalfToFloat( const uint16_t *, float *, size_t );
void neonPackedTransforms( const t3dPackedTransform_t *, const uint32_t *, size_t, int, float * );
void neonDerivedNormals( const float *, int, int, int, float, uint8_t * );
void neonUpdateCumulativeErosion( const uint8_t *, const float *, const float *, float *, size_t );

} // namespace terrain3d_simd::detail

#endif // TERRAIN3D_SIMD_INTERNAL_H
