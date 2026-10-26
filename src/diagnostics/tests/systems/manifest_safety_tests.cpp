// Regression cases for I-001/I-007: bounds must be established before
// allocating or publishing any manifest-derived region/dependency tables.
#include "diagnostics/tests/systems/test_harness.h"
#include "data/runtime/terrain_3d_runtime_world_manifest.h"
#include "Terrain3DConstants.h"
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>

namespace terrain3d_tests
{
static void writeU32At( PackedByteArray &bytes, uint32_t offset, uint32_t value ) {
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		bytes.set( ( int64_t ) offset + i, ( uint8_t ) ( value >> ( 8u * i ) ) );
	}
}
static bool writeInvalidManifest( const String &directory, uint32_t regionDependencyCount,
	uint32_t regionFirst, uint16_t regionDependencySpan, bool writeTables ) {
	const String path = directory.path_join( "terrain3d_world.t3dmanifest" );
	Ref< FileAccess > file = FileAccess::open( path, FileAccess::WRITE );
	if ( file.is_null() ) {
		return false;
	}
	file->store_64( T3D_RUNTIME_WORLD_MANIFEST_MAGIC );
	file->store_32( T3D_RUNTIME_WORLD_MANIFEST_VERSION );
	file->store_32( T3D_MANIFEST_HEADER_BYTES );
	file->store_32( T3D_REGION_GEOMETRY_SIGNATURE );
	file->store_32( 0u );
	file->store_32( 1u ); // one valid region
	file->store_32( 1u ); // one scene dependency
	file->store_32( regionDependencyCount );
	file->store_32( T3D_MANIFEST_REGION_BASE_BYTES + T3D_MAX_MESH_ASSETS * 4u );
	file->store_64( 1u );
	file->store_64( 1u );
	file->store_float( s_fRegionVertexSpacing );
	file->store_float( 0.0f );
	file->store_float( 1.0f );
	for ( int i = 0; i < 4; ++i ) {
		file->store_32( 0u );
	}
	file->store_32( T3D_MANIFEST_TOPOLOGY_BYTES );
	file->store_32( 0u );
	while ( file->get_position() < T3D_MANIFEST_HEADER_BYTES ) {
		file->store_8( 0u );
	}
	if ( writeTables ) {
		for ( uint32_t i = 0u; i < T3D_MANIFEST_TOPOLOGY_BYTES; ++i ) {
			file->store_8( 0u );
		}
		PackedByteArray record;
		record.resize( T3D_MANIFEST_REGION_BASE_BYTES + T3D_MAX_MESH_ASSETS * 4u );
		record.fill( 0u );
		writeU32At( record, 20u, 1u ); // material mask
		writeU32At( record, 24u, 1u ); // page signature
		writeU32At( record, 56u, regionFirst );
		record.set( 60u, ( uint8_t ) regionDependencySpan );
		record.set( 61u, ( uint8_t ) ( regionDependencySpan >> 8u ) );
		file->store_buffer( record );
		if ( regionDependencyCount <= 4u ) {
			for ( uint32_t i = 0u; i < regionDependencyCount; ++i ) {
				file->store_16( 0u );
			}
		}
		file->store_32( 0u ); // empty dependency string
	}
	file->flush();
	return file->get_error() == OK;
}

void register_manifest_safety_tests( TestRunner &runner ) {
	runner.run( "manifest", "reject_unbounded_table_before_allocation", []( TestContext &test ) {
		const String directory = "user://terrain3d_manifest_safety_test";
		T3D_REQUIRE( test, DirAccess::make_dir_recursive_absolute( directory ) == OK );
		T3D_REQUIRE( test, writeInvalidManifest( directory, UINT32_MAX, 0u, 0u, false ) );
		Terrain3DRuntimeWorldManifest manifest;
		T3D_CHECK( test, !manifest.load( directory ) );
		T3D_CHECK( test, !manifest.isLoaded() && manifest.getRegionCount() == 0 );
		T3D_REQUIRE( test, writeInvalidManifest( directory, 1u, UINT32_MAX, 2u, true ) );
		T3D_CHECK( test, !manifest.load( directory ) );
		T3D_CHECK( test, !manifest.isLoaded() && manifest.getRegionCount() == 0 );
		T3D_REQUIRE( test, writeInvalidManifest( directory, 1u, 0u, 0u, false ) );
		T3D_CHECK( test, !manifest.load( directory ) );
		T3D_CHECK( test, !manifest.isLoaded() && manifest.getRegionCount() == 0 );
		( void ) DirAccess::remove_absolute( directory.path_join( "terrain3d_world.t3dmanifest" ) );
		( void ) DirAccess::remove_absolute( directory );
	} );
}
} // namespace terrain3d_tests
