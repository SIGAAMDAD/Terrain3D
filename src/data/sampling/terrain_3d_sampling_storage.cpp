// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/sampling/terrain_3d_sampling_storage.h"
#include "core/utility/terrain_3d_sync.h"

#include "data/model/terrain_3d_region.h"
#include "data/persistence/terrain_3d_region_repository.h"

Terrain3DSamplingStorage::Terrain3DSamplingStorage(
	void ) {
	for ( size_t iRegion = 0u; iRegion < T3D_ARRAY_COUNT( m_szRegionSamplingGenerations ); iRegion++ ) {
		Terrain3DAtomic< uint32_t > &generation = ( m_szRegionSamplingGenerations )[iRegion];
		generation.store( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
}

Terrain3DSamplingStorage::~Terrain3DSamplingStorage(
	void ) {
	releaseActive();
}

int Terrain3DSamplingStorage::locationIndex(
	const Vector2i &rLocation ) {
	return Terrain3DRegionRepository::getRegionMapIndex( rLocation );
}

bool Terrain3DSamplingStorage::claimOrReconfigureActive(
	void ) {
	const uint32_t previousGeneration = m_nActiveStorageGeneration;
	const bool	   configured = m_activeStorage.reconfigure();
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

bool Terrain3DSamplingStorage::bindActive(
	void *const	 pBacking,
	const size_t nBackingBytes ) {
	const uint32_t previousGeneration = m_nActiveStorageGeneration;
	if ( !m_activeStorage.bind( pBacking, nBackingBytes ) ) {
		return false;
	}
	const uint32_t currentGeneration = refreshActiveGeneration();
	clearResidentPool();
	if ( currentGeneration != previousGeneration ) {
		invalidateAllSamplers();
	}
	return true;
}

void Terrain3DSamplingStorage::releaseActive(
	void ) {
	const bool bHadPublishedState = m_nActiveStorageGeneration != 0u;
	if ( isActiveOwner() ) {
		m_activeStorage.release();
	}
	m_nActiveStorageGeneration = 0u;
	if ( bHadPublishedState ) {
		invalidateAllSamplers();
	}
}

void Terrain3DSamplingStorage::resetActive(
	void ) {
	if ( !isActiveOwner() ) {
		return;
	}
	const uint32_t previousGeneration = m_nActiveStorageGeneration;
	m_activeStorage.reset();
	const uint32_t currentGeneration = refreshActiveGeneration();
	if ( currentGeneration != previousGeneration ) {
		invalidateAllSamplers();
	}
}

uint32_t Terrain3DSamplingStorage::refreshActiveGeneration(
	void ) {
	m_nActiveStorageGeneration = isActiveOwner() ? m_activeStorage.getGeneration() : 0u;
	return m_nActiveStorageGeneration;
}

bool Terrain3DSamplingStorage::containsActive(
	const Vector2i &rLocation ) const {
	return isActiveOwner() && m_activeStorage.contains( rLocation );
}

bool Terrain3DSamplingStorage::syncActiveRegion(
	const Vector2i				&rLocation,
	const Terrain3DRegion *const pRegion ) {
	return isActiveOwner() && m_activeStorage.syncRegion( rLocation, pRegion );
}

bool Terrain3DSamplingStorage::syncActiveHeightPage(
	const Vector2i		&rLocation,
	const uint8_t *const pHeightBytes,
	const size_t		 nHeightBytes ) {
	return isActiveOwner() && m_activeStorage.syncHeightPage( rLocation, pHeightBytes, nHeightBytes );
}

bool Terrain3DSamplingStorage::replaceActiveHeightPages(
	const uint8_t *const *const ppHeightPages,
	const int *const			pSlots,
	const size_t				nPageCount ) {
	if ( !isActiveOwner() || !m_activeStorage.replaceHeightPages( ppHeightPages, pSlots, nPageCount ) ) {
		return false;
	}
	refreshActiveGeneration();
	invalidateAllSamplers();
	return true;
}

bool Terrain3DSamplingStorage::syncActiveMap(
	const Vector2i				&rLocation,
	const Terrain3DRegion *const pRegion,
	const t3dMapType_t			 eType,
	const Rect2i				&rDirtyRect ) {
	return isActiveOwner() && m_activeStorage.syncMap( rLocation, pRegion, eType, rDirtyRect );
}

bool Terrain3DSamplingStorage::syncActiveDerived(
	const Vector2i				&rLocation,
	const Terrain3DRegion *const pRegion,
	const Rect2i				&rDirtyRect ) {
	return isActiveOwner() && m_activeStorage.syncDerived( rLocation, pRegion, rDirtyRect );
}

void Terrain3DSamplingStorage::invalidateActive(
	const Vector2i &rLocation ) {
	if ( isActiveOwner() ) {
		m_activeStorage.invalidate( rLocation );
	}
}

size_t Terrain3DSamplingStorage::getActiveReservedBytes(
	void ) const {
	return isActiveOwner() ? m_activeStorage.getReservedBytes() : 0u;
}

int Terrain3DSamplingStorage::getActivePhysicalCapacity(
	void ) const {
	return m_activeStorage.getPhysicalCapacity();
}

uint64_t Terrain3DSamplingStorage::getActiveReaderRetryCount(
	void ) const {
	return m_activeStorage.getReaderRetryCount();
}

uint64_t Terrain3DSamplingStorage::getActiveReaderMutexCount(
	void ) const {
	return m_activeStorage.getReaderMutexCount();
}

uint64_t Terrain3DSamplingStorage::getActivePublicationCount(
	void ) const {
	return m_activeStorage.getPublicationCount();
}

Terrain3DResidentMapLease Terrain3DSamplingStorage::acquireResident(
	const Vector2i				&rLocation,
	const Terrain3DRegion *const pRegion ) const {
	if ( isActiveOwner() || m_residentPool.getCapacity() <= 0 || !pRegion || pRegion->isDeleted() ) {
		return {};
	}
	return m_residentPool.acquire( rLocation, pRegion );
}

bool Terrain3DSamplingStorage::bindRuntimeAllocator(
	Terrain3DMemoryZone *const pZone ) {
	return m_residentPool.bindRuntimeAllocator( pZone );
}

bool Terrain3DSamplingStorage::configureResidentPool(
	const bool bControlMapsImplicit,
	const int  nRequestedCapacity ) {
	if ( isActiveOwner() ) {
		clearResidentPool();
		return true;
	}
	uint32_t channels = RESIDENT_CHANNEL_HEIGHT;
	if ( !bControlMapsImplicit ) {
		channels |= RESIDENT_CHANNEL_CONTROL;
	}
	const size_t					 bytesPerRegion = Terrain3DResidentRegionPool::estimateSlotBytes( channels );
	const size_t					 bytesPerSlot = bytesPerRegion + sizeof( t3dSlot_t );
	const Terrain3DMemoryZone *const pZone = m_residentPool.getRuntimeAllocatorZone();
	if ( !pZone || bytesPerSlot == 0u ) {
		return false;
	}
	const int parentCapacity = ( int ) MIN< size_t >( T3D_REGION_SLOTS, pZone->freeBytes() / bytesPerSlot );
	if ( parentCapacity <= 0 ) {
		return false;
	}
	const int	   capacity = CLAMP( MIN( nRequestedCapacity, parentCapacity ), 1, T3D_REGION_SLOTS );
	const int	   oldCapacity = m_residentPool.getCapacity();
	const uint32_t oldChannels = m_residentPool.getChannels();
	if ( !m_residentPool.configure( capacity, channels ) ) {
		return false;
	}
	if ( oldCapacity != capacity || oldChannels != channels ) {
		invalidateAllSamplers();
	}
	return true;
}

void Terrain3DSamplingStorage::clearResidentPool(
	void ) {
	if ( m_residentPool.getCapacity() > 0 ) {
		m_residentPool.clear();
	}
}

void Terrain3DSamplingStorage::resetResidentPool(
	void ) {
	if ( m_residentPool.getCapacity() > 0 ) {
		m_residentPool.resetResidency();
	}
}

void Terrain3DSamplingStorage::invalidateResident(
	const Vector2i &rLocation ) {
	if ( m_residentPool.getCapacity() > 0 ) {
		m_residentPool.invalidate( rLocation );
	}
}

void Terrain3DSamplingStorage::syncResidentMap(
	const Vector2i				&rLocation,
	const Terrain3DRegion *const pRegion,
	const t3dMapType_t			 eType,
	const Rect2i				&rDirtyRect ) {
	if ( m_residentPool.getCapacity() > 0 ) {
		m_residentPool.syncMap( rLocation, pRegion, eType, rDirtyRect );
	}
}

void Terrain3DSamplingStorage::syncResidentDerived(
	const Vector2i				&rLocation,
	const Terrain3DRegion *const pRegion,
	const Rect2i				&rDirtyRect ) {
	if ( m_residentPool.getCapacity() > 0 ) {
		m_residentPool.syncDerived( rLocation, pRegion, rDirtyRect );
	}
}

size_t Terrain3DSamplingStorage::getResidentReservedBytes(
	void ) const {
	return m_residentPool.getReservedBytes();
}

void Terrain3DSamplingStorage::invalidateAllSamplers(
	void ) {
	const uint64_t next = m_nSamplingGeneration.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
	if ( next == 0u ) {
		m_nSamplingGeneration.store( 1u, T3D_MEMORY_ORDER_RELEASE );
	}
}

void Terrain3DSamplingStorage::publishRegionGeneration(
	const Vector2i &rLocation ) {
	const int index = locationIndex( rLocation );
	if ( index < 0 ) {
		return;
	}
	Terrain3DAtomic< uint32_t > &generation = m_szRegionSamplingGenerations[( size_t ) ( index )];
	const uint32_t				 next = generation.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
	if ( next == 0u ) {
		generation.store( 1u, T3D_MEMORY_ORDER_RELEASE );
	}
}
