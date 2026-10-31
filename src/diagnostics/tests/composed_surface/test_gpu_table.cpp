#include "rendering/rd/vt/core/terrain_3d_composed_surface_gpu_table.h"
#include <assert.h>
#include <stdio.h>
#include <vector>

static t3dComposedSurfacePageMapping_t makeMapping( uint32_t slot, uint32_t generation )
{
    t3dComposedSurfacePageMapping_t value{};
    value.m_nRegionKey = 101u;
    value.m_nCompositionRevision = 12u;
    value.m_nPhysicalSlot = slot;
    value.m_nGeneration = generation;
    value.m_nDeviceGeneration = 3u;
    value.m_nResidentChannels = T3D_COMP_SURF_CHANNEL_MASK_ALL;
    value.m_nState = T3D_COMP_SURF_PAGE_RESIDENT;
    return value;
}

int main()
{
    std::vector<t3dComposedSurfaceGpuEntry_t> entries( 256u );
    Terrain3DComposedSurfaceGpuTable table;
    assert( !table.initializeCold( entries.data(), 255u, 3u ) );
    assert( table.initializeCold( entries.data(), 256u, 3u ) );
    for ( int i = 0; i < 48; ++i ) {
        uint32_t dirty = 0xffffffffu;
        assert( table.stageResident( i - 50, i * 3 - 70, 0u, makeMapping( i, i + 1u ), dirty ) );
        assert( dirty < 256u );
        assert( table.lookup( i - 50, i * 3 - 70, 0u ) == dirty );
        assert( entries[dirty].m_nPhysicalSlot == uint32_t( i ) );
        assert( entries[dirty].m_nChannelMask == 7u );
        assert( entries[dirty].m_nRevisionLo == 12u );
        assert( entries[dirty].m_nDeviceGeneration == 3u );
    }
    uint32_t dirty;
    assert( !table.stageResident( -50, -70, 0u, makeMapping( 49u, 1u ), dirty ) );
    assert( !table.stageResident( 90, 90, 1u, makeMapping( 49u, 1u ), dirty ) );
    t3dComposedSurfacePageMapping_t bad = makeMapping( 90u, 1u );
    bad.m_nResidentChannels = 3u;
    assert( !table.stageResident( 90, 90, 0u, bad, dirty ) );
    bad = makeMapping( 90u, 1u );
    bad.m_nDeviceGeneration = 2u;
    assert( !table.stageResident( 90, 90, 0u, bad, dirty ) );
    const uint32_t originalIndex = table.lookup( -27, 23 * 3 - 70, 0u );
    const t3dComposedSurfaceGpuEntry_t saved = entries[originalIndex];
    assert( table.retireSlot( 23u, dirty ) );
    assert( table.restoreRetirementAfterFailedWrite( dirty, saved ) );
    assert( table.lookup( -27, 23 * 3 - 70, 0u ) == originalIndex );
    assert( table.retireSlot( 23u, dirty ) );
    assert( table.lookup( -27, 23 * 3 - 70, 0u ) == T3D_COMP_SURF_INVALID_SLOT );
    assert( entries[dirty].m_nState == T3D_COMP_SURF_GPU_MAP_TOMBSTONE );
    assert( !table.retireSlot( 23u, dirty ) );
    assert( table.stageResident( -27, 23 * 3 - 70, 0u, makeMapping( 23u, 92u ), dirty ) );
    assert( table.lookup( -27, 23 * 3 - 70, 0u ) != T3D_COMP_SURF_INVALID_SLOT );
    table.resetCold( 4u );
    assert( table.lookup( -50, -70, 0u ) == T3D_COMP_SURF_INVALID_SLOT );
    assert( !table.stageResident( -50, -70, 0u, makeMapping( 1u, 1u ), dirty ) );
    puts( "CS05 GPU table tests passed" );
}
