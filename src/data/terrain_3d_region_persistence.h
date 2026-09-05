// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_REGION_PERSISTENCE_H
#define TERRAIN3D_REGION_PERSISTENCE_H

#include "core/terrain_3d_eastl_helpers.h"
#include "Terrain3DConstants.h"
#include "data/terrain_3d_region.h"
#include "eastl_config.h"

#include <EASTL/array.h>
#include <EASTL/vector.h>
#include <stdint.h>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

class Terrain3DRegionRepository;

typedef struct t3dStagedRegion_s {
	Vector2i m_location = s_v2iMax;
	String m_path;
	Ref<Terrain3DRegion> m_region;
	PackedByteArray m_stagedHeight;
	int m_nActiveSlot = -1;
} t3dStagedRegion_t;

// Complete replacement candidate. No live repository state is mutated while
// this object is being built or validated.
typedef struct t3dStagedRepository_s {
	eastl::vector<t3dStagedRegion_t, Terrain3DEastlAllocator> m_regions;
	Dictionary m_resources;
	eastl::array<Terrain3DRegion *, s_nRegionSlots> m_pRegionPtrs = {};
	eastl::array<t3dRegionCoord_t, s_nRegionSlots> m_regionLocations = {};
	eastl::array<uint16_t, s_nRegionSlots> m_logicalRegionMap = {};
	bool m_bBoundedCpuLoad = false;

	void reset( void );
	EA_FORCE_INLINE int getRegionCount( void ) const {
		return ( int )( m_regions.size() );
	}
} t3dStagedRepository_t;

typedef struct t3dRegionLoadResult_s {
	Error m_error = FAILED;
	Vector2i m_location = s_v2iMax;
	String m_path;
	Ref<Terrain3DRegion> m_region;
	bool m_bStorageUpgrade = false;

	EA_FORCE_INLINE bool succeeded( void ) const {
		return m_error == OK && m_region.is_valid();
	}
} t3dRegionLoadResult_t;

typedef struct t3dRegionSaveResult_s {
	Error m_error = FAILED;
	Vector2i m_location = s_v2iMax;
	String m_path;
	bool m_bFound = false;
	bool m_bDeleted = false;
	bool m_bSourceChanged = false;
	bool m_bSaved = false;
	bool m_bSkipped = false;

	EA_FORCE_INLINE bool succeeded( void ) const {
		return m_error == OK || m_error == ERR_SKIP;
	}
} t3dRegionSaveResult_t;

typedef struct t3dDirectorySaveResult_s {
	eastl::vector<t3dRegionSaveResult_t, Terrain3DEastlAllocator> m_regions;
	int m_nSaved = 0;
	int m_nDeleted = 0;
	int m_nSkipped = 0;
	int m_nFailed = 0;
} t3dDirectorySaveResult_t;

typedef struct t3dStartupLoadStats_s {
	uint64_t m_nTotalUsecLast = 0u;
	uint64_t m_nEnumerationUsecLast = 0u;
	uint64_t m_nDeserializeUsecLast = 0u;
	uint64_t m_nStagingUsecLast = 0u;
	uint64_t m_nPublishUsecLast = 0u;
	uint64_t m_nFinalizeUsecLast = 0u;
	int m_nRegionFilesLast = 0;
	int m_nRegionsPublishedLast = 0;
	bool m_bSucceededLast = false;
} t3dStartupLoadStats_t;

// Owns Terrain3DRegion resource serialization and staged repository loading.
// Runtime residency, GPU publication, page cooking, Far-atlas cooking, and
// terrain notifications are deliberately outside this class.
class Terrain3DRegionPersistence {
	CLASS_NAME_STATIC( "Terrain3DRegionPersistence" );

public:
	typedef bool ( *EnsureRegionResidentFn )( void *pContext, const Vector2i &rLocation );

	explicit Terrain3DRegionPersistence( Terrain3DRegionRepository *pRepository );
	Terrain3DRegionPersistence( const Terrain3DRegionPersistence & ) = delete;
	Terrain3DRegionPersistence &operator=( const Terrain3DRegionPersistence & ) = delete;

	Error stageDirectory( const String &rDirectory, bool bBoundedCpuLoad, t3dStagedRepository_t &rStaged );
	void beginPublication( void );
	void finishPublication( int nPublishedRegions );
	void beginFinalize( void );
	void finishDirectoryLoad( bool bSucceeded );
	void publishStagedPaths( t3dStagedRepository_t &rStaged ) const;

	t3dRegionLoadResult_t loadRegion( const Vector2i &rLocation, const String &rDirectory ) const;
	void publishLoadedRegionPath( const t3dRegionLoadResult_t &rResult ) const;
	bool loadRuntimeRegionSource( const String &rPath, Ref<Terrain3DRegion> &rSource ) const;
	bool prepareRuntimeRegionSource( const Ref<Terrain3DRegion> &rCandidate, Ref<Terrain3DRegion> &rSource ) const;

	t3dRegionSaveResult_t saveRegion( const Vector2i &rLocation,
	        const String &rDirectory,
	        bool b16Bit,
	        EnsureRegionResidentFn pEnsureResident = NULL,
	        void *pResidentContext = NULL );
	t3dDirectorySaveResult_t saveDirectory(
	        const String &rDirectory, bool b16Bit, EnsureRegionResidentFn pEnsureResident = NULL, void *pResidentContext = NULL );

	EA_FORCE_INLINE const t3dStartupLoadStats_t &getStartupLoadStats( void ) const {
		return m_startupLoadStats;
	}
	Dictionary getStartupLoadStatsDictionary( void ) const;

private:
	Terrain3DRegionRepository *m_pRepository = NULL;
	t3dStartupLoadStats_t m_startupLoadStats;
	uint64_t m_nLoadStartedUsec = 0u;
	uint64_t m_nPublishStartedUsec = 0u;
	uint64_t m_nFinalizeStartedUsec = 0u;

	void resetLoadStats( void );
	static void updateEditorFile( const String &rPath );
	static bool validateHeightPayload( const Ref<Terrain3DRegion> &rRegion, const String &rPath );
	void normalizeLoadedRegion( const Vector2i &rLocation, const Ref<Terrain3DRegion> &rRegion, bool &rStorageUpgrade ) const;
};

#endif // TERRAIN3D_REGION_PERSISTENCE_H
