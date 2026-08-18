// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "terrain_3d_active_storage.h"

#include "terrain_3d_active_access.h"
#include "terrain_3d_memory.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <mutex>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include "terrain_3d_region_codec.h"

namespace {

typedef enum t3dactiveOwnerState_e : uint32_t {
	TERRAIN3D_ACTIVE_OWNER_FREE = 0u,
	TERRAIN3D_ACTIVE_OWNER_OWNED = 1u,
	TERRAIN3D_ACTIVE_OWNER_DRAINING = 2u,
} t3dactiveOwnerState_t;

typedef struct t3dactiveHeightPage_s {
	void *pAllocation = nullptr;
	uint16_t *pHeight = nullptr;
} t3dactiveHeightPage_t;

static t3dactiveHeightPage_t s_szActiveHeightPages[TERRAIN3D_ACTIVE_MAX_REGION_SLOTS];
static std::atomic<size_t> s_nActiveHeightBytes{0u};

static t3dactiveAccessGate_t s_szActiveAccess[TERRAIN3D_ACTIVE_MAX_REGION_SLOTS];
static std::atomic<uint32_t> s_szActiveRegionGeneration[TERRAIN3D_ACTIVE_MAX_REGION_SLOTS];
static uint8_t s_szActivePresent[TERRAIN3D_ACTIVE_MAX_REGION_SLOTS];
static std::atomic<uint32_t> s_nActiveOwnerState{TERRAIN3D_ACTIVE_OWNER_FREE};
static std::atomic<uint32_t> s_nActiveOwnerGeneration{1u};
static std::atomic<const void *> s_pActiveOwner{nullptr};
static std::atomic<int> s_nActiveRegionSize{0};
static std::atomic<int> s_nActiveRegionMapSize{0};
static std::mutex s_activeOwnerMutex;

static bool terrain3dActiveValidRegionSize( const int nRegionSize ) {
	return nRegionSize >= TERRAIN3D_ACTIVE_MIN_REGION_SIZE &&
		nRegionSize <= TERRAIN3D_ACTIVE_WORLD_TEXELS &&
		( nRegionSize & ( nRegionSize - 1 ) ) == 0 &&
		TERRAIN3D_ACTIVE_WORLD_TEXELS % nRegionSize == 0;
}

static int terrain3dActiveLocationToSlot( const Vector2i &rLocation, const int nMapSize ) {
	if ( nMapSize <= 0 || nMapSize > TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE ) {
		return -1;
	}
	const int half = nMapSize / 2;
	const int x = rLocation.x + half;
	const int y = rLocation.y + half;
	if ( ( uint32_t ) ( x ) >= ( uint32_t ) ( nMapSize ) || ( uint32_t ) ( y ) >= ( uint32_t ) ( nMapSize ) ) {
		return -1;
	}
	return y * nMapSize + x;
}

static size_t terrain3dActivePageBytes( const int nRegionSize ) {
	const size_t size = nRegionSize > 0 ? ( size_t ) ( nRegionSize ) : 0u;
	return size * size * sizeof( uint16_t );
}

static uint16_t *terrain3dActiveAllocatePage( const int iSlot, const int nRegionSize ) {
	if ( iSlot < 0 || iSlot >= TERRAIN3D_ACTIVE_MAX_REGION_SLOTS || nRegionSize <= 0 ) {
		return nullptr;
	}
	t3dactiveHeightPage_t &page = s_szActiveHeightPages[iSlot];
	if ( page.pHeight ) {
		return page.pHeight;
	}
	const size_t page_bytes = terrain3dActivePageBytes( nRegionSize );
	void *pAllocation = terrain3d_memory::allocate( page_bytes + TERRAIN3D_ACTIVE_STORAGE_ALIGNMENT - 1u );
	if ( !pAllocation ) {
		return nullptr;
	}
	const uintptr_t address = ( uintptr_t ) ( pAllocation );
	const uintptr_t aligned = ( address + TERRAIN3D_ACTIVE_STORAGE_ALIGNMENT - 1u ) &
		~( uintptr_t ) ( TERRAIN3D_ACTIVE_STORAGE_ALIGNMENT - 1u );
	page.pAllocation = pAllocation;
	page.pHeight = ( uint16_t * ) ( aligned );
	s_nActiveHeightBytes.fetch_add( page_bytes, std::memory_order_relaxed );
	return page.pHeight;
}

static void terrain3dActiveFreePage( const int iSlot, const int nRegionSize ) {
	if ( iSlot < 0 || iSlot >= TERRAIN3D_ACTIVE_MAX_REGION_SLOTS ) {
		return;
	}
	t3dactiveHeightPage_t &page = s_szActiveHeightPages[iSlot];
	if ( !page.pAllocation ) {
		page.pHeight = nullptr;
		return;
	}
	terrain3d_memory::release( page.pAllocation );
	page = {};
	const size_t page_bytes = terrain3dActivePageBytes( nRegionSize );
	const size_t previous = s_nActiveHeightBytes.fetch_sub( page_bytes, std::memory_order_relaxed );
	if ( previous < page_bytes ) {
		s_nActiveHeightBytes.store( 0u, std::memory_order_relaxed );
	}
}

static void terrain3dActiveFreeAllPages( const int nRegionSize ) {
	for ( int slot = 0; slot < TERRAIN3D_ACTIVE_MAX_REGION_SLOTS; ++slot ) {
		terrain3dActiveFreePage( slot, nRegionSize );
	}
	s_nActiveHeightBytes.store( 0u, std::memory_order_release );
}

static void terrain3dActiveBumpGeneration( std::atomic<uint32_t> &nGeneration ) {
	const uint32_t next = nGeneration.fetch_add( 1u, std::memory_order_acq_rel ) + 1u;
	if ( next == 0u ) {
		nGeneration.store( 1u, std::memory_order_release );
	}
}

static bool terrain3dActiveOwnerMatches( const void *pOwner ) {
	return pOwner && s_nActiveOwnerState.load( std::memory_order_acquire ) == TERRAIN3D_ACTIVE_OWNER_OWNED &&
		s_pActiveOwner.load( std::memory_order_acquire ) == pOwner;
}

static void terrain3dActiveWaitForAllSlots( void ) {
	for ( int slot = 0; slot < TERRAIN3D_ACTIVE_MAX_REGION_SLOTS; ++slot ) {
		s_szActiveAccess[slot].waitIdle();
	}
}

static Rect2i terrain3dActiveClampRect( const Rect2i &rRect, const int nRegionSize ) {
	const Rect2i full( V2I_ZERO, V2I( nRegionSize ) );
	if ( rRect.size.x <= 0 || rRect.size.y <= 0 ) {
		return full;
	}
	return rRect.intersection( full );
}

static bool terrain3dActiveCopyHeight(
	const Image *pImage, uint16_t *pDestination, const int nRegionSize,
	const Rect2i &rRect
) {
	if ( !pDestination || nRegionSize <= 0 ) {
		return false;
	}
	const Rect2i rect = terrain3dActiveClampRect( rRect, nRegionSize );
	const Vector2i rect_end = rect.position + rect.size;
	if ( rect.size.x <= 0 || rect.size.y <= 0 ) {
		return true;
	}
	if ( !pImage || pImage->get_format() != Image::FORMAT_R16 || pImage->get_width() != nRegionSize ||
		pImage->get_height() != nRegionSize ) {
		const uint16_t value = Terrain3DRegionCodec::encodeHeightU16( 0.0f );
		for ( int y = rect.position.y; y < rect_end.y; ++y ) {
			std::fill_n( pDestination + ( size_t ) ( y ) * ( size_t ) ( nRegionSize ) + ( size_t ) ( rect.position.x ), rect.size.x, value );
		}
		return pImage == nullptr;
	}
	const PackedByteArray source = pImage->get_data();
	const size_t expected = ( size_t ) ( nRegionSize ) * ( size_t ) ( nRegionSize ) * sizeof( uint16_t );
	if ( ( size_t ) ( source.size() ) < expected ) {
		return false;
	}
	const uint8_t *pBytes = source.ptr();
	for ( int y = rect.position.y; y < rect_end.y; ++y ) {
		const size_t source_offset = ( ( size_t ) ( y ) * ( size_t ) ( nRegionSize ) + ( size_t ) ( rect.position.x ) ) * sizeof( uint16_t );
		const size_t destination_offset = ( size_t ) ( y ) * ( size_t ) ( nRegionSize ) + ( size_t ) ( rect.position.x );
		std::memcpy( pDestination + destination_offset, pBytes + source_offset, ( size_t ) ( rect.size.x ) * sizeof( uint16_t ) );
	}
	return true;
}

static void terrain3dActiveReleaseReader( const int iSlot ) {
	if ( iSlot < 0 || iSlot >= TERRAIN3D_ACTIVE_MAX_REGION_SLOTS ) {
		return;
	}
	s_szActiveAccess[iSlot].releaseRead();
}

} // namespace

