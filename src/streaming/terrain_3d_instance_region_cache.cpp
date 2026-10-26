// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "streaming/terrain_3d_region_residency_primitives.h"
#include "streaming/terrain_3d_instance_region_cache.h"
#include "streaming/terrain_3d_region_candidate_collector.h"

#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_runtime_bake_codec.h"
#include "instances/terrain_3d_instance_cache_policy.h"
#include "memory/terrain_3d_memory.h"
#include "streaming/terrain_3d_runtime_page_store.h"
#include "streaming/terrain_3d_runtime_profile.h"
#include "streaming/terrain_3d_scope_timer.h"

#include <godot_cpp/classes/time.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>



void t3dInstanceRegionCacheChanges_t::recordLoaded( const Vector2i &vLocation ) {
	if ( m_nLoaded < s_nInstanceCacheMaxPages ) {
		m_szLoaded[m_nLoaded++] = t3dPackRegionCoord( vLocation );
	}
}

void t3dInstanceRegionCacheChanges_t::recordEvicted( const Vector2i &vLocation ) {
	if ( m_nEvicted < s_nInstanceCacheMaxPages ) {
		m_szEvicted[m_nEvicted++] = t3dPackRegionCoord( vLocation );
	}
}

Terrain3DInstanceRegionCache::Terrain3DInstanceRegionCache(
        Terrain3DRegionRepository *const pRepository, Terrain3DRuntimePageStore *const pRuntimePageStore ) :
        m_pRepository( pRepository ), m_pRuntimePageStore( pRuntimePageStore ) {
	eastl::fill_n( m_szLocations, s_nInstanceCacheMaxPages, t3dInvalidRegionCoord() );
	eastl::fill_n( m_szResidentBytes, s_nInstanceCacheMaxPages, ( size_t )( 0u ) );
}

void Terrain3DInstanceRegionCache::reset( const bool bRestoreAll, t3dInstanceRegionCacheChanges_t *const pChanges ) {
	if ( bRestoreAll && m_pRepository ) {
		for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
			const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
			Terrain3DRegion *const pRegion = location == s_v2iMax ? NULL : m_pRepository->getRegionPtr( location );
			if ( pRegion && !pRegion->isDeleted() && pRegion->hasInstances() ) {
				if ( pRegion->ensureInstanceStoreResident() && pChanges ) {
					pChanges->recordLoaded( location );
				}
			}
		}
	} else {
		for ( int slot = 0; slot < m_nCapacity; ++slot ) {
			evictSlot( slot, pChanges, TERRAIN3D_INSTANCE_REGION_EVICTION_RESET, s_v2iMax );
		}
	}
	eastl::fill_n( m_szRegionMap, s_nRegionSlots, ( uint8_t )( 0 ) );
	eastl::fill_n( m_szLocations, s_nInstanceCacheMaxPages, t3dInvalidRegionCoord() );
	eastl::fill_n( m_szResidentBytes, s_nInstanceCacheMaxPages, ( size_t )( 0u ) );
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
	return t3dRegionResidencyFindMappedSlot( vLocation, m_szRegionMap, m_szLocations, m_nCapacity, []( const int ) { return true; } );
}

size_t Terrain3DInstanceRegionCache::getTrackedResidentBytes( const Vector2i &vLocation ) const {
	const int slot = findSlot( vLocation );
	return slot >= 0 ? m_szResidentBytes[slot] : 0u;
}

size_t Terrain3DInstanceRegionCache::getTrackedResidentBytesSum( void ) const {
	size_t trackedBytes = 0u;
	for ( int slot = 0; slot < m_nCapacity; ++slot ) {
		trackedBytes += m_szResidentBytes[slot];
	}
	return trackedBytes;
}

