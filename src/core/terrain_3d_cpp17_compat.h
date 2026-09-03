#ifndef TERRAIN3D_CPP17_COMPAT_H
#define TERRAIN3D_CPP17_COMPAT_H

#include <stdint.h>

// C++17 replacement for std::countr_zero used while configuring power-of-two
// region geometry.
EA_FORCE_INLINE int32_t t3dCountrZeroU32( uint32_t nValue ) noexcept {
	if ( nValue == 0u ) {
		return 32;
	}

	int32_t nCount = 0;
	while ( ( nValue & 1u ) == 0u ) {
		nValue >>= 1u;
		++nCount;
	}
	return nCount;
}

#endif // TERRAIN3D_CPP17_COMPAT_H
