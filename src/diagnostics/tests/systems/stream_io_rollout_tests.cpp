#include "diagnostics/tests/systems/test_harness.h"
#include "data/persistence/terrain_3d_raster_bundle.h"
#include "streaming/io/terrain_3d_stream_read_cache.h"
#include "streaming/hierarchy/terrain_3d_hierarchical_tile_store.h"
#include "streaming/hierarchy/terrain_3d_hierarchy_scheduler_io.h"
#include "data/cooking/terrain_3d_raster_hierarchy_builder.h"
#include "data/storage/terrain_3d_raster_tile_store.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include <chrono>
#include <thread>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <string.h>

namespace terrain3d_tests
{
namespace
{
uint64_t hashRaster(
	const uint8_t *pBytes,
	const size_t   nBytes ) {
	uint64_t hash = 1469598103934665603ull;
	for ( size_t i = 0u; i < nBytes; ++i ) {
		hash = ( hash ^ pBytes[i] ) * 1099511628211ull;
	}
	return hash;
}
} // namespace

void register_stream_io_rollout_tests(
	TestRunner &runner ) {
	runner.run( "stream_io_rollout", "raster_shared_payload_reuse_and_reopen", []( TestContext &test ) {
		const String	 dirPath = "user://terrain3d_io07_raster";
		Ref< DirAccess > directory = DirAccess::open( "user://" );
		T3D_REQUIRE( test, directory.is_valid() );
		const Error mkdirResult = directory->make_dir( "terrain3d_io07_raster" );
		T3D_REQUIRE( test, mkdirResult == OK || mkdirResult == ERR_ALREADY_EXISTS );
		const uint16_t first = t3dRasterTileId( Vector2i( 0, 0 ), TYPE_HEIGHT, 0u, 0u );
		const uint16_t second = t3dRasterTileId( Vector2i( 1, 0 ), TYPE_HEIGHT, 0u, 0u );
		T3D_REQUIRE( test, first != UINT16_MAX && second != UINT16_MAX );
		t3dRasterTile_t tile;
		tile.m_eEncoding = T3D_RASTER_TILE_RAW;
		tile.m_nRevision = 1u;
		tile.m_bytes.resize( t3dRasterTileRawBytes( TYPE_HEIGHT ) );
		for ( int i = 0; i < tile.m_bytes.size(); ++i ) {
			tile.m_bytes.set( i, ( uint8_t ) ( ( i * 67 + i / 3 ) % 251 ) );
		}
		tile.m_nHash = hashRaster( tile.m_bytes.ptr(), ( size_t ) tile.m_bytes.size() );
		Terrain3DRasterTileStore store;
		store.importTile( first, tile );
		store.importTile( second, tile );
		T3D_REQUIRE( test, Terrain3DRasterBundle::save( dirPath, store ) == OK );
		Terrain3DRasterBundleIndex index;
		T3D_REQUIRE( test, index.open( dirPath ) == OK );
		PackedByteArray a, b, again;
		T3D_REQUIRE( test, index.readTile( first, a ) );
		T3D_REQUIRE( test, index.readTile( second, b ) );
		T3D_REQUIRE( test, index.readTile( first, again ) );
		T3D_CHECK( test, a == tile.m_bytes && b == a && again == a );
		const Dictionary stats = index.getSourceIoStats();
		const uint64_t	 reads = ( uint64_t ) ( int64_t ) stats["raster_stored_file_reads"];
		const uint64_t	 hits = ( uint64_t ) ( int64_t ) stats["raster_stored_cache_hits"];
		const uint64_t	 modified = FileAccess::get_modified_time( Terrain3DRasterBundle::getPath( dirPath ) );
		T3D_CHECK( test, reads == ( modified ? 1u : 3u ) );
		T3D_CHECK( test, hits == ( modified ? 2u : 0u ) );
		T3D_CHECK( test, ( int64_t ) stats["raster_stored_cache_bytes"] <= 128ll * 4096ll );
		index.clear();
		T3D_CHECK( test, ( int64_t ) index.getSourceIoStats()["raster_stored_cache_bytes"] == 0 );
		T3D_REQUIRE( test, index.open( dirPath ) == OK );
		PackedByteArray after;
		T3D_REQUIRE( test, index.readTile( second, after ) );
		T3D_CHECK( test, after == tile.m_bytes );
		index.clear();
		directory->remove( "terrain3d_io07_raster/terrain3d_raster.t3rb" );
		directory->remove( "terrain3d_io07_raster" );
	} );
	runner.run( "stream_io_rollout", "cooked_cache_clearing_prevents_stale_payload_reuse", []( TestContext &test ) {
		const String	path = "user://terrain3d_io07_epoch.bin";
		PackedByteArray bytes;
		bytes.resize( 4096 );
		for ( int i = 0; i < bytes.size(); ++i ) {
			bytes.set( i, ( uint8_t ) ( i % 253 ) );
		}
		{
			Ref< FileAccess > writer = FileAccess::open( path, FileAccess::WRITE );
			T3D_REQUIRE( test, writer.is_valid() );
			writer->store_buffer( bytes );
		}
		Ref< FileAccess > reader = FileAccess::open( path, FileAccess::READ );
		T3D_REQUIRE( test, reader.is_valid() );
		Terrain3DStreamReadCache cache;
		t3dStreamRangeKey_t		 key;
		key.m_path = path;
		key.m_nModifiedTime = FileAccess::get_modified_time( path );
		key.m_nFileLength = reader->get_length();
		key.m_nSourceSignature = 111u;
		key.m_nStoredBytes = ( uint32_t ) bytes.size();
		key.m_nDecodedBytes = key.m_nStoredBytes;
		key.m_nCompression = TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE;
		key.m_nChecksum = t3dRuntimePageChecksum( bytes.ptr(), bytes.size() );
		PackedByteArray a, b;
		uint64_t		readUsec = 0u, verifyUsec = 0u;
		T3D_REQUIRE( test, cache.readStored( reader, key, a, readUsec, verifyUsec ) );
		cache.clear();
		T3D_CHECK( test, cache.getStats().m_nResidentBytes == 0u );
		T3D_REQUIRE( test, cache.readStored( reader, key, b, readUsec, verifyUsec ) );
		T3D_CHECK( test, a == b && b == bytes );
		T3D_CHECK( test, cache.getStats().m_nRangeReadCalls == 1u );
		reader.unref();
		{
			Ref< DirAccess > dir = DirAccess::open( "user://" );
			if ( dir.is_valid() ) {
				dir->remove( "terrain3d_io07_epoch.bin" );
			}
		}
	} );
    runner.run( "stream_io_rollout", "f02_hierarchy_async_merge_cancel_epoch_and_integrity", []( TestContext &test ) {
        const String directory = "user://terrain3d_async_hierarchy";
        T3D_REQUIRE( test, DirAccess::make_dir_recursive_absolute( directory ) == OK );
        Terrain3DRasterTileStore source;
        Terrain3DRasterHierarchyBuilder builder;
        const Vector2i locations[2] = { Vector2i( -1, -1 ), Vector2i( 0, 0 ) };
        for ( uint32_t i = 0u; i < 2u; ++i ) {
            const Ref< Image > height = t3dRegionCodecConvertHeightToR16( Ref< Image >(), T3D_REGION_SIZE );
            T3D_REQUIRE( test, height.is_valid() );
            t3dRegionCodecWriteHeightPixel( *height, Vector2i( 5, 7 ), 100.0f + ( float ) i );
            T3D_REQUIRE( test, source.updateRegion( locations[i], TYPE_HEIGHT, height,
                Rect2i( 0, 0, 128, 128 ) ) );
            builder.updateRegion( locations[i], source );
        }
        T3D_REQUIRE( test, Terrain3DRasterHierarchyBundle::saveV2( directory, builder ) == OK );
        Terrain3DRasterHierarchyStreamIndex index;
        T3D_REQUIRE( test, index.open( directory ) == OK );
        const uint16_t id = Terrain3DRasterHierarchyBuilder::nodeId( 5u, 15u, 15u );
        const t3dRasterHierarchyStreamNode_t *node = index.getNode( id );
        T3D_REQUIRE( test, node != nullptr );
        // Fixed pool is cold allocated; both consumer requests must share one
        // physical verified I/O operation and one immutable source revision.
        Terrain3DHierarchicalTileStore store;
        T3D_REQUIRE( test, store.open( directory, 2u ) );
        Terrain3DHierarchyScheduler scheduler;
        T3D_REQUIRE( test, t3dHierarchyBindFromTileStoreCold( scheduler, store ) );
        T3D_CHECK( test, scheduler.generation() == index.getGeneration() );
        t3dHierarchyView_t camera;
        camera.m_fCameraX = -32.0f;
        camera.m_fCameraY = 110.0f;
        camera.m_fCameraZ = -32.0f;
        t3dHierarchyScheduleLimits_t limits;
        scheduler.schedule( camera, limits, 4u );
        T3D_CHECK( test, scheduler.coverCount() > 0u &&
            scheduler.stats().m_nExactReserved == 4u );
        const uint64_t epoch = store.getEpoch();
        store.requestNode( id, T3D_HIERARCHY_TILE_HEIGHT, 10u, node->m_nRevision );
        store.requestNode( id, T3D_HIERARCHY_TILE_COLOR, 200u, node->m_nRevision );
        store.service();
        for ( int i = 0; i < 500 && !store.isReady( id, 5u, node->m_nRevision ); ++i ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        T3D_REQUIRE( test, store.isReady( id, 5u, node->m_nRevision ) );
        const t3dHierarchyTileStoreStats_t stats = store.getStats();
        T3D_CHECK( test, stats.m_nFileReads == 1u && stats.m_nCoalescedRequests == 1u );
        T3D_CHECK( test, stats.m_nSlotCount == 2u && stats.m_nResidentPoolBytes < 32768u );
        t3dHierarchyPreparedTile_t prepared;
        T3D_REQUIRE( test, store.takePrepared( id, 5u, node->m_nRevision, prepared ) );
        T3D_CHECK( test, prepared.m_nContentGeneration == index.getGeneration() &&
            prepared.m_nStoreEpoch == epoch && prepared.m_nSourceRevision == node->m_nRevision );
        T3D_CHECK( test, prepared.m_height.size() == T3D_RASTER_HIERARCHY_STREAM_HEIGHT_BYTES &&
            prepared.m_color.size() == T3D_RASTER_HIERARCHY_STREAM_COLOR_BYTES &&
            prepared.m_control.is_empty() );
        T3D_CHECK( test, !store.isReady( id, 5u, node->m_nRevision ) );
        // Cancelled work cannot publish and reset waits for worker quiescence.
        store.requestNode( id, 7u, 100u, node->m_nRevision );
        store.cancel( id );
        T3D_CHECK( test, !store.isReady( id, 7u, node->m_nRevision ) );
        store.reset();
        T3D_CHECK( test, !t3dHierarchyBindFromTileStoreCold( scheduler, store ) &&
            scheduler.generation() == 0u );
        T3D_CHECK( test, store.getEpoch() != epoch && store.getStats().m_nSlotCount == 0u );
        T3D_REQUIRE( test, store.open( directory, 1u ) );
        const String path = Terrain3DRasterHierarchyBundle::getPathV2( directory );
        Ref< FileAccess > file = FileAccess::open( path, FileAccess::READ_WRITE );
        T3D_REQUIRE( test, file.is_valid() );
        file->seek( node->m_nHeightOffset );
        const uint8_t original = file->get_8();
        file->seek( node->m_nHeightOffset );
        file->store_8( original ^ 0x40u );
        file->flush();
        file->close();
        store.requestNode( id, 7u, 1u, node->m_nRevision );
        for ( int i = 0; i < 500 && store.getStats().m_nFileReads == 0u; ++i ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        T3D_CHECK( test, store.getStats().m_nFailed >= 1u &&
            !store.isReady( id, 7u, node->m_nRevision ) );
        store.reset();
        T3D_REQUIRE( test, Terrain3DRasterHierarchyBundle::saveV2( directory, builder ) == OK );
        T3D_REQUIRE( test, store.open( directory, 1u ) );
        store.requestNode( id, 7u, 10u, node->m_nRevision );
        for ( int i = 0; i < 500 && !store.isReady( id, 7u, node->m_nRevision ); ++i ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        T3D_CHECK( test, store.isReady( id, 7u, node->m_nRevision ) );
        store.reset();
    } );

}
} // namespace terrain3d_tests
