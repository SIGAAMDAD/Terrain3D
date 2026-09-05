// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "memory/terrain_3d_active_storage.h"

#include "core/terrain_3d_compiler.h"
#include "data/terrain_3d_region_codec.h"
#include "memory/terrain_3d_memory.h"

#include <EASTL/numeric_limits.h>
#include <string.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
typedef enum t3dActiveOwnerState_e : uint32_t {
	TERRAIN3D_ACTIVE_OWNER_FREE = 0u,
	TERRAIN3D_ACTIVE_OWNER_OWNED = 1u,
	TERRAIN3D_ACTIVE_OWNER_DRAINING = 2u,
} t3dActiveOwnerState_t;

typedef enum t3dActivePageState_e : uint32_t {
	TERRAIN3D_ACTIVE_PAGE_FREE = 0u,
	TERRAIN3D_ACTIVE_PAGE_STAGING = 1u,
	TERRAIN3D_ACTIVE_PAGE_PUBLISHED = 2u,
	TERRAIN3D_ACTIVE_PAGE_RETIRING = 3u,
} t3dActivePageState_t;

typedef struct t3dActivePhysicalPage_s {
	uint16_t *m_pHeight = NULL;
	t3dAtomic_t<uint32_t> m_nPins{ 0u };
	t3dAtomic_t<uint32_t> m_nGeneration{ 1u };
	t3dAtomic_t<int> m_iLogicalSlot{ s_nActiveInvalidPage };
	t3dAtomic_t<uint32_t> m_nState{ TERRAIN3D_ACTIVE_PAGE_FREE };
} t3dActivePhysicalPage_t;

static t3dActivePhysicalPage_t s_szPhysicalPages[s_nActivePhysicalPageCount];
static t3dAtomic_t<int> s_szLogicalToPhysical[s_nRegionSlots];
static void *s_pActiveHeightBackingRaw = NULL;
static uint8_t *s_pActiveHeightBacking = NULL;
static bool s_bActiveHeightBackingOwned = false;
static t3dAtomic_t<size_t> s_nActiveHeightBytes{ 0u };
static t3dAtomic_t<uint32_t> s_nActiveOwnerState{ TERRAIN3D_ACTIVE_OWNER_FREE };
static t3dAtomic_t<uint32_t> s_nActiveOwnerGeneration{ 1u };
static t3dAtomic_t<const void *> s_pActiveOwner{ NULL };
static t3dAtomic_t<uint64_t> s_nActiveReaderRetries{ 0u };
static t3dAtomic_t<uint64_t> s_nActivePublications{ 0u };
static uint32_t s_nClockHand = 0u;
static t3dMutex_t s_activeWriterMutex;

EA_FORCE_INLINE static size_t t3dActiveStorageActivePageBytes( void ) {
	return ( size_t )( s_nRegionHeightBytes );
}

static int t3dActiveStorageActiveLocationToSlot( const Vector2i &rLocation ) {
	const int x = rLocation.x + s_nRegionMapHalf;
	const int y = rLocation.y + s_nRegionMapHalf;
	if ( ( uint32_t )( x ) >= ( uint32_t )( s_nRegionMapSize ) || ( uint32_t )( y ) >= ( uint32_t )( s_nRegionMapSize ) ) {
		return -1;
	}
	return ( y << s_nRegionMapShift ) + x;
}

static void t3dActiveStorageActiveBumpGeneration( t3dAtomic_t<uint32_t> &rGeneration ) {
	const uint32_t next = rGeneration.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
	if ( next == 0u ) {
		rGeneration.store( 1u, T3D_MEMORY_ORDER_RELEASE );
	}
}

EA_FORCE_INLINE static bool t3dActiveStorageActiveOwnerMatches( const void *pOwner ) {
	return pOwner && s_nActiveOwnerState.load( T3D_MEMORY_ORDER_ACQUIRE ) == TERRAIN3D_ACTIVE_OWNER_OWNED
	        && s_pActiveOwner.load( T3D_MEMORY_ORDER_ACQUIRE ) == pOwner;
}

