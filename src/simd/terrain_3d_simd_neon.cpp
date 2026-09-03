// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Baseline NEON implementation. It deliberately does not require ARMv8.2 FP16
// arithmetic, preserving compatibility with older ARMv7 NEON and ARMv8 CPUs.
#include "data/terrain_3d_half_buffer.h"
#include "data/terrain_3d_packed_transform.h"
#include "simd/terrain_3d_simd_internal.h"

#if defined( __ARM_NEON ) || defined( __ARM_NEON__ ) || defined( __aarch64__ ) || defined( _M_ARM64 )
#include <arm_neon.h>
#define TERRAIN3D_COMPILED_NEON 1
#else
#define TERRAIN3D_COMPILED_NEON 0
#endif

#include <math.h>

#if TERRAIN3D_COMPILED_NEON
static inline float32x4_t t3dSimdNeonDecode4Exact( const uint16_t *pSource ) {
	const uint16x4_t packed = vld1_u16( pSource );
	const uint32x4_t half32 = vmovl_u16( packed );
	const uint32x4_t exponent = vandq_u32( half32, vdupq_n_u32( 0x7c00u ) );
	const uint32x4_t special = vorrq_u32( vceqq_u32( exponent, vdupq_n_u32( 0u ) ), vceqq_u32( exponent, vdupq_n_u32( 0x7c00u ) ) );
	alignas( 16 ) uint32_t special_lanes[4];
	vst1q_u32( special_lanes, special );
	if ( ( special_lanes[0] | special_lanes[1] | special_lanes[2] | special_lanes[3] ) != 0u ) {
		alignas( 16 ) float fValues[4];
		for ( int lane = 0; lane < 4; ++lane ) {
			fValues[lane] = t3dHalfDecode( pSource[lane] );
		}
		return vld1q_f32( fValues );
	}
	const uint32x4_t sign = vshlq_n_u32( vandq_u32( half32, vdupq_n_u32( 0x8000u ) ), 16 );
	const uint32x4_t magnitude = vshlq_n_u32( vandq_u32( half32, vdupq_n_u32( 0x7fffu ) ), 13 );
	const uint32x4_t bits = vorrq_u32( sign, vaddq_u32( magnitude, vdupq_n_u32( ( uint32_t )( 127 - 15 ) << 23u ) ) );
	return vreinterpretq_f32_u32( bits );
}

static inline float32x4_t t3dSimdNeonReciprocalSqrt( const float32x4_t value ) {
#if defined( __aarch64__ ) || defined( _M_ARM64 )
	return vdivq_f32( vdupq_n_f32( 1.0f ), vsqrtq_f32( value ) );
#else
	float32x4_t estimate = vrsqrteq_f32( value );
	estimate = vmulq_f32( vrsqrtsq_f32( vmulq_f32( value, estimate ), estimate ), estimate );
	estimate = vmulq_f32( vrsqrtsq_f32( vmulq_f32( value, estimate ), estimate ), estimate );
	return estimate;
#endif
}

static inline void t3dSimdNeonEncodeNormal4( const float32x4_t argX, const float32x4_t argZ, uint8_t *pDestination ) {
	const float32x4_t scale = vdupq_n_f32( s_fByteMidpointFloat );
	const float32x4_t half = vdupq_n_f32( 0.5f );
	const float32x4_t zero = vdupq_n_f32( 0.0f );
	const float32x4_t max255 = vdupq_n_f32( s_fByteMaxFloat );
	const float32x4_t x =
	        vminq_f32( max255, vmaxq_f32( zero, vaddq_f32( vmulq_f32( vaddq_f32( argX, vdupq_n_f32( 1.0f ) ), scale ), half ) ) );
	const float32x4_t z =
	        vminq_f32( max255, vmaxq_f32( zero, vaddq_f32( vmulq_f32( vaddq_f32( argZ, vdupq_n_f32( 1.0f ) ), scale ), half ) ) );
	alignas( 16 ) uint32_t xi[4];
	alignas( 16 ) uint32_t zi[4];
	vst1q_u32( xi, vcvtq_u32_f32( x ) );
	vst1q_u32( zi, vcvtq_u32_f32( z ) );
	for ( int lane = 0; lane < 4; ++lane ) {
		pDestination[lane * 2] = ( uint8_t )( xi[lane] );
		pDestination[lane * 2 + 1] = ( uint8_t )( zi[lane] );
	}
}

static inline void t3dSimdNeonScalarNormalPixel(
        const float *pCenterRow, const float *pDownRow, const int iX, const float fSpacing, uint8_t *pDestination ) {
	const float fDx = pCenterRow[iX] - pCenterRow[iX + 1];
	const float fDz = pCenterRow[iX] - pDownRow[iX];
	const float fLength = sqrt( fDx * fDx + fSpacing * fSpacing + fDz * fDz );
	const float fInverse = fLength > 0.0f ? 1.0f / fLength : 0.0f;
	pDestination[0] = ( uint8_t )( floor( MAX( 0.0f, MIN( s_fByteMaxFloat, ( fDx * fInverse + 1.0f ) * s_fByteMidpointFloat ) ) + 0.5f ) );
	pDestination[1] = ( uint8_t )( floor( MAX( 0.0f, MIN( s_fByteMaxFloat, ( fDz * fInverse + 1.0f ) * s_fByteMidpointFloat ) ) + 0.5f ) );
}

#endif

bool t3dSimdNeonBackendAvailableAtBuild( void ) {
	return TERRAIN3D_COMPILED_NEON != 0;
}

