// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_MEMORY_MATH_H
#define TERRAIN3D_MEMORY_MATH_H

#include "core/config/terrain_3d_compiler.h"
#include "core/utility/terrain_3d_checked_math.h"

#include <EASTL/numeric_limits.h>
#include <stddef.h>

EA_FORCE_INLINE bool t3dMemoryIsPowerOfTwo( const size_t nValue ) {
	return nValue != 0u && ( nValue & ( nValue - 1u ) ) == 0u;
}

EA_FORCE_INLINE bool t3dMemoryAlignUp( const size_t nValue, const size_t nAlignment, size_t &rAligned ) {
	return t3dCheckedAlignUp( nValue, nAlignment, rAligned );
}

#endif // TERRAIN3D_MEMORY_MATH_H
