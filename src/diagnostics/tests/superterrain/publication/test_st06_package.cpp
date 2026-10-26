#include "authoring/vhm/terrain_3d_vhm_cooker.h"
#include "authoring/vt/cutout/terrain_3d_cutout_cooker.h"
#include "authoring/vt/build/terrain_3d_st_region_source_cooker.h"
#include "rendering/rd/superterrain/terrain_3d_st_region_package.h"
#include "rendering/rd/vhm/terrain_3d_st_sidecar_wire.h"
#include <assert.h>
#include <stdio.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

struct StFixture
{
    std::vector<uint8_t> data, vhm, mask;
    t3dStPublicationRegionInput_t input;
};
static void load( const std::string &root, const char *name, StFixture &f )
{
    std::ifstream stream( root + "/" + name + ".st00region", std::ios::binary );
    assert( stream );
    f.data.assign( std::istreambuf_iterator<char>( stream ), std::istreambuf_iterator<char>() );
    assert( f.data.size() == 32u + 128u * 128u * 12u );
    const uint8_t *h = f.data.data() + 32u;
    const uint8_t *c = h + 128u * 128u * 2u;
    const uint8_t *a = c + 128u * 128u * 4u;
    uint32_t id = t3dStLoadLe16( f.data.data() + 8u );
    t3dStVhmCookInput_t height = {};
    height.m_pHeightLe[1][1] = h;
    height.m_nNeighborGeneration[1][1] = 42u;
    height.m_nRegionIndex = id;
    height.m_nGeneration = 42u;
    height.m_nSourceSignature = 0x12345678u;
    height.m_nRecipeSignature = 0x87654321u;
    assert( Terrain3DVhmCooker::cookRegionCold( height, f.vhm ) );
    t3dStCutoutCookInput_t mask = {};
    mask.m_pControlLe = c;
    mask.m_pColorRgba8 = a;
    mask.m_nRegionIndex = id;
    mask.m_nGeneration = 42u;
    mask.m_nSourceSignature = height.m_nSourceSignature;
    mask.m_nRecipeSignature = height.m_nRecipeSignature;
    assert( Terrain3DCutoutCooker::cookRegionCold( mask, f.mask ) );
    f.input.m_nRegionIndex = id;
    f.input.m_nComposedAllocation = 30u + id * 3u;
    f.input.m_nGeneration = 42u;
    f.input.m_nPackedPhysicalFlags = 0x80000001u;
    f.input.m_nSourceSignature = height.m_nSourceSignature;
    f.input.m_nRecipeSignature = height.m_nRecipeSignature;
    f.input.m_pVhmBytes = f.vhm.data();
    f.input.m_nVhmBytes = f.vhm.size();
    f.input.m_pMaskBytes = f.mask.data();
    f.input.m_nMaskBytes = f.mask.size();
}
static bool resolve( void *context, uint32_t region, uint32_t &allocation, uint32_t &generation )
{
    const auto *inputs = ( const t3dStPublicationRegionInput_t * ) context;
    for ( uint32_t i = 0u; i < 3u; ++i )
    {
        if ( inputs[i].m_nRegionIndex == region )
        {
            allocation = inputs[i].m_nComposedAllocation;
            generation = inputs[i].m_nGeneration;
            return true;
        }
    }
    return false;
}
int main( int argc, char **argv )
{
    assert( argc == 2 );
    StFixture fixtures[3];
    load( argv[1], "flat", fixtures[0] );
    load( argv[1], "coverage_edges", fixtures[1] );
    load( argv[1], "negative_steep", fixtures[2] );
    t3dStPublicationRegionInput_t input[3] = { fixtures[0].input, fixtures[1].input, fixtures[2].input };
    std::vector<uint8_t> bytes;
    assert( Terrain3DStRegionPackage::buildCold( input, 3u, 1001u, bytes ) );
    assert( !bytes.empty() );
    Terrain3DStRegionPackage package;
    assert( package.openCold( bytes.data(), bytes.size(), 1001u, resolve, input ) );
    assert( package.regionCount() == 3u && package.vtPackageGeneration() == 1001u );
    input[0].m_nComposedAllocation += 3u;
    assert( !package.openCold( bytes.data(), bytes.size(), 1001u, resolve, input ) );
    input[0] = fixtures[0].input;
    Terrain3DStPublicationImage image;
    assert( package.preparePublicationCold( image, 8192u, 6u ) );
    assert( image.regionCount() == 3u && image.finePageCount() == 6u );
    assert( !package.openCold( bytes.data(), bytes.size(), 1002u, resolve, input ) );
    assert( package.regionCount() == 3u ); // rejected reload preserves last valid state
    std::vector<uint8_t> corrupted = bytes;
    corrupted[corrupted.size() - 1u] ^= 1u;
    assert( !package.openCold( corrupted.data(), corrupted.size(), 1001u, resolve, input ) );
    assert( package.regionCount() == 3u );
    std::vector<uint8_t> duplicate;
    input[1] = input[0];
    assert( !Terrain3DStRegionPackage::buildCold( input, 3u, 1001u, duplicate ) );
    input[1] = fixtures[1].input;
    input[0].m_nGeneration = 43u;
    assert( !Terrain3DStRegionPackage::buildCold( input, 3u, 1001u, duplicate ) );
    input[0] = fixtures[0].input;
    assert( package.preparePublicationCold( image, 8192u, 0u ) );
    t3dStAuthoredRegionSource_t authored[3] = {};
    for ( uint32_t i = 0u; i < 3u; ++i )
    {
        authored[i].m_pHeightR16 = fixtures[i].data.data() + 32u;
        authored[i].m_pControlRF = authored[i].m_pHeightR16 + 32768u;
        authored[i].m_pColorRgba8 = authored[i].m_pControlRF + 65536u;
        authored[i].m_nRegionIndex = fixtures[i].input.m_nRegionIndex;
        authored[i].m_nComposedAllocation = fixtures[i].input.m_nComposedAllocation;
        authored[i].m_nComposedGeneration = fixtures[i].input.m_nGeneration;
    }
    std::vector<uint8_t> cooked;
    assert( Terrain3DStRegionSourceCooker::cookCold( authored, 3u, 1001u, cooked ) );
    assert( package.openCold( cooked.data(), cooked.size(), 1001u, resolve, input ) );
    assert( package.preparePublicationCold( image, 8192u, 6u ) );
    assert( image.regionCount() == 3u );
    const std::vector<uint8_t> lastGood = cooked;
    authored[1].m_pControlRF = nullptr;
    assert( !Terrain3DStRegionSourceCooker::cookCold( authored, 3u, 1001u, cooked ) );
    assert( cooked == lastGood );

    // Adjacent regions may have distinct COMPOSED generations after dirty-only
    // cooks. Geometry gutters must still read the actual adjacent height.
    StFixture west, east;
    load( argv[1], "border_west", west );
    load( argv[1], "border_east", east );
    assert( west.input.m_nRegionIndex + 1u == east.input.m_nRegionIndex );
    t3dStAuthoredRegionSource_t adjacent[2] = {};
    for ( uint32_t i = 0u; i < 2u; ++i )
    {
        StFixture &fixture = i == 0u ? west : east;
        adjacent[i].m_pHeightR16 = fixture.data.data() + 32u;
        adjacent[i].m_pControlRF = adjacent[i].m_pHeightR16 + 32768u;
        adjacent[i].m_pColorRgba8 = adjacent[i].m_pControlRF + 65536u;
        adjacent[i].m_nRegionIndex = fixture.input.m_nRegionIndex;
        adjacent[i].m_nComposedAllocation = fixture.input.m_nComposedAllocation;
        adjacent[i].m_nComposedGeneration = i == 0u ? 42u : 43u;
    }
    assert( Terrain3DStRegionSourceCooker::cookCold( adjacent, 2u, 1001u, cooked ) );
    t3dStPublicationRegionInput_t resolveInputs[3] = {};
    for ( uint32_t i = 0u; i < 2u; ++i )
    {
        resolveInputs[i].m_nRegionIndex = adjacent[i].m_nRegionIndex;
        resolveInputs[i].m_nComposedAllocation = adjacent[i].m_nComposedAllocation;
        resolveInputs[i].m_nGeneration = adjacent[i].m_nComposedGeneration;
    }
    Terrain3DStRegionPackage adjacentPackage;
    assert( adjacentPackage.openCold( cooked.data(), cooked.size(), 1001u, resolve, resolveInputs ) );
    const t3dStPublicationRegionInput_t &westCooked = adjacentPackage.inputs()[0];
    const size_t fineBase = T3D_ST_VHM_PAGE_TABLE_BYTES + T3D_ST_VHM_TAIL_META_BYTES;
    const size_t eastGutterTexel = ( 32u * T3D_ST_VHM_PAGE_TILE + 66u ) * 2u;
    const uint16_t sampled = t3dStLoadLe16( westCooked.m_pVhmBytes +
        T3D_ST_SIDECAR_HEADER_BYTES + fineBase + T3D_ST_VHM_PAGE_BYTES + eastGutterTexel );
    const uint16_t expected = t3dStLoadLe16( adjacent[1].m_pHeightR16 +
        ( 30u * 128u ) * 2u );
    assert( sampled == expected );
    puts( "ST06 package: strict VT generation, grouped masks/heights, CRC, bad recook rollback: PASS" );
}
