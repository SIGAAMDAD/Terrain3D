// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_INSTANCE_CAPACITY_H
#define TERRAIN3D_INSTANCE_CAPACITY_H

#include "Terrain3DTypes.h"

#include "core/config/terrain_3d_compiler.h"

#include <EASTL/algorithm.h>
#include <stddef.h>
#include <stdint.h>

// t3dInstanceCellCapacityPlan_s centralized in Terrain3DTypes.h.


EA_FORCE_INLINE static bool t3dAccumulateInstanceCellCount( const size_t nCellCount, size_t &rRequiredCells ) {
	if ( nCellCount > SIZE_MAX - rRequiredCells ) {
		rRequiredCells = SIZE_MAX;
		return false;
	}
	rRequiredCells += nCellCount;
	return true;
}

EA_FORCE_INLINE static t3dInstanceCellCapacityPlan_t t3dPlanInstanceCellCapacity(
        const size_t nRequiredCells, const size_t nCurrentCells, const size_t nMinimumCapacity = 16u ) {
	t3dInstanceCellCapacityPlan_t plan;
	plan.m_nRequired = nRequiredCells;
	plan.m_nHeadroom = eastl::max( nMinimumCapacity, nRequiredCells / 8u );
	plan.m_nDesired = nRequiredCells > SIZE_MAX - plan.m_nHeadroom ? SIZE_MAX : nRequiredCells + plan.m_nHeadroom;
	plan.m_nTarget = eastl::max( eastl::max( plan.m_nDesired, nCurrentCells ), nMinimumCapacity );
	return plan;
}

#endif // TERRAIN3D_INSTANCE_CAPACITY_H
