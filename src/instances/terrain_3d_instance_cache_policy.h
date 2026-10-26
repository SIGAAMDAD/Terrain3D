// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_INSTANCE_CACHE_POLICY_H
#define TERRAIN3D_INSTANCE_CACHE_POLICY_H

#include "core/terrain_3d_compiler.h"
#include "instances/terrain_3d_instance_cache_constants.h"

#include <stddef.h>
#include <stdint.h>

inline bool t3dInstanceCacheCanStream(
        const size_t nResidentBytes, const size_t nIncomingBytes, const size_t nBudgetBytes, const int nResidentPages ) {
	if ( nResidentPages <= 0 ) {
		return true;
	}
	if ( nIncomingBytes > nBudgetBytes || nResidentBytes > nBudgetBytes ) {
		return false;
	}
	return nIncomingBytes <= nBudgetBytes - nResidentBytes;
}

EA_FORCE_INLINE bool t3dInstanceCacheIndexValid( const int iIndex, const int nCapacity ) {
	return iIndex >= 0 && nCapacity > 0 && nCapacity <= s_nInstanceCacheMaxPages && iIndex < nCapacity;
}

#endif // TERRAIN3D_INSTANCE_CACHE_POLICY_H
