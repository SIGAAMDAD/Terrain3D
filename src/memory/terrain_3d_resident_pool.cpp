#include "Terrain3DConstants.h"
#include "memory/terrain_3d_resident_pool.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include <godot_cpp/classes/image.hpp>

#include "memory/terrain_3d_linear_arena.h"
#include "memory/terrain_3d_memory.h"
#include "data/terrain_3d_region_codec.h"

namespace {
}

T3DResidentMapLease::T3DResidentMapLease(
	T3DResidentRegionPool *const pPool, const int iSlot,
	const uint32_t nGeneration, const t3dresidentMapView_t &rView
)
	: m_pPool( pPool ), m_iSlot( ( int16_t ) ( iSlot ) ), m_nGeneration( nGeneration ), m_view( rView ) {
}
T3DResidentMapLease::~T3DResidentMapLease( void ) {
	reset();
}

T3DResidentMapLease::T3DResidentMapLease( T3DResidentMapLease &&rOther ) noexcept
	: m_pPool( rOther.m_pPool ), m_iSlot( rOther.m_iSlot ), m_nGeneration( rOther.m_nGeneration ), m_view( rOther.m_view ) {
	rOther.m_pPool = nullptr;
	rOther.m_iSlot = -1;
	rOther.m_nGeneration = 0u;
	rOther.m_view = {};
}

T3DResidentMapLease &T3DResidentMapLease::operator=( T3DResidentMapLease &&rOther ) noexcept {
	if ( this == &rOther ) {
		return *this;
	}
	reset();
	m_pPool = rOther.m_pPool;
	m_iSlot = rOther.m_iSlot;
	m_nGeneration = rOther.m_nGeneration;
	m_view = rOther.m_view;
	rOther.m_pPool = nullptr;
	rOther.m_iSlot = -1;
	rOther.m_nGeneration = 0u;
	rOther.m_view = {};
	return *this;
}

void T3DResidentMapLease::reset( void ) {
	if ( m_pPool && m_iSlot >= 0 ) {
		m_pPool->releaseLease( m_iSlot, m_nGeneration );
	}
	m_pPool = nullptr;
	m_iSlot = -1;
	m_nGeneration = 0u;
	m_view = {};
}

bool T3DResidentRegionPool::alignUp( const size_t nValue, const size_t nAlignment, size_t &nAligned ) {
	if ( nAlignment == 0u || ( nAlignment & ( nAlignment - 1u ) ) != 0u ) {
		return false;
	}
	const size_t mask = nAlignment - 1u;
	if ( nValue > std::numeric_limits<size_t>::max() - mask ) {
		return false;
	}
	nAligned = ( nValue + mask ) & ~mask;
	return true;
}

bool T3DResidentRegionPool::buildSlotLayout(
	const int nRegionSize, const uint32_t nChannels,
	t3dslotLayout_t &rLayout, size_t &nTexelCount
) {
	rLayout = {};
	nTexelCount = 0u;
	if ( nRegionSize <= 0 || ( nChannels & CHANNEL_HEIGHT ) == 0u ) {
		return false;
	}
	const size_t size = ( size_t ) ( nRegionSize );
	if ( size > std::numeric_limits<size_t>::max() / size ) {
		return false;
	}
	const size_t texels = size * size;
	T3DLinearArenaLayout layout;

	if ( texels > std::numeric_limits<size_t>::max() / sizeof( uint16_t ) ) {
		return false;
	}
	const size_t height_bytes = texels * sizeof( uint16_t );
	size_t nPaddedHeightBytes = 0u;
	if ( !alignUp( height_bytes, TERRAIN3D_RESIDENT_CACHE_ALIGNMENT, nPaddedHeightBytes ) ||
		!layout.add( nPaddedHeightBytes, TERRAIN3D_RESIDENT_CACHE_ALIGNMENT, &rLayout.iHeightOffset ) || !layout.valid() ) {
		return false;
	}
	rLayout.stride = layout.size();
	nTexelCount = texels;
	return rLayout.stride > 0u;
}

