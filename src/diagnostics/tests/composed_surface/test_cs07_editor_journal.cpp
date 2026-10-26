#include "authoring/vt/integration/terrain_3d_composed_surface_edit_journal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static t3dComposedSurfaceEditRegion_t region( uint32_t id, int32_t x, int32_t z,
                                               uint16_t size = 4u, uint16_t mips = 3u )
{
    return { id, x, z, size, size, mips, true };
}

int main()
{
    Terrain3DComposedSurfaceEditJournal journal;
    const auto r0 = region( 512u, 0, 0 );
    const auto r1 = region( 513u, 1, 0 );
    // 6-texel expansion includes a neighboring region at the boundary.
    assert( journal.dirtyRegionCold( r0, { 127.0f, 5.0f, 128.0f, 6.0f }, 2u, 7u, 1u ) );
    assert( journal.dirtyRegionCold( r1, { 127.0f, 5.0f, 128.0f, 6.0f }, 2u, 7u, 1u ) );
    assert( journal.statsCold().m_nDirtyRegions == 2u );
    assert( journal.regionCold( 512u )->m_nDirtyMip0Mask == 8u );
    assert( journal.regionCold( 513u )->m_nDirtyMip0Mask == 1u );
    assert( journal.regionCold( 512u )->m_nDirtyClosure & ( 1u << 17u ) );
    assert( journal.regionCold( 512u )->m_nDirtyClosure & ( 1u << 20u ) );
    t3dComposedSurfaceEditTicket_t ticket{};
    assert( journal.claimCold( 512u, ticket ) );
    assert( !journal.claimCold( 512u, ticket ) );
    assert( !journal.acceptCold( ticket, 8u, 1u ) ); // changed material fingerprint
    assert( journal.statsCold().m_nStaleCompletions == 1u );
    assert( journal.claimCold( 512u, ticket ) );
    // Another edit before the cook completes supersedes the claimed revision.
    assert( journal.dirtyRegionCold( r0, { 55.0f, 55.0f, 57.0f, 57.0f }, 2u, 7u, 1u ) );
    assert( !journal.acceptCold( ticket, 7u, 1u ) );
    assert( journal.regionCold( 512u )->m_nDirtyMip0Mask != 0u );
    assert( journal.claimCold( 512u, ticket ) );
    assert( journal.completionCurrentCold( ticket, 7u, 1u ) );
    assert( journal.acceptCold( ticket, 7u, 1u ) );
    assert( journal.statsCold().m_nDirtyRegions == 1u );
    assert( !journal.acceptCold( ticket, 7u, 1u ) );

    // Global source change dirties all 16 base pages and the ancestor closure.
    assert( journal.invalidateRegionCold( r0, 9u, 2u ) );
    assert( journal.regionCold( 512u )->m_nDirtyMip0Mask == 0xffffu );
    assert( journal.regionCold( 512u )->m_nDirtyClosure == 0x1fffffu );
    assert( journal.claimCold( 512u, ticket ) );
    assert( !journal.acceptCold( ticket, 7u, 2u ) );
    assert( journal.claimCold( 512u, ticket ) );
    assert( journal.acceptCold( ticket, 9u, 2u ) );

    assert( !journal.dirtyRegionCold( r0, { 133.0f, 5.0f, 140.0f, 6.0f }, 2u, 9u, 2u ) );
    assert( !journal.dirtyRegionCold( r0, { 127.0f, 5.0f, 127.0f, 6.0f }, 2u, 9u, 2u ) );
    assert( !journal.dirtyRegionCold( r0, { 127.0f, 5.0f, 128.0f, 6.0f }, 0u, 9u, 2u ) );
    assert( !journal.dirtyRegionCold( r0, { 127.0f, 5.0f, 128.0f, 6.0f }, 2u, 0u, 2u ) );
    assert( !journal.invalidateRegionCold( region( 1024u, 0, 0 ), 9u, 2u ) );
    assert( !journal.claimCold( 1024u, ticket ) );
    assert( !journal.regionCold( 1024u ) );
    // A late completion/cancel from an earlier claim cannot unclaim a new one.
    assert( journal.invalidateRegionCold( r0, 19u, 3u ) );
    t3dComposedSurfaceEditTicket_t oldTicket{}, newTicket{};
    assert( journal.claimCold( 512u, oldTicket ) );
    journal.cancelCold( oldTicket );
    assert( journal.dirtyRegionCold( r0, { 50.0f, 50.0f, 60.0f, 60.0f }, 2u, 19u, 3u ) );
    assert( journal.claimCold( 512u, newTicket ) );
    journal.cancelCold( oldTicket );
    assert( journal.completionCurrentCold( newTicket, 19u, 3u ) );
    assert( journal.forgetRegionCold( 512u, 4u ) );
    assert( !journal.completionCurrentCold( newTicket, 19u, 3u ) );
    assert( journal.statsCold().m_nRemovedRegions == 1u );
    assert( !journal.regionCold( 512u )->m_nDirtyMip0Mask );
    assert( !journal.claimCold( 512u, newTicket ) );
    journal.resetCold();
    assert( journal.statsCold().m_nDirtyRegions == 0u );
    puts( "CS07 edit journal: PASS" );
    return 0;
}