T3DActiveMapLease::T3DActiveMapLease( const int iSlot, const t3dactiveMapView_t &rView )
	: m_iSlot( iSlot ), m_view( rView ) {
}

T3DActiveMapLease::~T3DActiveMapLease( void ) {
	reset();
}

T3DActiveMapLease::T3DActiveMapLease( T3DActiveMapLease &&rOther ) noexcept
	: m_iSlot( rOther.m_iSlot ), m_view( rOther.m_view ) {
	rOther.m_iSlot = -1;
	rOther.m_view = {};
}

T3DActiveMapLease &T3DActiveMapLease::operator=( T3DActiveMapLease &&rOther ) noexcept {
	if ( this == &rOther ) {
		return *this;
	}
	reset();
	m_iSlot = rOther.m_iSlot;
	m_view = rOther.m_view;
	rOther.m_iSlot = -1;
	rOther.m_view = {};
	return *this;
}

void T3DActiveMapLease::reset( void ) {
	if ( m_iSlot >= 0 ) {
		terrain3dActiveReleaseReader( m_iSlot );
	}
	m_iSlot = -1;
	m_view = {};
}

bool terrain3dActiveStorageClaim( const void *pOwner, const int nRegionSize ) {
	if ( !pOwner || !terrain3dActiveValidRegionSize( nRegionSize ) ) {
		return false;
	}
	std::lock_guard<std::mutex> lock( s_activeOwnerMutex );
	const uint32_t owner_state = s_nActiveOwnerState.load( std::memory_order_acquire );
	if ( owner_state != TERRAIN3D_ACTIVE_OWNER_FREE ) {
		return owner_state == TERRAIN3D_ACTIVE_OWNER_OWNED &&
			s_pActiveOwner.load( std::memory_order_acquire ) == pOwner &&
			s_nActiveRegionSize.load( std::memory_order_acquire ) == nRegionSize;
	}
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_DRAINING, std::memory_order_release );
	s_pActiveOwner.store( pOwner, std::memory_order_release );
	s_nActiveRegionSize.store( nRegionSize, std::memory_order_release );
	s_nActiveRegionMapSize.store( TERRAIN3D_ACTIVE_WORLD_TEXELS / nRegionSize, std::memory_order_release );
	std::memset( s_szActivePresent, 0, sizeof( s_szActivePresent ) );
	for ( int slot = 0; slot < TERRAIN3D_ACTIVE_MAX_REGION_SLOTS; ++slot ) {
		s_szActiveAccess[slot].resetUnsafe();
		s_szActiveRegionGeneration[slot].store( 1u, std::memory_order_relaxed );
	}
	terrain3dActiveBumpGeneration( s_nActiveOwnerGeneration );
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_OWNED, std::memory_order_release );
	return true;
}

