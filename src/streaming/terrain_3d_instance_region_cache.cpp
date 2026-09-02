// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "streaming/terrain_3d_instance_region_cache.h"

#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_runtime_bake_codec.h"
#include "instances/terrain_3d_instance_cache_policy.h"
#include "streaming/terrain_3d_runtime_page_store.h"
#include "streaming/terrain_3d_runtime_profile.h"

#include <godot_cpp/classes/time.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

class Terrain3DInstanceRegionCacheScopeTimer {
	uint64_t m_nStartedUsec = 0u;
	uint64_t *m_pElapsedUsec = NULL;

public:
	Terrain3DInstanceRegionCacheScopeTimer( uint64_t &nElapsedUsec, const bool bEnabled ) {
		nElapsedUsec = 0u;
		if ( !bEnabled ) {
			return;
		}
		m_pElapsedUsec = &nElapsedUsec;
		Time *const pTime = Time::get_singleton();
		m_nStartedUsec = pTime ? pTime->get_ticks_usec() : 0u;
	}
	~Terrain3DInstanceRegionCacheScopeTimer( void ) {
		if ( !m_pElapsedUsec ) {
			return;
		}
		Time *const pTime = Time::get_singleton();
		const uint64_t finishedUsec = pTime ? pTime->get_ticks_usec() : m_nStartedUsec;
		*m_pElapsedUsec = finishedUsec >= m_nStartedUsec ? finishedUsec - m_nStartedUsec : 0u;
	}
};

typedef bool ( *t3dInstanceRegionRingFn )( void *pContext, const Vector2i &rLocation );

static bool t3dInstanceRegionCacheForEachSquareRing(
        const Vector2i &rCenter, const int nMaxRadius, t3dInstanceRegionRingFn pCallback, void *const pContext ) {
	for ( int radius = 0; radius < nMaxRadius; ++radius ) {
		if ( radius == 0 ) {
			if ( !pCallback( pContext, rCenter ) ) {
				return false;
			}
			continue;
		}
		for ( int x = -radius; x <= radius; ++x ) {
			if ( !pCallback( pContext, rCenter + Vector2i( x, -radius ) ) || !pCallback( pContext, rCenter + Vector2i( x, radius ) ) ) {
				return false;
			}
		}
		for ( int y = -radius + 1; y <= radius - 1; ++y ) {
			if ( !pCallback( pContext, rCenter + Vector2i( -radius, y ) ) || !pCallback( pContext, rCenter + Vector2i( radius, y ) ) ) {
				return false;
			}
		}
	}
	return true;
}

void t3dInstanceRegionCacheChanges_t::recordLoaded( const Vector2i &vLocation ) {
	if ( m_nLoaded < s_nInstanceCacheMaxPages ) {
		m_szLoaded[m_nLoaded++] = vLocation;
	}
}

void t3dInstanceRegionCacheChanges_t::recordEvicted( const Vector2i &vLocation ) {
	if ( m_nEvicted < s_nInstanceCacheMaxPages ) {
		m_szEvicted[m_nEvicted++] = vLocation;
	}
}

Terrain3DInstanceRegionCache::Terrain3DInstanceRegionCache(
        Terrain3DRegionRepository *const pRepository, Terrain3DRuntimePageStore *const pRuntimePageStore ) :
        m_pRepository( pRepository ), m_pRuntimePageStore( pRuntimePageStore ) {
	eastl::fill_n( m_szLocations, s_nInstanceCacheMaxPages, s_v2iMax );
}

