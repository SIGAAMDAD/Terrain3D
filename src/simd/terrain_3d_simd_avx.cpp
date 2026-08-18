// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// This translation unit must be compiled with AVX and F16C enabled. Runtime
// dispatch guarantees it is never entered on unsupported CPUs or OS contexts.
#include "terrain_3d_simd_internal.h"

#include "terrain_3d_half_buffer.h"
#include "terrain_3d_packed_transform.h"

#if defined( TERRAIN3D_ENABLE_AVX_BACKEND ) && ( ( defined( __AVX__ ) && defined( __F16C__ ) ) || ( defined( _MSC_VER ) && ( defined( _M_X64 ) || defined( _M_IX86 ) ) ) )
#include <immintrin.h>
#define TERRAIN3D_COMPILED_AVX_F16C 1
#else
#define TERRAIN3D_COMPILED_AVX_F16C 0
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace terrain3d_simd::detail {

#if TERRAIN3D_COMPILED_AVX_F16C
namespace {

inline __m256 decode8( const uint16_t *pSource ) {
	return _mm256_cvtph_ps( _mm_loadu_si128( ( const __m128i * ) ( pSource ) ) );
}

inline void encodeNormal8( const __m256 argX, const __m256 argZ, uint8_t *pDestination ) {
	const __m256 scale = _mm256_set1_ps( TERRAIN3D_BYTE_MIDPOINT_FLOAT );
	const __m256 half = _mm256_set1_ps( 0.5f );
	const __m256 zero = _mm256_setzero_ps();
	const __m256 max255 = _mm256_set1_ps( TERRAIN3D_BYTE_MAX_FLOAT );
	const __m256 one = _mm256_set1_ps( 1.0f );
	const __m256 x = _mm256_min_ps( max255,
		_mm256_max_ps( zero, _mm256_add_ps( _mm256_mul_ps( _mm256_add_ps( argX, one ), scale ), half ) ) );
	const __m256 z = _mm256_min_ps( max255,
		_mm256_max_ps( zero, _mm256_add_ps( _mm256_mul_ps( _mm256_add_ps( argZ, one ), scale ), half ) ) );
	alignas( 32 ) int32_t xi[8];
	alignas( 32 ) int32_t zi[8];
	_mm256_store_si256( ( __m256i * ) ( xi ), _mm256_cvttps_epi32( x ) );
	_mm256_store_si256( ( __m256i * ) ( zi ), _mm256_cvttps_epi32( z ) );
	for ( int lane = 0; lane < 8; ++lane ) {
		pDestination[lane * 2] = ( uint8_t ) ( xi[lane] );
		pDestination[lane * 2 + 1] = ( uint8_t ) ( zi[lane] );
	}
}

inline void scalarNormalPixel(
	const float *pCenterRow, const float *pDownRow, const int iX,
	const float fSpacing, uint8_t *pDestination
) {
	const float fDx = pCenterRow[iX] - pCenterRow[iX + 1];
	const float fDz = pCenterRow[iX] - pDownRow[iX];
	const float fLength = std::sqrt( fDx * fDx + fSpacing * fSpacing + fDz * fDz );
	const float fInverse = fLength > 0.0f ? 1.0f / fLength : 0.0f;
	const float fNx = std::max( 0.0f, std::min( TERRAIN3D_BYTE_MAX_FLOAT, ( fDx * fInverse * 0.5f + 0.5f ) * TERRAIN3D_BYTE_MAX_FLOAT ) );
	const float fNz = std::max( 0.0f, std::min( TERRAIN3D_BYTE_MAX_FLOAT, ( fDz * fInverse * 0.5f + 0.5f ) * TERRAIN3D_BYTE_MAX_FLOAT ) );
	pDestination[0] = ( uint8_t ) ( std::floor( fNx + 0.5f ) );
	pDestination[1] = ( uint8_t ) ( std::floor( fNz + 0.5f ) );
}

inline void writeTransform(
	const t3dpackedTransform_t &rTransform, const uint32_t nColor, const int nStride,
	float *pDestination
) {
	alignas( 32 ) float fBasis[8];
	_mm256_store_ps( fBasis, decode8( rTransform.basis ) );
	pDestination[0] = fBasis[0];
	pDestination[1] = fBasis[1];
	pDestination[2] = fBasis[2];
	pDestination[3] = rTransform.origin[0];
	pDestination[4] = fBasis[3];
	pDestination[5] = fBasis[4];
	pDestination[6] = fBasis[5];
	pDestination[7] = rTransform.origin[1];
	pDestination[8] = fBasis[6];
	pDestination[9] = fBasis[7];
	pDestination[10] = terrain3d_half::decode( rTransform.basis[8] );
	pDestination[11] = rTransform.origin[2];
	if ( nStride == 16 ) {
		pDestination[12] = ( float ) ( ( nColor >> 24u ) & 0xffu ) * ( TERRAIN3D_BYTE_INV_MAX_FLOAT );
		pDestination[13] = ( float ) ( ( nColor >> 16u ) & 0xffu ) * ( TERRAIN3D_BYTE_INV_MAX_FLOAT );
		pDestination[14] = ( float ) ( ( nColor >> 8u ) & 0xffu ) * ( TERRAIN3D_BYTE_INV_MAX_FLOAT );
		pDestination[15] = ( float ) ( nColor & 0xffu ) * ( TERRAIN3D_BYTE_INV_MAX_FLOAT );
	}
}

} // namespace
#endif

bool avxBackendAvailableAtBuild( void ) {
	return TERRAIN3D_COMPILED_AVX_F16C != 0;
}

void avxHalfToFloat( const uint16_t *pSource, float *pDestination, const size_t nCount ) {
#if TERRAIN3D_COMPILED_AVX_F16C
	size_t index = 0u;
	for ( ; nCount - index >= 8u; index += 8u ) {
		_mm256_storeu_ps( pDestination + index, decode8( pSource + index ) );
	}
	for ( ; index < nCount; ++index ) {
		pDestination[index] = terrain3d_half::decode( pSource[index] );
	}
	_mm256_zeroupper();
#else
	scalarHalfToFloat( pSource, pDestination, nCount );
#endif
}

void avxPackedTransforms(
	const t3dpackedTransform_t *pTransforms, const uint32_t *pColors,
	const size_t nCount, const int nStride, float *pDestination
) {
#if TERRAIN3D_COMPILED_AVX_F16C
	for ( size_t i = 0u; i < nCount; ++i ) {
		writeTransform( pTransforms[i], pColors ? pColors[i] : 0xffffffffu, nStride,
			pDestination + i * ( size_t ) ( nStride ) );
	}
	_mm256_zeroupper();
#else
	scalarPackedTransforms( pTransforms, pColors, nCount, nStride, pDestination );
#endif
}

void avxDerivedNormals(
	const float *pHalo, const int nWidth, const int nHeight, const int nHaloStride,
	const float fSpacing, uint8_t *pDestination
) {
#if TERRAIN3D_COMPILED_AVX_F16C
	const __m256 spacing_sq = _mm256_set1_ps( fSpacing * fSpacing );
	const __m256 one = _mm256_set1_ps( 1.0f );
	for ( int y = 0; y < nHeight; ++y ) {
		const float *pCenterRow = pHalo + ( size_t ) ( y + 1 ) * ( size_t ) ( nHaloStride ) + 1u;
		const float *pDownRow = pCenterRow + nHaloStride;
		uint8_t *pOut = pDestination + ( size_t ) ( y ) * ( size_t ) ( nWidth ) * 2u;
		int x = 0;
		for ( ; nWidth - x >= 8; x += 8 ) {
			const __m256 center = _mm256_loadu_ps( pCenterRow + x );
			const __m256 dx = _mm256_sub_ps( center, _mm256_loadu_ps( pCenterRow + x + 1 ) );
			const __m256 dz = _mm256_sub_ps( center, _mm256_loadu_ps( pDownRow + x ) );
			const __m256 length = _mm256_sqrt_ps(
				_mm256_add_ps( spacing_sq, _mm256_add_ps( _mm256_mul_ps( dx, dx ), _mm256_mul_ps( dz, dz ) ) ) );
			const __m256 inverse = _mm256_div_ps( one, length );
			encodeNormal8( _mm256_mul_ps( dx, inverse ), _mm256_mul_ps( dz, inverse ), pOut + ( size_t ) ( x ) * 2u );
		}
		for ( ; x < nWidth; ++x ) {
			scalarNormalPixel( pCenterRow, pDownRow, x, fSpacing, pOut + ( size_t ) ( x ) * 2u );
		}
	}
	_mm256_zeroupper();
#else
	scalarDerivedNormals( pHalo, nWidth, nHeight, nHaloStride, fSpacing, pDestination );
#endif
}


void avxUpdateCumulativeErosion(
	const uint8_t *pActive, const float *pInitialHeight, const float *pHeight,
	float *pCumulativeErosion, const size_t nCount
) {
#if TERRAIN3D_COMPILED_AVX_F16C
	const __m256 zero = _mm256_setzero_ps();
	const __m128i all_ones = _mm_set1_epi32( -1 );
	size_t index = 0u;
	for ( ; nCount - index >= 8u; index += 8u ) {
		uint64_t active_bits = 0u;
		std::memcpy( &active_bits, pActive + index, sizeof( active_bits ) );
		const __m128i packed = _mm_cvtsi64_si128( ( long long ) ( active_bits ) );
		const __m128i active16 = _mm_unpacklo_epi8( packed, _mm_setzero_si128() );
		const __m128i active_lo = _mm_unpacklo_epi16( active16, _mm_setzero_si128() );
		const __m128i active_hi = _mm_unpackhi_epi16( active16, _mm_setzero_si128() );
		const __m128 lo_mask = _mm_castsi128_ps( _mm_xor_si128( _mm_cmpeq_epi32( active_lo, _mm_setzero_si128() ), all_ones ) );
		const __m128 hi_mask = _mm_castsi128_ps( _mm_xor_si128( _mm_cmpeq_epi32( active_hi, _mm_setzero_si128() ), all_ones ) );
		__m256 mask = _mm256_castps128_ps256( lo_mask );
		mask = _mm256_insertf128_ps( mask, hi_mask, 1 );
		const __m256 raw_delta =
			_mm256_sub_ps( _mm256_loadu_ps( pInitialHeight + index ), _mm256_loadu_ps( pHeight + index ) );
		const __m256 delta = _mm256_and_ps( _mm256_cmp_ps( raw_delta, zero, _CMP_GT_OQ ), raw_delta );
		const __m256 current = _mm256_loadu_ps( pCumulativeErosion + index );
		const __m256 updated =
			_mm256_blendv_ps( current, delta, _mm256_cmp_ps( current, delta, _CMP_LT_OQ ) );
		_mm256_storeu_ps( pCumulativeErosion + index, _mm256_blendv_ps( current, updated, mask ) );
	}
	sse2UpdateCumulativeErosion( pActive + index, pInitialHeight + index, pHeight + index,
		pCumulativeErosion + index, nCount - index );
#else
	scalarUpdateCumulativeErosion( pActive, pInitialHeight, pHeight, pCumulativeErosion, nCount );
#endif
}

} // namespace terrain3d_simd::detail
