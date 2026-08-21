// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// AVX2/F16C backend. Kept in a separate translation unit so the baseline binary
// remains executable on pre-AVX hardware.
#include "core/terrain_3d_compiler.h"
#include "data/terrain_3d_half_buffer.h"
#include "data/terrain_3d_packed_transform.h"
#include "simd/terrain_3d_simd_internal.h"

#if defined( TERRAIN3D_ENABLE_AVX2_BACKEND ) && \
		( ( defined( __AVX2__ ) && defined( __F16C__ ) ) || \
				( defined( _MSC_VER ) && ( defined( _M_X64 ) || defined( _M_IX86 ) ) ) )
#include <immintrin.h>
#define TERRAIN3D_COMPILED_AVX2_F16C 1
#else
#define TERRAIN3D_COMPILED_AVX2_F16C 0
#endif

#include <algorithm>
#include <cmath>

#if TERRAIN3D_COMPILED_AVX2_F16C
EA_FORCE_INLINE static __m256 t3dSimdAvx2Decode8( const uint16_t *pSource ) {
	return _mm256_cvtph_ps( _mm_loadu_si128( ( const __m128i * )( pSource ) ) );
}

static inline void t3dSimdAvx2EncodeNormal8( const __m256 argX, const __m256 argZ, uint8_t *pDestination ) {
	const __m256 scale = _mm256_set1_ps( s_fByteMidpointFloat );
	const __m256 half = _mm256_set1_ps( 0.5f );
	const __m256 zero = _mm256_setzero_ps();
	const __m256 max255 = _mm256_set1_ps( s_fByteMaxFloat );
	const __m256 one = _mm256_set1_ps( 1.0f );
	const __m256 x = _mm256_min_ps(
			max255, _mm256_max_ps( zero, _mm256_add_ps( _mm256_mul_ps( _mm256_add_ps( argX, one ), scale ), half ) ) );
	const __m256 z = _mm256_min_ps(
			max255, _mm256_max_ps( zero, _mm256_add_ps( _mm256_mul_ps( _mm256_add_ps( argZ, one ), scale ), half ) ) );
	const __m256i xi = _mm256_cvttps_epi32( x );
	const __m256i zi = _mm256_cvttps_epi32( z );
	alignas( 32 ) int32_t x_values[8];
	alignas( 32 ) int32_t z_values[8];
	_mm256_store_si256( ( __m256i * )( x_values ), xi );
	_mm256_store_si256( ( __m256i * )( z_values ), zi );
	for ( int lane = 0; lane < 8; ++lane ) {
		pDestination[lane * 2] = ( uint8_t )( x_values[lane] );
		pDestination[lane * 2 + 1] = ( uint8_t )( z_values[lane] );
	}
}

static inline void t3dSimdAvx2ScalarNormalPixel( const float *pCenterRow,
		const float *pDownRow,
		const int iX,
		const float fSpacing,
		uint8_t *pDestination ) {
	const float fDx = pCenterRow[iX] - pCenterRow[iX + 1];
	const float fDz = pCenterRow[iX] - pDownRow[iX];
	const float fLength = std::sqrt( fDx * fDx + fSpacing * fSpacing + fDz * fDz );
	const float fInverse = fLength > 0.0f ? 1.0f / fLength : 0.0f;
	pDestination[0] = ( uint8_t )( std::floor(
			std::max( 0.0f, std::min( s_fByteMaxFloat, ( fDx * fInverse + 1.0f ) * s_fByteMidpointFloat ) ) + 0.5f ) );
	pDestination[1] = ( uint8_t )( std::floor(
			std::max( 0.0f, std::min( s_fByteMaxFloat, ( fDz * fInverse + 1.0f ) * s_fByteMidpointFloat ) ) + 0.5f ) );
}
}
#endif

bool t3dSimdAvx2BackendAvailableAtBuild( void ) {
	return TERRAIN3D_COMPILED_AVX2_F16C != 0;
}

void t3dSimdAvx2HalfToFloat( const uint16_t *pSource, float *pDestination, const size_t nCount ) {
#if TERRAIN3D_COMPILED_AVX2_F16C
	size_t index = 0u;
	for ( ; nCount - index >= 8u; index += 8u ) {
		_mm256_storeu_ps( pDestination + index, t3dSimdAvx2Decode8( pSource + index ) );
	}
	for ( ; index < nCount; ++index ) {
		pDestination[index] = t3dHalfDecode( pSource[index] );
	}
	_mm256_zeroupper();
#else
	t3dSimdAvxHalfToFloat( pSource, pDestination, nCount );
#endif
}