bool terrain3dActiveStorageReconfigure( const void *pOwner, const int nRegionSize ) {
	if ( !pOwner || !terrain3dActiveValidRegionSize( nRegionSize ) ) {
		return false;
	}
	std::lock_guard<std::mutex> lock( s_activeOwnerMutex );
	if ( s_nActiveOwnerState.load( std::memory_order_acquire ) == TERRAIN3D_ACTIVE_OWNER_FREE ) {
		s_pActiveOwner.store( pOwner, std::memory_order_release );
		s_nActiveRegionSize.store( nRegionSize, std::memory_order_release );
		s_nActiveRegionMapSize.store( TERRAIN3D_ACTIVE_WORLD_TEXELS / nRegionSize, std::memory_order_release );
		std::memset( s_szActivePresent, 0, sizeof( s_szActivePresent ) );
		terrain3dActiveBumpGeneration( s_nActiveOwnerGeneration );
		s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_OWNED, std::memory_order_release );
		return true;
	}
	if ( s_pActiveOwner.load( std::memory_order_acquire ) != pOwner ) {
		return false;
	}
	if ( s_nActiveRegionSize.load( std::memory_order_acquire ) == nRegionSize &&
		s_nActiveOwnerState.load( std::memory_order_acquire ) == TERRAIN3D_ACTIVE_OWNER_OWNED ) {
		return true;
	}
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_DRAINING, std::memory_order_release );
	terrain3dActiveWaitForAllSlots();
	const int old_region_size = s_nActiveRegionSize.load( std::memory_order_acquire );
	terrain3dActiveFreeAllPages( old_region_size );
	s_nActiveRegionSize.store( nRegionSize, std::memory_order_release );
	s_nActiveRegionMapSize.store( TERRAIN3D_ACTIVE_WORLD_TEXELS / nRegionSize, std::memory_order_release );
	std::memset( s_szActivePresent, 0, sizeof( s_szActivePresent ) );
	terrain3dActiveBumpGeneration( s_nActiveOwnerGeneration );
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_OWNED, std::memory_order_release );
	return true;
}