void Terrain3DInstanceRegionCache::reset( const bool bRestoreAll, t3dInstanceRegionCacheChanges_t *const pChanges ) {
	if ( bRestoreAll && m_pRepository ) {
		for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
			const Vector2i location = m_pRepository->getSlotsData()[slot];
			Terrain3DRegion *const pRegion = location == s_v2iMax ? NULL : m_pRepository->getRegionPtr( location );
			if ( pRegion && !pRegion->isDeleted() && pRegion->hasInstances() ) {
				if ( pRegion->ensureInstanceStoreResident() && pChanges ) {
					pChanges->recordLoaded( location );
				}
			}
		}
	} else {
		for ( int slot = 0; slot < m_nCapacity; ++slot ) {
			evictSlot( slot, pChanges );
		}
	}
	eastl::fill_n( m_szRegionMap, s_nMaxRegionSlots, ( uint8_t )( 0 ) );
	eastl::fill_n( m_szLocations, s_nInstanceCacheMaxPages, s_v2iMax );
	m_nCapacity = s_nInstanceCacheMaxPages;
	m_nResidentCount = 0;
	m_nDesiredLastTick = 0;
	m_nLoadsLastTick = 0;
	m_nEvictionsLastTick = 0;
	m_nLoadBytesLastTick = 0u;
	m_nResidentBytes = 0u;
	m_center = s_v2iMax;
	m_nPrefetchStreamingGenerationLast = 0u;
	m_bSelectionDirty = true;
}

int Terrain3DInstanceRegionCache::findSlot( const Vector2i &vLocation ) const {
	const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex < 0 ) {
		return -1;
	}
	const int slot = ( int )( m_szRegionMap[mapIndex] ) - 1;
	if ( slot < 0 || slot >= m_nCapacity || m_szLocations[slot] != vLocation ) {
		return -1;
	}
	return slot;
}

void Terrain3DInstanceRegionCache::invalidate( const Vector2i &vLocation, t3dInstanceRegionCacheChanges_t *const pChanges ) {
	const int slot = findSlot( vLocation );
	if ( slot < 0 ) {
		return;
	}
	evictSlot( slot, pChanges );
	m_bSelectionDirty = true;
}

