#include "authoring/vt/build/terrain_3d_composed_surface_cold_pipeline.h"
#include "data/runtime/terrain_3d_composed_surface_sidecar.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#if !defined( _WIN32 )
#include <sys/stat.h>
#endif

// The pack validator is replaced ONLY in this fixture because the native V1
// reader depends on the rest of Terrain3D. This does not test real pack-open.
bool t3d_vt_pack_open_baseline_view( const void *bytes, uint64_t length, t3dVTPackBaselineView_t &view )
{
    if ( !bytes || length != sizeof( view ) ) return false;
    view = *static_cast< const t3dVTPackBaselineView_t * >( bytes );
    return view.m_pHeader.m_bValid;
}

struct Fixture
{
    t3dVTPackAllocationRecord_t allocation{};
    t3dVTPackAllocationKeyRecord_t key{};
    t3dVTChannelAllocation_t channels[T3D_VT_LOGICAL_CHANNEL_COUNT]{};
    t3dVTPackPageRecord_t pages[3]{};
    t3dVTPackTailRecord_t tails[3]{};
    t3dVTPackPayloadRecord_t payloads[6]{};
    t3dVTPackClusterRecord_t cluster{};
    std::vector< uint8_t > encoded;
    t3dVTPackBaselineView_t view{};
    uint32_t controls = 0u;
    uint32_t evaluates = 0u;
    uint32_t encodes = 0u;
    uint64_t current_terrain = 17u;
    bool mutate_mid_cook = false;

    void setup()
    {
        allocation.m_nConsumer = T3D_VT_CONSUMER_CLIPMAP;
        key.m_nStableKey = t3d_vt_terrain_material_stable_key( 0u );
        key.m_nAllocationId = 0u;
        const uint8_t values[6] = { 140u, 128u, 210u, 140u, 128u, 210u };
        for ( uint32_t c = 0u; c < 3u; ++c ) {
            const uint32_t format = c == 0u ? T3D_VT_PHYSICAL_FORMAT_BC1_UNORM :
                                    c == 1u ? T3D_VT_PHYSICAL_FORMAT_BC7_UNORM : T3D_VT_PHYSICAL_FORMAT_R8_UNORM;
            channels[c].m_nBaseVirtualPage = c;
            channels[c].m_nPageCount = 1u;
            channels[c].m_nTailIndex = c;
            channels[c].m_nMip0WidthPages = 1u;
            channels[c].m_nMip0HeightPages = 1u;
            channels[c].m_nMipCount = 2u;
            pages[c].m_nVirtualPage = c;
            pages[c].m_nAllocationId = 0u;
            pages[c].m_nPayloadId = c;
            pages[c].m_nChannel = uint16_t( c );
            tails[c].m_nCanonicalAllocationId = 0u;
            tails[c].m_nChannel = uint16_t( c );
            tails[c].m_nFirstMip = 1u;
            tails[c].m_nMipCount = 1u;
            tails[c].m_nFirstMipWidth = 64u;
            tails[c].m_nFirstMipHeight = 64u;
            tails[c].m_nPayloadId = c + 3u;
            for ( uint32_t mip = 0u; mip < 2u; ++mip ) {
                const uint32_t index = c + 3u * mip;
                const uint32_t bytes = t3d_vt_pack_expected_physical_tile_bytes( format );
                payloads[index].m_nFormat = format;
                payloads[index].m_nClusterOffset = uint32_t( encoded.size() );
                payloads[index].m_nDecodedBytes = bytes;
                payloads[index].m_nStoredBytes = bytes;
                encoded.insert( encoded.end(), bytes, values[index] );
                payloads[index].m_nChecksum = t3d_comp_surf_crc32( encoded.data() + payloads[index].m_nClusterOffset, bytes );
            }
        }
        cluster.m_nDecodedBytes = uint32_t( encoded.size() );
        cluster.m_nStoredBytes = uint32_t( encoded.size() );
        view.m_pHeader.m_bValid = true;
        view.m_pAllocations.m_pBytes = reinterpret_cast< const uint8_t * >( &allocation );
        view.m_pAllocationKeys.m_pBytes = reinterpret_cast< const uint8_t * >( &key );
        view.m_pChannelAllocations.m_pBytes = reinterpret_cast< const uint8_t * >( channels );
        view.m_pPages.m_pBytes = reinterpret_cast< const uint8_t * >( pages );
        view.m_pTails.m_pBytes = reinterpret_cast< const uint8_t * >( tails );
        view.m_pPayloads.m_pBytes = reinterpret_cast< const uint8_t * >( payloads );
        view.m_pClusters.m_pBytes = reinterpret_cast< const uint8_t * >( &cluster );
        view.m_pPayloadData = encoded.data();
        view.m_nPayloadDataBytes = encoded.size();
        view.m_nAllocationCount = 1u;
        view.m_nPageCount = 3u;
        view.m_nTailCount = 3u;
        view.m_nClusterCount = 1u;
        view.m_nPayloadCount = 6u;
    }
};