void Terrain3DInstanceRegionCache::updateTrackedResidentBytes( const int iSlot, const size_t nCurrentBytes ) {
	if ( !t3dInstanceCacheIndexValid( iSlot, m_nCapacity ) ) {
		return;
	}
	const size_t trackedBytes = m_szResidentBytes[iSlot];
	if ( trackedBytes == nCurrentBytes ) {
		return;
	}
	m_szResidentBytes[iSlot] = nCurrentBytes;
	if ( nCurrentBytes > trackedBytes ) {
		const size_t growthBytes = nCurrentBytes - trackedBytes;
		if ( growthBytes <= SIZE_MAX - m_nResidentBytes ) {
			m_nResidentBytes += growthBytes;
		} else {
			m_nResidentBytes = getTrackedResidentBytesSum();
		}
		return;
	}
	const size_t shrinkBytes = trackedBytes - nCurrentBytes;
	if ( shrinkBytes <= m_nResidentBytes ) {
		m_nResidentBytes -= shrinkBytes;
	} else {
		// A prior stale/corrupt aggregate must not zero unrelated resident slots.
		m_nResidentBytes = getTrackedResidentBytesSum();
	}
}

bool Terrain3DInstanceRegionCache::refreshResidentBytes( const Vector2i &vLocation ) {
	const int slot = findSlot( vLocation );
	if ( slot < 0 || !m_pRepository ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() || !pRegion->isInstanceStoreResident() ) {
		return false;
	}
	const size_t currentBytes = pRegion->getInstanceResidentBytes();
	if ( currentBytes != m_szResidentBytes[slot] ) {
		updateTrackedResidentBytes( slot, currentBytes );
		m_bSelectionDirty = true;
	}
	return true;
}

size_t Terrain3DInstanceRegionCache::reconcileResidentBytes( void ) {
	if ( !m_pRepository ) {
		return m_nResidentBytes;
	}
	bool bChanged = false;
	for ( int slot = 0; slot < m_nCapacity; ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( m_szLocations[slot] );
		if ( location == s_v2iMax ) {
			if ( m_szResidentBytes[slot] != 0u ) {
				updateTrackedResidentBytes( slot, 0u );
				bChanged = true;
			}
			continue;
		}
		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		const size_t currentBytes =
		        pRegion && !pRegion->isDeleted() && pRegion->isInstanceStoreResident() ? pRegion->getInstanceResidentBytes() : 0u;
		if ( currentBytes != m_szResidentBytes[slot] ) {
			updateTrackedResidentBytes( slot, currentBytes );
			bChanged = true;
		}
	}
	if ( bChanged ) {
		m_bSelectionDirty = true;
	}
	return m_nResidentBytes;
}

void Terrain3DInstanceRegionCache::recordEvictionDiagnostic( const int iSlot,
        const Vector2i &rLocation,
        const Vector2i &rAdmittedLocation,
        const size_t nCurrentBytes,
        const t3dInstanceRegionCacheEvictionReason_t eReason ) {
	m_lastEviction.m_evictedLocation = t3dPackRegionCoord( rLocation );
	m_lastEviction.m_admittedLocation = t3dPackRegionCoord( rAdmittedLocation );
	m_lastEviction.m_nTrackedBytes = t3dInstanceCacheIndexValid( iSlot, m_nCapacity ) ? m_szResidentBytes[iSlot] : 0u;
	m_lastEviction.m_nCurrentBytes = nCurrentBytes;
	m_lastEviction.m_nBudgetBytes = ( size_t )( m_nBudgetMb )*s_nInstanceCacheMib;
	m_lastEviction.m_eReason = eReason;
	++m_nEvictionsTotal;
	if ( eReason == TERRAIN3D_INSTANCE_REGION_EVICTION_SLOT_PRESSURE || eReason == TERRAIN3D_INSTANCE_REGION_EVICTION_DEMAND_BUDGET
	        || eReason == TERRAIN3D_INSTANCE_REGION_EVICTION_BUDGET_RECOVERY
	        || eReason == TERRAIN3D_INSTANCE_REGION_EVICTION_SELECTION_CHANGE ) {
		++m_nCachePolicyEvictions;
	}
}

