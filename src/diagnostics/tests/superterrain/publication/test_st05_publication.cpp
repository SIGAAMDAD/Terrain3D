// ST05 cold transaction test: coherent material/height/mask generation and
// exact owner rollback. Uses ST00 authored bytes and the real ST03/ST04 cooks.
#include "authoring/vhm/terrain_3d_vhm_cooker.h"
#include "authoring/vt/cutout/terrain_3d_cutout_cooker.h"
#include "rendering/rd/superterrain/terrain_3d_st_publication.h"
#include "rendering/rd/vhm/terrain_3d_st_sidecar_wire.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

struct Fixture
{
    std::vector<uint8_t> source;
    std::vector<uint8_t> height;
    std::vector<uint8_t> mask;
    t3dStPublicationRegionInput_t input;
};

static void loadFixture( const std::string &rDirectory, const char *pName, Fixture &rFixture )
{
    std::ifstream file( rDirectory + "/" + pName + ".st00region", std::ios::binary );
    assert( file );
    rFixture.source.assign( std::istreambuf_iterator<char>( file ),
                            std::istreambuf_iterator<char>() );
    assert( rFixture.source.size() == 32u + 128u * 128u * 12u );
    const uint32_t region = t3dStLoadLe16( rFixture.source.data() + 8u );
    const uint32_t generation = 42u;
    const uint64_t sourceSignature = UINT64_C( 0x1234567812345678 );
    const uint64_t recipeSignature = UINT64_C( 0x9876543212345678 );
    const uint8_t *pHeight = rFixture.source.data() + 32u;
    const uint8_t *pControl = pHeight + 128u * 128u * 2u;
    const uint8_t *pColor = pControl + 128u * 128u * 4u;
    t3dStVhmCookInput_t heightInput = {};
    heightInput.m_pHeightLe[1][1] = pHeight;
    heightInput.m_nNeighborGeneration[1][1] = generation;
    heightInput.m_nRegionIndex = region;
    heightInput.m_nGeneration = generation;
    heightInput.m_nSourceSignature = sourceSignature;
    heightInput.m_nRecipeSignature = recipeSignature;
    assert( Terrain3DVhmCooker::cookRegionCold( heightInput, rFixture.height ) );
    t3dStCutoutCookInput_t maskInput = {};
    maskInput.m_pControlLe = pControl;
    maskInput.m_pColorRgba8 = pColor;
    maskInput.m_nRegionIndex = region;
    maskInput.m_nGeneration = generation;
    maskInput.m_nSourceSignature = sourceSignature;
    maskInput.m_nRecipeSignature = recipeSignature;
    assert( Terrain3DCutoutCooker::cookRegionCold( maskInput, rFixture.mask ) );
    rFixture.input.m_nRegionIndex = region;
    rFixture.input.m_nComposedAllocation = region * 3u;
    rFixture.input.m_nGeneration = generation;
    rFixture.input.m_nPackedPhysicalFlags = 0x80000001u;
    rFixture.input.m_nMegaVTAllocation = 100u;
    rFixture.input.m_nMegaVTGeneration = generation;
    rFixture.input.m_nSourceSignature = sourceSignature;
    rFixture.input.m_nRecipeSignature = recipeSignature;
    rFixture.input.m_pVhmBytes = rFixture.height.data();
    rFixture.input.m_nVhmBytes = rFixture.height.size();
    rFixture.input.m_pMaskBytes = rFixture.mask.data();
    rFixture.input.m_nMaskBytes = rFixture.mask.size();
}