void t3dSimdAvx2PackedTransforms( const t3dPackedTransform_t *pTransforms,
		const uint32_t *pColors,
		const size_t nCount,
		const int nStride,
		float *pDestination ) {
#if TERRAIN3D_COMPILED_AVX2_F16C
	for ( size_t i = 0u; i < nCount; ++i ) {
		const t3dPackedTransform_t &t = pTransforms[i];
		float *pOut = pDestination + i * ( size_t )( nStride );
		alignas( 32 ) float fBasis[8];
		_mm256_store_ps( fBasis, t3dSimdAvx2Decode8( t.m_szBasis ) );
		pOut[0] = fBasis[0];
		pOut[1] = fBasis[1];
		pOut[2] = fBasis[2];
		pOut[3] = t.m_szOrigin[0];
		pOut[4] = fBasis[3];
		pOut[5] = fBasis[4];
		pOut[6] = fBasis[5];
		pOut[7] = t.m_szOrigin[1];
		pOut[8] = fBasis[6];
		pOut[9] = fBasis[7];
		pOut[10] = t3dHalfDecode( t.m_szBasis[8] );
		pOut[11] = t.m_szOrigin[2];
		if ( nStride == 16 ) {
			const uint32_t color = pColors ? pColors[i] : 0xffffffffu;
			const __m128i bytes = _mm_cvtsi32_si128( ( int )( color ) );
			const __m128 floats = _mm_cvtepi32_ps( _mm_cvtepu8_epi32( bytes ) );
			alignas( 16 ) float fRgbaLe[4];
			_mm_store_ps( fRgbaLe, _mm_mul_ps( floats, _mm_set1_ps( s_fByteInvMaxFloat ) ) );
			pOut[12] = fRgbaLe[3];
			pOut[13] = fRgbaLe[2];
			pOut[14] = fRgbaLe[1];
			pOut[15] = fRgbaLe[0];
		}
	}
	_mm256_zeroupper();
#else
	t3dSimdAvxPackedTransforms( pTransforms, pColors, nCount, nStride, pDestination );
#endif
}

void t3dSimdAvx2DerivedNormals( const float *pHalo,
		const int nWidth,
		const int nHeight,
		const int nHaloStride,
		const float fSpacing,
		uint8_t *pDestination ) {
#if TERRAIN3D_COMPILED_AVX2_F16C
	const __m256 spacing_sq = _mm256_set1_ps( fSpacing * fSpacing );
	const __m256 one = _mm256_set1_ps( 1.0f );
	for ( int y = 0; y < nHeight; ++y ) {
		const float *pCenterRow = pHalo + ( size_t )( y + 1 ) * ( size_t )( nHaloStride ) + 1u;
		const float *pDownRow = pCenterRow + nHaloStride;
		uint8_t *pOut = pDestination + ( size_t )( y ) * ( size_t )( nWidth ) * 2u;
		int x = 0;
		for ( ; nWidth - x >= 8; x += 8 ) {
			const __m256 center = _mm256_loadu_ps( pCenterRow + x );
			const __m256 dx = _mm256_sub_ps( center, _mm256_loadu_ps( pCenterRow + x + 1 ) );
			const __m256 dz = _mm256_sub_ps( center, _mm256_loadu_ps( pDownRow + x ) );
			const __m256 length = _mm256_sqrt_ps(
					_mm256_add_ps( spacing_sq, _mm256_add_ps( _mm256_mul_ps( dx, dx ), _mm256_mul_ps( dz, dz ) ) ) );
			const __m256 inverse = _mm256_div_ps( one, length );
			t3dSimdAvx2EncodeNormal8(
					_mm256_mul_ps( dx, inverse ), _mm256_mul_ps( dz, inverse ), pOut + ( size_t )( x ) * 2u );
		}
		for ( ; x < nWidth; ++x ) {
			t3dSimdAvx2ScalarNormalPixel( pCenterRow, pDownRow, x, fSpacing, pOut + ( size_t )( x ) * 2u );
		}
	}
	_mm256_zeroupper();
#else
	t3dSimdAvxDerivedNormals( pHalo, nWidth, nHeight, nHaloStride, fSpacing, pDestination );
#endif
}

void t3dSimdAvx2UpdateCumulativeErosion( const uint8_t *pActive,
		const float *pInitialHeight,
		const float *pHeight,
		float *pCumulativeErosion,
		const size_t nCount ) {
#if TERRAIN3D_COMPILED_AVX2_F16C
	const __m256 zero = _mm256_setzero_ps();
	size_t index = 0u;
	for ( ; nCount - index >= 8u; index += 8u ) {
		const __m128i active8 = _mm_loadl_epi64( ( const __m128i * )( pActive + index ) );
		const __m256i active32 = _mm256_cvtepu8_epi32( active8 );
		const __m256 mask = _mm256_castsi256_ps( _mm256_cmpgt_epi32( active32, _mm256_setzero_si256() ) );
		const __m256 raw_delta =
				_mm256_sub_ps( _mm256_loadu_ps( pInitialHeight + index ), _mm256_loadu_ps( pHeight + index ) );
		const __m256 delta = _mm256_and_ps( _mm256_cmp_ps( raw_delta, zero, _CMP_GT_OQ ), raw_delta );
		const __m256 current = _mm256_loadu_ps( pCumulativeErosion + index );
		const __m256 updated = _mm256_blendv_ps( current, delta, _mm256_cmp_ps( current, delta, _CMP_LT_OQ ) );
		_mm256_storeu_ps( pCumulativeErosion + index, _mm256_blendv_ps( current, updated, mask ) );
	}
	t3dSimdAvxUpdateCumulativeErosion(
			pActive + index, pInitialHeight + index, pHeight + index, pCumulativeErosion + index, nCount - index );
#else
	t3dSimdScalarUpdateCumulativeErosion( pActive, pInitialHeight, pHeight, pCumulativeErosion, nCount );
#endif
}
