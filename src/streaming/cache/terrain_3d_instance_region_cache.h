// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_INSTANCE_REGION_CACHE_H
#define TERRAIN3D_INSTANCE_REGION_CACHE_H

#include "Terrain3DTypeSupport.h"
#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"
#include "streaming/residency/terrain_3d_residency_state.h"

#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/variant/dictionary.hpp>
#include "data/persistence/terrain_3d_region_repository.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"

typedef struct t3dInstanceRegionCacheChanges_s {

	t3dRegionCoord_t m_szLoaded[T3D_INSTANCE_CACHE_MAX_PAGES] = {};
	t3dRegionCoord_t m_szEvicted[T3D_INSTANCE_CACHE_MAX_PAGES] = {};
	int m_nLoaded = 0;
	int m_nEvicted = 0;

	void recordLoaded( const Vector2i &vLocation );
	void recordEvicted( const Vector2i &vLocation );
} t3dInstanceRegionCacheChanges_t;

// t3dInstanceRegionCacheEvictionReason_e centralized in Terrain3DTypes.h.

typedef struct t3dInstanceRegionCacheEvictionDiagnostic_s {

	t3dRegionCoord_t m_admittedLocation = t3dInvalidRegionCoord();
	t3dRegionCoord_t m_evictedLocation = t3dInvalidRegionCoord();
	size_t m_nTrackedBytes = 0u;
	size_t m_nCurrentBytes = 0u;
	size_t m_nBudgetBytes = 0u;
	t3dInstanceRegionCacheEvictionReason_t m_eReason = TERRAIN3D_INSTANCE_REGION_EVICTION_NONE;
} t3dInstanceRegionCacheEvictionDiagnostic_t;

typedef struct t3dInstanceRegionCacheStats_s {

	bool m_bEnabled = false;
	bool m_bSelectionPending = true;
	int m_nCapacity = 0;
	int m_nResident = 0;
	int m_nDesiredLastTick = 0;
	int m_nLoadsLastTick = 0;
	int m_nEvictionsLastTick = 0;
	uint32_t m_nStallFlagsLastTick = 0u;
	size_t m_nResidentBytes = 0u;
	size_t m_nLoadBytesLastTick = 0u;
	uint64_t m_nServiceUsecLast = 0u;
	bool m_bMaterializationPending = false;
	bool m_bMaterializationServiceActive = false;
	uint64_t m_nColdMaterializations = 0u;
	uint64_t m_nMaterializationMisses = 0u;
	uint64_t m_nEvictionsTotal = 0u;
	uint64_t m_nCachePolicyEvictions = 0u;
	size_t m_nTrackedResidentBytes = 0u;
	t3dInstanceRegionCacheEvictionDiagnostic_t m_lastEviction;
} t3dInstanceRegionCacheStats_t;

class Terrain3DInstanceRegionCache {
public:
	Terrain3DInstanceRegionCache( Terrain3DRegionRepository *pRepository, Terrain3DRuntimePageStore *pRuntimePageStore );

	void configure( bool bEnabled, int nBudgetMb, int nLoadPages, int nLoadBudgetMb, t3dInstanceRegionCacheChanges_s *pChanges = NULL );
	void reset( bool bRestoreAll, t3dInstanceRegionCacheChanges_s *pChanges = NULL );
	void invalidate( const Vector2i &vLocation, t3dInstanceRegionCacheChanges_s *pChanges = NULL );
	bool evict( const Vector2i &vLocation, t3dInstanceRegionCacheChanges_s *pChanges = NULL );
	bool ensureResident( const Vector2i &vLocation, bool bDemandLoad, t3dInstanceRegionCacheChanges_s *pChanges = NULL );
	bool refreshResidentBytes( const Vector2i &vLocation );
	size_t reconcileResidentBytes( void );
	size_t getTrackedResidentBytes( const Vector2i &vLocation ) const;
	size_t getTrackedResidentBytesSum( void ) const;
	EA_FORCE_INLINE const t3dInstanceRegionCacheEvictionDiagnostic_t &getLastEvictionDiagnostic( void ) const {
		return m_lastEviction;
	}
	void serviceColdMaterialization( const t3dResidencyFrameState_s &rFrame, bool bForce, int nEffectiveLoadPages );
	void update( const t3dResidencyFrameState_s &rFrame, bool bForce, int nEffectiveLoadPages, t3dInstanceRegionCacheChanges_s &rChanges );

