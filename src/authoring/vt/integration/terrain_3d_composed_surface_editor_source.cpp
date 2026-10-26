#include "authoring/vt/integration/terrain_3d_composed_surface_editor_source.h"
#if defined( T3D_EDITOR_NATIVE )

#include "authoring/vt/build/terrain_3d_composed_surface_v3_cold_pipeline.h"
#include "authoring/vt/build/terrain_3d_composed_surface_v3_godot_decode.h"
#include "authoring/vt/build/terrain_3d_composed_surface_encoder_adapter.h"
#include "authoring/vt/integration/terrain_3d_composed_surface_coordinator_hooks.h"
#include "assets/catalog/terrain_3d_assets.h"
#include "assets/resources/terrain_3d_texture_asset.h"
#include "data/model/terrain_3d_data.h"
#include "rendering/material/terrain_3d_material.h"
#include "rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h"

#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include "rendering/rd/vt/runtime/terrain_3d_composed_surface_cache.h"
#include "data/runtime/terrain_3d_composed_surface_sidecar.h"
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <vector>
#include <cmath>

using namespace godot;

// One additional control sample is needed for the four-corner interpolation.
#define T3D_CS_EDITOR_SNAPSHOT_SIDE ( T3D_CS_PHYSICAL_TILE + 2u )
#define T3D_CS_EDITOR_SNAPSHOT_COUNT ( T3D_CS_EDITOR_SNAPSHOT_SIDE * T3D_CS_EDITOR_SNAPSHOT_SIDE )

struct t3dCsEditorControlTile_t
{
    uint32_t m_nRegionIndex = 0u;
    int32_t m_nOriginX = 0;
    int32_t m_nOriginZ = 0;
    PackedInt32Array m_controls;
    uint64_t m_nControlsRevision = 0u;
};

struct t3dCsEditorSourceContext_t
{
    Terrain3DData *m_pData = nullptr;
    const Terrain3DTerrainMegaVT *m_pMegaVT = nullptr;
    const Terrain3DComposedSurfaceEditJournal *m_pEdits = nullptr;
    const Terrain3DAssets *m_pAssets = nullptr;
    std::vector< t3dCsEditorControlTile_t > m_tiles;
    const t3dCsEditorControlTile_t *m_pActive = nullptr;
    uint64_t m_nTerrainRevision = 0u;
    uint64_t m_nMaterialRevision = 0u;
    uint64_t m_nTopologyRevision = 0u;
};

static uint64_t t3d_cs_editor_material_revision( const Terrain3DAssets &rAssets )
{
    const uint64_t a = rAssets.getTerrainVtContentGeneration();
    const uint64_t b = rAssets.getInstanceVtMaterialSourceGeneration();
    uint64_t revision = ( a ^ ( b + 0x9e3779b97f4a7c15ull + ( a << 6u ) + ( a >> 2u ) ) );
    return revision ? revision : 1u;
}

static uint64_t t3d_cs_editor_terrain_revision( const Terrain3DComposedSurfaceEditJournal &rJournal )
{
    const auto &s = rJournal.statsCold();
    uint64_t result = 1u + s.m_nEditEvents + s.m_nSourceInvalidations + s.m_nRemovedRegions;
    return result ? result : 1u;
}

static bool t3d_cs_editor_current( void *pUser, const uint64_t nTerrain,
                                   const uint64_t nMaterial, const uint64_t nTopology )
{
    const auto &c = *static_cast< const t3dCsEditorSourceContext_t * >( pUser );
    return c.m_pData && c.m_pMegaVT && c.m_pEdits && c.m_pAssets &&
           nTerrain == c.m_nTerrainRevision && nMaterial == c.m_nMaterialRevision &&
           nTopology == c.m_nTopologyRevision &&
           t3d_cs_editor_terrain_revision( *c.m_pEdits ) == nTerrain &&
           t3d_cs_editor_material_revision( *c.m_pAssets ) == nMaterial &&
           c.m_pMegaVT->getTopologyGeneration() == nTopology;
}