static void t3dActiveStorageActiveClearMappingsUnsafe( void ) {
	for ( int slot = 0; slot < s_nRegionSlots; ++slot ) {
		s_szLogicalToPhysical[slot].store( s_nActiveInvalidPage, T3D_MEMORY_ORDER_RELEASE );
	}
	for ( int page = 0; page < s_nActivePhysicalPageCount; ++page ) {
		s_szPhysicalPages[page].m_iLogicalSlot.store( s_nActiveInvalidPage, T3D_MEMORY_ORDER_RELEASE );
		if ( s_szPhysicalPages[page].m_nPins.load( T3D_MEMORY_ORDER_ACQUIRE ) == 0u ) {
			s_szPhysicalPages[page].m_nState.store( TERRAIN3D_ACTIVE_PAGE_FREE, T3D_MEMORY_ORDER_RELEASE );
		} else {
			s_szPhysicalPages[page].m_nState.store( TERRAIN3D_ACTIVE_PAGE_RETIRING, T3D_MEMORY_ORDER_RELEASE );
		}
		t3dActiveStorageActiveBumpGeneration( s_szPhysicalPages[page].m_nGeneration );
	}
}

static void t3dActiveStorageActiveWaitForReadersUnsafe( void ) {
	for ( ;; ) {
		bool idle = true;
		for ( int page = 0; page < s_nActivePhysicalPageCount; ++page ) {
			if ( s_szPhysicalPages[page].m_nPins.load( T3D_MEMORY_ORDER_ACQUIRE ) != 0u ) {
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

static void t3dActiveStorageActiveFreeBackingUnsafe( void ) {
	if ( s_bActiveHeightBackingOwned ) {
		t3dMemoryRelease( s_pActiveHeightBackingRaw );
	}
	s_pActiveHeightBackingRaw = NULL;
	s_pActiveHeightBacking = NULL;
	s_bActiveHeightBackingOwned = false;
	for ( int page = 0; page < s_nActivePhysicalPageCount; ++page ) {
		s_szPhysicalPages[page].m_pHeight = NULL;
		s_szPhysicalPages[page].m_nState.store( TERRAIN3D_ACTIVE_PAGE_FREE, T3D_MEMORY_ORDER_RELAXED );
	}
	s_nActiveHeightBytes.store( 0u, T3D_MEMORY_ORDER_RELEASE );
}

static bool t3dActiveStorageActiveConfigureBackingUnsafe( void *pBacking, const size_t nBackingBytes, const bool bOwned ) {
	const size_t required = t3dActiveStorageRequiredBytes();
	if ( required == 0u || !pBacking || nBackingBytes < required ) {
		return false;
	}
	const uintptr_t address = ( uintptr_t )( pBacking );
	const uintptr_t aligned = ( address + s_nActiveStorageAlignment - 1u ) & ~( uintptr_t )( s_nActiveStorageAlignment - 1u );
	const size_t alignment_slop = ( size_t )( aligned - address );
	if ( alignment_slop > nBackingBytes || nBackingBytes - alignment_slop < required ) {
		return false;
	}
	s_pActiveHeightBackingRaw = pBacking;
	s_pActiveHeightBacking = ( uint8_t * )aligned;
	s_bActiveHeightBackingOwned = bOwned;
	memset( s_pActiveHeightBacking, 0, required );
	const size_t page_bytes = t3dActiveStorageActivePageBytes();
	for ( int page = 0; page < s_nActivePhysicalPageCount; ++page ) {
		s_szPhysicalPages[page].m_pHeight = ( uint16_t * )( s_pActiveHeightBacking + ( size_t )( page )*page_bytes );
		s_szPhysicalPages[page].m_nPins.store( 0u, T3D_MEMORY_ORDER_RELAXED );
		s_szPhysicalPages[page].m_nGeneration.store( 1u, T3D_MEMORY_ORDER_RELAXED );
		s_szPhysicalPages[page].m_iLogicalSlot.store( s_nActiveInvalidPage, T3D_MEMORY_ORDER_RELAXED );
		s_szPhysicalPages[page].m_nState.store( TERRAIN3D_ACTIVE_PAGE_FREE, T3D_MEMORY_ORDER_RELAXED );
	}
	s_nActiveHeightBytes.store( required, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

static bool t3dActiveStorageActiveAllocateBackingUnsafe( void ) {
	const size_t required = t3dActiveStorageRequiredBytes();
	if ( required == 0u || required > eastl::numeric_limits<size_t>::max() - s_nActiveStorageAlignment ) {
		return false;
	}
	void *const pRaw = t3dMemoryAllocate( required + s_nActiveStorageAlignment - 1u );
	if ( !pRaw ) {
		return false;
	}
	if ( !t3dActiveStorageActiveConfigureBackingUnsafe( pRaw, required + s_nActiveStorageAlignment - 1u, true ) ) {
		t3dMemoryRelease( pRaw );
		return false;
	}
	return true;
}

static int t3dActiveStorageActiveChooseStagingPageUnsafe( const int iPreservedPage ) {
	for ( int pass = 0; pass < 2; ++pass ) {
		for ( int offset = 0; offset < s_nActivePhysicalPageCount; ++offset ) {
			const int page = ( int )( ( s_nClockHand + ( uint32_t )( offset ) ) % ( uint32_t )( s_nActivePhysicalPageCount ) );
			if ( page == iPreservedPage ) {
				continue;
			}
			t3dActivePhysicalPage_t &rPage = s_szPhysicalPages[page];
			const uint32_t state = rPage.m_nState.load( T3D_MEMORY_ORDER_ACQUIRE );
			if ( rPage.m_nPins.load( T3D_MEMORY_ORDER_ACQUIRE ) != 0u ) {
				continue;
			}
			if ( pass == 0 && state != TERRAIN3D_ACTIVE_PAGE_FREE && state != TERRAIN3D_ACTIVE_PAGE_RETIRING ) {
				continue;
			}
			if ( pass == 1 && state == TERRAIN3D_ACTIVE_PAGE_PUBLISHED ) {
				const int victim_slot = rPage.m_iLogicalSlot.load( T3D_MEMORY_ORDER_ACQUIRE );
				if ( victim_slot >= 0 && victim_slot < s_nRegionSlots ) {
					int expected = page;
					s_szLogicalToPhysical[victim_slot].compare_exchange_strong( expected, s_nActiveInvalidPage, T3D_MEMORY_ORDER_ACQ_REL );
				}
				rPage.m_nState.store( TERRAIN3D_ACTIVE_PAGE_RETIRING, T3D_MEMORY_ORDER_RELEASE );
				if ( rPage.m_nPins.load( T3D_MEMORY_ORDER_ACQUIRE ) != 0u ) {
					continue;
				}
			} else if ( pass == 1 && state != TERRAIN3D_ACTIVE_PAGE_FREE && state != TERRAIN3D_ACTIVE_PAGE_RETIRING ) {
				continue;
			}
			s_nClockHand = ( uint32_t )( page + 1 );
			return page;
		}
	}
	return s_nActiveInvalidPage;
}

static bool t3dActiveStorageActivePublishBytesUnsafe( const int m_iLogicalSlot, const uint8_t *pHeightBytes, const size_t nHeightBytes ) {
	const size_t expected_bytes = t3dActiveStorageActivePageBytes();
	if ( m_iLogicalSlot < 0 || m_iLogicalSlot >= s_nRegionSlots || !pHeightBytes || nHeightBytes != expected_bytes ) {
		return false;
	}
	const int old_page = s_szLogicalToPhysical[m_iLogicalSlot].load( T3D_MEMORY_ORDER_ACQUIRE );
	const int staging_page = t3dActiveStorageActiveChooseStagingPageUnsafe( old_page );
	if ( staging_page < 0 ) {
		return false;
	}
	t3dActivePhysicalPage_t &rStaging = s_szPhysicalPages[staging_page];
	rStaging.m_nState.store( TERRAIN3D_ACTIVE_PAGE_STAGING, T3D_MEMORY_ORDER_RELEASE );
	rStaging.m_iLogicalSlot.store( m_iLogicalSlot, T3D_MEMORY_ORDER_RELEASE );
	t3dActiveStorageActiveBumpGeneration( rStaging.m_nGeneration );
	memcpy( rStaging.m_pHeight, pHeightBytes, expected_bytes );
	rStaging.m_nState.store( TERRAIN3D_ACTIVE_PAGE_PUBLISHED, T3D_MEMORY_ORDER_RELEASE );
	const int replaced_page = s_szLogicalToPhysical[m_iLogicalSlot].exchange( staging_page, T3D_MEMORY_ORDER_ACQ_REL );
	if ( replaced_page >= 0 && replaced_page != staging_page ) {
		t3dActivePhysicalPage_t &rReplaced = s_szPhysicalPages[replaced_page];
		rReplaced.m_iLogicalSlot.store( s_nActiveInvalidPage, T3D_MEMORY_ORDER_RELEASE );
		rReplaced.m_nState.store( TERRAIN3D_ACTIVE_PAGE_RETIRING, T3D_MEMORY_ORDER_RELEASE );
	}
	s_nActivePublications.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	return true;
}

static bool t3dActiveStorageActiveCopyRegionUnsafe( const int m_iLogicalSlot, const Terrain3DRegion *pRegion ) {
	const Image *const pImage = pRegion ? pRegion->getMapPtr( TYPE_HEIGHT ) : NULL;
	PackedByteArray bytes;
	if ( pImage && pImage->get_format() == Image::FORMAT_R16 && pImage->get_width() == s_nRegionSize
	        && pImage->get_height() == s_nRegionSize ) {
		bytes = pImage->get_data();
	} else if ( !pImage ) {
		const size_t expected_bytes = t3dActiveStorageActivePageBytes();
		bytes.resize( ( int64_t )( expected_bytes ) );
		uint16_t *const pValues = ( uint16_t * )( bytes.ptrw() );
		eastl::fill_n( pValues, expected_bytes / sizeof( uint16_t ), t3dRegionCodecEncodeHeightU16( 0.0f ) );
	} else {
		return false;
	}
	return t3dActiveStorageActivePublishBytesUnsafe( m_iLogicalSlot, bytes.ptr(), ( size_t )( bytes.size() ) );
}

static void t3dActiveStorageActiveReleaseReader( const int iPhysicalPage ) {
	if ( iPhysicalPage >= 0 && iPhysicalPage < s_nActivePhysicalPageCount ) {
		s_szPhysicalPages[iPhysicalPage].m_nPins.fetch_sub( 1u, T3D_MEMORY_ORDER_RELEASE );
	}
}

size_t t3dActiveStorageRequiredBytes( void ) {
	const size_t page_bytes = t3dActiveStorageActivePageBytes();
	return page_bytes <= eastl::numeric_limits<size_t>::max() / ( size_t )( s_nActivePhysicalPageCount )
	        ? page_bytes * ( size_t )( s_nActivePhysicalPageCount )
	        : 0u;
}

int t3dActiveStorageGetPhysicalCapacity( void ) {
	return s_nActivePhysicalPageCount;
}

uint64_t t3dActiveStorageGetReaderRetryCount( void ) {
	return s_nActiveReaderRetries.load( T3D_MEMORY_ORDER_RELAXED );
}

uint64_t t3dActiveStorageGetReaderMutexCount( void ) {
	return 0u;
}

uint64_t t3dActiveStorageGetPublicationCount( void ) {
	return s_nActivePublications.load( T3D_MEMORY_ORDER_RELAXED );
}

bool t3dActiveStorageBind( const void *const pOwner, void *const pBacking, const size_t nBackingBytes ) {
	if ( !pOwner || !pBacking || ( ( uintptr_t )( pBacking ) & ( s_nActiveStorageAlignment - 1u ) ) != 0u ) {
		return false;
	}
	t3dScopedLock_t lock( s_activeWriterMutex );
	const void *const current_owner = s_pActiveOwner.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( current_owner && current_owner != pOwner ) {
		return false;
	}
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_DRAINING, T3D_MEMORY_ORDER_RELEASE );
	t3dActiveStorageActiveClearMappingsUnsafe();
	t3dActiveStorageActiveWaitForReadersUnsafe();
	t3dActiveStorageActiveFreeBackingUnsafe();
	if ( !t3dActiveStorageActiveConfigureBackingUnsafe( pBacking, nBackingBytes, false ) ) {
		s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_FREE, T3D_MEMORY_ORDER_RELEASE );
		return false;
	}
	for ( int slot = 0; slot < s_nRegionSlots; ++slot ) {
		s_szLogicalToPhysical[slot].store( s_nActiveInvalidPage, T3D_MEMORY_ORDER_RELAXED );
	}
	s_pActiveOwner.store( pOwner, T3D_MEMORY_ORDER_RELEASE );
	t3dActiveStorageActiveBumpGeneration( s_nActiveOwnerGeneration );
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_OWNED, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

Terrain3DActiveMapLease::Terrain3DActiveMapLease( const int iSlot, const t3dActiveMapView_t &rView ) : m_iSlot( iSlot ), m_view( rView ) {
}

Terrain3DActiveMapLease::~Terrain3DActiveMapLease( void ) {
	reset();
}

Terrain3DActiveMapLease::Terrain3DActiveMapLease( Terrain3DActiveMapLease &&rOther ) noexcept :
        m_iSlot( rOther.m_iSlot ), m_view( rOther.m_view ) {
	rOther.m_iSlot = -1;
	rOther.m_view = {};
}

Terrain3DActiveMapLease &Terrain3DActiveMapLease::operator=( Terrain3DActiveMapLease &&rOther ) noexcept {
	if ( this != &rOther ) {
		reset();
		m_iSlot = rOther.m_iSlot;
		m_view = rOther.m_view;
		rOther.m_iSlot = -1;
		rOther.m_view = {};
	}
	return *this;
}

void Terrain3DActiveMapLease::reset( void ) {
	if ( m_iSlot >= 0 ) {
		t3dActiveStorageActiveReleaseReader( m_iSlot );
	}
	m_iSlot = -1;
	m_view = {};
}

bool t3dActiveStorageClaim( const void *pOwner ) {
	if ( !pOwner ) {
		return false;
	}
	t3dScopedLock_t lock( s_activeWriterMutex );
	const uint32_t owner_state = s_nActiveOwnerState.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( owner_state != TERRAIN3D_ACTIVE_OWNER_FREE ) {
		return owner_state == TERRAIN3D_ACTIVE_OWNER_OWNED && s_pActiveOwner.load( T3D_MEMORY_ORDER_ACQUIRE ) == pOwner;
	}
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_DRAINING, T3D_MEMORY_ORDER_RELEASE );
	if ( !t3dActiveStorageActiveAllocateBackingUnsafe() ) {
		s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_FREE, T3D_MEMORY_ORDER_RELEASE );
		return false;
	}
	for ( int slot = 0; slot < s_nRegionSlots; ++slot ) {
		s_szLogicalToPhysical[slot].store( s_nActiveInvalidPage, T3D_MEMORY_ORDER_RELAXED );
	}
	s_pActiveOwner.store( pOwner, T3D_MEMORY_ORDER_RELEASE );
	t3dActiveStorageActiveBumpGeneration( s_nActiveOwnerGeneration );
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_OWNED, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

bool t3dActiveStorageReconfigure( const void *pOwner ) {
	if ( !pOwner ) {
		return false;
	}
	return t3dActiveStorageIsOwner( pOwner ) ? true : t3dActiveStorageClaim( pOwner );
}

void t3dActiveStorageRelease( const void *pOwner ) {
	if ( !pOwner ) {
		return;
	}
	t3dScopedLock_t lock( s_activeWriterMutex );
	if ( s_pActiveOwner.load( T3D_MEMORY_ORDER_ACQUIRE ) != pOwner ) {
		return;
	}
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_DRAINING, T3D_MEMORY_ORDER_RELEASE );
	t3dActiveStorageActiveClearMappingsUnsafe();
	t3dActiveStorageActiveWaitForReadersUnsafe();
	t3dActiveStorageActiveFreeBackingUnsafe();
	s_pActiveOwner.store( NULL, T3D_MEMORY_ORDER_RELEASE );
	t3dActiveStorageActiveBumpGeneration( s_nActiveOwnerGeneration );
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_FREE, T3D_MEMORY_ORDER_RELEASE );
}

void t3dActiveStorageReset( const void *pOwner ) {
	t3dScopedLock_t lock( s_activeWriterMutex );
	if ( !t3dActiveStorageActiveOwnerMatches( pOwner ) ) {
		return;
	}
	t3dActiveStorageActiveClearMappingsUnsafe();
	t3dActiveStorageActiveBumpGeneration( s_nActiveOwnerGeneration );
}

bool t3dActiveStorageIsOwner( const void *pOwner ) {
	return t3dActiveStorageActiveOwnerMatches( pOwner );
}

int t3dActiveStorageGetRegionMapSize( const void *pOwner ) {
	return t3dActiveStorageActiveOwnerMatches( pOwner ) ? s_nRegionMapSize : 0;
}

uint32_t t3dActiveStorageGetOwnerGeneration( const void *pOwner ) {
	return t3dActiveStorageActiveOwnerMatches( pOwner ) ? s_nActiveOwnerGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) : 0u;
}

size_t t3dActiveStorageGetReservedBytes( void ) {
	return s_nActiveHeightBytes.load( T3D_MEMORY_ORDER_ACQUIRE );
}

bool t3dActiveStorageContains( const void *pOwner, const Vector2i &rLocation ) {
	if ( !t3dActiveStorageActiveOwnerMatches( pOwner ) ) {
		return false;
	}
	const int logical_slot = t3dActiveStorageActiveLocationToSlot( rLocation );
	if ( logical_slot < 0 ) {
		return false;
	}
	const int page = s_szLogicalToPhysical[logical_slot].load( T3D_MEMORY_ORDER_ACQUIRE );
	return page >= 0 && page < s_nActivePhysicalPageCount
	        && s_szPhysicalPages[page].m_nState.load( T3D_MEMORY_ORDER_ACQUIRE ) == TERRAIN3D_ACTIVE_PAGE_PUBLISHED
	        && s_szPhysicalPages[page].m_iLogicalSlot.load( T3D_MEMORY_ORDER_ACQUIRE ) == logical_slot;
}

Terrain3DActiveMapLease t3dActiveStorageAcquire( const void *pOwner, const Vector2i &rLocation ) {
	if ( !t3dActiveStorageActiveOwnerMatches( pOwner ) ) {
		return {};
	}
		const int logical_slot = t3dActiveStorageActiveLocationToSlot( rLocation );
	if ( logical_slot < 0 ) {
		return {};
	}
	for ( int attempt = 0; attempt < 4; ++attempt ) {
		const int page = s_szLogicalToPhysical[logical_slot].load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( page < 0 || page >= s_nActivePhysicalPageCount ) {
			return {};
		}
		t3dActivePhysicalPage_t &rPage = s_szPhysicalPages[page];
		const uint32_t generation = rPage.m_nGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( rPage.m_nState.load( T3D_MEMORY_ORDER_ACQUIRE ) != TERRAIN3D_ACTIVE_PAGE_PUBLISHED
		        || rPage.m_iLogicalSlot.load( T3D_MEMORY_ORDER_ACQUIRE ) != logical_slot ) {
			s_nActiveReaderRetries.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			continue;
		}
		rPage.m_nPins.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
		const bool valid = t3dActiveStorageActiveOwnerMatches( pOwner )
		        && s_szLogicalToPhysical[logical_slot].load( T3D_MEMORY_ORDER_ACQUIRE ) == page
		        && rPage.m_nState.load( T3D_MEMORY_ORDER_ACQUIRE ) == TERRAIN3D_ACTIVE_PAGE_PUBLISHED
		        && rPage.m_iLogicalSlot.load( T3D_MEMORY_ORDER_ACQUIRE ) == logical_slot
		        && rPage.m_nGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) == generation;
		if ( valid ) {
			t3dActiveMapView_t view;
			view.m_pHeight = rPage.m_pHeight;
			view.m_nOwnerGeneration = s_nActiveOwnerGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
			view.m_nRegionGeneration = generation;
			view.m_iSlot = logical_slot;
			return Terrain3DActiveMapLease( page, view );
		}
		rPage.m_nPins.fetch_sub( 1u, T3D_MEMORY_ORDER_RELEASE );
		s_nActiveReaderRetries.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	return {};
}

bool t3dActiveStorageSyncRegion( const void *pOwner, const Vector2i &rLocation, const Terrain3DRegion *pRegion ) {
	if ( !t3dActiveStorageActiveOwnerMatches( pOwner ) || !pRegion || pRegion->isDeleted()
	        || pRegion->getRegionSize() != s_nRegionSize ) {
		return false;
	}
	const int logical_slot = t3dActiveStorageActiveLocationToSlot( rLocation );
	if ( logical_slot < 0 ) {
		return false;
	}
	t3dScopedLock_t lock( s_activeWriterMutex );
	return t3dActiveStorageActiveOwnerMatches( pOwner ) && t3dActiveStorageActiveCopyRegionUnsafe( logical_slot, pRegion );
}

bool t3dActiveStorageSyncHeightPage(
        const void *pOwner, const Vector2i &rLocation, const uint8_t *pHeightBytes, const size_t nHeightBytes ) {
	if ( !t3dActiveStorageActiveOwnerMatches( pOwner ) || !pHeightBytes ) {
		return false;
	}
	const int logical_slot = t3dActiveStorageActiveLocationToSlot( rLocation );
	if ( logical_slot < 0 ) {
		return false;
	}
	t3dScopedLock_t lock( s_activeWriterMutex );
	return t3dActiveStorageActiveOwnerMatches( pOwner )
	        && t3dActiveStorageActivePublishBytesUnsafe( logical_slot, pHeightBytes, nHeightBytes );
}

bool t3dActiveStorageReplaceHeightPages(
        const void *pOwner, const uint8_t *const *pHeightPages, const int *pSlots, const size_t nPageCount ) {
	if ( !pOwner || nPageCount > ( size_t )( s_nActivePhysicalPageCount )
	        || ( nPageCount > 0u && ( !pHeightPages || !pSlots ) ) ) {
		return false;
	}
	const int logical_count = s_nRegionSlots;
	for ( size_t index = 0; index < nPageCount; ++index ) {
		if ( pSlots[index] < 0 || pSlots[index] >= logical_count || !pHeightPages[index] ) {
			return false;
		}
		for ( size_t previous = 0; previous < index; ++previous ) {
			if ( pSlots[previous] == pSlots[index] ) {
				return false;
			}
		}
	}
	t3dScopedLock_t lock( s_activeWriterMutex );
	if ( !t3dActiveStorageActiveOwnerMatches( pOwner ) ) {
		return false;
	}
	t3dActiveStorageActiveClearMappingsUnsafe();
	const size_t page_bytes = t3dActiveStorageActivePageBytes();
	for ( size_t index = 0; index < nPageCount; ++index ) {
		if ( !t3dActiveStorageActivePublishBytesUnsafe( pSlots[index], pHeightPages[index], page_bytes ) ) {
			t3dActiveStorageActiveClearMappingsUnsafe();
			return false;
		}
	}
	t3dActiveStorageActiveBumpGeneration( s_nActiveOwnerGeneration );
	return true;
}

bool t3dActiveStorageSyncMap(
        const void *pOwner, const Vector2i &rLocation, const Terrain3DRegion *pRegion, const t3dMapType_t type, const Rect2i & ) {
	if ( type < TYPE_HEIGHT || type >= REGION_TYPE_MAX ) {
		return false;
	}
	return type == TYPE_HEIGHT ? t3dActiveStorageSyncRegion( pOwner, rLocation, pRegion ) : true;
}

bool t3dActiveStorageSyncDerived( const void *pOwner, const Vector2i &rLocation, const Terrain3DRegion *pRegion, const Rect2i & ) {
	return t3dActiveStorageActiveOwnerMatches( pOwner ) && pRegion && !pRegion->isDeleted()
	        && t3dActiveStorageActiveLocationToSlot( rLocation ) >= 0;
}

void t3dActiveStorageInvalidate( const void *pOwner, const Vector2i &rLocation ) {
	if ( !t3dActiveStorageActiveOwnerMatches( pOwner ) ) {
		return;
	}
	const int logical_slot = t3dActiveStorageActiveLocationToSlot( rLocation );
	if ( logical_slot < 0 ) {
		return;
	}
	t3dScopedLock_t lock( s_activeWriterMutex );
	if ( !t3dActiveStorageActiveOwnerMatches( pOwner ) ) {
		return;
	}
	const int page = s_szLogicalToPhysical[logical_slot].exchange( s_nActiveInvalidPage, T3D_MEMORY_ORDER_ACQ_REL );
	if ( page >= 0 && page < s_nActivePhysicalPageCount ) {
		s_szPhysicalPages[page].m_iLogicalSlot.store( s_nActiveInvalidPage, T3D_MEMORY_ORDER_RELEASE );
		s_szPhysicalPages[page].m_nState.store( TERRAIN3D_ACTIVE_PAGE_RETIRING, T3D_MEMORY_ORDER_RELEASE );
		t3dActiveStorageActiveBumpGeneration( s_szPhysicalPages[page].m_nGeneration );
	}
}
