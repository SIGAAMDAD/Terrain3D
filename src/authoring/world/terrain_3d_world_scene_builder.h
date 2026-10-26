// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_SCENE_BUILDER_H
#define TERRAIN3D_WORLD_SCENE_BUILDER_H

#include "authoring/world/terrain_3d_world_authoring.h"
#include "rendering/rd/world/terrain_3d_world_publication.h"

#include <stdint.h>

struct T3DWorldPrototypeBounds {
	float vCenter[3];
	float fRadius;
};

struct T3DWorldCellHashEntry {
	int32_t iCellX;
	int32_t iCellZ;
	uint32_t nCellIndex;
	uint32_t nInstanceCount;
	float vMin[3];
	float vMax[3];
	bool bOccupied;
};

struct T3DWorldCellBuildScratch {
	t3dWorldInstance_t *pReorderedInstances;
	t3dWorldTransform_t *pReorderedTransforms;
	uint64_t *pReorderedSourceIds;
	uint32_t nInstanceCapacity;
	uint32_t *pInstanceCellIds;
	uint32_t *pCellCounts;
	uint32_t *pCellWriteCursor;
	uint32_t nCellScratchCapacity;
	T3DWorldCellHashEntry *pCellHash;
	uint32_t nCellHashCapacity;
};

bool t3d_world_build_scene_cells_cold( t3dWorldInstance_t *pInstances,
        t3dWorldTransform_t *pTransforms,
        uint64_t *pInstanceSourceIds,
        uint32_t pInstanceCount,
        const T3DWorldPrototypeBounds *pPrototypeBounds,
        uint32_t pPrototypeBoundsCount,
        float pCellSize,
        t3dWorldCell_t *pCells,
        uint32_t pCellCapacity,
        T3DWorldCellBuildScratch &rScratch,
        T3DWorldOwnershipRegistry &rOwnership,
        uint32_t &rCellCount );

#endif // TERRAIN3D_WORLD_SCENE_BUILDER_H
