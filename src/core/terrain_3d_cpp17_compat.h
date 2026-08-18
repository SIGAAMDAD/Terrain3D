#pragma once

#ifndef TERRAIN_3D_CPP17_COMPAT_H
#define TERRAIN_3D_CPP17_COMPAT_H

#include <cstdint>

namespace terrain3d {

// C++17 replacement for std::countr_zero. The call sites use this only while
// configuring power-of-two region geometry, so a branch-free compiler builtin
// would provide no measurable benefit over this small portable loop.
constexpr int32_t countrZeroU32( uint32_t nValue ) noexcept {
	if ( nValue == 0u ) {
		return 32;
	}

	int32_t count = 0;
	while ( ( nValue & 1u ) == 0u ) {
		nValue >>= 1u;
		++count;
	}
	return count;
}

} // namespace terrain3d

#endif // TERRAIN_3D_CPP17_COMPAT_H
