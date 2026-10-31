#include "authoring/vhm/terrain_3d_vhm_cooker.h"
#include "rendering/rd/vhm/terrain_3d_vhm_reader.h"
#include "rendering/rd/vhm/terrain_3d_vhm_residency.h"
#include "rendering/rd/vhm/terrain_3d_st_sidecar_wire.h"
#include "diagnostics/tests/superterrain/st_region_contract.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static_assert( T3D_ST_VHM_PAGE_BYTES == 9248u, "ST04 contract drift" );
static_assert( T3D_ST_VHM_PAGES_PER_REGION == 11u, "ST04 contract drift" );
static_assert( sizeof( t3dStVhmPageEntry_t ) == sizeof( t3dStRegionVhmPageEntry_t ), "ST04 contract drift" );
static_assert( T3D_ST_VHM_TAIL_SLOT_HEIGHT == 20u, "ST04 tail contract drift" );

static bool readFile( const std::string &rPath, std::vector<uint8_t> &rBytes )
{
    std::ifstream f( rPath, std::ios::binary );
    if ( !f ) return false;
    rBytes.assign( std::istreambuf_iterator<char>( f ), std::istreambuf_iterator<char>() );
    return true;
}
static uint32_t sourceRegion( const std::vector<uint8_t> &rSource )
{
    assert( rSource.size() == 32u + 128u * 128u * 12u );
    return t3dStLoadLe16( rSource.data() + 8u );
}
static void checkFixture( const std::string &rRoot, const char *pszName )
{
    std::vector<uint8_t> source;
    assert( readFile( rRoot + "/" + pszName + ".st00region", source ) );
    t3dStVhmCookInput_t input = {};
    input.m_pHeightLe[1][1] = source.data() + 32u;
    input.m_nNeighborGeneration[1][1] = 42u;
    input.m_nRegionIndex = sourceRegion( source );
    input.m_nGeneration = 42u;
    input.m_nSourceSignature = 0x1234567812345678ull;
    input.m_nRecipeSignature = 0x9876543212345678ull;
    std::vector<uint8_t> sidecar;
    assert( Terrain3DVhmCooker::cookRegionCold( input, sidecar ) );
    assert( sidecar.size() == 48u + T3D_ST_VHM_PAYLOAD_BYTES );
    Terrain3DVhmRegionView view;
    assert( view.open( sidecar.data(), sidecar.size(), input.m_nRegionIndex, 42u,
                       input.m_nSourceSignature, input.m_nRecipeSignature ) );
    assert( view.getRootIndex() == input.m_nRegionIndex );
    assert( view.getFinePageBytes( 0u ) != nullptr );
    assert( view.getFinePageBytes( 6u ) == nullptr );
    assert( view.getTailSlotBytes() != nullptr );
    for ( uint32_t y = 0u; y < 128u; ++y )
    {
        for ( uint32_t x = 0u; x < 128u; ++x )
        {
            uint16_t got = 0u;
            uint32_t resolved = 0xffffffffu;
            assert( view.sampleR16( x, y, 0u, 0x3fu, 42u, got, resolved ) );
            assert( resolved == 0u );
            assert( got == t3dStLoadLe16( source.data() + 32u + ( y * 128u + x ) * 2u ) );
        }
    }
    // Every coarser level must agree with the signed-height reduction to
    // within one canonical R16 LSB, including negative slopes and cliffs.
    for ( uint32_t mip = 1u; mip < 8u; ++mip )
    {
        const uint32_t side = 128u >> mip;
        for ( uint32_t y = 0u; y < side; ++y )
        {
            for ( uint32_t x = 0u; x < side; ++x )
            {
                uint16_t coarse = 0u;
                uint32_t resolved = 99u;
                assert( view.sampleR16( x << mip, y << mip, mip, 0x3fu, 42u, coarse, resolved ) );
                assert( resolved == mip );
                uint32_t sum = 0u;
                for ( uint32_t dy = 0u; dy < 2u; ++dy )
                {
                    for ( uint32_t dx = 0u; dx < 2u; ++dx )
                    {
                        uint16_t prior = 0u;
                        assert( view.sampleR16( ( x * 2u + dx ) << ( mip - 1u ),
                                                ( y * 2u + dy ) << ( mip - 1u ),
                                                mip - 1u, 0x3fu, 42u, prior, resolved ) );
                        sum += prior;
                    }
                }
                const int difference = int( coarse ) - int( ( sum + 2u ) / 4u );
                assert( std::abs( difference ) <= 1 );
            }
        }
    }
    // Parent walk and guaranteed pinned roots on forced eviction.
    for ( uint32_t mip = 0u; mip < 8u; ++mip )
    {
        for ( uint32_t y = 0u; y < 128u; y += 17u )
        {
            for ( uint32_t x = 0u; x < 128u; x += 17u )
            {
                uint16_t got = 0u;
                uint32_t resolved = 99u;
                assert( view.sampleR16( x, y, mip, 0u, 42u, got, resolved ) );
                assert( resolved == ( mip < 3u ? 3u : mip ) );
                const double meters = -384.0 + double( got ) * ( 1152.0 / 65535.0 );
                assert( meters >= -384.0 && meters <= 768.0 );
            }
        }
    }
    uint16_t h = 0u;
    uint32_t resolved = 99u;
    assert( !view.sampleR16( 128u, 0u, 0u, 0x3fu, 42u, h, resolved ) );
    assert( !view.sampleR16( 0u, 0u, 8u, 0x3fu, 42u, h, resolved ) );
    assert( !view.sampleR16( 0u, 0u, 0u, 0x3fu, 41u, h, resolved ) );
    assert( view.sampleR16( 0u, 0u, 0u, 0x10u, 42u, h, resolved ) && resolved == 1u );
    assert( view.sampleR16( 0u, 0u, 0u, 0x20u, 42u, h, resolved ) && resolved == 2u );
    const std::vector<uint8_t> validCopy = sidecar;
    std::vector<uint8_t> corrupted = sidecar;
    corrupted.back() ^= 1u;
    assert( !view.open( corrupted.data(), corrupted.size(), input.m_nRegionIndex, 42u,
                        input.m_nSourceSignature, input.m_nRecipeSignature ) );
    assert( !view.open( validCopy.data(), validCopy.size(), input.m_nRegionIndex, 43u,
                        input.m_nSourceSignature, input.m_nRecipeSignature ) );
    corrupted = validCopy;
    t3dStStoreLe32( corrupted.data() + 48u + 4u, 9u );
    t3dStStoreLe32( corrupted.data() + 40u,
        t3dStPayloadCrc32( corrupted.data() + 48u, corrupted.size() - 48u ) );
    assert( !view.open( corrupted.data(), corrupted.size(), input.m_nRegionIndex, 42u,
                        input.m_nSourceSignature, input.m_nRecipeSignature ) );
    std::vector<uint8_t> sentinel = { 5u, 4u, 3u };
    input.m_pHeightLe[1][1] = nullptr;
    assert( !Terrain3DVhmCooker::cookRegionCold( input, sentinel ) );
    assert( sentinel.size() == 3u && sentinel[0] == 5u );
}

