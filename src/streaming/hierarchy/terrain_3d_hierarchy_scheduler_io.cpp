#include "streaming/hierarchy/terrain_3d_hierarchy_scheduler_io.h"

#include <vector>

bool t3dHierarchyBindFromTileStoreCold( Terrain3DHierarchyScheduler &scheduler,
                                        const Terrain3DHierarchicalTileStore &store ) {
    std::vector< t3dRasterHierarchyV2Node_t > nodes( T3D_HIERARCHY_MAX_NODES );
    std::vector< uint8_t > present( T3D_HIERARCHY_MAX_NODES );
    uint32_t generation = 0u;
    if ( !store.snapshotDirectoryCold( nodes.data(), present.data(),
                                       ( uint32_t ) nodes.size(), generation ) ) {
        scheduler.reset();
        return false;
    }
    return scheduler.bindCold( nodes.data(), present.data(),
                               ( uint32_t ) nodes.size(), generation );
}

// Owner-thread adapter. The scheduler itself never owns F02's worker/pool.
uint32_t t3dHierarchyDispatchToTileStore( Terrain3DHierarchyScheduler &scheduler,
                                          Terrain3DHierarchicalTileStore &store,
                                          const t3dHierarchyScheduleLimits_t &limits ) {
    t3dHierarchyTileRequest_t requests[T3D_HIERARCHY_TILE_MAX_SLOTS]{};
    const uint32_t count = scheduler.selectTileDispatch( requests,
        T3D_HIERARCHY_TILE_MAX_SLOTS, limits );
    for ( uint32_t i = 0u; i < count; ++i ) {
        const auto &r = requests[i];
        store.requestNode( r.m_nNodeId, r.m_nChannels, r.m_nPriority, r.m_nSourceRevision );
    }
    store.service();
    return count;
}
