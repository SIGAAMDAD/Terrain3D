// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "diagnostics/core/terrain_3d_diagnostics.h"
#include "Terrain3DTypeSupport.h"
#include "streaming/residency/terrain_3d_region_residency_primitives.h"
#include "streaming/cache/terrain_3d_cpu_region_cache.h"
#include "streaming/residency/terrain_3d_region_candidate_collector.h"

#include "data/model/terrain_3d_region.h"
#include "data/persistence/terrain_3d_region_persistence.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "core/utility/terrain_3d_util.h"
#include "streaming/policy/terrain_3d_cpu_cache_policy.h"
#include "streaming/policy/terrain_3d_runtime_profile.h"
#include "streaming/utility/terrain_3d_scope_timer.h"

#include <math.h>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>



static Rect2i t3dCpuRegionCacheGetRegionBoundsForArea( const Rect2i &rArea ) {
	if ( rArea.size.x <= 0 || rArea.size.y <= 0 ) {
		return Rect2i();
	}
	const Vector2i begin = t3dRegionCoordinateFromGrid( rArea.position );
	const Vector2i inclusive_end = rArea.get_end() - Vector2i( 1, 1 );
	const Vector2i end = t3dRegionCoordinateFromGrid( inclusive_end ) + Vector2i( 1, 1 );
	return Rect2i( begin, end - begin );
}

Terrain3DCpuRegionCache::Terrain3DCpuRegionCache( Terrain3DRegionRepository *const pRepository,
        Terrain3DRegionPersistence *const pPersistence,
        Terrain3DSamplingStorage *const pSamplingStorage,
        Terrain3DRuntimePageStore *const pRuntimePageStore ) :
        m_pRepository( pRepository ),
        m_pPersistence( pPersistence ),
        m_pSamplingStorage( pSamplingStorage ),
        m_pRuntimePageStore( pRuntimePageStore ) {
	eastl::fill_n( m_szLocations, T3D_CPU_CACHE_MAX_LAYERS, t3dInvalidRegionCoord() );
}

void Terrain3DCpuRegionCache::setRuntimePageConfig( const t3dRuntimePageConfig_s &rConfig ) {
	m_runtimePageConfig = rConfig;
}

bool Terrain3DCpuRegionCache::cookedRuntimeEnabled( void ) const {
	return m_pRuntimePageStore && m_runtimePageConfig.m_bCookedPagesEnabled && !m_runtimePageConfig.m_bEditorAuthoring;
}

bool Terrain3DCpuRegionCache::loadCookedMapsNow(
        const Vector2i &vLocation, Ref<Image> &rHeight, Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived ) const {
	return cookedRuntimeEnabled()
	        && m_pRuntimePageStore->loadCpuMapsNow( vLocation, m_runtimePageConfig, rHeight, rControl, rColor, rDerived );
}

int Terrain3DCpuRegionCache::calculateCapacity( void ) const {
	return m_pRepository ? t3dCpuCacheCalculateCapacity( m_nBudgetMb, T3D_CPU_CACHE_MAX_LAYERS ) : 0;
}

size_t Terrain3DCpuRegionCache::estimateLayerBytes( void ) const {
	return m_pRepository ? t3dCpuCacheEstimateLayerBytes() : 0u;
}

void Terrain3DCpuRegionCache::reset( const bool bReleaseCleanMaps ) {
	if ( bReleaseCleanMaps ) {
		for ( int slot = 0; slot < m_nCapacity; ++slot ) {
			evictSlot( slot );
		}
	}
	eastl::fill_n( m_szRegionMap, T3D_REGION_SLOTS, ( uint8_t )( 0 ) );
	eastl::fill_n( m_szLocations, T3D_CPU_CACHE_MAX_LAYERS, t3dInvalidRegionCoord() );
	m_nCapacity = 0;
	m_nResidentCount = 0;
	m_nDesiredLastTick = 0;
	m_nLoadsLastTick = 0;
	m_nLoadsSinceReset = 0u;
	m_nEvictionsLastTick = 0;
	m_nPinnedLastTick = 0;
	m_nLoadBytesLastTick = 0u;
	m_nAsyncRequestsLastTick = 0;
	m_nAsyncReadyLastTick = 0;
	m_nAsyncPendingLastTick = 0;
	m_nAsyncOutstanding = 0;
	m_nPrefetchStreamingGenerationLast = 0u;
	eastl::fill_n( m_szAsyncState, T3D_REGION_SLOTS, ( uint8_t )( 0 ) );
	m_center = s_v2iMax;
	m_bSelectionDirty = true;
}

