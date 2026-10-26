// Editor owner bridge: no resource mutation or sidecar IO in map edit callbacks.
#ifndef TERRAIN3D_COMPOSED_SURFACE_COORDINATOR_HOOKS_H
#define TERRAIN3D_COMPOSED_SURFACE_COORDINATOR_HOOKS_H
#include "authoring/vt/integration/terrain_3d_composed_surface_edit_journal.h"
#include "rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h"

inline bool t3d_cs_editor_region( const Terrain3DTerrainMegaVT &megaVT,
                                  uint32_t nIndex, t3dComposedSurfaceEditRegion_t &out )
{
    const t3dTerrainMegaVTRegion_t *region = megaVT.getRegion( nIndex );
    if ( !region || !( region->m_nFlags & T3D_TERRAIN_MEGAVT_REGION_PRESENT ) ) return false;
    int32_t x = 0, z = 0;
    Terrain3DTerrainMegaVT::regionIndexToLocation( nIndex, x, z );
    out = { nIndex, x, z, region->m_nMip0WidthPages,
            region->m_nMip0HeightPages, region->m_nMipCount, true };
    return true;
}

inline void t3d_cs_editor_dirty_bounds( const Terrain3DTerrainMegaVT &megaVT,
                                        Terrain3DComposedSurfaceEditJournal &journal,
                                        const t3dComposedSurfaceEditBounds_t &bounds,
                                        uint64_t nFingerprint )
{
    // Event-only scan; 1024 fixed logical slots, never a per-frame poll.
    // The journal expands by both the control halo and the composed page gutter.
    const uint64_t topology = megaVT.getTopologyGeneration() ? megaVT.getTopologyGeneration() : 1u;
    for ( uint32_t i = 0u; i < T3D_CS_EDITOR_REGION_COUNT; ++i ) {
        t3dComposedSurfaceEditRegion_t region{};
        if ( t3d_cs_editor_region( megaVT, i, region ) )
            journal.dirtyRegionCold( region, bounds, megaVT.getTexelsPerMeter(), nFingerprint, topology );
    }
}

inline void t3d_cs_editor_invalidate_sources( const Terrain3DTerrainMegaVT &megaVT,
                                               Terrain3DComposedSurfaceEditJournal &journal,
                                               uint64_t nFingerprint )
{
    const uint64_t topology = megaVT.getTopologyGeneration() ? megaVT.getTopologyGeneration() : 1u;
    for ( uint32_t i = 0u; i < T3D_CS_EDITOR_REGION_COUNT; ++i ) {
        t3dComposedSurfaceEditRegion_t region{};
        if ( t3d_cs_editor_region( megaVT, i, region ) )
            journal.invalidateRegionCold( region, nFingerprint, topology );
        else journal.forgetRegionCold( i, topology );
    }
}
#endif
