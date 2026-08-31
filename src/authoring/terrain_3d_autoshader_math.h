// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Allocation-free scalar math used by the editor-time autoshader baker.
#ifndef TERRAIN3D_AUTOSHADER_MATH_H
#define TERRAIN3D_AUTOSHADER_MATH_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"

#include <math.h>
#include <stdint.h>

EA_FORCE_INLINE float t3dAutoshaderClamp01( const float fValue ) {
	return fValue < 0.0f ? 0.0f : ( fValue > 1.0f ? 1.0f : fValue );
}

EA_FORCE_INLINE float t3dAutoshaderLerp( const float fA, const float fB, const float fWeight ) {
	return fA + ( fB - fA ) * fWeight;
}

EA_FORCE_INLINE float t3dAutoshaderSmoothCurve( const float fValue ) {
	const float value = t3dAutoshaderClamp01( fValue );
	return value * value * ( 3.0f - 2.0f * value );
}

EA_FORCE_INLINE float t3dAutoshaderSmootherCurve( const float fValue ) {
	const float value = t3dAutoshaderClamp01( fValue );
	return value * value * value * ( value * ( value * 6.0f - 15.0f ) + 10.0f );
}

inline float t3dAutoshaderRangeWeight( const float fValue, const float fMinimum, const float fMaximum, const float fFalloff ) {
	const float minimum = fMinimum < fMaximum ? fMinimum : fMaximum;
	const float maximum = fMinimum < fMaximum ? fMaximum : fMinimum;
	if ( fValue >= minimum && fValue <= maximum ) {
		return 1.0f;
	}
	if ( fFalloff <= 0.0f ) {
		return 0.0f;
	}
	const float fDistance = fValue < minimum ? minimum - fValue : fValue - maximum;
	return t3dAutoshaderSmoothCurve( 1.0f - fDistance / fFalloff );
}

EA_FORCE_INLINE bool t3dAutoshaderRuleCanProcessPixel( const bool bAutoPixel, const bool bOnlyAutoPixels, const bool bOverwriteManual ) {
	return bAutoPixel || !bOnlyAutoPixels || bOverwriteManual;
}

inline float t3dAutoshaderAngleDistanceDegrees( const float fA, const float fB ) {
	float fDelta = fmod( fabs( fA - fB ), 360.0f );
	if ( fDelta < 0.0f ) {
		fDelta += 360.0f;
	}
	return fDelta > 180.0f ? 360.0f - fDelta : fDelta;
}

EA_FORCE_INLINE uint32_t t3dAutoshaderHashU32( uint32_t nValue ) {
	nValue ^= nValue >> 16u;
	nValue *= 0x7feb352du;
	nValue ^= nValue >> 15u;
	nValue *= 0x846ca68bu;
	nValue ^= nValue >> 16u;
	return nValue;
}

EA_FORCE_INLINE float t3dAutoshaderHash01( const int32_t iX, const int32_t iY, const uint32_t nSeed ) {
	const uint32_t x = ( uint32_t )( iX );
	const uint32_t y = ( uint32_t )( iY );
	return ( float )( t3dAutoshaderHashU32( x * s_nHashGoldenRatio32 ^ y * s_nHashMix32 ^ nSeed ) ) / s_fUint32MaxFloat;
}

inline float t3dAutoshaderValueNoise( const float fX, const float fY, const uint32_t nSeed ) {
	const int32_t x0 = ( int32_t )( floor( fX ) );
	const int32_t y0 = ( int32_t )( floor( fY ) );
	const int32_t x1 = x0 + 1;
	const int32_t y1 = y0 + 1;
	const float fTx = t3dAutoshaderSmoothCurve( fX - ( float )( x0 ) );
	const float fTy = t3dAutoshaderSmoothCurve( fY - ( float )( y0 ) );
	const float fA = t3dAutoshaderLerp( t3dAutoshaderHash01( x0, y0, nSeed ), t3dAutoshaderHash01( x1, y0, nSeed ), fTx );
	const float fB = t3dAutoshaderLerp( t3dAutoshaderHash01( x0, y1, nSeed ), t3dAutoshaderHash01( x1, y1, nSeed ), fTx );
	return t3dAutoshaderLerp( fA, fB, fTy );
}

inline float t3dAutoshaderFbmNoise(
        float fX, float fY, const uint32_t nSeed, const int nOctaves, const float fLacunarity, const float fGain ) {
	float fAmplitude = 1.0f;
	float fSum = 0.0f;
	float fTotal = 0.0f;
	for ( int octave = 0; octave < nOctaves; ++octave ) {
		fSum += t3dAutoshaderValueNoise( fX, fY, nSeed + ( uint32_t )( octave ) * 1013u ) * fAmplitude;
		fTotal += fAmplitude;
		fAmplitude *= fGain;
		fX *= fLacunarity;
		fY *= fLacunarity;
	}
	return fTotal > 0.0f ? t3dAutoshaderClamp01( fSum / fTotal ) : 0.0f;
}

#endif // TERRAIN3D_AUTOSHADER_MATH_H