bool T3DResidentRegionPool::addEstimatedOptionalBytes(
	const uint32_t nChannels,
	const uint32_t nChannel, const size_t nTexelCount, const size_t nBytesPerTexel, size_t &nTotal
) {
	if ( ( nChannels & nChannel ) == 0u ) {
		return true;
	}
	if ( nTexelCount > std::numeric_limits<size_t>::max() / nBytesPerTexel ) {
		return false;
	}
	size_t padded = 0u;
	if ( !alignUp( nTexelCount * nBytesPerTexel, TERRAIN3D_RESIDENT_CACHE_ALIGNMENT, padded ) ||
		nTotal > std::numeric_limits<size_t>::max() - padded ) {
		return false;
	}
	nTotal += padded;
	return true;
}

size_t T3DResidentRegionPool::estimateSlotBytes( const int nRegionSize, const uint32_t nChannels ) {
	t3dslotLayout_t layout;
	size_t texel_count = 0u;
	if ( !buildSlotLayout( nRegionSize, nChannels, layout, texel_count ) ) {
		return 0u;
	}
	size_t total = layout.stride;
	return addEstimatedOptionalBytes( nChannels, CHANNEL_CONTROL, texel_count, sizeof( uint32_t ), total ) &&
			addEstimatedOptionalBytes( nChannels, CHANNEL_COLOR, texel_count, sizeof( uint32_t ), total ) &&
			addEstimatedOptionalBytes( nChannels, CHANNEL_DERIVED, texel_count, sizeof( uint16_t ), total )
		? total
		: 0u;
}

bool T3DResidentRegionPool::t3dalignedAllocation_t::allocate( const size_t nRequestedBytes, const size_t nAlignment ) {
	release();
	if ( nRequestedBytes == 0u || nAlignment == 0u || ( nAlignment & ( nAlignment - 1u ) ) != 0u ||
		nRequestedBytes > std::numeric_limits<size_t>::max() - ( nAlignment - 1u ) ) {
		return false;
	}
	pRaw = terrain3d_memory::allocate( nRequestedBytes + nAlignment - 1u );
	if ( !pRaw ) {
		return false;
	}
	const uintptr_t start = ( uintptr_t ) ( pRaw );
	const uintptr_t mask = ( uintptr_t ) ( nAlignment - 1u );
	pAligned = ( uint8_t * ) ( ( start + mask ) & ~mask );
	nBytes = nRequestedBytes;
	std::memset( pAligned, 0, nBytes );
	return true;
}

void T3DResidentRegionPool::t3dalignedAllocation_t::release( void ) {
	terrain3d_memory::release( pRaw );
	pRaw = nullptr;
	pAligned = nullptr;
	nBytes = 0u;
}

T3DResidentRegionPool::~T3DResidentRegionPool( void ) {
	clear();
}

int T3DResidentRegionPool::locationIndex( const Vector2i &rLocation ) {
	const int x = rLocation.x + TERRAIN3D_RESIDENT_WORLD_HALF;
	const int y = rLocation.y + TERRAIN3D_RESIDENT_WORLD_HALF;
	return ( x < 0 || x >= TERRAIN3D_RESIDENT_WORLD_SIZE || y < 0 ||
		y >= TERRAIN3D_RESIDENT_WORLD_SIZE )
		? -1
		: y * TERRAIN3D_RESIDENT_WORLD_SIZE + x;
}

bool T3DResidentRegionPool::hasPinnedSlotsUnlocked( void ) const {
	if ( !m_pSlots ) {
		return false;
	}
	for ( int i = 0; i < m_nCapacity; ++i ) {
		if ( m_pSlots[( size_t ) ( i )].nPinCount != 0u ) {
			return true;
		}
	}
	return false;
}

bool T3DResidentRegionPool::allocateSlotsUnlocked( const int nCapacity ) {
	releaseSlotsUnlocked();
	if ( nCapacity <= 0 || ( size_t ) ( nCapacity ) > std::numeric_limits<size_t>::max() / sizeof( t3dslot_t ) ) {
		return false;
	}
	m_pSlots = ( t3dslot_t * ) ( terrain3d_memory::allocate( ( size_t ) ( nCapacity ) * sizeof( t3dslot_t ) ) );
	if ( !m_pSlots ) {
		return false;
	}
	for ( int i = 0; i < nCapacity; ++i ) {
		new ( &m_pSlots[( size_t ) ( i )] ) t3dslot_t;
	}
	return true;
}

void T3DResidentRegionPool::releaseSlotsUnlocked( void ) {
	if ( !m_pSlots ) {
		return;
	}
	for ( int i = 0; i < m_nCapacity; ++i ) {
		t3dslot_t &slot = m_pSlots[( size_t ) ( i )];
		releaseSparseStorageUnlocked( slot );
		slot.~t3dslot_s();
	}
	terrain3d_memory::release( m_pSlots );
	m_pSlots = nullptr;
}

