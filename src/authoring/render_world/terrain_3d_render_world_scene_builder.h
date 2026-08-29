// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_SCENE_BUILDER_H
#define TERRAIN3D_RENDER_WORLD_SCENE_BUILDER_H

#include "authoring/render_world/terrain_3d_render_world_authoring.h"

#include <stdint.h>

struct T3DRenderWorldPrototypeBounds {
	float vCenter[3];
	float fRadius;
};

struct T3DRenderWorldCellHashEntry {
	int32_t iCellX;
	int32_t iCellZ;
	uint32_t nCellIndex;
	uint32_t nInstanceCount;
	float vMin[3];
	float vMax[3];
	bool bOccupied;
};

struct T3DRenderWorldCellBuildScratch {
	T3DInstanceRecord *pReorderedInstances;
	uint64_t *pReorderedSourceIds;
	uint32_t nInstanceCapacity;
	uint32_t *pInstanceCellIds;
	uint32_t *pCellCounts;
	uint32_t *pCellWriteCursor;
	uint32_t nCellScratchCapacity;
	T3DRenderWorldCellHashEntry *pCellHash;
	uint32_t nCellHashCapacity;
};

bool t3d_render_world_build_scene_cells_cold( T3DInstanceRecord *pInstances,
        uint64_t *pInstanceSourceIds,
        uint32_t pInstanceCount,
        const T3DRenderWorldPrototypeBounds *pPrototypeBounds,
        uint32_t pPrototypeBoundsCount,
        float pCellSize,
        T3DSceneCell *pCells,
        uint32_t pCellCapacity,
        T3DRenderWorldCellBuildScratch &rScratch,
        T3DRenderWorldOwnershipRegistry &rOwnership,
        uint32_t &rCellCount );

#endif // TERRAIN3D_RENDER_WORLD_SCENE_BUILDER_H
