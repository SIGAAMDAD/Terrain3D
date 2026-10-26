#ifndef TERRAIN3D_CHECKED_MATH_H
#define TERRAIN3D_CHECKED_MATH_H

#include "core/config/terrain_3d_compiler.h"

// Unsigned size/count arithmetic. Failure leaves the output unchanged, including
// when it aliases an input. Saturating or sentinel policies stay with callers.
template <typename T>
EA_FORCE_INLINE bool t3dCheckedAdd( const T nA, const T nB, T &rValue ) {
	static_assert( ( T )-1 > ( T )0 && ( T )2 != ( T )true, "Unsigned integer required" );
	if ( nA > ( T )~( T )0 - nB ) {
		return false;
	}
	rValue = nA + nB;
	return true;
}

template <typename T>
EA_FORCE_INLINE bool t3dCheckedMultiply( const T nA, const T nB, T &rValue ) {
	static_assert( ( T )-1 > ( T )0 && ( T )2 != ( T )true, "Unsigned integer required" );
	if ( nA != 0 && nB > ( T )~( T )0 / nA ) {
		return false;
	}
	rValue = nA * nB;
	return true;
}

template <typename T>
EA_FORCE_INLINE bool t3dCheckedAlignUp( const T nValue, const T nAlignment, T &rAligned ) {
	if ( nAlignment == 0 || ( nAlignment & ( nAlignment - 1 ) ) != 0 ) {
		return false;
	}
	T sum = 0;
	if ( !t3dCheckedAdd( nValue, ( T )( nAlignment - 1 ), sum ) ) {
		return false;
	}
	rAligned = sum & ~( nAlignment - 1 );
	return true;
}

template <typename T>
EA_FORCE_INLINE bool t3dByteRangeFits( const T nOffset, const T nBytes, const T nLimit ) {
	static_assert( ( T )-1 > ( T )0 && ( T )2 != ( T )true, "Unsigned integer required" );
	return nOffset <= nLimit && nBytes <= nLimit - nOffset;
}

#endif // TERRAIN3D_CHECKED_MATH_H