void T3DResidentRegionPool::releaseSparseStorageUnlocked( t3dslot_t &iSlot ) {
	void *control = iSlot.pControl;
	void *color = iSlot.pColor;
	void *derived = iSlot.pDerived;
	releaseSparsePlaneUnlocked( iSlot.controlMemory, control );
	releaseSparsePlaneUnlocked( iSlot.colorMemory, color );
	releaseSparsePlaneUnlocked( iSlot.derivedMemory, derived );
	iSlot.pControl = nullptr;
	iSlot.pColor = nullptr;
	iSlot.pDerived = nullptr;
	iSlot.bDerivedValid = false;
}

bool T3DResidentRegionPool::allocateSparsePlaneUnlocked(
	t3dalignedAllocation_t &rMemory,
	const size_t nBytes, void *&pPlane
) {
	if ( rMemory.pAligned ) {
		pPlane = rMemory.pAligned;
		return true;
	}
	if ( !rMemory.allocate( nBytes, TERRAIN3D_RESIDENT_CACHE_ALIGNMENT ) ) {
		pPlane = nullptr;
		return false;
	}
	m_nReservedBytes += rMemory.nBytes;
	pPlane = rMemory.pAligned;
	return true;
}

void T3DResidentRegionPool::releaseSparsePlaneUnlocked( t3dalignedAllocation_t &rMemory, void *&pPlane ) {
	if ( rMemory.pAligned ) {
		m_nReservedBytes -= MIN( m_nReservedBytes, rMemory.nBytes );
		rMemory.release();
	}
	pPlane = nullptr;
}

void T3DResidentRegionPool::clearUnlocked( void ) {
	releaseSlotsUnlocked();
	m_mapMemory.release();
	std::fill_n( m_szLocationToSlot, TERRAIN3D_RESIDENT_WORLD_SLOT_COUNT, ( int16_t ) ( -1 ) );
	m_nRegionSize = 0;
	m_nCapacity = 0;
	m_channels = 0u;
	m_nTexelCount = 0u;
	m_nReservedBytes = 0u;
	m_iSlotLayout = {};
	m_iClockHand = 0;
}

bool T3DResidentRegionPool::configure( const int nRegionSize, const int nCapacity, const uint32_t nChannels ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( nRegionSize <= 0 || nCapacity <= 0 || nCapacity > TERRAIN3D_RESIDENT_WORLD_SLOT_COUNT ||
		( nChannels & CHANNEL_HEIGHT ) == 0u ) {
		return false;
	}
	if ( m_nRegionSize == nRegionSize && m_nCapacity == nCapacity && m_channels == nChannels && m_mapMemory.pAligned &&
		m_pSlots ) {
		return true;
	}

	// Reconfiguration would invalidate raw map pointers held by active sampler
	// leases. Keep the current pool intact and let the caller retry after the
	// in-flight sampling operation completes.
	if ( hasPinnedSlotsUnlocked() ) {
		return false;
	}

	clearUnlocked();
	t3dslotLayout_t layout;
	size_t texel_count = 0u;
	if ( !buildSlotLayout( nRegionSize, nChannels, layout, texel_count ) ||
		( size_t ) ( nCapacity ) > std::numeric_limits<size_t>::max() / layout.stride ) {
		return false;
	}
	const size_t total_bytes = ( size_t ) ( nCapacity ) * layout.stride;
	if ( !m_mapMemory.allocate( total_bytes, TERRAIN3D_RESIDENT_CACHE_ALIGNMENT ) ) {
		return false;
	}

	m_nRegionSize = nRegionSize;
	m_nCapacity = nCapacity;
	m_channels = nChannels;
	m_nTexelCount = texel_count;
	m_nReservedBytes = total_bytes;
	m_iSlotLayout = layout;
	if ( !allocateSlotsUnlocked( m_nCapacity ) ) {
		clearUnlocked();
		return false;
	}

	for ( int i = 0; i < m_nCapacity; ++i ) {
		t3dslot_t &slot = m_pSlots[( size_t ) ( i )];
		uint8_t *const pBase = m_mapMemory.pAligned + ( size_t ) ( i ) * m_iSlotLayout.stride;
		slot.pHeight = ( uint16_t * ) ( pBase + m_iSlotLayout.iHeightOffset );
		if ( !resetSlotStorage( i, false ) ) {
			clearUnlocked();
			return false;
		}
	}
	std::fill_n( m_szLocationToSlot, TERRAIN3D_RESIDENT_WORLD_SLOT_COUNT, ( int16_t ) ( -1 ) );
	return true;
}

