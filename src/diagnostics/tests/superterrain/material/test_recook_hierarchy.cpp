#include "authoring/vt/integration/terrain_3d_composed_surface_recook_bridge.h"

#include <cassert>
#include <cstdio>

int main( void )
{
    t3dComposedSurfaceEditTicket_t ticket{};
    ticket.m_nRevision = 1u;
    ticket.m_nSourceFingerprint = 9u;
    ticket.m_nDirtyClosure = ( 1u << 0u ) | ( 1u << 16u ) | ( 1u << 20u );
    t3dComposedSurfaceCookPage_t pages[21]{};
    uint32_t count = 0u;
    for ( uint32_t mip = 0u; mip < 3u; ++mip ) {
        const uint32_t width = 4u >> mip;
        for ( uint32_t y = 0u; y < width; ++y ) {
            for ( uint32_t x = 0u; x < width; ++x ) {
                pages[count] = { 1u, uint64_t( count + 1u ), mip, x, y, 1u };
                ++count;
            }
        }
    }
    assert( count == 21u );
    uint8_t dirty[21]{};
    assert( t3d_comp_surf_make_recook_flags_cold( ticket, 1u, 4u, 4u, pages, count, dirty, count ) );
    uint32_t dirtyCount = 0u;
    for ( uint8_t flag : dirty ) dirtyCount += flag;
    assert( dirtyCount == 3u );
    assert( !t3d_comp_surf_make_recook_flags_cold( ticket, 1u, 4u, 4u, pages, count - 1u, dirty, count ) );
    const auto saved = pages[1];
    pages[1] = pages[0];
    assert( !t3d_comp_surf_make_recook_flags_cold( ticket, 1u, 4u, 4u, pages, count, dirty, count ) );
    pages[1] = saved;
    pages[1].m_nPageX = 4u;
    assert( !t3d_comp_surf_make_recook_flags_cold( ticket, 1u, 4u, 4u, pages, count, dirty, count ) );
    pages[1] = saved;
    pages[1] = saved;
    pages[1].m_nMip = UINT32_MAX;
    assert( !t3d_comp_surf_make_recook_flags_cold( ticket, 1u, 4u, 4u, pages, count, dirty, count ) );
    pages[1] = saved;
    ticket.m_nDirtyClosure |= ( 1u << 22u );
    assert( !t3d_comp_surf_make_recook_flags_cold( ticket, 1u, 4u, 4u, pages, count, dirty, count ) );
    ticket.m_nDirtyClosure &= ~( 1u << 22u );
    assert( t3d_comp_surf_make_recook_flags_cold( ticket, 1u, 4u, 4u, pages, count, dirty, count ) );
    std::puts( "ST01 recook exact hierarchy: PASS" );
    return 0;
}
