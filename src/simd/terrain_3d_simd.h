// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Runtime-dispatched SIMD kernels. The baseline binary remains safe on old CPUs;
// wider instruction sets are isolated in separately compiled translation units.
#ifndef TERRAIN3D_SIMD_H
#define TERRAIN3D_SIMD_H

#include <stddef.h>
#include <stdint.h>

typedef struct t3dPackedTransform_s t3dPackedTransform_t;

typedef enum t3dSimdBackend_e : uint8_t {
	T3D_SIMD_SCALAR = 0,
	T3D_SIMD_SSE2,
	T3D_SIMD_AVX_F16C,
	T3D_SIMD_AVX2_F16C,
	T3D_SIMD_NEON,
} t3dSimdBackend_t;

typedef struct t3dSimdCapabilities_s {
	bool m_bSse2 = false;
	bool m_bAvx = false;
	bool m_bF16c = false;
	bool m_bAvx2 = false;
	bool m_bNeon = false;
} t3dSimdCapabilities_t;

typedef struct t3dSimdDispatch_s {
	t3dSimdBackend_t m_backend = T3D_SIMD_SCALAR;
	void ( *m_pPfnHalfToFloat )( const uint16_t *, float *, size_t ) = NULL;
	void ( *m_pPfnPackedTransformsToMultimesh )( const t3dPackedTransform_t *, const uint32_t *, size_t, int, float * ) = NULL;
	void ( *m_pPfnDerivedNormalsRg8 )( const float *, int, int, int, float, uint8_t * ) = NULL;
	void ( *m_pPfnUpdateCumulativeErosion )( const uint8_t *, const float *, const float *, float *, size_t ) = NULL;
} t3dSimdDispatch_t;

const t3dSimdCapabilities_t &t3dSimdGetCapabilities( void );
const t3dSimdDispatch_t &t3dSimdGetDispatch( void );
t3dSimdBackend_t t3dSimdGetActiveBackend( void );
const char *t3dSimdGetBackendName( t3dSimdBackend_t backend );

void t3dSimdHalfToFloat( const uint16_t *pSource, float *pDestination, size_t nCount );
void t3dSimdPackedTransformsToMultimesh(
        const t3dPackedTransform_t *pTransforms, const uint32_t *pColors, size_t nCount, int nStride, float *pDestination );
void t3dSimdDerivedNormalsRg8( const float *pHalo, int nWidth, int nHeight, int nHaloStride, float fSpacing, uint8_t *pDestination );
void t3dSimdUpdateCumulativeErosion(
        const uint8_t *pActive, const float *pInitialHeight, const float *pHeight, float *pCumulativeErosion, size_t nCount );

// Test-only override. Unsupported backends are rejected; CPU and OS feature
// checks remain active. The scalar backend is always accepted.
bool t3dSimdSetBackendOverrideForTests( t3dSimdBackend_t backend );
void t3dSimdClearBackendOverrideForTests( void );

#endif // TERRAIN3D_SIMD_H
