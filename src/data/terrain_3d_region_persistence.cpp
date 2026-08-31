// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/terrain_3d_region_persistence.h"

#include "core/logger.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_region_repository.h"
#include "eastl_config.h"

#include <EASTL/bitset.h>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

void t3dStagedRepository_t::reset( void ) {
	m_regions.clear();
	m_resources.clear();
	memset( m_pRegionPtrs.data(), 0, sizeof( m_pRegionPtrs[0] ) * m_pRegionPtrs.size() );
	for ( size_t i = 0u; i < m_regionLocations.size(); ++i ) {
		m_regionLocations[i] = s_v2iMax;
	}
	memset( m_logicalRegionMap.data(), 0, sizeof( m_logicalRegionMap[0] ) * m_logicalRegionMap.size() );
	m_nTargetRegionSize = 0;
	m_bBoundedCpuLoad = false;
}

Terrain3DRegionPersistence::Terrain3DRegionPersistence( Terrain3DRegionRepository *const pRepository ) : m_pRepository( pRepository ) {
}

void Terrain3DRegionPersistence::resetLoadStats( void ) {
	m_startupLoadStats = t3dStartupLoadStats_t{};
	m_nLoadStartedUsec = Time::get_singleton()->get_ticks_usec();
	m_nPublishStartedUsec = 0u;
	m_nFinalizeStartedUsec = 0u;
}

bool Terrain3DRegionPersistence::validateHeightPayload( const Ref<Terrain3DRegion> &rRegion, const String &rPath ) {
	if ( rRegion.is_null() ) {
		return false;
	}
	const Ref<Image> pHeight = rRegion->getHeightMap();
	if ( pHeight.is_null() ) {
		return true;
	}
	const Image::Format format = pHeight->get_format();
	if ( format != Image::FORMAT_R16 && format != Image::FORMAT_RF ) {
		return true;
	}
	if ( t3dRegionCodecHasValidHeightPayload( pHeight, rRegion->getRegionSize() ) ) {
		return true;
	}
	LOG( ERROR, "Malformed height payload in file: ", rPath, ". Format: ", format, ", bytes: ", pHeight->get_data().size() );
	return false;
}

void Terrain3DRegionPersistence::normalizeLoadedRegion(
        const Vector2i &rLocation, const Ref<Terrain3DRegion> &rRegion, const bool bApplyRepositorySpacing, bool &rStorageUpgrade ) const {
	const Ref<Image> pLoadedHeight = rRegion->getHeightMap();
	rStorageUpgrade = rRegion->getVersion() < s_fDataCurrentVersion || pLoadedHeight.is_null()
	        || pLoadedHeight->get_format() != Image::FORMAT_R16
	        || ( rRegion->getColorMap().is_valid() && rRegion->getColorMap()->has_mipmaps() );

	// Resource property setters are mutation-oriented and may dirty freshly
	// deserialized resources. Establish the serialized state as the clean baseline
	// before applying explicit format/version normalization.
	rRegion->setModified( false );
	rRegion->markRuntimeMapsCleanInternal();
	rRegion->setLocation( rLocation );
	rRegion->setVersion( s_fDataCurrentVersion );
	rRegion->setDeleted( false );
	rRegion->setEdited( false );
	if ( bApplyRepositorySpacing && m_pRepository ) {
		rRegion->setVertexSpacing( m_pRepository->getVertexSpacing() );
	}
	rRegion->sanitizeMaps();
	rRegion->setModified( rStorageUpgrade || rRegion->isModified() );
}

