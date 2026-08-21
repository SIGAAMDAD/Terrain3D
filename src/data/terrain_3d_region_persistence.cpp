// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/terrain_3d_region_persistence.h"

#include "core/logger.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_region_repository.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "eastl_config.h"
#include <EASTL/bitset.h>

void T3DStagedRepository::reset( void ) {
	regions.clear();
	resources.clear();
	regionPtrs.fill( nullptr );
	regionLocations.fill( V2I_MAX );
	logicalRegionMap.fill( 0u );
	nTargetRegionSize = 0;
	bBoundedCpuLoad = false;
}

T3DRegionPersistence::T3DRegionPersistence( T3DRegionRepository *const pRepository ) :
		m_pRepository( pRepository ) {
}

void T3DRegionPersistence::resetLoadStats( void ) {
	m_startupLoadStats = T3DStartupLoadStats{};
	m_nLoadStartedUsec = Time::get_singleton()->get_ticks_usec();
	m_nPublishStartedUsec = 0u;
	m_nFinalizeStartedUsec = 0u;
}

bool T3DRegionPersistence::validateHeightPayload( const Ref<T3DRegion> &rRegion, const String &rPath ) {
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
	if ( T3DRegionCodec::hasValidHeightPayload( pHeight, rRegion->getRegionSize() ) ) {
		return true;
	}
	LOG( ERROR, "Malformed height payload in file: ", rPath, ". Format: ", format,
		", bytes: ", pHeight->get_data().size() );
	return false;
}

void T3DRegionPersistence::normalizeLoadedRegion(
	const Vector2i &rLocation, const Ref<T3DRegion> &rRegion,
	const bool bApplyRepositorySpacing, bool &rStorageUpgrade
) const {
	const Ref<Image> pLoadedHeight = rRegion->getHeightMap();
	rStorageUpgrade = rRegion->getVersion() < TERRAIN3D_DATA_CURRENT_VERSION || pLoadedHeight.is_null() ||
		pLoadedHeight->get_format() != Image::FORMAT_R16 ||
		( rRegion->getColorMap().is_valid() && rRegion->getColorMap()->has_mipmaps() );

	// Resource property setters are mutation-oriented and may dirty freshly
	// deserialized resources. Establish the serialized state as the clean baseline
	// before applying explicit format/version normalization.
	rRegion->setModified( false );
	rRegion->markRuntimeMapsCleanInternal();
	rRegion->setLocation( rLocation );
	rRegion->setVersion( TERRAIN3D_DATA_CURRENT_VERSION );
	rRegion->setDeleted( false );
	rRegion->setEdited( false );
	if ( bApplyRepositorySpacing && m_pRepository ) {
		rRegion->setVertexSpacing( m_pRepository->getVertexSpacing() );
	}
	rRegion->sanitizeMaps();
	rRegion->setModified( rStorageUpgrade || rRegion->isModified() );
}

