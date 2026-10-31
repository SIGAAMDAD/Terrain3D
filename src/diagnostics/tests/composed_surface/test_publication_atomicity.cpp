#include "rendering/rd/vt/core/terrain_3d_composed_surface_gpu_table.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static t3dComposedSurfacePageMapping_t mapping( uint32_t slot, uint32_t generation )
{
    t3dComposedSurfacePageMapping_t result{};
    result.m_nRegionKey = 9u;
    result.m_nCompositionRevision = 77u;
    result.m_nGeneration = generation;
    result.m_nDeviceGeneration = 4u;
    result.m_nPhysicalSlot = slot;
    result.m_nResidentChannels = T3D_COMP_SURF_CHANNEL_MASK_ALL;
    result.m_nState = T3D_COMP_SURF_PAGE_RESIDENT;
    return result;
}

int main()
{
    t3dComposedSurfaceGpuEntry_t entries[64]{};
    Terrain3DComposedSurfaceGpuTable table;
    assert( table.initializeCold( entries, 64u, 4u ) );
    uint32_t index = T3D_COMP_SURF_INVALID_SLOT;
    t3dComposedSurfaceGpuEntry_t before{};
    assert( table.stageResident( -15, 16, 0u, mapping( 1u, 1u ), index, &before ) );
    const auto first = entries[index];
    assert( before.m_nState == T3D_COMP_SURF_GPU_MAP_EMPTY );
    assert( !table.rollbackStagedResident( 64u, first, before ) );
    assert( table.rollbackStagedResident( index, first, before ) );
    assert( table.lookup( -15, 16, 0u ) == T3D_COMP_SURF_INVALID_SLOT );
    assert( entries[index].m_nState == T3D_COMP_SURF_GPU_MAP_EMPTY );

    assert( table.stageResident( -15, 16, 0u, mapping( 1u, 1u ), index, &before ) );
    uint32_t retired = T3D_COMP_SURF_INVALID_SLOT;
    assert( table.retireSlot( 1u, retired ) && retired == index );
    const auto tombstone = entries[index];
    assert( table.stageResident( -15, 16, 0u, mapping( 2u, 2u ), index, &before ) );
    assert( std::memcmp( &before, &tombstone, sizeof( before ) ) == 0 );
    const auto staged = entries[index];
    // No delayed stale rollback may clobber the newer generation.
    assert( !table.rollbackStagedResident( index, first, tombstone ) );
    assert( table.lookup( -15, 16, 0u ) == index );
    assert( table.rollbackStagedResident( index, staged, before ) );
    assert( entries[index].m_nState == T3D_COMP_SURF_GPU_MAP_TOMBSTONE );
    assert( table.lookup( -15, 16, 0u ) == T3D_COMP_SURF_INVALID_SLOT );

    // Tombstone must preserve probe continuity for other colliding entries.
    int32_t x1 = 0, x2 = 0;
    bool found = false;
    for ( int32_t a = -30; a < 30 && !found; ++a ) {
        for ( int32_t b = a + 1; b < 30; ++b ) {
            if ( ( t3d_comp_surf_gpu_hash( a, 1, 0 ) & 63u ) ==
                 ( t3d_comp_surf_gpu_hash( b, 1, 0 ) & 63u ) ) {
                x1 = a; x2 = b; found = true; break;
            }
        }
    }
    assert( found );
    assert( table.stageResident( x1, 1, 0u, mapping( 3u, 3u ), index ) );
    assert( table.stageResident( x2, 1, 0u, mapping( 4u, 4u ), index ) );
    assert( table.retireSlot( 3u, retired ) );
    assert( table.lookup( x2, 1, 0u ) != T3D_COMP_SURF_INVALID_SLOT );
    assert( table.stageResident( x1, 1, 0u, mapping( 5u, 5u ), index, &before ) );
    assert( before.m_nState == T3D_COMP_SURF_GPU_MAP_TOMBSTONE );
    const auto collision = entries[index];
    assert( table.rollbackStagedResident( index, collision, before ) );
    assert( table.lookup( x2, 1, 0u ) != T3D_COMP_SURF_INVALID_SLOT );
    std::puts( "CS17 GPU mapping rollback and collision tests PASS" );
}
