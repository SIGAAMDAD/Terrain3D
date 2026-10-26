#include "diagnostics/tests/systems/test_harness.h"
#include "authoring/vt/build/terrain_3d_vt_demand_cooker.h"
#include "authoring/vt/visibility/terrain_3d_vt_visibility_aggregator.h"
#include "rendering/rd/vt/streaming/terrain_3d_vt_spatial_demand.h"
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <string.h>

namespace terrain3d_tests {
void register_vt_visibility_aggregation_tests( TestRunner &runner ) {
    runner.run( "vt_visibility_aggregate", "t03_max_reduction_partial_truncation_and_compressed_codec", []( TestContext &test ) {
        const uint64_t keys[] = { 10u, 20u, 30u };
        t3dVTVisibilityProbeResult_t probes[3]{};
        for ( uint32_t i = 0u; i < 3u; ++i ) {
            auto &r = probes[i];
            r.m_nCellX = i < 2 ? -1 : 4;
            r.m_nCellZ = i < 2 ? -5 : 0;
            r.m_nSourceRevision = 3u;
            r.m_nMaterialCount = 3u;
            r.m_nNominalTextureSize = 4096u;
            r.m_nNominalRenderSize = 128u;
            r.m_counts.resize( 3u * 8u * 16u );
            r.m_counts[( ( 0u * 8u + 0u ) * 16u + 1u )] = 10u + i;
            r.m_counts[( ( 1u * 8u + 0u ) * 16u + 2u )] = 20u;
            r.m_counts[( ( 2u * 8u + 0u ) * 16u + 3u )] = 30u;
            r.m_bHasVisibleSamples = true;
        }
        probes[1].m_counts[1u] = 90u;
        std::vector< t3dVTDemandV1Cell_t > cells;
        std::vector< t3dVTDemandV1Record_t > records;
        t3dVTVisibilityAggregateStats_t stats;
        T3D_REQUIRE( test, Terrain3DVTVisibilityAggregator::reduce(
            probes, 3u, keys, 3u, 2u, false, cells, records, stats ) );
        T3D_CHECK( test, cells.size() == 2u && records.size() == 4u &&
            stats.m_nDroppedRecords == 2u && stats.m_nPartialCells == 2u );
        T3D_CHECK( test, cells[0].m_nX == -1 && cells[0].m_nZ == -5 &&
            records[0].m_nSourceRevision == 3u && records[0].m_nProbeCount == 2u );
        t3dVTDemandV1CookConfig_t config;
        config.m_nPackageSignature = 911u;
        config.m_nAssetIdentityHash = 912u;
        config.m_nSourceSignature = 913u;
        config.m_nBakeConfigSignature = 914u;
        std::vector< uint8_t > bytes, repeated;
        T3D_REQUIRE( test, t3d_vt_demand_write_v1( cells.data(), ( uint32_t ) cells.size(),
            records.data(), ( uint32_t ) records.size(), config, bytes ) );
        T3D_REQUIRE( test, t3d_vt_demand_write_v1( cells.data(), ( uint32_t ) cells.size(),
            records.data(), ( uint32_t ) records.size(), config, repeated ) );
        T3D_CHECK( test, bytes == repeated );
        std::vector< t3dVTDemandV1Cell_t > outputCells;
        std::vector< t3dVTDemandV1Record_t > outputRecords;
        T3D_CHECK( test, t3d_vt_demand_read_v1( bytes.data(), bytes.size(), 911u, 913u,
            outputCells, outputRecords ) && outputRecords.size() == 4u );
        T3D_CHECK( test, !t3d_vt_demand_read_v1( bytes.data(), bytes.size(), 912u, 913u,
            outputCells, outputRecords ) );
        T3D_CHECK( test, !t3d_vt_demand_read_v1( bytes.data(), bytes.size(), 911u, 914u,
            outputCells, outputRecords ) );
        const size_t originalCount = outputRecords.size();
        bytes.back() ^= 0x40u;
        T3D_CHECK( test, !t3d_vt_demand_read_v1( bytes.data(), bytes.size(), 911u, 913u,
            outputCells, outputRecords ) && outputRecords.size() == originalCount );
    } );
    runner.run( "vt_visibility_aggregate", "t03_package_v2_v1_transaction_rollback", []( TestContext &test ) {
        const String dir = "user://terrain3d_t03_publish";
        T3D_REQUIRE( test, DirAccess::make_dir_recursive_absolute( dir ) == OK );
        const String pack = dir.path_join( "pack.t3dvt" );
        const String v2 = dir.path_join( "presence.t3vd" );
        const String v1 = dir.path_join( "presence.t3vd.v1" );
        PackedByteArray old, newer;
        old.resize( 32 ); newer.resize( 32 );
        for ( int i = 0; i < 32; ++i ) { old.set( i, ( uint8_t ) i ); newer.set( i, ( uint8_t ) ( i + 17 ) ); }
        String error;
        T3D_REQUIRE( test, t3d_vt_demand_publish_triplet_cold( pack, old, v2, old,
            v1, old, error ) );
        T3D_CHECK( test, FileAccess::get_file_as_bytes( pack ) == old &&
            FileAccess::get_file_as_bytes( v2 ) == old && FileAccess::get_file_as_bytes( v1 ) == old );
        const String badV1 = dir.path_join( "missing_dir/failed.t3vd" );
        T3D_CHECK( test, !t3d_vt_demand_publish_triplet_cold( pack, newer, v2, newer,
            badV1, newer, error ) );
        T3D_CHECK( test, FileAccess::get_file_as_bytes( pack ) == old &&
            FileAccess::get_file_as_bytes( v2 ) == old );
        T3D_REQUIRE( test, t3d_vt_demand_publish_triplet_cold( pack, newer, v2, newer,
            v1, newer, error ) );
        T3D_CHECK( test, FileAccess::get_file_as_bytes( pack ) == newer &&
            FileAccess::get_file_as_bytes( v2 ) == newer && FileAccess::get_file_as_bytes( v1 ) == newer );
        ( void ) DirAccess::remove_absolute( pack );
        ( void ) DirAccess::remove_absolute( v2 );
        ( void ) DirAccess::remove_absolute( v1 );
        ( void ) DirAccess::remove_absolute( dir );
    } );
}
}