Error T3DRegionPersistence::stageDirectory(
	const String &rDirectory, const bool bBoundedCpuLoad,
	T3DStagedRepository &rStaged
) {
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
	PackedStringArray files = T3DUtil::getFiles( rDirectory, "terrain3d*.res" );
	files.sort();
	m_startupLoadStats.nEnumerationUsecLast = Time::get_singleton()->get_ticks_usec() - m_nLoadStartedUsec;
	m_startupLoadStats.nRegionFilesLast = files.size();
	if ( files.is_empty() ) {
		LOG( INFO, "No Terrain3D region files found in: ", rDirectory );
		return ERR_FILE_NOT_FOUND;
	}

	rStaged.bBoundedCpuLoad = bBoundedCpuLoad;
	rStaged.regions.reserve( ( size_t ) ( files.size() ) );
	if ( files.size() > TERRAIN3D_MAX_REGION_SLOTS ) {
		LOG( ERROR, "Directory contains ", files.size(),
			" regions, exceeding the fixed region-slot capacity of ", TERRAIN3D_MAX_REGION_SLOTS );
		return ERR_OUT_OF_MEMORY;
	}

	for ( const String &fname : files ) {
		T3DStagedRegion entry;
		entry.location = T3DUtil::filenameToLocation( fname );
		entry.path = rDirectory.path_join( fname );
		rStaged.regions.push_back( entry );
	}

	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( !pLoader ) {
		return ERR_UNAVAILABLE;
	}

	LOG( DEBUG, "[terrain-data] Phase 1/4: synchronous region deserialization and validation" );
	const uint64_t deserializeStartedUsec = Time::get_singleton()->get_ticks_usec();
	eastl::bitset<TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE * TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE> occupied;

	for ( int fileIndex = 0; fileIndex < files.size(); ++fileIndex ) {
		const String &fname = files[fileIndex];
		T3DStagedRegion &entry = rStaged.regions[( size_t ) ( fileIndex )];
		const int mapIndex = T3DRegionRepository::getRegionMapIndex( entry.location );
		if ( mapIndex < 0 ) {
			LOG( ERROR, "Invalid or out-of-bounds region file name: ", fname );
			return ERR_FILE_CORRUPT;
		}
		if ( occupied.test( ( size_t ) ( mapIndex ) ) ) {
			LOG( ERROR, "Duplicate region location in directory: ", entry.location );
			return ERR_ALREADY_EXISTS;
		}
		occupied.set( ( size_t ) ( mapIndex ) );

		Ref<T3DRegion> region = pLoader->load(
			entry.path, "T3DRegion", ResourceLoader::CACHE_MODE_IGNORE );
		if ( region.is_null() ) {
			LOG( ERROR, "Cannot load region at ", entry.path );
			return ERR_FILE_CORRUPT;
		}
		const int regionSize = region->getRegionSize();
		if ( !isValidRegionSize( regionSize ) ) {
			LOG( ERROR, "Invalid region size ", regionSize, " in file: ", entry.path );
			return ERR_FILE_CORRUPT;
		}
		if ( !validateHeightPayload( region, entry.path ) ) {
			return ERR_FILE_CORRUPT;
		}
		if ( rStaged.nTargetRegionSize == 0 ) {
			rStaged.nTargetRegionSize = regionSize;
		} else if ( rStaged.nTargetRegionSize != regionSize ) {
			LOG( ERROR, "Region size mismatch. First loaded: ", rStaged.nTargetRegionSize,
				" next: ", regionSize, " in file: ", entry.path );
			return ERR_FILE_CORRUPT;
		}

		const int targetMapSize = TERRAIN3D_ACTIVE_WORLD_TEXELS / rStaged.nTargetRegionSize;
		if ( !terrain3dRegionLocationValid( entry.location, targetMapSize ) ) {
			LOG( ERROR, "Region ", entry.location, " is outside the fixed ",
				TERRAIN3D_ACTIVE_WORLD_TEXELS, "-texel world for region size ", rStaged.nTargetRegionSize );
			return ERR_FILE_CORRUPT;
		}

		bool storageUpgrade = false;
		normalizeLoadedRegion( entry.location, region, true, storageUpgrade );
		if ( bBoundedCpuLoad ) {
			const Ref<Image> pStagedHeight = region->getHeightMap();
			if ( pStagedHeight.is_null() || pStagedHeight->get_format() != Image::FORMAT_R16 ) {
				LOG( ERROR, "Unable to stage R16 height payload from: ", entry.path );
				return ERR_FILE_CORRUPT;
			}
			entry.stagedHeight = pStagedHeight->get_data();
			const size_t regionTexelCount = ( size_t ) ( rStaged.nTargetRegionSize ) *
				( size_t ) ( rStaged.nTargetRegionSize );
			const size_t expectedHeightBytes = regionTexelCount * sizeof( uint16_t );
			if ( ( size_t ) ( entry.stagedHeight.size() ) < expectedHeightBytes ) {
				LOG( ERROR, "Truncated staged height payload in: ", entry.path );
				return ERR_FILE_CORRUPT;
			}
			const int halfMap = targetMapSize / 2;
			const int activeX = entry.location.x + halfMap;
			const int activeY = entry.location.y + halfMap;
			entry.nActiveSlot = activeY * targetMapSize + activeX;
			if ( !region->isModified() ) {
				region->releaseRuntimeMapsUnmodifiedInternal();
			}
		}
		entry.pRegion = region;
	}

	m_startupLoadStats.nDeserializeUsecLast = Time::get_singleton()->get_ticks_usec() - deserializeStartedUsec;
	LOG( DEBUG, "[terrain-data] Phase 1/4 complete: validated ", rStaged.regions.size(),
		" regions at size ", rStaged.nTargetRegionSize );
	LOG( DEBUG, "[terrain-data] Phase 2/4: staging replacement repository tables" );
	const uint64_t stagingStartedUsec = Time::get_singleton()->get_ticks_usec();

	for ( int slot = 0; slot < ( int ) ( rStaged.regions.size() ); ++slot ) {
		T3DStagedRegion &entry = rStaged.regions[( size_t ) ( slot )];
		const int mapIndex = T3DRegionRepository::getRegionMapIndex( entry.location );
		if ( mapIndex < 0 ) {
			return ERR_FILE_CORRUPT;
		}
		rStaged.resources[entry.location] = entry.pRegion;
		rStaged.regionPtrs[( size_t ) ( mapIndex )] = entry.pRegion.ptr();
		rStaged.regionLocations[( size_t ) ( slot )] = entry.location;
		rStaged.logicalRegionMap[( size_t ) ( mapIndex )] = ( uint16_t ) ( slot + 1 );
	}

	m_startupLoadStats.nStagingUsecLast = Time::get_singleton()->get_ticks_usec() - stagingStartedUsec;
	LOG( DEBUG, "[terrain-data] Phase 2/4 complete: repository tables staged" );
	return OK;
}