void terrain3dActiveStorageRelease( const void *pOwner ) {
	if ( !pOwner ) {
		return;
	}
	std::lock_guard<std::mutex> lock( s_activeOwnerMutex );
	if ( s_pActiveOwner.load( std::memory_order_acquire ) != pOwner ||
		s_nActiveOwnerState.load( std::memory_order_acquire ) == TERRAIN3D_ACTIVE_OWNER_FREE ) {
		return;
	}
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_DRAINING, std::memory_order_release );
	terrain3dActiveWaitForAllSlots();
	terrain3dActiveFreeAllPages( s_nActiveRegionSize.load( std::memory_order_acquire ) );
	std::memset( s_szActivePresent, 0, sizeof( s_szActivePresent ) );
	s_pActiveOwner.store( nullptr, std::memory_order_release );
	s_nActiveRegionSize.store( 0, std::memory_order_release );
	s_nActiveRegionMapSize.store( 0, std::memory_order_release );
	terrain3dActiveBumpGeneration( s_nActiveOwnerGeneration );
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_FREE, std::memory_order_release );
}

void terrain3dActiveStorageReset( const void *pOwner ) {
	if ( !terrain3dActiveOwnerMatches( pOwner ) ) {
		return;
	}
	std::lock_guard<std::mutex> lock( s_activeOwnerMutex );
	if ( !terrain3dActiveOwnerMatches( pOwner ) ) {
		return;
	}
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_DRAINING, std::memory_order_release );
	terrain3dActiveWaitForAllSlots();
	terrain3dActiveFreeAllPages( s_nActiveRegionSize.load( std::memory_order_acquire ) );
	std::memset( s_szActivePresent, 0, sizeof( s_szActivePresent ) );
	terrain3dActiveBumpGeneration( s_nActiveOwnerGeneration );
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_OWNED, std::memory_order_release );
}

