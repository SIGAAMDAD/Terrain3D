// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "streaming/terrain_3d_instance_region_cache.h"

#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_runtime_bake_codec.h"
#include "instances/terrain_3d_instance_cache_policy.h"
#include "streaming/terrain_3d_runtime_page_store.h"
#include "streaming/terrain_3d_runtime_profile.h"

#include <godot_cpp/classes/time.hpp>

#include <algorithm>

class T3DInstanceRegionCacheScopeTimer {
	uint64_t m_startedUsec = 0u;
	uint64_t *m_pElapsedUsec = nullptr;

public:
	T3DInstanceRegionCacheScopeTimer( uint64_t &nElapsedUsec, const bool bEnabled ) {
		nElapsedUsec = 0u;
		if ( !bEnabled ) {
			return;
		}
		m_pElapsedUsec = &nElapsedUsec;
		Time *const pTime = Time::get_singleton();
		m_startedUsec = pTime ? pTime->get_ticks_usec() : 0u;
	}
	~T3DInstanceRegionCacheScopeTimer( void ) {
		if ( !m_pElapsedUsec ) {
			return;
		}
		Time *const pTime = Time::get_singleton();
		const uint64_t finishedUsec = pTime ? pTime->get_ticks_usec() : m_startedUsec;
		*m_pElapsedUsec = finishedUsec >= m_startedUsec ? finishedUsec - m_startedUsec : 0u;
	}
};

template <typename Callback>
static bool t3dInstanceRegionCacheForEachSquareRing( const Vector2i &rCenter, const int nMaxRadius, Callback &&rCallback ) {
	for ( int radius = 0; radius < nMaxRadius; ++radius ) {
		if ( radius == 0 ) {
			if ( !rCallback( rCenter ) ) {
				return false;
			}
			continue;
		}
		for ( int x = -radius; x <= radius; ++x ) {
			if ( !rCallback( rCenter + Vector2i( x, -radius ) ) ||
				!rCallback( rCenter + Vector2i( x, radius ) ) ) {
				return false;
			}
		}
		for ( int y = -radius + 1; y <= radius - 1; ++y ) {
			if ( !rCallback( rCenter + Vector2i( -radius, y ) ) ||
				!rCallback( rCenter + Vector2i( radius, y ) ) ) {
				return false;
			}
		}
	}
	return true;
}

void T3DInstanceRegionCacheChanges::recordLoaded( const Vector2i &vLocation ) {
	if ( nLoaded < TERRAIN3D_INSTANCE_CACHE_MAX_PAGES ) {
		loaded[nLoaded++] = vLocation;
	}
}

void T3DInstanceRegionCacheChanges::recordEvicted( const Vector2i &vLocation ) {
	if ( nEvicted < TERRAIN3D_INSTANCE_CACHE_MAX_PAGES ) {
		evicted[nEvicted++] = vLocation;
	}
}

T3DInstanceRegionCache::T3DInstanceRegionCache(
	T3DRegionRepository *const pRepository,
	T3DRuntimePageStore *const pRuntimePageStore
) :
		m_pRepository( pRepository ),
		m_pRuntimePageStore( pRuntimePageStore ) {
	std::fill_n( m_szLocations, TERRAIN3D_INSTANCE_CACHE_MAX_PAGES, V2I_MAX );
}

void T3DInstanceRegionCache::reset(
	const bool bRestoreAll, T3DInstanceRegionCacheChanges *const pChanges
) {
	if ( bRestoreAll && m_pRepository ) {
		for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
			const Vector2i location = m_pRepository->getSlotsData()[slot];
			T3DRegion *const pRegion = location == V2I_MAX ? nullptr : m_pRepository->getRegionPtr( location );
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
	std::fill_n( m_szRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint8_t ) ( 0 ) );
	std::fill_n( m_szLocations, TERRAIN3D_INSTANCE_CACHE_MAX_PAGES, V2I_MAX );
	m_nCapacity = TERRAIN3D_INSTANCE_CACHE_MAX_PAGES;
	m_nResidentCount = 0;
	m_nDesiredLastTick = 0;
	m_nLoadsLastTick = 0;
	m_nEvictionsLastTick = 0;
	m_nLoadBytesLastTick = 0u;
	m_nResidentBytes = 0u;
	m_center = V2I_MAX;
	m_nPrefetchStreamingGenerationLast = 0u;
	m_bSelectionDirty = true;
}

int T3DInstanceRegionCache::findSlot( const Vector2i &vLocation ) const {
	const int mapIndex = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex < 0 ) {
		return -1;
	}
	const int slot = ( int ) ( m_szRegionMap[mapIndex] ) - 1;
	if ( slot < 0 || slot >= m_nCapacity || m_szLocations[slot] != vLocation ) {
		return -1;
	}
	return slot;
}