void T3DRegionPersistence::beginPublication( void ) {
	m_nPublishStartedUsec = Time::get_singleton()->get_ticks_usec();
}

void T3DRegionPersistence::finishPublication( const int nPublishedRegions ) {
	if ( m_nPublishStartedUsec != 0u ) {
		m_startupLoadStats.nPublishUsecLast = Time::get_singleton()->get_ticks_usec() - m_nPublishStartedUsec;
	}
	m_startupLoadStats.nRegionsPublishedLast = MAX( 0, nPublishedRegions );
}

void T3DRegionPersistence::beginFinalize( void ) {
	m_nFinalizeStartedUsec = Time::get_singleton()->get_ticks_usec();
}

void T3DRegionPersistence::finishDirectoryLoad( const bool bSucceeded ) {
	if ( m_nFinalizeStartedUsec != 0u ) {
		m_startupLoadStats.nFinalizeUsecLast = Time::get_singleton()->get_ticks_usec() - m_nFinalizeStartedUsec;
	}
	if ( m_nLoadStartedUsec != 0u ) {
		m_startupLoadStats.nTotalUsecLast = Time::get_singleton()->get_ticks_usec() - m_nLoadStartedUsec;
	}
	m_startupLoadStats.bSucceededLast = bSucceeded;
}

void T3DRegionPersistence::publishStagedPaths( T3DStagedRepository &rStaged ) const {
	for ( T3DStagedRegion &entry : rStaged.regions ) {
		if ( entry.pRegion.is_valid() ) {
			entry.pRegion->take_over_path( entry.path );
		}
	}
}

