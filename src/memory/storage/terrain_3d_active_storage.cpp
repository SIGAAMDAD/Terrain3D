// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "memory/storage/terrain_3d_active_storage.h"

#include "data/persistence/terrain_3d_region_codec.h"
#include "memory/core/terrain_3d_memory.h"

#include <EASTL/numeric_limits.h>
#include <string.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

static void t3dActiveLeaseReleaseThunk(
	void *const pContext,
	const int	iPage ) {
	( ( Terrain3DActiveStorage * ) pContext )->releaseLease( iPage );
}

Terrain3DActiveStorage::Terrain3DActiveStorage(
	void ) {
	for ( int slot = 0; slot < T3D_REGION_SLOTS; ++slot ) {
		m_szLogicalToPhysical[slot].store( T3D_ACTIVE_INVALID_PAGE, T3D_MEMORY_ORDER_RELAXED );
	}
}

Terrain3DActiveStorage::~Terrain3DActiveStorage(
	void ) {
	release();
}

size_t Terrain3DActiveStorage::requiredBytes(
	void ) {
	const size_t page_bytes = activePageBytes();
	return page_bytes <= eastl::numeric_limits< size_t >::max() / ( size_t ) ( T3D_ACTIVE_PHYSICAL_PAGE_COUNT )
			   ? page_bytes * ( size_t ) ( T3D_ACTIVE_PHYSICAL_PAGE_COUNT )
			   : 0u;
}

int Terrain3DActiveStorage::locationToSlot(
	const Vector2i &rLocation ) {
	const int64_t x = ( int64_t ) rLocation.x + T3D_REGION_MAP_HALF;
	const int64_t y = ( int64_t ) rLocation.y + T3D_REGION_MAP_HALF;
	if ( x < 0 || x >= T3D_REGION_MAP_SIZE || y < 0 || y >= T3D_REGION_MAP_SIZE ) {
		return -1;
	}
	return ( int ) ( ( y << T3D_REGION_MAP_SHIFT ) + x );
}

void Terrain3DActiveStorage::bumpGeneration(
	Terrain3DAtomic< uint32_t > &rGeneration ) {
	const uint32_t next = rGeneration.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
	if ( next == 0u ) {
		rGeneration.store( 1u, T3D_MEMORY_ORDER_RELEASE );
	}
}

void Terrain3DActiveStorage::clearMappingsUnsafe(
	void ) {
	for ( int slot = 0; slot < T3D_REGION_SLOTS; ++slot ) {
		m_szLogicalToPhysical[slot].store( T3D_ACTIVE_INVALID_PAGE, T3D_MEMORY_ORDER_RELEASE );
	}
	for ( int page = 0; page < T3D_ACTIVE_PHYSICAL_PAGE_COUNT; ++page ) {
		m_szPhysicalPages[page].m_iLogicalSlot.store( T3D_ACTIVE_INVALID_PAGE, T3D_MEMORY_ORDER_RELEASE );
		if ( m_szPhysicalPages[page].m_nPins.load( T3D_MEMORY_ORDER_ACQUIRE ) == 0u ) {
			m_szPhysicalPages[page].m_nState.store( TERRAIN3D_ACTIVE_PAGE_FREE, T3D_MEMORY_ORDER_RELEASE );
		} else {
			m_szPhysicalPages[page].m_nState.store( TERRAIN3D_ACTIVE_PAGE_RETIRING, T3D_MEMORY_ORDER_RELEASE );
		}
		bumpGeneration( m_szPhysicalPages[page].m_nGeneration );
	}
}

void Terrain3DActiveStorage::waitForReadersUnsafe(
	void ) {
	for ( ;; ) {
		bool idle = true;
		for ( int page = 0; page < T3D_ACTIVE_PHYSICAL_PAGE_COUNT; ++page ) {
			if ( m_szPhysicalPages[page].m_nPins.load( T3D_MEMORY_ORDER_ACQUIRE ) != 0u ) {
				idle = false;
				break;
			}
		}
		if ( idle ) {
			return;
		}
		t3dThreadYield();
	}
}

