#include "authoring/vt/cutout/terrain_3d_cutout_cooker.h"
#include "rendering/rd/vt/cutout/terrain_3d_cutout_reader.h"
#include "rendering/rd/vhm/terrain_3d_st_sidecar_wire.h"
#include "diagnostics/tests/superterrain/st_region_contract.h"

#include <assert.h>
#include <stdio.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static_assert( T3D_ST_MASK_PAGE_BYTES == T3D_ST_CUTOUT_PAGE_BYTES, "ST03 contract drift" );
static_assert( T3D_ST_MASK_ENTRY_VALID_FLAG == T3D_ST_CUTOUT_VALID, "ST03 contract drift" );
static_assert( T3D_ST_MASK_ENTRY_UNIFORM_FLAG == T3D_ST_CUTOUT_UNIFORM, "ST03 contract drift" );

static bool readFile( const std::string &rPath, std::vector<uint8_t> &rBytes )
{
    std::ifstream f( rPath, std::ios::binary );
    if ( !f ) return false;
    rBytes.assign( std::istreambuf_iterator<char>( f ), std::istreambuf_iterator<char>() );
    return true;
}

static void checkFixture( const std::string &rRoot, const char *pszName )
{
    std::vector<uint8_t> source;
    assert( readFile( rRoot + "/" + pszName + ".st00region", source ) );
    assert( source.size() == 32u + 128u * 128u * 12u );
    const uint32_t region = t3dStLoadLe16( source.data() + 8u );
    const uint8_t *pControl = source.data() + 32u + 128u * 128u * 2u;
    const uint8_t *pColor = pControl + 128u * 128u * 4u;
    t3dStCutoutCookInput_t input = {};
    input.m_pControlLe = pControl;
    input.m_pColorRgba8 = pColor;
    input.m_nRegionIndex = region;
    input.m_nGeneration = 42u;
    input.m_nSourceSignature = 0x1234567812345678ull;
    input.m_nRecipeSignature = 0x9876543212345678ull;
    std::vector<uint8_t> sidecar;
    assert( Terrain3DCutoutCooker::cookRegionCold( input, sidecar ) );
    assert( sidecar.size() >= 48u + 64u );
    Terrain3DCutoutRegionView view;
    assert( view.open( sidecar.data(), sidecar.size(), region, 42u,
                       input.m_nSourceSignature, input.m_nRecipeSignature ) );
    assert( view.getRootIndex() == region * 4u );
    uint32_t mixed = 0u;
    for ( uint32_t i = 0u; i < 4u; ++i )
    {
        if ( ( view.getEntry( i ).m_nPackedState & T3D_ST_CUTOUT_UNIFORM ) == 0u ) ++mixed;
    }
    assert( sidecar.size() == 48u + 64u + mixed * 1536u );
    assert( view.getPinnedWordCount() == mixed * 384u );
    for ( uint32_t y = 0u; y < 128u; ++y )
    {
        for ( uint32_t x = 0u; x < 128u; ++x )
        {
            const uint32_t pixel = y * 128u + x;
            const uint8_t expected = t3dStCutoutSourceValue(
                t3dStLoadLe32( pControl + pixel * 4u ), pColor[pixel * 4u + 3u] );
            uint8_t actual = 99u;
            assert( view.sample( x, y, 42u, actual ) && actual == expected );
        }
    }
    uint8_t invalid = 7u;
    assert( !view.sample( 128u, 0u, 42u, invalid ) );
    assert( !view.sample( 0u, 128u, 42u, invalid ) );
    assert( !view.sample( 0u, 0u, 41u, invalid ) );
    std::vector<uint32_t> pinned = { 0xdeadbeefu };
    t3dStCutoutEntry_t gpu[4] = {};
    assert( view.appendGpuPayload( pinned, gpu, 1u + 1024u * 384u ) );
    assert( pinned.size() == size_t( 1u + view.getPinnedWordCount() ) );
    for ( uint32_t page = 0u; page < 4u; ++page )
    {
        if ( ( gpu[page].m_nPackedState & T3D_ST_CUTOUT_UNIFORM ) == 0u )
        {
            assert( gpu[page].m_nPinnedWordOffset == view.getEntry( page ).m_nPinnedWordOffset + 1u );
        }
    }
    if ( mixed > 0u )
    {
        const size_t oldSize = pinned.size();
        assert( !view.appendGpuPayload( pinned, gpu, uint32_t( oldSize ) ) );
        assert( pinned.size() == oldSize );
    }
    const std::vector<uint8_t> validCopy = sidecar;
    std::vector<uint8_t> corrupted = sidecar;
    corrupted.back() ^= 1u;
    assert( !view.open( corrupted.data(), corrupted.size(), region, 42u,
                        input.m_nSourceSignature, input.m_nRecipeSignature ) );
    assert( view.sample( 64u, 64u, 42u, invalid ) ); // last-good view still valid
    assert( !view.open( validCopy.data(), validCopy.size(), region, 43u,
                        input.m_nSourceSignature, input.m_nRecipeSignature ) );
    assert( !view.open( validCopy.data(), validCopy.size(), region, 42u,
                        input.m_nSourceSignature + 1u, input.m_nRecipeSignature ) );
    // Valid CRC but invalid page descriptor must fail structural validation.
    corrupted = validCopy;
    t3dStStoreLe32( corrupted.data() + 48u + 4u, 23u );
    t3dStStoreLe32( corrupted.data() + 40u,
        t3dStPayloadCrc32( corrupted.data() + 48u, corrupted.size() - 48u ) );
    assert( !view.open( corrupted.data(), corrupted.size(), region, 42u,
                        input.m_nSourceSignature, input.m_nRecipeSignature ) );
    std::vector<uint8_t> sentinel = { 1u, 2u, 3u };
    input.m_pColorRgba8 = nullptr;
    assert( !Terrain3DCutoutCooker::cookRegionCold( input, sentinel ) );
    assert( sentinel.size() == 3u && sentinel[0] == 1u );
}

int main( int argc, char **argv )
{
    assert( argc == 2 );
    const char *names[] = { "flat", "empty_holes", "negative_steep", "cliff",
        "border_west", "border_east", "coverage_edges", "alpha_extremes",
        "sparse_controls", "wet_surface", "rotated_slopes", "distance_regimes" };
    for ( const char *name : names ) checkFixture( argv[1], name );
    puts( "ST03: exact source bits, sentinels, compact mixed payload, GPU relocation, signatures, CRC, fail-closed: PASS" );
    return 0;
}
