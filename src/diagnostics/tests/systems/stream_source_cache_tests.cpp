#include "diagnostics/tests/systems/test_harness.h"
#include "streaming/io/terrain_3d_stream_read_cache.h"
#include "streaming/io/terrain_3d_shared_stream_pack.h"
#include "streaming/cache/terrain_3d_cpu_region_cache.h"
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>

namespace terrain3d_tests
{

static void t3dPackedTestCleanup( const String &base ) {
	Ref< DirAccess > dir = DirAccess::open( "user://" );
	if ( dir.is_null() ) {
		return;
	}
	// Only the known test namespace; never clean arbitrary published assets.
	const String name = base.get_file();
	dir->remove( name );
	dir->remove( name + String( ".t3pm" ) );
	dir->remove( name + String( ".commit0" ) );
	dir->remove( name + String( ".commit1" ) );
	for ( int slot = 0; slot < 2; ++slot ) {
		const String commit = name + ( slot == 0 ? String( ".commit0" ) : String( ".commit1" ) );
		dir->remove( commit + String( ".staged" ) );
		dir->remove( commit + String( ".previous" ) );
	}
	for ( int i = 1; i <= 16; ++i ) {
		const String generation = name + String( ".gen" ) + String::num_int64( i );
		dir->remove( generation );
		dir->remove( generation + String( ".t3pm" ) );
		dir->remove( generation + String( ".staged" ) );
		dir->remove( generation + String( ".previous" ) );
		dir->remove( generation + String( ".t3pm.tmp" ) );
		dir->remove( generation + String( ".t3pm.bak" ) );
	}
}

static void t3dPackedTestWriteU32(
	PackedByteArray &rData,
	const int		 nOffset,
	const uint32_t	 nValue ) {
	for ( int i = 0; i < 4; ++i ) {
		rData.set( nOffset + i, ( uint8_t ) ( nValue >> ( 8 * i ) ) );
	}
}

static PackedByteArray t3dPackedTestPage(
	const PackedByteArray &rPayload ) {
	PackedByteArray page;
	const uint32_t	size = ( uint32_t ) rPayload.size();
	page.resize( T3D_RUNTIME_PAGE_HEADER_BYTES );
	page.fill( 0u );
	t3dPackedTestWriteU32( page, 0, T3D_RUNTIME_PAGE_MAGIC );
	t3dPackedTestWriteU32( page, 4, T3D_RUNTIME_PAGE_VERSION );
	t3dPackedTestWriteU32( page, 8, T3D_RUNTIME_PAGE_HEADER_BYTES );
	t3dPackedTestWriteU32( page, 28, T3D_RUNTIME_PAGE_HEADER_BYTES + size );
	t3dPackedTestWriteU32( page, 32, 1u );
	t3dPackedTestWriteU32( page, 36, 40u );
	t3dPackedTestWriteU32( page, 40, TERRAIN3D_RUNTIME_PAGE_HEIGHT );
	t3dPackedTestWriteU32( page, 44, 1u );
	t3dPackedTestWriteU32( page, 48, TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE );
	t3dPackedTestWriteU32( page, 52, T3D_RUNTIME_PAGE_HEADER_BYTES );
	t3dPackedTestWriteU32( page, 56, size );
	t3dPackedTestWriteU32( page, 60, size );
	t3dPackedTestWriteU32( page, 80, t3dRuntimePageChecksum( rPayload.ptr(), size ) );
	t3dPackedTestWriteU32( page, 40 + T3D_RUNTIME_PAGE_CHANNEL_COUNT * 48, 555u );
	page.append_array( rPayload );
	return page;
}

void register_stream_source_cache_tests(
	TestRunner &runner ) {
#if defined( T3D_TESTS_ENABLED )
	runner.run( "stream_source_cache", "cpu_unmapped_invalidation_preserves_async_retirement", []( TestContext &test ) {
		Terrain3DCpuRegionCache cache( NULL, NULL, NULL, NULL );
		const Vector2i location( 2, -3 );
		T3D_REQUIRE( test, cache.seedAsyncRequestForTest( location, false, "user://async_old_region.res" ) );
		const uint32_t firstGeneration = cache.asyncGenerationForTest( location );
		T3D_CHECK( test, cache.outstandingForTest() == 1 );
		cache.invalidate( location ); // Not in the resident cache or repository.
		T3D_CHECK( test, cache.asyncStateForTest( location ) == 3u );
		T3D_CHECK( test, cache.outstandingForTest() == 1 );
		cache.invalidate( location );
		T3D_CHECK( test, cache.outstandingForTest() == 1 );
		T3D_CHECK( test, !cache.seedAsyncRequestForTest( location, false, "user://async_new_region.res" ) );
		cache.completeAsyncRequestForTest( location ); // Late callback drains old request.
		cache.completeAsyncRequestForTest( location ); // Duplicate must not underflow.
		T3D_CHECK( test, cache.outstandingForTest() == 0 );
		T3D_REQUIRE( test, cache.seedAsyncRequestForTest( location, false, "user://async_new_region.res" ) );
		T3D_CHECK( test, cache.asyncGenerationForTest( location ) > firstGeneration );
		cache.completeAsyncRequestForTest( location );
		T3D_CHECK( test, cache.outstandingForTest() == 0 );
	} );
	runner.run( "stream_source_cache", "cpu_cooked_async_cancel_does_not_clear_outstanding", []( TestContext &test ) {
		Terrain3DCpuRegionCache cache( NULL, NULL, NULL, NULL );
		const Vector2i location( -2, 4 );
		T3D_REQUIRE( test, cache.seedAsyncRequestForTest( location, true, String() ) );
		cache.invalidate( location );
		T3D_CHECK( test, cache.asyncStateForTest( location ) == 3u );
		T3D_CHECK( test, cache.outstandingForTest() == 1 );
		cache.completeAsyncRequestForTest( location );
		T3D_CHECK( test, cache.outstandingForTest() == 0 );
	} );
#endif

	runner.run( "stream_io", "packed_baked_pages_alias_and_cache", []( TestContext &test ) {
		const String	packPath = "user://terrain3d_t3sp_native_test.t3sp";
		t3dPackedTestCleanup( packPath );
		const String	first = "user://region0.t3spage";
		const String	second = "user://region1.t3spage";
		PackedByteArray payload;
		payload.resize( 2048 );
		for ( int i = 0; i < payload.size(); ++i ) {
			payload.set( i, ( uint8_t ) ( i % 107 ) );
		}
		eastl::vector< t3dBakedRegionPage_t > pages;
		for ( const String &path : { first, second } ) {
			t3dBakedRegionPage_t page;
			page.m_virtualPath = path;
			page.m_encodedPage = t3dPackedTestPage( payload );
			pages.push_back( page );
		}
		T3D_REQUIRE( test, Terrain3DSharedStreamPack::buildBakedPages( packPath, pages ) );
		Terrain3DSharedStreamPack pack;
		T3D_REQUIRE( test, pack.open( packPath ) );
		T3D_CHECK( test, pack.itemCount() == 2u );
		Terrain3DStreamReadCache cache;
		T3D_REQUIRE( test, cache.setSharedPackPath( packPath ) );
		t3dStreamRangeKey_t physical[2];
		for ( int i = 0; i < 2; ++i ) {
			const String	virtualPath = i ? second : first;
			PackedByteArray encodedHeader;
			uint64_t		logicalLength = 0u;
			T3D_REQUIRE( test, cache.getPackPageMetadata( virtualPath, encodedHeader, logicalLength ) );
			T3D_CHECK( test, logicalLength == ( uint64_t ) ( T3D_RUNTIME_PAGE_HEADER_BYTES + payload.size() ) );
			t3dStreamRangeKey_t request;
			request.m_path = virtualPath;
			request.m_nSourceSignature = 555u;
			request.m_nOffset = T3D_RUNTIME_PAGE_HEADER_BYTES;
			request.m_nStoredBytes = payload.size();
			request.m_nDecodedBytes = payload.size();
			request.m_nFileLength = logicalLength;
			request.m_nChecksum = t3dRuntimePageChecksum( payload.ptr(), payload.size() );
			request.m_nChannel = TERRAIN3D_RUNTIME_PAGE_HEIGHT;
			request.m_nFormat = 1u;
			T3D_REQUIRE( test, pack.resolve( request, physical[i] ) );
			Ref< FileAccess > noIndividualFile;
			PackedByteArray	  loaded;
			uint64_t		  readUs = 0u, verifyUs = 0u;
			T3D_REQUIRE( test, cache.readStored( noIndividualFile, request, loaded, readUs, verifyUs ) );
			T3D_CHECK( test, loaded == payload );
		}
		T3D_CHECK( test, physical[0].m_nOffset == physical[1].m_nOffset );
		T3D_CHECK( test, cache.getStats().m_nPackHits >= 2u );
		T3D_CHECK( test, cache.getStats().m_nOpenCalls == 0u );
		cache.setBudget( 0u );
		T3D_CHECK( test, cache.getStats().m_nResidentBytes == 0u );
		( void ) cache.setSharedPackPath( String() );
		t3dPackedTestCleanup( packPath );
	} );
	runner.run( "stream_io", "packed_index_order_and_cold_dedup", []( TestContext &test ) {
		const String base = "user://terrain3d_t3sp_index_test.t3sp";
		t3dPackedTestCleanup( base );
		eastl::vector< t3dBakedRegionPage_t > pages;
		PackedByteArray payloads[7];
		for ( uint32_t category = 0u; category < 7u; ++category ) {
			payloads[category].resize( 64 );
			for ( int i = 0; i < payloads[category].size(); ++i ) {
				payloads[category].set( i, ( uint8_t ) ( category * 17u + ( uint32_t ) i ) );
			}
		}
		// Reverse insertion order to exercise the sorted metadata/index lookup.
		for ( int i = 63; i >= 0; --i ) {
			t3dBakedRegionPage_t page;
			page.m_virtualPath = "user://p13_p14_region_" + String::num_int64( i ) + ".t3spage";
			page.m_encodedPage = t3dPackedTestPage( payloads[i % 7] );
			pages.push_back( page );
		}
		T3D_REQUIRE( test, Terrain3DSharedStreamPack::buildBakedPages( base, pages ) );
		Terrain3DSharedStreamPack pack;
		T3D_REQUIRE( test, pack.open( base ) );
		T3D_CHECK( test, pack.itemCount() == 64u );
		Terrain3DStreamReadCache cache;
		T3D_REQUIRE( test, cache.setSharedPackPath( base ) );
		uint32_t seenOffsets[7] = {};
		for ( int i = 0; i < 64; ++i ) {
			const String path = "user://p13_p14_region_" + String::num_int64( i ) + ".t3spage";
			PackedByteArray header;
			uint64_t length = 0u;
			T3D_REQUIRE( test, pack.lookupPage( path, header, length ) );
			PackedByteArray adoptedHeader;
			uint64_t adoptedLength = 0u;
			T3D_REQUIRE( test, cache.getPackPageMetadata( path, adoptedHeader, adoptedLength ) );
			T3D_CHECK( test, adoptedHeader == header && adoptedLength == length );
			T3D_CHECK( test, length == T3D_RUNTIME_PAGE_HEADER_BYTES + 64u );
			t3dStreamRangeKey_t key;
			key.m_path = path;
			key.m_nSourceSignature = 555u;
			key.m_nOffset = T3D_RUNTIME_PAGE_HEADER_BYTES;
			key.m_nStoredBytes = 64u;
			key.m_nDecodedBytes = 64u;
			key.m_nChecksum = t3dRuntimePageChecksum( payloads[i % 7].ptr(), 64u );
			key.m_nChannel = TERRAIN3D_RUNTIME_PAGE_HEIGHT;
			key.m_nFormat = 1u;
			t3dStreamRangeKey_t resolved;
			T3D_REQUIRE( test, pack.resolve( key, resolved ) );
			T3D_CHECK( test, resolved.m_path == pack.path() && resolved.m_nOffset >= 40u );
			uint32_t &same = seenOffsets[i % 7];
			if ( same == 0u ) {
				same = resolved.m_nOffset;
			} else {
				T3D_CHECK( test, same == resolved.m_nOffset );
			}
		}
		t3dStreamRangeKey_t missing;
		missing.m_path = "user://p13_p14_absent.t3spage";
		missing.m_nSourceSignature = 555u;
		T3D_CHECK( test, !pack.resolve( missing, missing ) );
		T3D_CHECK( test, cache.setSharedPackPath( String() ) );
		t3dPackedTestCleanup( base );
	} );
	runner.run( "stream_io", "packed_metadata_corruption_is_rejected", []( TestContext &test ) {
		const String	packPath = "user://terrain3d_t3sp_corrupt_test.t3sp";
		t3dPackedTestCleanup( packPath );
		PackedByteArray payload;
		payload.resize( 64 );
		payload.fill( 19u );
		t3dBakedRegionPage_t entry;
		entry.m_virtualPath = "user://terrain_corrupt.t3spage";
		entry.m_encodedPage = t3dPackedTestPage( payload );
		eastl::vector< t3dBakedRegionPage_t > pages;
		pages.push_back( entry );
		T3D_REQUIRE( test, Terrain3DSharedStreamPack::buildBakedPages( packPath, pages ) );
		Terrain3DSharedStreamPack valid;
		T3D_REQUIRE( test, valid.open( packPath ) );
		Ref< FileAccess > metadata = FileAccess::open( valid.path() + String( ".t3pm" ), FileAccess::READ_WRITE );
		T3D_REQUIRE( test, metadata.is_valid() );
		metadata->seek( 32u );
		const uint8_t original = metadata->get_8();
		metadata->seek( 32u );
		metadata->store_8( original ^ 0xffu );
		metadata->flush();
		metadata.unref();
		Terrain3DSharedStreamPack corrupted;
		T3D_CHECK( test, !corrupted.open( packPath ) );
		t3dPackedTestCleanup( packPath );
	} );
#ifdef T3D_TESTS_ENABLED
	runner.run( "stream_io", "generation_commit_faults_retain_previous_pair", []( TestContext &test ) {
		const String base = "user://terrain3d_t3sp_publication_test.t3sp";
		t3dPackedTestCleanup( base );
		PackedByteArray firstPayload, secondPayload;
		firstPayload.resize( 64 );
		secondPayload.resize( 64 );
		firstPayload.fill( 7u );
		secondPayload.fill( 23u );
		t3dBakedRegionPage_t entry;
		entry.m_virtualPath = "user://publication_test.t3spage";
		entry.m_encodedPage = t3dPackedTestPage( firstPayload );
		eastl::vector< t3dBakedRegionPage_t > pages;
		pages.push_back( entry );
		T3D_REQUIRE( test, Terrain3DSharedStreamPack::buildBakedPages( base, pages ) );
		Terrain3DSharedStreamPack initial;
		T3D_REQUIRE( test, initial.open( base ) );
		const String oldPath = initial.path();
		pages[0].m_encodedPage = t3dPackedTestPage( secondPayload );
		for ( uint32_t fault = 1u; fault <= 5u; ++fault ) {
			Terrain3DSharedStreamPack::setPublicationFaultForTest( fault );
			T3D_CHECK( test, !Terrain3DSharedStreamPack::buildBakedPages( base, pages ) );
			Terrain3DSharedStreamPack recovered;
			T3D_REQUIRE( test, recovered.open( base ) );
			T3D_CHECK( test, recovered.path() == oldPath );
		}
		Terrain3DSharedStreamPack::setPublicationFaultForTest( 0u );
		T3D_REQUIRE( test, Terrain3DSharedStreamPack::buildBakedPages( base, pages ) );
		Terrain3DSharedStreamPack current;
		T3D_REQUIRE( test, current.open( base ) );
		T3D_CHECK( test, current.path() != oldPath );
		// Old readers have already resolved an immutable physical generation.
		PackedByteArray header;
		uint64_t length = 0u;
		T3D_CHECK( test, initial.lookupPage( entry.m_virtualPath, header, length ) );
		// An invalid newer pair must fall back to the previous committed slot.
		Ref< FileAccess > meta = FileAccess::open( current.path() + String( ".t3pm" ), FileAccess::READ_WRITE );
		T3D_REQUIRE( test, meta.is_valid() );
		meta->seek( 32u );
		const uint8_t previous = meta->get_8();
		meta->seek( 32u );
		meta->store_8( previous ^ 0xa5u );
		meta->flush();
		meta.unref();
		Terrain3DSharedStreamPack recovered;
		T3D_REQUIRE( test, recovered.open( base ) );
		T3D_CHECK( test, recovered.path() == oldPath );
		t3dPackedTestCleanup( base );
	} );
#endif // T3D_TESTS_ENABLED
	runner.run( "stream_io", "old_individual_page_not_a_runtime_source", []( TestContext &test ) {
		Terrain3DStreamReadCache cache;
		t3dStreamRangeKey_t		 key;
		key.m_path = "user://old_terrain.t3dpage";
		key.m_nFileLength = 256u;
		key.m_nSourceSignature = 123u;
		key.m_nStoredBytes = 16u;
		key.m_nDecodedBytes = 16u;
		key.m_nChecksum = 1u;
		Ref< FileAccess > noIndividualFile;
		PackedByteArray	  read;
		uint64_t		  readUs = 0u, verifyUs = 0u;
		T3D_CHECK( test, !cache.readStored( noIndividualFile, key, read, readUs, verifyUs ) );
	} );
}
} // namespace terrain3d_tests