static bool t3d_cs_editor_page( void *pUser, const t3dComposedSurfaceCookPage_t &rPage,
                                t3dComposedStaticV3Footprint_t &rFootprint )
{
    auto &c = *static_cast< t3dCsEditorSourceContext_t * >( pUser );
    c.m_pActive = nullptr;
    if ( rPage.m_nMip != 0u || rPage.m_nPageX != 0u || rPage.m_nPageY != 0u ||
         rPage.m_nRegionKey == 0u || rPage.m_nRegionKey > T3D_CS_EDITOR_REGION_COUNT ) return false;
    const uint32_t nIndex = uint32_t( rPage.m_nRegionKey - 1u );
    for ( const auto &tile : c.m_tiles ) {
        if ( tile.m_nRegionIndex != nIndex ) continue;
        c.m_pActive = &tile;
        rFootprint = {};
        rFootprint.m_fWorldOrigin[0] = float( tile.m_nOriginX + int32_t( T3D_CS_PAGE_GUTTER ) );
        rFootprint.m_fWorldOrigin[1] = float( tile.m_nOriginZ + int32_t( T3D_CS_PAGE_GUTTER ) );
        rFootprint.m_fControlOrigin[0] = rFootprint.m_fWorldOrigin[0];
        rFootprint.m_fControlOrigin[1] = rFootprint.m_fWorldOrigin[1];
        rFootprint.m_fWorldStepX[0] = 1.0f;
        rFootprint.m_fWorldStepY[1] = 1.0f;
        rFootprint.m_fControlStepX[0] = 1.0f;
        rFootprint.m_fControlStepY[1] = 1.0f;
        return true;
    }
    return false;
}

static bool t3d_cs_editor_control( void *pUser, const int32_t nX, const int32_t nZ,
                                   uint32_t &rControl )
{
    const auto &c = *static_cast< const t3dCsEditorSourceContext_t * >( pUser );
    const auto *pTile = c.m_pActive;
    if ( !pTile ) return false;
    const int64_t x = int64_t( nX ) - pTile->m_nOriginX;
    const int64_t z = int64_t( nZ ) - pTile->m_nOriginZ;
    if ( x < 0 || z < 0 || x >= T3D_CS_EDITOR_SNAPSHOT_SIDE || z >= T3D_CS_EDITOR_SNAPSHOT_SIDE ) return false;
    const uint32_t index = uint32_t( z * T3D_CS_EDITOR_SNAPSHOT_SIDE + x );
    if ( index >= uint32_t( pTile->m_controls.size() ) ) return false;
    rControl = uint32_t( pTile->m_controls[index] );
    // Missing source cells remain non-authoritative: no fabricated controls.
    return rControl != UINT32_MAX;
}