void Terrain3DCpuRegionCache::refreshCapacity( void ) {
	if ( !m_bEnabled ) {
		return;
	}
	const int capacity = calculateCapacity();
	if ( capacity == m_nCapacity ) {
		return;
	}
	reset( true );
	m_nCapacity = capacity;
	m_bSelectionDirty = true;
}

int Terrain3DCpuRegionCache::findSlot( const Vector2i &vLocation ) const {
	return t3dRegionResidencyFindMappedSlot( vLocation, m_szRegionMap, m_szLocations, m_nCapacity, []( const int ) { return true; } );
}

void Terrain3DCpuRegionCache::invalidate( const Vector2i &vLocation ) {
	const int slot = findSlot( vLocation );
	if ( slot < 0 ) {
		return;
	}
	const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex >= 0 ) {
		m_szRegionMap[mapIndex] = 0u;
	}
	m_szLocations[slot] = t3dInvalidRegionCoord();
	m_nResidentCount = MAX( 0, m_nResidentCount - 1 );
	m_bSelectionDirty = true;
}

bool Terrain3DCpuRegionCache::loadSource( const Vector2i &vLocation, Ref<Terrain3DRegion> &rSource ) const {
	rSource.unref();
	if ( !m_pRepository || !m_pPersistence ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( pRegion->hasRuntimeMapPayloadInternal() ) {
		rSource = Ref<Terrain3DRegion>( pRegion );
		return true;
	}
	return m_pPersistence->loadRuntimeRegionSource( pRegion->get_path(), rSource );
}

bool Terrain3DCpuRegionCache::requestSourceAsync( const Vector2i &vLocation ) {
	if ( !m_pRepository ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex < 0 || !pRegion || pRegion->isDeleted() || pRegion->hasRuntimeMapPayloadInternal() || m_szAsyncState[mapIndex] != 0u ) {
		return pRegion && !pRegion->isDeleted();
	}
	if ( cookedRuntimeEnabled() ) {
		m_pRuntimePageStore->request( vLocation, RUNTIME_PAGE_CONSUMER_CPU_MAPS, 1u, true );
		m_szAsyncState[mapIndex] = 1u;
		++m_nAsyncOutstanding;
		++m_nAsyncRequestsLastTick;
		return true;
	}
	const String path = pRegion->get_path();
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		m_szAsyncState[mapIndex] = 2u;
		return false;
	}
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( !pLoader ) {
		return false;
	}
	const Error error = pLoader->load_threaded_request( path, "Terrain3DRegion", true, ResourceLoader::CACHE_MODE_IGNORE );
	if ( error != OK && error != ERR_BUSY ) {
		m_szAsyncState[mapIndex] = 2u;
		return false;
	}
	m_szAsyncState[mapIndex] = 1u;
	++m_nAsyncOutstanding;
	++m_nAsyncRequestsLastTick;
	return true;
}

bool Terrain3DCpuRegionCache::takeSourceAsync( const Vector2i &vLocation, Ref<Terrain3DRegion> &rSource ) {
	rSource.unref();
	if ( !m_pRepository || !m_pPersistence ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex < 0 || !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( pRegion->hasRuntimeMapPayloadInternal() ) {
		rSource = Ref<Terrain3DRegion>( pRegion );
		return true;
	}
	if ( m_szAsyncState[mapIndex] != 1u ) {
		requestSourceAsync( vLocation );
		return false;
	}
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( !pLoader ) {
		return false;
	}
	const String path = pRegion->get_path();
	const ResourceLoader::ThreadLoadStatus status = pLoader->load_threaded_get_status( path );
	if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS ) {
		++m_nAsyncPendingLastTick;
		return false;
	}
	if ( status != ResourceLoader::THREAD_LOAD_LOADED ) {
		m_szAsyncState[mapIndex] = 2u;
		m_nAsyncOutstanding = MAX( 0, m_nAsyncOutstanding - 1 );
		return false;
	}
	const Ref<Resource> pResource = pLoader->load_threaded_get( path );
	const Ref<Terrain3DRegion> pLoaded = pResource;
	m_szAsyncState[mapIndex] = 0u;
	m_nAsyncOutstanding = MAX( 0, m_nAsyncOutstanding - 1 );
	if ( !m_pPersistence->prepareRuntimeRegionSource( pLoaded, rSource ) ) {
		return false;
	}
	++m_nAsyncReadyLastTick;
	return true;
}

void Terrain3DCpuRegionCache::prewarmSources( const t3dResidencyFrameState_s &rFrame ) {
	if ( !m_bEnabled || rFrame.m_bAuthoring || !m_pRepository ) {
		return;
	}
	const Vector2i center = rFrame.m_predictedCenter;
	const int radius = rFrame.m_nStreamingActiveRadius;
	for ( int y = -radius; y <= radius; ++y ) {
		for ( int x = -radius; x <= radius; ++x ) {
			const Vector2i location = center + Vector2i( x, y );
			if ( !m_pRepository->isLocationValid( location ) || !m_pRepository->getRegionPtr( location ) || findSlot( location ) >= 0 ) {
				continue;
			}
			Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
			if ( pRegion && !pRegion->isDeleted() && !pRegion->hasRuntimeMapPayloadInternal() ) {
				requestSourceAsync( location );
			}
		}
	}
}