bool terrain3dActiveStorageIsOwner( const void *pOwner ) {
	return terrain3dActiveOwnerMatches( pOwner );
}

int terrain3dActiveStorageGetRegionMapSize( const void *pOwner ) {
	return terrain3dActiveOwnerMatches( pOwner ) ? s_nActiveRegionMapSize.load( std::memory_order_acquire ) : 0;
}

uint32_t terrain3dActiveStorageGetOwnerGeneration( const void *pOwner ) {
	return terrain3dActiveOwnerMatches( pOwner ) ? s_nActiveOwnerGeneration.load( std::memory_order_acquire ) : 0u;
}

size_t terrain3dActiveStorageGetReservedBytes( void ) {
	return s_nActiveHeightBytes.load( std::memory_order_acquire );
}

T3DActiveMapLease terrain3dActiveStorageAcquire( const void *pOwner, const Vector2i &rLocation ) {
	if ( !terrain3dActiveOwnerMatches( pOwner ) ) {
		return {};
	}
	const int map_size = s_nActiveRegionMapSize.load( std::memory_order_acquire );
	const int region_size = s_nActiveRegionSize.load( std::memory_order_acquire );
	const int slot = terrain3dActiveLocationToSlot( rLocation, map_size );
	if ( slot < 0 ) {
		return {};
	}
	if ( !s_szActiveAccess[slot].acquireRead() ) {
		return {};
	}
	if ( !terrain3dActiveOwnerMatches( pOwner ) || s_szActivePresent[slot] == 0u ) {
		terrain3dActiveReleaseReader( slot );
		return {};
	}
	const uint16_t *pHeightPage = s_szActiveHeightPages[slot].pHeight;
	if ( !pHeightPage ) {
		terrain3dActiveReleaseReader( slot );
		return {};
	}
	t3dactiveMapView_t view;
	view.pHeight = pHeightPage;
	view.nOwnerGeneration = s_nActiveOwnerGeneration.load( std::memory_order_acquire );
	view.nRegionGeneration = s_szActiveRegionGeneration[slot].load( std::memory_order_acquire );
	view.nRegionSize = region_size;
	view.iSlot = slot;
	return T3DActiveMapLease( slot, view );
}

bool terrain3dActiveStorageSyncRegion(
	const void *pOwner, const Vector2i &rLocation,
	const T3DRegion *pRegion
) {
	if ( !terrain3dActiveOwnerMatches( pOwner ) || !pRegion || pRegion->isDeleted() ||
		pRegion->getRegionSize() != s_nActiveRegionSize.load( std::memory_order_acquire ) ) {
		return false;
	}
	const int slot = terrain3dActiveLocationToSlot( rLocation,
		s_nActiveRegionMapSize.load( std::memory_order_acquire ) );
	if ( slot < 0 ) {
		return false;
	}
	s_szActiveAccess[slot].acquireWrite();
	if ( !terrain3dActiveOwnerMatches( pOwner ) ) {
		s_szActiveAccess[slot].releaseWrite();
		return false;
	}
	const int region_size = s_nActiveRegionSize.load( std::memory_order_acquire );
	uint16_t *pHeightPage = terrain3dActiveAllocatePage( slot, region_size );
	const bool bSuccess = pHeightPage && terrain3dActiveCopyHeight( pRegion->getMapPtr( T3DRegion::TYPE_HEIGHT ),
		pHeightPage, region_size, Rect2i( V2I_ZERO, V2I( region_size ) ) );
	if ( !bSuccess ) {
		terrain3dActiveFreePage( slot, region_size );
	}
	s_szActivePresent[slot] = bSuccess ? 1u : 0u;
	terrain3dActiveBumpGeneration( s_szActiveRegionGeneration[slot] );
	s_szActiveAccess[slot].releaseWrite();
	return bSuccess;
}

