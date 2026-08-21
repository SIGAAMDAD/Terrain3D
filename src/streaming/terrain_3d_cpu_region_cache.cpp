// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "streaming/terrain_3d_cpu_region_cache.h"

#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_persistence.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_sampling_storage.h"
#include "streaming/terrain_3d_cpu_cache_policy.h"
#include "streaming/terrain_3d_runtime_profile.h"

#include <algorithm>
#include <cmath>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>

class T3DCpuRegionCacheScopeTimer {
	uint64_t m_nStartedUsec = 0u;
	uint64_t *m_pElapsedUsec = nullptr;

public:
	T3DCpuRegionCacheScopeTimer( uint64_t &nElapsedUsec, const bool bEnabled ) {
		nElapsedUsec = 0u;
		if ( !bEnabled ) {
			return;
		}
		m_pElapsedUsec = &nElapsedUsec;
		Time *const pTime = Time::get_singleton();
		m_nStartedUsec = pTime ? pTime->get_ticks_usec() : 0u;
	}

	~T3DCpuRegionCacheScopeTimer( void ) {
		if ( !m_pElapsedUsec ) {
			return;
		}
		Time *const pTime = Time::get_singleton();
		const uint64_t finishedUsec = pTime ? pTime->get_ticks_usec() : m_nStartedUsec;
		*m_pElapsedUsec = finishedUsec >= m_nStartedUsec ? finishedUsec - m_nStartedUsec : 0u;
	}
};

template <typename Callback>
static bool t3dCpuRegionCacheForEachSquareRing( const Vector2i &rCenter, const int nMaxRadius, Callback &&rCallback ) {
	for ( int radius = 0; radius < nMaxRadius; ++radius ) {
		if ( radius == 0 ) {
			if ( !rCallback( rCenter ) ) {
				return false;
			}
			continue;
		}
		for ( int x = -radius; x <= radius; ++x ) {
			if ( !rCallback( rCenter + Vector2i( x, -radius ) ) || !rCallback( rCenter + Vector2i( x, radius ) ) ) {
				return false;
			}
		}
		for ( int y = -radius + 1; y <= radius - 1; ++y ) {
			if ( !rCallback( rCenter + Vector2i( -radius, y ) ) || !rCallback( rCenter + Vector2i( radius, y ) ) ) {
				return false;
			}
		}
	}
	return true;
}

static Rect2i t3dCpuRegionCacheGetRegionBoundsForArea( const Rect2i &rArea, const int nRegionSize ) {
	if ( nRegionSize <= 0 || rArea.size.x <= 0 || rArea.size.y <= 0 ) {
		return Rect2i();
	}
	const Vector2i begin = V2I_DIVIDE_FLOOR( rArea.position, nRegionSize );
	const Vector2i end = V2I_DIVIDE_CEIL( rArea.get_end(), nRegionSize );
	return Rect2i( begin, end - begin );
}

T3DCpuRegionCache::T3DCpuRegionCache( T3DRegionRepository *const pRepository,
		T3DRegionPersistence *const pPersistence,
		T3DSamplingStorage *const pSamplingStorage,
		T3DRuntimePageStore *const pRuntimePageStore ) :
		m_pRepository( pRepository ),
		m_pPersistence( pPersistence ),
		m_pSamplingStorage( pSamplingStorage ),
		m_pRuntimePageStore( pRuntimePageStore ) {
	std::fill_n( m_szLocations, s_nCpuCacheMaxLayers, s_v2iMax );
}

void T3DCpuRegionCache::setRuntimePageConfig( const t3dRuntimePageConfig_t &rConfig ) {
	m_runtimePageConfig = rConfig;
}

bool T3DCpuRegionCache::cookedRuntimeEnabled( void ) const {
	return m_pRuntimePageStore && m_runtimePageConfig.m_bCookedPagesEnabled && !m_runtimePageConfig.m_bEditorAuthoring;
}

bool T3DCpuRegionCache::loadCookedMapsNow( const Vector2i &vLocation,
		Ref<Image> &rHeight,
		Ref<Image> &rControl,
		Ref<Image> &rColor,
		Ref<Image> &rDerived ) const {
	return cookedRuntimeEnabled() &&
			m_pRuntimePageStore->loadCpuMapsNow( vLocation, m_runtimePageConfig, rHeight, rControl, rColor, rDerived );
}

