#include "diagnostics/tests/systems/test_harness.h"
#include "rendering/rd/vt/streaming/terrain_3d_vt_spatial_demand.h"
#include "rendering/rd/vt/streaming/terrain_3d_vt_demand_bundle_store.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <thread>
#include <chrono>
#include "assets/catalog/terrain_3d_assets.h"
#include <string.h>
#include <vector>

namespace terrain3d_tests
{
static std::vector< uint8_t > make_demand_fixture() {
	t3dVTDemandCell_t cells[2]{};
	cells[0].nX = -1;
	cells[0].nZ = 2;
	cells[0].nCount = 1;
	cells[0].nFirstRecord = 0;
	cells[1].nX = 3;
	cells[1].nZ = 4;
	cells[1].nCount = 1;
	cells[1].nFirstRecord = 1;
	t3dVTDemandRecord_t records[2]{};
	records[0].nAllocation = 7;
	records[0].nMip = 2;
	records[0].nCoverage = 32000;
	records[0].nUvMaxX = 65535;
	records[0].nUvMaxY = 65535;
	records[0].nViewMask = 255;
	records[1] = records[0];
	records[1].nAllocation = 8;
	t3dVTDemandHeader_t header{};
	header.nMagic = T3D_VT_DEMAND_MAGIC;
	header.nVersion = T3D_VT_DEMAND_VERSION;
	header.nCellMeters = 128;
	header.nCellCount = 2;
	header.nRecordCount = 2;
	header.nPackageSignature = 0x123456789u;
	std::vector< uint8_t > data( sizeof( header ) + sizeof( cells ) + sizeof( records ) );
	memcpy( data.data() + sizeof( header ), cells, sizeof( cells ) );
	memcpy( data.data() + sizeof( header ) + sizeof( cells ), records, sizeof( records ) );
	header.nPayloadHash = t3d_vt_demand_hash( data.data() + sizeof( header ), data.size() - sizeof( header ) );
	memcpy( data.data(), &header, sizeof( header ) );
	return data;
}
void register_vt_spatial_demand_tests(
	TestRunner &runner ) {
	runner.run( "vt_spatial_demand", "per_asset_sidecar_paths", []( TestContext &test ) {
		String first, second, moved, error;
		T3D_REQUIRE(
			test,
			Terrain3DAssets::resolveVtDemandIndexPathFromResourcePathCold( "res://World/Assets/TerrainMaterials.tres",
																		   first,
																		   &error ) );
		T3D_REQUIRE(
			test,
			Terrain3DAssets::resolveVtDemandIndexPathFromResourcePathCold( "res://World/Assets/TerrainMaterials2.res",
																		   second,
																		   &error ) );
		T3D_CHECK( test, first == "res://World/Assets/TerrainMaterials.vt_demand.t3vd" );
		T3D_CHECK( test, second == "res://World/Assets/TerrainMaterials2.vt_demand.t3vd" );
		T3D_CHECK( test, first != second );
		T3D_REQUIRE(
			test,
			Terrain3DAssets::resolveVtDemandIndexPathFromResourcePathCold( "res://Reorganized/TerrainMaterials.tres",
																		   moved,
																		   &error ) );
		T3D_CHECK( test, moved == "res://Reorganized/TerrainMaterials.vt_demand.t3vd" );
		T3D_CHECK( test, !Terrain3DAssets::resolveVtDemandIndexPathFromResourcePathCold( "", moved, &error ) );
		T3D_CHECK( test, moved.is_empty() && !error.is_empty() );
		T3D_CHECK( test,
				   !Terrain3DAssets::resolveVtDemandIndexPathFromResourcePathCold( "res://World/Embedded.tscn::123",
																				   moved,
																				   &error ) );
		T3D_CHECK( test, moved.is_empty() && !error.is_empty() );
	} );
	runner.run( "vt_spatial_demand", "v2_stable_key_roundtrip_and_package_binding", []( TestContext &test ) {
		t3dVTDemandCell_t cells[2]{};
		cells[0].nX = -2;
		cells[0].nZ = 4;
		cells[0].nFirstRecord = 0;
		cells[0].nCount = 2;
		cells[1].nX = 3;
		cells[1].nZ = 0;
		cells[1].nFirstRecord = 2;
		cells[1].nCount = 1;
		t3dVTDemandRecordV2_t records[3]{};
		records[0].nStableMaterialKey = 100u;
		records[1].nStableMaterialKey = 200u;
		records[2].nStableMaterialKey = 300u;
		for ( auto &record : records ) {
			record.Footprint.nCoverage = 32768u;
			record.Footprint.nViewMask = 255u;
			record.Footprint.nUvMaxX = record.Footprint.nUvMaxY = 65535u;
		}
		records[1].nFlags = 1u;
		std::vector< uint8_t > bytes, again;
		T3D_REQUIRE( test, t3d_vt_demand_write_v2( cells, 2u, records, 3u, 777u, 888u, bytes ) );
		T3D_REQUIRE( test, t3d_vt_demand_write_v2( cells, 2u, records, 3u, 777u, 888u, again ) );
		T3D_CHECK( test, bytes == again );
		Terrain3DVTSpatialDemand index;
		T3D_REQUIRE( test, index.loadCold( bytes.data(), bytes.size() ) );
		T3D_CHECK( test, index.getVersion() == T3D_VT_DEMAND_VERSION_V2 );
		T3D_CHECK( test, index.getAssetIdentityHash() == 888u );
		T3D_CHECK( test, index.isValidFor( 777u ) && !index.isValidFor( 778u ) );
		T3D_CHECK( test, !index.hasBoundPredictor() );
		const uint64_t keys[2] = { 100u, 200u };
		const uint32_t alloc[2] = { 24u, 27u };
		T3D_REQUIRE( test, index.bindStableKeysCold( keys, alloc, 2u ) );
		T3D_CHECK( test, index.getMappedRecordCount() == 2u );
		uint32_t				   count = 0u;
		const t3dVTDemandRecord_t *r = index.findCellHot( -2, 4, count );
		T3D_REQUIRE( test, r != nullptr && count == 2u );
		T3D_CHECK( test, r[0].nAllocation == 24u && r[0].nReserved1 == 0u );
		T3D_CHECK( test, r[1].nAllocation == 27u && r[1].nReserved1 == 1u );
		r = index.findCellHot( 3, 0, count );
		T3D_REQUIRE( test, r != nullptr && count == 1u );
		T3D_CHECK( test, r[0].nAllocation == UINT32_MAX );
		bytes.back() ^= 1u;
		T3D_CHECK( test, !index.loadCold( bytes.data(), bytes.size() ) );
	} );
	runner.run( "vt_spatial_demand", "v2_cooker_rejects_noncanonical_cells", []( TestContext &test ) {
		t3dVTDemandCell_t cells[2]{};
		cells[0].nX = 0;
		cells[0].nZ = 0;
		cells[0].nCount = 1;
		cells[1].nX = 0;
		cells[1].nZ = 0;
		cells[1].nCount = 1;
		cells[1].nFirstRecord = 1;
		t3dVTDemandRecordV2_t records[2]{};
		for ( auto &r : records ) {
			r.nStableMaterialKey = 50;
			r.Footprint.nCoverage = 100;
			r.Footprint.nViewMask = 255;
		}
		std::vector< uint8_t > bytes;
		T3D_CHECK( test, !t3d_vt_demand_write_v2( cells, 2u, records, 2u, 111u, 222u, bytes ) );
		T3D_CHECK( test, bytes.empty() );
	} );
	runner.run( "vt_spatial_demand", "offline_index_load_and_mru_lookup", []( TestContext &test ) {
		const auto				 data = make_demand_fixture();
		Terrain3DVTSpatialDemand index;
		T3D_REQUIRE( test, index.loadCold( data.data(), data.size() ) );
		T3D_CHECK( test, index.isValidFor( 0x123456789u ) );
		T3D_CHECK( test, !index.isValidFor( 0x123456788u ) );
		uint32_t				   count = 0;
		const t3dVTDemandRecord_t *r = index.findCellHot( -1, 2, count );
		T3D_REQUIRE( test, r != nullptr && count == 1 && r[0].nAllocation == 7 );
		r = index.findCellHot( 3, 4, count );
		T3D_REQUIRE( test, r != nullptr && count == 1 && r[0].nMip == 2 );
		T3D_CHECK( test, index.findCellHot( 9, 9, count ) == nullptr && count == 0 );
	} );
	runner.run( "vt_spatial_demand", "reject_corrupt_and_noncanonical_index", []( TestContext &test ) {
		Terrain3DVTSpatialDemand index;
		auto					 bytes = make_demand_fixture();
		bytes.back() ^= 1;
		T3D_CHECK( test, !index.loadCold( bytes.data(), bytes.size() ) );
		bytes = make_demand_fixture();
		auto *header = reinterpret_cast< t3dVTDemandHeader_t * >( bytes.data() );
		header->nPackageSignature = 0;
		T3D_CHECK( test, !index.loadCold( bytes.data(), bytes.size() ) );
		bytes = make_demand_fixture();
		auto *cells = reinterpret_cast< t3dVTDemandCell_t * >( bytes.data() + sizeof( t3dVTDemandHeader_t ) );
		cells[1].nX = cells[0].nX;
		cells[1].nZ = cells[0].nZ;
		header = reinterpret_cast< t3dVTDemandHeader_t * >( bytes.data() );
		header->nPayloadHash = t3d_vt_demand_hash( bytes.data() + sizeof( *header ), bytes.size() - sizeof( *header ) );
		T3D_CHECK( test, !index.loadCold( bytes.data(), bytes.size() ) );
	} );
    runner.run( "vt_spatial_demand", "v3_async_bundles_and_reopen", []( TestContext &test ) {
        t3dVTDemandV3CookConfig_t cfg{};
        cfg.m_nPackageSignature = 7721u;
        cfg.m_nAssetIdentityHash = 7722u;
        cfg.m_nSourceSignature = 7723u;
        cfg.m_nBakeConfigSignature = 7724u;
        t3dVTDemandV3Cell_t cells[2]{};
        cells[0].m_nX = -5; cells[0].m_nZ = -1;
        cells[0].m_nFirstRecord = 0u; cells[0].m_nRecordCount = 1u;
        cells[1].m_nX = 5; cells[1].m_nZ = 2;
        cells[1].m_nFirstRecord = 1u; cells[1].m_nRecordCount = 1u;
        t3dVTDemandV3Record_t records[2]{};
        for ( uint32_t i = 0u; i < 2u; ++i ) {
            records[i].m_nStableMaterialKey = 100u + i;
            records[i].m_nSourceRevision = 1u;
            records[i].m_nProbeCount = 6u;
            records[i].m_nSampleCount = 20u;
            records[i].m_nCoverageSampleCount = 20u;
            records[i].m_nCoverageConfidence = 65535u;
            records[i].m_szMipWeight[5] = 20u;
        }
        std::vector< uint8_t > bytes;
        T3D_REQUIRE( test, t3d_vt_demand_write_v3( cells, 2u, records, 2u, cfg, bytes ) );
        const String path = "user://t3d_w04_async_vt_test.t3vd";
        {
            Ref<godot::FileAccess> file = godot::FileAccess::open( path, godot::FileAccess::WRITE );
            T3D_REQUIRE( test, file.is_valid() );
            godot::PackedByteArray data;
            data.resize( ( int64_t ) bytes.size() );
            memcpy( data.ptrw(), bytes.data(), bytes.size() );
            file->store_buffer( data );
            file->flush();
        }
        Terrain3DVTDemandBundleStore store;
        T3D_CHECK( test, !store.openCold( path, 7721u, 7725u ) );
        T3D_REQUIRE( test, store.openCold( path, 7721u, 7722u ) );
        T3D_CHECK( test, store.validFor( 7721u ) && !store.validFor( 7724u ) );
        uint32_t count = 0u, flags = 0u;
        T3D_CHECK( test, store.findCellHot( -5, -1, count, flags ) == nullptr );
        store.requestCellHot( -5, -1, 255u );
        store.requestCellHot( 5, 2, 128u );
        for ( uint32_t attempt = 0u; attempt < 200u; ++attempt ) {
            if ( store.stats().nResident == 2u ) break;
            std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
        }
        const auto *r = store.findCellHot( -5, -1, count, flags );
        T3D_CHECK( test, r != nullptr && count == 1u && r[0].m_nStableMaterialKey == 100u );
        r = store.findCellHot( 5, 2, count, flags );
        T3D_CHECK( test, r != nullptr && count == 1u && r[0].m_nStableMaterialKey == 101u );
        store.resetCold();
        T3D_CHECK( test, !store.validFor( 7721u ) );
        ( void ) godot::DirAccess::remove_absolute( path );
    } );
	runner.run( "vt_spatial_demand", "v3_histogram_index_roundtrip_and_v2_isolation", []( TestContext &test ) {
		t3dVTDemandV3CookConfig_t config;
		config.m_nPackageSignature = 1001u;
		config.m_nAssetIdentityHash = 1002u;
		config.m_nSourceSignature = 1003u;
		config.m_nBakeConfigSignature = 1004u;
		t3dVTDemandV3Cell_t cells[3]{};
		cells[0].m_nX = -5; cells[0].m_nZ = -1;
		cells[1].m_nX = -4; cells[1].m_nZ = -1;
		cells[2].m_nX = 1; cells[2].m_nZ = 2;
		for ( uint32_t i = 0u; i < 3u; ++i ) {
			cells[i].m_nFirstRecord = i;
			cells[i].m_nRecordCount = 1u;
		}
		cells[1].m_nFlags = 1u; // explicitly incomplete static coverage
		t3dVTDemandV3Record_t records[3]{};
		for ( uint32_t i = 0u; i < 3u; ++i ) {
			records[i].m_nStableMaterialKey = i + 11u;
			records[i].m_nSourceRevision = 1u;
			records[i].m_nProbeCount = 6u;
			records[i].m_nViewBin = ( uint8_t ) i;
		}
		records[0].m_nSampleCount = 20u;
		records[0].m_nCoverageSampleCount = 12u;
		records[0].m_nCoverageConfidence = 45000u;
		records[0].m_szMipWeight[3] = 12u;
		records[0].m_szMipWeight[4] = 8u;
		records[1].m_nFlags = T3D_VT_VISIBILITY_NO_VISIBLE | T3D_VT_VISIBILITY_PARTIAL;
		records[2] = records[0];
		records[2].m_nStableMaterialKey = 13u;
		records[2].m_nViewBin = 2u;
		std::vector< uint8_t > bytes, again;
		T3D_REQUIRE( test, t3d_vt_demand_write_v3( cells, 3u, records, 3u, config, bytes ) );
		T3D_REQUIRE( test, t3d_vt_demand_write_v3( cells, 3u, records, 3u, config, again ) );
		T3D_CHECK( test, bytes == again );
		std::vector< t3dVTDemandV3Cell_t > decodedCells;
		std::vector< t3dVTDemandV3Record_t > decodedRecords;
		t3dVTDemandV3CookConfig_t decodedConfig;
		T3D_REQUIRE( test, t3d_vt_demand_read_v3( bytes.data(), bytes.size(), 1001u, 1003u,
			decodedCells, decodedRecords, &decodedConfig ) );
		T3D_REQUIRE( test, decodedCells.size() == 3u && decodedRecords.size() == 3u );
		T3D_CHECK( test, decodedCells[0].m_nX == -5 && decodedCells[1].m_nX == -4 );
		T3D_CHECK( test, decodedConfig.m_nAssetIdentityHash == 1002u );
		T3D_CHECK( test, decodedRecords[0].m_szMipWeight[3] == 12u );
		T3D_CHECK( test, decodedRecords[1].m_nFlags == ( T3D_VT_VISIBILITY_NO_VISIBLE | T3D_VT_VISIBILITY_PARTIAL ) );
		Terrain3DVTSpatialDemand production;
		T3D_CHECK( test, !production.loadCold( bytes.data(), bytes.size() ) ); // v3 not enabled in T01
		T3D_CHECK( test, !t3d_vt_demand_read_v3( bytes.data(), bytes.size(), 1002u, 1003u,
			decodedCells, decodedRecords ) );
		T3D_CHECK( test, !t3d_vt_demand_read_v3( bytes.data(), bytes.size(), 1001u, 1004u,
			decodedCells, decodedRecords ) );
		T3D_CHECK( test, decodedCells.size() == 3u && decodedRecords.size() == 3u ); // failure is atomic
		for ( uint64_t limit : { ( uint64_t ) 0u, ( uint64_t ) 16u, ( uint64_t ) 135u, ( uint64_t ) bytes.size() - 1u } ) {
			T3D_CHECK( test, !t3d_vt_demand_read_v3( bytes.data(), limit, 1001u, 1003u,
				decodedCells, decodedRecords ) );
		}
		for ( size_t offset : { ( size_t ) 8u, ( size_t ) 96u, ( size_t ) 144u, bytes.size() - 1u } ) {
			std::vector< uint8_t > corrupt = bytes;
			corrupt[offset] ^= 0x10u;
			T3D_CHECK( test, !t3d_vt_demand_read_v3( corrupt.data(), corrupt.size(), 1001u, 1003u,
				decodedCells, decodedRecords ) );
		}
		cells[1].m_nX = -5; cells[1].m_nZ = -1;
		T3D_CHECK( test, !t3d_vt_demand_write_v3( cells, 3u, records, 3u, config, again ) );
	} );

}
} // namespace terrain3d_tests
