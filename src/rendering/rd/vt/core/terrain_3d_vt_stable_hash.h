// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_STABLE_HASH_H
#define TERRAIN3D_VT_STABLE_HASH_H

#include <stdint.h>
#include <stddef.h>
#include "core/utility/terrain_3d_hash_bytes.h"

static inline uint64_t t3d_vt_stable_hash64( const void *pBytes, const uint64_t nBytes ) {
	const uint8_t *const pData = ( const uint8_t * )pBytes;
	uint64_t nHash = 1469598103934665603ull;
	if ( pData == NULL ) {
		return nBytes == 0u ? nHash : 0u;
	}
	return t3dHashFnv1a64( pData, nBytes, nHash );
}

#endif // TERRAIN3D_VT_STABLE_HASH_H
