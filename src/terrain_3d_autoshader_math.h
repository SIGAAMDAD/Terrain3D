// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Allocation-free scalar math used by the editor-time autoshader baker.
#pragma once

#ifndef TERRAIN3D_AUTOSHADER_MATH_H
#define TERRAIN3D_AUTOSHADER_MATH_H

#include <cmath>
#include <cstdint>

namespace terrain3d_autoshader_math {

inline float clamp01( const float fValue ) {
	return fValue < 0.0f ? 0.0f : ( fValue > 1.0f ? 1.0f : fValue );
}

inline float lerp( const float fA, const float fB, const float fWeight ) {
	return fA + ( fB - fA ) * fWeight;
}

inline float smoothCurve( const float fValue ) {
	const float value = clamp01( fValue );
	return value * value * ( 3.0f - 2.0f * value );
}

inline float smootherCurve( const float fValue ) {
	const float value = clamp01( fValue );
	return value * value * value * ( value * ( value * 6.0f - 15.0f ) + 10.0f );
}

inline float rangeWeight(
	const float fValue, const float fMinimum, const float fMaximum,
	const float fFalloff
) {
	const float minimum = fMinimum < fMaximum ? fMinimum : fMaximum;
	const float maximum = fMinimum < fMaximum ? fMaximum : fMinimum;
	if ( fValue >= minimum && fValue <= maximum ) {
		return 1.0f;
	}
	if ( fFalloff <= 0.0f ) {
		return 0.0f;
	}
	const float fDistance = fValue < minimum ? minimum - fValue : fValue - maximum;
	return smoothCurve( 1.0f - fDistance / fFalloff );
}

inline bool ruleCanProcessPixel(
	const bool bAutoPixel, const bool bOnlyAutoPixels,
	const bool bOverwriteManual
) {
	return bAutoPixel || !bOnlyAutoPixels || bOverwriteManual;
}

inline float angleDistanceDegrees( const float fA, const float fB ) {
	float fDelta = std::fmod( std::fabs( fA - fB ), 360.0f );
	if ( fDelta < 0.0f ) {
		fDelta += 360.0f;
	}
	return fDelta > 180.0f ? 360.0f - fDelta : fDelta;
}

inline uint32_t hashU32( uint32_t nValue ) {
	nValue ^= nValue >> 16u;
	nValue *= 0x7feb352du;
	nValue ^= nValue >> 15u;
	nValue *= 0x846ca68bu;
	nValue ^= nValue >> 16u;
	return nValue;
}

inline float hash01( const int32_t iX, const int32_t iY, const uint32_t nSeed ) {
	const uint32_t x = ( uint32_t ) ( iX );
	const uint32_t y = ( uint32_t ) ( iY );
	return ( float ) ( hashU32( x * TERRAIN3D_HASH_GOLDEN_RATIO_32 ^ y * TERRAIN3D_HASH_MIX_32 ^ nSeed ) ) / TERRAIN3D_UINT32_MAX_FLOAT;
}

inline float valueNoise( const float fX, const float fY, const uint32_t nSeed ) {
	const int32_t x0 = ( int32_t ) ( std::floor( fX ) );
	const int32_t y0 = ( int32_t ) ( std::floor( fY ) );
	const int32_t x1 = x0 + 1;
	const int32_t y1 = y0 + 1;
	const float fTx = smoothCurve( fX - ( float ) ( x0 ) );
	const float fTy = smoothCurve( fY - ( float ) ( y0 ) );
	const float fA = lerp( hash01( x0, y0, nSeed ), hash01( x1, y0, nSeed ), fTx );
	const float fB = lerp( hash01( x0, y1, nSeed ), hash01( x1, y1, nSeed ), fTx );
	return lerp( fA, fB, fTy );
}

inline float fbmNoise(
	float fX, float fY, const uint32_t nSeed, const int nOctaves,
	const float fLacunarity, const float fGain
) {
	float fAmplitude = 1.0f;
	float fSum = 0.0f;
	float fTotal = 0.0f;
	for ( int octave = 0; octave < nOctaves; ++octave ) {
		fSum += valueNoise( fX, fY, nSeed + ( uint32_t ) ( octave ) * 1013u ) * fAmplitude;
		fTotal += fAmplitude;
		fAmplitude *= fGain;
		fX *= fLacunarity;
		fY *= fLacunarity;
	}
	return fTotal > 0.0f ? clamp01( fSum / fTotal ) : 0.0f;
}

} // namespace terrain3d_autoshader_math

#endif // TERRAIN3D_AUTOSHADER_MATH_H
