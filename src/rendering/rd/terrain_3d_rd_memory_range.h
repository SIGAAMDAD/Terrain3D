// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_MEMORY_RANGE_H
#define TERRAIN3D_RD_MEMORY_RANGE_H

#include <stdint.h>

inline static bool t3dRdByteRangeValid( const uint32_t nCapacity, const uint32_t nOffset, const uint32_t nBytes ) {
	return nOffset <= nCapacity && nBytes <= nCapacity - nOffset;
}

#endif // TERRAIN3D_RD_MEMORY_RANGE_H