bool Terrain3DInstanceRegionCache::evictSlot( const int iSlot, t3dInstanceRegionCacheChanges_t *const pChanges ) {
	if ( !t3dInstanceCacheIndexValid( iSlot, m_nCapacity ) || !m_pRepository ) {
		return false;
	}
	const Vector2i location = m_szLocations[iSlot];
	if ( location == s_v2iMax ) {
		return true;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
	size_t residentBytes = 0u;
	if ( pRegion && pRegion->isInstanceStoreResident() ) {
		residentBytes = pRegion->getInstanceResidentBytes();
		const PackedByteArray page = pRegion->getInstancePage();
		if ( page.is_empty() && pRegion->hasInstances() ) {
			return false;
		}
	}
	if ( pRegion && pRegion->isInstanceStoreResident() ) {
		if ( !pRegion->evictInstanceStore() ) {
			return false;
		}
		m_nResidentBytes = residentBytes <= m_nResidentBytes ? m_nResidentBytes - residentBytes : 0u;
	}
	const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( location );
	if ( mapIndex >= 0 ) {
		m_szRegionMap[mapIndex] = 0u;
	}
	m_szLocations[iSlot] = s_v2iMax;
	m_nResidentCount = MAX( 0, m_nResidentCount - 1 );
	if ( pChanges ) {
		pChanges->recordEvicted( location );
	}
	return true;
}

bool Terrain3DInstanceRegionCache::evict( const Vector2i &vLocation, t3dInstanceRegionCacheChanges_t *const pChanges ) {
	const int slot = findSlot( vLocation );
	if ( slot >= 0 ) {
		return evictSlot( slot, pChanges );
	}
	Terrain3DRegion *const pRegion = m_pRepository ? m_pRepository->getRegionPtr( vLocation ) : NULL;
	if ( !pRegion || !pRegion->isInstanceStoreResident() ) {
		return true;
	}
	const bool bEvicted = pRegion->evictInstanceStore();
	if ( bEvicted && pChanges ) {
		pChanges->recordEvicted( vLocation );
	}
	return bEvicted;
}

bool Terrain3DInstanceRegionCache::ensureResident(
        const Vector2i &vLocation, const bool bDemandLoad, t3dInstanceRegionCacheChanges_t *const pChanges ) {
	Terrain3DRegion *const pRegion = m_pRepository ? m_pRepository->getRegionPtr( vLocation ) : NULL;
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( !m_bEnabled ) {
		const bool bReady = pRegion->ensureInstanceStoreResident();
		if ( bReady && pChanges ) {
			pChanges->recordLoaded( vLocation );
		}
		return bReady;
	}
	if ( findSlot( vLocation ) >= 0 ) {
		return true;
	}
	if ( !pRegion->hasInstances() ) {
		return true;
	}
	int targetSlot = -1;
	for ( int slot = 0; slot < m_nCapacity; ++slot ) {
		if ( m_szLocations[slot] == s_v2iMax ) {
			targetSlot = slot;
			break;
		}
	}
	if ( targetSlot < 0 ) {
		int farthestDistance = -1;
		const Vector2i center = m_center == s_v2iMax ? vLocation : m_center;
		for ( int slot = 0; slot < m_nCapacity; ++slot ) {
			const Vector2i oldLocation = m_szLocations[slot];
			const int distance = t3dInstanceCacheDistance( { oldLocation.x, oldLocation.y }, { center.x, center.y } );
			if ( distance > farthestDistance ) {
				farthestDistance = distance;
				targetSlot = slot;
			}
		}
	}
	if ( targetSlot < 0 ) {
		return false;
	}
	if ( m_szLocations[targetSlot] != s_v2iMax && !evictSlot( targetSlot, pChanges ) ) {
		return false;
	}
	if ( !pRegion->ensureInstanceStoreResident() ) {
		return false;
	}
	const size_t bytes = pRegion->getInstanceResidentBytes();
	const size_t budget = ( size_t )( m_nBudgetMb )*s_nInstanceCacheMib;
	while ( bDemandLoad && m_nResidentCount > 0 && m_nResidentBytes + bytes > budget ) {
		int farthestSlot = -1;
		int farthestDistance = -1;
		const Vector2i center = m_center == s_v2iMax ? vLocation : m_center;
		for ( int slot = 0; slot < m_nCapacity; ++slot ) {
			const Vector2i oldLocation = m_szLocations[slot];
			if ( oldLocation == s_v2iMax || oldLocation == vLocation ) {
				continue;
			}
			const int distance = t3dInstanceCacheDistance( { oldLocation.x, oldLocation.y }, { center.x, center.y } );
			if ( distance > farthestDistance ) {
				farthestDistance = distance;
				farthestSlot = slot;
			}
		}
		if ( farthestSlot < 0 || !evictSlot( farthestSlot, pChanges ) ) {
			break;
		}
		++m_nEvictionsLastTick;
	}
	if ( !bDemandLoad && !t3dInstanceCacheCanStream( m_nResidentBytes, bytes, budget, m_nResidentCount ) ) {
		pRegion->evictInstanceStore();
		return false;
	}
	m_szLocations[targetSlot] = vLocation;
	const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex >= 0 ) {
		m_szRegionMap[mapIndex] = ( uint8_t )( targetSlot + 1 );
	}
	++m_nResidentCount;
	m_nResidentBytes += bytes;
	if ( bDemandLoad ) {
		++m_nDemandLoads;
	}
	if ( pChanges ) {
		pChanges->recordLoaded( vLocation );
	}
	return true;
}

typedef struct t3dInstanceCollectDesiredContext_s {
	const Terrain3DRegionRepository *m_pRepository;
	const t3dResidencyFrameState_t *m_pFrame;
	Vector2i *m_pLocations;
	t3dRegionStreamingState_t m_eWanted;
	int m_nCapacity;
	int *m_pOutputCount;
} t3dInstanceCollectDesiredContext_t;

