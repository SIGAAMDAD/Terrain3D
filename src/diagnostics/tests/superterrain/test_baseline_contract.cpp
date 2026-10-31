#include "st_region_contract.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void testPageIdentities( void )
{
    for ( uint32_t region = 0u; region < T3D_ST_REGION_COUNT; ++region ) {
        bool seen[ T3D_ST_VHM_PAGES_PER_REGION ] = {};
        uint32_t count = 0u;
        for ( uint32_t mip = 0u; mip < T3D_ST_VHM_MIP_COUNT; ++mip ) {
            const uint32_t side = mip == 0u ? 2u : 1u;
            for ( uint32_t y = 0u; y < side; ++y ) {
                for ( uint32_t x = 0u; x < side; ++x ) {
                    const uint32_t id = t3dStVhmPageId( region, mip, x, y );
                    assert( id >= region * T3D_ST_VHM_PAGES_PER_REGION );
                    const uint32_t local = id - region * T3D_ST_VHM_PAGES_PER_REGION;
                    assert( local < T3D_ST_VHM_PAGES_PER_REGION && !seen[local] );
                    seen[local] = true;
                    ++count;
                    const uint32_t parent = t3dStVhmParentPageId( region, mip, x, y );
                    if ( mip == T3D_ST_VHM_MIP_COUNT - 1u ) {
                        assert( parent == T3D_ST_INVALID_ID );
                    } else {
                        assert( parent == t3dStVhmPageId( region, mip + 1u, x >> 1u, y >> 1u ) );
                    }
                }
            }
        }
        assert( count == T3D_ST_VHM_PAGES_PER_REGION );
        for ( uint32_t page = 0u; page < T3D_ST_MASK_PAGES_PER_REGION; ++page ) {
            assert( t3dStMaskPageId( region, page % 2u, page / 2u ) ==
                    region * T3D_ST_MASK_PAGES_PER_REGION + page );
        }
    }
    assert( t3dStVhmPageId( T3D_ST_REGION_COUNT, 0u, 0u, 0u ) == T3D_ST_INVALID_ID );
    assert( t3dStVhmPageId( 0u, 8u, 0u, 0u ) == T3D_ST_INVALID_ID );
    assert( t3dStVhmPageId( 0u, 0u, 2u, 0u ) == T3D_ST_INVALID_ID );
    assert( t3dStVhmParentPageId( 0u, 0u, 2u, 0u ) == T3D_ST_INVALID_ID );
    assert( t3dStMaskPageId( 0u, 0u, 2u ) == T3D_ST_INVALID_ID );
}

static void testExactMask( void )
{
    uint32_t planes[ T3D_ST_MASK_PLANE_WORDS * T3D_ST_MASK_BITPLANES ] = {};
    for ( uint32_t sample = 0u; sample < T3D_ST_MASK_PAGE_INTERIOR * T3D_ST_MASK_PAGE_INTERIOR; ++sample ) {
        const uint32_t value = ( sample ^ ( sample >> 5u ) ) & 7u;
        for ( uint32_t plane = 0u; plane < T3D_ST_MASK_BITPLANES; ++plane ) {
            planes[plane * T3D_ST_MASK_PLANE_WORDS + ( sample >> 5u )] |=
                ( ( value >> plane ) & 1u ) << ( sample & 31u );
        }
    }
    for ( uint32_t y = 0u; y < T3D_ST_MASK_PAGE_INTERIOR; ++y ) {
        for ( uint32_t x = 0u; x < T3D_ST_MASK_PAGE_INTERIOR; ++x ) {
            const uint32_t sample = y * T3D_ST_MASK_PAGE_INTERIOR + x;
            assert( t3dStMaskSampleBit( planes, x, y ) == ( ( sample ^ ( sample >> 5u ) ) & 7u ) );
        }
    }
}

static void testGenerationGate( void )
{
    t3dStRegionRecord_t record = {};
    const uint32_t epoch = 9u;
    assert( !t3dStRegionReady( nullptr ) );
    assert( !t3dStRegionReady( &record ) );
    record.m_nPackedPhysicalFlags = 0x80000000u;
    record.m_nPublicationGeneration = epoch;
    record.m_nComposedVTGeneration = epoch;
    record.m_nVhmGeneration = epoch;
    record.m_nMaskGeneration = epoch;
    record.m_nComposedVTAllocation = 1u;
    record.m_nVhmRootIndex = 1u;
    record.m_nMaskRootIndex = 1u;
    record.m_nRegionStateFlags = T3D_ST_REGION_VALID_MASK;
    assert( t3dStRegionReady( &record ) );

    record.m_nMaskGeneration = epoch - 1u;
    assert( !t3dStRegionReady( &record ) );
    record.m_nMaskGeneration = epoch;
    record.m_nVhmGeneration = epoch + 1u;
    assert( !t3dStRegionReady( &record ) );
    record.m_nVhmGeneration = epoch;
    record.m_nComposedVTGeneration = 0u;
    assert( !t3dStRegionReady( &record ) );
    record.m_nComposedVTGeneration = epoch;

    uint32_t *pIdentifiers[] = { &record.m_nComposedVTAllocation, &record.m_nVhmRootIndex,
                                 &record.m_nMaskRootIndex };
    for ( uint32_t *pId : pIdentifiers ) {
        const uint32_t before = *pId;
        *pId = T3D_ST_INVALID_ID;
        assert( !t3dStRegionReady( &record ) );
        *pId = before;
    }
    for ( uint32_t bit = 0u; bit < 4u; ++bit ) {
        record.m_nRegionStateFlags ^= ( 1u << bit );
        assert( !t3dStRegionReady( &record ) );
        record.m_nRegionStateFlags ^= ( 1u << bit );
    }
    record.m_nPackedPhysicalFlags = 0u;
    assert( !t3dStRegionReady( &record ) );
    record.m_nPackedPhysicalFlags = 0x80000000u;
    record.m_nPublicationGeneration = 0u;
    assert( !t3dStRegionReady( &record ) );
}

int main( void )
{
    testPageIdentities();
    testExactMask();
    testGenerationGate();
    printf( "ST00 C++17 page identities, exact bitplanes and generation checks passed\n" );
    return 0;
}
