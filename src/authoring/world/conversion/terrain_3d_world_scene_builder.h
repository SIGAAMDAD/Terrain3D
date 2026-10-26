// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_SCENE_BUILDER_H
#define TERRAIN3D_WORLD_SCENE_BUILDER_H

#include "Terrain3DTypes.h"

#include "authoring/world/core/terrain_3d_world_authoring.h"
#include "rendering/rd/world/runtime/terrain_3d_world_publication.h"

#include <stdint.h>

// t3dWorldPrototypeBounds_s centralized in Terrain3DTypes.h.


// t3dWorldCellHashEntry_s centralized in Terrain3DTypes.h.


// t3dWorldCellBuildScratch_s centralized in Terrain3DTypes.h.


bool t3d_world_build_scene_cells_cold( t3dWorldInstance_t *pInstances,
        t3dWorldTransform_t *pTransforms,
        uint64_t *pInstanceSourceIds,
        uint32_t pInstanceCount,
        const t3dWorldPrototypeBounds_t *pPrototypeBounds,
        uint32_t pPrototypeBoundsCount,
        float pCellSize,
        t3dWorldCell_t *pCells,
        uint32_t pCellCapacity,
        t3dWorldCellBuildScratch_t &rScratch,
        t3dWorldOwnershipRegistry_t &rOwnership,
        uint32_t &rCellCount );

#endif // TERRAIN3D_WORLD_SCENE_BUILDER_H