void Terrain3DInstanceRegionCache::invalidate( const Vector2i &vLocation, t3dInstanceRegionCacheChanges_t *const pChanges ) {
	const int slot = findSlot( vLocation );
	if ( slot < 0 ) {
		return;
	}
	evictSlot( slot, pChanges, TERRAIN3D_INSTANCE_REGION_EVICTION_INVALIDATE, s_v2iMax );
	m_bSelectionDirty = true;
}

bool Terrain3DInstanceRegionCache::evictSlot( const int iSlot,
        t3dInstanceRegionCacheChanges_t *const pChanges,
        const t3dInstanceRegionCacheEvictionReason_t eReason,
        const Vector2i &rAdmittedLocation ) {
	if ( !t3dInstanceCacheIndexValid( iSlot, m_nCapacity ) || !m_pRepository ) {
		return false;
	}
	const Vector2i location = t3dUnpackRegionCoord( m_szLocations[iSlot] );
	if ( location == s_v2iMax ) {
		return true;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
	size_t currentBytes = 0u;
	if ( pRegion && pRegion->isInstanceStoreResident() ) {
		currentBytes = pRegion->getInstanceResidentBytes();
		const PackedByteArray page = pRegion->getInstancePage();
		if ( page.is_empty() && pRegion->hasInstances() ) {
			return false;
		}
		if ( !pRegion->evictInstanceStore() ) {
			return false;
		}
	}
	recordEvictionDiagnostic( iSlot, location, rAdmittedLocation, currentBytes, eReason );
	updateTrackedResidentBytes( iSlot, 0u );
	const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( location );
	if ( mapIndex >= 0 ) {
		m_szRegionMap[mapIndex] = 0u;
	}
	m_szLocations[iSlot] = t3dInvalidRegionCoord();
	m_nResidentCount = MAX( 0, m_nResidentCount - 1 );
	if ( pChanges ) {
		pChanges->recordEvicted( location );
	}
	return true;
}

bool Terrain3DInstanceRegionCache::evict( const Vector2i &vLocation, t3dInstanceRegionCacheChanges_t *const pChanges ) {
	const int slot = findSlot( vLocation );
	if ( slot >= 0 ) {
		return evictSlot( slot, pChanges, TERRAIN3D_INSTANCE_REGION_EVICTION_EXPLICIT, s_v2iMax );
	}
	Terrain3DRegion *const pRegion = m_pRepository ? m_pRepository->getRegionPtr( vLocation ) : NULL;
	if ( !pRegion || !pRegion->isInstanceStoreResident() ) {
		return true;
	}
	const size_t currentBytes = pRegion->getInstanceResidentBytes();
	const bool bEvicted = pRegion->evictInstanceStore();
	if ( bEvicted ) {
		recordEvictionDiagnostic( -1, vLocation, s_v2iMax, currentBytes, TERRAIN3D_INSTANCE_REGION_EVICTION_EXPLICIT );
		if ( pChanges ) {
			pChanges->recordEvicted( vLocation );
		}
	}
	return bEvicted;
}

bool Terrain3DInstanceRegionCache::attachResidentStore(
        const Vector2i &vLocation, const bool bDemandLoad, t3dInstanceRegionCacheChanges_t *const pChanges ) {
	Terrain3DRegion *const pRegion = m_pRepository ? m_pRepository->getRegionPtr( vLocation ) : NULL;
	if ( !pRegion || pRegion->isDeleted() || !pRegion->isInstanceStoreResident() ) {
		return false;
	}
	if ( !m_bEnabled ) {
		if ( pChanges ) {
			pChanges->recordLoaded( vLocation );
		}
		return true;
	}
	const int residentSlot = findSlot( vLocation );
	if ( residentSlot >= 0 ) {
		refreshResidentBytes( vLocation );
		return true;
	}
	if ( !pRegion->hasInstances() ) {
		return true;
	}
	int targetSlot = t3dRegionResidencyFindFirstSlot( m_nCapacity,
	        [&]( const int slot ) { return !t3dRegionCoordValid( m_szLocations[slot] ); } );
	if ( targetSlot < 0 ) {
		const Vector2i center = m_center == s_v2iMax ? vLocation : m_center;
		targetSlot = t3dRegionResidencySelectFarthestSlot( m_szLocations, m_nCapacity, center,
		        []( const int, const Vector2i & ) { return true; } );
	}
	if ( targetSlot < 0 ) {
		return false;
	}
	if ( t3dRegionCoordValid( m_szLocations[targetSlot] )
	        && !evictSlot( targetSlot, pChanges, TERRAIN3D_INSTANCE_REGION_EVICTION_SLOT_PRESSURE, vLocation ) ) {
		return false;
	}
	const size_t bytes = pRegion->getInstanceResidentBytes();
	const size_t budget = ( size_t )( m_nBudgetMb )*s_nInstanceCacheMib;
	while ( bDemandLoad && m_nResidentCount > 0 && m_nResidentBytes + bytes > budget ) {
		const Vector2i center = m_center == s_v2iMax ? vLocation : m_center;
		const int farthestSlot = t3dRegionResidencySelectFarthestSlot( m_szLocations, m_nCapacity, center,
		        [&]( const int, const Vector2i &oldLocation ) { return oldLocation != s_v2iMax && oldLocation != vLocation; } );
		if ( farthestSlot < 0 || !evictSlot( farthestSlot, pChanges, TERRAIN3D_INSTANCE_REGION_EVICTION_DEMAND_BUDGET, vLocation ) ) {
			break;
		}
		++m_nEvictionsLastTick;
	}
	if ( !bDemandLoad && !t3dInstanceCacheCanStream( m_nResidentBytes, bytes, budget, m_nResidentCount ) ) {
		return false;
	}
	m_szLocations[targetSlot] = t3dPackRegionCoord( vLocation );
	const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex >= 0 ) {
		m_szRegionMap[mapIndex] = ( uint8_t )( targetSlot + 1 );
	}
	++m_nResidentCount;
	updateTrackedResidentBytes( targetSlot, bytes );
	if ( bDemandLoad ) {
		++m_nDemandLoads;
	}
	if ( pChanges ) {
		pChanges->recordLoaded( vLocation );
	}
	return true;
}