Error Terrain3DRegionPersistence::stageDirectory( const String &rDirectory, const bool bBoundedCpuLoad, t3dStagedRepository_t &rStaged ) {
	rStaged.reset();
	resetLoadStats();
	if ( rDirectory.is_empty() ) {
		LOG( ERROR, "Specified directory name is blank" );
		return ERR_INVALID_PARAMETER;
	}
	if ( !m_pRepository ) {
		return ERR_UNCONFIGURED;
	}

	LOG( INFO, "Loading region files from ", rDirectory );
	PackedStringArray files = Terrain3DUtil::getFiles( rDirectory, "terrain3d*.res" );
	files.sort();
	m_startupLoadStats.m_nEnumerationUsecLast = Time::get_singleton()->get_ticks_usec() - m_nLoadStartedUsec;
	m_startupLoadStats.m_nRegionFilesLast = files.size();
	if ( files.is_empty() ) {
		LOG( INFO, "No Terrain3D region files found in: ", rDirectory );
		return ERR_FILE_NOT_FOUND;
	}

	rStaged.m_bBoundedCpuLoad = bBoundedCpuLoad;
	rStaged.m_regions.reserve( ( size_t )( files.size() ) );
	if ( files.size() > s_nMaxRegionSlots ) {
		LOG( ERROR, "Directory contains ", files.size(), " regions, exceeding the fixed region-slot capacity of ", s_nMaxRegionSlots );
		return ERR_OUT_OF_MEMORY;
	}

	for ( size_t iFile = 0u; iFile < ( size_t )( ( files ).size() ); iFile++ ) {
		const String &fname = ( files )[iFile];
		t3dStagedRegion_t entry;
		entry.m_location = Terrain3DUtil::filenameToLocation( fname );
		entry.m_path = rDirectory.path_join( fname );
		rStaged.m_regions.push_back( entry );
	}

	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( !pLoader ) {
		return ERR_UNAVAILABLE;
	}

	LOG( DEBUG, "[terrain-data] Phase 1/4: synchronous region deserialization and validation" );
	const uint64_t deserializeStartedUsec = Time::get_singleton()->get_ticks_usec();
	eastl::bitset<s_nActiveMaxRegionMapSize * s_nActiveMaxRegionMapSize> occupied;

	for ( int fileIndex = 0; fileIndex < files.size(); ++fileIndex ) {
		const String &fname = files[fileIndex];
		t3dStagedRegion_t &entry = rStaged.m_regions[( size_t )( fileIndex )];
		const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( entry.m_location );
		if ( mapIndex < 0 ) {
			LOG( ERROR, "Invalid or out-of-bounds region file name: ", fname );
			return ERR_FILE_CORRUPT;
		}
		if ( occupied.test( ( size_t )( mapIndex ) ) ) {
			LOG( ERROR, "Duplicate region location in directory: ", entry.m_location );
			return ERR_ALREADY_EXISTS;
		}
		occupied.set( ( size_t )( mapIndex ) );

		Ref<Terrain3DRegion> region = pLoader->load( entry.m_path, "Terrain3DRegion", ResourceLoader::CACHE_MODE_IGNORE );
		if ( region.is_null() ) {
			LOG( ERROR, "Cannot load region at ", entry.m_path );
			return ERR_FILE_CORRUPT;
		}
		const int regionSize = region->getRegionSize();
		if ( !t3dIsValidRegionSize( regionSize ) ) {
			LOG( ERROR, "Invalid region size ", regionSize, " in file: ", entry.m_path );
			return ERR_FILE_CORRUPT;
		}
		if ( !validateHeightPayload( region, entry.m_path ) ) {
			return ERR_FILE_CORRUPT;
		}
		if ( rStaged.m_nTargetRegionSize == 0 ) {
			rStaged.m_nTargetRegionSize = regionSize;
		} else if ( rStaged.m_nTargetRegionSize != regionSize ) {
			LOG( ERROR,
			        "Region size mismatch. First loaded: ",
			        rStaged.m_nTargetRegionSize,
			        " next: ",
			        regionSize,
			        " in file: ",
			        entry.m_path );
			return ERR_FILE_CORRUPT;
		}

		const int targetMapSize = s_nActiveWorldTexels / rStaged.m_nTargetRegionSize;
		if ( !t3dRegionLocationValid( entry.m_location, targetMapSize ) ) {
			LOG( ERROR,
			        "Region ",
			        entry.m_location,
			        " is outside the fixed ",
			        s_nActiveWorldTexels,
			        "-texel world for region size ",
			        rStaged.m_nTargetRegionSize );
			return ERR_FILE_CORRUPT;
		}

		bool storageUpgrade = false;
		normalizeLoadedRegion( entry.m_location, region, true, storageUpgrade );
		if ( bBoundedCpuLoad ) {
			const Ref<Image> pStagedHeight = region->getHeightMap();
			if ( pStagedHeight.is_null() || pStagedHeight->get_format() != Image::FORMAT_R16 ) {
				LOG( ERROR, "Unable to stage R16 height payload from: ", entry.m_path );
				return ERR_FILE_CORRUPT;
			}
			entry.m_stagedHeight = pStagedHeight->get_data();
			const size_t regionTexelCount = ( size_t )( rStaged.m_nTargetRegionSize ) * ( size_t )( rStaged.m_nTargetRegionSize );
			const size_t expectedHeightBytes = regionTexelCount * sizeof( uint16_t );
			if ( ( size_t )( entry.m_stagedHeight.size() ) < expectedHeightBytes ) {
				LOG( ERROR, "Truncated staged height payload in: ", entry.m_path );
				return ERR_FILE_CORRUPT;
			}
			const int halfMap = targetMapSize / 2;
			const int activeX = entry.m_location.x + halfMap;
			const int activeY = entry.m_location.y + halfMap;
			entry.m_nActiveSlot = activeY * targetMapSize + activeX;
			if ( !region->isModified() ) {
				region->releaseRuntimeMapsUnmodifiedInternal();
			}
		}
		entry.m_region = region;
	}

	m_startupLoadStats.m_nDeserializeUsecLast = Time::get_singleton()->get_ticks_usec() - deserializeStartedUsec;
	LOG( DEBUG,
	        "[terrain-data] Phase 1/4 complete: validated ",
	        rStaged.m_regions.size(),
	        " regions at size ",
	        rStaged.m_nTargetRegionSize );
	LOG( DEBUG, "[terrain-data] Phase 2/4: staging replacement repository tables" );
	const uint64_t stagingStartedUsec = Time::get_singleton()->get_ticks_usec();

	for ( int slot = 0; slot < ( int )( rStaged.m_regions.size() ); ++slot ) {
		t3dStagedRegion_t &entry = rStaged.m_regions[( size_t )( slot )];
		const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( entry.m_location );
		if ( mapIndex < 0 ) {
			return ERR_FILE_CORRUPT;
		}
		rStaged.m_resources[entry.m_location] = entry.m_region;
		rStaged.m_pRegionPtrs[( size_t )( mapIndex )] = entry.m_region.ptr();
		rStaged.m_regionLocations[( size_t )( slot )] = entry.m_location;
		rStaged.m_logicalRegionMap[( size_t )( mapIndex )] = ( uint16_t )( slot + 1 );
	}

	m_startupLoadStats.m_nStagingUsecLast = Time::get_singleton()->get_ticks_usec() - stagingStartedUsec;
	LOG( DEBUG, "[terrain-data] Phase 2/4 complete: repository tables staged" );
	return OK;
}