static bool t3dInstanceCollectDesiredLocation( void *const pContext, const Vector2i &rLocation ) {
	t3dInstanceCollectDesiredContext_t *const pState = ( t3dInstanceCollectDesiredContext_t * )( pContext );
	if ( *pState->m_pOutputCount >= pState->m_nCapacity ) {
		return false;
	}
	if ( !pState->m_pRepository->isLocationValid( rLocation ) || !pState->m_pRepository->getRegionPtr( rLocation )
	        || pState->m_pFrame->streamingState( rLocation ) != pState->m_eWanted ) {
		return true;
	}
	const Terrain3DRegion *const pRegion = pState->m_pRepository->getRegionPtr( rLocation );
	if ( pRegion && pRegion->hasInstances() ) {
		pState->m_pLocations[( *pState->m_pOutputCount )++] = rLocation;
	}
	return *pState->m_pOutputCount < pState->m_nCapacity;
}

int Terrain3DInstanceRegionCache::collectDesired(
        const t3dResidencyFrameState_t &rFrame, const Vector2i &rCenter, Vector2i *const pLocations, const int nCapacity ) const {
	if ( !pLocations || nCapacity <= 0 || !m_pRepository ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, s_nInstanceCacheMaxPages );
	int outputCount = 0;
	for ( int stateValue = ( int )( TERRAIN3D_REGION_STREAMING_ACTIVE );
	        stateValue >= ( int )( TERRAIN3D_REGION_STREAMING_HOT ) && outputCount < capacity;
	        --stateValue ) {
		const t3dRegionStreamingState_t wanted = ( t3dRegionStreamingState_t )( stateValue );
		t3dInstanceCollectDesiredContext_t context;
		context.m_pRepository = m_pRepository;
		context.m_pFrame = &rFrame;
		context.m_pLocations = pLocations;
		context.m_eWanted = wanted;
		context.m_nCapacity = capacity;
		context.m_pOutputCount = &outputCount;
		t3dInstanceRegionCacheForEachSquareRing( rCenter, m_pRepository->getRegionMapSize(), t3dInstanceCollectDesiredLocation, &context );
	}
	return outputCount;
}

void Terrain3DInstanceRegionCache::configure( const bool bEnabled,
        const int nBudgetMb,
        const int nLoadPages,
        const int nLoadBudgetMb,
        t3dInstanceRegionCacheChanges_t *const pChanges ) {
	const bool bWasEnabled = m_bEnabled;
	m_bEnabled = bEnabled;
	m_nBudgetMb = CLAMP( nBudgetMb, s_nInstanceRegionCacheMinBudgetMb, s_nInstanceRegionCacheMaxBudgetMb );
	m_nLoadBudgetPages = CLAMP( nLoadPages, 1, s_nInstanceRegionLoadMaxPages );
	m_nLoadBudgetMb = CLAMP( nLoadBudgetMb, 1, s_nInstanceRegionLoadMaxBudgetMb );
	if ( bWasEnabled && !bEnabled ) {
		reset( true, pChanges );
		return;
	}
	if ( !bWasEnabled && bEnabled ) {
		reset( false, pChanges );
	}
	m_bSelectionDirty = true;
}

