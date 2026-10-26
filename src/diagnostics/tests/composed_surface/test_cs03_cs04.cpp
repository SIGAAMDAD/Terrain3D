// Standalone CS03–CS04 system tests: clang++ -std=c++17 -Isrc -Wall -Wextra -Werror ...
#include "authoring/vt/build/terrain_3d_composed_surface_cooker.h"
#include "authoring/vt/io/terrain_3d_composed_surface_sidecar_writer.h"
#include "rendering/rd/vt/runtime/terrain_3d_composed_surface_cache.h"
#include "authoring/vt/integration/terrain_3d_composed_surface_resident_bridge.h"
#include "rendering/rd/vt/runtime/terrain_3d_composed_surface_mapped_file.h"
#include "data/runtime/terrain_3d_composed_surface_sidecar.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <fstream>
#include <iterator>
#include <vector>

struct TestProducer
{
    uint32_t evaluations = 0u;
    uint32_t encodes = 0u;
    uint32_t fail_at = 0u;
};
static bool evaluate( void *p, const t3dComposedSurfaceCookPage_t &page,
                      const uint32_t channel, uint8_t *pixels, const uint32_t bytes )
{
    TestProducer &producer = *static_cast< TestProducer * >( p );
    ++producer.evaluations;
    if ( producer.fail_at && producer.evaluations == producer.fail_at ) return false;
    assert( bytes == T3D_CS_RGBA_SCRATCH_BYTES );
    for ( uint32_t y = 0u; y < T3D_CS_PHYSICAL_TILE; ++y ) {
        for ( uint32_t x = 0u; x < T3D_CS_PHYSICAL_TILE; ++x ) {
            const uint32_t i = ( y * T3D_CS_PHYSICAL_TILE + x ) * 4u;
            const uint32_t world_x = page.m_nPageX * T3D_CS_PAGE_INTERIOR + x - T3D_CS_PAGE_GUTTER;
            // Unsigned wrap in this fixture ensures matching edge texels across
            // adjacent pages: first gutter pixel equals neighbor interior.
            pixels[i] = uint8_t( world_x );
            pixels[i + 1u] = uint8_t( page.m_nMip * 37u + y );
            pixels[i + 2u] = uint8_t( channel * 71u );
            pixels[i + 3u] = 255u;
        }
    }
    return true;
}
static bool encode( void *p, const uint32_t channel, const uint8_t *pixels, const uint32_t pixel_bytes,
                    uint8_t *encoded, const uint32_t capacity, uint32_t &written )
{
    TestProducer &producer = *static_cast< TestProducer * >( p );
    ++producer.encodes;
    assert( pixel_bytes == T3D_CS_RGBA_SCRATCH_BYTES );
    assert( capacity == t3d_cs_channel_bytes( channel ) );
    // Transport mock: deterministic channel bytes, not a BC decoder test.
    for ( uint32_t i = 0u; i < capacity; ++i ) encoded[i] = uint8_t( pixels[( i * 17u ) % pixel_bytes] + channel );
    written = capacity;
    return true;
}

