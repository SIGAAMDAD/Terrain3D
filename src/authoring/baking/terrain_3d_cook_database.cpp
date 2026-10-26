// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "authoring/baking/terrain_3d_cook_database.h"

#include "data/persistence/terrain_3d_region_repository.h"
#include "data/persistence/terrain_3d_file_publication.h"
#include "data/runtime/terrain_3d_runtime_page_constants.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>


void Terrain3DCookDatabase::reset( void ) {
	m_bLoaded = false;
	memset( m_regionSignatures, 0, sizeof( m_regionSignatures ) );
}

String Terrain3DCookDatabase::getPath( const String &rDataDirectory ) const {
	return rDataDirectory.path_join( "terrain3d_cook.t3ddb" );
}

bool Terrain3DCookDatabase::load( const String &rDataDirectory, const uint64_t nMaterialSignature ) {
	reset();
	const String path = getPath( rDataDirectory );
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::READ );
	if ( file.is_null() ) {
		return false;
	}
	const uint64_t expected_bytes = T3D_COOK_DATABASE_HEADER_BYTES + ( uint64_t )T3D_REGION_SLOTS * sizeof( uint64_t );
	if ( ( uint64_t )file->get_length() != expected_bytes || file->get_64() != T3D_COOK_DATABASE_MAGIC
	        || file->get_32() != T3D_COOK_DATABASE_VERSION || file->get_32() != T3D_RUNTIME_PAGE_VERSION
	        || file->get_32() != T3D_REGION_GEOMETRY_SIGNATURE ) {
		return false;
	}
	file->get_32(); // reserved
	if ( file->get_64() != nMaterialSignature ) {
		return false;
	}
	file->get_64(); // reserved for future world/artifact signature
	for ( size_t iRegion = 0u; iRegion < T3D_ARRAY_COUNT( m_regionSignatures ); iRegion++ ) {
		uint64_t &signature = ( m_regionSignatures )[iRegion];
		signature = file->get_64();
	}
	if ( file->get_error() != OK ) {
		reset();
		return false;
	}
	m_bLoaded = true;
	return true;
}

bool Terrain3DCookDatabase::write( const String &rDataDirectory, const uint64_t nMaterialSignature ) const {
	if ( rDataDirectory.is_empty() ) {
		return false;
	}
	const String path = getPath( rDataDirectory );
	const String temporary = path + String( ".tmp" );
	Terrain3DFilePublication publication( path, temporary, path + String( ".bak" ) );
	if ( publication.prepare() != OK ) {
		return false;
	}
	Ref<FileAccess> file = FileAccess::open( temporary, FileAccess::WRITE );
	if ( file.is_null() ) {
		return false;
	}
	file->store_64( T3D_COOK_DATABASE_MAGIC );
	file->store_32( T3D_COOK_DATABASE_VERSION );
	file->store_32( T3D_RUNTIME_PAGE_VERSION );
	file->store_32( T3D_REGION_GEOMETRY_SIGNATURE );
	file->store_32( 0u );
	file->store_64( nMaterialSignature );
	file->store_64( 0u );
	for ( size_t iRegion = 0u; iRegion < T3D_ARRAY_COUNT( m_regionSignatures ); iRegion++ ) {
		const uint64_t signature = ( m_regionSignatures )[iRegion];
		file->store_64( signature );
	}
	const Error error = file->get_error();
	file->close();
	if ( error != OK ) {
		DirAccess::remove_absolute( temporary );
		return false;
	}
	return publication.publish() == OK && publication.commit() == OK;
}

bool Terrain3DCookDatabase::hasCurrentRegion( const Vector2i &rLocation, const uint64_t nSourceSignature ) const {
	if ( !m_bLoaded || nSourceSignature == 0u ) {
		return false;
	}
	const int index = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
	return index >= 0 && m_regionSignatures[( size_t )index] == nSourceSignature;
}

void Terrain3DCookDatabase::setRegion( const Vector2i &rLocation, const uint64_t nSourceSignature ) {
	const int index = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
	if ( index >= 0 ) {
		m_regionSignatures[( size_t )index] = nSourceSignature;
	}
}

void Terrain3DCookDatabase::clearRegion( const Vector2i &rLocation ) {
	setRegion( rLocation, 0u );
}