bool T3DResidentRegionPool::resetSlotStorage( const int iSlot, const bool bZero ) {
	if ( !m_pSlots || iSlot < 0 || iSlot >= m_nCapacity ) {
		return false;
	}
	t3dslot_t &slot = m_pSlots[( size_t ) ( iSlot )];
	if ( bZero ) {
		std::memset( slot.pHeight, 0, m_nTexelCount * sizeof( uint16_t ) );
		if ( slot.pControl ) {
			std::memset( slot.pControl, 0, m_nTexelCount * sizeof( uint32_t ) );
		}
		if ( slot.pColor ) {
			std::memset( slot.pColor, 0, m_nTexelCount * sizeof( uint32_t ) );
		}
		if ( slot.pDerived ) {
			std::memset( slot.pDerived, 0, m_nTexelCount * sizeof( uint16_t ) );
		}
	}
	slot.bDerivedValid = false;
	return slot.pHeight != nullptr;
}

void T3DResidentRegionPool::resetResidency( void ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( !m_pSlots ) {
		return;
	}
	// Unmap every region immediately, but preserve pinned planes until their
	// readers release them. This gives in-flight samplers an immutable snapshot
	// instead of clearing storage underneath their raw pointers.
	for ( int i = 0; i < m_nCapacity; ++i ) {
		if ( m_pSlots[( size_t ) ( i )].bOccupied ) {
			retireSlotUnlocked( i );
		}
	}
	std::fill_n( m_szLocationToSlot, TERRAIN3D_RESIDENT_WORLD_SLOT_COUNT, ( int16_t ) ( -1 ) );
	m_iClockHand = 0;
}

void T3DResidentRegionPool::clear( void ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	clearUnlocked();
}

int T3DResidentRegionPool::chooseReplacementSlotUnlocked( void ) {
	if ( !m_pSlots || m_nCapacity <= 0 ) {
		return -1;
	}
	for ( int i = 0; i < m_nCapacity; ++i ) {
		const int slot_index = ( m_iClockHand + i ) % m_nCapacity;
		if ( !m_pSlots[( size_t ) ( slot_index )].bOccupied ) {
			m_iClockHand = ( slot_index + 1 ) % m_nCapacity;
			return slot_index;
		}
	}

	// Clock replacement keeps the metadata scan linear and cache-friendly. A
	// pinned slot is never overwritten; samplers retain leases while raw map
	// pointers are cached. Two sweeps are sufficient to clear reference bits and
	// select the oldest unpinned working-set member.
	for ( int pass = 0; pass < 2; ++pass ) {
		for ( int i = 0; i < m_nCapacity; ++i ) {
			const int slot_index = m_iClockHand;
			m_iClockHand = ( m_iClockHand + 1 ) % m_nCapacity;
			t3dslot_t &slot = m_pSlots[( size_t ) ( slot_index )];
			if ( slot.nPinCount != 0u ) {
				continue;
			}
			if ( slot.bReferenced ) {
				slot.bReferenced = false;
				continue;
			}
			return slot_index;
		}
	}
	return -1;
}

int T3DResidentRegionPool::findSlotUnlocked( const Vector2i &rLocation ) const {
	const int index = locationIndex( rLocation );
	if ( index < 0 ) {
		return -1;
	}
	const int slot = m_szLocationToSlot[index];
	if ( slot < 0 || slot >= m_nCapacity ) {
		return -1;
	}
	const t3dslot_t &entry = m_pSlots[( size_t ) ( slot )];
	return entry.bOccupied && entry.location == rLocation ? slot : -1;
}

