// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Allocation-free scalar math used by the editor-time autoshader baker.
#ifndef TERRAIN3D_AUTOSHADER_MATH_H
#define TERRAIN3D_AUTOSHADER_MATH_H

#include "core/utility/terrain_3d_numeric.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"

#include <math.h>
#include <stdint.h>

EA_FORCE_INLINE float t3dAutoshaderLerp( const float fA, const float fB, const float fWeight ) {
	return fA + ( fB - fA ) * fWeight;
}

EA_FORCE_INLINE float t3dAutoshaderSmoothCurve( const float fValue ) {
	const float value = t3dClampUnitFloat( fValue );
	return value * value * ( 3.0f - 2.0f * value );
}

EA_FORCE_INLINE float t3dAutoshaderSmootherCurve( const float fValue ) {
	const float value = t3dClampUnitFloat( fValue );
	return value * value * value * ( value * ( value * 6.0f - 15.0f ) + 10.0f );
}

float t3dAutoshaderRangeWeight( const float fValue, const float fMinimum, const float fMaximum, const float fFalloff );

EA_FORCE_INLINE bool t3dAutoshaderRuleCanProcessPixel( const bool bAutoPixel, const bool bOnlyAutoPixels, const bool bOverwriteManual ) {
	return bAutoPixel || !bOnlyAutoPixels || bOverwriteManual;
}

float t3dAutoshaderAngleDistanceDegrees( const float fA, const float fB );

EA_FORCE_INLINE float t3dAutoshaderHash01( const int32_t iX, const int32_t iY, const uint32_t nSeed ) {
	const uint32_t x = ( uint32_t )( iX );
	const uint32_t y = ( uint32_t )( iY );
	return ( float )( t3dHashAvalanche32( x * T3D_HASH_GOLDEN_RATIO32 ^ y * T3D_HASH_MIX32 ^ nSeed ) ) / s_fUint32MaxFloat;
}

float t3dAutoshaderValueNoise( const float fX, const float fY, const uint32_t nSeed );

float t3dAutoshaderFbmNoise(
        float fX, float fY, const uint32_t nSeed, const int nOctaves, const float fLacunarity, const float fGain );

#endif // TERRAIN3D_AUTOSHADER_MATH_H
