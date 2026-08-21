#include "memory/terrain_3d_resident_pool.h"

#include "Terrain3DConstants.h"
#include "data/terrain_3d_region_codec.h"
#include "memory/terrain_3d_memory.h"

#include <algorithm>
#include <cstring>
#include <godot_cpp/classes/image.hpp>
#include <limits>
#include <new>

static constexpr uint8_t s_nResidentSlotFree = 0u;
static constexpr uint8_t s_nResidentSlotStaging = 1u;
static constexpr uint8_t s_nResidentSlotPublished = 2u;
static constexpr uint8_t s_nResidentSlotRetiring = 3u;

T3DResidentMapLease::T3DResidentMapLease( T3DResidentRegionPool *const pPool,
		const int iSlot,
		const uint32_t nGeneration,
		const t3dResidentMapView_t &rView ) :
		m_pPool( pPool ), m_iSlot( ( int16_t )( iSlot ) ), m_nGeneration( nGeneration ), m_view( rView ) {}
T3DResidentMapLease::~T3DResidentMapLease( void ) {
	reset();
}

T3DResidentMapLease::T3DResidentMapLease( T3DResidentMapLease &&rOther ) noexcept :
		m_pPool( rOther.m_pPool ),
		m_iSlot( rOther.m_iSlot ),
		m_nGeneration( rOther.m_nGeneration ),
		m_view( rOther.m_view ) {
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

bool T3DResidentRegionPool::buildSlotLayout( const int nRegionSize,
		const uint32_t nChannels,
		t3dSlotLayout_t &rLayout,
		size_t &nTexelCount ) {
	rLayout = {};
	nTexelCount = 0u;
	if ( nRegionSize <= 0 || ( nChannels & RESIDENT_CHANNEL_HEIGHT ) == 0u ) {
		return false;
	}
	const size_t size = ( size_t )( nRegionSize );
	if ( size > std::numeric_limits<size_t>::max() / size ) {
		return false;
	}
	const size_t texels = size * size;
	if ( texels > std::numeric_limits<size_t>::max() / sizeof( uint16_t ) ) {
		return false;
	}
	const size_t height_bytes = texels * sizeof( uint16_t );
	size_t padded = 0u;
	if ( !alignUp( height_bytes, s_nResidentCacheAlignment, padded ) ) {
		return false;
	}
	rLayout.m_nHeightSlotBytes = padded;
	rLayout.m_nStride = padded;
	if ( ( nChannels & RESIDENT_CHANNEL_CONTROL ) != 0u ) {
		if ( !alignUp( texels * sizeof( uint32_t ), s_nResidentCacheAlignment, padded ) ||
				rLayout.m_nStride > std::numeric_limits<size_t>::max() - padded ) {
			return false;
		}
		rLayout.m_nControlSlotBytes = padded;
		rLayout.m_nStride += padded;
	}
	if ( ( nChannels & RESIDENT_CHANNEL_COLOR ) != 0u ) {
		if ( !alignUp( texels * sizeof( uint32_t ), s_nResidentCacheAlignment, padded ) ||
				rLayout.m_nStride > std::numeric_limits<size_t>::max() - padded ) {
			return false;
		}
		rLayout.m_nColorSlotBytes = padded;
		rLayout.m_nStride += padded;
	}
	if ( ( nChannels & RESIDENT_CHANNEL_DERIVED ) != 0u ) {
		if ( !alignUp( texels * sizeof( uint16_t ), s_nResidentCacheAlignment, padded ) ||
				rLayout.m_nStride > std::numeric_limits<size_t>::max() - padded ) {
			return false;
		}
		rLayout.m_nDerivedSlotBytes = padded;
		rLayout.m_nStride += padded;
	}
	nTexelCount = texels;
	return rLayout.m_nStride > 0u;
}

bool T3DResidentRegionPool::addEstimatedOptionalBytes( const uint32_t nChannels,
		const uint32_t nChannel,
		const size_t nTexelCount,
		const size_t nBytesPerTexel,
		size_t &nTotal ) {
	if ( ( nChannels & nChannel ) == 0u ) {
		return true;
	}
	if ( nTexelCount > std::numeric_limits<size_t>::max() / nBytesPerTexel ) {
		return false;
	}
	size_t padded = 0u;
	if ( !alignUp( nTexelCount * nBytesPerTexel, s_nResidentCacheAlignment, padded ) ||
			nTotal > std::numeric_limits<size_t>::max() - padded ) {
		return false;
	}
	nTotal += padded;
	return true;
}

size_t T3DResidentRegionPool::estimateSlotBytes( const int nRegionSize, const uint32_t nChannels ) {
	t3dSlotLayout_t layout;
	size_t texel_count = 0u;
	if ( !buildSlotLayout( nRegionSize, nChannels, layout, texel_count ) ) {
		return 0u;
	}
	return layout.m_nStride;
}

bool t3dAlignedAllocation_t::allocate( const size_t nRequestedBytes, const size_t nAlignment ) {
	release();
	if ( nRequestedBytes == 0u || nAlignment == 0u || ( nAlignment & ( nAlignment - 1u ) ) != 0u ||
			nRequestedBytes > std::numeric_limits<size_t>::max() - ( nAlignment - 1u ) ) {
		return false;
	}
	pRaw = t3dMemoryAllocate( nRequestedBytes + nAlignment - 1u );
	if ( !pRaw ) {
		return false;
	}
	const uintptr_t start = ( uintptr_t )( pRaw );
	const uintptr_t mask = ( uintptr_t )( nAlignment - 1u );
	pAligned = ( uint8_t * )( ( start + mask ) & ~mask );
	nBytes = nRequestedBytes;
	bOwned = true;
	std::memset( pAligned, 0, nBytes );
	return true;
}

void t3dAlignedAllocation_t::release( void ) {
	if ( bOwned ) {
		t3dMemoryRelease( pRaw );
	}
	pRaw = nullptr;
	pAligned = nullptr;
	nBytes = 0u;
	bOwned = false;
}

T3DResidentRegionPool::~T3DResidentRegionPool( void ) {
	clear();
}

int T3DResidentRegionPool::locationIndex( const Vector2i &rLocation ) {
	const int x = rLocation.x + s_nResidentWorldHalf;
	const int y = rLocation.y + s_nResidentWorldHalf;
	return ( x < 0 || x >= s_nResidentWorldSize || y < 0 || y >= s_nResidentWorldSize ) ? -1
																						: y * s_nResidentWorldSize + x;
}

bool T3DResidentRegionPool::hasPinnedSlotsUnlocked( void ) const {
	if ( !m_pSlots ) {
		return false;
	}
	for ( int i = 0; i < m_nCapacity; ++i ) {
		if ( m_pSlots[( size_t )( i )].m_nPinCount.load( std::memory_order_acquire ) != 0u ) {
			return true;
		}
	}
	return false;
}

bool T3DResidentRegionPool::allocateSlotsUnlocked( const int nCapacity ) {
	releaseSlotsUnlocked();
	if ( nCapacity <= 0 || ( size_t )( nCapacity ) > std::numeric_limits<size_t>::max() / sizeof( t3dSlot_t ) ) {
		return false;
	}
	m_pSlots = ( t3dSlot_t * )( t3dMemoryAllocate( ( size_t )( nCapacity ) * sizeof( t3dSlot_t ) ) );
	if ( !m_pSlots ) {
		return false;
	}
	m_bSlotsOwned = true;
	for ( int i = 0; i < nCapacity; ++i ) {
		new ( &m_pSlots[( size_t )( i )] ) t3dSlot_t;
	}
	return true;
}

bool T3DResidentRegionPool::assignMapBankPointersUnlocked( void ) {
	if ( !m_pSlots || !m_mapMemory.m_pAligned || m_nCapacity <= 0 ) {
		return false;
	}
	uint8_t *cursor = m_mapMemory.m_pAligned;
	uint8_t *const pHeightBank = cursor;
	cursor += m_iSlotLayout.m_nHeightSlotBytes * ( size_t )( m_nCapacity );
	uint8_t *pControlBank = nullptr;
	uint8_t *pColorBank = nullptr;
	uint8_t *pDerivedBank = nullptr;
	if ( m_iSlotLayout.m_nControlSlotBytes > 0u ) {
		pControlBank = cursor;
		cursor += m_iSlotLayout.m_nControlSlotBytes * ( size_t )( m_nCapacity );
	}
	if ( m_iSlotLayout.m_nColorSlotBytes > 0u ) {
		pColorBank = cursor;
		cursor += m_iSlotLayout.m_nColorSlotBytes * ( size_t )( m_nCapacity );
	}
	if ( m_iSlotLayout.m_nDerivedSlotBytes > 0u ) {
		pDerivedBank = cursor;
		cursor += m_iSlotLayout.m_nDerivedSlotBytes * ( size_t )( m_nCapacity );
	}
	if ( ( size_t )( cursor - m_mapMemory.m_pAligned ) > m_mapMemory.m_nBytes ) {
		return false;
	}
	for ( int i = 0; i < m_nCapacity; ++i ) {
		t3dSlot_t &slot = m_pSlots[( size_t )( i )];
		slot.m_pHeight = ( uint16_t * )( pHeightBank + ( size_t )( i )*m_iSlotLayout.m_nHeightSlotBytes );
		slot.m_pControl = pControlBank
				? ( uint32_t * )( pControlBank + ( size_t )( i )*m_iSlotLayout.m_nControlSlotBytes )
				: nullptr;
		slot.m_pColor =
				pColorBank ? ( uint32_t * )( pColorBank + ( size_t )( i )*m_iSlotLayout.m_nColorSlotBytes ) : nullptr;
		slot.m_pDerived = pDerivedBank ? pDerivedBank + ( size_t )( i )*m_iSlotLayout.m_nDerivedSlotBytes : nullptr;
	}
	return true;
}

void T3DResidentRegionPool::releaseSlotsUnlocked( void ) {
	if ( !m_pSlots ) {
		return;
	}
	for ( int i = 0; i < m_nCapacity; ++i ) {
		t3dSlot_t &slot = m_pSlots[( size_t )( i )];
		slot.~t3dSlot_s();
	}
	if ( m_bSlotsOwned ) {
		t3dMemoryRelease( m_pSlots );
	}
	m_pSlots = nullptr;
	m_bSlotsOwned = false;
}

void T3DResidentRegionPool::clearUnlocked( void ) {
	releaseSlotsUnlocked();
	m_mapMemory.release();
	for ( std::atomic<int16_t> &mapping : m_szLocationToSlot ) {
		mapping.store( -1, std::memory_order_relaxed );
	}
	m_nRegionSize = 0;
	m_nCapacity = 0;
	m_nChannels = 0u;
	m_nTexelCount = 0u;
	m_nBackingBytes = 0u;
	m_nMetadataBytes = 0u;
	m_iSlotLayout = {};
	m_iClockHand = 0;
}

bool T3DResidentRegionPool::configure( const int nRegionSize, const int nCapacity, const uint32_t nChannels ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( nRegionSize <= 0 || nCapacity <= 0 || nCapacity > s_nResidentWorldSlotCount ||
			( nChannels & RESIDENT_CHANNEL_HEIGHT ) == 0u ) {
		return false;
	}
	if ( m_nRegionSize == nRegionSize && m_nCapacity == nCapacity && m_nChannels == nChannels &&
			m_mapMemory.m_pAligned && m_pSlots ) {
		return true;
	}

	// Reconfiguration would invalidate raw map pointers held by active sampler
	// leases. Keep the current pool intact and let the caller retry after the
	// in-flight sampling operation completes.
	if ( hasPinnedSlotsUnlocked() ) {
		return false;
	}

	clearUnlocked();
	t3dSlotLayout_t layout;
	size_t texel_count = 0u;
	if ( !buildSlotLayout( nRegionSize, nChannels, layout, texel_count ) ||
			( size_t )( nCapacity ) > std::numeric_limits<size_t>::max() / layout.m_nStride ) {
		return false;
	}
	const size_t total_bytes = ( size_t )( nCapacity )*layout.m_nStride;
	if ( !m_mapMemory.allocate( total_bytes, s_nResidentCacheAlignment ) ) {
		return false;
	}

	m_nRegionSize = nRegionSize;
	m_nCapacity = nCapacity;
	m_nChannels = nChannels;
	m_nTexelCount = texel_count;
	m_nBackingBytes = total_bytes;
	m_nMetadataBytes = ( size_t )nCapacity * sizeof( t3dSlot_t );
	m_iSlotLayout = layout;
	if ( !allocateSlotsUnlocked( m_nCapacity ) ) {
		clearUnlocked();
		return false;
	}

	if ( !assignMapBankPointersUnlocked() ) {
		clearUnlocked();
		return false;
	}
	for ( int i = 0; i < m_nCapacity; ++i ) {
		if ( !resetSlotStorage( i, false ) ) {
			clearUnlocked();
			return false;
		}
	}
	for ( std::atomic<int16_t> &mapping : m_szLocationToSlot ) {
		mapping.store( -1, std::memory_order_relaxed );
	}
	return true;
}

bool T3DResidentRegionPool::resetSlotStorage( const int iSlot, const bool bZero ) {
	if ( !m_pSlots || iSlot < 0 || iSlot >= m_nCapacity ) {
		return false;
	}
	t3dSlot_t &slot = m_pSlots[( size_t )( iSlot )];
	if ( bZero ) {
		std::memset( slot.m_pHeight, 0, m_nTexelCount * sizeof( uint16_t ) );
		if ( slot.m_pControl ) {
			std::memset( slot.m_pControl, 0, m_nTexelCount * sizeof( uint32_t ) );
		}
		if ( slot.m_pColor ) {
			std::memset( slot.m_pColor, 0, m_nTexelCount * sizeof( uint32_t ) );
		}
		if ( slot.m_pDerived ) {
			std::memset( slot.m_pDerived, 0, m_nTexelCount * sizeof( uint16_t ) );
		}
	}
	slot.m_nValidChannels.store( 0u, std::memory_order_release );
	return slot.m_pHeight != nullptr;
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
		if ( m_pSlots[( size_t )( i )].m_nState.load( std::memory_order_acquire ) != s_nResidentSlotFree ) {
			retireSlotUnlocked( i );
		}
	}
	for ( std::atomic<int16_t> &mapping : m_szLocationToSlot ) {
		mapping.store( -1, std::memory_order_release );
	}
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
		const uint8_t state = m_pSlots[( size_t )( slot_index )].m_nState.load( std::memory_order_acquire );
		if ( state == s_nResidentSlotFree ||
				( state == s_nResidentSlotRetiring &&
						m_pSlots[( size_t )( slot_index )].m_nPinCount.load( std::memory_order_acquire ) == 0u ) ) {
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
			t3dSlot_t &slot = m_pSlots[( size_t )( slot_index )];
			if ( slot.m_nPinCount.load( std::memory_order_acquire ) != 0u ||
					slot.m_nState.load( std::memory_order_acquire ) != s_nResidentSlotPublished ) {
				continue;
			}
			if ( slot.m_bReferenced.exchange( false, std::memory_order_acq_rel ) ) {
				continue;
			}
			slot.m_nState.store( s_nResidentSlotRetiring, std::memory_order_release );
			const int old_index = locationIndex( slot.m_location );
			if ( old_index >= 0 && m_szLocationToSlot[old_index].load( std::memory_order_acquire ) == slot_index ) {
				m_szLocationToSlot[old_index].store( -1, std::memory_order_release );
			}
			if ( slot.m_nPinCount.load( std::memory_order_acquire ) != 0u ) {
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
	const int slot = m_szLocationToSlot[index].load( std::memory_order_acquire );
	if ( slot < 0 || slot >= m_nCapacity ) {
		return -1;
	}
	const t3dSlot_t &entry = m_pSlots[( size_t )( slot )];
	return entry.m_nState.load( std::memory_order_acquire ) == s_nResidentSlotPublished && entry.m_location == rLocation
			? slot
			: -1;
}

void T3DResidentRegionPool::assignSlot( const int iSlot, const Vector2i &rLocation ) {
	t3dSlot_t &slot = m_pSlots[( size_t )( iSlot )];
	if ( slot.m_nState.load( std::memory_order_acquire ) != s_nResidentSlotFree ) {
		const int old_index = locationIndex( slot.m_location );
		if ( old_index >= 0 && m_szLocationToSlot[old_index].load( std::memory_order_acquire ) == iSlot ) {
			m_szLocationToSlot[old_index].store( -1, std::memory_order_release );
		}
	}
	resetSlotStorage( iSlot, false );
	slot.m_location = rLocation;
	slot.m_bReferenced.store( true, std::memory_order_relaxed );
	slot.m_nPinCount.store( 0u, std::memory_order_relaxed );
	uint32_t generation = slot.m_nGeneration.fetch_add( 1u, std::memory_order_acq_rel ) + 1u;
	if ( generation == 0u ) {
		slot.m_nGeneration.store( 1u, std::memory_order_release );
	}
	slot.m_nState.store( s_nResidentSlotStaging, std::memory_order_release );
}

void T3DResidentRegionPool::publishSlot( const int iSlot ) {
	t3dSlot_t &slot = m_pSlots[( size_t )( iSlot )];
	const int index = locationIndex( slot.m_location );
	if ( index < 0 ) {
		return;
	}
	slot.m_nState.store( s_nResidentSlotPublished, std::memory_order_release );
	m_szLocationToSlot[index].store( ( int16_t )( iSlot ), std::memory_order_release );
}

void T3DResidentRegionPool::retireSlotUnlocked( const int iSlot ) {
	if ( !m_pSlots || iSlot < 0 || iSlot >= m_nCapacity ) {
		return;
	}
	t3dSlot_t &slot = m_pSlots[( size_t )( iSlot )];
	const int old_index = locationIndex( slot.m_location );
	slot.m_nState.store( s_nResidentSlotRetiring, std::memory_order_release );
	if ( old_index >= 0 && m_szLocationToSlot[old_index].load( std::memory_order_acquire ) == iSlot ) {
		m_szLocationToSlot[old_index].store( -1, std::memory_order_release );
	}
	if ( slot.m_nPinCount.load( std::memory_order_acquire ) != 0u ) {
		return;
	}
	slot.m_bReferenced.store( false, std::memory_order_relaxed );
	slot.m_location = Vector2i( INT32_MAX, INT32_MAX );
	resetSlotStorage( iSlot, false );
	uint32_t generation = slot.m_nGeneration.fetch_add( 1u, std::memory_order_acq_rel ) + 1u;
	if ( generation == 0u ) {
		slot.m_nGeneration.store( 1u, std::memory_order_release );
	}
	slot.m_nState.store( s_nResidentSlotFree, std::memory_order_release );
}

void T3DResidentRegionPool::releaseLease( const int iSlot, const uint32_t nGeneration ) {
	if ( !m_pSlots || iSlot < 0 || iSlot >= m_nCapacity ) {
		return;
	}
	t3dSlot_t &slot = m_pSlots[( size_t )( iSlot )];
	if ( slot.m_nGeneration.load( std::memory_order_acquire ) != nGeneration ) {
		return;
	}
	uint32_t pins = slot.m_nPinCount.load( std::memory_order_acquire );
	while ( pins > 0u &&
			!slot.m_nPinCount.compare_exchange_weak(
					pins, pins - 1u, std::memory_order_acq_rel, std::memory_order_acquire ) ) {
	}
}

bool T3DResidentRegionPool::copyMap( const int iSlot, const T3DRegion *pRegion, const t3dMapType_t eType ) {
	return copyMapRect( iSlot, pRegion, eType, Rect2i( s_v2iZero, V2I( m_nRegionSize ) ) );
}

bool T3DResidentRegionPool::copyMapRect( const int iSlot,
		const T3DRegion *pRegion,
		const t3dMapType_t eType,
		const Rect2i &rDirtyRect ) {
	if ( !pRegion || iSlot < 0 || iSlot >= m_nCapacity ) {
		return false;
	}
	const Rect2i bounds( s_v2iZero, V2I( m_nRegionSize ) );
	Rect2i rect = rDirtyRect.size.x > 0 && rDirtyRect.size.y > 0 ? rDirtyRect.intersection( bounds ) : bounds;
	if ( rect.size.x <= 0 || rect.size.y <= 0 ) {
		return true;
	}
	t3dSlot_t &slot = m_pSlots[( size_t )( iSlot )];
	Image *const pImage = pRegion->getMapPtr( eType );
	if ( eType == TYPE_HEIGHT ) {
		if ( !slot.m_pHeight ) {
			return true;
		}
		if ( !pImage || pImage->get_format() != Image::FORMAT_R16 || pImage->get_width() != m_nRegionSize ||
				pImage->get_height() != m_nRegionSize ) {
			return false;
		}
		const PackedByteArray data = pImage->get_data();
		const size_t bytes = m_nTexelCount * sizeof( uint16_t );
		if ( ( size_t )( data.size() ) < bytes ) {
			return false;
		}
		const uint16_t *pSource = ( const uint16_t * )( data.ptr() );
		if ( rect == bounds ) {
			std::memcpy( slot.m_pHeight, pSource, bytes );
		} else {
			for ( int y = rect.position.y; y < rect.get_end().y; ++y ) {
				const size_t offset = ( size_t )( y ) * ( size_t )( m_nRegionSize ) + ( size_t )( rect.position.x );
				std::memcpy(
						slot.m_pHeight + offset, pSource + offset, ( size_t )( rect.size.x ) * sizeof( uint16_t ) );
			}
		}
		slot.m_nValidChannels.fetch_and( ( uint8_t )~RESIDENT_CHANNEL_DERIVED, std::memory_order_release );
		return true;
	}

	if ( eType != TYPE_CONTROL && eType != TYPE_COLOR ) {
		return false;
	}
	const uint32_t channel = eType == TYPE_CONTROL ? RESIDENT_CHANNEL_CONTROL : RESIDENT_CHANNEL_COLOR;
	if ( ( m_nChannels & channel ) == 0u ) {
		return true;
	}
	uint32_t *const plane = eType == TYPE_CONTROL ? slot.m_pControl : slot.m_pColor;
	const uint8_t valid_bit = ( uint8_t )( eType == TYPE_CONTROL ? RESIDENT_CHANNEL_CONTROL : RESIDENT_CHANNEL_COLOR );
	const bool bPlaneValid = ( slot.m_nValidChannels.load( std::memory_order_acquire ) & valid_bit ) != 0u;
	if ( !pImage ) {
		slot.m_nValidChannels.fetch_and( ( uint8_t )~valid_bit, std::memory_order_release );
		return true;
	}
	if ( !plane ) {
		return false;
	}
	const Image::Format expected = eType == TYPE_CONTROL ? Image::FORMAT_RF : Image::FORMAT_RGBA8;
	if ( pImage->get_format() != expected || pImage->get_width() != m_nRegionSize ||
			pImage->get_height() != m_nRegionSize ) {
		return false;
	}
	const PackedByteArray data = pImage->get_data();
	const size_t bytes = m_nTexelCount * sizeof( uint32_t );
	if ( ( size_t )( data.size() ) < bytes ) {
		return false;
	}
	const bool bHadPlane = bPlaneValid;
	const uint32_t *pSource = ( const uint32_t * )( data.ptr() );
	if ( !bHadPlane || rect == bounds ) {
		std::memcpy( plane, pSource, bytes );
	} else {
		for ( int y = rect.position.y; y < rect.get_end().y; ++y ) {
			const size_t offset = ( size_t )( y ) * ( size_t )( m_nRegionSize ) + ( size_t )( rect.position.x );
			std::memcpy( plane + offset, pSource + offset, ( size_t )( rect.size.x ) * sizeof( uint32_t ) );
		}
	}
	slot.m_nValidChannels.fetch_or( valid_bit, std::memory_order_release );
	return true;
}

bool T3DResidentRegionPool::copyDerived( const int iSlot, const T3DRegion *pRegion ) {
	return copyDerivedRect( iSlot, pRegion, Rect2i( s_v2iZero, V2I( m_nRegionSize ) ) );
}

bool T3DResidentRegionPool::copyDerivedRect( const int iSlot, const T3DRegion *pRegion, const Rect2i &rDirtyRect ) {
	if ( !pRegion || iSlot < 0 || iSlot >= m_nCapacity ) {
		return false;
	}
	t3dSlot_t &slot = m_pSlots[( size_t )( iSlot )];
	if ( ( m_nChannels & RESIDENT_CHANNEL_DERIVED ) == 0u ) {
		slot.m_nValidChannels.fetch_and( ( uint8_t )~RESIDENT_CHANNEL_DERIVED, std::memory_order_release );
		return true;
	}
	const Ref<Image> pImage = pRegion->getDerivedNormalSlopeMap();
	const size_t bytes = m_nTexelCount * 2u;
	if ( pImage.is_null() || pImage->get_format() != Image::FORMAT_RG8 || pImage->get_width() != m_nRegionSize ||
			pImage->get_height() != m_nRegionSize ) {
		slot.m_nValidChannels.fetch_and( ( uint8_t )~RESIDENT_CHANNEL_DERIVED, std::memory_order_release );
		return true;
	}
	if ( !slot.m_pDerived ) {
		return false;
	}
	const PackedByteArray data = pImage->get_data();
	if ( ( size_t )( data.size() ) < bytes ) {
		return false;
	}
	const bool bHadPlane = ( slot.m_nValidChannels.load( std::memory_order_acquire ) & RESIDENT_CHANNEL_DERIVED ) != 0u;
	const Rect2i bounds( s_v2iZero, V2I( m_nRegionSize ) );
	const Rect2i rect = rDirtyRect.size.x > 0 && rDirtyRect.size.y > 0 ? rDirtyRect.intersection( bounds ) : bounds;
	if ( rect.size.x <= 0 || rect.size.y <= 0 ) {
		return true;
	}
	if ( !bHadPlane || rect == bounds ) {
		std::memcpy( slot.m_pDerived, data.ptr(), bytes );
	} else {
		for ( int y = rect.position.y; y < rect.get_end().y; ++y ) {
			const size_t offset =
					( ( size_t )( y ) * ( size_t )( m_nRegionSize ) + ( size_t )( rect.position.x ) ) * 2u;
			std::memcpy( slot.m_pDerived + offset, data.ptr() + offset, ( size_t )( rect.size.x ) * 2u );
		}
	}
	slot.m_nValidChannels.fetch_or( ( uint8_t )RESIDENT_CHANNEL_DERIVED, std::memory_order_release );
	return true;
}

t3dResidentMapView_t T3DResidentRegionPool::getView( const int iSlot ) const {
	if ( iSlot < 0 || iSlot >= m_nCapacity ||
			m_pSlots[( size_t )( iSlot )].m_nState.load( std::memory_order_acquire ) != s_nResidentSlotPublished ) {
		return {};
	}
	const t3dSlot_t &slot = m_pSlots[( size_t )( iSlot )];
	const uint8_t valid = slot.m_nValidChannels.load( std::memory_order_acquire );
	return { slot.m_pHeight,
		( valid & RESIDENT_CHANNEL_CONTROL ) ? slot.m_pControl : nullptr,
		( valid & RESIDENT_CHANNEL_COLOR ) ? slot.m_pColor : nullptr,
		( valid & RESIDENT_CHANNEL_DERIVED ) ? slot.m_pDerived : nullptr,
		slot.m_nGeneration.load( std::memory_order_acquire ) };
}

T3DResidentMapLease T3DResidentRegionPool::acquire( const Vector2i &rLocation, const T3DRegion *pRegion ) {
	( void )pRegion;
	const int index = locationIndex( rLocation );
	if ( index < 0 || !m_pSlots ) {
		return {};
	}
	const int existing = m_szLocationToSlot[index].load( std::memory_order_acquire );
	if ( existing < 0 || existing >= m_nCapacity ) {
		return {};
	}
	t3dSlot_t &entry = m_pSlots[( size_t )( existing )];
	const uint32_t generation = entry.m_nGeneration.load( std::memory_order_acquire );
	uint32_t pins = entry.m_nPinCount.load( std::memory_order_acquire );
	do {
		if ( pins == std::numeric_limits<uint32_t>::max() ||
				entry.m_nState.load( std::memory_order_acquire ) != s_nResidentSlotPublished ) {
			return {};
		}
	} while ( !entry.m_nPinCount.compare_exchange_weak(
			pins, pins + 1u, std::memory_order_acq_rel, std::memory_order_acquire ) );
	if ( m_szLocationToSlot[index].load( std::memory_order_acquire ) != existing ||
			entry.m_nGeneration.load( std::memory_order_acquire ) != generation ||
			entry.m_nState.load( std::memory_order_acquire ) != s_nResidentSlotPublished ) {
		entry.m_nPinCount.fetch_sub( 1u, std::memory_order_release );
		return {};
	}
	entry.m_bReferenced.store( true, std::memory_order_relaxed );
	const t3dResidentMapView_t view = getView( existing );
	if ( !view.valid() ) {
		entry.m_nPinCount.fetch_sub( 1u, std::memory_order_release );
		return {};
	}
	return T3DResidentMapLease( this, existing, generation, view );
}

bool T3DResidentRegionPool::ensureResident( const Vector2i &rLocation, const T3DRegion *pRegion ) {
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( findSlotUnlocked( rLocation ) >= 0 ) {
		return true;
	}
	const int slot = chooseReplacementSlotUnlocked();
	if ( slot < 0 ) {
		return false;
	}
	assignSlot( slot, rLocation );
	if ( !copyMap( slot, pRegion, TYPE_HEIGHT ) || !copyMap( slot, pRegion, TYPE_CONTROL ) ||
			!copyMap( slot, pRegion, TYPE_COLOR ) ) {
		retireSlotUnlocked( slot );
		return false;
	}
	copyDerived( slot, pRegion );
	publishSlot( slot );
	return true;
}

void T3DResidentRegionPool::syncMap( const Vector2i &rLocation,
		const T3DRegion *pRegion,
		const t3dMapType_t eType,
		const Rect2i &rDirtyRect ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	const int slot = findSlotUnlocked( rLocation );
	if ( slot >= 0 ) {
		t3dSlot_t &entry = m_pSlots[( size_t )( slot )];
		entry.m_nState.store( s_nResidentSlotRetiring, std::memory_order_release );
		const int map_index = locationIndex( rLocation );
		if ( map_index >= 0 ) {
			m_szLocationToSlot[map_index].store( -1, std::memory_order_release );
		}
		if ( entry.m_nPinCount.load( std::memory_order_acquire ) != 0u ) {
			return;
		}
		entry.m_nState.store( s_nResidentSlotStaging, std::memory_order_release );
		if ( !copyMapRect( slot, pRegion, eType, rDirtyRect ) ) {
			retireSlotUnlocked( slot );
		} else {
			publishSlot( slot );
		}
	}
}

void T3DResidentRegionPool::syncDerived( const Vector2i &rLocation,
		const T3DRegion *pRegion,
		const Rect2i &rDirtyRect ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	const int slot = findSlotUnlocked( rLocation );
	if ( slot >= 0 ) {
		t3dSlot_t &entry = m_pSlots[( size_t )( slot )];
		entry.m_nState.store( s_nResidentSlotRetiring, std::memory_order_release );
		const int map_index = locationIndex( rLocation );
		if ( map_index >= 0 ) {
			m_szLocationToSlot[map_index].store( -1, std::memory_order_release );
		}
		if ( entry.m_nPinCount.load( std::memory_order_acquire ) != 0u ) {
			return;
		}
		entry.m_nState.store( s_nResidentSlotStaging, std::memory_order_release );
		if ( !copyDerivedRect( slot, pRegion, rDirtyRect ) ) {
			retireSlotUnlocked( slot );
		} else {
			publishSlot( slot );
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
