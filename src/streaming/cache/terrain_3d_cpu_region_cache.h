// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_CPU_REGION_CACHE_H
#define TERRAIN3D_CPU_REGION_CACHE_H

#include "Terrain3DTypes.h"
#include "streaming/cache/terrain_3d_region_cache_common.h"

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"
#include "streaming/residency/terrain_3d_residency_state.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"

#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/variant/dictionary.hpp>
#include "data/model/terrain_3d_region.h"
#include "data/persistence/terrain_3d_region_persistence.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"

class Terrain3DCpuRegionCache : private t3dRegionCacheCommon_t
{
public:
	Terrain3DCpuRegionCache( Terrain3DRegionRepository	*pRepository,
							 Terrain3DRegionPersistence *pPersistence,
							 Terrain3DSamplingStorage	*pSamplingStorage,
							 Terrain3DRuntimePageStore	*pRuntimePageStore );

#if defined( T3D_TESTS_ENABLED )
	// Controlled lifecycle injection without starting Godot worker jobs.
	bool seedAsyncRequestForTest( const Vector2i &vLocation, bool bCooked, const String &rPath );
	uint8_t asyncStateForTest( const Vector2i &vLocation ) const;
	uint32_t asyncGenerationForTest( const Vector2i &vLocation ) const;
	int outstandingForTest() const { return m_nAsyncOutstanding; }
	void completeAsyncRequestForTest( const Vector2i &vLocation );
#endif
	bool configure( bool bEnabled, int nBudgetMb, int nLoadLayers, int nLoadBudgetMb );
	void setRuntimePageConfig( const t3dRuntimePageConfig_s &rConfig );
	void reset( bool bReleaseCleanMaps );
	void refreshCapacity( void );
	void invalidate( const Vector2i &vLocation );

	bool loadSource( const Vector2i &vLocation, Ref< Terrain3DRegion > &rSource, bool bAllowBlocking = true ) const;
	void releaseUntrackedMaps( void );
	bool requestSourceAsync( const Vector2i &vLocation );
	bool ensureResident( const Vector2i &vLocation, bool bDemandLoad = true, bool bAllowBlocking = true );
	bool ensureSampleArea( const Vector3 &rWorldPosition, int nRadiusTexels, bool bAllowBlocking = true );
	void update( const t3dResidencyFrameState_s &rFrame, bool bForce, int nEffectiveLoadLayers );

	EA_FORCE_INLINE bool isEnabled(
		void ) const {
		return m_bEnabled;
	}
	EA_FORCE_INLINE int getCapacity(
		void ) const {
		return m_nCapacity;
	}
	EA_FORCE_INLINE int getLoadBudgetLayers(
		void ) const {
		return m_nLoadBudgetLayers;
	}
	EA_FORCE_INLINE uint32_t getStallFlagsLastTick(
		void ) const {
		return m_nStallFlagsLastTick;
	}
	EA_FORCE_INLINE bool isResident(
		const Vector2i &vLocation ) const {
		return findSlot( vLocation ) >= 0;
	}
	EA_FORCE_INLINE bool isSelectionDirty(
		void ) const {
		return m_bSelectionDirty;
	}
	EA_FORCE_INLINE void markSelectionDirty(
		void ) {
		m_bSelectionDirty = true;
	}

	size_t					 estimateLayerBytes( void ) const;
	size_t					 getReservedBytes( void ) const;
	size_t					 getMinimumBytes( void ) const;
	size_t					 getHardCapBytes( void ) const;
	t3dCpuRegionCacheStats_t getStats( void ) const;
	Dictionary				 getStatsDictionary( bool bDetailed, int nEffectiveLoadLayers ) const;

private:
	Terrain3DRegionRepository  *m_pRepository = NULL;
	Terrain3DRegionPersistence *m_pPersistence = NULL;
	Terrain3DSamplingStorage   *m_pSamplingStorage = NULL;
	Terrain3DRuntimePageStore  *m_pRuntimePageStore = NULL;
	t3dRuntimePageConfig_s		m_runtimePageConfig;

	int				 m_nLoadBudgetLayers = 4;
	uint64_t		 m_nLoadsSinceReset = 0u;
	int				 m_nPinnedLastTick = 0;
	int				 m_nAsyncRequestsLastTick = 0;
	int				 m_nAsyncReadyLastTick = 0;
	int				 m_nAsyncPendingLastTick = 0;
	int				 m_nAsyncOutstanding = 0;
	uint8_t			 m_szAsyncState[T3D_REGION_SLOTS] = {}; // 0 idle, 1 pending, 2 retry, 3 cancelled/draining
	String			 m_szAsyncPath[T3D_REGION_SLOTS];
	bool			 m_szAsyncCooked[T3D_REGION_SLOTS] = {};
	uint32_t		 m_szAsyncGeneration[T3D_REGION_SLOTS] = {};
	uint32_t		 m_szAsyncRequestGeneration[T3D_REGION_SLOTS] = {};
	uint32_t		 m_szRetryAfter[T3D_REGION_SLOTS] = {};
	uint8_t			 m_szFailureCount[T3D_REGION_SLOTS] = {};
	uint32_t		 m_nRetryTick = 0u;
	void			 markSourceFailed( uint32_t index );
	void			 cancelSourceAsync( int nIndex );
	void			 retireSourceAsync( int nIndex );
	void			 drainCancelledSourceAsync( int nIndex, const Vector2i &vLocation );
	t3dRegionCoord_t m_szLocations[T3D_CPU_CACHE_MAX_LAYERS] = {};
	uint8_t			 m_szRegionMap[T3D_REGION_SLOTS] = {};

	int	 calculateCapacity( void ) const;
	int	 findSlot( const Vector2i &vLocation ) const;
	bool takeSourceAsync( const Vector2i &vLocation, Ref< Terrain3DRegion > &rSource );
	void prewarmSources( const t3dResidencyFrameState_s &rFrame );
	void reapSourceAsync( const t3dResidencyFrameState_s &rFrame );
	bool publishSlot( int iSlot, const Vector2i &vLocation, const Ref< Terrain3DRegion > &rSource );
	bool publishCookedSlot( int					iSlot,
							const Vector2i	   &vLocation,
							const Ref< Image > &rHeight,
							const Ref< Image > &rControl,
							const Ref< Image > &rColor,
							const Ref< Image > &rDerived );
	bool cookedRuntimeEnabled( void ) const;
	bool loadCookedMapsNow( const Vector2i &vLocation,
							Ref< Image >   &rHeight,
							Ref< Image >   &rControl,
							Ref< Image >   &rColor,
							Ref< Image >   &rDerived ) const;
	bool evictSlot( int iSlot );
	bool restoreAllMaps( void );
	int	 collectDesired( const t3dResidencyFrameState_s &rFrame,
						 const Vector2i					&rCenter,
						 Vector2i						*pLocations,
						 int							 nCapacity ) const;
};

#endif // TERRAIN3D_CPU_REGION_CACHE_H
