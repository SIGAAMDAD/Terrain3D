#include "rendering/rd/vt/core/terrain_3d_composed_surface_contract.h"

#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include <string.h>

// UCMP-00/03 portable acceptance for complete V1-bound composed region
// pyramids. No Godot/GLSL/device dependency or production cutover.
static void buildRegion( t3dComposedSurfacePageRecord_t *pPages,
                         const uint32_t nTiles, const uint64_t nRegion )
{
    uint32_t index = 0u;
    for ( uint32_t mip = 0u, n = nTiles; n; ++mip, n >>= 1u ) {
        for ( uint32_t y = 0u; y < n; ++y ) {
            for ( uint32_t x = 0u; x < n; ++x ) {
                t3dComposedSurfacePageRecord_t &p = pPages[index++];
                p = {};
                p.m_nRegionKey = nRegion;
                p.m_nMip = mip;
                p.m_nPageX = x;
                p.m_nPageY = y;
                p.m_nChannelMask = T3D_CS_CHANNEL_MASK_ALL;
                p.m_nContentFingerprint = 51u + index;
                p.m_nCompositionRevision = 71u;
                p.m_nGeneration = 3u;
            }
        }
    }
}

int main()
{
    static_assert( T3D_CS_FILE_VERSION == 1u, "Sidecar format must remain unchanged" );
    static_assert( T3D_CS_PAGE_INTERIOR == 128u, "V1 page interior contract" );
    static_assert( T3D_CS_CHANNEL_COUNT == 3u, "Exactly three composed channels" );
    static_assert( T3D_CS_COVERAGE_FROM_CONTROLS == 1u, "Holes remain authoritative" );
    t3dComposedSurfacePageRecord_t pages[T3D_CS_MAX_REGION_MIP_PAGES]{};
    uint32_t accepted = 0u;
    for ( const uint32_t tiles : { 1u, 2u, 4u } ) {
        const uint32_t count = tiles == 4u ? 21u : tiles == 2u ? 5u : 1u;
        buildRegion( pages, tiles, 18u );
        uint32_t resultTiles = 0u;
        uint64_t sig = 0u;
        assert( t3d_cs_import_region_mips_complete( pages, count, resultTiles, sig ) );
        assert( resultTiles == tiles && sig != 0u );
        const uint64_t before = sig;
        pages[count - 1u].m_nContentFingerprint ^= 1u;
        assert( t3d_cs_import_region_mips_complete( pages, count, resultTiles, sig ) );
        assert( sig != before );
        pages[count - 1u].m_nContentFingerprint ^= 1u;
        for ( const uint32_t partial : { 0u, count - 1u } ) {
            assert( !t3d_cs_import_region_mips_complete( pages, partial, resultTiles, sig ) );
            assert( resultTiles == 0u && sig == 0u );
        }
        pages[count - 1u].m_nMip += 1u;
        assert( !t3d_cs_import_region_mips_complete( pages, count, resultTiles, sig ) );
        pages[count - 1u].m_nMip -= 1u;
        pages[count - 1u].m_nPageX += 1u;
        assert( !t3d_cs_import_region_mips_complete( pages, count, resultTiles, sig ) );
        pages[count - 1u].m_nPageX -= 1u;
        pages[count - 1u].m_nChannelMask = 3u;
        assert( !t3d_cs_import_region_mips_complete( pages, count, resultTiles, sig ) );
        pages[count - 1u].m_nChannelMask = T3D_CS_CHANNEL_MASK_ALL;
        pages[count - 1u].m_nGeneration = 0u;
        assert( !t3d_cs_import_region_mips_complete( pages, count, resultTiles, sig ) );
        pages[count - 1u].m_nGeneration = 3u;
        ++accepted;
    }
    // All unsupported page counts fail even if the first page is valid.
    buildRegion( pages, 4u, 18u );
    for ( const uint32_t count : { 2u, 3u, 4u, 6u, 10u, 16u, 20u, 22u } ) {
        uint32_t tiles = 0u;
        uint64_t signature = 0u;
        assert( !t3d_cs_import_region_mips_complete( pages, count, tiles, signature ) );
    }
    printf( "UCMP Wave1 complete composed region mips: PASS (%u pyramids + rejection cases)\n", accepted );
    return 0;
}