void Terrain3DCpuRegionCache::reapSourceAsync( const t3dResidencyFrameState_s &rFrame ) {
	if ( m_nAsyncOutstanding <= 0 || rFrame.m_bAuthoring || !m_pRepository ) {
		return;
	}
	if ( cookedRuntimeEnabled() ) {
		const Vector2i predictedCenter = rFrame.m_predictedCenter;
		const int activeRadius = rFrame.m_nStreamingActiveRadius;
		for ( int slot = 0; slot < m_pRepository->getSlotCount() && m_nAsyncOutstanding > 0; ++slot ) {
			const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
			const int mapIndex = location == s_v2iMax ? -1 : Terrain3DRegionRepository::getRegionMapIndex( location );
			if ( mapIndex < 0 || m_szAsyncState[mapIndex] == 0u ) {
				continue;
			}
			const bool activeNow = rFrame.streamingState( location ) == TERRAIN3D_REGION_STREAMING_ACTIVE;
			const bool predictedActive =
			        MAX( Math::abs( location.x - predictedCenter.x ), Math::abs( location.y - predictedCenter.y ) ) <= activeRadius;
			if ( activeNow || predictedActive ) {
				continue;
			}
			m_pRuntimePageStore->request( location, RUNTIME_PAGE_CONSUMER_CPU_MAPS, 0u, false );
			m_szAsyncState[mapIndex] = 0u;
			m_nAsyncOutstanding = MAX( 0, m_nAsyncOutstanding - 1 );
		}
		return;
	}
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( !pLoader ) {
		return;
	}
	const Vector2i predictedCenter = rFrame.m_predictedCenter;
	const int activeRadius = rFrame.m_nStreamingActiveRadius;
	for ( int slot = 0; slot < m_pRepository->getSlotCount() && m_nAsyncOutstanding > 0; ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
		const int mapIndex = location == s_v2iMax ? -1 : Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( mapIndex < 0 || m_szAsyncState[mapIndex] == 0u ) {
			continue;
		}
		const bool activeNow = rFrame.streamingState( location ) == TERRAIN3D_REGION_STREAMING_ACTIVE;
		const bool predictedActive =
		        MAX( Math::abs( location.x - predictedCenter.x ), Math::abs( location.y - predictedCenter.y ) ) <= activeRadius;
		if ( activeNow || predictedActive ) {
			continue;
		}
		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() || pRegion->get_path().is_empty() ) {
			m_szAsyncState[mapIndex] = 0u;
			m_nAsyncOutstanding = MAX( 0, m_nAsyncOutstanding - 1 );
			continue;
		}
		const String path = pRegion->get_path();
		const ResourceLoader::ThreadLoadStatus status = pLoader->load_threaded_get_status( path );
		if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS ) {
			continue;
		}
		if ( status == ResourceLoader::THREAD_LOAD_LOADED ) {
			const Ref<Resource> discard = pLoader->load_threaded_get( path );
			( void )discard;
		}
		m_szAsyncState[mapIndex] = 0u;
		m_nAsyncOutstanding = MAX( 0, m_nAsyncOutstanding - 1 );
	}
}

bool Terrain3DCpuRegionCache::publishSlot( const int iSlot, const Vector2i &vLocation, const Ref<Terrain3DRegion> &rSource ) {
	if ( iSlot < 0 || iSlot >= m_nCapacity || rSource.is_null() || !m_pRepository ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	pRegion->importRuntimeMapsUnmodifiedInternal( rSource.ptr() );
	if ( !pRegion->hasRuntimeMapPayloadInternal() ) {
		return false;
	}
	const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex < 0 ) {
		return false;
	}
	m_szLocations[iSlot] = t3dPackRegionCoord( vLocation );
	m_szRegionMap[mapIndex] = ( uint8_t )( iSlot + 1 );
	++m_nResidentCount;
	if ( m_pSamplingStorage ) {
		m_pSamplingStorage->publishRegionGeneration( vLocation );
	}
	return true;
}