static void test_pages()
{
    const t3dComposedSurfaceCookPage_t pages[] = {
        { 55u, 91u, 0u, 0u, 0u, 1u },
        { 55u, 91u, 0u, 1u, 0u, 1u },
        { 55u, 91u, 1u, 0u, 0u, 1u },
        { 56u, 92u, 0u, 0u, 0u, 2u }
    };
    uint64_t file_bytes = 0u;
    assert( !t3d_cs_cook_total_bytes( 0u, file_bytes ) );
    assert( t3d_cs_cook_total_bytes( 4u, file_bytes ) );
    TestProducer producer{};
    const t3dComposedSurfaceCookSource_t source = { &producer, evaluate, encode, 0x12345678u, true };
    std::vector< uint8_t > scratch( T3D_CS_COOK_SCRATCH_BYTES );
    std::vector< uint8_t > bytes( size_t( file_bytes ), 0u );
    uint64_t written = 0u;
    auto gated = source;
    gated.m_bCanonicalMaterialParityAccepted = false;
    assert( !t3d_cs_cook_pages_cold( pages, 4u, gated, scratch.data(), uint32_t( scratch.size() ),
                                     bytes.data(), bytes.size(), written ) );
    assert( producer.evaluations == 0u );
    assert( !t3d_cs_cook_pages_cold( pages, 4u, source, scratch.data(), uint32_t( scratch.size() ),
                                     bytes.data(), bytes.size() - 1u, written ) );
    const t3dComposedSurfaceCookPage_t duplicate[] = { pages[0], pages[0] };
    assert( !t3d_cs_cook_pages_cold( duplicate, 2u, source, scratch.data(), uint32_t( scratch.size() ),
                                     bytes.data(), bytes.size(), written ) );
    assert( t3d_cs_cook_pages_cold( pages, 4u, source, scratch.data(), uint32_t( scratch.size() ),
                                    bytes.data(), bytes.size(), written ) );
    assert( written == bytes.size() && producer.encodes == 12u && producer.evaluations == 12u );
    assert( t3d_cs_validate_sidecar_bytes( bytes.data(), bytes.size(), true ) );
    std::vector< uint8_t > reproduced( bytes.size() );
    assert( t3d_cs_cook_pages_cold( pages, 4u, source, scratch.data(), uint32_t( scratch.size() ),
                                    reproduced.data(), reproduced.size(), written ) );
    assert( bytes == reproduced );
    // One dirty page recooks three channels; three unchanged pages reuse bytes.
    uint8_t dirty[] = { 0u, 1u, 0u, 0u };
    const uint32_t eval_before = producer.evaluations;
    std::vector< uint8_t > incremental( bytes.size() );
    auto dirty_pages = std::vector< t3dComposedSurfaceCookPage_t >( pages, pages + 4u );
    dirty_pages[1].m_nCompositionRevision = 93u;
    dirty_pages[1].m_nGeneration = 2u;
    assert( t3d_cs_recook_incremental_cold( dirty_pages.data(), 4u, source, bytes.data(), bytes.size(),
                                             dirty, scratch.data(), uint32_t( scratch.size() ),
                                             incremental.data(), incremental.size(), written ) );
    assert( producer.evaluations == eval_before + 3u );
    assert( t3d_cs_validate_sidecar_bytes( incremental.data(), incremental.size(), true ) );
    t3dComposedSurfacePageRecord_t unchanged_old{}, unchanged_new{};
    memcpy( &unchanged_old, bytes.data() + sizeof( t3dComposedSurfaceFileHeader_t ), sizeof( unchanged_old ) );
    memcpy( &unchanged_new, incremental.data() + sizeof( t3dComposedSurfaceFileHeader_t ), sizeof( unchanged_new ) );
    assert( unchanged_old.m_nContentFingerprint == unchanged_new.m_nContentFingerprint );
    for ( uint32_t channel = 0u; channel < T3D_CS_CHANNEL_COUNT; ++channel ) {
        assert( unchanged_old.m_channels[channel].m_nCrc32 == unchanged_new.m_channels[channel].m_nCrc32 );
    }
    // Explicit dirty flag also forces recook even with unchanged revision.
    dirty_pages[1] = pages[1];
    assert( t3d_cs_recook_incremental_cold( dirty_pages.data(), 4u, source, bytes.data(), bytes.size(),
                                             dirty, scratch.data(), uint32_t( scratch.size() ),
                                             incremental.data(), incremental.size(), written ) );
    assert( producer.evaluations == eval_before + 6u );
    const char *path = "/tmp/terrain3d_composed_surface_cooker_test.t3sc";
    assert( t3d_cs_write_sidecar_cold( path, bytes.data(), bytes.size() ) );
    std::ifstream stream( path, std::ios::binary );
    std::vector< uint8_t > saved( ( std::istreambuf_iterator< char >( stream ) ),
                                   std::istreambuf_iterator< char >() );
    assert( saved == bytes );
    stream.close();
    Terrain3DComposedSurfaceMappedFile mapped;
    assert( !mapped.openCold( path, 111u ) );
    assert( mapped.openCold( path, source.m_nSourceFingerprint ) );
    assert( mapped.view().m_header.m_nPageCount == 4u );
    mapped.closeCold();
    assert( mapped.view().m_pFile == nullptr );
    remove( path );
    t3dComposedSurfaceSidecarView_t view{};
    assert( !t3d_cs_open_sidecar_cold( bytes.data(), bytes.size(), 555u, view ) );
    assert( t3d_cs_open_sidecar_cold( bytes.data(), bytes.size(), 0x12345678u, view ) );
    uint32_t page_id = T3D_CS_INVALID_SLOT;
    assert( t3d_cs_find_page( view, 55u, 0u, 1u, 0u, 91u, page_id ) && page_id == 1u );
    assert( !t3d_cs_find_page( view, 55u, 0u, 1u, 0u, 92u, page_id ) );
    assert( !t3d_cs_find_page( view, 99u, 0u, 1u, 0u, 91u, page_id ) );

    t3dComposedSurfaceSlot_t slots[2]{};
    t3dComposedSurfaceRequest_t queue[4]{};
    Terrain3DComposedSurfaceCache cache;
    assert( cache.initializeCold( view, slots, 2u, queue, 4u, 5u ) );
    assert( cache.demandHot( 0u, 1u, 10u ) );
    assert( cache.demandHot( 0u, 4u, 12u ) );
    assert( cache.demandHot( 1u, 8u, 13u ) );
    assert( cache.stats().m_nPending == 2u && cache.stats().m_nDeduplicated == 1u );
    t3dComposedSurfaceJob_t first{}, second{};
    assert( !cache.claimNextHot( false, first ) );
    assert( cache.claimNextHot( true, first ) && first.m_nPageIndex == 1u );
    std::vector< uint8_t > raw( T3D_CS_PAGE_PAYLOAD_BYTES );
    assert( cache.readJobCold( first, raw.data(), uint32_t( raw.size() ) ) );
    assert( !cache.completeUploadHot( first, T3D_CS_CHANNEL_MASK_ALL, true ) );
    assert( cache.markUploadSubmittedHot( first ) );
    assert( !cache.completeUploadHot( first, 3u, true ) );
    assert( !cache.completeUploadHot( first, 7u, false ) );
    t3dComposedSurfacePageMapping_t mapping{};
    assert( !cache.residentMappingHot( 1u, mapping ) );
    assert( cache.completeUploadHot( first, 7u, true ) );
    assert( cache.residentMappingHot( 1u, mapping ) && mapping.m_nPhysicalSlot == first.m_nSlot );
    assert( cache.demandHot( 1u, 99u, 20u ) && cache.stats().m_nHits == 1u );
    assert( cache.claimNextHot( true, second ) && second.m_nPageIndex == 0u );
    assert( cache.readJobCold( second, raw.data(), uint32_t( raw.size() ) ) );
    assert( cache.markUploadSubmittedHot( second ) );
    assert( cache.completeUploadHot( second, 7u, true ) );
    assert( cache.demandHot( 2u, 16u, 30u ) );
    t3dComposedSurfaceJob_t retired{};
    assert( !cache.claimNextHot( true, retired ) );
    assert( cache.beginEvictionHot( retired ) && retired.m_nPageIndex == 0u );
    assert( !cache.residentMappingHot( retired.m_nPageIndex, mapping ) );
    assert( !cache.claimNextHot( true, second ) ); // No early reuse.
    assert( cache.confirmEvictionHot( retired ) );
    t3dComposedSurfaceJob_t third{};
    assert( cache.claimNextHot( true, third ) && third.m_nPageIndex == 2u );
    // Corruption after opening is detected at the read/CRC boundary.
    const size_t corruption_at = size_t( view.m_header.m_nDataOffset + 2u * T3D_CS_PAGE_PAYLOAD_BYTES );
    bytes[corruption_at] ^= 0x01u;
    assert( !cache.readJobCold( third, raw.data(), uint32_t( raw.size() ) ) );
    assert( cache.stats().m_nCorruptPages == 1u );
    bytes[corruption_at] ^= 0x01u;
    assert( cache.readJobCold( third, raw.data(), uint32_t( raw.size() ) ) );
    assert( cache.markUploadSubmittedHot( third ) );
    cache.resetDeviceCold( 6u );
    assert( !cache.completeUploadHot( third, 7u, true ) );
    assert( !cache.residentMappingHot( 2u, mapping ) );
    assert( cache.demandHot( 3u, 1u, 1u ) );
    t3dComposedSurfaceJob_t fresh{};
    assert( cache.claimNextHot( true, fresh ) && fresh.m_nDeviceGeneration == 6u );
    assert( fresh.m_nToken != third.m_nToken );
    assert( cache.cancelJobHot( fresh ) );
    assert( !cache.readJobCold( fresh, raw.data(), uint32_t( raw.size() ) ) );
    assert( !cache.residentMappingHot( 3u, mapping ) );

    // CS09: invalidate a *particular* resident page after an editor revision
    // change, even with no replacement demand and no cache-pressure eviction.
    assert( cache.demandHot( 1u, 8u, 31u ) );
    t3dComposedSurfaceJob_t editJob{};
    assert( cache.claimNextHot( true, editJob ) && editJob.m_nPageIndex == 1u );
    assert( cache.readJobCold( editJob, raw.data(), uint32_t( raw.size() ) ) );
    assert( cache.markUploadSubmittedHot( editJob ) );
    assert( cache.completeUploadHot( editJob, T3D_CS_CHANNEL_MASK_ALL, true ) );
    assert( cache.residentMappingHot( 1u, mapping ) );
    // Editor bridge maps *new revision* authoring identity back to old sidecar
    // records and rejects insufficient fixed retirement storage atomically.
    t3dComposedSurfaceCookPage_t updatedPages[4] = { pages[0], pages[1], pages[2], pages[3] };
    updatedPages[1].m_nCompositionRevision = 94u;
    updatedPages[1].m_nGeneration = 2u;
    uint8_t editedFlags[4] = { 0u, 1u, 0u, 0u };
    t3dComposedSurfaceJob_t editJobs[2]{};
    uint32_t editsStarted = 99u;
    assert( !t3d_cs_begin_dirty_page_retirements_hot( cache, updatedPages, editedFlags,
        4u, editJobs, 0u, editsStarted ) && editsStarted == 0u );
    assert( cache.residentMappingHot( 1u, mapping ) );
    assert( t3d_cs_begin_dirty_page_retirements_hot( cache, updatedPages, editedFlags,
        4u, editJobs, 2u, editsStarted ) && editsStarted == 1u );
    assert( cache.retirementPendingHot( editJobs[0] ) );
    assert( !cache.residentMappingHot( 1u, mapping ) );
    assert( cache.cancelRetirementHot( editJobs[0] ) );
    assert( cache.residentMappingHot( 1u, mapping ) );

    t3dComposedSurfaceJob_t editRetirement{};
    assert( !cache.beginInvalidationHot( 99u, editRetirement ) );
    assert( cache.beginInvalidationHot( 1u, editRetirement ) );
    assert( editRetirement.m_nSlot == editJob.m_nSlot );
    assert( cache.retirementPendingHot( editRetirement ) );
    assert( !cache.residentMappingHot( 1u, mapping ) );
    assert( !cache.beginInvalidationHot( 1u, editJob ) );
    t3dComposedSurfaceJob_t forged = editRetirement;
    ++forged.m_nToken;
    assert( !cache.retirementPendingHot( forged ) );
    assert( !cache.confirmEvictionHot( forged ) );
    assert( !cache.cancelRetirementHot( forged ) );
    assert( cache.cancelRetirementHot( editRetirement ) );
    assert( cache.residentMappingHot( 1u, mapping ) );
    assert( cache.beginInvalidationHot( 1u, editRetirement ) );
    assert( cache.confirmEvictionHot( editRetirement ) );
    assert( !cache.residentMappingHot( 1u, mapping ) );
    assert( !cache.retirementPendingHot( editRetirement ) );

    // A dirty page must also cancel old-sidecar requests still waiting in the
    // bounded priority queue, without claiming or allocating a physical slot.
    editedFlags[1] = 0u;
    editedFlags[3] = 1u;
    assert( cache.demandHot( 3u, 11u, 33u ) );
    assert( cache.stats().m_nPending == 1u );
    assert( t3d_cs_begin_dirty_page_retirements_hot( cache, updatedPages, editedFlags,
        4u, editJobs, 2u, editsStarted ) && editsStarted == 0u );
    assert( cache.stats().m_nPending == 0u );

    // No obsolete reader/upload job may publish after an authoring edit.
    assert( cache.demandHot( 0u, 3u, 35u ) );
    t3dComposedSurfaceJob_t staleRead{};
    assert( cache.claimNextHot( true, staleRead ) && staleRead.m_nPageIndex == 0u );
    editedFlags[1] = 0u;
    editedFlags[0] = 1u;
    assert( t3d_cs_begin_dirty_page_retirements_hot( cache, updatedPages, editedFlags,
        4u, editJobs, 2u, editsStarted ) && editsStarted == 0u );
    assert( !cache.readJobCold( staleRead, raw.data(), uint32_t( raw.size() ) ) );
    assert( cache.demandHot( 0u, 3u, 37u ) );
    assert( cache.claimNextHot( true, staleRead ) );
    assert( cache.readJobCold( staleRead, raw.data(), uint32_t( raw.size() ) ) );
    assert( cache.markUploadSubmittedHot( staleRead ) );
    // An in-flight GPU transfer must first finish/retire at the owning GPU boundary.
    assert( !t3d_cs_begin_dirty_page_retirements_hot( cache, updatedPages, editedFlags,
        4u, editJobs, 2u, editsStarted ) && editsStarted == 0u );
    assert( cache.completeUploadHot( staleRead, 7u, true ) );
    assert( t3d_cs_begin_dirty_page_retirements_hot( cache, updatedPages, editedFlags,
        4u, editJobs, 2u, editsStarted ) && editsStarted == 1u );
    assert( cache.confirmEvictionHot( editJobs[0] ) );

    producer.fail_at = producer.evaluations + 2u;
    assert( !t3d_cs_cook_pages_cold( pages, 4u, source, scratch.data(), uint32_t( scratch.size() ),
                                     reproduced.data(), reproduced.size(), written ) );
    assert( written == 0u );
    assert( !t3d_cs_write_sidecar_cold( "/tmp/bad.t3sc", reproduced.data(), reproduced.size() ) );
    printf( "CS03/CS04: deterministic bake, persisted sidecar, CRC, lookup, demand, eviction and generations passed\n" );
}
int main() { test_pages(); return 0; }
