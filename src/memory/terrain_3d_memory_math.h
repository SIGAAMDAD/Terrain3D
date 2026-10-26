// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_MEMORY_MATH_H
#define TERRAIN3D_MEMORY_MATH_H

#include "core/terrain_3d_compiler.h"

#include <EASTL/numeric_limits.h>
#include <stddef.h>

EA_FORCE_INLINE bool t3dMemoryIsPowerOfTwo( const size_t nValue ) {
	return nValue != 0u && ( nValue & ( nValue - 1u ) ) == 0u;
}

EA_FORCE_INLINE bool t3dMemoryAlignUp( const size_t nValue, const size_t nAlignment, size_t &rAligned ) {
	if ( !t3dMemoryIsPowerOfTwo( nAlignment ) ) {
		return false;
	}
	const size_t mask = nAlignment - 1u;
	if ( nValue > eastl::numeric_limits<size_t>::max() - mask ) {
		return false;
	}
	rAligned = ( nValue + mask ) & ~mask;
	return true;
}

#endif // TERRAIN3D_MEMORY_MATH_H
