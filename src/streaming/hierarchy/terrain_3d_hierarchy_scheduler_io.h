#ifndef TERRAIN3D_HIERARCHY_SCHEDULER_IO_H
#define TERRAIN3D_HIERARCHY_SCHEDULER_IO_H

#include "streaming/hierarchy/terrain_3d_hierarchy_scheduler.h"
#include "streaming/hierarchy/terrain_3d_hierarchical_tile_store.h"

// Initialize the owner-thread scheduler from the same immutable generation
// and node directory that the F02 tile worker actually services.
bool t3dHierarchyBindFromTileStoreCold( Terrain3DHierarchyScheduler &rScheduler,
                                        const Terrain3DHierarchicalTileStore &rStore );

uint32_t t3dHierarchyDispatchToTileStore( Terrain3DHierarchyScheduler &rScheduler,
                                          Terrain3DHierarchicalTileStore &rStore,
                                          const t3dHierarchyScheduleLimits_t &rLimits );

#endif