int main( int argc, char **argv )
{
    assert( argc == 2 );
    Fixture fixtures[3];
    loadFixture( argv[1], "flat", fixtures[0] );
    loadFixture( argv[1], "coverage_edges", fixtures[1] );
    loadFixture( argv[1], "negative_steep", fixtures[2] );
    t3dStPublicationRegionInput_t inputs[3] = {
        fixtures[0].input, fixtures[1].input, fixtures[2].input
    };
    Terrain3DStPublicationImage image;
    assert( image.prepareCold( inputs, 3u, 8192u ) );
    assert( image.valid() && image.regionCount() == 3u && image.signature() != 0u );
    assert( image.regions().size() == 1024u );
    assert( image.heightPages().size() == 1024u * 11u );
    assert( image.heightTails().size() == 1024u );
    assert( image.maskPages().size() == 1024u * 4u );
    assert( image.heightAtlas().size() == T3D_ST_VHM_TAIL_BYTES );
    assert( image.maskWords().size() <= 8192u );
    assert( !t3dStRegionReady( &image.regions()[0] ) );
    for ( const Fixture &f : fixtures )
    {
        const uint32_t region = f.input.m_nRegionIndex;
        const t3dStRegionRecordV1_t &r = image.regions()[region];
        assert( t3dStRegionReady( &r ) );
        assert( r.m_nComposedVTAllocation == f.input.m_nComposedAllocation );
        assert( r.m_nVhmRootIndex == region && r.m_nMaskRootIndex == region * 4u );
        assert( r.m_nPublicationGeneration == 42u );
        assert( image.heightTails()[region].m_nGeneration == 42u );
        for ( uint32_t p = 0u; p < 11u; ++p )
        {
            const t3dStVhmPageEntryV1_t &entry = image.heightPages()[region * 11u + p];
            assert( entry.m_nGeneration == 42u );
            assert( ( entry.m_nPackedMipFlags & T3D_ST_VHM_PAGE_RESIDENT_FLAG ) == 0u );
        }
        for ( uint32_t p = 0u; p < 4u; ++p )
        {
            const t3dStMaskPageEntryV1_t &entry = image.maskPages()[region * 4u + p];
            assert( entry.m_nGeneration == 42u );
            assert( ( entry.m_nPackedState & T3D_ST_MASK_ENTRY_VALID_FLAG ) != 0u );
            if ( ( entry.m_nPackedState & T3D_ST_MASK_ENTRY_UNIFORM_FLAG ) == 0u )
            {
                assert( entry.m_nPinnedWordOffset + 384u <= image.maskWords().size() );
            }
        }
    }
    // ST05.1: an immutable bounded bootstrap pool must never advertise a fine
    // page without its exact 68x68 R16 upload payload.
    Terrain3DStPublicationImage fine;
    assert( fine.prepareCold( inputs, 3u, 8192u, 4u ) );
    assert( fine.finePageCount() == 4u );
    assert( fine.finePages().size() == 4u * T3D_ST_VHM_PAGE_BYTES );
    for ( uint32_t i = 0u; i < 3u; ++i )
    {
        const uint32_t region = inputs[i].m_nRegionIndex;
        const t3dStVhmPageEntryV1_t &entry = fine.heightPages()[region * 11u + 5u];
        assert( entry.m_nGeneration == 42u );
        assert( entry.m_nPhysicalSlot == i );
        assert( ( entry.m_nPackedMipFlags & T3D_ST_VHM_PAGE_RESIDENT_FLAG ) != 0u );
        Terrain3DVhmRegionView view;
        assert( view.open( inputs[i].m_pVhmBytes, inputs[i].m_nVhmBytes, region,
                           42u, inputs[i].m_nSourceSignature, inputs[i].m_nRecipeSignature ) );
        assert( memcmp( view.getFinePageBytes( 5u ),
                        fine.finePages().data() + size_t( i ) * T3D_ST_VHM_PAGE_BYTES,
                        T3D_ST_VHM_PAGE_BYTES ) == 0 );
    }
    assert( fine.heightPages()[inputs[0].m_nRegionIndex * 11u + 4u].m_nPhysicalSlot == 3u );
    assert( ( fine.heightPages()[inputs[0].m_nRegionIndex * 11u + 0u].m_nPackedMipFlags &
              T3D_ST_VHM_PAGE_RESIDENT_FLAG ) == 0u );
    assert( fine.signature() != image.signature() );
    Terrain3DStPublicationImage maxFine;
    assert( maxFine.prepareCold( inputs, 3u, 8192u, 4096u ) );
    assert( maxFine.finePageCount() == 18u );
    assert( !maxFine.prepareCold( inputs, 3u, 8192u, 4097u ) );
    assert( maxFine.finePageCount() == 18u ); // rejected recook preserves last candidate
    const uint64_t signature = image.signature();
    Terrain3DStPublicationOwner owner;
    assert( owner.commitCold( image ) );
    assert( owner.commitEpoch() == 1u && owner.active().signature() == signature );
    // An invalid/duplicate new revision never evicts the committed snapshot.
    Terrain3DStPublicationImage bad;
    assert( !bad.prepareCold( inputs, 0xffffffffu, 8192u ) );
    assert( !owner.commitCold( bad ) );
    assert( owner.active().signature() == signature && owner.commitEpoch() == 1u );
    inputs[0].m_nGeneration = 43u;
    assert( !bad.prepareCold( inputs, 3u, 8192u ) );
    assert( owner.active().signature() == signature );
    inputs[0].m_nGeneration = 42u;
    inputs[1] = inputs[0];
    assert( !bad.prepareCold( inputs, 3u, 8192u ) );
    assert( owner.active().signature() == signature );
    puts( "ST05: 64-byte ABI, atomic three-family CPU transaction, full root atlas, exact masks, mixed generations, rollback and bounded fine residency: PASS" );
    return 0;
}