bool Terrain3DInstanceRegionCache::ensureResident(
        const Vector2i &vLocation, const bool bDemandLoad, t3dInstanceRegionCacheChanges_t *const pChanges ) {
	Terrain3DRegion *const pRegion = m_pRepository ? m_pRepository->getRegionPtr( vLocation ) : NULL;
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( !pRegion->hasInstances() ) {
		return true;
	}
	if ( !pRegion->isInstanceStoreResident() ) {
		// Residency selection never materializes backing. The cache owner queues the
		// cold service explicitly and the caller observes non-residency until publish.
		m_bMaterializationPending = true;
		++m_nMaterializationMisses;
		return false;
	}
	return attachResidentStore( vLocation, bDemandLoad, pChanges );
}

bool Terrain3DInstanceRegionCache::materializeRegionCold( const Vector2i &vLocation, const t3dResidencyFrameState_t &rFrame ) {
	if ( !m_bMaterializationServiceActive ) {
		return false;
	}
	if ( !m_pRepository ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() || !pRegion->hasInstances() || pRegion->isInstanceStoreResident() ) {
		return pRegion && !pRegion->isDeleted();
	}
	const bool asyncRuntime = !rFrame.m_bAuthoring && rFrame.m_bCookedPagesEnabled && m_pRuntimePageStore;
	if ( !asyncRuntime ) {
		return pRegion->ensureInstanceStoreResident();
	}
	const uint8_t priority = rFrame.streamingState( vLocation ) == TERRAIN3D_REGION_STREAMING_ACTIVE ? 1u : 2u;
	m_pRuntimePageStore->request( vLocation, RUNTIME_PAGE_CONSUMER_INSTANCE_STORE, priority, true );
	Terrain3DInstanceStore preparedStore;
	PackedByteArray instanceMetadata;
	PackedByteArray multimeshFastStart;
	if ( !m_pRuntimePageStore->takeInstance( vLocation, preparedStore, &instanceMetadata, &multimeshFastStart ) ) {
		return false;
	}
	bool accelerationValid = true;
	if ( !instanceMetadata.is_empty() ) {
		t3dInstanceMetadataView_t view;
		accelerationValid = Terrain3DRuntimeBakeCodec::getInstanceMetadataView( instanceMetadata, view );
		accelerationValid = accelerationValid && view.m_nInstanceCount == preparedStore.instanceCount();
	}
	if ( !accelerationValid ) {
		instanceMetadata.clear();
		multimeshFastStart.clear();
	}
	pRegion->adoptPreparedInstanceStoreInternal( eastl::move( preparedStore ) );
	pRegion->adoptCookedInstanceAccelerationInternal( instanceMetadata, multimeshFastStart );
	return true;
}