bool Terrain3DCpuRegionCache::publishCookedSlot( const int iSlot,
        const Vector2i &vLocation,
        const Ref<Image> &rHeight,
        const Ref<Image> &rControl,
        const Ref<Image> &rColor,
        const Ref<Image> &rDerived ) {
	if ( iSlot < 0 || iSlot >= m_nCapacity || !m_pRepository || rHeight.is_null() || rDerived.is_null() ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	pRegion->adoptCookedRuntimeMapsInternal( rHeight, rControl, rColor, rDerived );
	if ( !pRegion->hasRuntimeMapPayloadInternal() ) {
		return false;
	}
	const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex < 0 ) {
		return false;
	}
	m_szLocations[iSlot] = t3dPackRegionCoord( vLocation );
	m_szRegionMap[mapIndex] = ( uint8_t )( iSlot + 1 );
	++m_nResidentCount;
	if ( m_pSamplingStorage ) {
		m_pSamplingStorage->publishRegionGeneration( vLocation );
	}
	return true;
}

bool Terrain3DCpuRegionCache::evictSlot( const int iSlot ) {
	if ( iSlot < 0 || iSlot >= m_nCapacity || !m_pRepository ) {
		return false;
	}
	const Vector2i location = t3dUnpackRegionCoord( m_szLocations[iSlot] );
	if ( location == s_v2iMax ) {
		return true;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
	if ( pRegion && pRegion->hasRuntimeMapPayloadInternal() && !pRegion->runtimeMapsEvictableInternal() ) {
		return false;
	}
	if ( m_pSamplingStorage ) {
		m_pSamplingStorage->invalidateResident( location );
	}
	if ( pRegion ) {
		pRegion->releaseRuntimeMapsUnmodifiedInternal();
	}
	const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( location );
	if ( mapIndex >= 0 ) {
		m_szRegionMap[mapIndex] = 0u;
	}
	m_szLocations[iSlot] = t3dInvalidRegionCoord();
	m_nResidentCount = MAX( 0, m_nResidentCount - 1 );
	if ( m_pSamplingStorage ) {
		m_pSamplingStorage->publishRegionGeneration( location );
	}
	return true;
}

bool Terrain3DCpuRegionCache::ensureResident( const Vector2i &vLocation, const bool bDemandLoad ) {
	if ( !m_bEnabled ) {
		return !m_pRepository || m_pRepository->getRegionPtr( vLocation ) != NULL;
	}
	if ( !m_pRepository || !m_pRepository->getRegionPtr( vLocation ) ) {
		return false;
	}
	if ( findSlot( vLocation ) >= 0 ) {
		return true;
	}
	Terrain3DRegion *const pExistingRegion = m_pRepository->getRegionPtr( vLocation );
	if ( pExistingRegion && pExistingRegion->hasRuntimeMapPayloadInternal() && !pExistingRegion->runtimeMapsEvictableInternal() ) {
		return true;
	}
	Ref<Terrain3DRegion> pSource;
	Ref<Image> pHeight, pControl, pColor, pDerived;
	const bool bCooked = cookedRuntimeEnabled();
	if ( bCooked ) {
		if ( !loadCookedMapsNow( vLocation, pHeight, pControl, pColor, pDerived ) ) {
			return false;
		}
	} else if ( !loadSource( vLocation, pSource ) ) {
		return false;
	}
	int targetSlot = t3dRegionResidencyFindFirstSlot( m_nCapacity,
	        [&]( const int slot ) { return !t3dRegionCoordValid( m_szLocations[slot] ); } );
	if ( targetSlot < 0 ) {
		const Vector2i evictionCenter = m_center == s_v2iMax ? vLocation : m_center;
		targetSlot = t3dRegionResidencySelectFarthestSlot( m_szLocations, m_nCapacity, evictionCenter,
		        [&]( const int, const Vector2i &oldLocation ) {
		        const Terrain3DRegion *const pOldRegion = m_pRepository->getRegionPtr( oldLocation );
		        return pOldRegion && pOldRegion->runtimeMapsEvictableInternal();
		        } );
	}
	if ( targetSlot < 0 ) {
		return false;
	}
	if ( t3dRegionCoordValid( m_szLocations[targetSlot] ) ) {
		if ( !evictSlot( targetSlot ) ) {
			return false;
		}
		++m_nEvictionsLastTick;
	}
	if ( bCooked ? !publishCookedSlot( targetSlot, vLocation, pHeight, pControl, pColor, pDerived )
	             : !publishSlot( targetSlot, vLocation, pSource ) ) {
		return false;
	}
	if ( bDemandLoad ) {
		++m_nDemandLoads;
	}
	return true;
}

void Terrain3DCpuRegionCache::releaseUntrackedMaps( void ) {
	if ( !m_bEnabled || !m_pRepository ) {
		return;
	}
	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
		if ( location == s_v2iMax || findSlot( location ) >= 0 ) {
			continue;
		}
		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		if ( !pRegion || !pRegion->hasRuntimeMapPayloadInternal() || !pRegion->runtimeMapsEvictableInternal() ) {
			continue;
		}
		if ( m_pSamplingStorage ) {
			m_pSamplingStorage->invalidateResident( location );
		}
		pRegion->releaseRuntimeMapsUnmodifiedInternal();
		if ( m_pSamplingStorage ) {
			m_pSamplingStorage->publishRegionGeneration( location );
		}
	}
}