void Terrain3DActiveStorage::freeBackingUnsafe(
	void ) {
	if ( m_bHeightBackingOwned ) {
		t3dMemoryRelease( m_pHeightBackingRaw );
	}
	m_pHeightBackingRaw = NULL;
	m_pHeightBacking = NULL;
	m_bHeightBackingOwned = false;
	for ( int page = 0; page < T3D_ACTIVE_PHYSICAL_PAGE_COUNT; ++page ) {
		m_szPhysicalPages[page].m_pHeight = NULL;
		m_szPhysicalPages[page].m_nState.store( TERRAIN3D_ACTIVE_PAGE_FREE, T3D_MEMORY_ORDER_RELAXED );
	}
	m_nHeightBytes.store( 0u, T3D_MEMORY_ORDER_RELEASE );
}

bool Terrain3DActiveStorage::configureBackingUnsafe(
	void *const	 pBacking,
	const size_t nBackingBytes,
	const bool	 bOwned ) {
	const size_t required = requiredBytes();
	if ( required == 0u || !pBacking || nBackingBytes < required ) {
		return false;
	}
	const uintptr_t address = ( uintptr_t ) ( pBacking );
	const uintptr_t aligned =
		( address + T3D_ACTIVE_STORAGE_ALIGNMENT - 1u ) & ~( uintptr_t ) ( T3D_ACTIVE_STORAGE_ALIGNMENT - 1u );
	const size_t alignment_slop = ( size_t ) ( aligned - address );
	if ( alignment_slop > nBackingBytes || nBackingBytes - alignment_slop < required ) {
		return false;
	}
	m_pHeightBackingRaw = pBacking;
	m_pHeightBacking = ( uint8_t * ) aligned;
	m_bHeightBackingOwned = bOwned;
	memset( m_pHeightBacking, 0, required );
	const size_t page_bytes = activePageBytes();
	for ( int page = 0; page < T3D_ACTIVE_PHYSICAL_PAGE_COUNT; ++page ) {
		m_szPhysicalPages[page].m_pHeight = ( uint16_t * ) ( m_pHeightBacking + ( size_t ) ( page ) *page_bytes );
		m_szPhysicalPages[page].m_nPins.store( 0u, T3D_MEMORY_ORDER_RELAXED );
		m_szPhysicalPages[page].m_nGeneration.store( 1u, T3D_MEMORY_ORDER_RELAXED );
		m_szPhysicalPages[page].m_iLogicalSlot.store( T3D_ACTIVE_INVALID_PAGE, T3D_MEMORY_ORDER_RELAXED );
		m_szPhysicalPages[page].m_nState.store( TERRAIN3D_ACTIVE_PAGE_FREE, T3D_MEMORY_ORDER_RELAXED );
	}
	m_nHeightBytes.store( required, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

bool Terrain3DActiveStorage::allocateBackingUnsafe(
	void ) {
	const size_t required = requiredBytes();
	if ( required == 0u || required > eastl::numeric_limits< size_t >::max() - T3D_ACTIVE_STORAGE_ALIGNMENT ) {
		return false;
	}
	void *const pRaw = t3dMemoryAllocate( required + T3D_ACTIVE_STORAGE_ALIGNMENT - 1u );
	if ( !pRaw ) {
		return false;
	}
	if ( !configureBackingUnsafe( pRaw, required + T3D_ACTIVE_STORAGE_ALIGNMENT - 1u, true ) ) {
		t3dMemoryRelease( pRaw );
		return false;
	}
	return true;
}

int Terrain3DActiveStorage::chooseStagingPageUnsafe(
	const int iPreservedPage ) {
	for ( int pass = 0; pass < 2; ++pass ) {
		for ( int offset = 0; offset < T3D_ACTIVE_PHYSICAL_PAGE_COUNT; ++offset ) {
			const int page = ( int ) ( ( m_nClockHand + ( uint32_t ) ( offset ) ) %
									   ( uint32_t ) ( T3D_ACTIVE_PHYSICAL_PAGE_COUNT ) );
			if ( page == iPreservedPage ) {
				continue;
			}
			t3dActivePhysicalPage_t &rPage = m_szPhysicalPages[page];
			const uint32_t			 state = rPage.m_nState.load( T3D_MEMORY_ORDER_ACQUIRE );
			if ( rPage.m_nPins.load( T3D_MEMORY_ORDER_ACQUIRE ) != 0u ) {
				continue;
			}
			if ( pass == 0 && state != TERRAIN3D_ACTIVE_PAGE_FREE && state != TERRAIN3D_ACTIVE_PAGE_RETIRING ) {
				continue;
			}
			if ( pass == 1 && state == TERRAIN3D_ACTIVE_PAGE_PUBLISHED ) {
				const int victim_slot = rPage.m_iLogicalSlot.load( T3D_MEMORY_ORDER_ACQUIRE );
				if ( victim_slot >= 0 && victim_slot < T3D_REGION_SLOTS ) {
					int expected = page;
					m_szLogicalToPhysical[victim_slot].compare_exchange_strong( expected,
																				T3D_ACTIVE_INVALID_PAGE,
																				T3D_MEMORY_ORDER_ACQ_REL );
				}
				rPage.m_nState.store( TERRAIN3D_ACTIVE_PAGE_RETIRING, T3D_MEMORY_ORDER_RELEASE );
				if ( rPage.m_nPins.load( T3D_MEMORY_ORDER_ACQUIRE ) != 0u ) {
					continue;
				}
			} else if ( pass == 1 && state != TERRAIN3D_ACTIVE_PAGE_FREE && state != TERRAIN3D_ACTIVE_PAGE_RETIRING ) {
				continue;
			}
			m_nClockHand = ( uint32_t ) ( page + 1 );
			return page;
		}
	}
	return T3D_ACTIVE_INVALID_PAGE;
}

bool Terrain3DActiveStorage::publishBytesUnsafe(
	const int			 iLogicalSlot,
	const uint8_t *const pHeightBytes,
	const size_t		 nHeightBytes ) {
	const size_t expected_bytes = activePageBytes();
	if ( iLogicalSlot < 0 || iLogicalSlot >= T3D_REGION_SLOTS || !pHeightBytes || nHeightBytes != expected_bytes ) {
		return false;
	}
	const int old_page = m_szLogicalToPhysical[iLogicalSlot].load( T3D_MEMORY_ORDER_ACQUIRE );
	const int staging_page = chooseStagingPageUnsafe( old_page );
	if ( staging_page < 0 ) {
		return false;
	}
	t3dActivePhysicalPage_t &rStaging = m_szPhysicalPages[staging_page];
	rStaging.m_nState.store( TERRAIN3D_ACTIVE_PAGE_STAGING, T3D_MEMORY_ORDER_RELEASE );
	rStaging.m_iLogicalSlot.store( iLogicalSlot, T3D_MEMORY_ORDER_RELEASE );
	bumpGeneration( rStaging.m_nGeneration );
	memcpy( rStaging.m_pHeight, pHeightBytes, expected_bytes );
	rStaging.m_nState.store( TERRAIN3D_ACTIVE_PAGE_PUBLISHED, T3D_MEMORY_ORDER_RELEASE );
	const int replaced_page = m_szLogicalToPhysical[iLogicalSlot].exchange( staging_page, T3D_MEMORY_ORDER_ACQ_REL );
	if ( replaced_page >= 0 && replaced_page != staging_page ) {
		t3dActivePhysicalPage_t &rReplaced = m_szPhysicalPages[replaced_page];
		rReplaced.m_iLogicalSlot.store( T3D_ACTIVE_INVALID_PAGE, T3D_MEMORY_ORDER_RELEASE );
		rReplaced.m_nState.store( TERRAIN3D_ACTIVE_PAGE_RETIRING, T3D_MEMORY_ORDER_RELEASE );
	}
	m_nPublications.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	return true;
}

bool Terrain3DActiveStorage::copyRegionUnsafe(
	const int					 iLogicalSlot,
	const Terrain3DRegion *const pRegion ) {
	const Image *const pImage = pRegion ? pRegion->getMapPtr( TYPE_HEIGHT ) : NULL;
	PackedByteArray	   bytes;
	if ( pImage && pImage->get_format() == Image::FORMAT_R16 && pImage->get_width() == T3D_REGION_SIZE &&
		 pImage->get_height() == T3D_REGION_SIZE ) {
		bytes = pImage->get_data();
	} else if ( !pImage ) {
		const size_t expected_bytes = activePageBytes();
		bytes.resize( ( int64_t ) ( expected_bytes ) );
		uint8_t *const pValues = bytes.ptrw();
		const uint16_t	flat_height = t3dRegionCodecEncodeHeightU16( 0.0f );
		for ( size_t iValue = 0u; iValue < expected_bytes / sizeof( uint16_t ); ++iValue ) {
			memcpy( pValues + iValue * sizeof( uint16_t ), &flat_height, sizeof( flat_height ) );
		}
	} else {
		return false;
	}
	return publishBytesUnsafe( iLogicalSlot, bytes.ptr(), ( size_t ) ( bytes.size() ) );
}

void Terrain3DActiveStorage::releaseReader(
	const int iPhysicalPage ) {
	if ( iPhysicalPage >= 0 && iPhysicalPage < T3D_ACTIVE_PHYSICAL_PAGE_COUNT ) {
		m_szPhysicalPages[iPhysicalPage].m_nPins.fetch_sub( 1u, T3D_MEMORY_ORDER_RELEASE );
	}
}

bool Terrain3DActiveStorage::bind(
	void *const	 pBacking,
	const size_t nBackingBytes ) {
	if ( !pBacking || ( ( uintptr_t ) ( pBacking ) & ( T3D_ACTIVE_STORAGE_ALIGNMENT - 1u ) ) != 0u ) {
		return false;
	}
	Terrain3DScopedLock lock( m_writerMutex );
	m_nState.store( TERRAIN3D_ACTIVE_STORAGE_DRAINING, T3D_MEMORY_ORDER_RELEASE );
	clearMappingsUnsafe();
	waitForReadersUnsafe();
	freeBackingUnsafe();
	if ( !configureBackingUnsafe( pBacking, nBackingBytes, false ) ) {
		m_nState.store( TERRAIN3D_ACTIVE_STORAGE_FREE, T3D_MEMORY_ORDER_RELEASE );
		return false;
	}
	for ( int slot = 0; slot < T3D_REGION_SLOTS; ++slot ) {
		m_szLogicalToPhysical[slot].store( T3D_ACTIVE_INVALID_PAGE, T3D_MEMORY_ORDER_RELAXED );
	}
	m_nClockHand = 0u;
	bumpGeneration( m_nGeneration );
	m_nState.store( TERRAIN3D_ACTIVE_STORAGE_ACTIVE, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

bool Terrain3DActiveStorage::claim(
	void ) {
	Terrain3DScopedLock lock( m_writerMutex );
	const uint32_t		state = m_nState.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( state != TERRAIN3D_ACTIVE_STORAGE_FREE ) {
		return state == TERRAIN3D_ACTIVE_STORAGE_ACTIVE;
	}
	m_nState.store( TERRAIN3D_ACTIVE_STORAGE_DRAINING, T3D_MEMORY_ORDER_RELEASE );
	if ( !allocateBackingUnsafe() ) {
		m_nState.store( TERRAIN3D_ACTIVE_STORAGE_FREE, T3D_MEMORY_ORDER_RELEASE );
		return false;
	}
	for ( int slot = 0; slot < T3D_REGION_SLOTS; ++slot ) {
		m_szLogicalToPhysical[slot].store( T3D_ACTIVE_INVALID_PAGE, T3D_MEMORY_ORDER_RELAXED );
	}
	m_nClockHand = 0u;
	bumpGeneration( m_nGeneration );
	m_nState.store( TERRAIN3D_ACTIVE_STORAGE_ACTIVE, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

bool Terrain3DActiveStorage::reconfigure(
	void ) {
	return isActive() ? true : claim();
}

void Terrain3DActiveStorage::release(
	void ) {
	Terrain3DScopedLock lock( m_writerMutex );
	if ( m_nState.load( T3D_MEMORY_ORDER_ACQUIRE ) == TERRAIN3D_ACTIVE_STORAGE_FREE ) {
		return;
	}
	m_nState.store( TERRAIN3D_ACTIVE_STORAGE_DRAINING, T3D_MEMORY_ORDER_RELEASE );
	clearMappingsUnsafe();
	waitForReadersUnsafe();
	freeBackingUnsafe();
	m_nClockHand = 0u;
	bumpGeneration( m_nGeneration );
	m_nState.store( TERRAIN3D_ACTIVE_STORAGE_FREE, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DActiveStorage::reset(
	void ) {
	Terrain3DScopedLock lock( m_writerMutex );
	if ( !isActive() ) {
		return;
	}
	clearMappingsUnsafe();
	m_nClockHand = 0u;
	bumpGeneration( m_nGeneration );
}

bool Terrain3DActiveStorage::contains(
	const Vector2i &rLocation ) const {
	if ( !isActive() ) {
		return false;
	}
	const int logical_slot = locationToSlot( rLocation );
	if ( logical_slot < 0 ) {
		return false;
	}
	const int page = m_szLogicalToPhysical[logical_slot].load( T3D_MEMORY_ORDER_ACQUIRE );
	return page >= 0 && page < T3D_ACTIVE_PHYSICAL_PAGE_COUNT &&
		   m_szPhysicalPages[page].m_nState.load( T3D_MEMORY_ORDER_ACQUIRE ) == TERRAIN3D_ACTIVE_PAGE_PUBLISHED &&
		   m_szPhysicalPages[page].m_iLogicalSlot.load( T3D_MEMORY_ORDER_ACQUIRE ) == logical_slot;
}

Terrain3DActiveMapLease Terrain3DActiveStorage::acquire(
	const Vector2i &rLocation ) {
	if ( !isActive() ) {
		return {};
	}
	const int logical_slot = locationToSlot( rLocation );
	if ( logical_slot < 0 ) {
		return {};
	}
	for ( int attempt = 0; attempt < 4; ++attempt ) {
		const int page = m_szLogicalToPhysical[logical_slot].load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( page < 0 || page >= T3D_ACTIVE_PHYSICAL_PAGE_COUNT ) {
			return {};
		}
		t3dActivePhysicalPage_t &rPage = m_szPhysicalPages[page];
		const uint32_t			 generation = rPage.m_nGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( rPage.m_nState.load( T3D_MEMORY_ORDER_ACQUIRE ) != TERRAIN3D_ACTIVE_PAGE_PUBLISHED ||
			 rPage.m_iLogicalSlot.load( T3D_MEMORY_ORDER_ACQUIRE ) != logical_slot ) {
			m_nReaderRetries.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			continue;
		}
		rPage.m_nPins.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
		const bool valid = isActive() && m_szLogicalToPhysical[logical_slot].load( T3D_MEMORY_ORDER_ACQUIRE ) == page &&
						   rPage.m_nState.load( T3D_MEMORY_ORDER_ACQUIRE ) == TERRAIN3D_ACTIVE_PAGE_PUBLISHED &&
						   rPage.m_iLogicalSlot.load( T3D_MEMORY_ORDER_ACQUIRE ) == logical_slot &&
						   rPage.m_nGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) == generation;
		if ( valid ) {
			t3dActiveMapView_t view;
			view.m_pHeight = rPage.m_pHeight;
			view.m_nOwnerGeneration = m_nGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
			view.m_nRegionGeneration = generation;
			view.m_iSlot = logical_slot;
			return Terrain3DActiveMapLease( this, &t3dActiveLeaseReleaseThunk, page, view );
		}
		rPage.m_nPins.fetch_sub( 1u, T3D_MEMORY_ORDER_RELEASE );
		m_nReaderRetries.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	return {};
}

bool Terrain3DActiveStorage::syncRegion(
	const Vector2i				&rLocation,
	const Terrain3DRegion *const pRegion ) {
	if ( !isActive() || !pRegion || pRegion->isDeleted() || pRegion->getRegionSize() != T3D_REGION_SIZE ) {
		return false;
	}
	const int logical_slot = locationToSlot( rLocation );
	if ( logical_slot < 0 ) {
		return false;
	}
	Terrain3DScopedLock lock( m_writerMutex );
	return isActive() && copyRegionUnsafe( logical_slot, pRegion );
}

bool Terrain3DActiveStorage::syncHeightPage(
	const Vector2i		&rLocation,
	const uint8_t *const pHeightBytes,
	const size_t		 nHeightBytes ) {
	if ( !isActive() || !pHeightBytes ) {
		return false;
	}
	const int logical_slot = locationToSlot( rLocation );
	if ( logical_slot < 0 ) {
		return false;
	}
	Terrain3DScopedLock lock( m_writerMutex );
	return isActive() && publishBytesUnsafe( logical_slot, pHeightBytes, nHeightBytes );
}

bool Terrain3DActiveStorage::replaceHeightPages(
	const uint8_t *const *const pHeightPages,
	const int *const			pSlots,
	const size_t				nPageCount ) {
	if ( nPageCount > ( size_t ) ( T3D_ACTIVE_PHYSICAL_PAGE_COUNT ) ||
		 ( nPageCount > 0u && ( !pHeightPages || !pSlots ) ) ) {
		return false;
	}
	for ( size_t index = 0; index < nPageCount; ++index ) {
		if ( pSlots[index] < 0 || pSlots[index] >= T3D_REGION_SLOTS || !pHeightPages[index] ) {
			return false;
		}
		for ( size_t previous = 0; previous < index; ++previous ) {
			if ( pSlots[previous] == pSlots[index] ) {
				return false;
			}
		}
	}
	Terrain3DScopedLock lock( m_writerMutex );
	if ( !isActive() ) {
		return false;
	}
	clearMappingsUnsafe();
	const size_t page_bytes = activePageBytes();
	for ( size_t index = 0; index < nPageCount; ++index ) {
		if ( !publishBytesUnsafe( pSlots[index], pHeightPages[index], page_bytes ) ) {
			clearMappingsUnsafe();
			return false;
		}
	}
	bumpGeneration( m_nGeneration );
	return true;
}

bool Terrain3DActiveStorage::syncMap(
	const Vector2i				&rLocation,
	const Terrain3DRegion *const pRegion,
	const t3dMapType_t			 type,
	const Rect2i & ) {
	if ( type < TYPE_HEIGHT || type >= REGION_TYPE_MAX ) {
		return false;
	}
	return type == TYPE_HEIGHT ? syncRegion( rLocation, pRegion ) : isActive();
}

bool Terrain3DActiveStorage::syncDerived(
	const Vector2i				&rLocation,
	const Terrain3DRegion *const pRegion,
	const Rect2i & ) {
	return isActive() && pRegion && !pRegion->isDeleted() && locationToSlot( rLocation ) >= 0;
}

void Terrain3DActiveStorage::invalidate(
	const Vector2i &rLocation ) {
	if ( !isActive() ) {
		return;
	}
	const int logical_slot = locationToSlot( rLocation );
	if ( logical_slot < 0 ) {
		return;
	}
	Terrain3DScopedLock lock( m_writerMutex );
	if ( !isActive() ) {
		return;
	}
	const int page = m_szLogicalToPhysical[logical_slot].exchange( T3D_ACTIVE_INVALID_PAGE, T3D_MEMORY_ORDER_ACQ_REL );
	if ( page >= 0 && page < T3D_ACTIVE_PHYSICAL_PAGE_COUNT ) {
		m_szPhysicalPages[page].m_iLogicalSlot.store( T3D_ACTIVE_INVALID_PAGE, T3D_MEMORY_ORDER_RELEASE );
		m_szPhysicalPages[page].m_nState.store( TERRAIN3D_ACTIVE_PAGE_RETIRING, T3D_MEMORY_ORDER_RELEASE );
		bumpGeneration( m_szPhysicalPages[page].m_nGeneration );
	}
}

Terrain3DActiveMapLease::Terrain3DActiveMapLease(
	void *const						pReleaseContext,
	const t3dActiveLeaseReleaseFn_t pRelease,
	const int						iPage,
	const t3dActiveMapView_t	   &rView ) :
	m_lease( { pReleaseContext, pRelease, iPage }, rView ) {
}

Terrain3DActiveMapLease::~Terrain3DActiveMapLease( void ) = default;
Terrain3DActiveMapLease::Terrain3DActiveMapLease( Terrain3DActiveMapLease &&rOther ) noexcept = default;
Terrain3DActiveMapLease &Terrain3DActiveMapLease::operator=( Terrain3DActiveMapLease &&rOther ) noexcept = default;
void					 Terrain3DActiveMapLease::reset(
	void ) {
	m_lease.reset();
}