bool t3d_cs_cook_editor_candidate_cold(
    const PackedByteArray &rPack, Terrain3DData &rData,
    const Ref< Terrain3DAssets > &rAssets,
    const Ref< Terrain3DMaterial > &rMaterial,
    const Terrain3DTerrainMegaVT &rMegaVT,
    const Terrain3DComposedSurfaceEditJournal &rEdits,
    const String &rDirectory, t3dCsEditorCandidateResult_t &rResult )
{
    rResult = {};
    rResult.m_error = "invalid_source";
    if ( rPack.is_empty() || rAssets.is_null() || rMaterial.is_null() || rDirectory.is_empty() ||
         !rMegaVT.isReady() ) return false;
    const uint32_t nMode = uint32_t( rMaterial->getMiddleTerrainSamplingMode() );
    const t3dTextureFiltering_t filter = rMaterial->getMiddleTextureFiltering();
    if ( ( nMode != 1u && nMode != 2u && nMode != 5u ) ||
         ( filter != LINEAR && filter != NEAREST ) ||
         rMaterial->getMiddleDetilingMode() != DETILING_DISABLED ||
         rMaterial->getMiddleAutoShaderEnabled() || rMaterial->getDualScalingEnabled() ||
         rMaterial->getProjectionEnabled() ||
         rData.getVertexSpacingInternal() != 1.0 ) {
        rResult.m_error = "unsupported_static_material_mode";
        return false;
    }
    const uint32_t nLayers = uint32_t( rAssets->getTextureCount() );
    if ( !nLayers || nLayers > T3D_CS_STATIC_V3_MAX_LAYERS ) {
        rResult.m_error = "unsupported_material_count";
        return false;
    }
    t3dComposedStaticV3Layer_t layers[T3D_CS_STATIC_V3_MAX_LAYERS]{};
    t3dComposedStaticV3Layer_t resolved[T3D_CS_STATIC_V3_MAX_LAYERS]{};
    for ( uint32_t i = 0u; i < nLayers; ++i ) {
        const Ref< Terrain3DTextureAsset > asset = rAssets->getTextureAsset( int( i ) );
        if ( asset.is_null() ) { rResult.m_error = "missing_material"; return false; }
        const Color color = asset->getAlbedoColor();
        layers[i].m_fColor[0] = float( color.r );
        layers[i].m_fColor[1] = float( color.g );
        layers[i].m_fColor[2] = float( color.b );
        layers[i].m_fPbr0[0] = float( asset->getUvScale() );
        layers[i].m_fPbr0[1] = float( asset->getNormalDepth() );
        layers[i].m_fPbr0[2] = float( asset->getRoughness() );
        layers[i].m_fPbr0[3] = float( asset->getMetallic() );
        layers[i].m_fPbr1[0] = float( asset->getAoStrength() );
        layers[i].m_fPbr1[1] = float( asset->getAoLightAffect() );
    }
    t3dCsEditorSourceContext_t context{};
    context.m_pData = &rData;
    context.m_pMegaVT = &rMegaVT;
    context.m_pEdits = &rEdits;
    context.m_pAssets = rAssets.ptr();
    context.m_nTerrainRevision = t3d_cs_editor_terrain_revision( rEdits );
    context.m_nMaterialRevision = t3d_cs_editor_material_revision( *rAssets.ptr() );
    context.m_nTopologyRevision = rMegaVT.getTopologyGeneration();
    if ( !context.m_nTopologyRevision ) {
        rResult.m_error = "terrain_topology_unavailable";
        return false;
    }

    std::vector< t3dComposedSurfaceCookPage_t > pages;
    pages.reserve( 128u );
    context.m_tiles.reserve( 128u );
    uint64_t controlsHash = 14695981039346656037ull;
    for ( uint32_t region = 0u; region < T3D_CS_EDITOR_REGION_COUNT; ++region ) {
        t3dComposedSurfaceEditRegion_t info{};
        if ( !t3d_cs_editor_region( rMegaVT, region, info ) ||
             !rData.getRegionPtr( Vector2i( info.m_nRegionX, info.m_nRegionZ ) ) ) continue;
        // Each candidate page covers exactly one 128 m region. Do not write
        // misleading locations for larger virtual texture layouts.
        if ( info.m_nWidth != 1u || info.m_nHeight != 1u ||
             rMegaVT.getTexelsPerMeter() != 1u ) {
            rResult.m_error = "unsupported_region_page_layout";
            return false;
        }
        t3dCsEditorControlTile_t tile{};
        tile.m_nRegionIndex = region;
        tile.m_nOriginX = info.m_nRegionX * int32_t( T3D_CS_PAGE_INTERIOR ) - int32_t( T3D_CS_PAGE_GUTTER );
        tile.m_nOriginZ = info.m_nRegionZ * int32_t( T3D_CS_PAGE_INTERIOR ) - int32_t( T3D_CS_PAGE_GUTTER );
        PackedVector3Array positions;
        positions.resize( T3D_CS_EDITOR_SNAPSHOT_COUNT );
        Vector3 *pPositions = positions.ptrw();
        for ( uint32_t z = 0u; z < T3D_CS_EDITOR_SNAPSHOT_SIDE; ++z )
            for ( uint32_t x = 0u; x < T3D_CS_EDITOR_SNAPSHOT_SIDE; ++x )
                pPositions[z * T3D_CS_EDITOR_SNAPSHOT_SIDE + x] =
                    Vector3( float( tile.m_nOriginX + int32_t( x ) ) + 0.5f, 0.0f,
                             float( tile.m_nOriginZ + int32_t( z ) ) + 0.5f );
        tile.m_controls = rData.getControls( positions );
        if ( tile.m_controls.size() != T3D_CS_EDITOR_SNAPSHOT_COUNT ) {
            rResult.m_error = "control_snapshot_failed";
            return false;
        }
        const int32_t *pControl = tile.m_controls.ptr();
        // A missing *exterior* neighbor cannot block a diagnostic bake of
        // an isolated region. Clamp only its physical gutter to the nearest
        // valid authored interior cell, recording the substitution. Interior
        // holes/failed source reads remain fatal. Boundary-clamped candidate
        // pages are NEVER renderer-authoritative without a seam parity test.
        bool boundaryClamped = false;
        const int32_t first = int32_t( T3D_CS_PAGE_GUTTER );
        const int32_t last = first + int32_t( T3D_CS_PAGE_INTERIOR ) - 1;
        int32_t *pWritable = tile.m_controls.ptrw();
        for ( uint32_t z = 0u; z < T3D_CS_EDITOR_SNAPSHOT_SIDE; ++z ) {
            for ( uint32_t x = 0u; x < T3D_CS_EDITOR_SNAPSHOT_SIDE; ++x ) {
                const uint32_t i = z * T3D_CS_EDITOR_SNAPSHOT_SIDE + x;
                if ( uint32_t( pWritable[i] ) != UINT32_MAX ) continue;
                const int32_t ix = int32_t( x ), iz = int32_t( z );
                if ( ix >= first && ix <= last && iz >= first && iz <= last ) {
                    rResult.m_error = "missing_interior_control";
                    return false;
                }
                const int32_t sx = ix < first ? first : ( ix > last ? last : ix );
                const int32_t sz = iz < first ? first : ( iz > last ? last : iz );
                const uint32_t source = uint32_t( sz ) * T3D_CS_EDITOR_SNAPSHOT_SIDE + uint32_t( sx );
                if ( uint32_t( pWritable[source] ) == UINT32_MAX ) {
                    rResult.m_error = "missing_control_halo_source";
                    return false;
                }
                pWritable[i] = pWritable[source];
                boundaryClamped = true;
            }
        }
        if ( boundaryClamped ) ++rResult.m_nBoundaryClampedPages;
        pControl = tile.m_controls.ptr();
        uint64_t pageHash = 14695981039346656037ull;
        for ( uint32_t i = 0u; i < T3D_CS_EDITOR_SNAPSHOT_COUNT; ++i ) {
            const uint32_t value = uint32_t( pControl[i] );
            for ( uint32_t b = 0u; b < 4u; ++b ) {
                const uint8_t byte = uint8_t( value >> ( b * 8u ) );
                pageHash = ( pageHash ^ byte ) * 1099511628211ull;
                controlsHash = ( controlsHash ^ byte ) * 1099511628211ull;
            }
        }
        tile.m_nControlsRevision = pageHash ? pageHash : 1u;
        context.m_tiles.push_back( tile );
        pages.push_back( { uint64_t( region ) + 1u, tile.m_nControlsRevision, 0u, 0u, 0u, 1u } );
    }
    if ( pages.empty() ) { rResult.m_error = "no_eligible_regions"; return false; }
    if ( !t3d_cs_editor_current( &context, context.m_nTerrainRevision,
                                 context.m_nMaterialRevision, context.m_nTopologyRevision ) ) {
        rResult.m_error = "source_changed_while_snapshotting";
        return false;
    }
    uint64_t bytes = 0u;
    if ( !t3d_cs_cook_total_bytes( uint32_t( pages.size() ), bytes ) || bytes > INT32_MAX ) {
        rResult.m_error = "composed_budget_exceeded";
        return false;
    }
    PackedByteArray output, scratch, cache;
    output.resize( int64_t( bytes ) );
    scratch.resize( T3D_CS_COOK_FUSED_SCRATCH_BYTES );
    cache.resize( 4u * T3D_CS12_DECODED_TILE_BYTES );
    if ( uint64_t( output.size() ) != bytes || scratch.is_empty() || cache.is_empty() ) {
        rResult.m_error = "authoring_memory_exhausted";
        return false;
    }
    const String path = rDirectory.path_join( "terrain_composed.t3sc.candidate" );
    const CharString pathUtf8 = path.utf8();
    t3dCs13ColdCookRequest_t request{};
    request.m_pSourcePackage = rPack.ptr();
    request.m_nSourcePackageBytes = uint64_t( rPack.size() );
    request.m_pPages = pages.data();
    request.m_nPageCount = uint32_t( pages.size() );
    request.m_pAuthoredLayers = layers;
    request.m_nLayerCount = nLayers;
    request.m_pResolvedLayers = resolved;
    request.m_nResolvedLayerCapacity = T3D_CS_STATIC_V3_MAX_LAYERS;
    request.m_source.m_pUser = &context;
    request.m_source.m_pResolvePage = t3d_cs_editor_page;
    request.m_source.m_pReadControl = t3d_cs_editor_control;
    request.m_source.m_nSamplingMode = nMode;
    request.m_source.m_nVtFiltering = filter == LINEAR ? 1u : 3u;
    request.m_source.m_fStaticMipBias = float( rMaterial->getMiddleMaterialLodBias() );
    request.m_source.m_fBlendSharpness = 0.5f; // Candidate only; canonical comparison still required.
    request.m_pEncode = t3d_cs_reference_encode;
    request.m_pDecode = t3d_cs12_decode_godot_image;
    request.m_pDecodedCache = cache.ptrw();
    request.m_nCacheSlots = 4u;
    request.m_pCookScratch = scratch.ptrw();
    request.m_nCookScratchBytes = uint32_t( scratch.size() );
    request.m_pOutput = output.ptrw();
    request.m_nOutputCapacity = bytes;
    request.m_nTerrainRevision = context.m_nTerrainRevision;
    request.m_nMaterialRevision = context.m_nMaterialRevision;
    request.m_nTopologyGeneration = context.m_nTopologyRevision;
    request.m_nControlPayloadFingerprint = controlsHash ? controlsHash : 1u;
    request.m_bPageLocalCompositionIdentity = true;
    request.m_pCurrent = t3d_cs_editor_current;
    request.m_pCurrentUser = &context;
    request.m_bReferenceOnly = true;
    request.m_pDestination = pathUtf8.get_data();
    // Incremental reuse is valid only for a CRC-verified candidate with the
    // same immutable source/material/filter identity and per-page revision.
    const uint64_t identity = t3d_cs13_source_fingerprint_cold( request );
    PackedByteArray previous = FileAccess::get_file_as_bytes( path );
    t3dComposedSurfaceSidecarView_t priorView{};
    if ( !previous.is_empty() &&
         t3d_cs_validate_sidecar_bytes( previous.ptr(), size_t( previous.size() ), true ) &&
         t3d_cs_open_sidecar_cold( previous.ptr(), uint64_t( previous.size() ),
                                    identity, priorView ) ) {
        request.m_pPreviousSidecar = previous.ptr();
        request.m_nPreviousSidecarBytes = uint64_t( previous.size() );
        for ( const auto &page : pages ) {
            uint32_t index = 0u;
            if ( t3d_cs_find_page( priorView, page.m_nRegionKey, page.m_nMip,
                                  page.m_nPageX, page.m_nPageY,
                                  page.m_nCompositionRevision, index ) )
                ++rResult.m_nReusedPages;
        }
    }
    t3dCs13ColdCookResult_t cooked{};
    rResult.m_nPages = request.m_nPageCount;
    if ( !t3d_cs13_cook_source_pack_cold( request, cooked ) ) {
        rResult.m_error = String( "candidate_cook_failed_status=" ) + String::num_int64( cooked.m_status );
        rResult.m_nReusedPages = 0u;
        return false;
    }
    // Publication is successful only after the actual on-disk sidecar can
    // be reopened and every channel payload passes its CRC checks. This is
    // not a claim of canonical color/DNR/alpha material parity.
    PackedByteArray committed = FileAccess::get_file_as_bytes( path );
    t3dComposedSurfaceSidecarView_t verified{};
    if ( uint64_t( committed.size() ) != cooked.m_nWrittenBytes ||
         !t3d_cs_validate_sidecar_bytes( committed.ptr(), size_t( committed.size() ), true ) ||
         !t3d_cs_open_sidecar_cold( committed.ptr(), uint64_t( committed.size() ),
                                    identity, verified ) ||
         verified.m_header.m_nPageCount != request.m_nPageCount ) {
        rResult.m_error = "candidate_reopen_or_crc_failed";
        rResult.m_nReusedPages = 0u;
        return false;
    }
    rResult.m_bSaved = true;
    rResult.m_nWrittenBytes = cooked.m_nWrittenBytes;
    rResult.m_nSourceFingerprint = cooked.m_nSourceFingerprint;
    rResult.m_nDecodedTiles = cooked.m_nDecodedTiles;
    rResult.m_path = path;
    rResult.m_error = String();
    return true;
}
#endif // T3D_EDITOR_NATIVE