static void checkNeighborGutters( const std::string &rRoot )
{
    std::vector<uint8_t> west, east;
    assert( readFile( rRoot + "/border_west.st00region", west ) );
    assert( readFile( rRoot + "/border_east.st00region", east ) );
    t3dStVhmCookInput_t input = {};
    input.m_pHeightLe[1][1] = west.data() + 32u;
    input.m_pHeightLe[1][2] = east.data() + 32u;
    input.m_nNeighborGeneration[1][1] = 42u;
    input.m_nNeighborGeneration[1][2] = 42u;
    input.m_nRegionIndex = sourceRegion( west );
    input.m_nGeneration = 42u;
    input.m_nSourceSignature = 3u;
    input.m_nRecipeSignature = 5u;
    std::vector<uint8_t> sidecar;
    assert( Terrain3DVhmCooker::cookRegionCold( input, sidecar ) );
    Terrain3DVhmRegionView view;
    assert( view.open( sidecar.data(), sidecar.size(), input.m_nRegionIndex, 42u, 3u, 5u ) );
    const uint8_t *pRightPage = view.getFinePageBytes( 1u );
    assert( pRightPage );
    for ( uint32_t y = 0u; y < 128u; y += 11u )
    {
        const uint8_t *pPage = view.getFinePageBytes( y >= 64u ? 3u : 1u );
        const uint32_t localY = y % 64u + 2u;
        const uint16_t edge = t3dStLoadLe16( pPage + ( localY * 68u + 66u ) * 2u );
        const uint16_t expected = t3dStLoadLe16( east.data() + 32u + y * 128u * 2u );
        assert( edge == expected );
    }
    // Stale neighbor is excluded; gutter clamps to this region's last texel.
    input.m_nNeighborGeneration[1][2] = 41u;
    assert( Terrain3DVhmCooker::cookRegionCold( input, sidecar ) );
    assert( view.open( sidecar.data(), sidecar.size(), input.m_nRegionIndex, 42u, 3u, 5u ) );
    const uint16_t clamped = t3dStLoadLe16( view.getFinePageBytes( 1u ) + ( 2u * 68u + 66u ) * 2u );
    const uint16_t expectedClamp = t3dStLoadLe16( west.data() + 32u + 127u * 2u );
    assert( clamped == expectedClamp );
}

