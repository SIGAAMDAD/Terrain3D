#ifndef TERRAIN3D_BYTE_ACCESS_H
#define TERRAIN3D_BYTE_ACCESS_H

#include "core/config/terrain_3d_compiler.h"
#include <string.h>
#include <type_traits>

// Caller supplies at least sizeof(T) readable bytes. This is a native-order
// load, independent of alignment; serialized little-endian readers stay with
// their format codecs and must not use this operation.
template <typename T>
EA_FORCE_INLINE T t3dReadNativeUnaligned( const void *pBytes ) {
	static_assert( std::is_trivially_copyable<T>::value, "Byte loads require a trivially copyable value" );
	T value;
	memcpy( &value, pBytes, sizeof( value ) );
	return value;
}

#endif // TERRAIN3D_BYTE_ACCESS_H
