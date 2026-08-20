// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_INSTANCE_REGION_CACHE_H
#define TERRAIN3D_INSTANCE_REGION_CACHE_H

#include "core/terrain_3d_compiler.h"
#include "Terrain3DConstants.h"
#include "instances/terrain_3d_instance_cache_constants.h"
#include "streaming/terrain_3d_residency_state.h"

#include <godot_cpp/variant/dictionary.hpp>

#include <cstddef>
#include <cstdint>

class T3DRegionRepository;
class T3DRuntimePageStore;

struct T3DInstanceRegionCacheChanges {
	Vector2i loaded[TERRAIN3D_INSTANCE_CACHE_MAX_PAGES] = {};
	Vector2i evicted[TERRAIN3D_INSTANCE_CACHE_MAX_PAGES] = {};
	int nLoaded = 0;
	int nEvicted = 0;

	void recordLoaded( const Vector2i &vLocation );
	void recordEvicted( const Vector2i &vLocation );
};

struct T3DInstanceRegionCacheStats {
	bool bEnabled = false;
	bool bSelectionPending = true;
	int nCapacity = 0;
	int nResident = 0;
	int nDesiredLastTick = 0;
	int nLoadsLastTick = 0;
	int nEvictionsLastTick = 0;
	uint32_t nStallFlagsLastTick = 0u;
	size_t nResidentBytes = 0u;
	size_t nLoadBytesLastTick = 0u;
	uint64_t nServiceUsecLast = 0u;
};

class T3DInstanceRegionCache {
public:
	T3DInstanceRegionCache( T3DRegionRepository *pRepository, T3DRuntimePageStore *pRuntimePageStore );

	void configure( bool bEnabled, int nBudgetMb, int nLoadPages, int nLoadBudgetMb,
		T3DInstanceRegionCacheChanges *pChanges = nullptr );
	void reset( bool bRestoreAll, T3DInstanceRegionCacheChanges *pChanges = nullptr );
	void invalidate( const Vector2i &vLocation, T3DInstanceRegionCacheChanges *pChanges = nullptr );
	bool evict( const Vector2i &vLocation, T3DInstanceRegionCacheChanges *pChanges = nullptr );
	bool ensureResident( const Vector2i &vLocation, bool bDemandLoad,
		T3DInstanceRegionCacheChanges *pChanges = nullptr );
	void update( const T3DResidencyFrameState &rFrame, bool bForce, int nEffectiveLoadPages,
		T3DInstanceRegionCacheChanges &rChanges );

	EA_FORCE_INLINE bool isEnabled( void ) const { return m_bEnabled; }
	EA_FORCE_INLINE bool isResident( const Vector2i &vLocation ) const { return !m_bEnabled || findSlot( vLocation ) >= 0; }
	EA_FORCE_INLINE int getLoadBudgetPages( void ) const { return m_nLoadBudgetPages; }
	EA_FORCE_INLINE uint32_t getStallFlagsLastTick( void ) const { return m_nStallFlagsLastTick; }
	EA_FORCE_INLINE bool isSelectionDirty( void ) const { return m_bSelectionDirty; }
	EA_FORCE_INLINE void markSelectionDirty( void ) { m_bSelectionDirty = true; }
	EA_FORCE_INLINE size_t getReservedBytes( void ) const { return m_nResidentBytes; }
	T3DInstanceRegionCacheStats getStats( void ) const;
	Dictionary getStatsDictionary( bool bDetailed, int nEffectiveLoadPages ) const;

private:
	T3DRegionRepository *m_pRepository = nullptr;
	T3DRuntimePageStore *m_pRuntimePageStore = nullptr;

	bool m_bEnabled = false;
	bool m_bSelectionDirty = true;
	int m_nBudgetMb = 16;
	int m_nLoadBudgetPages = 4;
	int m_nLoadBudgetMb = 2;
	int m_nCapacity = TERRAIN3D_INSTANCE_CACHE_MAX_PAGES;
	int m_nResidentCount = 0;
	int m_nDesiredLastTick = 0;
	int m_nLoadsLastTick = 0;
	uint32_t m_nStallFlagsLastTick = 0u;
	int m_nEvictionsLastTick = 0;
	int m_nDemandLoads = 0;
	size_t m_nResidentBytes = 0u;
	size_t m_nLoadBytesLastTick = 0u;
	uint64_t m_nServiceUsecLast = 0u;
	uint64_t m_nPrefetchStreamingGenerationLast = 0u;
	Vector2i m_center = V2I_MAX;
	Vector2i m_szLocations[TERRAIN3D_INSTANCE_CACHE_MAX_PAGES] = {};
	uint8_t m_szRegionMap[TERRAIN3D_MAX_REGION_SLOTS] = {};

	int findSlot( const Vector2i &vLocation ) const;
	bool evictSlot( int iSlot, T3DInstanceRegionCacheChanges *pChanges );
	int collectDesired( const T3DResidencyFrameState &rFrame, const Vector2i &rCenter,
		Vector2i *pLocations, int nCapacity ) const;
};

#endif // TERRAIN3D_INSTANCE_REGION_CACHE_H