void Terrain3DInstanceRegionCache::update( const t3dResidencyFrameState_t &rFrame,
        const bool bForce,
        const int nEffectiveLoadPages,
        t3dInstanceRegionCacheChanges_t &rChanges ) {
	const Terrain3DInstanceRegionCacheScopeTimer serviceTimer( m_nServiceUsecLast, rFrame.m_bProfilingEnabled );
	m_nLoadsLastTick = 0;
	m_nStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_nEvictionsLastTick = 0;
	m_nLoadBytesLastTick = 0u;
	if ( !m_bEnabled || !m_pRepository || m_pRepository->getRegionSize() <= 0 || m_pRepository->getVertexSpacing() <= 0.0f ) {
		return;
	}
	const bool asyncRuntime = !rFrame.m_bAuthoring && rFrame.m_bCookedPagesEnabled && m_pRuntimePageStore;
	const uint64_t streamingGeneration = rFrame.m_nStreamingGeneration;
	if ( asyncRuntime && ( bForce || streamingGeneration != m_nPrefetchStreamingGenerationLast ) ) {
		for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
			const Vector2i location = m_pRepository->getSlotsData()[slot];
			Terrain3DRegion *const pRegion = location == s_v2iMax ? NULL : m_pRepository->getRegionPtr( location );
			if ( !pRegion || pRegion->isDeleted() || !pRegion->hasInstances() || pRegion->isInstanceStoreResident()
			        || findSlot( location ) >= 0 || !rFrame.isPrefetch( location ) ) {
				continue;
			}
			m_pRuntimePageStore->request( location, RUNTIME_PAGE_CONSUMER_INSTANCE_STORE, 3u, true );
		}
		m_nPrefetchStreamingGenerationLast = streamingGeneration;
	}
	const Vector2i center = rFrame.m_residencyCenter;
	if ( !bForce && center == m_center && !m_bSelectionDirty ) {
		return;
	}
	Vector2i desired[s_nInstanceCacheMaxPages];
	const int desiredCount = collectDesired( rFrame, center, desired, m_nCapacity );
	m_nDesiredLastTick = desiredCount;
	uint8_t desiredMap[s_nMaxRegionSlots] = {};
	for ( int index = 0; index < desiredCount; ++index ) {
		const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( desired[index] );
		if ( mapIndex >= 0 ) {
			desiredMap[mapIndex] = 1u;
		}
	}
	const size_t residentBudget = ( size_t )( m_nBudgetMb )*s_nInstanceCacheMib;
	while ( m_nResidentCount > 1 && m_nResidentBytes > residentBudget ) {
		int farthestSlot = -1;
		int farthestDistance = -1;
		for ( int slot = 0; slot < m_nCapacity; ++slot ) {
			const Vector2i location = m_szLocations[slot];
			if ( location == s_v2iMax ) {
				continue;
			}
			const int distance = t3dInstanceCacheDistance( { location.x, location.y }, { center.x, center.y } );
			if ( distance > farthestDistance ) {
				farthestDistance = distance;
				farthestSlot = slot;
			}
		}
		if ( farthestSlot < 0 || !evictSlot( farthestSlot, &rChanges ) ) {
			break;
		}
		++m_nEvictionsLastTick;
	}
	for ( int slot = 0; slot < m_nCapacity; ++slot ) {
		const Vector2i location = m_szLocations[slot];
		const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( location != s_v2iMax && ( mapIndex < 0 || desiredMap[mapIndex] == 0u ) ) {
			if ( evictSlot( slot, &rChanges ) ) {
				++m_nEvictionsLastTick;
			}
		}
	}
	const size_t loadBudget = ( size_t )( m_nLoadBudgetMb )*s_nInstanceCacheMib;
	const int loadPageBudget = CLAMP( nEffectiveLoadPages, 0, m_nLoadBudgetPages );
	bool bComplete = true;
	for ( int index = 0; index < desiredCount; ++index ) {
		const Vector2i location = desired[index];
		if ( findSlot( location ) >= 0 ) {
			continue;
		}
		bComplete = false;
		if ( !bForce && m_nLoadsLastTick >= loadPageBudget ) {
			m_nStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BUDGET;
			break;
		}
		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		const size_t estimated = pRegion ? MAX<size_t>( pRegion->getInstancePageBytes(), 1u ) : 0u;
		if ( !bForce && m_nLoadsLastTick > 0 && m_nLoadBytesLastTick + estimated > loadBudget ) {
			m_nStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BYTE_BUDGET;
			break;
		}
		if ( asyncRuntime && pRegion && !pRegion->isInstanceStoreResident() ) {
			const uint8_t priority = rFrame.streamingState( location ) == TERRAIN3D_REGION_STREAMING_ACTIVE ? 1u : 2u;
			m_pRuntimePageStore->request( location, RUNTIME_PAGE_CONSUMER_INSTANCE_STORE, priority, true );
			Terrain3DInstanceStore preparedStore;
			PackedByteArray instance_metadata;
			PackedByteArray multimesh_fast_start;
			if ( !m_pRuntimePageStore->takeInstance( location, preparedStore, &instance_metadata, &multimesh_fast_start ) ) {
				continue;
			}
			bool acceleration_valid = true;
			if ( !instance_metadata.is_empty() ) {
				t3dInstanceMetadataView_t view;
				acceleration_valid = Terrain3DRuntimeBakeCodec::getInstanceMetadataView( instance_metadata, view );
				acceleration_valid = acceleration_valid && view.m_nInstanceCount == preparedStore.instanceCount();
			}
			if ( !acceleration_valid ) {
				instance_metadata.clear();
				multimesh_fast_start.clear();
			}
			pRegion->adoptPreparedInstanceStoreInternal( eastl::move( preparedStore ) );
			pRegion->adoptCookedInstanceAccelerationInternal( instance_metadata, multimesh_fast_start );
		}
		if ( ensureResident( location, false, &rChanges ) ) {
			++m_nLoadsLastTick;
			m_nLoadBytesLastTick += estimated;
		}
	}
	if ( !bComplete ) {
		bComplete = true;
		for ( int index = 0; index < desiredCount; ++index ) {
			if ( findSlot( desired[index] ) < 0 ) {
				bComplete = false;
				break;
			}
		}
	}
	m_center = center;
	m_bSelectionDirty = !bComplete;
}