static bool decoder( void *, uint32_t, const uint8_t *data, uint32_t bytes,
                     uint8_t *out, uint32_t capacity )
{
    if ( !data || !bytes || capacity != T3D_COMPOSED_SURFACE_PACK_DECODED_TILE_BYTES ) return false;
    for ( uint32_t i = 0u; i < T3D_VT_PHYSICAL_TILE * T3D_VT_PHYSICAL_TILE; ++i ) {
        out[i * 4u] = data[0];
        out[i * 4u + 1u] = 128u;
        out[i * 4u + 2u] = 196u;
        out[i * 4u + 3u] = 255u;
    }
    return true;
}
static bool page( void *, const t3dComposedSurfaceCookPage_t &p, t3dComposedStaticFootprint_t &foot )
{
    foot = {};
    foot.m_fWorldOrigin[0] = float( p.m_nPageX * 128u );
    foot.m_fWorldOrigin[1] = float( p.m_nPageY * 128u );
    foot.m_fWorldStepX[0] = 1.0f;
    foot.m_fWorldStepY[1] = 1.0f;
    foot.m_fControlStepX[0] = 1.0f;
    foot.m_fControlStepY[1] = 1.0f;
    return true;
}
static bool control( void *u, int32_t, int32_t, uint32_t &value )
{
    auto &f = *static_cast< Fixture * >( u );
    ++f.controls;
    if ( f.mutate_mid_cook ) f.current_terrain = 18u;
    value = 0u;
    return true;
}
static bool encoder( void *u, uint32_t channel, const uint8_t *rgba, uint32_t size,
                     uint8_t *out, uint32_t capacity, uint32_t &written )
{
    auto &f = *static_cast< Fixture * >( u );
    ++f.encodes;
    if ( size != T3D_COMP_SURF_RGBA_SCRATCH_BYTES || capacity != t3d_comp_surf_channel_bytes( channel ) ) return false;
    for ( uint32_t i = 0u; i < capacity; ++i ) out[i] = uint8_t( rgba[(i * 19u) % size] ^ uint8_t(channel) );
    written = capacity;
    return true;
}
static bool current( void *u, uint64_t terrain, uint64_t material, uint64_t topology )
{
    const auto &f = *static_cast< Fixture * >( u );
    return terrain == f.current_terrain && material == 23u && topology == 31u;
}
int main()
{
    Fixture fixture{};
    fixture.setup();
    t3dComposedStaticLayer_t layer{};
    layer.m_nAllocation = UINT32_MAX; // Ignored: stable allocation resolved by CS13.
    layer.m_fColor[0] = layer.m_fColor[1] = layer.m_fColor[2] = 1.0f;
    layer.m_fPbr0[0] = layer.m_fPbr0[1] = 1.0f;
    layer.m_fPbr1[0] = layer.m_fPbr1[1] = 1.0f;
    t3dComposedStaticLayer_t resolved{};
    t3dComposedSurfaceCookPage_t page_spec{ 77u, 991u, 0u, 0u, 0u, 1u };
    uint64_t file_bytes = 0u;
    assert( t3d_comp_surf_cook_total_bytes( 1u, file_bytes ) );
    std::vector< uint8_t > cache( 6u * T3D_COMPOSED_SURFACE_PACK_DECODED_TILE_BYTES );
    std::vector< uint8_t > scratch( T3D_COMP_SURF_COOK_SCRATCH_BYTES );
    std::vector< uint8_t > output( file_bytes );
    t3dComposedSurfaceColdCookRequest_t req{};
    req.m_pSourcePackage = &fixture.view;
    req.m_nSourcePackageBytes = sizeof( fixture.view );
    req.m_pPages = &page_spec;
    req.m_nPageCount = 1u;
    req.m_pAuthoredLayers = &layer;
    req.m_nLayerCount = 1u;
    req.m_pResolvedLayers = &resolved;
    req.m_nResolvedLayerCapacity = 1u;
    req.m_source.m_pUser = &fixture;
    req.m_source.m_pResolvePage = page;
    req.m_source.m_pReadControl = control;
    req.m_source.m_nSamplingMode = T3D_COMP_SURF_STATIC_SAMPLE_NEAREST;
    req.m_source.m_nVtFiltering = 3u;
    req.m_source.m_fBlendSharpness = 0.5f;
    req.m_pEncode = encoder;
    req.m_pEncoderUser = &fixture;
    req.m_pDecode = decoder;
    req.m_pDecodedCache = cache.data();
    req.m_nCacheSlots = 6u;
    req.m_pCookScratch = scratch.data();
    req.m_nCookScratchBytes = uint32_t( scratch.size() );
    req.m_pOutput = output.data();
    req.m_nOutputCapacity = output.size();
    req.m_nTerrainRevision = 17u;
    req.m_nMaterialRevision = 23u;
    req.m_nTopologyGeneration = 31u;
    req.m_pCurrent = current;
    req.m_pCurrentUser = &fixture;

    t3dComposedSurfaceColdCookResult_t result{};
    assert( !t3d_composed_surface_cook_source_pack_cold( req, result ) ); // parity still closed
    req.m_source.m_bCanonicalMaterialParityAccepted = true; // test authority only
    // Reject caller aliasing before any byte in either source or output changes.
    const uint8_t original_header = reinterpret_cast<const uint8_t*>(&fixture.view)[0];
    req.m_pOutput = reinterpret_cast<uint8_t*>( &fixture.view );
    assert( !t3d_composed_surface_cook_source_pack_cold( req, result ) );
    assert( reinterpret_cast<const uint8_t*>(&fixture.view)[0] == original_header );
    req.m_pOutput = output.data();
    const uint64_t fingerprint = t3d_composed_surface_source_fingerprint_cold( req );
    assert( fingerprint != 0u );
    assert( t3d_composed_surface_cook_source_pack_cold( req, result ) );
    assert( result.m_status == T3D_COMPOSED_SURFACE_COLD_COOK_SUCCESS && result.m_nWrittenBytes == file_bytes );
    assert( result.m_nSourceFingerprint == fingerprint );
    assert( resolved.m_nAllocation == 0u );
    assert( fixture.controls != 0u && fixture.encodes == 3u && result.m_nDecodedTiles > 0u );
    assert( t3d_comp_surf_validate_sidecar_bytes( output.data(), output.size(), true ) );
    t3dComposedSurfaceFileHeader_t header{};
    std::memcpy( &header, output.data(), sizeof( header ) );
    assert( header.m_nSourceFingerprint == fingerprint );
    std::vector< uint8_t > prior = output;

    // An identical build reuses all compressed channels without a source sample.
    req.m_pPreviousSidecar = prior.data();
    req.m_nPreviousSidecarBytes = prior.size();
    uint8_t dirty = 0u;
    req.m_pDirtyFlags = &dirty;
    const uint32_t controls_before = fixture.controls;
    const uint32_t encodes_before = fixture.encodes;
    assert( t3d_composed_surface_cook_source_pack_cold( req, result ) );
    assert( fixture.controls == controls_before && fixture.encodes == encodes_before );
    assert( output == prior );

    // Changing a scalar material parameter changes the fingerprint and recooks.
    layer.m_fPbr0[2] = 0.2f;
    assert( t3d_composed_surface_source_fingerprint_cold( req ) != fingerprint );
    assert( t3d_composed_surface_cook_source_pack_cold( req, result ) );
    assert( fixture.encodes == encodes_before + 3u );
    layer.m_fPbr0[2] = 0.0f;

    // A corrupted channel checksum must abort composition before publication.
    req.m_pPreviousSidecar = nullptr;
    req.m_nPreviousSidecarBytes = 0u;
    // This footprint samples mip 1, so corrupt its packed tail payload.
    const uint32_t saved_crc = fixture.payloads[3].m_nChecksum;
    fixture.payloads[3].m_nChecksum ^= 0x800u;
    assert( !t3d_composed_surface_cook_source_pack_cold( req, result ) );
    assert( result.m_status == T3D_COMPOSED_SURFACE_COLD_COOK_COMPOSE_FAILURE );
    fixture.payloads[3].m_nChecksum = saved_crc;
    req.m_pPreviousSidecar = prior.data();
    req.m_nPreviousSidecarBytes = prior.size();

    // Missing canonical material keys fail before composition.
    fixture.key.m_nStableKey = t3d_vt_terrain_material_stable_key( 3u );
    assert( !t3d_composed_surface_cook_source_pack_cold( req, result ) );
    assert( result.m_status == T3D_COMPOSED_SURFACE_COLD_COOK_MISSING_MATERIAL );
    fixture.key.m_nStableKey = t3d_vt_terrain_material_stable_key( 0u );

    // An authoring revision that changes while evaluating cannot be published.
    req.m_pPreviousSidecar = nullptr;
    req.m_nPreviousSidecarBytes = 0u;
    fixture.mutate_mid_cook = true;
    assert( !t3d_composed_surface_cook_source_pack_cold( req, result ) );
    assert( result.m_status == T3D_COMPOSED_SURFACE_COLD_COOK_STALE_REVISION && result.m_nWrittenBytes == 0u );
    fixture.mutate_mid_cook = false;
    fixture.current_terrain = 17u;

    // Valid output can be published atomically through the existing cold writer.
    const char *path = "/tmp/terrain3d_cold_pipeline_test.t3sc";
    req.m_pDestination = path;
    assert( t3d_composed_surface_cook_source_pack_cold( req, result ) );
    std::ifstream file( path, std::ios::binary );
    const std::vector< uint8_t > disk( (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>() );
    assert( disk == output );
#if !defined( _WIN32 )
    // Identical publication must NOT replace the file inode: no disk writes.
    struct stat previous_stat{}, current_stat{};
    assert( stat( path, &previous_stat ) == 0 );
    assert( t3d_composed_surface_cook_source_pack_cold( req, result ) );
    assert( stat( path, &current_stat ) == 0 );
    assert( current_stat.st_ino == previous_stat.st_ino );
#endif
    std::remove( path );
    // Editor reference-only incremental identity: a brush changes local
    // page revision, NOT the immutable package/material/filter fingerprint.
    req.m_pDestination = nullptr;
    req.m_bReferenceOnly = true;
    req.m_source.m_bCanonicalMaterialParityAccepted = false;
    req.m_nControlPayloadFingerprint = 0x123456u;
    req.m_bPageLocalCompositionIdentity = true;
    req.m_pPreviousSidecar = nullptr;
    req.m_nPreviousSidecarBytes = 0u;
    req.m_pDirtyFlags = nullptr;
    const uint64_t candidate_identity = t3d_composed_surface_source_fingerprint_cold( req );
    assert( candidate_identity != 0u );
    assert( t3d_composed_surface_cook_source_pack_cold( req, result ) );
    const std::vector< uint8_t > reference = output;
    req.m_pPreviousSidecar = reference.data();
    req.m_nPreviousSidecarBytes = reference.size();
    const uint32_t before_candidate = fixture.encodes;
    req.m_nTerrainRevision = 18u;
    fixture.current_terrain = 18u;
    req.m_nControlPayloadFingerprint = 0x777777u;
    assert( t3d_composed_surface_source_fingerprint_cold( req ) == candidate_identity );
    assert( t3d_composed_surface_cook_source_pack_cold( req, result ) );
    assert( fixture.encodes == before_candidate );
    assert( output == reference );
    ++page_spec.m_nCompositionRevision;
    assert( t3d_composed_surface_cook_source_pack_cold( req, result ) );
    assert( fixture.encodes == before_candidate + 3u );
    layer.m_fPbr0[2] = 0.33f;
    assert( t3d_composed_surface_source_fingerprint_cold( req ) != candidate_identity );
    layer.m_fPbr0[2] = 0.0f;
    // The candidate identity mode is never available for accepted cooks.
    req.m_bReferenceOnly = false;
    req.m_source.m_bCanonicalMaterialParityAccepted = true;
    assert( !t3d_composed_surface_cook_source_pack_cold( req, result ) );
    req.m_bReferenceOnly = true;
    req.m_source.m_bCanonicalMaterialParityAccepted = false;
    req.m_pDestination = "/tmp/candidate_cannot_be_live.t3sc";
    assert( !t3d_composed_surface_cook_source_pack_cold( req, result ) );
    req.m_pDestination = "/tmp/candidate_reference.t3sc.candidate";
    assert( t3d_composed_surface_cook_source_pack_cold( req, result ) );
    std::remove( req.m_pDestination );
    printf( "CS13 source-pack composition: PASS (%u controls, %u encodes)\n", fixture.controls, fixture.encodes );
}