bool Terrain3DCpuRegionCache::restoreAllMaps( void ) {
	if ( !m_pRepository ) {
		return false;
	}
	bool bSuccess = true;
	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
		Terrain3DRegion *const pRegion = location == s_v2iMax ? NULL : m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() || pRegion->hasRuntimeMapPayloadInternal() ) {
			continue;
		}
		if ( cookedRuntimeEnabled() ) {
			Ref<Image> pHeight, pControl, pColor, pDerived;
			if ( !loadCookedMapsNow( location, pHeight, pControl, pColor, pDerived ) ) {
				bSuccess = false;
				continue;
			}
			pRegion->adoptCookedRuntimeMapsInternal( pHeight, pControl, pColor, pDerived );
		} else {
			Ref<Terrain3DRegion> pSource;
			if ( !loadSource( location, pSource ) ) {
				bSuccess = false;
				continue;
			}
			pRegion->importRuntimeMapsUnmodifiedInternal( pSource.ptr() );
		}
		if ( m_pSamplingStorage ) {
			m_pSamplingStorage->publishRegionGeneration( location );
		}
	}
	return bSuccess;
}

int Terrain3DCpuRegionCache::collectDesired(
        const t3dResidencyFrameState_s &rFrame, const Vector2i &rCenter, Vector2i *const pLocations, const int nCapacity ) const {
	if ( !pLocations || nCapacity <= 0 || !m_pRepository ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, T3D_CPU_CACHE_MAX_LAYERS );
	return t3dCollectDesiredRegionsForState( *m_pRepository,
	        rFrame,
	        rCenter,
	        m_pRepository->getRegionMapSize(),
	        TERRAIN3D_REGION_STREAMING_ACTIVE,
	        pLocations,
	        capacity,
	        0 );
}

bool Terrain3DCpuRegionCache::configure( const bool bEnabled, const int nBudgetMb, const int nLoadLayers, const int nLoadBudgetMb ) {
	const bool bOldEnabled = m_bEnabled;
	const int oldBudget = m_nBudgetMb;
	m_bEnabled = bEnabled;
	m_nBudgetMb = CLAMP( nBudgetMb, 0, T3D_CPU_REGION_CACHE_MAX_BUDGET_MB );
	m_nLoadBudgetLayers = CLAMP( nLoadLayers, 1, T3D_CPU_REGION_LOAD_MAX_LAYERS );
	m_nLoadBudgetMb = CLAMP( nLoadBudgetMb, 1, T3D_CPU_REGION_LOAD_MAX_BUDGET_MB );
	if ( !bOldEnabled && !m_bEnabled ) {
		return true;
	}
	if ( bOldEnabled && !m_bEnabled ) {
		if ( !restoreAllMaps() ) {
			m_bEnabled = true;
			m_bSelectionDirty = true;
			return false;
		}
		reset( false );
		return true;
	}
	const int capacity = calculateCapacity();
	if ( !bOldEnabled || oldBudget != m_nBudgetMb || capacity != m_nCapacity ) {
		reset( true );
		m_nCapacity = capacity;
		m_bSelectionDirty = true;
	}
	return true;
}