bool terrain3dActiveStorageSyncHeightPage(
	const void *pOwner, const Vector2i &rLocation,
	const uint8_t *pHeightBytes, const size_t nHeightBytes
) {
	if ( !terrain3dActiveOwnerMatches( pOwner ) || !pHeightBytes ) {
		return false;
	}
	const int region_size = s_nActiveRegionSize.load( std::memory_order_acquire );
	const size_t expected_bytes = terrain3dActivePageBytes( region_size );
	if ( nHeightBytes != expected_bytes ) {
		return false;
	}
	const int slot = terrain3dActiveLocationToSlot( rLocation,
		s_nActiveRegionMapSize.load( std::memory_order_acquire ) );
	if ( slot < 0 ) {
		return false;
	}
	s_szActiveAccess[slot].acquireWrite();
	if ( !terrain3dActiveOwnerMatches( pOwner ) ) {
		s_szActiveAccess[slot].releaseWrite();
		return false;
	}
	uint16_t *const pHeightPage = terrain3dActiveAllocatePage( slot, region_size );
	const bool bSuccess = pHeightPage != nullptr;
	if ( bSuccess ) {
		std::memcpy( pHeightPage, pHeightBytes, expected_bytes );
	} else {
		terrain3dActiveFreePage( slot, region_size );
	}
	s_szActivePresent[slot] = bSuccess ? 1u : 0u;
	terrain3dActiveBumpGeneration( s_szActiveRegionGeneration[slot] );
	s_szActiveAccess[slot].releaseWrite();
	return bSuccess;
}

bool terrain3dActiveStorageReplaceHeightPages(
	const void *pOwner, const int nRegionSize,
	const uint8_t *const *pHeightPages, const int *pSlots, const size_t nPageCount
) {
	if ( !pOwner || !terrain3dActiveValidRegionSize( nRegionSize ) ||
		( nPageCount > 0u && ( !pHeightPages || !pSlots ) ) ) {
		return false;
	}
	const int map_size = TERRAIN3D_ACTIVE_WORLD_TEXELS / nRegionSize;
	const size_t active_slot_count = ( size_t ) ( map_size ) * ( size_t ) ( map_size );
	if ( active_slot_count > ( size_t ) ( TERRAIN3D_ACTIVE_MAX_REGION_SLOTS ) || nPageCount > active_slot_count ) {
		return false;
	}
	uint8_t staged_present[TERRAIN3D_ACTIVE_MAX_REGION_SLOTS] = {};
	for ( size_t index = 0; index < nPageCount; ++index ) {
		const int slot = pSlots[index];
		if ( slot < 0 || ( size_t ) ( slot ) >= active_slot_count || !pHeightPages[index] || staged_present[slot] != 0u ) {
			return false;
		}
		staged_present[slot] = 1u;
	}

	std::lock_guard<std::mutex> lock( s_activeOwnerMutex );
	if ( s_pActiveOwner.load( std::memory_order_acquire ) != pOwner ||
		s_nActiveOwnerState.load( std::memory_order_acquire ) == TERRAIN3D_ACTIVE_OWNER_FREE ) {
		return false;
	}

	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_DRAINING, std::memory_order_release );
	terrain3dActiveWaitForAllSlots();
	const int old_region_size = s_nActiveRegionSize.load( std::memory_order_acquire );
	terrain3dActiveFreeAllPages( old_region_size );
	s_nActiveRegionSize.store( nRegionSize, std::memory_order_release );
	s_nActiveRegionMapSize.store( map_size, std::memory_order_release );
	std::memset( s_szActivePresent, 0, sizeof( s_szActivePresent ) );
	const size_t page_bytes = terrain3dActivePageBytes( nRegionSize );
	bool bSuccess = true;
	for ( size_t index = 0; index < nPageCount; ++index ) {
		const int slot = pSlots[index];
		uint16_t *pHeightPage = terrain3dActiveAllocatePage( slot, nRegionSize );
		if ( !pHeightPage ) {
			bSuccess = false;
			break;
		}
		std::memcpy( pHeightPage, pHeightPages[index], page_bytes );
		s_szActivePresent[slot] = 1u;
	}
	if ( !bSuccess ) {
		terrain3dActiveFreeAllPages( nRegionSize );
		std::memset( s_szActivePresent, 0, sizeof( s_szActivePresent ) );
	}
	for ( int slot = 0; slot < TERRAIN3D_ACTIVE_MAX_REGION_SLOTS; ++slot ) {
		terrain3dActiveBumpGeneration( s_szActiveRegionGeneration[slot] );
	}
	terrain3dActiveBumpGeneration( s_nActiveOwnerGeneration );
	s_nActiveOwnerState.store( TERRAIN3D_ACTIVE_OWNER_OWNED, std::memory_order_release );
	return bSuccess;
}