static void checkGpuStaging( const std::string &rRoot )
{
    std::vector<uint8_t> source;
    assert( readFile( rRoot + "/cliff.st00region", source ) );
    t3dStVhmCookInput_t input = {};
    input.m_pHeightLe[1][1] = source.data() + 32u;
    input.m_nNeighborGeneration[1][1] = 42u;
    input.m_nRegionIndex = sourceRegion( source );
    input.m_nGeneration = 42u;
    input.m_nSourceSignature = 3u;
    input.m_nRecipeSignature = 5u;
    std::vector<uint8_t> sidecar;
    assert( Terrain3DVhmCooker::cookRegionCold( input, sidecar ) );
    Terrain3DVhmRegionView view;
    assert( view.open( sidecar.data(), sidecar.size(), input.m_nRegionIndex, 42u, 3u, 5u ) );
    Terrain3DVhmRootAtlas roots;
    assert( roots.initializeCold() );
    assert( roots.getAtlasBytes() == 2129920u && roots.getTailCount() == 1024u );
    assert( roots.stageRootCold( view ) );
    const t3dStVhmTail_t &tail = roots.getTailEntries()[input.m_nRegionIndex];
    assert( tail.m_nGeneration == 42u );
    const size_t off = ( size_t( tail.m_nAtlasY ) * T3D_ST_VHM_TAIL_ATLAS_WIDTH + tail.m_nAtlasX ) * 2u;
    assert( t3dStLoadLe16( roots.getAtlasPixels() + off ) == t3dStLoadLe16( view.getTailSlotBytes() ) );

    Terrain3DVhmFinePagePool pool;
    assert( !pool.initializeCold( 0u ) );
    assert( pool.initializeCold( 2u ) );
    assert( pool.getSlotCount() == 2u && pool.getFreeCount() == 2u );
    uint32_t a = 99u, b = 99u, ticketA = 0u, ticketB = 0u;
    assert( pool.preparePageHot( view, 0u, 42u, a, ticketA ) );
    assert( pool.getFreeCount() == 1u && ticketA != 0u );
    assert( pool.getUploadBytes( a, ticketA ) != nullptr );
    assert( !pool.isResident( input.m_nRegionIndex * 11u, 42u, b ) );
    assert( pool.preparePageHot( view, 0u, 42u, b, ticketB ) && b == a && ticketA == ticketB );
    assert( pool.finishUpload( a, ticketA ) );
    assert( pool.isResident( input.m_nRegionIndex * 11u, 42u, b ) && b == a );
    assert( !pool.finishUpload( a, ticketA ) );
    assert( pool.preparePageHot( view, 1u, 42u, b, ticketB ) );
    assert( pool.getFreeCount() == 0u );
    uint32_t noSlot = 0xccccu, noTicket = 0xdddd;
    assert( !pool.preparePageHot( view, 2u, 42u, noSlot, noTicket ) );
    assert( noSlot == 0xccccu && noTicket == 0xddddu );
    assert( !pool.retireAfterFence( a, ticketA + 1u ) );
    assert( pool.retireAfterFence( a, ticketA ) );
    assert( pool.getFreeCount() == 1u );
    assert( !pool.isResident( input.m_nRegionIndex * 11u, 42u, a ) );
    assert( pool.preparePageHot( view, 2u, 42u, a, ticketA ) );
    assert( a != b && ticketA != ticketB );
    assert( pool.finishUpload( a, ticketA ) );
    assert( pool.isResident( input.m_nRegionIndex * 11u + 2u, 42u, a ) );
    t3dStVhmPageEntry_t table[11] = {};
    assert( pool.buildPageEntries( view, 42u, table ) );
    assert( ( table[2].m_nPackedMipFlags & T3D_ST_VHM_RESIDENT ) != 0u );
    assert( table[2].m_nPhysicalSlot == a );
    assert( table[0].m_nPhysicalSlot == T3D_ST_VHM_INVALID );
    assert( ( table[3].m_nPackedMipFlags & T3D_ST_VHM_RESIDENT ) == 0u );
    assert( ( table[6].m_nPackedMipFlags & T3D_ST_VHM_PINNED ) != 0u );
    assert( !pool.buildPageEntries( view, 41u, table ) );
    assert( !pool.preparePageHot( view, 0u, 41u, a, ticketA ) );
    assert( !pool.preparePageHot( view, 6u, 42u, a, ticketA ) );
}

int main( int argc, char **argv )
{
    assert( argc == 2 );
    const char *names[] = { "flat", "empty_holes", "negative_steep", "cliff",
        "border_west", "border_east", "coverage_edges", "alpha_extremes",
        "sparse_controls", "wet_surface", "rotated_slopes", "distance_regimes" };
    for ( const char *name : names ) checkFixture( argv[1], name );
    checkNeighborGutters( argv[1] );
    checkGpuStaging( argv[1] );
    puts( "ST04: mip0 exact, 11 stable IDs, 6 fine pages, compact pinned ancestors, borders, signatures, CRC: PASS" );
    return 0;
}
