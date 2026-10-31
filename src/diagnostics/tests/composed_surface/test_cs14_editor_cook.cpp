// Reuse CS13's deterministic fixture while exercising the editor-owned journal
// transaction. This fixture is NOT a Vulkan/material-parity acceptance test.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#define main cs13_reference_test_main
#include "test_cs13_cold_pipeline.cpp"
#undef main
#pragma clang diagnostic pop

#include "authoring/vt/integration/terrain_3d_composed_surface_editor_cook.h"

struct CommitFixture
{
    Terrain3DComposedSurfaceEditJournal *journal{};
    t3dComposedSurfaceEditRegion_t region{ 1u, 0, 0, 1u, 1u, 1u, true };
    bool allow = true;
    bool mutate = false;
    uint32_t begin_count = 0u;
    uint32_t end_count = 0u;
};
static bool begin_commit( void *u )
{
    auto &c = *static_cast< CommitFixture * >( u );
    ++c.begin_count;
    if ( !c.allow ) return false;
    if ( c.mutate )
        assert( c.journal->invalidateRegionCold( c.region, 99u, 31u ) );
    return true;
}
static void end_commit( void *u )
{
    ++static_cast< CommitFixture * >( u )->end_count;
}

int main()
{
    Fixture f{};
    f.setup();
    t3dComposedStaticV1Layer_t layer{};
    layer.m_nAllocation = UINT32_MAX;
    layer.m_fColor[0] = layer.m_fColor[1] = layer.m_fColor[2] = 1.0f;
    layer.m_fPbr0[0] = layer.m_fPbr0[1] = 1.0f;
    layer.m_fPbr1[0] = layer.m_fPbr1[1] = 1.0f;
    t3dComposedStaticV1Layer_t resolved{};
    t3dComposedSurfaceCookPage_t page_spec{ 77u, 991u, 0u, 0u, 0u, 1u };
    uint64_t bytes = 0u;
    assert( t3d_cs_cook_total_bytes( 1u, bytes ) );
    std::vector< uint8_t > cache( 6u * T3D_COMPOSED_SURFACE_PACK_DECODED_TILE_BYTES );
    std::vector< uint8_t > scratch( T3D_CS_COOK_SCRATCH_BYTES );
    std::vector< uint8_t > output( bytes );
    const char *path = "/tmp/terrain3d_cs14_editor_cook_test.t3sc";
    std::remove( path );
    Terrain3DComposedSurfaceEditJournal journal{};
    CommitFixture commit{};
    commit.journal = &journal;
    t3dComposedSurfaceEditorCookRequest_t req{};
    auto &cook = req.m_cook;
    cook.m_pSourcePackage = &f.view;
    cook.m_nSourcePackageBytes = sizeof( f.view );
    cook.m_pPages = &page_spec;
    cook.m_nPageCount = 1u;
    cook.m_pAuthoredLayers = &layer;
    cook.m_nLayerCount = 1u;
    cook.m_pResolvedLayers = &resolved;
    cook.m_nResolvedLayerCapacity = 1u;
    cook.m_source.m_pUser = &f;
    cook.m_source.m_pResolvePage = page;
    cook.m_source.m_pReadControl = control;
    cook.m_source.m_nSamplingMode = T3D_CS_STATIC_V1_SAMPLE_NEAREST;
    cook.m_source.m_nVtFiltering = 3u;
    cook.m_source.m_fBlendSharpness = 0.5f;
    cook.m_source.m_bCanonicalMaterialParityAccepted = true; // test fixture only
    cook.m_pEncode = encoder;
    cook.m_pEncoderUser = &f;
    cook.m_pDecode = decoder;
    cook.m_pDecodedCache = cache.data();
    cook.m_nCacheSlots = 6u;
    cook.m_pCookScratch = scratch.data();
    cook.m_nCookScratchBytes = uint32_t( scratch.size() );
    cook.m_pOutput = output.data();
    cook.m_nOutputCapacity = output.size();
    cook.m_nTerrainRevision = 17u;
    cook.m_nMaterialRevision = 23u;
    cook.m_nTopologyGeneration = 31u;
    cook.m_pCurrent = current;
    cook.m_pCurrentUser = &f;
    cook.m_pDestination = path;
    req.m_nRegionIndex = 1u;
    req.m_nJournalFingerprint = 99u;
    req.m_nJournalTopologyGeneration = 31u;
    req.m_nRegionKey = 77u;
    req.m_nMip0Width = req.m_nMip0Height = 1u;
    uint8_t flag = 0u;
    req.m_pDirtyFlags = &flag;
    req.m_nDirtyCapacity = 1u;
    req.m_pBeginCommit = begin_commit;
    req.m_pEndCommit = end_commit;
    req.m_pCommitUser = &commit;
    t3dComposedSurfaceEditorCookResult_t result{};

    // No dirty work is reported, not accidentally published.
    assert( !t3d_composed_surface_editor_cook_cold( journal, req, result ) );
    assert( result.m_status == T3D_COMPOSED_SURFACE_EDITOR_COOK_NOT_DIRTY );
    assert( journal.invalidateRegionCold( commit.region, 99u, 31u ) );

    // Absent parity rejects work without consuming an edit claim.
    cook.m_source.m_bCanonicalMaterialParityAccepted = false;
    assert( !t3d_composed_surface_editor_cook_cold( journal, req, result ) );
    assert( result.m_status == T3D_COMPOSED_SURFACE_EDITOR_COOK_INVALID_INPUT );
    cook.m_source.m_bCanonicalMaterialParityAccepted = true;

    // A terrain revision that changes during source sampling cannot enter commit.
    f.mutate_mid_cook = true;
    assert( !t3d_composed_surface_editor_cook_cold( journal, req, result ) );
    assert( result.m_status == T3D_COMPOSED_SURFACE_EDITOR_COOK_STALE );
    assert( commit.begin_count == 0u && commit.end_count == 0u );
    assert( journal.statsCold().m_nDirtyRegions == 1u );
    f.mutate_mid_cook = false;
    f.current_terrain = 17u;

    // Refusal to enter serialized publication leaves the region dirty.
    commit.allow = false;
    assert( !t3d_composed_surface_editor_cook_cold( journal, req, result ) );
    assert( result.m_status == T3D_COMPOSED_SURFACE_EDITOR_COOK_COMMIT_FAILED );
    assert( journal.statsCold().m_nDirtyRegions == 1u );
    assert( commit.begin_count == 1u && commit.end_count == 0u );
    std::ifstream absent( path, std::ios::binary );
    assert( !absent.good() );

    // A new authoring edit at commit revokes the old ticket before file IO.
    commit.allow = true;
    commit.mutate = true;
    assert( !t3d_composed_surface_editor_cook_cold( journal, req, result ) );
    assert( result.m_status == T3D_COMPOSED_SURFACE_EDITOR_COOK_STALE );
    assert( journal.statsCold().m_nDirtyRegions == 1u );
    std::ifstream absent2( path, std::ios::binary );
    assert( !absent2.good() );
    commit.mutate = false;

    // Successful commit validates and writes all channels, then acknowledges.
    assert( t3d_composed_surface_editor_cook_cold( journal, req, result ) );
    assert( result.m_status == T3D_COMPOSED_SURFACE_EDITOR_COOK_SUCCESS && flag == 1u );
    assert( result.m_cook.m_status == T3D_COMPOSED_SURFACE_COLD_COOK_SUCCESS );
    assert( journal.statsCold().m_nDirtyRegions == 0u );
    assert( journal.statsCold().m_nAcceptedCompletions == 1u );
    assert( commit.begin_count == 3u && commit.end_count == 2u );
    std::ifstream file( path, std::ios::binary );
    std::vector< uint8_t > disk( (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>() );
    assert( disk == output );
    assert( t3d_cs_validate_sidecar_bytes( disk.data(), disk.size(), true ) );
    std::remove( path );
    printf( "CS14 editor journal transaction: PASS\n" );
}