bool terrain3dActiveStorageSyncMap(
	const void *pOwner, const Vector2i &rLocation,
	const T3DRegion *pRegion, const t3dmapType_t type, const Rect2i &rDirtyRect
) {
	if ( !terrain3dActiveOwnerMatches( pOwner ) || !pRegion || pRegion->isDeleted() ||
		pRegion->getRegionSize() != s_nActiveRegionSize.load( std::memory_order_acquire ) ||
		type < T3DRegion::TYPE_HEIGHT || type >= T3DRegion::TYPE_MAX ) {
		return false;
	}
	if ( type != T3DRegion::TYPE_HEIGHT ) {
		return true;
	}
	const int slot = terrain3dActiveLocationToSlot( rLocation,
		s_nActiveRegionMapSize.load( std::memory_order_acquire ) );
	if ( slot < 0 ) {
		return false;
	}
	s_szActiveAccess[slot].acquireWrite();
	if ( !terrain3dActiveOwnerMatches( pOwner ) ) {
		s_szActiveAccess[slot].releaseWrite();
		return false;
	}
	const int region_size = s_nActiveRegionSize.load( std::memory_order_acquire );
	uint16_t *pHeightPage = terrain3dActiveAllocatePage( slot, region_size );
	const bool bSuccess = pHeightPage && terrain3dActiveCopyHeight( pRegion->getMapPtr( T3DRegion::TYPE_HEIGHT ),
		pHeightPage, region_size, rDirtyRect );
	if ( bSuccess ) {
		s_szActivePresent[slot] = 1u;
		terrain3dActiveBumpGeneration( s_szActiveRegionGeneration[slot] );
	}
	s_szActiveAccess[slot].releaseWrite();
	return bSuccess;
}

bool terrain3dActiveStorageSyncDerived(
	const void *pOwner, const Vector2i &rLocation,
	const T3DRegion *pRegion, const Rect2i &
) {
	if ( !terrain3dActiveOwnerMatches( pOwner ) || !pRegion || pRegion->isDeleted() ||
		pRegion->getRegionSize() != s_nActiveRegionSize.load( std::memory_order_acquire ) ) {
		return false;
	}
	return terrain3dActiveLocationToSlot( rLocation,
		s_nActiveRegionMapSize.load( std::memory_order_acquire ) ) >= 0;
}

void terrain3dActiveStorageInvalidate( const void *pOwner, const Vector2i &rLocation ) {
	if ( !terrain3dActiveOwnerMatches( pOwner ) ) {
		return;
	}
	const int slot = terrain3dActiveLocationToSlot( rLocation,
		s_nActiveRegionMapSize.load( std::memory_order_acquire ) );
	if ( slot < 0 ) {
		return;
	}
	s_szActiveAccess[slot].acquireWrite();
	if ( !terrain3dActiveOwnerMatches( pOwner ) ) {
		s_szActiveAccess[slot].releaseWrite();
		return;
	}
	s_szActivePresent[slot] = 0u;
	terrain3dActiveFreePage( slot, s_nActiveRegionSize.load( std::memory_order_acquire ) );
	terrain3dActiveBumpGeneration( s_szActiveRegionGeneration[slot] );
	s_szActiveAccess[slot].releaseWrite();
}
