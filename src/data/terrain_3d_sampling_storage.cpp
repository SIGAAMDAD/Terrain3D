// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/terrain_3d_sampling_storage.h"

#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_repository.h"

#include <algorithm>

T3DSamplingStorage::T3DSamplingStorage( void ) {
	for ( std::atomic<uint32_t> &generation : m_szRegionSamplingGenerations ) {
		generation.store( 1u, std::memory_order_relaxed );
	}
}

T3DSamplingStorage::~T3DSamplingStorage( void ) {
	releaseActive();
}

int T3DSamplingStorage::locationIndex( const Vector2i &rLocation ) {
	return T3DRegionRepository::getRegionMapIndex( rLocation );
}

bool T3DSamplingStorage::claimOrReconfigureActive( const int nRegionSize ) {
	const uint32_t previousGeneration = m_nActiveStorageGeneration;
	const bool configured = isActiveOwner() ? t3dActiveStorageReconfigure( this, nRegionSize )
											: t3dActiveStorageClaim( this, nRegionSize );
	if ( !configured ) {
		return false;
	}
	const uint32_t currentGeneration = refreshActiveGeneration();
	clearResidentPool();
	if ( currentGeneration != previousGeneration ) {
		invalidateAllSamplers();
	}
	return true;
}

bool T3DSamplingStorage::bindActive( const int nRegionSize, void *const pBacking, const size_t nBackingBytes ) {
	const uint32_t previousGeneration = m_nActiveStorageGeneration;
	if ( !t3dActiveStorageBind( this, nRegionSize, pBacking, nBackingBytes ) ) {
		return false;
	}
	const uint32_t currentGeneration = refreshActiveGeneration();
	clearResidentPool();
	if ( currentGeneration != previousGeneration ) {
		invalidateAllSamplers();
	}
	return true;
}

void T3DSamplingStorage::releaseActive( void ) {
	const bool bHadPublishedState = m_nActiveStorageGeneration != 0u;
	if ( isActiveOwner() ) {
		t3dActiveStorageRelease( this );
	}
	m_nActiveStorageGeneration = 0u;
	if ( bHadPublishedState ) {
		invalidateAllSamplers();
	}
}

void T3DSamplingStorage::resetActive( void ) {
	if ( !isActiveOwner() ) {
		return;
	}
	const uint32_t previousGeneration = m_nActiveStorageGeneration;
	t3dActiveStorageReset( this );
	const uint32_t currentGeneration = refreshActiveGeneration();
	if ( currentGeneration != previousGeneration ) {
		invalidateAllSamplers();
	}
}

uint32_t T3DSamplingStorage::refreshActiveGeneration( void ) {
	m_nActiveStorageGeneration = isActiveOwner() ? t3dActiveStorageGetOwnerGeneration( this ) : 0u;
	return m_nActiveStorageGeneration;
}

bool T3DSamplingStorage::containsActive( const Vector2i &rLocation ) const {
	return isActiveOwner() && t3dActiveStorageContains( this, rLocation );
}

bool T3DSamplingStorage::syncActiveRegion( const Vector2i &rLocation, const T3DRegion *const pRegion ) {
	return isActiveOwner() && t3dActiveStorageSyncRegion( this, rLocation, pRegion );
}

bool T3DSamplingStorage::syncActiveHeightPage( const Vector2i &rLocation,
		const uint8_t *const pHeightBytes,
		const size_t nHeightBytes ) {
	return isActiveOwner() && t3dActiveStorageSyncHeightPage( this, rLocation, pHeightBytes, nHeightBytes );
}

bool T3DSamplingStorage::replaceActiveHeightPages( const int nRegionSize,
		const uint8_t *const *const ppHeightPages,
		const int *const pSlots,
		const size_t nPageCount ) {
	if ( !isActiveOwner() ||
			!t3dActiveStorageReplaceHeightPages( this, nRegionSize, ppHeightPages, pSlots, nPageCount ) ) {
		return false;
	}
	refreshActiveGeneration();
	invalidateAllSamplers();
	return true;
}

bool T3DSamplingStorage::syncActiveMap( const Vector2i &rLocation,
		const T3DRegion *const pRegion,
		const t3dMapType_t eType,
		const Rect2i &rDirtyRect ) {
	return isActiveOwner() && t3dActiveStorageSyncMap( this, rLocation, pRegion, eType, rDirtyRect );
}

bool T3DSamplingStorage::syncActiveDerived( const Vector2i &rLocation,
		const T3DRegion *const pRegion,
		const Rect2i &rDirtyRect ) {
	return isActiveOwner() && t3dActiveStorageSyncDerived( this, rLocation, pRegion, rDirtyRect );
}

void T3DSamplingStorage::invalidateActive( const Vector2i &rLocation ) {
	if ( isActiveOwner() ) {
		t3dActiveStorageInvalidate( this, rLocation );
	}
}

