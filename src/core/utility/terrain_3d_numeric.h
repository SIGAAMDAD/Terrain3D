#ifndef TERRAIN3D_NUMERIC_H
#define TERRAIN3D_NUMERIC_H

#include "core/config/terrain_3d_compiler.h"
#include <stdint.h>

// Positive divisor, with mathematical floor for negative coordinates. Invalid
// divisors preserve the HLOD codec/streamer sentinel contract.
EA_FORCE_INLINE int32_t t3dDivideFloor(
	const int32_t nValue,
	const int32_t nDivisor ) {
	if ( nDivisor <= 0 ) {
		return 0;
	}
	const int32_t quotient = nValue / nDivisor;
	return quotient - ( nValue % nDivisor < 0 ? 1 : 0 );
}

// Comparisons deliberately preserve NaNs, matching the material/baker callers.
EA_FORCE_INLINE float t3dClampUnitFloat(
	const float fValue ) {
	return fValue < 0.0f ? 0.0f : ( fValue > 1.0f ? 1.0f : fValue );
}

// Finite input contract. Clamp first, then nearest integer with half-up ties.
EA_FORCE_INLINE uint32_t t3dPackUnorm8Nearest(
	const float fValue ) {
	const float value = t3dClampUnitFloat( fValue );
	return ( uint32_t ) ( value * 255.0f + 0.5f );
}

#endif // TERRAIN3D_NUMERIC_H
