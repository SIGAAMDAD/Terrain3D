// Copyright (c) 2023-2026 Terrain3D contributors.
// Transient cold-load / offline hierarchy index. No persisted format or runtime ABI.
#ifndef TERRAIN3D_HLOD_CELL_INDEX_H
#define TERRAIN3D_HLOD_CELL_INDEX_H

#include "core/runtime/terrain_3d_eastl_helpers.h"

#include <EASTL/sort.h>
#include <stdint.h>

typedef struct t3dHlodCellIndex_s
{
	int32_t m_nLevel;
	int32_t m_nX;
	int32_t m_nY;
	int32_t m_nEntry;
} t3dHlodCellIndex_t;

EA_FORCE_INLINE bool t3dHlodCellIndexLess(
	const t3dHlodCellIndex_t &rA,
	const t3dHlodCellIndex_t &rB )
{
	if ( rA.m_nLevel != rB.m_nLevel ) {
		return rA.m_nLevel < rB.m_nLevel;
	}
	if ( rA.m_nX != rB.m_nX ) {
		return rA.m_nX < rB.m_nX;
	}
	return rA.m_nY < rB.m_nY;
}

// Sorted index detects ambiguous same-level cells before publishing any links.
template< typename TEntries >
EA_FORCE_INLINE bool t3dBuildHlodCellIndex(
	const TEntries &rEntries,
	eastl::vector< t3dHlodCellIndex_t, Terrain3DEastlAllocator > &rIndex )
{
	if ( rEntries.size() > 65535u || !t3dVectorResizeCold( rIndex, rEntries.size() ) ) {
		return false;
	}
	for ( size_t i = 0u; i < rEntries.size(); ++i ) {
		const auto cluster = rEntries[i].getCluster();
		rIndex[i] = { ( int32_t ) rEntries[i].m_nLevel, cluster.x, cluster.y, ( int32_t ) i };
	}
	eastl::sort( rIndex.begin(), rIndex.end(), t3dHlodCellIndexLess );
	for ( size_t i = 1u; i < rIndex.size(); ++i ) {
		const t3dHlodCellIndex_t &previous = rIndex[i - 1u];
		const t3dHlodCellIndex_t &current = rIndex[i];
		if ( previous.m_nLevel == current.m_nLevel && previous.m_nX == current.m_nX &&
			 previous.m_nY == current.m_nY ) {
			return false;
		}
	}
	return true;
}

EA_FORCE_INLINE int t3dFindHlodCellIndex(
	const eastl::vector< t3dHlodCellIndex_t, Terrain3DEastlAllocator > &rIndex,
	const int nLevel,
	const int nX,
	const int nY )
{
	const t3dHlodCellIndex_t key = { nLevel, nX, nY, -1 };
	size_t begin = 0u;
	size_t end = rIndex.size();
	while ( begin < end ) {
		const size_t midpoint = begin + ( end - begin ) / 2u;
		if ( t3dHlodCellIndexLess( rIndex[midpoint], key ) ) {
			begin = midpoint + 1u;
		} else {
			end = midpoint;
		}
	}
	if ( begin == rIndex.size() || t3dHlodCellIndexLess( key, rIndex[begin] ) ) {
		return -1;
	}
	return rIndex[begin].m_nEntry;
}

#endif // TERRAIN3D_HLOD_CELL_INDEX_H