void T3DInstanceRegionCache::invalidate(
	const Vector2i &vLocation, T3DInstanceRegionCacheChanges *const pChanges
) {
	const int slot = findSlot( vLocation );
	if ( slot < 0 ) {
		return;
	}
	evictSlot( slot, pChanges );
	m_bSelectionDirty = true;
}

bool T3DInstanceRegionCache::evictSlot(
	const int iSlot, T3DInstanceRegionCacheChanges *const pChanges
) {
	if ( !terrain3dInstanceCacheIndexValid( iSlot, m_nCapacity ) || !m_pRepository ) {
		return false;
	}
	const Vector2i location = m_szLocations[iSlot];
	if ( location == V2I_MAX ) {
		return true;
	}
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
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
	const int mapIndex = T3DRegionRepository::getRegionMapIndex( location );
	if ( mapIndex >= 0 ) {
		m_szRegionMap[mapIndex] = 0u;
	}
	m_szLocations[iSlot] = V2I_MAX;
	m_nResidentCount = MAX( 0, m_nResidentCount - 1 );
	if ( pChanges ) {
		pChanges->recordEvicted( location );
	}
	return true;
}

bool T3DInstanceRegionCache::evict(
	const Vector2i &vLocation, T3DInstanceRegionCacheChanges *const pChanges
) {
	const int slot = findSlot( vLocation );
	if ( slot >= 0 ) {
		return evictSlot( slot, pChanges );
	}
	T3DRegion *const pRegion = m_pRepository ? m_pRepository->getRegionPtr( vLocation ) : nullptr;
	if ( !pRegion || !pRegion->isInstanceStoreResident() ) {
		return true;
	}
	const bool bEvicted = pRegion->evictInstanceStore();
	if ( bEvicted && pChanges ) {
		pChanges->recordEvicted( vLocation );
	}
	return bEvicted;
}

