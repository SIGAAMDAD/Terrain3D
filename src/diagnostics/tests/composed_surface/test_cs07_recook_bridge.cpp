#include "authoring/vt/integration/terrain_3d_composed_surface_recook_bridge.h"
#include <assert.h>
#include <stdio.h>

int main()
{
    Terrain3DComposedSurfaceEditJournal journal;
    const t3dComposedSurfaceEditRegion_t region{ 1u, 0, 0, 4u, 4u, 3u, true };
    assert( journal.dirtyRegionCold( region, { 110.0f, 1.0f, 112.0f, 2.0f }, 2u, 99u, 1u ) );
    t3dComposedSurfaceEditTicket_t ticket{};
    assert( journal.claimCold( 1u, ticket ) );
    t3dComposedSurfaceCookPage_t pages[21]{};
    uint32_t n = 0u;
    for ( uint32_t mip = 0u; mip < 3u; ++mip ) {
        const uint32_t size = 4u >> mip;
        for ( uint32_t y = 0u; y < size; ++y )
            for ( uint32_t x = 0u; x < size; ++x )
                pages[n++] = { 123u, 5u, mip, x, y, 1u };
    }
    uint8_t flags[21]{};
    assert( t3d_cs_make_recook_flags_cold( ticket, 123u, 4u, 4u, pages, n, flags, 21u ) );
    uint32_t changed = 0;
    for ( uint8_t flag : flags ) changed += flag;
    assert( changed == 3u ); // leaf, mip1 ancestor, root
    pages[20].m_nRegionKey = 124u;
    assert( !t3d_cs_make_recook_flags_cold( ticket, 123u, 4u, 4u, pages, n, flags, 21u ) );
    pages[20].m_nRegionKey = 123u;
    t3dComposedSurfaceCookSource_t source{};
    source.m_nSourceFingerprint = 99u;
    uint64_t written = 222u;
    assert( !t3d_cs_recook_journal_ticket_cold( journal, ticket, 123u, 4u, 4u,
                                                  pages, n, source, nullptr, 0u,
                                                  flags, 21u, nullptr, 0u,
                                                  nullptr, 0u, written ) );
    assert( written == 0u ); // parity gate rejects incomplete source evaluator
    assert( journal.acceptCold( ticket, 99u, 1u ) );
    puts( "CS07 recook bridge: PASS" );
}