bool Terrain3DCpuRegionCache::ensureSampleArea( const Vector3 &rWorldPosition, const int nRadiusTexels ) {
	if ( !m_pRepository || !rWorldPosition.is_finite() || nRadiusTexels < 0
	        || nRadiusTexels > T3D_REGION_MAP_SIZE * T3D_REGION_SIZE ) {
		return false;
	}
	// Include the upper interpolation neighbor, with bounded integer conversion.
	const int radius = nRadiusTexels + 1;
	const double gridX = Math::floor( ( double )rWorldPosition.x / s_fRegionVertexSpacing );
	const double gridZ = Math::floor( ( double )rWorldPosition.z / s_fRegionVertexSpacing );
	if ( gridX < ( double )INT32_MIN + radius || gridX > ( double )INT32_MAX - radius - 1
	        || gridZ < ( double )INT32_MIN + radius || gridZ > ( double )INT32_MAX - radius - 1 ) {
		return false;
	}
	const Vector2i center( ( int32_t )gridX, ( int32_t )gridZ );
	const Vector2i extent( radius * 2 + 1, radius * 2 + 1 );
	const Rect2i requested = t3dCpuRegionCacheGetRegionBoundsForArea( Rect2i( center - Vector2i( radius, radius ), extent ) );
	const int halfGrid = m_pRepository->getRegionMapSize() / 2;
	const Rect2i bounds = requested.intersection( Rect2i( Vector2i( -halfGrid, -halfGrid ), Vector2i( halfGrid * 2, halfGrid * 2 ) ) );
	const Vector2i end = bounds.get_end();
	int regionCount = 0;
	int requiredSlots = 0;
	for ( int y = bounds.position.y; y < end.y; ++y ) {
		for ( int x = bounds.position.x; x < end.x; ++x ) {
			const Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( Vector2i( x, y ) );
			if ( !pRegion || pRegion->isDeleted() ) {
				continue;
			}
			++regionCount;
			if ( !pRegion->hasRuntimeMapPayloadInternal() || pRegion->runtimeMapsEvictableInternal() ) {
				++requiredSlots;
			}
		}
	}
	if ( regionCount == 0 || ( m_bEnabled && requiredSlots > m_nCapacity ) ) {
		return false;
	}
	bool bSuccess = true;
	for ( int y = bounds.position.y; y < end.y; ++y ) {
		for ( int x = bounds.position.x; x < end.x; ++x ) {
			const Vector2i location( x, y );
			const Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
			if ( pRegion && !pRegion->isDeleted() && !ensureResident( location, true ) ) {
				bSuccess = false;
			}
		}
	}
	// Demand loading can evict an earlier region. Success describes the whole
	// footprint at return, rather than just each individual load's result.
	for ( int y = bounds.position.y; y < end.y; ++y ) {
		for ( int x = bounds.position.x; x < end.x; ++x ) {
			const Vector2i location( x, y );
			const Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
			if ( pRegion && !pRegion->isDeleted() && findSlot( location ) < 0
			        && !pRegion->hasRuntimeMapPayloadInternal() ) {
				bSuccess = false;
			}
		}
	}
	return bSuccess;
}

