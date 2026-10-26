#include "rendering/rd/vt/core/terrain_3d_composed_surface_completion.h"
#include "rendering/rd/vt/core/terrain_3d_composed_surface_gpu_table.h"
#include <cassert>
#include <cstdint>
int main()
{
    assert( !t3d_cs_gpu_epoch_completed( 0u, 0u ) );
    assert( !t3d_cs_gpu_epoch_completed( 0u, 99u ) );
    assert( !t3d_cs_gpu_epoch_completed( 11u, 10u ) );
    assert( t3d_cs_gpu_epoch_completed( 11u, 11u ) );
    assert( t3d_cs_gpu_epoch_completed( 11u, 12u ) );
    assert( !t3d_cs_gpu_submission_epoch_valid( 0u, 0u ) );
    assert( !t3d_cs_gpu_submission_epoch_valid( 7u, 7u ) );
    assert( t3d_cs_gpu_submission_epoch_valid( 8u, 7u ) );
    assert( !t3d_cs_gpu_submission_epoch_valid( 7u, 9u ) );

    t3dComposedSurfaceGpuEntry_t entries[64]{};
    Terrain3DComposedSurfaceGpuTable table;
    assert( table.initializeCold( entries, 64u, 2u ) );
    t3dComposedSurfacePageMapping_t mapping{};
    mapping.m_nRegionKey = 55u;
    mapping.m_nCompositionRevision = 99u;
    mapping.m_nGeneration = 1u;
    mapping.m_nDeviceGeneration = 2u;
    mapping.m_nPhysicalSlot = 3u;
    mapping.m_nResidentChannels = T3D_CS_CHANNEL_MASK_ALL;
    mapping.m_nState = T3D_CS_PAGE_RESIDENT;
    uint32_t index = T3D_CS_INVALID_SLOT;
    assert( table.stageResident( -2, 17, 0, mapping, index ) );
    assert( index != T3D_CS_INVALID_SLOT );
    assert( table.lookup( -2, 17, 0 ) == index );
    uint32_t dirty = T3D_CS_INVALID_SLOT;
    assert( table.retireSlot( 3u, dirty ) && dirty == index );
    assert( table.lookup( -2, 17, 0 ) == T3D_CS_INVALID_SLOT );
    assert( table.stageResident( -2, 17, 0, mapping, dirty ) );
    assert( table.lookup( -2, 17, 0 ) == index );
    return 0;
}