	EA_FORCE_INLINE bool isEnabled( void ) const {
		return m_bEnabled;
	}
	EA_FORCE_INLINE bool isResident( const Vector2i &vLocation ) const {
		return !m_bEnabled || findSlot( vLocation ) >= 0;
	}
	EA_FORCE_INLINE int getLoadBudgetPages( void ) const {
		return m_nLoadBudgetPages;
	}
	EA_FORCE_INLINE uint32_t getStallFlagsLastTick( void ) const {
		return m_nStallFlagsLastTick;
	}
	EA_FORCE_INLINE bool isSelectionDirty( void ) const {
		return m_bSelectionDirty;
	}
	EA_FORCE_INLINE void markSelectionDirty( void ) {
		m_bSelectionDirty = true;
	}
	EA_FORCE_INLINE size_t getReservedBytes( void ) const {
		return m_nResidentBytes;
	}
	t3dInstanceRegionCacheStats_t getStats( void ) const;
	Dictionary getStatsDictionary( bool bDetailed, int nEffectiveLoadPages ) const;

private:
	Terrain3DRegionRepository *m_pRepository = NULL;
	Terrain3DRuntimePageStore *m_pRuntimePageStore = NULL;

	bool m_bEnabled = false;
	bool m_bSelectionDirty = true;
	int m_nBudgetMb = 16;
	int m_nLoadBudgetPages = 4;
	int m_nLoadBudgetMb = 2;
	int m_nCapacity = T3D_INSTANCE_CACHE_MAX_PAGES;
	int m_nResidentCount = 0;
	int m_nDesiredLastTick = 0;
	int m_nLoadsLastTick = 0;
	uint32_t m_nStallFlagsLastTick = 0u;
	int m_nEvictionsLastTick = 0;
	int m_nDemandLoads = 0;
	size_t m_nResidentBytes = 0u;
	size_t m_nLoadBytesLastTick = 0u;
	uint64_t m_nServiceUsecLast = 0u;
	bool m_bMaterializationPending = false;
	bool m_bMaterializationServiceActive = false;
	uint64_t m_nColdMaterializations = 0u;
	uint64_t m_nMaterializationMisses = 0u;
	uint64_t m_nEvictionsTotal = 0u;
	uint64_t m_nCachePolicyEvictions = 0u;
	uint64_t m_nPrefetchStreamingGenerationLast = 0u;
	Vector2i m_center = s_v2iMax;
	t3dRegionCoord_t m_szLocations[T3D_INSTANCE_CACHE_MAX_PAGES] = {};
	size_t m_szResidentBytes[T3D_INSTANCE_CACHE_MAX_PAGES] = {};
	uint8_t m_szRegionMap[T3D_REGION_SLOTS] = {};
	t3dInstanceRegionCacheEvictionDiagnostic_t m_lastEviction;

	int findSlot( const Vector2i &vLocation ) const;
	bool attachResidentStore( const Vector2i &vLocation, bool bDemandLoad, t3dInstanceRegionCacheChanges_s *pChanges );
	bool materializeRegionCold( const Vector2i &vLocation, const t3dResidencyFrameState_s &rFrame );
	void updateTrackedResidentBytes( int iSlot, size_t nCurrentBytes );
	void recordEvictionDiagnostic( int iSlot,
	        const Vector2i &rLocation,
	        const Vector2i &rAdmittedLocation,
	        size_t nCurrentBytes,
	        t3dInstanceRegionCacheEvictionReason_t eReason );
	bool evictSlot( int iSlot,
	        t3dInstanceRegionCacheChanges_s *pChanges,
	        t3dInstanceRegionCacheEvictionReason_t eReason,
	        const Vector2i &rAdmittedLocation );
	int collectDesired( const t3dResidencyFrameState_s &rFrame, const Vector2i &rCenter, Vector2i *pLocations, int nCapacity ) const;
};

#endif // TERRAIN3D_INSTANCE_REGION_CACHE_H