void Terrain3DCpuRegionCache::update( const t3dResidencyFrameState_s &rFrame, const bool bForce, const int nEffectiveLoadLayers ) {
	const Terrain3DLastScopeTimer serviceTimer( m_nServiceUsecLast, rFrame.m_bProfilingEnabled );
	setRuntimePageConfig( rFrame.m_runtimePageConfig );
	m_nLoadsLastTick = 0;
	m_nStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_nEvictionsLastTick = 0;
	m_nPinnedLastTick = 0;
	m_nLoadBytesLastTick = 0u;
	m_nAsyncRequestsLastTick = 0;
	m_nAsyncReadyLastTick = 0;
	m_nAsyncPendingLastTick = 0;
	if ( !m_bEnabled || m_nCapacity <= 0 || !m_pRepository ) {
		return;
	}

	const uint64_t streamingGeneration = rFrame.m_nStreamingGeneration;
	if ( bForce || streamingGeneration != m_nPrefetchStreamingGenerationLast ) {
		prewarmSources( rFrame );
		m_nPrefetchStreamingGenerationLast = streamingGeneration;
	}
	reapSourceAsync( rFrame );

	const Vector2i center = rFrame.m_residencyCenter;
	if ( !bForce && center == m_center && !m_bSelectionDirty ) {
		return;
	}
	Vector2i desired[T3D_CPU_CACHE_MAX_LAYERS];
	const int desiredCount = collectDesired( rFrame, center, desired, m_nCapacity );
	m_nDesiredLastTick = desiredCount;
	uint8_t desiredMap[T3D_REGION_SLOTS];
	t3dRegionResidencyBuildDesiredMap( desired, desiredCount, desiredMap, T3D_ARRAY_COUNT( desiredMap ) );
	const size_t layerBytes = estimateLayerBytes();
	const size_t byteBudget = ( size_t )( m_nLoadBudgetMb )*T3D_CPU_CACHE_MIB;
	const int loadLayerBudget = CLAMP( nEffectiveLoadLayers, 0, m_nLoadBudgetLayers );
	bool bAllDesiredResident = true;
	for ( int desiredIndex = 0; desiredIndex < desiredCount; ++desiredIndex ) {
		const Vector2i location = desired[desiredIndex];
		if ( findSlot( location ) >= 0 ) {
			continue;
		}
		Terrain3DRegion *const pAvailableRegion = m_pRepository->getRegionPtr( location );
		if ( pAvailableRegion && pAvailableRegion->hasRuntimeMapPayloadInternal() && !pAvailableRegion->runtimeMapsEvictableInternal() ) {
			continue;
		}
		bAllDesiredResident = false;
		if ( !bForce && m_nLoadsLastTick >= loadLayerBudget ) {
			m_nStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_CPU_LOAD_BUDGET;
			break;
		}
		if ( !bForce && m_nLoadsLastTick > 0 && m_nLoadBytesLastTick + layerBytes > byteBudget ) {
			m_nStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_CPU_LOAD_BYTE_BUDGET;
			break;
		}
		Ref<Terrain3DRegion> pSource;
		Ref<Image> pHeight, pControl, pColor, pDerived;
		const bool bCooked = cookedRuntimeEnabled();
		bool bSourceReady = false;
		if ( bCooked ) {
			if ( bForce ) {
				bSourceReady = loadCookedMapsNow( location, pHeight, pControl, pColor, pDerived );
			} else {
				bSourceReady = m_pRuntimePageStore->takeCpuMaps( location, pHeight, pControl, pColor, pDerived );
				const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( location );
				if ( bSourceReady && mapIndex >= 0 && m_szAsyncState[mapIndex] != 0u ) {
					m_szAsyncState[mapIndex] = 0u;
					m_nAsyncOutstanding = MAX( 0, m_nAsyncOutstanding - 1 );
					++m_nAsyncReadyLastTick;
				} else if ( !bSourceReady ) {
					++m_nAsyncPendingLastTick;
				}
			}
		} else {
			bSourceReady = bForce ? loadSource( location, pSource ) : takeSourceAsync( location, pSource );
		}
		if ( !bSourceReady ) {
			if ( !bForce ) {
				requestSourceAsync( location );
			}
			continue;
		}
		int targetSlot = t3dRegionResidencyFindFirstSlot( m_nCapacity,
		        [&]( const int slot ) { return !t3dRegionCoordValid( m_szLocations[slot] ); } );
		if ( targetSlot < 0 ) {
			targetSlot = t3dRegionResidencySelectFarthestSlot( m_szLocations, m_nCapacity, center,
			        [&]( const int, const Vector2i &oldLocation ) {
			        const int oldMapIndex = Terrain3DRegionRepository::getRegionMapIndex( oldLocation );
			        if ( oldMapIndex >= 0 && desiredMap[oldMapIndex] != 0u ) {
			        	return false;
			        }
			        const Terrain3DRegion *const pOldRegion = m_pRepository->getRegionPtr( oldLocation );
			        if ( !pOldRegion || !pOldRegion->runtimeMapsEvictableInternal() ) {
			        	++m_nPinnedLastTick;
			        	return false;
			        }
			        return true;
			        } );
		}
		if ( targetSlot < 0 ) {
			m_nStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_CPU_PINNED_CAPACITY;
			continue;
		}
		if ( t3dRegionCoordValid( m_szLocations[targetSlot] ) ) {
			if ( !evictSlot( targetSlot ) ) {
				continue;
			}
			++m_nEvictionsLastTick;
		}
		if ( bCooked ? !publishCookedSlot( targetSlot, location, pHeight, pControl, pColor, pDerived )
		             : !publishSlot( targetSlot, location, pSource ) ) {
			continue;
		}
		++m_nLoadsLastTick;
		++m_nLoadsSinceReset;
		m_nLoadBytesLastTick += layerBytes;
	}
	if ( !bAllDesiredResident ) {
		bAllDesiredResident = true;
		for ( int index = 0; index < desiredCount; ++index ) {
			const Vector2i location = desired[index];
			Terrain3DRegion *const pAvailableRegion = m_pRepository->getRegionPtr( location );
			const bool bPinnedAvailable = pAvailableRegion && pAvailableRegion->hasRuntimeMapPayloadInternal()
			        && !pAvailableRegion->runtimeMapsEvictableInternal();
			if ( findSlot( location ) < 0 && !bPinnedAvailable ) {
				bAllDesiredResident = false;
				break;
			}
		}
	}
	m_center = center;
	m_bSelectionDirty = !bAllDesiredResident;
	releaseUntrackedMaps();
}

size_t Terrain3DCpuRegionCache::getReservedBytes( void ) const {
	return estimateLayerBytes() * ( size_t )( MAX( 0, m_nCapacity ) );
}

size_t Terrain3DCpuRegionCache::getMinimumBytes( void ) const {
	return estimateLayerBytes() * ( size_t )( T3D_CPU_CACHE_MIN_SAMPLE_LAYERS );
}

size_t Terrain3DCpuRegionCache::getHardCapBytes( void ) const {
	return estimateLayerBytes() * ( size_t )( T3D_CPU_CACHE_MAX_LAYERS );
}