void T3DResidentRegionPool::assignSlot( const int iSlot, const Vector2i &rLocation ) {
	t3dslot_t &slot = m_pSlots[( size_t ) ( iSlot )];
	if ( slot.bOccupied ) {
		const int old_index = locationIndex( slot.location );
		if ( old_index >= 0 && m_szLocationToSlot[old_index] == iSlot ) {
			m_szLocationToSlot[old_index] = -1;
		}
	}
	releaseSparseStorageUnlocked( slot );
	resetSlotStorage( iSlot, false );
	slot.location = rLocation;
	slot.bOccupied = true;
	slot.bReferenced = true;
	slot.bPendingInvalidate = false;
	slot.nPinCount = 0u;
	if ( ++slot.nGeneration == 0u ) {
		slot.nGeneration = 1u;
	}
	const int index = locationIndex( rLocation );
	if ( index >= 0 ) {
		m_szLocationToSlot[index] = ( int16_t ) ( iSlot );
	}
}

void T3DResidentRegionPool::retireSlotUnlocked( const int iSlot ) {
	if ( !m_pSlots || iSlot < 0 || iSlot >= m_nCapacity ) {
		return;
	}
	t3dslot_t &slot = m_pSlots[( size_t ) ( iSlot )];
	const int old_index = locationIndex( slot.location );
	if ( old_index >= 0 && m_szLocationToSlot[old_index] == iSlot ) {
		m_szLocationToSlot[old_index] = -1;
	}
	if ( slot.nPinCount != 0u ) {
		slot.bPendingInvalidate = true;
		return;
	}
	releaseSparseStorageUnlocked( slot );
	slot.bOccupied = false;
	slot.bReferenced = false;
	slot.bPendingInvalidate = false;
	slot.location = Vector2i( INT32_MAX, INT32_MAX );
	resetSlotStorage( iSlot, false );
	if ( ++slot.nGeneration == 0u ) {
		slot.nGeneration = 1u;
	}
}

void T3DResidentRegionPool::releaseLease( const int iSlot, const uint32_t nGeneration ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( !m_pSlots || iSlot < 0 || iSlot >= m_nCapacity ) {
		return;
	}
	t3dslot_t &slot = m_pSlots[( size_t ) ( iSlot )];
	if ( slot.nGeneration != nGeneration || slot.nPinCount == 0u ) {
		return;
	}
	--slot.nPinCount;
	if ( slot.nPinCount == 0u && slot.bPendingInvalidate ) {
		retireSlotUnlocked( iSlot );
	}
}

bool T3DResidentRegionPool::copyMap( const int iSlot, const T3DRegion *pRegion, const t3dmapType_t eType ) {
	return copyMapRect( iSlot, pRegion, eType, Rect2i( V2I_ZERO, V2I( m_nRegionSize ) ) );
}