int T3DCpuRegionCache::calculateCapacity( void ) const {
	return m_pRepository
			? t3dCpuCacheCalculateCapacity( m_pRepository->getRegionSize(), m_nBudgetMb, s_nCpuCacheMaxLayers )
			: 0;
}

size_t T3DCpuRegionCache::estimateLayerBytes( void ) const {
	return m_pRepository ? t3dCpuCacheEstimateLayerBytes( m_pRepository->getRegionSize() ) : 0u;
}

void T3DCpuRegionCache::reset( const bool bReleaseCleanMaps ) {
	if ( bReleaseCleanMaps ) {
		for ( int slot = 0; slot < m_nCapacity; ++slot ) {
			evictSlot( slot );
		}
	}
	std::fill_n( m_szRegionMap, s_nMaxRegionSlots, ( uint8_t )( 0 ) );
	std::fill_n( m_szLocations, s_nCpuCacheMaxLayers, s_v2iMax );
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
	std::fill_n( m_szAsyncState, s_nMaxRegionSlots, ( uint8_t )( 0 ) );
	m_center = s_v2iMax;
	m_bSelectionDirty = true;
}

void T3DCpuRegionCache::refreshCapacity( void ) {
	if ( !m_bEnabled ) {
		return;
	}
	const int capacity = calculateCapacity();
	if ( capacity == m_nCapacity ) {
		return;
	}
	reset( false );
	m_nCapacity = capacity;
	m_bSelectionDirty = true;
}

int T3DCpuRegionCache::findSlot( const Vector2i &vLocation ) const {
	const int mapIndex = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex < 0 ) {
		return -1;
	}
	const int slot = ( int )( m_szRegionMap[mapIndex] ) - 1;
	if ( slot < 0 || slot >= m_nCapacity || m_szLocations[slot] != vLocation ) {
		return -1;
	}
	return slot;
}

void T3DCpuRegionCache::invalidate( const Vector2i &vLocation ) {
	const int slot = findSlot( vLocation );
	if ( slot < 0 ) {
		return;
	}
	const int mapIndex = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex >= 0 ) {
		m_szRegionMap[mapIndex] = 0u;
	}
	m_szLocations[slot] = s_v2iMax;
	m_nResidentCount = MAX( 0, m_nResidentCount - 1 );
	m_bSelectionDirty = true;
}

bool T3DCpuRegionCache::loadSource( const Vector2i &vLocation, Ref<T3DRegion> &rSource ) const {
	rSource.unref();
	if ( !m_pRepository || !m_pPersistence ) {
		return false;
	}
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( pRegion->hasRuntimeMapPayloadInternal() ) {
		rSource = Ref<T3DRegion>( pRegion );
		return true;
	}
	return m_pPersistence->loadRuntimeRegionSource( pRegion->get_path(), m_pRepository->getRegionSize(), rSource );
}

bool T3DCpuRegionCache::requestSourceAsync( const Vector2i &vLocation ) {
	if ( !m_pRepository ) {
		return false;
	}
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	const int mapIndex = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex < 0 || !pRegion || pRegion->isDeleted() || pRegion->hasRuntimeMapPayloadInternal() ||
			m_szAsyncState[mapIndex] != 0u ) {
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
	const Error error = pLoader->load_threaded_request( path, "T3DRegion", true, ResourceLoader::CACHE_MODE_IGNORE );
	if ( error != OK && error != ERR_BUSY ) {
		m_szAsyncState[mapIndex] = 2u;
		return false;
	}
	m_szAsyncState[mapIndex] = 1u;
	++m_nAsyncOutstanding;
	++m_nAsyncRequestsLastTick;
	return true;
}