void Terrain3DInstanceRegionCache::serviceColdMaterialization(
        const t3dResidencyFrameState_t &rFrame, const bool bForce, const int nEffectiveLoadPages ) {
	if ( !m_pRepository || ( !bForce && !m_bMaterializationPending ) ) {
		return;
	}
	m_bMaterializationServiceActive = true;
	m_bMaterializationPending = false;
	if ( !m_bEnabled ) {
		// Disabled cache means whole-world instance residency. Materialize backing
		// during the cold service phase so hot instancer access never decodes pages.
		for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
			const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
			if ( location != s_v2iMax ) {
				Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
				if ( pRegion && pRegion->hasInstances() && !pRegion->isInstanceStoreResident() ) {
					if ( materializeRegionCold( location, rFrame ) ) {
						++m_nColdMaterializations;
					} else {
						m_bMaterializationPending = true;
					}
				}
			}
		}
		m_bMaterializationServiceActive = false;
		return;
	}
	const Vector2i center = rFrame.m_residencyCenter;
	Vector2i desired[s_nInstanceCacheMaxPages];
	const int desiredCount = collectDesired( rFrame, center, desired, m_nCapacity );
	const int attemptBudget = bForce ? desiredCount : CLAMP( nEffectiveLoadPages, 0, m_nLoadBudgetPages );
	int attempts = 0;
	for ( int index = 0; index < desiredCount && attempts < attemptBudget; ++index ) {
		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( desired[index] );
		if ( !pRegion || pRegion->isDeleted() || !pRegion->hasInstances() || pRegion->isInstanceStoreResident() ) {
			continue;
		}
		++attempts;
		if ( materializeRegionCold( desired[index], rFrame ) ) {
			++m_nColdMaterializations;
		} else {
			m_bMaterializationPending = true;
		}
	}
	m_bMaterializationServiceActive = false;
}