t3dInstanceRegionCacheStats_t Terrain3DInstanceRegionCache::getStats( void ) const {
	t3dInstanceRegionCacheStats_t stats;
	stats.m_bEnabled = m_bEnabled;
	stats.m_bSelectionPending = m_bSelectionDirty;
	stats.m_nCapacity = m_nCapacity;
	stats.m_nResident = m_nResidentCount;
	stats.m_nDesiredLastTick = m_nDesiredLastTick;
	stats.m_nLoadsLastTick = m_nLoadsLastTick;
	stats.m_nEvictionsLastTick = m_nEvictionsLastTick;
	stats.m_nStallFlagsLastTick = m_nStallFlagsLastTick;
	stats.m_nResidentBytes = m_nResidentBytes;
	stats.m_nLoadBytesLastTick = m_nLoadBytesLastTick;
	stats.m_nServiceUsecLast = m_nServiceUsecLast;
	return stats;
}

Dictionary Terrain3DInstanceRegionCache::getStatsDictionary( const bool bDetailed, const int nEffectiveLoadPages ) const {
	Dictionary stats;
	stats["enabled"] = m_bEnabled;
	stats["budget_mb"] = m_nBudgetMb;
	stats["capacity"] = m_nCapacity;
	stats["resident"] = m_nResidentCount;
	stats["resident_bytes"] = ( int64_t )( m_nResidentBytes );
	stats["desired_last_tick"] = m_nDesiredLastTick;
	stats["loads_last_tick"] = m_nLoadsLastTick;
	stats["evictions_last_tick"] = m_nEvictionsLastTick;
	stats["load_bytes_last_tick"] = ( int64_t )( m_nLoadBytesLastTick );
	stats["service_cpu_usec_last"] = ( int64_t )( m_nServiceUsecLast );
	stats["demand_loads"] = m_nDemandLoads;
	stats["selection_pending"] = m_bSelectionDirty;
	if ( !bDetailed ) {
		return stats;
	}
	stats["load_page_budget"] = m_nLoadBudgetPages;
	stats["effective_load_page_budget"] = nEffectiveLoadPages;
	stats["load_budget_mb"] = m_nLoadBudgetMb;
	stats["stall_flags_last_tick"] = ( int64_t )( m_nStallFlagsLastTick );
	stats["center"] = m_center;
	stats["budget_overcommitted"] = m_nResidentBytes > ( size_t )( m_nBudgetMb )*s_nInstanceCacheMib;
	return stats;
}
