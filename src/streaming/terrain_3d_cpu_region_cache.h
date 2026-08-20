// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_CPU_REGION_CACHE_H
#define TERRAIN3D_CPU_REGION_CACHE_H

#include "Terrain3DConstants.h"
#include "streaming/terrain_3d_residency_state.h"
#include "streaming/terrain_3d_runtime_page_store.h"

#include <godot_cpp/variant/dictionary.hpp>

#include <cstddef>
#include <cstdint>

class Terrain3DRegion;
class T3DRegionPersistence;
class T3DRegionRepository;
class T3DSamplingStorage;


struct T3DCpuRegionCacheStats {
	bool bEnabled = false;
	bool bSelectionPending = true;
	int nCapacity = 0;
	int nResident = 0;
	int nDesiredLastTick = 0;
	int nLoadsLastTick = 0;
	int nEvictionsLastTick = 0;
	int nPinnedLastTick = 0;
	uint32_t nStallFlagsLastTick = 0u;
	size_t nLoadBytesLastTick = 0u;
	uint64_t nServiceUsecLast = 0u;
};

class T3DCpuRegionCache {
public:
	T3DCpuRegionCache(
		T3DRegionRepository *pRepository,
		T3DRegionPersistence *pPersistence,
		T3DSamplingStorage *pSamplingStorage,
		T3DRuntimePageStore *pRuntimePageStore
	);

	bool configure( bool bEnabled, int nBudgetMb, int nLoadLayers, int nLoadBudgetMb );
	void setRuntimePageConfig( const T3DRuntimePageConfig &rConfig );
	void reset( bool bReleaseCleanMaps );
	void refreshCapacity( void );
	void invalidate( const Vector2i &vLocation );

	bool loadSource( const Vector2i &vLocation, Ref<Terrain3DRegion> &rSource ) const;
	bool requestSourceAsync( const Vector2i &vLocation );
	bool ensureResident( const Vector2i &vLocation, bool bDemandLoad = true );
	bool ensureSampleArea( const Vector3 &rWorldPosition, int nRadiusTexels );
	void update( const T3DResidencyFrameState &rFrame, bool bForce, int nEffectiveLoadLayers );

	bool isEnabled( void ) const { return m_bEnabled; }
	int getCapacity( void ) const { return m_nCapacity; }
	int getLoadBudgetLayers( void ) const { return m_nLoadBudgetLayers; }
	uint32_t getStallFlagsLastTick( void ) const { return m_nStallFlagsLastTick; }
	bool isResident( const Vector2i &vLocation ) const { return findSlot( vLocation ) >= 0; }
	bool isSelectionDirty( void ) const { return m_bSelectionDirty; }
	void markSelectionDirty( void ) { m_bSelectionDirty = true; }

	size_t estimateLayerBytes( void ) const;
	size_t getReservedBytes( void ) const;
	size_t getMinimumBytes( void ) const;
	size_t getHardCapBytes( void ) const;
	T3DCpuRegionCacheStats getStats( void ) const;
	Dictionary getStatsDictionary( bool bDetailed, int nEffectiveLoadLayers ) const;

private:
	T3DRegionRepository *m_pRepository = nullptr;
	T3DRegionPersistence *m_pPersistence = nullptr;
	T3DSamplingStorage *m_pSamplingStorage = nullptr;
	T3DRuntimePageStore *m_pRuntimePageStore = nullptr;
	T3DRuntimePageConfig m_runtimePageConfig;

	bool m_bEnabled = false;
	bool m_bSelectionDirty = true;
	int m_nBudgetMb = 16;
	int m_nLoadBudgetLayers = 4;
	int m_nLoadBudgetMb = 2;
	int m_nCapacity = 0;
	int m_nResidentCount = 0;
	int m_nDesiredLastTick = 0;
	int m_nLoadsLastTick = 0;
	uint64_t m_nLoadsSinceReset = 0u;
	uint32_t m_nStallFlagsLastTick = 0u;
	int m_nEvictionsLastTick = 0;
	int m_nPinnedLastTick = 0;
	int m_nDemandLoads = 0;
	size_t m_nLoadBytesLastTick = 0u;
	uint64_t m_nServiceUsecLast = 0u;
	uint64_t m_nPrefetchStreamingGenerationLast = 0u;
	int m_nAsyncRequestsLastTick = 0;
	int m_nAsyncReadyLastTick = 0;
	int m_nAsyncPendingLastTick = 0;
	int m_nAsyncOutstanding = 0;
	uint8_t m_szAsyncState[TERRAIN3D_MAX_REGION_SLOTS] = {};
	Vector2i m_center = V2I_MAX;
	Vector2i m_szLocations[TERRAIN3D_CPU_CACHE_MAX_LAYERS] = {};
	uint8_t m_szRegionMap[TERRAIN3D_MAX_REGION_SLOTS] = {};

	int calculateCapacity( void ) const;
	int findSlot( const Vector2i &vLocation ) const;
	bool takeSourceAsync( const Vector2i &vLocation, Ref<Terrain3DRegion> &rSource );
	void prewarmSources( const T3DResidencyFrameState &rFrame );
	void reapSourceAsync( const T3DResidencyFrameState &rFrame );
	bool publishSlot( int iSlot, const Vector2i &vLocation, const Ref<Terrain3DRegion> &rSource );
	bool publishCookedSlot( int iSlot, const Vector2i &vLocation, const Ref<Image> &rHeight,
		const Ref<Image> &rControl, const Ref<Image> &rColor, const Ref<Image> &rDerived );
	bool cookedRuntimeEnabled( void ) const;
	bool loadCookedMapsNow( const Vector2i &vLocation, Ref<Image> &rHeight, Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived ) const;
	bool evictSlot( int iSlot );
	void releaseUntrackedMaps( void );
	bool restoreAllMaps( void );
	int collectDesired(
		const T3DResidencyFrameState &rFrame, const Vector2i &rCenter,
		Vector2i *pLocations, int nCapacity
	) const;
};

#endif // TERRAIN3D_CPU_REGION_CACHE_H
