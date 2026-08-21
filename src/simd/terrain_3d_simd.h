// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Runtime-dispatched SIMD kernels. The baseline binary remains safe on old CPUs;
// wider instruction sets are isolated in separately compiled translation units.
#pragma once

#ifndef TERRAIN3D_SIMD_H
#define TERRAIN3D_SIMD_H

#include <cstddef>
#include <cstdint>

typedef struct t3dPackedTransform_s t3dPackedTransform_t;

namespace terrain3d_simd {

typedef enum t3dBackend_e : uint8_t {
	SCALAR = 0,
	SSE2,
	AVX_F16C,
	AVX2_F16C,
	NEON,
} t3dBackend_t;

typedef struct t3dCapabilities_s {
	bool bSse2 = false;
	bool bAvx = false;
	bool bF16c = false;
	bool bAvx2 = false;
	bool bNeon = false;
} t3dCapabilities_t;

typedef struct t3dDispatch_s {
	t3dBackend_t backend = t3dBackend_t::SCALAR;
	void ( *pfnHalfToFloat )( const uint16_t *, float *, size_t ) = nullptr;
	void ( *pfnPackedTransformsToMultimesh )( const t3dPackedTransform_t *, const uint32_t *, size_t, int, float * ) =
		nullptr;
	void ( *pfnDerivedNormalsRg8 )( const float *, int, int, int, float, uint8_t * ) = nullptr;
	void ( *pfnUpdateCumulativeErosion )( const uint8_t *, const float *, const float *, float *, size_t ) = nullptr;
} t3dDispatch_t;

const t3dCapabilities_t &capabilities( void );
const t3dDispatch_t &dispatch( void );
t3dBackend_t activeBackend( void );
const char *backendName( t3dBackend_t backend );

void halfToFloat( const uint16_t *pSource, float *pDestination, size_t nCount );
void packedTransformsToMultimesh(
	const t3dPackedTransform_t *pTransforms, const uint32_t *pColors,
	size_t nCount, int nStride, float *pDestination
);
void derivedNormalsRg8(
	const float *pHalo, int nWidth, int nHeight, int nHaloStride, float fSpacing,
	uint8_t *pDestination
);
void updateCumulativeErosion(
	const uint8_t *pActive, const float *pInitialHeight, const float *pHeight,
	float *pCumulativeErosion, size_t nCount
);

// Test-only override. It rejects unsupported backends and does not bypass CPU or
// operating-system feature checks. Passing SCALAR always succeeds.
bool setBackendOverrideForTests( t3dBackend_t backend );
void clearBackendOverrideForTests( void );

} // namespace terrain3d_simd

#endif // TERRAIN3D_SIMD_H