bool T3DInstanceRegionCache::ensureResident(
	const Vector2i &vLocation, const bool bDemandLoad,
	T3DInstanceRegionCacheChanges *const pChanges
) {
	T3DRegion *const pRegion = m_pRepository ? m_pRepository->getRegionPtr( vLocation ) : nullptr;
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
		if ( m_szLocations[slot] == V2I_MAX ) {
			targetSlot = slot;
			break;
		}
	}
	if ( targetSlot < 0 ) {
		int farthestDistance = -1;
		const Vector2i center = m_center == V2I_MAX ? vLocation : m_center;
		for ( int slot = 0; slot < m_nCapacity; ++slot ) {
			const Vector2i oldLocation = m_szLocations[slot];
			const int distance = terrain3dInstanceCacheDistance(
				{oldLocation.x, oldLocation.y}, {center.x, center.y} );
			if ( distance > farthestDistance ) {
				farthestDistance = distance;
				targetSlot = slot;
			}
		}
	}
	if ( targetSlot < 0 ) {
		return false;
	}
	if ( m_szLocations[targetSlot] != V2I_MAX && !evictSlot( targetSlot, pChanges ) ) {
		return false;
	}
	if ( !pRegion->ensureInstanceStoreResident() ) {
		return false;
	}
	const size_t bytes = pRegion->getInstanceResidentBytes();
	const size_t budget = ( size_t ) ( m_nBudgetMb ) * TERRAIN3D_INSTANCE_CACHE_MIB;
	while ( bDemandLoad && m_nResidentCount > 0 && m_nResidentBytes + bytes > budget ) {
		int farthestSlot = -1;
		int farthestDistance = -1;
		const Vector2i center = m_center == V2I_MAX ? vLocation : m_center;
		for ( int slot = 0; slot < m_nCapacity; ++slot ) {
			const Vector2i oldLocation = m_szLocations[slot];
			if ( oldLocation == V2I_MAX || oldLocation == vLocation ) {
				continue;
			}
			const int distance = terrain3dInstanceCacheDistance(
				{oldLocation.x, oldLocation.y}, {center.x, center.y} );
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
	if ( !bDemandLoad && !terrain3dInstanceCacheCanStream(
		m_nResidentBytes, bytes, budget, m_nResidentCount ) ) {
		pRegion->evictInstanceStore();
		return false;
	}
	m_szLocations[targetSlot] = vLocation;
	const int mapIndex = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex >= 0 ) {
		m_szRegionMap[mapIndex] = ( uint8_t ) ( targetSlot + 1 );
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

int T3DInstanceRegionCache::collectDesired(
	const T3DResidencyFrameState &rFrame, const Vector2i &rCenter,
	Vector2i *const pLocations, const int nCapacity
) const {
	if ( !pLocations || nCapacity <= 0 || !m_pRepository ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, TERRAIN3D_INSTANCE_CACHE_MAX_PAGES );
	int outputCount = 0;
	for ( int stateValue = ( int ) ( TERRAIN3D_REGION_STREAMING_ACTIVE );
		stateValue >= ( int ) ( TERRAIN3D_REGION_STREAMING_HOT ) && outputCount < capacity; --stateValue ) {
		const t3dRegionStreamingState_t wanted = ( t3dRegionStreamingState_t ) ( stateValue );
		t3dInstanceRegionCacheForEachSquareRing( rCenter, m_pRepository->getRegionMapSize(), [&]( const Vector2i &location ) {
			if ( outputCount >= capacity ) {
				return false;
			}
			if ( !m_pRepository->isLocationValid( location ) || !m_pRepository->getRegionPtr( location ) ||
				rFrame.streamingState( location ) != wanted ) {
				return true;
			}
			const T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
			if ( pRegion && pRegion->hasInstances() ) {
				pLocations[outputCount++] = location;
			}
			return outputCount < capacity;
		} );
	}
	return outputCount;
}

void T3DInstanceRegionCache::configure(
	const bool bEnabled, const int nBudgetMb,
	const int nLoadPages, const int nLoadBudgetMb,
	T3DInstanceRegionCacheChanges *const pChanges
) {
	const bool bWasEnabled = m_bEnabled;
	m_bEnabled = bEnabled;
	m_nBudgetMb = CLAMP( nBudgetMb, TERRAIN3D_INSTANCE_REGION_CACHE_MIN_BUDGET_MB,
		TERRAIN3D_INSTANCE_REGION_CACHE_MAX_BUDGET_MB );
	m_nLoadBudgetPages = CLAMP( nLoadPages, 1, TERRAIN3D_INSTANCE_REGION_LOAD_MAX_PAGES );
	m_nLoadBudgetMb = CLAMP( nLoadBudgetMb, 1, TERRAIN3D_INSTANCE_REGION_LOAD_MAX_BUDGET_MB );
	if ( bWasEnabled && !bEnabled ) {
		reset( true, pChanges );
		return;
	}
	if ( !bWasEnabled && bEnabled ) {
		reset( false, pChanges );
	}
	m_bSelectionDirty = true;
}

void T3DInstanceRegionCache::update(
	const T3DResidencyFrameState &rFrame, const bool bForce,
	const int nEffectiveLoadPages, T3DInstanceRegionCacheChanges &rChanges
) {
	const T3DInstanceRegionCacheScopeTimer serviceTimer( m_nServiceUsecLast, rFrame.bProfilingEnabled );
	m_nLoadsLastTick = 0;
	m_nStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_nEvictionsLastTick = 0;
	m_nLoadBytesLastTick = 0u;
	if ( !m_bEnabled || !m_pRepository || m_pRepository->getRegionSize() <= 0 ||
		m_pRepository->getVertexSpacing() <= 0.0f ) {
		return;
	}
	const bool asyncRuntime = !rFrame.bAuthoring && rFrame.bCookedPagesEnabled && m_pRuntimePageStore;
	const uint64_t streamingGeneration = rFrame.streamingGeneration;
	if ( asyncRuntime && ( bForce || streamingGeneration != m_nPrefetchStreamingGenerationLast ) ) {
		for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
			const Vector2i location = m_pRepository->getSlotsData()[slot];
			T3DRegion *const pRegion = location == V2I_MAX ? nullptr : m_pRepository->getRegionPtr( location );
			if ( !pRegion || pRegion->isDeleted() || !pRegion->hasInstances() ||
				pRegion->isInstanceStoreResident() || findSlot( location ) >= 0 || !rFrame.isPrefetch( location ) ) {
				continue;
			}
			m_pRuntimePageStore->request( location, RUNTIME_PAGE_CONSUMER_INSTANCE_STORE, 3u, true );
		}
		m_nPrefetchStreamingGenerationLast = streamingGeneration;
	}
	const Vector2i center = rFrame.residencyCenter;
	if ( !bForce && center == m_center && !m_bSelectionDirty ) {
		return;
	}
	Vector2i desired[TERRAIN3D_INSTANCE_CACHE_MAX_PAGES];
	const int desiredCount = collectDesired( rFrame, center, desired, m_nCapacity );
	m_nDesiredLastTick = desiredCount;
	uint8_t desiredMap[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int index = 0; index < desiredCount; ++index ) {
		const int mapIndex = T3DRegionRepository::getRegionMapIndex( desired[index] );
		if ( mapIndex >= 0 ) {
			desiredMap[mapIndex] = 1u;
		}
	}
	const size_t residentBudget = ( size_t ) ( m_nBudgetMb ) * TERRAIN3D_INSTANCE_CACHE_MIB;
	while ( m_nResidentCount > 1 && m_nResidentBytes > residentBudget ) {
		int farthestSlot = -1;
		int farthestDistance = -1;
		for ( int slot = 0; slot < m_nCapacity; ++slot ) {
			const Vector2i location = m_szLocations[slot];
			if ( location == V2I_MAX ) {
				continue;
			}
			const int distance = terrain3dInstanceCacheDistance(
				{location.x, location.y}, {center.x, center.y} );
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
		const int mapIndex = T3DRegionRepository::getRegionMapIndex( location );
		if ( location != V2I_MAX && ( mapIndex < 0 || desiredMap[mapIndex] == 0u ) ) {
			if ( evictSlot( slot, &rChanges ) ) {
				++m_nEvictionsLastTick;
			}
		}
	}
	const size_t loadBudget = ( size_t ) ( m_nLoadBudgetMb ) * TERRAIN3D_INSTANCE_CACHE_MIB;
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
		T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		const size_t estimated = pRegion ? MAX<size_t>( pRegion->getInstancePageBytes(), 1u ) : 0u;
		if ( !bForce && m_nLoadsLastTick > 0 && m_nLoadBytesLastTick + estimated > loadBudget ) {
			m_nStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BYTE_BUDGET;
			break;
		}
		if ( asyncRuntime && pRegion && !pRegion->isInstanceStoreResident() ) {
			const uint8_t priority = rFrame.streamingState( location ) ==
				TERRAIN3D_REGION_STREAMING_ACTIVE ? 1u : 2u;
			m_pRuntimePageStore->request( location, RUNTIME_PAGE_CONSUMER_INSTANCE_STORE, priority, true );
			T3DInstanceStore preparedStore;
			PackedByteArray instance_metadata;
			PackedByteArray multimesh_fast_start;
			if ( !m_pRuntimePageStore->takeInstance( location, preparedStore,
				&instance_metadata, &multimesh_fast_start ) ) {
				continue;
			}
			bool acceleration_valid = true;
			if ( !instance_metadata.is_empty() ) {
				T3DInstanceMetadataView view; acceleration_valid = T3DRuntimeBakeCodec::getInstanceMetadataView( instance_metadata, view );
				acceleration_valid = acceleration_valid && view.nInstanceCount == preparedStore.instanceCount();
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

T3DInstanceRegionCacheStats T3DInstanceRegionCache::getStats( void ) const {
	T3DInstanceRegionCacheStats stats;
	stats.bEnabled = m_bEnabled;
	stats.bSelectionPending = m_bSelectionDirty;
	stats.nCapacity = m_nCapacity;
	stats.nResident = m_nResidentCount;
	stats.nDesiredLastTick = m_nDesiredLastTick;
	stats.nLoadsLastTick = m_nLoadsLastTick;
	stats.nEvictionsLastTick = m_nEvictionsLastTick;
	stats.nStallFlagsLastTick = m_nStallFlagsLastTick;
	stats.nResidentBytes = m_nResidentBytes;
	stats.nLoadBytesLastTick = m_nLoadBytesLastTick;
	stats.nServiceUsecLast = m_nServiceUsecLast;
	return stats;
}

Dictionary T3DInstanceRegionCache::getStatsDictionary(
	const bool bDetailed, const int nEffectiveLoadPages
) const {
	Dictionary stats;
	stats["enabled"] = m_bEnabled;
	stats["budget_mb"] = m_nBudgetMb;
	stats["capacity"] = m_nCapacity;
	stats["resident"] = m_nResidentCount;
	stats["resident_bytes"] = ( int64_t ) ( m_nResidentBytes );
	stats["desired_last_tick"] = m_nDesiredLastTick;
	stats["loads_last_tick"] = m_nLoadsLastTick;
	stats["evictions_last_tick"] = m_nEvictionsLastTick;
	stats["load_bytes_last_tick"] = ( int64_t ) ( m_nLoadBytesLastTick );
	stats["service_cpu_usec_last"] = ( int64_t ) ( m_nServiceUsecLast );
	stats["demand_loads"] = m_nDemandLoads;
	stats["selection_pending"] = m_bSelectionDirty;
	if ( !bDetailed ) {
		return stats;
	}
	stats["load_page_budget"] = m_nLoadBudgetPages;
	stats["effective_load_page_budget"] = nEffectiveLoadPages;
	stats["load_budget_mb"] = m_nLoadBudgetMb;
	stats["stall_flags_last_tick"] = ( int64_t ) ( m_nStallFlagsLastTick );
	stats["center"] = m_center;
	stats["budget_overcommitted"] = m_nResidentBytes >
		( size_t ) ( m_nBudgetMb ) * TERRAIN3D_INSTANCE_CACHE_MIB;
	return stats;
}