static bool t3dInstanceRegionCandidateHasInstances(
        const Terrain3DRegion *const pRegion, const Vector2i &, void * ) {
	return pRegion && pRegion->hasInstances();
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
		outputCount = t3dCollectDesiredRegionsForState( *m_pRepository,
		        rFrame,
		        rCenter,
		        m_pRepository->getRegionMapSize(),
		        ( t3dRegionStreamingState_t )( stateValue ),
		        pLocations,
		        capacity,
		        outputCount,
		        NULL,
		        t3dInstanceRegionCandidateHasInstances );
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
	const Terrain3DLastScopeTimer serviceTimer( m_nServiceUsecLast, rFrame.m_bProfilingEnabled );
	m_nLoadsLastTick = 0;
	m_nStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_nEvictionsLastTick = 0;
	m_nLoadBytesLastTick = 0u;
	if ( !m_bEnabled || !m_pRepository ) {
		return;
	}
	const bool asyncRuntime = !rFrame.m_bAuthoring && rFrame.m_bCookedPagesEnabled && m_pRuntimePageStore;
	const uint64_t streamingGeneration = rFrame.m_nStreamingGeneration;
	if ( asyncRuntime && ( bForce || streamingGeneration != m_nPrefetchStreamingGenerationLast ) ) {
		for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
			const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
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
	uint8_t desiredMap[s_nRegionSlots];
	t3dRegionResidencyBuildDesiredMap( desired, desiredCount, desiredMap, T3D_ARRAY_COUNT( desiredMap ) );
	const size_t residentBudget = ( size_t )( m_nBudgetMb )*s_nInstanceCacheMib;
	while ( m_nResidentCount > 1 && m_nResidentBytes > residentBudget ) {
		const int farthestSlot = t3dRegionResidencySelectFarthestSlot( m_szLocations, m_nCapacity, center,
		        []( const int, const Vector2i &location ) { return location != s_v2iMax; } );
		if ( farthestSlot < 0 || !evictSlot( farthestSlot, &rChanges, TERRAIN3D_INSTANCE_REGION_EVICTION_BUDGET_RECOVERY, s_v2iMax ) ) {
			break;
		}
		++m_nEvictionsLastTick;
	}
	for ( int slot = 0; slot < m_nCapacity; ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( m_szLocations[slot] );
		const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( location != s_v2iMax && ( mapIndex < 0 || desiredMap[mapIndex] == 0u ) ) {
			if ( evictSlot( slot, &rChanges, TERRAIN3D_INSTANCE_REGION_EVICTION_SELECTION_CHANGE, s_v2iMax ) ) {
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
		}
		if ( pRegion && !pRegion->isInstanceStoreResident() ) {
			m_bMaterializationPending = true;
			++m_nMaterializationMisses;
		}
		if ( pRegion && pRegion->isInstanceStoreResident() && attachResidentStore( location, false, &rChanges ) ) {
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
	stats.m_bMaterializationPending = m_bMaterializationPending;
	stats.m_bMaterializationServiceActive = m_bMaterializationServiceActive;
	stats.m_nColdMaterializations = m_nColdMaterializations;
	stats.m_nMaterializationMisses = m_nMaterializationMisses;
	stats.m_nEvictionsTotal = m_nEvictionsTotal;
	stats.m_nCachePolicyEvictions = m_nCachePolicyEvictions;
	stats.m_nTrackedResidentBytes = getTrackedResidentBytesSum();
	stats.m_lastEviction = m_lastEviction;
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
	stats["materialization_pending"] = m_bMaterializationPending;
	stats["materialization_service_active"] = m_bMaterializationServiceActive;
	stats["cold_materializations"] = ( int64_t )( m_nColdMaterializations );
	stats["materialization_misses"] = ( int64_t )( m_nMaterializationMisses );
	stats["evictions_total"] = ( int64_t )( m_nEvictionsTotal );
	stats["cache_policy_evictions"] = ( int64_t )( m_nCachePolicyEvictions );
	stats["tracked_resident_bytes"] = ( int64_t )( getTrackedResidentBytesSum() );
	stats["resident_byte_accounting_consistent"] = m_nResidentBytes == getTrackedResidentBytesSum();
	if ( !bDetailed ) {
		return stats;
	}
	stats["load_page_budget"] = m_nLoadBudgetPages;
	stats["effective_load_page_budget"] = nEffectiveLoadPages;
	stats["load_budget_mb"] = m_nLoadBudgetMb;
	stats["stall_flags_last_tick"] = ( int64_t )( m_nStallFlagsLastTick );
	stats["center"] = m_center;
	stats["budget_overcommitted"] = m_nResidentBytes > ( size_t )( m_nBudgetMb )*s_nInstanceCacheMib;
	stats["last_eviction_reason"] = ( int64_t )( m_lastEviction.m_eReason );
	stats["last_eviction_region"] = t3dUnpackRegionCoord( m_lastEviction.m_evictedLocation );
	stats["last_eviction_admitted_region"] = t3dUnpackRegionCoord( m_lastEviction.m_admittedLocation );
	stats["last_eviction_tracked_bytes"] = ( int64_t )( m_lastEviction.m_nTrackedBytes );
	stats["last_eviction_current_bytes"] = ( int64_t )( m_lastEviction.m_nCurrentBytes );
	stats["last_eviction_budget_bytes"] = ( int64_t )( m_lastEviction.m_nBudgetBytes );
	return stats;
}
