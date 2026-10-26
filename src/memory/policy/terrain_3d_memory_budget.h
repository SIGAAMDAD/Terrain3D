// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_MEMORY_BUDGET_H
#define TERRAIN3D_MEMORY_BUDGET_H

#include "Terrain3DTypes.h"

#include "core/config/terrain_3d_compiler.h"

#include <stddef.h>
#include <stdint.h>

// t3dMemorySubsystem_e centralized in Terrain3DTypes.h.


// t3dMemoryLimitReason_e centralized in Terrain3DTypes.h.


// t3dMemorySubsystemInput_s centralized in Terrain3DTypes.h.


// t3dMemoryBudgetInput_s centralized in Terrain3DTypes.h.


// t3dMemorySubsystemAllocation_s centralized in Terrain3DTypes.h.


// t3dMemoryBudgetResult_s centralized in Terrain3DTypes.h.


class Terrain3DMemoryBudgetCoordinator {
	EA_FORCE_INLINE static uint64_t addChecked( const uint64_t nA, const uint64_t nB, bool &rOverflow ) {
		if ( nA > UINT64_MAX - nB ) {
			rOverflow = true;
			return UINT64_MAX;
		}
		return nA + nB;
	}

	EA_FORCE_INLINE static uint64_t clampToCap( const uint64_t nBytes, const uint64_t nCap ) {
		return nBytes > nCap ? nCap : nBytes;
	}

public:
	static t3dMemoryBudgetResult_t calculate( const t3dMemoryBudgetInput_t &rInput );
};

#endif // TERRAIN3D_MEMORY_BUDGET_H