T3DRegionLoadResult T3DRegionPersistence::loadRegion(
	const Vector2i &rLocation, const String &rDirectory
) const {
	T3DRegionLoadResult result;
	result.location = rLocation;
	if ( !m_pRepository || rDirectory.is_empty() ) {
		result.error = ERR_INVALID_PARAMETER;
		return result;
	}

	result.path = rDirectory.path_join( T3DUtil::locationToFilename( rLocation ) );
	LOG( INFO, "Loading region from location ", rLocation );
	if ( !FileAccess::file_exists( result.path ) ) {
		LOG( ERROR, "File ", result.path, " doesn't exist" );
		result.error = ERR_FILE_NOT_FOUND;
		return result;
	}

	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( !pLoader ) {
		result.error = ERR_UNAVAILABLE;
		return result;
	}
	result.pRegion = pLoader->load( result.path, "T3DRegion", ResourceLoader::CACHE_MODE_IGNORE );
	if ( result.pRegion.is_null() ) {
		LOG( ERROR, "Cannot load region at ", result.path );
		result.error = ERR_FILE_CORRUPT;
		return result;
	}
	if ( !validateHeightPayload( result.pRegion, result.path ) ) {
		result.pRegion.unref();
		result.error = ERR_FILE_CORRUPT;
		return result;
	}

	const int regionSize = result.pRegion->getRegionSize();
	if ( !isValidRegionSize( regionSize ) ) {
		LOG( ERROR, "Invalid region size ", regionSize, " in file: ", result.path );
		result.pRegion.unref();
		result.error = ERR_FILE_CORRUPT;
		return result;
	}
	if ( !m_pRepository->isEmpty() && m_pRepository->getRegionSize() != regionSize ) {
		LOG( ERROR, "Region size mismatch. First loaded: ", m_pRepository->getRegionSize(),
			" next: ", regionSize, " in file: ", result.path );
		result.pRegion.unref();
		result.error = ERR_INVALID_DATA;
		return result;
	}

	normalizeLoadedRegion( rLocation, result.pRegion, false, result.bStorageUpgrade );
	result.error = OK;
	return result;
}

bool T3DRegionPersistence::prepareRuntimeRegionSource(
	const Ref<T3DRegion> &rCandidate, const int nExpectedRegionSize,
	Ref<T3DRegion> &rSource
) const {
	rSource.unref();
	if ( rCandidate.is_null() || rCandidate->getRegionSize() != nExpectedRegionSize ) {
		return false;
	}
	const Ref<Image> pHeight = rCandidate->getHeightMap();
	if ( pHeight.is_null() || pHeight->get_format() != Image::FORMAT_R16 ||
		!T3DRegionCodec::hasValidHeightPayload( pHeight, nExpectedRegionSize ) ) {
		return false;
	}
	rCandidate->setModified( false );
	rCandidate->markRuntimeMapsCleanInternal();
	rCandidate->sanitizeMaps();
	rSource = rCandidate;
	return true;
}

bool T3DRegionPersistence::loadRuntimeRegionSource(
	const String &rPath, const int nExpectedRegionSize,
	Ref<T3DRegion> &rSource
) const {
	rSource.unref();
	if ( rPath.is_empty() || !FileAccess::file_exists( rPath ) ) {
		return false;
	}
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( !pLoader ) {
		return false;
	}
	const Ref<Resource> pResource = pLoader->load(
		rPath, "T3DRegion", ResourceLoader::CACHE_MODE_IGNORE );
	const Ref<T3DRegion> pLoaded = pResource;
	return prepareRuntimeRegionSource( pLoaded, nExpectedRegionSize, rSource );
}

void T3DRegionPersistence::publishLoadedRegionPath( const T3DRegionLoadResult &rResult ) const {
	if ( rResult.succeeded() ) {
		rResult.pRegion->take_over_path( rResult.path );
	}
}

