// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_REGION_PERSISTENCE_H
#define TERRAIN3D_REGION_PERSISTENCE_H

#include "Terrain3DConstants.h"
#include "data/terrain_3d_region.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "eastl_config.h"
#include <EASTL/array.h>
#include <EASTL/vector.h>
#include <cstdint>

class T3DRegionRepository;

typedef struct t3dStagedRegion_s {
	Vector2i location = V2I_MAX;
	String path;
	Ref<T3DRegion> pRegion;
	PackedByteArray stagedHeight;
	int nActiveSlot = -1;
} t3dStagedRegion_t;

// Complete replacement candidate. No live repository state is mutated while
// this object is being built or validated.
typedef struct t3dStagedRepository_s {
	eastl::vector<t3dStagedRegion_t> regions;
	Dictionary resources;
	eastl::array<T3DRegion *, TERRAIN3D_MAX_REGION_SLOTS> regionPtrs = {};
	eastl::array<Vector2i, TERRAIN3D_MAX_REGION_SLOTS> regionLocations = {};
	eastl::array<uint16_t, TERRAIN3D_MAX_REGION_SLOTS> logicalRegionMap = {};
	int nTargetRegionSize = 0;
	bool bBoundedCpuLoad = false;

	void reset( void );
	EA_FORCE_INLINE int getRegionCount( void ) const { return ( int ) ( regions.size() ); }
} t3dStagedRepository_t;

typedef struct t3dRegionLoadResult_s {
	Error error = FAILED;
	Vector2i location = V2I_MAX;
	String path;
	Ref<T3DRegion> pRegion;
	bool bStorageUpgrade = false;

	EA_FORCE_INLINE bool succeeded( void ) const { return error == OK && pRegion.is_valid(); }
} t3dRegionLoadResult_t;

typedef struct t3dRegionSaveResult_s {
	Error error = FAILED;
	Vector2i location = V2I_MAX;
	String path;
	bool bFound = false;
	bool bDeleted = false;
	bool bSourceChanged = false;
	bool bSaved = false;
	bool bSkipped = false;

	EA_FORCE_INLINE bool succeeded( void ) const { return error == OK || error == ERR_SKIP; }
} t3dRegionSaveResult_t;

typedef struct t3dDirectorySaveResult_s {
	eastl::vector<t3dRegionSaveResult_t> regions;
	int nSaved = 0;
	int nDeleted = 0;
	int nSkipped = 0;
	int nFailed = 0;
} t3dDirectorySaveResult_t;

typedef struct t3dStartupLoadStats_s {
	uint64_t nTotalUsecLast = 0u;
	uint64_t nEnumerationUsecLast = 0u;
	uint64_t nDeserializeUsecLast = 0u;
	uint64_t nStagingUsecLast = 0u;
	uint64_t nPublishUsecLast = 0u;
	uint64_t nFinalizeUsecLast = 0u;
	int nRegionFilesLast = 0;
	int nRegionsPublishedLast = 0;
	bool bSucceededLast = false;
} t3dStartupLoadStats_t;

// Owns T3DRegion resource serialization and staged repository loading.
// Runtime residency, GPU publication, page cooking, Far-atlas cooking, and
// terrain notifications are deliberately outside this class.
class T3DRegionPersistence {
public:
	using EnsureRegionResidentFn = bool ( * )( void *pContext, const Vector2i &rLocation );

	explicit T3DRegionPersistence( T3DRegionRepository *pRepository );
	T3DRegionPersistence( const T3DRegionPersistence & ) = delete;
	T3DRegionPersistence &operator=( const T3DRegionPersistence & ) = delete;

	Error stageDirectory( const String &rDirectory, bool bBoundedCpuLoad, t3dStagedRepository_t &rStaged );
	void beginPublication( void );
	void finishPublication( int nPublishedRegions );
	void beginFinalize( void );
	void finishDirectoryLoad( bool bSucceeded );
	void publishStagedPaths( t3dStagedRepository_t &rStaged ) const;

	t3dRegionLoadResult_t loadRegion( const Vector2i &rLocation, const String &rDirectory ) const;
	void publishLoadedRegionPath( const t3dRegionLoadResult_t &rResult ) const;
	bool loadRuntimeRegionSource(
		const String &rPath, int nExpectedRegionSize, Ref<T3DRegion> &rSource
	) const;
	bool prepareRuntimeRegionSource(
		const Ref<T3DRegion> &rCandidate, int nExpectedRegionSize, Ref<T3DRegion> &rSource
	) const;

	t3dRegionSaveResult_t saveRegion(
		const Vector2i &rLocation, const String &rDirectory, bool b16Bit,
		EnsureRegionResidentFn pEnsureResident = nullptr, void *pResidentContext = nullptr
	);
	t3dDirectorySaveResult_t saveDirectory(
		const String &rDirectory, bool b16Bit,
		EnsureRegionResidentFn pEnsureResident = nullptr, void *pResidentContext = nullptr
	);

	EA_FORCE_INLINE const t3dStartupLoadStats_t &getStartupLoadStats( void ) const { return m_startupLoadStats; }
	Dictionary getStartupLoadStatsDictionary( void ) const;

private:
	T3DRegionRepository *m_pRepository = nullptr;
	t3dStartupLoadStats_t m_startupLoadStats;
	uint64_t m_nLoadStartedUsec = 0u;
	uint64_t m_nPublishStartedUsec = 0u;
	uint64_t m_nFinalizeStartedUsec = 0u;

	void resetLoadStats( void );
	static void updateEditorFile( const String &rPath );
	static bool validateHeightPayload( const Ref<T3DRegion> &rRegion, const String &rPath );
	void normalizeLoadedRegion(
		const Vector2i &rLocation, const Ref<T3DRegion> &rRegion,
		bool bApplyRepositorySpacing, bool &rStorageUpgrade
	) const;
};

#endif // TERRAIN3D_REGION_PERSISTENCE_H
