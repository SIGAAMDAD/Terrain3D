#include "diagnostics/tests/systems/test_harness.h"
#include "streaming/hierarchy/terrain_3d_hierarchy_scheduler.h"
#include <vector>

namespace terrain3d_tests {
namespace {
uint16_t schedulerNodeId( uint32_t l, uint32_t x, uint32_t z ) {
    uint32_t start = 0u;
    for ( uint32_t i = 0u; i < l; ++i ) start += 1u << ( i * 2u );
    return ( uint16_t ) ( start + z * ( 1u << l ) + x );
}
void makeHierarchy( std::vector< t3dRasterHierarchyStreamNode_t > &nodes,
                    std::vector< uint8_t > &present ) {
    nodes.resize( T3D_HIERARCHY_MAX_NODES );
    present.resize( T3D_HIERARCHY_MAX_NODES, 1u );
    for ( uint32_t level = 0u; level <= 5u; ++level ) {
        const uint32_t w = 1u << level;
        for ( uint32_t z = 0u; z < w; ++z ) for ( uint32_t x = 0u; x < w; ++x ) {
            auto &n = nodes[schedulerNodeId( level, x, z )];
            n.m_nLevel = ( uint8_t ) level;
            n.m_nGeneration = 7u;
            n.m_nRevision = 11u;
            n.m_nCoverageMask = 7u;
            n.m_nChildMask = level < 5u ? 15u : 0u;
            n.m_nLeafCount = 1u << ( ( 5u - level ) * 2u );
            n.m_fMinHeight = -10.0f;
            n.m_fMaxHeight = 200.0f;
            n.m_fMaxHeightErrorM = 16.0f / ( float ) ( 1u << level );
        }
    }
}
} // namespace

void register_hierarchy_scheduler_tests( TestRunner &runner ) {
    runner.run( "hierarchy_scheduler", "f03_bounded_cover_parent_hold_and_exact_priority", []( TestContext &test ) {
        std::vector< t3dRasterHierarchyStreamNode_t > nodes;
        std::vector< uint8_t > present;
        makeHierarchy( nodes, present );
        Terrain3DHierarchyScheduler scheduler;
        T3D_REQUIRE( test, scheduler.bindCold( nodes.data(), present.data(), present.size(), 7u ) );
        scheduler.markGpuCommitted( 0u, 7u, 11u, 7u );
        t3dHierarchyView_t view;
        view.m_fCameraX = 18.0f;
        view.m_fCameraY = 20.0f;
        view.m_fCameraZ = 22.0f;
        t3dHierarchyScheduleLimits_t limits;
        scheduler.schedule( view, limits, 32u );
        T3D_REQUIRE( test, scheduler.coverCount() > 0u );
        T3D_CHECK( test, scheduler.coverCount() <= T3D_HIERARCHY_MAX_COVER &&
            scheduler.demandCount() <= T3D_HIERARCHY_MAX_DEMANDS );
        T3D_CHECK( test, scheduler.stats().m_nParentHeld > 0u &&
            scheduler.stats().m_nLegacyFallback == 0u && scheduler.stats().m_nExactReserved == 16u );
        for ( uint32_t i = 0u; i < scheduler.coverCount(); ++i )
            T3D_CHECK( test, scheduler.cover()[i].m_nAvailableAncestor != UINT16_MAX );
        t3dHierarchyTileRequest_t batch[32]{};
        const uint32_t count = scheduler.selectTileDispatch( batch, 32u, limits );
        T3D_CHECK( test, count <= 16u && scheduler.stats().m_nDispatched == count );
        for ( uint32_t i = 0u; i < count; ++i ) {
            T3D_CHECK( test, batch[i].m_nNodeId < T3D_HIERARCHY_MAX_NODES &&
                batch[i].m_nSourceRevision == 11u && batch[i].m_nChannels != 0u );
        }
        scheduler.schedule( view, limits, 32u );
        const uint32_t coverCount = scheduler.coverCount();
        scheduler.schedule( view, limits, 32u );
        T3D_CHECK( test, scheduler.coverCount() == coverCount );
        const uint16_t desired = scheduler.cover()[0].m_nDesiredNode;
        scheduler.markGpuCommitted( desired, 7u, 11u, 6u ); // stale generation rejected
        scheduler.schedule( view, limits, 0u );
        T3D_CHECK( test, scheduler.cover()[0].m_nAvailableAncestor != desired || desired == 0u );
        scheduler.markGpuCommitted( desired, 7u, 11u, 7u );
        scheduler.schedule( view, limits, 0u );
        T3D_CHECK( test, scheduler.cover()[0].m_nAvailableAncestor == desired );
        scheduler.invalidateGpuNode( desired );
        scheduler.schedule( view, limits, 0u );
        T3D_CHECK( test, scheduler.cover()[0].m_nAvailableAncestor != desired || desired == 0u );
    } );
    runner.run( "hierarchy_scheduler", "f03_sparse_holes_teleport_and_invalid_source", []( TestContext &test ) {
        std::vector< t3dRasterHierarchyStreamNode_t > nodes;
        std::vector< uint8_t > present;
        makeHierarchy( nodes, present );
        // Remove a full quadrant and its descendants; parent is still sparse.
        nodes[schedulerNodeId( 0, 0, 0 )].m_nChildMask &= ~1u;
        for ( uint32_t l = 1u; l <= 5u; ++l ) for ( uint32_t z = 0u; z < ( 1u << ( l - 1u ) ); ++z )
            for ( uint32_t x = 0u; x < ( 1u << ( l - 1u ) ); ++x )
                present[schedulerNodeId( l, x, z )] = 0u;
        Terrain3DHierarchyScheduler scheduler;
        T3D_REQUIRE( test, scheduler.bindCold( nodes.data(), present.data(), present.size(), 7u ) );
        t3dHierarchyView_t view;
        view.m_fCameraX = 1500.0f;
        view.m_fCameraZ = 1500.0f;
        t3dHierarchyScheduleLimits_t limits;
        scheduler.schedule( view, limits, 0u );
        T3D_CHECK( test, scheduler.coverCount() > 0u );
        view.m_fCameraX = -1500.0f; view.m_fCameraZ = -1500.0f;
        view.m_fForwardX = -1.0f;
        scheduler.schedule( view, limits, 0u );
        T3D_CHECK( test, scheduler.coverCount() == 0u ||
            scheduler.cover()[0].m_nDesiredNode != schedulerNodeId( 5u, 2u, 2u ) );
        scheduler.reset();
        scheduler.schedule( view, limits, 0u );
        T3D_CHECK( test, scheduler.coverCount() == 0u );
        nodes[0].m_fMaxHeightErrorM = -1.0f;
        T3D_CHECK( test, !scheduler.bindCold( nodes.data(), present.data(), present.size(), 7u ) );
    } );
}
}