bool T3DResidentRegionPool::copyMapRect(
	const int iSlot, const T3DRegion *pRegion,
	const t3dmapType_t eType, const Rect2i &rDirtyRect
) {
	if ( !pRegion || iSlot < 0 || iSlot >= m_nCapacity ) {
		return false;
	}
	const Rect2i bounds( V2I_ZERO, V2I( m_nRegionSize ) );
	Rect2i rect = rDirtyRect.size.x > 0 && rDirtyRect.size.y > 0 ? rDirtyRect.intersection( bounds ) : bounds;
	if ( rect.size.x <= 0 || rect.size.y <= 0 ) {
		return true;
	}
	t3dslot_t &slot = m_pSlots[( size_t ) ( iSlot )];
	Image *const pImage = pRegion->getMapPtr( eType );
	if ( eType == T3DRegion::TYPE_HEIGHT ) {
		if ( !slot.pHeight ) {
			return true;
		}
		if ( !pImage || pImage->get_format() != Image::FORMAT_R16 || pImage->get_width() != m_nRegionSize ||
			pImage->get_height() != m_nRegionSize ) {
			return false;
		}
		const PackedByteArray data = pImage->get_data();
		const size_t bytes = m_nTexelCount * sizeof( uint16_t );
		if ( ( size_t ) ( data.size() ) < bytes ) {
			return false;
		}
		const uint16_t *pSource = ( const uint16_t * ) ( data.ptr() );
		if ( rect == bounds ) {
			std::memcpy( slot.pHeight, pSource, bytes );
		} else {
			for ( int y = rect.position.y; y < rect.get_end().y; ++y ) {
				const size_t offset = ( size_t ) ( y ) * ( size_t ) ( m_nRegionSize ) + ( size_t ) ( rect.position.x );
				std::memcpy( slot.pHeight + offset, pSource + offset, ( size_t ) ( rect.size.x ) * sizeof( uint16_t ) );
			}
		}
		slot.bDerivedValid = false;
		return true;
	}

	if ( eType != T3DRegion::TYPE_CONTROL && eType != T3DRegion::TYPE_COLOR ) {
		return false;
	}
	const uint32_t channel = eType == T3DRegion::TYPE_CONTROL ? CHANNEL_CONTROL : CHANNEL_COLOR;
	if ( ( m_channels & channel ) == 0u ) {
		return true;
	}
	uint32_t *&plane = eType == T3DRegion::TYPE_CONTROL ? slot.pControl : slot.pColor;
	t3dalignedAllocation_t &memory = eType == T3DRegion::TYPE_CONTROL ? slot.controlMemory : slot.colorMemory;
	if ( !pImage ) {
		void *pUntyped = plane;
		releaseSparsePlaneUnlocked( memory, pUntyped );
		plane = nullptr;
		return true;
	}
	const Image::Format expected = eType == T3DRegion::TYPE_CONTROL ? Image::FORMAT_RF : Image::FORMAT_RGBA8;
	if ( pImage->get_format() != expected || pImage->get_width() != m_nRegionSize || pImage->get_height() != m_nRegionSize ) {
		return false;
	}
	const PackedByteArray data = pImage->get_data();
	const size_t bytes = m_nTexelCount * sizeof( uint32_t );
	if ( ( size_t ) ( data.size() ) < bytes ) {
		return false;
	}
	const bool bHadPlane = plane != nullptr;
	void *pUntyped = plane;
	if ( !allocateSparsePlaneUnlocked( memory, bytes, pUntyped ) ) {
		return false;
	}
	plane = ( uint32_t * ) ( pUntyped );
	const uint32_t *pSource = ( const uint32_t * ) ( data.ptr() );
	if ( !bHadPlane || rect == bounds ) {
		std::memcpy( plane, pSource, bytes );
	} else {
		for ( int y = rect.position.y; y < rect.get_end().y; ++y ) {
			const size_t offset = ( size_t ) ( y ) * ( size_t ) ( m_nRegionSize ) + ( size_t ) ( rect.position.x );
			std::memcpy( plane + offset, pSource + offset, ( size_t ) ( rect.size.x ) * sizeof( uint32_t ) );
		}
	}
	return true;
}

bool T3DResidentRegionPool::copyDerived( const int iSlot, const T3DRegion *pRegion ) {
	return copyDerivedRect( iSlot, pRegion, Rect2i( V2I_ZERO, V2I( m_nRegionSize ) ) );
}

bool T3DResidentRegionPool::copyDerivedRect(
	const int iSlot, const T3DRegion *pRegion,
	const Rect2i &rDirtyRect
) {
	if ( !pRegion || iSlot < 0 || iSlot >= m_nCapacity ) {
		return false;
	}
	t3dslot_t &slot = m_pSlots[( size_t ) ( iSlot )];
	if ( ( m_channels & CHANNEL_DERIVED ) == 0u ) {
		slot.bDerivedValid = false;
		return true;
	}
	const Ref<Image> pImage = pRegion->getDerivedNormalSlopeMap();
	const size_t bytes = m_nTexelCount * 2u;
	if ( pImage.is_null() || pImage->get_format() != Image::FORMAT_RG8 || pImage->get_width() != m_nRegionSize ||
		pImage->get_height() != m_nRegionSize ) {
		void *plane = slot.pDerived;
		releaseSparsePlaneUnlocked( slot.derivedMemory, plane );
		slot.pDerived = nullptr;
		slot.bDerivedValid = false;
		return true;
	}
	const PackedByteArray data = pImage->get_data();
	if ( ( size_t ) ( data.size() ) < bytes ) {
		return false;
	}
	const bool bHadPlane = slot.pDerived != nullptr;
	void *plane = slot.pDerived;
	if ( !allocateSparsePlaneUnlocked( slot.derivedMemory, bytes, plane ) ) {
		return false;
	}
	slot.pDerived = ( uint8_t * ) ( plane );
	const Rect2i bounds( V2I_ZERO, V2I( m_nRegionSize ) );
	const Rect2i rect = rDirtyRect.size.x > 0 && rDirtyRect.size.y > 0 ? rDirtyRect.intersection( bounds ) : bounds;
	if ( rect.size.x <= 0 || rect.size.y <= 0 ) {
		return true;
	}
	if ( !bHadPlane || rect == bounds ) {
		std::memcpy( slot.pDerived, data.ptr(), bytes );
	} else {
		for ( int y = rect.position.y; y < rect.get_end().y; ++y ) {
			const size_t offset = ( ( size_t ) ( y ) * ( size_t ) ( m_nRegionSize ) + ( size_t ) ( rect.position.x ) ) * 2u;
			std::memcpy( slot.pDerived + offset, data.ptr() + offset, ( size_t ) ( rect.size.x ) * 2u );
		}
	}
	slot.bDerivedValid = true;
	return true;
}