size_t T3DSamplingStorage::getActiveReservedBytes( void ) const {
	return isActiveOwner() ? t3dActiveStorageGetReservedBytes() : 0u;
}

int T3DSamplingStorage::getActivePhysicalCapacity( void ) const {
	return t3dActiveStorageGetPhysicalCapacity();
}

uint64_t T3DSamplingStorage::getActiveReaderRetryCount( void ) const {
	return t3dActiveStorageGetReaderRetryCount();
}

uint64_t T3DSamplingStorage::getActiveReaderMutexCount( void ) const {
	return t3dActiveStorageGetReaderMutexCount();
}

uint64_t T3DSamplingStorage::getActivePublicationCount( void ) const {
	return t3dActiveStorageGetPublicationCount();
}

T3DResidentMapLease T3DSamplingStorage::acquireResident( const Vector2i &rLocation,
		const T3DRegion *const pRegion ) const {
	if ( isActiveOwner() || m_residentPool.getCapacity() <= 0 || !pRegion || pRegion->isDeleted() ) {
		return {};
	}
	return m_residentPool.acquire( rLocation, pRegion );
}

bool T3DSamplingStorage::configureResidentPool( const int nRegionSize,
		const int nRegionMapSize,
		const bool bControlMapsImplicit,
		const int nRequestedCapacity,
		const size_t nBudgetBytes ) {
	if ( isActiveOwner() ) {
		clearResidentPool();
		return true;
	}
	if ( nRegionSize <= 0 || nRegionMapSize <= 0 ) {
		return false;
	}

	uint32_t channels = T3DResidentRegionPool::CHANNEL_HEIGHT;
	if ( !bControlMapsImplicit ) {
		channels |= T3DResidentRegionPool::CHANNEL_CONTROL;
	}
	const size_t bytesPerRegion = T3DResidentRegionPool::estimateSlotBytes( nRegionSize, channels );
	const int budgetCapacity = bytesPerRegion > 0u ? ( int )( MAX<size_t>( 1u, nBudgetBytes / bytesPerRegion ) ) : 1;
	const int capacity = CLAMP( MIN( nRequestedCapacity, budgetCapacity ), 1, nRegionMapSize * nRegionMapSize );
	const int oldRegionSize = m_residentPool.getRegionSize();
	const int oldCapacity = m_residentPool.getCapacity();
	const uint32_t oldChannels = m_residentPool.getChannels();
	if ( !m_residentPool.configure( nRegionSize, capacity, channels ) ) {
		return false;
	}
	if ( oldRegionSize != nRegionSize || oldCapacity != capacity || oldChannels != channels ) {
		invalidateAllSamplers();
	}
	return true;
}

void T3DSamplingStorage::clearResidentPool( void ) {
	if ( m_residentPool.getCapacity() > 0 ) {
		m_residentPool.clear();
	}
}

void T3DSamplingStorage::resetResidentPool( void ) {
	if ( m_residentPool.getCapacity() > 0 ) {
		m_residentPool.resetResidency();
	}
}

void T3DSamplingStorage::invalidateResident( const Vector2i &rLocation ) {
	if ( m_residentPool.getCapacity() > 0 ) {
		m_residentPool.invalidate( rLocation );
	}
}

void T3DSamplingStorage::syncResidentMap( const Vector2i &rLocation,
		const T3DRegion *const pRegion,
		const t3dMapType_t eType,
		const Rect2i &rDirtyRect ) {
	if ( m_residentPool.getCapacity() > 0 ) {
		m_residentPool.syncMap( rLocation, pRegion, eType, rDirtyRect );
	}
}

void T3DSamplingStorage::syncResidentDerived( const Vector2i &rLocation,
		const T3DRegion *const pRegion,
		const Rect2i &rDirtyRect ) {
	if ( m_residentPool.getCapacity() > 0 ) {
		m_residentPool.syncDerived( rLocation, pRegion, rDirtyRect );
	}
}

size_t T3DSamplingStorage::getResidentReservedBytes( void ) const {
	return m_residentPool.getReservedBytes();
}

void T3DSamplingStorage::invalidateAllSamplers( void ) {
	const uint64_t next = m_nSamplingGeneration.fetch_add( 1u, std::memory_order_acq_rel ) + 1u;
	if ( next == 0u ) {
		m_nSamplingGeneration.store( 1u, std::memory_order_release );
	}
}

void T3DSamplingStorage::publishRegionGeneration( const Vector2i &rLocation ) {
	const int index = locationIndex( rLocation );
	if ( index < 0 ) {
		return;
	}
	std::atomic<uint32_t> &generation = m_szRegionSamplingGenerations[( size_t )( index )];
	const uint32_t next = generation.fetch_add( 1u, std::memory_order_acq_rel ) + 1u;
	if ( next == 0u ) {
		generation.store( 1u, std::memory_order_release );
	}
}
