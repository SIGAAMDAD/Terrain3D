#include "authoring/baking/terrain_3d_autoshader_math.h"

float t3dAutoshaderRangeWeight( const float fValue, const float fMinimum, const float fMaximum, const float fFalloff ) {
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

float t3dAutoshaderAngleDistanceDegrees( const float fA, const float fB ) {
	float fDelta = fmod( fabs( fA - fB ), 360.0f );
	if ( fDelta < 0.0f ) {
		fDelta += 360.0f;
	}
	return fDelta > 180.0f ? 360.0f - fDelta : fDelta;
}

float t3dAutoshaderValueNoise( const float fX, const float fY, const uint32_t nSeed ) {
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

float t3dAutoshaderFbmNoise(
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
	return fTotal > 0.0f ? t3dClampUnitFloat( fSum / fTotal ) : 0.0f;
}
