// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/terrain_3d_cook_database.h"

#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_runtime_page_constants.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>

static constexpr uint64_t s_nCookDatabaseMagic = 0x3142444b43443354ull; // T3DCKDB1
static constexpr uint32_t s_nCookDatabaseVersion = 1u;
static constexpr uint32_t s_nCookDatabaseHeaderBytes = 40u;

void T3DCookDatabase::reset( void ) {
	m_bLoaded = false;
	m_regionSignatures.fill( 0u );
}

String T3DCookDatabase::getPath( const String &rDataDirectory ) const {
	return rDataDirectory.path_join( "terrain3d_cook.t3ddb" );
}

bool T3DCookDatabase::load( const String &rDataDirectory, const int nRegionSize, const uint64_t nMaterialSignature ) {
	reset();
	const String path = getPath( rDataDirectory );
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::READ );
	if ( file.is_null() ) {
		return false;
	}
	const uint64_t expected_bytes = s_nCookDatabaseHeaderBytes + ( uint64_t )s_nMaxRegionSlots * sizeof( uint64_t );
	if ( ( uint64_t )file->get_length() != expected_bytes || file->get_64() != s_nCookDatabaseMagic ||
			file->get_32() != s_nCookDatabaseVersion || file->get_32() != s_nRuntimePageVersion ||
			( int )file->get_32() != nRegionSize ) {
		return false;
	}
	file->get_32(); // reserved
	if ( file->get_64() != nMaterialSignature ) {
		return false;
	}
	file->get_64(); // reserved for future world/artifact signature
	for ( uint64_t &signature : m_regionSignatures ) {
		signature = file->get_64();
	}
	if ( file->get_error() != OK ) {
		reset();
		return false;
	}
	m_bLoaded = true;
	return true;
}

bool T3DCookDatabase::write( const String &rDataDirectory,
		const int nRegionSize,
		const uint64_t nMaterialSignature ) const {
	if ( rDataDirectory.is_empty() || nRegionSize <= 0 ) {
		return false;
	}
	const String path = getPath( rDataDirectory );
	const String temporary = path + ".tmp";
	Ref<FileAccess> file = FileAccess::open( temporary, FileAccess::WRITE );
	if ( file.is_null() ) {
		return false;
	}
	file->store_64( s_nCookDatabaseMagic );
	file->store_32( s_nCookDatabaseVersion );
	file->store_32( s_nRuntimePageVersion );
	file->store_32( ( uint32_t )nRegionSize );
	file->store_32( 0u );
	file->store_64( nMaterialSignature );
	file->store_64( 0u );
	for ( const uint64_t signature : m_regionSignatures ) {
		file->store_64( signature );
	}
	const Error error = file->get_error();
	file->close();
	if ( error != OK ) {
		DirAccess::remove_absolute( temporary );
		return false;
	}
	const String backup = path + ".bak";
	DirAccess::remove_absolute( backup );
	if ( FileAccess::file_exists( path ) && DirAccess::rename_absolute( path, backup ) != OK ) {
		DirAccess::remove_absolute( temporary );
		return false;
	}
	if ( DirAccess::rename_absolute( temporary, path ) != OK ) {
		if ( FileAccess::file_exists( backup ) ) {
			DirAccess::rename_absolute( backup, path );
		}
		return false;
	}
	DirAccess::remove_absolute( backup );
	return true;
}

bool T3DCookDatabase::hasCurrentRegion( const Vector2i &rLocation, const uint64_t nSourceSignature ) const {
	if ( !m_bLoaded || nSourceSignature == 0u ) {
		return false;
	}
	const int index = T3DRegionRepository::getRegionMapIndex( rLocation );
	return index >= 0 && m_regionSignatures[( size_t )index] == nSourceSignature;
}

void T3DCookDatabase::setRegion( const Vector2i &rLocation, const uint64_t nSourceSignature ) {
	const int index = T3DRegionRepository::getRegionMapIndex( rLocation );
	if ( index >= 0 ) {
		m_regionSignatures[( size_t )index] = nSourceSignature;
	}
}

void T3DCookDatabase::clearRegion( const Vector2i &rLocation ) {
	setRegion( rLocation, 0u );
}
