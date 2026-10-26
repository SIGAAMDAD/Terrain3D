// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_CLUSTER_COOKER_H
#define TERRAIN3D_WORLD_CLUSTER_COOKER_H

#include "Terrain3DTypes.h"

#include "rendering/rd/world/core/terrain_3d_world_contract.h"

#include <stdint.h>

bool t3dWorldCookMeshClustersCold( const t3dWorldClusterCookSource_t &rSource,
								   t3dWorldClusterCookBuffers_t		 &rBuffers,
								   t3dWorldMeshClusterSpan_t		 &rSpan,
								   t3dWorldClusterCookStats_t		 &rStats );

#endif // TERRAIN3D_WORLD_CLUSTER_COOKER_H