void Terrain3DRegionPersistence::beginPublication( void ) {
	m_nPublishStartedUsec = Time::get_singleton()->get_ticks_usec();
}

void Terrain3DRegionPersistence::finishPublication( const int nPublishedRegions ) {
	if ( m_nPublishStartedUsec != 0u ) {
		m_startupLoadStats.m_nPublishUsecLast = Time::get_singleton()->get_ticks_usec() - m_nPublishStartedUsec;
	}
	m_startupLoadStats.m_nRegionsPublishedLast = MAX( 0, nPublishedRegions );
}

void Terrain3DRegionPersistence::beginFinalize( void ) {
	m_nFinalizeStartedUsec = Time::get_singleton()->get_ticks_usec();
}

void Terrain3DRegionPersistence::finishDirectoryLoad( const bool bSucceeded ) {
	if ( m_nFinalizeStartedUsec != 0u ) {
		m_startupLoadStats.m_nFinalizeUsecLast = Time::get_singleton()->get_ticks_usec() - m_nFinalizeStartedUsec;
	}
	if ( m_nLoadStartedUsec != 0u ) {
		m_startupLoadStats.m_nTotalUsecLast = Time::get_singleton()->get_ticks_usec() - m_nLoadStartedUsec;
	}
	m_startupLoadStats.m_bSucceededLast = bSucceeded;
}

void Terrain3DRegionPersistence::publishStagedPaths( t3dStagedRepository_t &rStaged ) const {
	for ( size_t iRegion = 0u; iRegion < ( size_t )( ( rStaged.m_regions ).size() ); iRegion++ ) {
		t3dStagedRegion_t &entry = ( rStaged.m_regions )[iRegion];
		if ( entry.m_region.is_valid() ) {
			entry.m_region->take_over_path( entry.m_path );
		}
	}
}