t3dresidentMapView_t T3DResidentRegionPool::getView( const int iSlot ) const {
	if ( iSlot < 0 || iSlot >= m_nCapacity || !m_pSlots[( size_t ) ( iSlot )].bOccupied ) {
		return {};
	}
	const t3dslot_t &slot = m_pSlots[( size_t ) ( iSlot )];
	return {slot.pHeight, slot.pControl, slot.pColor, slot.bDerivedValid ? slot.pDerived : nullptr, slot.nGeneration};
}

T3DResidentMapLease T3DResidentRegionPool::acquire(
	const Vector2i &rLocation,
	const T3DRegion *pRegion
) {
	std::lock_guard<std::mutex> lock( m_mutex );
	const int existing = findSlotUnlocked( rLocation );
	if ( existing >= 0 ) {
		t3dslot_t &entry = m_pSlots[( size_t ) ( existing )];
		if ( entry.nPinCount == std::numeric_limits<uint32_t>::max() ) {
			return {};
		}
		entry.bReferenced = true;
		++entry.nPinCount;
		const t3dresidentMapView_t view = getView( existing );
		return T3DResidentMapLease( this, existing, entry.nGeneration, view );
	}
	const int slot = chooseReplacementSlotUnlocked();
	if ( slot < 0 ) {
		return {};
	}
	assignSlot( slot, rLocation );
	if ( !copyMap( slot, pRegion, T3DRegion::TYPE_HEIGHT ) || !copyMap( slot, pRegion, T3DRegion::TYPE_CONTROL ) ||
		!copyMap( slot, pRegion, T3DRegion::TYPE_COLOR ) ) {
		retireSlotUnlocked( slot );
		return {};
	}
	copyDerived( slot, pRegion );
	t3dslot_t &entry = m_pSlots[( size_t ) ( slot )];
	entry.bReferenced = true;
	entry.nPinCount = 1u;
	const t3dresidentMapView_t view = getView( slot );
	return T3DResidentMapLease( this, slot, entry.nGeneration, view );
}

void T3DResidentRegionPool::syncMap(
	const Vector2i &rLocation, const T3DRegion *pRegion,
	const t3dmapType_t eType, const Rect2i &rDirtyRect
) {
	std::lock_guard<std::mutex> lock( m_mutex );
	const int slot = findSlotUnlocked( rLocation );
	if ( slot >= 0 ) {
		t3dslot_t &entry = m_pSlots[( size_t ) ( slot )];
		if ( entry.nPinCount != 0u ) {
			// Never mutate a plane that is being sampled without the pool mutex.
			// Removing the lookup mapping makes the next acquire populate a fresh
			// slot while the old lease keeps its immutable snapshot.
			retireSlotUnlocked( slot );
			return;
		}
		if ( !copyMapRect( slot, pRegion, eType, rDirtyRect ) ) {
			retireSlotUnlocked( slot );
		}
	}
}

void T3DResidentRegionPool::syncDerived(
	const Vector2i &rLocation, const T3DRegion *pRegion,
	const Rect2i &rDirtyRect
) {
	std::lock_guard<std::mutex> lock( m_mutex );
	const int slot = findSlotUnlocked( rLocation );
	if ( slot >= 0 ) {
		t3dslot_t &entry = m_pSlots[( size_t ) ( slot )];
		if ( entry.nPinCount != 0u ) {
			retireSlotUnlocked( slot );
			return;
		}
		if ( !copyDerivedRect( slot, pRegion, rDirtyRect ) ) {
			retireSlotUnlocked( slot );
		}
	}
}

void T3DResidentRegionPool::invalidate( const Vector2i &rLocation ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	const int slot = findSlotUnlocked( rLocation );
	if ( slot >= 0 ) {
		retireSlotUnlocked( slot );
	}
}
