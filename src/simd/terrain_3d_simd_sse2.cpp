// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "core/terrain_3d_compiler.h"
#include "simd/terrain_3d_simd_internal.h"

#include "data/terrain_3d_half_buffer.h"
#include "data/terrain_3d_packed_transform.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined( TERRAIN3D_ENABLE_SSE2_BACKEND ) && ( defined( __SSE2__ ) || defined( _M_X64 ) || ( defined( _M_IX86_FP ) && _M_IX86_FP >= 2 ) )
#include <emmintrin.h>
#define TERRAIN3D_COMPILED_SSE2 1
#else
#define TERRAIN3D_COMPILED_SSE2 0
#endif

namespace terrain3d_simd::detail {

#if TERRAIN3D_COMPILED_SSE2
namespace {

inline __m128 decode4NormalHalf( const uint16_t *pSource, bool &bHasSpecial ) {
	const __m128i packed16 = _mm_loadl_epi64( ( const __m128i * ) ( pSource ) );
	const __m128i half32 = _mm_unpacklo_epi16( packed16, _mm_setzero_si128() );
	const __m128i exponent = _mm_and_si128( half32, _mm_set1_epi32( 0x7c00 ) );
	const __m128i exp_zero = _mm_cmpeq_epi32( exponent, _mm_setzero_si128() );
	const __m128i exp_max = _mm_cmpeq_epi32( exponent, _mm_set1_epi32( 0x7c00 ) );
	bHasSpecial = _mm_movemask_ps( _mm_castsi128_ps( _mm_or_si128( exp_zero, exp_max ) ) ) != 0;
	const __m128i sign = _mm_slli_epi32( _mm_and_si128( half32, _mm_set1_epi32( 0x8000 ) ), 16 );
	const __m128i magnitude = _mm_slli_epi32( _mm_and_si128( half32, _mm_set1_epi32( 0x7fff ) ), 13 );
	const __m128i bias = _mm_set1_epi32( ( 127 - 15 ) << 23 );
	return _mm_castsi128_ps( _mm_or_si128( sign, _mm_add_epi32( magnitude, bias ) ) );
}

inline __m128 decode4Exact( const uint16_t *pSource ) {
	bool bSpecial = false;
	const __m128 result = decode4NormalHalf( pSource, bSpecial );
	if ( !bSpecial ) {
		return result;
	}
	alignas( 16 ) float fValues[4];
	for ( int lane = 0; lane < 4; ++lane ) {
		fValues[lane] = terrain3d_half::decode( pSource[lane] );
	}
	return _mm_load_ps( fValues );
}

inline void encodeNormal4( __m128 x, __m128 z, uint8_t *pDestination ) {
	const __m128 scale = _mm_set1_ps( TERRAIN3D_BYTE_MIDPOINT_FLOAT );
	const __m128 half = _mm_set1_ps( 0.5f );
	const __m128 zero = _mm_setzero_ps();
	const __m128 max255 = _mm_set1_ps( TERRAIN3D_BYTE_MAX_FLOAT );
	x = _mm_min_ps( max255, _mm_max_ps( zero, _mm_add_ps( _mm_mul_ps( _mm_add_ps( x, _mm_set1_ps( 1.0f ) ), scale ), half ) ) );
	z = _mm_min_ps( max255, _mm_max_ps( zero, _mm_add_ps( _mm_mul_ps( _mm_add_ps( z, _mm_set1_ps( 1.0f ) ), scale ), half ) ) );
	alignas( 16 ) int32_t xi[4];
	alignas( 16 ) int32_t zi[4];
	_mm_store_si128( ( __m128i * ) ( xi ), _mm_cvttps_epi32( x ) );
	_mm_store_si128( ( __m128i * ) ( zi ), _mm_cvttps_epi32( z ) );
	for ( int lane = 0; lane < 4; ++lane ) {
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
	pDestination[0] = ( uint8_t ) ( std::floor( std::max( 0.0f, std::min( TERRAIN3D_BYTE_MAX_FLOAT, ( fDx * fInverse + 1.0f ) * TERRAIN3D_BYTE_MIDPOINT_FLOAT ) ) + 0.5f ) );
	pDestination[1] = ( uint8_t ) ( std::floor( std::max( 0.0f, std::min( TERRAIN3D_BYTE_MAX_FLOAT, ( fDz * fInverse + 1.0f ) * TERRAIN3D_BYTE_MIDPOINT_FLOAT ) ) + 0.5f ) );
}

inline void writeTransform(
	const t3dpackedTransform_t &rTransform, const uint32_t nColor, const int nStride,
	float *pDestination
) {
	EA_FORCE_INLINE alignas( 16 ) float fBasis[12] = {};
	_mm_store_ps( fBasis, decode4Exact( rTransform.basis ) );
	_mm_store_ps( fBasis + 4, decode4Exact( rTransform.basis + 4 ) );
	fBasis[8] = terrain3d_half::decode( rTransform.basis[8] );
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
	pDestination[10] = fBasis[8];
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

bool sse2BackendAvailableAtBuild( void ) {
	return TERRAIN3D_COMPILED_SSE2 != 0;
}

void sse2HalfToFloat( const uint16_t *pSource, float *pDestination, const size_t nCount ) {
#if TERRAIN3D_COMPILED_SSE2
	size_t index = 0u;
	for ( ; nCount - index >= 4u; index += 4u ) {
		_mm_storeu_ps( pDestination + index, decode4Exact( pSource + index ) );
	}
	for ( ; index < nCount; ++index ) {
		pDestination[index] = terrain3d_half::decode( pSource[index] );
	}
#else
	scalarHalfToFloat( pSource, pDestination, nCount );
#endif
}

void sse2PackedTransforms(
	const t3dpackedTransform_t *pTransforms, const uint32_t *pColors,
	const size_t nCount, const int nStride, float *pDestination
) {
#if TERRAIN3D_COMPILED_SSE2
	for ( size_t i = 0u; i < nCount; ++i ) {
		writeTransform( pTransforms[i], pColors ? pColors[i] : 0xffffffffu, nStride,
			pDestination + i * ( size_t ) ( nStride ) );
	}
#else
	scalarPackedTransforms( pTransforms, pColors, nCount, nStride, pDestination );
#endif
}

void sse2DerivedNormals(
	const float *pHalo, const int nWidth, const int nHeight, const int nHaloStride,
	const float fSpacing, uint8_t *pDestination
) {
#if TERRAIN3D_COMPILED_SSE2
	const __m128 spacing_sq = _mm_set1_ps( fSpacing * fSpacing );
	for ( int y = 0; y < nHeight; ++y ) {
		const float *pCenterRow = pHalo + ( size_t ) ( y + 1 ) * ( size_t ) ( nHaloStride ) + 1u;
		const float *pDownRow = pCenterRow + nHaloStride;
		uint8_t *pOut = pDestination + ( size_t ) ( y ) * ( size_t ) ( nWidth ) * 2u;
		int x = 0;
		for ( ; nWidth - x >= 4; x += 4 ) {
			const __m128 center = _mm_loadu_ps( pCenterRow + x );
			const __m128 dx = _mm_sub_ps( center, _mm_loadu_ps( pCenterRow + x + 1 ) );
			const __m128 dz = _mm_sub_ps( center, _mm_loadu_ps( pDownRow + x ) );
			const __m128 length = _mm_sqrt_ps( _mm_add_ps( spacing_sq, _mm_add_ps( _mm_mul_ps( dx, dx ), _mm_mul_ps( dz, dz ) ) ) );
			const __m128 inv = _mm_div_ps( _mm_set1_ps( 1.0f ), length );
			encodeNormal4( _mm_mul_ps( dx, inv ), _mm_mul_ps( dz, inv ), pOut + ( size_t ) ( x ) * 2u );
		}
		for ( ; x < nWidth; ++x ) {
			scalarNormalPixel( pCenterRow, pDownRow, x, fSpacing, pOut + ( size_t ) ( x ) * 2u );
		}
	}
#else
	scalarDerivedNormals( pHalo, nWidth, nHeight, nHaloStride, fSpacing, pDestination );
#endif
}


void sse2UpdateCumulativeErosion(
	const uint8_t *pActive, const float *pInitialHeight, const float *pHeight,
	float *pCumulativeErosion, const size_t nCount
) {
#if TERRAIN3D_COMPILED_SSE2
	const __m128 zero = _mm_setzero_ps();
	const __m128i all_ones = _mm_set1_epi32( -1 );
	size_t index = 0u;
	for ( ; nCount - index >= 4u; index += 4u ) {
		uint32_t active_bits = 0u;
		std::memcpy( &active_bits, pActive + index, sizeof( active_bits ) );
		__m128i active = _mm_cvtsi32_si128( ( int ) ( active_bits ) );
		active = _mm_unpacklo_epi8( active, _mm_setzero_si128() );
		active = _mm_unpacklo_epi16( active, _mm_setzero_si128() );
		const __m128i inactive = _mm_cmpeq_epi32( active, _mm_setzero_si128() );
		const __m128 mask = _mm_castsi128_ps( _mm_xor_si128( inactive, all_ones ) );
		const __m128 raw_delta =
			_mm_sub_ps( _mm_loadu_ps( pInitialHeight + index ), _mm_loadu_ps( pHeight + index ) );
		const __m128 delta = _mm_and_ps( _mm_cmpgt_ps( raw_delta, zero ), raw_delta );
		const __m128 current = _mm_loadu_ps( pCumulativeErosion + index );
		const __m128 update_mask = _mm_cmplt_ps( current, delta );
		const __m128 updated =
			_mm_or_ps( _mm_and_ps( update_mask, delta ), _mm_andnot_ps( update_mask, current ) );
		_mm_storeu_ps( pCumulativeErosion + index,
			_mm_or_ps( _mm_and_ps( mask, updated ), _mm_andnot_ps( mask, current ) ) );
	}
	scalarUpdateCumulativeErosion( pActive + index, pInitialHeight + index, pHeight + index,
		pCumulativeErosion + index, nCount - index );
#else
	scalarUpdateCumulativeErosion( pActive, pInitialHeight, pHeight, pCumulativeErosion, nCount );
#endif
}

} // namespace terrain3d_simd::detail