t3dRegionLoadResult_t Terrain3DRegionPersistence::loadRegion( const Vector2i &rLocation, const String &rDirectory ) const {
	t3dRegionLoadResult_t result;
	result.m_location = rLocation;
	if ( !m_pRepository || rDirectory.is_empty() ) {
		result.m_error = ERR_INVALID_PARAMETER;
		return result;
	}

	result.m_path = rDirectory.path_join( Terrain3DUtil::locationToFilename( rLocation ) );
	LOG( INFO, "Loading region from location ", rLocation );
	if ( !FileAccess::file_exists( result.m_path ) ) {
		LOG( ERROR, "File ", result.m_path, " doesn't exist" );
		result.m_error = ERR_FILE_NOT_FOUND;
		return result;
	}

	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( !pLoader ) {
		result.m_error = ERR_UNAVAILABLE;
		return result;
	}
	result.m_region = pLoader->load( result.m_path, "Terrain3DRegion", ResourceLoader::CACHE_MODE_IGNORE );
	if ( result.m_region.is_null() ) {
		LOG( ERROR, "Cannot load region at ", result.m_path );
		result.m_error = ERR_FILE_CORRUPT;
		return result;
	}
	if ( !validateHeightPayload( result.m_region, result.m_path ) ) {
		result.m_region.unref();
		result.m_error = ERR_FILE_CORRUPT;
		return result;
	}

	const int regionSize = result.m_region->getRegionSize();
	if ( !t3dIsValidRegionSize( regionSize ) ) {
		LOG( ERROR, "Invalid region size ", regionSize, " in file: ", result.m_path );
		result.m_region.unref();
		result.m_error = ERR_FILE_CORRUPT;
		return result;
	}
	if ( !m_pRepository->isEmpty() && m_pRepository->getRegionSize() != regionSize ) {
		LOG( ERROR,
		        "Region size mismatch. First loaded: ",
		        m_pRepository->getRegionSize(),
		        " next: ",
		        regionSize,
		        " in file: ",
		        result.m_path );
		result.m_region.unref();
		result.m_error = ERR_INVALID_DATA;
		return result;
	}

	normalizeLoadedRegion( rLocation, result.m_region, false, result.m_bStorageUpgrade );
	result.m_error = OK;
	return result;
}

bool Terrain3DRegionPersistence::prepareRuntimeRegionSource(
        const Ref<Terrain3DRegion> &rCandidate, const int nExpectedRegionSize, Ref<Terrain3DRegion> &rSource ) const {
	rSource.unref();
	if ( rCandidate.is_null() || rCandidate->getRegionSize() != nExpectedRegionSize ) {
		return false;
	}
	const Ref<Image> pHeight = rCandidate->getHeightMap();
	if ( pHeight.is_null() || pHeight->get_format() != Image::FORMAT_R16
	        || !t3dRegionCodecHasValidHeightPayload( pHeight, nExpectedRegionSize ) ) {
		return false;
	}
	rCandidate->setModified( false );
	rCandidate->markRuntimeMapsCleanInternal();
	rCandidate->sanitizeMaps();
	rSource = rCandidate;
	return true;
}

bool Terrain3DRegionPersistence::loadRuntimeRegionSource(
        const String &rPath, const int nExpectedRegionSize, Ref<Terrain3DRegion> &rSource ) const {
	rSource.unref();
	if ( rPath.is_empty() || !FileAccess::file_exists( rPath ) ) {
		return false;
	}
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( !pLoader ) {
		return false;
	}
	const Ref<Resource> pResource = pLoader->load( rPath, "Terrain3DRegion", ResourceLoader::CACHE_MODE_IGNORE );
	const Ref<Terrain3DRegion> pLoaded = pResource;
	return prepareRuntimeRegionSource( pLoaded, nExpectedRegionSize, rSource );
}

void Terrain3DRegionPersistence::publishLoadedRegionPath( const t3dRegionLoadResult_t &rResult ) const {
	if ( rResult.succeeded() ) {
		rResult.m_region->take_over_path( rResult.m_path );
	}
}

void Terrain3DRegionPersistence::updateEditorFile( const String &rPath ) {
	if ( !IS_EDITOR || !rPath.begins_with( "res://" ) ) {
		return;
	}
	EditorInterface *const pEditor = EditorInterface::get_singleton();
	if ( !pEditor ) {
		return;
	}
	EditorFileSystem *const pFilesystem = pEditor->get_resource_filesystem();
	if ( pFilesystem && !pFilesystem->is_scanning() ) {
		pFilesystem->update_file( rPath );
	}
}