bool T3DCpuRegionCache::takeSourceAsync( const Vector2i &vLocation, Ref<T3DRegion> &rSource ) {
	rSource.unref();
	if ( !m_pRepository || !m_pPersistence ) {
		return false;
	}
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	const int mapIndex = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex < 0 || !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( pRegion->hasRuntimeMapPayloadInternal() ) {
		rSource = Ref<T3DRegion>( pRegion );
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
	const Ref<T3DRegion> pLoaded = pResource;
	m_szAsyncState[mapIndex] = 0u;
	m_nAsyncOutstanding = MAX( 0, m_nAsyncOutstanding - 1 );
	if ( !m_pPersistence->prepareRuntimeRegionSource( pLoaded, m_pRepository->getRegionSize(), rSource ) ) {
		return false;
	}
	++m_nAsyncReadyLastTick;
	return true;
}

void T3DCpuRegionCache::prewarmSources( const t3dResidencyFrameState_t &rFrame ) {
	if ( !m_bEnabled || rFrame.m_bAuthoring || !m_pRepository ) {
		return;
	}
	const Vector2i center = rFrame.m_predictedCenter;
	const int radius = rFrame.m_nStreamingActiveRadius;
	for ( int y = -radius; y <= radius; ++y ) {
		for ( int x = -radius; x <= radius; ++x ) {
			const Vector2i location = center + Vector2i( x, y );
			if ( !m_pRepository->isLocationValid( location ) || !m_pRepository->getRegionPtr( location ) ||
					findSlot( location ) >= 0 ) {
				continue;
			}
			T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
			if ( pRegion && !pRegion->isDeleted() && !pRegion->hasRuntimeMapPayloadInternal() ) {
				requestSourceAsync( location );
			}
		}
	}
}

void T3DCpuRegionCache::reapSourceAsync( const t3dResidencyFrameState_t &rFrame ) {
	if ( m_nAsyncOutstanding <= 0 || rFrame.m_bAuthoring || !m_pRepository ) {
		return;
	}
	if ( cookedRuntimeEnabled() ) {
		const Vector2i predictedCenter = rFrame.m_predictedCenter;
		const int activeRadius = rFrame.m_nStreamingActiveRadius;
		for ( int slot = 0; slot < m_pRepository->getSlotCount() && m_nAsyncOutstanding > 0; ++slot ) {
			const Vector2i location = m_pRepository->getSlotsData()[slot];
			const int mapIndex = location == s_v2iMax ? -1 : T3DRegionRepository::getRegionMapIndex( location );
			if ( mapIndex < 0 || m_szAsyncState[mapIndex] == 0u ) {
				continue;
			}
			const bool activeNow = rFrame.streamingState( location ) == TERRAIN3D_REGION_STREAMING_ACTIVE;
			const bool predictedActive = MAX( Math::abs( location.m_nX - predictedCenter.x ),
												 Math::abs( location.m_nY - predictedCenter.y ) ) <= activeRadius;
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
		const Vector2i location = m_pRepository->getSlotsData()[slot];
		const int mapIndex = location == s_v2iMax ? -1 : T3DRegionRepository::getRegionMapIndex( location );
		if ( mapIndex < 0 || m_szAsyncState[mapIndex] == 0u ) {
			continue;
		}
		const bool activeNow = rFrame.streamingState( location ) == TERRAIN3D_REGION_STREAMING_ACTIVE;
		const bool predictedActive = MAX( Math::abs( location.m_nX - predictedCenter.x ),
											 Math::abs( location.m_nY - predictedCenter.y ) ) <= activeRadius;
		if ( activeNow || predictedActive ) {
			continue;
		}
		T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
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

bool T3DCpuRegionCache::publishSlot( const int iSlot, const Vector2i &vLocation, const Ref<T3DRegion> &rSource ) {
	if ( iSlot < 0 || iSlot >= m_nCapacity || rSource.is_null() || !m_pRepository ) {
		return false;
	}
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	pRegion->importRuntimeMapsUnmodifiedInternal( rSource.ptr() );
	if ( !pRegion->hasRuntimeMapPayloadInternal() ) {
		return false;
	}
	const int mapIndex = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex < 0 ) {
		return false;
	}
	m_szLocations[iSlot] = vLocation;
	m_szRegionMap[mapIndex] = ( uint8_t )( iSlot + 1 );
	++m_nResidentCount;
	if ( m_pSamplingStorage ) {
		m_pSamplingStorage->publishRegionGeneration( vLocation );
	}
	return true;
}

bool T3DCpuRegionCache::publishCookedSlot( const int iSlot,
		const Vector2i &vLocation,
		const Ref<Image> &rHeight,
		const Ref<Image> &rControl,
		const Ref<Image> &rColor,
		const Ref<Image> &rDerived ) {
	if ( iSlot < 0 || iSlot >= m_nCapacity || !m_pRepository || rHeight.is_null() || rDerived.is_null() ) {
		return false;
	}
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	pRegion->adoptCookedRuntimeMapsInternal( rHeight, rControl, rColor, rDerived );
	if ( !pRegion->hasRuntimeMapPayloadInternal() ) {
		return false;
	}
	const int mapIndex = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( mapIndex < 0 ) {
		return false;
	}
	m_szLocations[iSlot] = vLocation;
	m_szRegionMap[mapIndex] = ( uint8_t )( iSlot + 1 );
	++m_nResidentCount;
	if ( m_pSamplingStorage ) {
		m_pSamplingStorage->publishRegionGeneration( vLocation );
	}
	return true;
}

bool T3DCpuRegionCache::evictSlot( const int iSlot ) {
	if ( iSlot < 0 || iSlot >= m_nCapacity || !m_pRepository ) {
		return false;
	}
	const Vector2i location = m_szLocations[iSlot];
	if ( location == s_v2iMax ) {
		return true;
	}
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
	if ( pRegion && pRegion->hasRuntimeMapPayloadInternal() && !pRegion->runtimeMapsEvictableInternal() ) {
		return false;
	}
	if ( m_pSamplingStorage ) {
		m_pSamplingStorage->invalidateResident( location );
	}
	if ( pRegion ) {
		pRegion->releaseRuntimeMapsUnmodifiedInternal();
	}
	const int mapIndex = T3DRegionRepository::getRegionMapIndex( location );
	if ( mapIndex >= 0 ) {
		m_szRegionMap[mapIndex] = 0u;
	}
	m_szLocations[iSlot] = s_v2iMax;
	m_nResidentCount = MAX( 0, m_nResidentCount - 1 );
	if ( m_pSamplingStorage ) {
		m_pSamplingStorage->publishRegionGeneration( location );
	}
	return true;
}

bool T3DCpuRegionCache::ensureResident( const Vector2i &vLocation, const bool bDemandLoad ) {
	if ( !m_bEnabled ) {
		return !m_pRepository || m_pRepository->getRegionPtr( vLocation ) != nullptr;
	}
	if ( !m_pRepository || !m_pRepository->getRegionPtr( vLocation ) ) {
		return false;
	}
	if ( findSlot( vLocation ) >= 0 ) {
		return true;
	}
	T3DRegion *const pExistingRegion = m_pRepository->getRegionPtr( vLocation );
	if ( pExistingRegion && pExistingRegion->hasRuntimeMapPayloadInternal() &&
			!pExistingRegion->runtimeMapsEvictableInternal() ) {
		return true;
	}
	Ref<T3DRegion> pSource;
	Ref<Image> pHeight, pControl, pColor, pDerived;
	const bool bCooked = cookedRuntimeEnabled();
	if ( bCooked ) {
		if ( !loadCookedMapsNow( vLocation, pHeight, pControl, pColor, pDerived ) ) {
			return false;
		}
	} else if ( !loadSource( vLocation, pSource ) ) {
		return false;
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
		const Vector2i evictionCenter = m_center == s_v2iMax ? vLocation : m_center;
		const t3dCpuCacheCoord_t center = { evictionCenter.x, evictionCenter.y };
		for ( int slot = 0; slot < m_nCapacity; ++slot ) {
			const Vector2i oldLocation = m_szLocations[slot];
			T3DRegion *const pOldRegion = m_pRepository->getRegionPtr( oldLocation );
			if ( !pOldRegion || !pOldRegion->runtimeMapsEvictableInternal() ) {
				continue;
			}
			const int distance = t3dCpuCacheDistance( { oldLocation.x, oldLocation.y }, center );
			if ( distance > farthestDistance ) {
				farthestDistance = distance;
				targetSlot = slot;
			}
		}
	}
	if ( targetSlot < 0 ) {
		return false;
	}
	if ( m_szLocations[targetSlot] != s_v2iMax ) {
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

void T3DCpuRegionCache::releaseUntrackedMaps( void ) {
	if ( !m_bEnabled || !m_pRepository ) {
		return;
	}
	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		const Vector2i location = m_pRepository->getSlotsData()[slot];
		if ( location == s_v2iMax || findSlot( location ) >= 0 ) {
			continue;
		}
		T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
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

bool T3DCpuRegionCache::restoreAllMaps( void ) {
	if ( !m_pRepository ) {
		return false;
	}
	bool bSuccess = true;
	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		const Vector2i location = m_pRepository->getSlotsData()[slot];
		T3DRegion *const pRegion = location == s_v2iMax ? nullptr : m_pRepository->getRegionPtr( location );
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
			Ref<T3DRegion> pSource;
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

int T3DCpuRegionCache::collectDesired( const t3dResidencyFrameState_t &rFrame,
		const Vector2i &rCenter,
		Vector2i *const pLocations,
		const int nCapacity ) const {
	if ( !pLocations || nCapacity <= 0 || !m_pRepository ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, s_nCpuCacheMaxLayers );
	int outputCount = 0;
	t3dCpuRegionCacheForEachSquareRing( rCenter, m_pRepository->getRegionMapSize(), [&]( const Vector2i &location ) {
		if ( outputCount >= capacity ) {
			return false;
		}
		if ( !m_pRepository->isLocationValid( location ) || !m_pRepository->getRegionPtr( location ) ||
				rFrame.streamingState( location ) != TERRAIN3D_REGION_STREAMING_ACTIVE ) {
			return true;
		}
		pLocations[outputCount++] = location;
		return outputCount < capacity;
	} );
	return outputCount;
}

bool T3DCpuRegionCache::configure( const bool bEnabled,
		const int nBudgetMb,
		const int nLoadLayers,
		const int nLoadBudgetMb ) {
	const bool bOldEnabled = m_bEnabled;
	const int oldBudget = m_nBudgetMb;
	m_bEnabled = bEnabled;
	m_nBudgetMb = CLAMP( nBudgetMb, s_nCpuRegionCacheMinBudgetMb, s_nCpuRegionCacheMaxBudgetMb );
	m_nLoadBudgetLayers = CLAMP( nLoadLayers, 1, s_nCpuRegionLoadMaxLayers );
	m_nLoadBudgetMb = CLAMP( nLoadBudgetMb, 1, s_nCpuRegionLoadMaxBudgetMb );
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
		reset( false );
		m_nCapacity = capacity;
		m_bSelectionDirty = true;
	}
	return true;
}

bool T3DCpuRegionCache::ensureSampleArea( const Vector3 &rWorldPosition, const int nRadiusTexels ) {
	if ( !m_bEnabled || !m_pRepository || m_pRepository->getRegionSize() <= 0 ||
			m_pRepository->getVertexSpacing() <= 0.0f ) {
		return true;
	}
	const int radius = MAX( 0, nRadiusTexels ) + 1;
	const Vector2i center( ( int )( Math::floor( rWorldPosition.x / m_pRepository->getVertexSpacing() ) ),
			( int )( Math::floor( rWorldPosition.z / m_pRepository->getVertexSpacing() ) ) );
	const Vector2i extent( radius * 2 + 1, radius * 2 + 1 );
	const Rect2i bounds = t3dCpuRegionCacheGetRegionBoundsForArea(
			Rect2i( center - Vector2i( radius, radius ), extent ), m_pRepository->getRegionSize() );
	bool bSuccess = true;
	for ( int y = bounds.position.y; y < bounds.get_end().y; ++y ) {
		for ( int x = bounds.position.x; x < bounds.get_end().x; ++x ) {
			const Vector2i location( x, y );
			if ( m_pRepository->getRegionPtr( location ) && !ensureResident( location, true ) ) {
				bSuccess = false;
			}
		}
	}
	return bSuccess;
}

void T3DCpuRegionCache::update( const t3dResidencyFrameState_t &rFrame,
		const bool bForce,
		const int nEffectiveLoadLayers ) {
	const T3DCpuRegionCacheScopeTimer serviceTimer( m_nServiceUsecLast, rFrame.m_bProfilingEnabled );
	setRuntimePageConfig( rFrame.m_runtimePageConfig );
	m_nLoadsLastTick = 0;
	m_nStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_nEvictionsLastTick = 0;
	m_nPinnedLastTick = 0;
	m_nLoadBytesLastTick = 0u;
	m_nAsyncRequestsLastTick = 0;
	m_nAsyncReadyLastTick = 0;
	m_nAsyncPendingLastTick = 0;
	if ( !m_bEnabled || m_nCapacity <= 0 || !m_pRepository || m_pRepository->getRegionSize() <= 0 ||
			m_pRepository->getVertexSpacing() <= 0.0f ) {
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
	Vector2i desired[s_nCpuCacheMaxLayers];
	const int desiredCount = collectDesired( rFrame, center, desired, m_nCapacity );
	m_nDesiredLastTick = desiredCount;
	uint8_t desiredMap[s_nMaxRegionSlots] = {};
	for ( int index = 0; index < desiredCount; ++index ) {
		const int mapIndex = T3DRegionRepository::getRegionMapIndex( desired[index] );
		if ( mapIndex >= 0 ) {
			desiredMap[mapIndex] = 1u;
		}
	}
	const size_t layerBytes = estimateLayerBytes();
	const size_t byteBudget = ( size_t )( m_nLoadBudgetMb )*s_nCpuCacheMib;
	const int loadLayerBudget = CLAMP( nEffectiveLoadLayers, 0, m_nLoadBudgetLayers );
	bool bAllDesiredResident = true;
	for ( int desiredIndex = 0; desiredIndex < desiredCount; ++desiredIndex ) {
		const Vector2i location = desired[desiredIndex];
		if ( findSlot( location ) >= 0 ) {
			continue;
		}
		T3DRegion *const pAvailableRegion = m_pRepository->getRegionPtr( location );
		if ( pAvailableRegion && pAvailableRegion->hasRuntimeMapPayloadInternal() &&
				!pAvailableRegion->runtimeMapsEvictableInternal() ) {
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
		Ref<T3DRegion> pSource;
		Ref<Image> pHeight, pControl, pColor, pDerived;
		const bool bCooked = cookedRuntimeEnabled();
		bool bSourceReady = false;
		if ( bCooked ) {
			if ( bForce ) {
				bSourceReady = loadCookedMapsNow( location, pHeight, pControl, pColor, pDerived );
			} else {
				bSourceReady = m_pRuntimePageStore->takeCpuMaps( location, pHeight, pControl, pColor, pDerived );
				const int mapIndex = T3DRegionRepository::getRegionMapIndex( location );
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
		int targetSlot = -1;
		for ( int slot = 0; slot < m_nCapacity; ++slot ) {
			if ( m_szLocations[slot] == s_v2iMax ) {
				targetSlot = slot;
				break;
			}
		}
		if ( targetSlot < 0 ) {
			int farthestDistance = -1;
			for ( int slot = 0; slot < m_nCapacity; ++slot ) {
				const Vector2i oldLocation = m_szLocations[slot];
				const int oldMapIndex = T3DRegionRepository::getRegionMapIndex( oldLocation );
				if ( oldMapIndex >= 0 && desiredMap[oldMapIndex] != 0u ) {
					continue;
				}
				T3DRegion *const pOldRegion = m_pRepository->getRegionPtr( oldLocation );
				if ( !pOldRegion || !pOldRegion->runtimeMapsEvictableInternal() ) {
					++m_nPinnedLastTick;
					continue;
				}
				const int distance =
						MAX( std::abs( oldLocation.x - center.m_nX ), std::abs( oldLocation.y - center.m_nY ) );
				if ( distance > farthestDistance ) {
					farthestDistance = distance;
					targetSlot = slot;
				}
			}
		}
		if ( targetSlot < 0 ) {
			m_nStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_CPU_PINNED_CAPACITY;
			continue;
		}
		if ( m_szLocations[targetSlot] != s_v2iMax ) {
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
			T3DRegion *const pAvailableRegion = m_pRepository->getRegionPtr( location );
			const bool bPinnedAvailable = pAvailableRegion && pAvailableRegion->hasRuntimeMapPayloadInternal() &&
					!pAvailableRegion->runtimeMapsEvictableInternal();
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

size_t T3DCpuRegionCache::getReservedBytes( void ) const {
	return estimateLayerBytes() * ( size_t )( MAX( 0, m_nCapacity ) );
}

size_t T3DCpuRegionCache::getMinimumBytes( void ) const {
	return estimateLayerBytes() * ( size_t )( s_nCpuCacheMinSampleLayers );
}

size_t T3DCpuRegionCache::getHardCapBytes( void ) const {
	return estimateLayerBytes() * ( size_t )( s_nCpuCacheMaxLayers );
}

t3dCpuRegionCacheStats_t T3DCpuRegionCache::getStats( void ) const {
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

Dictionary T3DCpuRegionCache::getStatsDictionary( const bool bDetailed, const int nEffectiveLoadLayers ) const {
	Dictionary stats;
	stats["enabled"] = m_bEnabled;
	stats["budget_mb"] = m_nBudgetMb;
	stats["capacity"] = m_nCapacity;
	stats["resident"] = m_nResidentCount;
	stats["desired_last_tick"] = m_nDesiredLastTick;
	stats["loads_last_tick"] = m_nLoadsLastTick;
	stats["loads_since_reset"] = ( int64_t )( m_nLoadsSinceReset );
	stats["evictions_last_tick"] = m_nEvictionsLastTick;
	stats["pinned_last_tick"] = m_nPinnedLastTick;
	stats["demand_loads"] = m_nDemandLoads;
	stats["load_bytes_last_tick"] = ( int64_t )( m_nLoadBytesLastTick );
	stats["async_requests_last_tick"] = m_nAsyncRequestsLastTick;
	stats["async_ready_last_tick"] = m_nAsyncReadyLastTick;
	stats["async_pending_last_tick"] = m_nAsyncPendingLastTick;
	stats["service_cpu_usec_last"] = ( int64_t )( m_nServiceUsecLast );
	stats["selection_pending"] = m_bSelectionDirty;
	stats["reserved_bytes"] = ( int64_t )( getReservedBytes() );
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
		const Vector2i location = m_pRepository->getSlotsData()[slot];
		const T3DRegion *const pRegion = location == s_v2iMax ? nullptr : m_pRepository->getRegionPtr( location );
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
	const size_t budgetBytes = ( size_t )( m_nBudgetMb )*s_nCpuCacheMib;
	const size_t activeHeightBytes = m_pSamplingStorage ? m_pSamplingStorage->getActiveReservedBytes() : 0u;
	const size_t defaultMapBytes = m_pRepository->getRegionSize() > 0 ? layerBytes : 0u;
	const size_t payloadBytes = layerBytes * ( size_t )( MAX( 0, totalPayloadCount ) );
	const size_t pinnedBytes = layerBytes * ( size_t )( MAX( 0, pinnedPayloadCount ) );
	const size_t totalMapBytes = activeHeightBytes + defaultMapBytes + payloadBytes;
	stats["estimated_layer_bytes"] = ( int64_t )( layerBytes );
	stats["estimated_worst_case_bytes"] = ( int64_t )( worstCaseBytes );
	stats["estimated_payload_bytes"] = ( int64_t )( payloadBytes );
	stats["estimated_pinned_bytes"] = ( int64_t )( pinnedBytes );
	stats["default_map_bytes"] = ( int64_t )( defaultMapBytes );
	stats["estimated_total_map_bytes"] = ( int64_t )( totalMapBytes );
	stats["target_map_ceiling_bytes"] = ( int64_t )( s_nXbox360CpuMapCeilingBytes );
	stats["budget_overcommitted"] = worstCaseBytes > budgetBytes;
	stats["runtime_overcommitted"] = totalPayloadCount > m_nCapacity;
	stats["target_map_ceiling_exceeded"] = totalMapBytes > s_nXbox360CpuMapCeilingBytes;
	stats["center"] = m_center;
	return stats;
}
