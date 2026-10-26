#include "rendering/material/terrain_3d_material_tile_class.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t state = 0x913c34b7u;
static uint32_t next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static uint8_t oracle( const uint32_t *p, uint32_t tx, uint32_t ty ) {
    if ( tx == 0 || ty == 0 || tx == 7 || ty == 7 ) return T3D_MATERIAL_TILE_UNKNOWN;
    uint32_t observed[1024];
    uint32_t nObserved = 0;
    for ( uint32_t y = ty * 16u - 1u; y <= ( ty + 1u ) * 16u + 1u; ++y ) {
        for ( uint32_t x = tx * 16u - 1u; x <= ( tx + 1u ) * 16u + 1u; ++x ) {
            uint32_t raw = p[y * 128u + x];
            if ( raw & 1u ) return T3D_MATERIAL_TILE_UNKNOWN;
            uint32_t key = raw & T3D_BR22_CONTROL_SAMPLE_KEY_MASK;
            bool found = false;
            for ( uint32_t i = 0; i < nObserved; ++i ) found |= observed[i] == key;
            if ( !found ) observed[nObserved++] = key;
        }
    }
    return nObserved == 1u ? T3D_MATERIAL_TILE_HOMOGENEOUS :
           nObserved == 2u ? T3D_MATERIAL_TILE_TWO_KEY : T3D_MATERIAL_TILE_GENERAL;
}

int main() {
    static uint32_t control[128 * 128]{};
    t3dMaterialTileRegionClasses_t region{};
    for ( uint32_t caseId = 0; caseId < 72; ++caseId ) {
        for ( uint32_t i = 0; i < 128u * 128u; ++i ) {
            uint32_t v = next();
            uint32_t choice = ( v >> 21u ) & ( ( caseId % 5u ) == 0u ? 0u :
                                        ( caseId % 5u ) == 1u ? 1u :
                                        ( caseId % 5u ) == 2u ? 3u : 7u );
            control[i] = ( choice << 27u ) | ( ( v & 255u ) << 14u );
            if ( caseId % 5u == 4u && ( v & 31u ) == 0u ) control[i] |= 1u;
        }
        assert( t3d_material_tiles_rebuild( region, control, 128, 128 ) );
        for ( uint32_t y = 0u; y < 8u; ++y )
            for ( uint32_t x = 0u; x < 8u; ++x )
                assert( region.m_szClass[y * 8u + x] == oracle( control, x, y ) );
        const uint32_t rev = region.m_nRevision;
        assert( t3d_material_tiles_certify_batch( &region, 1u, &rev ) == T3D_MATERIAL_TILE_UNKNOWN );
        t3d_material_tiles_invalidate( region );
        assert( !region.m_bReady && region.m_nRevision != rev );
        assert( t3d_material_tiles_certify_batch( &region, 1u, &rev ) == T3D_MATERIAL_TILE_UNKNOWN );
    }
    // The two-key case must include the bilinear halo, not just tile interiors.
    memset( control, 0, sizeof( control ) );
    assert( t3d_material_tile_classify( control, 128, 128, 3, 3 ) == T3D_MATERIAL_TILE_HOMOGENEOUS );
    control[47u * 128u + 47u] = 1u << 27u; // tile-3 left/top halo
    assert( t3d_material_tile_classify( control, 128, 128, 3, 3 ) == T3D_MATERIAL_TILE_TWO_KEY );
    control[49u * 128u + 49u] = 2u << 27u;
    assert( t3d_material_tile_classify( control, 128, 128, 3, 3 ) == T3D_MATERIAL_TILE_GENERAL );
    control[50u * 128u + 50u] = 1u;
    assert( t3d_material_tile_classify( control, 128, 128, 3, 3 ) == T3D_MATERIAL_TILE_UNKNOWN );
    // A draw containing only provably interior tiles can be certified, while
    // inclusion of an unknown boundary or a stale revision rejects it.
    memset( control, 0, sizeof( control ) );
    assert( t3d_material_tiles_rebuild( region, control, 128u, 128u ) );
    const uint32_t current = region.m_nRevision;
    const uint64_t center = uint64_t( 1 ) << ( 3u * 8u + 3u );
    assert( t3d_material_tiles_certify_covered_batch( &region, 1u, &current, &center ) == T3D_MATERIAL_TILE_HOMOGENEOUS );
    const uint64_t includesBorder = center | 1u;
    assert( t3d_material_tiles_certify_covered_batch( &region, 1u, &current, &includesBorder ) == T3D_MATERIAL_TILE_UNKNOWN );
    const uint64_t none = 0u;
    assert( t3d_material_tiles_certify_covered_batch( &region, 1u, &current, &none ) == T3D_MATERIAL_TILE_UNKNOWN );
    t3d_material_tiles_invalidate( region );
    assert( t3d_material_tiles_certify_covered_batch( &region, 1u, &current, &center ) == T3D_MATERIAL_TILE_UNKNOWN );
    puts("BR14 material tile/halo oracle PASS");
}