void T3DRegionPersistence::updateEditorFile( const String &rPath ) {
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

T3DRegionSaveResult T3DRegionPersistence::saveRegion(
	const Vector2i &rLocation, const String &rDirectory, const bool b16Bit,
	EnsureRegionResidentFn pEnsureResident, void *pResidentContext
) {
	T3DRegionSaveResult result;
	result.location = rLocation;
	if ( !m_pRepository ) {
		result.error = ERR_UNCONFIGURED;
		return result;
	}
	const Ref<T3DRegion> pRegion = m_pRepository->getRegion( rLocation );
	if ( pRegion.is_null() ) {
		LOG( ERROR, "No region found at: ", rLocation );
		result.error = ERR_DOES_NOT_EXIST;
		return result;
	}
	result.bFound = true;
	const String fname = T3DUtil::locationToFilename( rLocation );
	result.path = rDirectory.path_join( fname );

	if ( pRegion->isDeleted() ) {
		result.bDeleted = true;
		LOG( DEBUG, "Removing ", rLocation, " from _regions" );
		m_pRepository->eraseRegionResource( rLocation );
		LOG( DEBUG, "File to be deleted: ", result.path );
		if ( FileAccess::file_exists( result.path ) ) {
			Ref<DirAccess> pDirectory = DirAccess::open( rDirectory );
			if ( pDirectory.is_null() ) {
				LOG( ERROR, "Cannot open directory for writing: ", rDirectory,
					" error: ", DirAccess::get_open_error() );
				result.error = DirAccess::get_open_error();
				return result;
			}
			const Error removeError = pDirectory->remove( fname );
			if ( removeError != OK ) {
				LOG( ERROR, "Could not remove file: ", fname, ", error code: ", removeError );
				result.error = removeError;
				return result;
			}
			LOG( INFO, "File ", result.path, " deleted" );
		} else {
			LOG( INFO, "File to delete ", result.path,
				" doesn't exist. (Maybe from add, undo, save)" );
		}

		updateEditorFile( result.path );
		result.bSourceChanged = true;
		result.error = OK;
		return result;
	}

	if ( !pRegion->isModified() ) {
		result.bSkipped = true;
		result.error = ERR_SKIP;
		return result;
	}
	if ( pEnsureResident && !pEnsureResident( pResidentContext, rLocation ) ) {
		LOG( ERROR, "Unable to page CPU terrain maps before saving region: ", rLocation );
		result.error = ERR_UNAVAILABLE;
		return result;
	}

	result.error = pRegion->save( result.path, b16Bit );
	if ( !( result.error == OK || result.error == ERR_SKIP ) ) {
		LOG( ERROR, "Could not save file: ", result.path, ", error: ",
			UtilityFunctions::error_string( result.error ), " (", result.error, ")" );
		return result;
	}
	if ( result.error == ERR_SKIP ) {
		result.bSkipped = true;
		return result;
	}

	result.bSaved = true;
	result.bSourceChanged = true;
	updateEditorFile( result.path );
	return result;
}

T3DDirectorySaveResult T3DRegionPersistence::saveDirectory(
	const String &rDirectory, const bool b16Bit,
	EnsureRegionResidentFn pEnsureResident, void *pResidentContext
) {
	T3DDirectorySaveResult result;
	if ( !m_pRepository ) {
		return result;
	}
	const Array locations = m_pRepository->getRegionKeys();
	result.regions.reserve( ( size_t ) ( locations.size() ) );
	for ( const Vector2i location : locations ) {
		T3DRegionSaveResult saveResult = saveRegion(
			location, rDirectory, b16Bit, pEnsureResident, pResidentContext );
		if ( saveResult.bDeleted && saveResult.succeeded() ) {
			++result.nDeleted;
		} else if ( saveResult.bSaved ) {
			++result.nSaved;
		} else if ( saveResult.bSkipped ) {
			++result.nSkipped;
		} else if ( !saveResult.succeeded() ) {
			++result.nFailed;
		}
		result.regions.push_back( saveResult );
	}
	return result;
}

Dictionary T3DRegionPersistence::getStartupLoadStatsDictionary( void ) const {
	Dictionary stats;
	stats["total_usec_last"] = ( int64_t ) ( m_startupLoadStats.nTotalUsecLast );
	stats["enumeration_usec_last"] = ( int64_t ) ( m_startupLoadStats.nEnumerationUsecLast );
	stats["deserialize_usec_last"] = ( int64_t ) ( m_startupLoadStats.nDeserializeUsecLast );
	stats["staging_usec_last"] = ( int64_t ) ( m_startupLoadStats.nStagingUsecLast );
	stats["publish_usec_last"] = ( int64_t ) ( m_startupLoadStats.nPublishUsecLast );
	stats["finalize_usec_last"] = ( int64_t ) ( m_startupLoadStats.nFinalizeUsecLast );
	stats["region_files_last"] = m_startupLoadStats.nRegionFilesLast;
	stats["regions_published_last"] = m_startupLoadStats.nRegionsPublishedLast;
	stats["succeeded_last"] = m_startupLoadStats.bSucceededLast;
	stats["format"] = "legacy_godot_resources";
	stats["world_manifest_used"] = false;
	stats["resource_deserialize_thread"] = "caller/main";
	return stats;
}