void t3dSimdNeonHalfToFloat( const uint16_t *pSource, float *pDestination, const size_t nCount ) {
#if TERRAIN3D_COMPILED_NEON
	size_t index = 0u;
	for ( ; nCount - index >= 4u; index += 4u ) {
		vst1q_f32( pDestination + index, t3dSimdNeonDecode4Exact( pSource + index ) );
	}
	for ( ; index < nCount; ++index ) {
		pDestination[index] = t3dHalfDecode( pSource[index] );
	}
#else
	t3dSimdScalarHalfToFloat( pSource, pDestination, nCount );
#endif
}

void t3dSimdNeonPackedTransforms(
        const t3dPackedTransform_t *pTransforms, const uint32_t *pColors, const size_t nCount, const int nStride, float *pDestination ) {
#if TERRAIN3D_COMPILED_NEON
	for ( size_t i = 0u; i < nCount; ++i ) {
		const t3dPackedTransform_t &t = pTransforms[i];
		float *pOut = pDestination + i * ( size_t )( nStride );
		alignas( 16 ) float fBasis[8];
		vst1q_f32( fBasis, t3dSimdNeonDecode4Exact( t.m_szBasis ) );
		vst1q_f32( fBasis + 4, t3dSimdNeonDecode4Exact( t.m_szBasis + 4 ) );
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
			pOut[12] = ( float )( ( color >> 24u ) & 0xffu ) * ( s_fByteInvMaxFloat );
			pOut[13] = ( float )( ( color >> 16u ) & 0xffu ) * ( s_fByteInvMaxFloat );
			pOut[14] = ( float )( ( color >> 8u ) & 0xffu ) * ( s_fByteInvMaxFloat );
			pOut[15] = ( float )( color & 0xffu ) * ( s_fByteInvMaxFloat );
		}
	}
#else
	t3dSimdScalarPackedTransforms( pTransforms, pColors, nCount, nStride, pDestination );
#endif
}

void t3dSimdNeonDerivedNormals(
        const float *pHalo, const int nWidth, const int nHeight, const int nHaloStride, const float fSpacing, uint8_t *pDestination ) {
#if TERRAIN3D_COMPILED_NEON
	const float32x4_t spacing_sq = vdupq_n_f32( fSpacing * fSpacing );
	for ( int y = 0; y < nHeight; ++y ) {
		const float *pCenterRow = pHalo + ( size_t )( y + 1 ) * ( size_t )( nHaloStride ) + 1u;
		const float *pDownRow = pCenterRow + nHaloStride;
		uint8_t *pOut = pDestination + ( size_t )( y ) * ( size_t )( nWidth ) * 2u;
		int x = 0;
		for ( ; nWidth - x >= 4; x += 4 ) {
			const float32x4_t center = vld1q_f32( pCenterRow + x );
			const float32x4_t dx = vsubq_f32( center, vld1q_f32( pCenterRow + x + 1 ) );
			const float32x4_t dz = vsubq_f32( center, vld1q_f32( pDownRow + x ) );
			const float32x4_t length_sq = vaddq_f32( spacing_sq, vaddq_f32( vmulq_f32( dx, dx ), vmulq_f32( dz, dz ) ) );
			const float32x4_t inverse = t3dSimdNeonReciprocalSqrt( length_sq );
			t3dSimdNeonEncodeNormal4( vmulq_f32( dx, inverse ), vmulq_f32( dz, inverse ), pOut + ( size_t )( x ) * 2u );
		}
		for ( ; x < nWidth; ++x ) {
			t3dSimdNeonScalarNormalPixel( pCenterRow, pDownRow, x, fSpacing, pOut + ( size_t )( x ) * 2u );
		}
	}
#else
	t3dSimdScalarDerivedNormals( pHalo, nWidth, nHeight, nHaloStride, fSpacing, pDestination );
#endif
}

void t3dSimdNeonUpdateCumulativeErosion(
        const uint8_t *pActive, const float *pInitialHeight, const float *pHeight, float *pCumulativeErosion, const size_t nCount ) {
#if TERRAIN3D_COMPILED_NEON
	const float32x4_t zero = vdupq_n_f32( 0.0f );
	size_t index = 0u;
	for ( ; nCount - index >= 8u; index += 8u ) {
		const uint8x8_t active8 = vld1_u8( pActive + index );
		const uint16x8_t active16 = vmovl_u8( active8 );
		const uint32x4_t active_lo = vmovl_u16( vget_low_u16( active16 ) );
		const uint32x4_t active_hi = vmovl_u16( vget_high_u16( active16 ) );
		for ( int half = 0; half < 2; ++half ) {
			const size_t lane_index = index + ( size_t )( half ) * 4u;
			const uint32x4_t mask = vcgtq_u32( half == 0 ? active_lo : active_hi, vdupq_n_u32( 0u ) );
			const float32x4_t raw_delta = vsubq_f32( vld1q_f32( pInitialHeight + lane_index ), vld1q_f32( pHeight + lane_index ) );
			const float32x4_t delta = vbslq_f32( vcgtq_f32( raw_delta, zero ), raw_delta, zero );
			const float32x4_t current = vld1q_f32( pCumulativeErosion + lane_index );
			const float32x4_t updated = vbslq_f32( vcltq_f32( current, delta ), delta, current );
			vst1q_f32( pCumulativeErosion + lane_index, vbslq_f32( mask, updated, current ) );
		}
	}
	t3dSimdScalarUpdateCumulativeErosion(
	        pActive + index, pInitialHeight + index, pHeight + index, pCumulativeErosion + index, nCount - index );
#else
	t3dSimdScalarUpdateCumulativeErosion( pActive, pInitialHeight, pHeight, pCumulativeErosion, nCount );
#endif
}