t3dRegionSaveResult_t Terrain3DRegionPersistence::saveRegion( const Vector2i &rLocation,
        const String &rDirectory,
        const bool b16Bit,
        EnsureRegionResidentFn pEnsureResident,
        void *pResidentContext ) {
	t3dRegionSaveResult_t result;
	result.m_location = rLocation;
	if ( !m_pRepository ) {
		result.m_error = ERR_UNCONFIGURED;
		return result;
	}
	const Ref<Terrain3DRegion> pRegion = m_pRepository->getRegion( rLocation );
	if ( pRegion.is_null() ) {
		LOG( ERROR, "No region found at: ", rLocation );
		result.m_error = ERR_DOES_NOT_EXIST;
		return result;
	}
	result.m_bFound = true;
	const String fname = Terrain3DUtil::locationToFilename( rLocation );
	result.m_path = rDirectory.path_join( fname );

	if ( pRegion->isDeleted() ) {
		result.m_bDeleted = true;
		LOG( DEBUG, "Removing ", rLocation, " from _regions" );
		m_pRepository->eraseRegionResource( rLocation );
		LOG( DEBUG, "File to be deleted: ", result.m_path );
		if ( FileAccess::file_exists( result.m_path ) ) {
			Ref<DirAccess> pDirectory = DirAccess::open( rDirectory );
			if ( pDirectory.is_null() ) {
				LOG( ERROR, "Cannot open directory for writing: ", rDirectory, " error: ", DirAccess::get_open_error() );
				result.m_error = DirAccess::get_open_error();
				return result;
			}
			const Error removeError = pDirectory->remove( fname );
			if ( removeError != OK ) {
				LOG( ERROR, "Could not remove file: ", fname, ", error code: ", removeError );
				result.m_error = removeError;
				return result;
			}
			LOG( INFO, "File ", result.m_path, " deleted" );
		} else {
			LOG( INFO, "File to delete ", result.m_path, " doesn't exist. (Maybe from add, undo, save)" );
		}

		updateEditorFile( result.m_path );
		result.m_bSourceChanged = true;
		result.m_error = OK;
		return result;
	}

	if ( !pRegion->isModified() ) {
		result.m_bSkipped = true;
		result.m_error = ERR_SKIP;
		return result;
	}
	if ( pEnsureResident && !pEnsureResident( pResidentContext, rLocation ) ) {
		LOG( ERROR, "Unable to page CPU terrain maps before saving region: ", rLocation );
		result.m_error = ERR_UNAVAILABLE;
		return result;
	}

	result.m_error = pRegion->save( result.m_path, b16Bit );
	if ( !( result.m_error == OK || result.m_error == ERR_SKIP ) ) {
		LOG( ERROR,
		        "Could not save file: ",
		        result.m_path,
		        ", error: ",
		        UtilityFunctions::error_string( result.m_error ),
		        " (",
		        result.m_error,
		        ")" );
		return result;
	}
	if ( result.m_error == ERR_SKIP ) {
		result.m_bSkipped = true;
		return result;
	}

	result.m_bSaved = true;
	result.m_bSourceChanged = true;
	updateEditorFile( result.m_path );
	return result;
}

t3dDirectorySaveResult_t Terrain3DRegionPersistence::saveDirectory(
        const String &rDirectory, const bool b16Bit, EnsureRegionResidentFn pEnsureResident, void *pResidentContext ) {
	t3dDirectorySaveResult_t result;
	if ( !m_pRepository ) {
		return result;
	}
	const Array locations = m_pRepository->getRegionKeys();
	result.m_regions.reserve( ( size_t )( locations.size() ) );
	for ( size_t iLocation = 0u; iLocation < ( size_t )( ( locations ).size() ); iLocation++ ) {
		const Vector2i location = ( locations )[iLocation];
		t3dRegionSaveResult_t saveResult = saveRegion( location, rDirectory, b16Bit, pEnsureResident, pResidentContext );
		if ( saveResult.m_bDeleted && saveResult.succeeded() ) {
			++result.m_nDeleted;
		} else if ( saveResult.m_bSaved ) {
			++result.m_nSaved;
		} else if ( saveResult.m_bSkipped ) {
			++result.m_nSkipped;
		} else if ( !saveResult.succeeded() ) {
			++result.m_nFailed;
		}
		result.m_regions.push_back( saveResult );
	}
	return result;
}

Dictionary Terrain3DRegionPersistence::getStartupLoadStatsDictionary( void ) const {
	Dictionary stats;
	stats["total_usec_last"] = ( int64_t )( m_startupLoadStats.m_nTotalUsecLast );
	stats["enumeration_usec_last"] = ( int64_t )( m_startupLoadStats.m_nEnumerationUsecLast );
	stats["deserialize_usec_last"] = ( int64_t )( m_startupLoadStats.m_nDeserializeUsecLast );
	stats["staging_usec_last"] = ( int64_t )( m_startupLoadStats.m_nStagingUsecLast );
	stats["publish_usec_last"] = ( int64_t )( m_startupLoadStats.m_nPublishUsecLast );
	stats["finalize_usec_last"] = ( int64_t )( m_startupLoadStats.m_nFinalizeUsecLast );
	stats["region_files_last"] = m_startupLoadStats.m_nRegionFilesLast;
	stats["regions_published_last"] = m_startupLoadStats.m_nRegionsPublishedLast;
	stats["succeeded_last"] = m_startupLoadStats.m_bSucceededLast;
	stats["format"] = "legacy_godot_resources";
	stats["world_manifest_used"] = false;
	stats["resource_deserialize_thread"] = "caller/main";
	return stats;
}