t3dCpuRegionCacheStats_t Terrain3DCpuRegionCache::getStats( void ) const {
	t3dCpuRegionCacheStats_t stats;
	stats.m_bEnabled = m_bEnabled;
	stats.m_bSelectionPending = m_bSelectionDirty;
	stats.m_nCapacity = m_nCapacity;
	stats.m_nResident = m_nResidentCount;
	stats.m_nDesiredLastTick = m_nDesiredLastTick;
	stats.m_nLoadsLastTick = m_nLoadsLastTick;
	stats.m_nEvictionsLastTick = m_nEvictionsLastTick;
	stats.m_nPinnedLastTick = m_nPinnedLastTick;
	stats.m_nStallFlagsLastTick = m_nStallFlagsLastTick;
	stats.m_nLoadBytesLastTick = m_nLoadBytesLastTick;
	stats.m_nServiceUsecLast = m_nServiceUsecLast;
	return stats;
}

Dictionary Terrain3DCpuRegionCache::getStatsDictionary( const bool bDetailed, const int nEffectiveLoadLayers ) const {
	const auto values = getStats();
	Dictionary stats;
	t3dAppendRegionLoadCacheStats( stats, values, m_nBudgetMb, m_nDemandLoads );
	stats["loads_since_reset"] = ( int64_t )( m_nLoadsSinceReset );
	stats[ "pinned_last_tick" ] = m_nPinnedLastTick;
	stats[ "async_requests_last_tick" ] = m_nAsyncRequestsLastTick;
	stats["async_ready_last_tick"] = m_nAsyncReadyLastTick;
	stats["async_pending_last_tick"] = m_nAsyncPendingLastTick;
	stats[ "reserved_bytes" ] = (int64_t)( getReservedBytes() );
	if ( m_pSamplingStorage ) {
		stats["active_height_bytes"] = ( int64_t )( m_pSamplingStorage->getActiveReservedBytes() );
		stats["active_height_physical_capacity"] = m_pSamplingStorage->getActivePhysicalCapacity();
		stats["active_height_reader_retries"] = ( int64_t )( m_pSamplingStorage->getActiveReaderRetryCount() );
		stats["active_height_reader_mutexes"] = ( int64_t )( m_pSamplingStorage->getActiveReaderMutexCount() );
		stats["active_height_publications"] = ( int64_t )( m_pSamplingStorage->getActivePublicationCount() );
	}
	if ( !bDetailed || !m_pRepository ) {
		return stats;
	}

	int pinnedPayloadCount = 0;
	int totalPayloadCount = 0;
	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
		const Terrain3DRegion *const pRegion = location == s_v2iMax ? NULL : m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() || !pRegion->hasRuntimeMapPayloadInternal() ) {
			continue;
		}
		++totalPayloadCount;
		if ( findSlot( location ) < 0 ) {
			++pinnedPayloadCount;
		}
	}
	stats["load_layer_budget"] = m_nLoadBudgetLayers;
	stats["effective_load_layer_budget"] = nEffectiveLoadLayers;
	stats["load_budget_mb"] = m_nLoadBudgetMb;
	stats["stall_flags_last_tick"] = ( int64_t )( m_nStallFlagsLastTick );
	stats["pinned_payloads"] = pinnedPayloadCount;
	stats["total_payloads"] = totalPayloadCount;
	const size_t layerBytes = estimateLayerBytes();
	const size_t worstCaseBytes = layerBytes * ( size_t )( MAX( 0, m_nCapacity ) );
	const size_t budgetBytes = ( size_t )( m_nBudgetMb )*T3D_CPU_CACHE_MIB;
	const size_t activeHeightBytes = m_pSamplingStorage ? m_pSamplingStorage->getActiveReservedBytes() : 0u;
	const size_t defaultMapBytes = layerBytes;
	const size_t payloadBytes = layerBytes * ( size_t )( MAX( 0, totalPayloadCount ) );
	const size_t pinnedBytes = layerBytes * ( size_t )( MAX( 0, pinnedPayloadCount ) );
	const size_t totalMapBytes = activeHeightBytes + defaultMapBytes + payloadBytes;
	stats["estimated_layer_bytes"] = ( int64_t )( layerBytes );
	stats["estimated_worst_case_bytes"] = ( int64_t )( worstCaseBytes );
	stats["estimated_payload_bytes"] = ( int64_t )( payloadBytes );
	stats["estimated_pinned_bytes"] = ( int64_t )( pinnedBytes );
	stats["default_map_bytes"] = ( int64_t )( defaultMapBytes );
	stats["estimated_total_map_bytes"] = ( int64_t )( totalMapBytes );
	stats["target_map_ceiling_bytes"] = ( int64_t )( T3D_XBOX360_CPU_MAP_CEILING_BYTES );
	stats["budget_overcommitted"] = worstCaseBytes > budgetBytes;
	stats["runtime_overcommitted"] = totalPayloadCount > m_nCapacity;
	stats["target_map_ceiling_exceeded"] = totalMapBytes > T3D_XBOX360_CPU_MAP_CEILING_BYTES;
	stats["center"] = m_center;
	return stats;
}
