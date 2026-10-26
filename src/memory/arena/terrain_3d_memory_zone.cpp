// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "core/utility/terrain_3d_object_bytes.h"
#include "memory/arena/terrain_3d_memory_arena.h"
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

#include "memory/arena/terrain_3d_memory_zone.h"
#include "core/utility/terrain_3d_sync.h"

#include "memory/core/terrain_3d_memory.h"
#include "core/utility/terrain_3d_checked_math.h"

#include <EASTL/numeric_limits.h>
#include <stdlib.h>
#include <string.h>
EA_FORCE_INLINE static void t3dMemoryZoneResetSmallPage(
	t3dMemoryZoneSmallPage_t &rPage ) {
	t3dClearTrivialObjectBytes( rPage );
	rPage.m_iFirstFree = UINT32_MAX;
	rPage.m_iClass = UINT16_MAX;
}

EA_FORCE_INLINE static void t3dMemoryZoneResetAllocationHeader(
	t3dMemoryZoneAllocationHeader_t &rHeader ) {
	t3dClearTrivialObjectBytes( rHeader );
	rHeader.m_iPage = UINT16_MAX;
	rHeader.m_iClass = UINT16_MAX;
}

Terrain3DMemoryZone::~Terrain3DMemoryZone() {
	if ( !release() ) {
		abort();
	}
}

bool Terrain3DMemoryZone::initialize(
	void *const	 pBacking,
	const size_t nBytes ) {
	Terrain3DScopedLock lock( m_mutex );
	if ( !pBacking || nBytes < T3D_SMALL_PAGE_BYTES || m_state == T3D_MEMORY_ZONE_ACTIVE ||
		 m_state == T3D_MEMORY_ZONE_DRAINING ) {
		return false;
	}
	m_pBase = ( uint8_t * ) ( pBacking );
	m_nCapacity = nBytes;
	m_nFreeExtentCount = 1u;
	m_szFreeExtents[0] = { 0u, nBytes };
	for ( size_t iPage = 0u; iPage < T3D_ARRAY_COUNT( m_szSmallPages ); iPage++ ) {
		t3dMemoryZoneSmallPage_t &page = ( m_szSmallPages )[iPage];
		t3dMemoryZoneResetSmallPage( page );
	}
	m_nLiveAllocations = 0u;
	m_nLiveBytes = 0u;
	m_nLivePhysicalBytes = 0u;
	m_nLiveMetadataBytes = 0u;
	m_nLiveAlignmentWasteBytes = 0u;
	m_nLiveSizeClassWasteBytes = 0u;
	m_nFailedAllocations = 0u;
#if defined( TERRAIN3D_STORAGE_FAULT_TEST )
	m_nTestAllocationCallCount = 0u;
	m_nTestFailAllocationAt = 0u;
#endif
	m_nExtentTableSaturationEvents = 0u;
	m_nLostFreePreventionEvents = 0u;
	m_nActiveSmallPages = 0u;
	m_nPeakActiveSmallPages = 0u;
	m_nPeakFreeExtentCount = 1u;
	m_state = T3D_MEMORY_ZONE_ACTIVE;
	return true;
}

int Terrain3DMemoryZone::classIndexFor(
	const size_t nBytes,
	const size_t nAlignment ) const {
	if ( nAlignment > 16u ) {
		return -1;
	}
	for ( uint32_t i = 0u; i < T3D_SMALL_CLASS_COUNT; ++i ) {
		if ( nBytes <= s_szSmallClasses[i] ) {
			return ( int ) i;
		}
	}
	return -1;
}

bool Terrain3DMemoryZone::insertFreeExtentUnlocked(
	const size_t nOffset,
	const size_t nBytes ) {
	if ( nBytes == 0u ) {
		return true;
	}
	if ( nOffset > m_nCapacity || nBytes > m_nCapacity - nOffset ) {
		return false;
	}

	const size_t extent_end = nOffset + nBytes;
	uint32_t	 index = 0u;
	while ( index < m_nFreeExtentCount && m_szFreeExtents[index].m_iOffset < nOffset ) {
		++index;
	}

	bool merge_previous = false;
	bool merge_next = false;
	if ( index > 0u ) {
		const t3dMemoryZoneExtent_t &previous = m_szFreeExtents[index - 1u];
		const size_t				 previous_end = previous.m_iOffset + previous.m_nSize;
		if ( previous_end > nOffset ) {
			return false;
		}
		merge_previous = previous_end == nOffset;
	}
	if ( index < m_nFreeExtentCount ) {
		const t3dMemoryZoneExtent_t &next = m_szFreeExtents[index];
		if ( extent_end > next.m_iOffset ) {
			return false;
		}
		merge_next = extent_end == next.m_iOffset;
	}

	if ( merge_previous ) {
		t3dMemoryZoneExtent_t &previous = m_szFreeExtents[index - 1u];
		previous.m_nSize += nBytes;
		if ( merge_next ) {
			previous.m_nSize += m_szFreeExtents[index].m_nSize;
			if ( index + 1u < m_nFreeExtentCount ) {
				memmove( &m_szFreeExtents[index],
						 &m_szFreeExtents[index + 1u],
						 ( size_t ) ( m_nFreeExtentCount - index - 1u ) * sizeof( t3dMemoryZoneExtent_t ) );
			}
			--m_nFreeExtentCount;
		}
		return true;
	}
	if ( merge_next ) {
		t3dMemoryZoneExtent_t &next = m_szFreeExtents[index];
		next.m_iOffset = nOffset;
		next.m_nSize += nBytes;
		return true;
	}

	if ( m_nFreeExtentCount >= T3D_MAX_EXTENTS ) {
		++m_nExtentTableSaturationEvents;
		return false;
	}
	if ( index < m_nFreeExtentCount ) {
		memmove( &m_szFreeExtents[index + 1u],
				 &m_szFreeExtents[index],
				 ( size_t ) ( m_nFreeExtentCount - index ) * sizeof( t3dMemoryZoneExtent_t ) );
	}
	m_szFreeExtents[index] = { nOffset, nBytes };
	++m_nFreeExtentCount;
	m_nPeakFreeExtentCount = MAX( m_nPeakFreeExtentCount, m_nFreeExtentCount );
	return true;
}

bool Terrain3DMemoryZone::canTakeExtentUnlocked(
	const size_t nBytes,
	const size_t nAlignment,
	const size_t nPrefixBytes ) const {
	for ( uint32_t index = 0u; index < m_nFreeExtentCount; ++index ) {
		const t3dMemoryZoneExtent_t source = m_szFreeExtents[index];
		const uintptr_t				base = ( uintptr_t ) ( m_pBase );
		const uintptr_t				address_max = eastl::numeric_limits< uintptr_t >::max();
		if ( base > address_max - source.m_iOffset ) {
			continue;
		}
		const uintptr_t source_address = base + source.m_iOffset;
		if ( source_address > address_max - nPrefixBytes ) {
			continue;
		}
		size_t aligned_address = 0u;
		if ( !t3dCheckedAlignUp< size_t >( source_address + nPrefixBytes, nAlignment, aligned_address ) ||
			 aligned_address < base ) {
			continue;
		}
		const size_t user = aligned_address - base;
		const size_t start = user - nPrefixBytes;
		if ( start < source.m_iOffset || nBytes > source.m_nSize - ( start - source.m_iOffset ) ||
			 user > eastl::numeric_limits< size_t >::max() - nBytes ) {
			continue;
		}
		const size_t end = user + nBytes;
		if ( end > source.m_iOffset + source.m_nSize ) {
			continue;
		}
		const uint32_t required_extent_slots =
			( start > source.m_iOffset ? 1u : 0u ) + ( end < source.m_iOffset + source.m_nSize ? 1u : 0u );
		if ( m_nFreeExtentCount - 1u + required_extent_slots <= T3D_MAX_EXTENTS ) {
			return true;
		}
	}
	return false;
}

bool Terrain3DMemoryZone::takeExtentUnlocked(
	const size_t nBytes,
	const size_t nAlignment,
	const size_t nPrefixBytes,
	size_t		&rExtentOffset,
	size_t		&rUserOffset ) {
	for ( uint32_t index = 0u; index < m_nFreeExtentCount; ++index ) {
		const t3dMemoryZoneExtent_t source = m_szFreeExtents[index];
		const uintptr_t				base = ( uintptr_t ) ( m_pBase );
		const uintptr_t				address_max = eastl::numeric_limits< uintptr_t >::max();
		if ( base > address_max - source.m_iOffset ) {
			continue;
		}
		const uintptr_t source_address = base + source.m_iOffset;
		if ( source_address > address_max - nPrefixBytes ) {
			continue;
		}
		size_t aligned_address = 0u;
		if ( !t3dCheckedAlignUp< size_t >( source_address + nPrefixBytes, nAlignment, aligned_address ) ||
			 aligned_address < base ) {
			continue;
		}
		const size_t user = aligned_address - base;
		const size_t start = user - nPrefixBytes;
		if ( start < source.m_iOffset || nBytes > source.m_nSize - ( start - source.m_iOffset ) ||
			 user > eastl::numeric_limits< size_t >::max() - nBytes ) {
			continue;
		}
		const size_t end = user + nBytes;
		if ( end > source.m_iOffset + source.m_nSize ) {
			continue;
		}
		const uint32_t required_extent_slots =
			( start > source.m_iOffset ? 1u : 0u ) + ( end < source.m_iOffset + source.m_nSize ? 1u : 0u );
		if ( m_nFreeExtentCount - 1u + required_extent_slots > T3D_MAX_EXTENTS ) {
			continue;
		}
		const bool has_prefix = start > source.m_iOffset;
		const bool has_suffix = end < source.m_iOffset + source.m_nSize;
		if ( has_prefix && has_suffix ) {
			if ( index + 1u < m_nFreeExtentCount ) {
				memmove( &m_szFreeExtents[index + 2u],
						 &m_szFreeExtents[index + 1u],
						 ( size_t ) ( m_nFreeExtentCount - index - 1u ) * sizeof( t3dMemoryZoneExtent_t ) );
			}
			m_szFreeExtents[index] = { source.m_iOffset, start - source.m_iOffset };
			m_szFreeExtents[index + 1u] = { end, source.m_iOffset + source.m_nSize - end };
			++m_nFreeExtentCount;
			m_nPeakFreeExtentCount = MAX( m_nPeakFreeExtentCount, m_nFreeExtentCount );
		} else if ( has_prefix ) {
			m_szFreeExtents[index] = { source.m_iOffset, start - source.m_iOffset };
		} else if ( has_suffix ) {
			m_szFreeExtents[index] = { end, source.m_iOffset + source.m_nSize - end };
		} else {
			if ( index + 1u < m_nFreeExtentCount ) {
				memmove( &m_szFreeExtents[index],
						 &m_szFreeExtents[index + 1u],
						 ( size_t ) ( m_nFreeExtentCount - index - 1u ) * sizeof( t3dMemoryZoneExtent_t ) );
			}
			--m_nFreeExtentCount;
		}
		rExtentOffset = start;
		rUserOffset = user;
		return true;
	}
	return false;
}

void *Terrain3DMemoryZone::allocateSmallUnlocked(
	const int	 nClassIndex,
	const size_t nBytes ) {
	uint32_t page_index = T3D_MAX_SMALL_PAGES;
	for ( uint32_t i = 0u; i < T3D_MAX_SMALL_PAGES; ++i ) {
		if ( m_szSmallPages[i].m_bActive && m_szSmallPages[i].m_iClass == nClassIndex &&
			 m_szSmallPages[i].m_iFirstFree != UINT32_MAX ) {
			page_index = i;
			break;
		}
	}
	if ( page_index == T3D_MAX_SMALL_PAGES ) {
		for ( uint32_t i = 0u; i < T3D_MAX_SMALL_PAGES; ++i ) {
			if ( !m_szSmallPages[i].m_bActive ) {
				page_index = i;
				break;
			}
		}
		if ( page_index == T3D_MAX_SMALL_PAGES ) {
			return NULL;
		}
		size_t page_offset = 0u, ignored = 0u;
		if ( !takeExtentUnlocked( T3D_SMALL_PAGE_BYTES, 64u, 0u, page_offset, ignored ) ) {
			return NULL;
		}
		t3dMemoryZoneSmallPage_t &page = m_szSmallPages[page_index];
		t3dMemoryZoneResetSmallPage( page );
		page.m_bActive = true;
		page.m_iOffset = page_offset;
		page.m_nBytes = T3D_SMALL_PAGE_BYTES;
		page.m_iClass = ( uint16_t ) nClassIndex;
		size_t stride = 0u;
		t3dCheckedAlignUp< size_t >( sizeof( t3dMemoryZoneAllocationHeader_t ) + s_szSmallClasses[nClassIndex],
									 16u,
									 stride );
		page.m_nBlockStride = ( uint32_t ) stride;
		page.m_nTotalBlocks = ( uint16_t ) ( T3D_SMALL_PAGE_BYTES / stride );
		page.m_iFirstFree = 0u;
		++m_nActiveSmallPages;
		m_nPeakActiveSmallPages = MAX( m_nPeakActiveSmallPages, m_nActiveSmallPages );
		for ( uint32_t block = 0u; block < page.m_nTotalBlocks; ++block ) {
			uint8_t *payload =
				m_pBase + page.m_iOffset + ( size_t ) block * stride + sizeof( t3dMemoryZoneAllocationHeader_t );
			*( uint32_t * ) ( payload ) = block + 1u < page.m_nTotalBlocks ? block + 1u : UINT32_MAX;
		}
	}
	t3dMemoryZoneSmallPage_t &page = m_szSmallPages[page_index];
	const uint32_t			  block = page.m_iFirstFree;
	uint8_t *const			  block_start = m_pBase + page.m_iOffset + ( size_t ) block * page.m_nBlockStride;
	uint8_t *const			  payload = block_start + sizeof( t3dMemoryZoneAllocationHeader_t );
	page.m_iFirstFree = *( uint32_t * ) ( payload );
	++page.m_nLiveBlocks;
	t3dMemoryZoneAllocationHeader_t *const header = ( t3dMemoryZoneAllocationHeader_t * ) ( block_start );
	t3dMemoryZoneResetAllocationHeader( *header );
	header->m_nMagic = T3D_ALLOCATION_MAGIC;
	header->m_iExtentOffset = page.m_iOffset + ( size_t ) block * page.m_nBlockStride;
	header->m_nExtentBytes = page.m_nBlockStride;
	header->m_nRequestedBytes = nBytes;
	header->m_nAlignment = 16u;
	header->m_iPage = ( uint16_t ) page_index;
	header->m_iClass = ( uint16_t ) nClassIndex;
	++m_nLiveAllocations;
	m_nLiveBytes += nBytes;
	page.m_nLiveRequestedBytes += nBytes;
	m_nLivePhysicalBytes += page.m_nBlockStride;
	m_nLiveMetadataBytes += sizeof( t3dMemoryZoneAllocationHeader_t );
	m_nLiveSizeClassWasteBytes += s_szSmallClasses[nClassIndex] - nBytes;
	m_nLiveAlignmentWasteBytes +=
		page.m_nBlockStride - sizeof( t3dMemoryZoneAllocationHeader_t ) - s_szSmallClasses[nClassIndex];
	return payload;
}

void *Terrain3DMemoryZone::allocateLargeUnlocked(
	const size_t nBytes,
	const size_t nAlignment ) {
	if ( nBytes >
		 eastl::numeric_limits< size_t >::max() - sizeof( t3dMemoryZoneAllocationHeader_t ) - ( nAlignment - 1u ) ) {
		return NULL;
	}
	size_t extent_offset = 0u, user_offset = 0u;
	if ( !takeExtentUnlocked( nBytes,
							  nAlignment,
							  sizeof( t3dMemoryZoneAllocationHeader_t ),
							  extent_offset,
							  user_offset ) ) {
		return NULL;
	}
	const size_t						   used = user_offset + nBytes - extent_offset;
	t3dMemoryZoneAllocationHeader_t *const header =
		( t3dMemoryZoneAllocationHeader_t * ) ( m_pBase + user_offset - sizeof( t3dMemoryZoneAllocationHeader_t ) );
	t3dMemoryZoneResetAllocationHeader( *header );
	header->m_nMagic = T3D_ALLOCATION_MAGIC;
	header->m_iExtentOffset = extent_offset;
	header->m_nExtentBytes = used;
	header->m_nRequestedBytes = nBytes;
	header->m_nAlignment = ( uint32_t ) nAlignment;
	++m_nLiveAllocations;
	m_nLiveBytes += nBytes;
	m_nLivePhysicalBytes += used;
	m_nLiveMetadataBytes += sizeof( t3dMemoryZoneAllocationHeader_t );
	m_nLiveAlignmentWasteBytes += used - sizeof( t3dMemoryZoneAllocationHeader_t ) - nBytes;
	return m_pBase + user_offset;
}

void *Terrain3DMemoryZone::allocateUnlocked(
	const size_t nBytes,
	const size_t nAlignment ) {
	if ( m_state != T3D_MEMORY_ZONE_ACTIVE || nBytes == 0u || !t3dIsPowerOfTwo< size_t >( nAlignment ) ||
		 nAlignment > UINT32_MAX ) {
		return NULL;
	}
	const int class_index = classIndexFor( nBytes, nAlignment );
	return class_index >= 0 ? allocateSmallUnlocked( class_index, nBytes )
							: allocateLargeUnlocked( nBytes, nAlignment );
}

bool Terrain3DMemoryZone::canAllocate(
	const size_t nBytes,
	const size_t nAlignment ) const {
	if ( nBytes == 0u ) {
		return false;
	}
	Terrain3DScopedLock lock( m_mutex );
	const size_t		alignment = MAX( nAlignment, ( size_t ) alignof( void * ) );
	if ( m_state != T3D_MEMORY_ZONE_ACTIVE || !t3dIsPowerOfTwo< size_t >( alignment ) || alignment > UINT32_MAX ) {
		return false;
	}
#if defined( TERRAIN3D_STORAGE_FAULT_TEST )
	if ( m_nTestFailAllocationAt != 0u && m_nTestAllocationCallCount + 1u == m_nTestFailAllocationAt ) {
		return false;
	}
#endif
	const int class_index = classIndexFor( nBytes, alignment );
	if ( class_index >= 0 ) {
		for ( uint32_t i = 0u; i < T3D_MAX_SMALL_PAGES; ++i ) {
			if ( m_szSmallPages[i].m_bActive && m_szSmallPages[i].m_iClass == class_index &&
				 m_szSmallPages[i].m_iFirstFree != UINT32_MAX ) {
				return true;
			}
		}
		bool has_inactive_page = false;
		for ( uint32_t i = 0u; i < T3D_MAX_SMALL_PAGES; ++i ) {
			if ( !m_szSmallPages[i].m_bActive ) {
				has_inactive_page = true;
				break;
			}
		}
		return has_inactive_page && canTakeExtentUnlocked( T3D_SMALL_PAGE_BYTES, 64u, 0u );
	}
	if ( nBytes >
		 eastl::numeric_limits< size_t >::max() - sizeof( t3dMemoryZoneAllocationHeader_t ) - ( alignment - 1u ) ) {
		return false;
	}
	return canTakeExtentUnlocked( nBytes, alignment, sizeof( t3dMemoryZoneAllocationHeader_t ) );
}

void *Terrain3DMemoryZone::allocate(
	const size_t nBytes,
	const size_t nAlignment ) {
	if ( nBytes == 0u ) {
		return NULL;
	}
	Terrain3DScopedLock lock( m_mutex );
#if defined( TERRAIN3D_STORAGE_FAULT_TEST )
	++m_nTestAllocationCallCount;
	if ( m_nTestFailAllocationAt != 0u && m_nTestAllocationCallCount == m_nTestFailAllocationAt ) {
		++m_nFailedAllocations;
		return NULL;
	}
#endif
	void *const pMemory = allocateUnlocked( nBytes, MAX( nAlignment, ( size_t ) alignof( void * ) ) );
	if ( !pMemory ) {
		++m_nFailedAllocations;
		return NULL;
	}
	t3dRecordRuntimeZoneAllocation( nBytes );
	return pMemory;
}

#if defined( TERRAIN3D_STORAGE_FAULT_TEST )
void Terrain3DMemoryZone::testFailAllocationAt(
	const size_t nCall ) {
	Terrain3DScopedLock lock( m_mutex );
	m_nTestAllocationCallCount = 0u;
	m_nTestFailAllocationAt = nCall;
}

void Terrain3DMemoryZone::testClearAllocationFailure(
	void ) {
	Terrain3DScopedLock lock( m_mutex );
	m_nTestAllocationCallCount = 0u;
	m_nTestFailAllocationAt = 0u;
}

size_t Terrain3DMemoryZone::testAllocationCallCount(
	void ) const {
	Terrain3DScopedLock lock( m_mutex );
	return m_nTestAllocationCallCount;
}
#endif

t3dMemoryZoneAllocationHeader_t *Terrain3DMemoryZone::headerFor(
	void *const pMemory ) const {
	return owns( pMemory ) ? ( t3dMemoryZoneAllocationHeader_t * ) ( ( uint8_t * ) ( pMemory ) -
																	 sizeof( t3dMemoryZoneAllocationHeader_t ) )
						   : NULL;
}
const t3dMemoryZoneAllocationHeader_t *Terrain3DMemoryZone::headerFor(
	const void *const pMemory ) const {
	return owns( pMemory ) ? ( const t3dMemoryZoneAllocationHeader_t * ) ( ( const uint8_t * ) ( pMemory ) -
																		   sizeof( t3dMemoryZoneAllocationHeader_t ) )
						   : NULL;
}

bool Terrain3DMemoryZone::freeUnlocked(
	void *const pMemory ) {
	t3dMemoryZoneAllocationHeader_t *const header = headerFor( pMemory );
	if ( !header || header->m_nMagic != T3D_ALLOCATION_MAGIC ) {
		return false;
	}
	const size_t   requested = header->m_nRequestedBytes;
	const size_t   physical = header->m_nExtentBytes;
	const size_t   extent_offset = header->m_iExtentOffset;
	const uint16_t page_index = header->m_iPage;
	const uint16_t class_index = header->m_iClass;
	if ( requested == 0u || physical < sizeof( t3dMemoryZoneAllocationHeader_t ) || extent_offset > m_nCapacity ||
		 physical > m_nCapacity - extent_offset || requested > physical - sizeof( t3dMemoryZoneAllocationHeader_t ) ||
		 m_nLiveAllocations == 0u || m_nLiveBytes < requested || m_nLivePhysicalBytes < physical ||
		 m_nLiveMetadataBytes < sizeof( t3dMemoryZoneAllocationHeader_t ) ) {
		return false;
	}
	if ( page_index != UINT16_MAX ) {
		if ( page_index >= T3D_MAX_SMALL_PAGES || class_index >= T3D_SMALL_CLASS_COUNT ) {
			return false;
		}
		t3dMemoryZoneSmallPage_t &page = m_szSmallPages[page_index];
		if ( !page.m_bActive || page.m_iClass != class_index || page.m_nBlockStride == 0u ||
			 extent_offset < page.m_iOffset || extent_offset >= page.m_iOffset + page.m_nBytes ||
			 ( extent_offset - page.m_iOffset ) % page.m_nBlockStride != 0u ) {
			return false;
		}
		const uint32_t block = ( uint32_t ) ( ( extent_offset - page.m_iOffset ) / page.m_nBlockStride );
		if ( block >= page.m_nTotalBlocks || requested > s_szSmallClasses[class_index] || page.m_nLiveBlocks == 0u ||
			 page.m_nLiveRequestedBytes < requested ||
			 page.m_nBlockStride < sizeof( t3dMemoryZoneAllocationHeader_t ) + s_szSmallClasses[class_index] ) {
			return false;
		}
		const size_t size_class_waste = s_szSmallClasses[class_index] - requested;
		const size_t alignment_waste =
			page.m_nBlockStride - sizeof( t3dMemoryZoneAllocationHeader_t ) - s_szSmallClasses[class_index];
		if ( m_nLiveSizeClassWasteBytes < size_class_waste || m_nLiveAlignmentWasteBytes < alignment_waste ) {
			return false;
		}
		if ( page.m_nLiveBlocks == 1u && !insertFreeExtentUnlocked( page.m_iOffset, page.m_nBytes ) ) {
			++m_nLostFreePreventionEvents;
			return false;
		}

		header->m_nMagic = 0u;
		*( uint32_t * ) ( pMemory ) = page.m_iFirstFree;
		page.m_iFirstFree = block;
		--page.m_nLiveBlocks;
		page.m_nLiveRequestedBytes -= requested;
		m_nLiveSizeClassWasteBytes -= size_class_waste;
		m_nLiveAlignmentWasteBytes -= alignment_waste;
		if ( page.m_nLiveBlocks == 0u ) {
			t3dMemoryZoneResetSmallPage( page );
			if ( m_nActiveSmallPages > 0u ) {
				--m_nActiveSmallPages;
			}
		}
	} else {
		if ( class_index != UINT16_MAX ) {
			return false;
		}
		const size_t alignment_waste = physical - sizeof( t3dMemoryZoneAllocationHeader_t ) - requested;
		if ( m_nLiveAlignmentWasteBytes < alignment_waste ) {
			return false;
		}
		if ( !insertFreeExtentUnlocked( extent_offset, physical ) ) {
			++m_nLostFreePreventionEvents;
			return false;
		}
		header->m_nMagic = 0u;
		m_nLiveAlignmentWasteBytes -= alignment_waste;
	}
	--m_nLiveAllocations;
	m_nLiveBytes -= requested;
	m_nLivePhysicalBytes -= physical;
	m_nLiveMetadataBytes -= sizeof( t3dMemoryZoneAllocationHeader_t );
	return true;
}

void Terrain3DMemoryZone::deallocate(
	void *const pMemory ) {
	if ( !pMemory ) {
		return;
	}
	Terrain3DScopedLock lock( m_mutex );
	freeUnlocked( pMemory );
}

bool Terrain3DMemoryZone::release() {
	Terrain3DScopedLock lock( m_mutex );
	if ( m_state == T3D_MEMORY_ZONE_UNINITIALIZED || m_state == T3D_MEMORY_ZONE_DEAD ) {
		return true;
	}
	if ( m_nLiveAllocations != 0u ) {
		m_state = T3D_MEMORY_ZONE_DRAINING;
		return false;
	}
	m_pBase = NULL;
	m_nCapacity = 0u;
	m_nFreeExtentCount = 0u;
	m_state = T3D_MEMORY_ZONE_DEAD;
	return true;
}

size_t Terrain3DMemoryZone::freeExtentBytes(
	void ) const {
	Terrain3DScopedLock lock( m_mutex );
	size_t				bytes = 0u;
	for ( uint32_t i = 0u; i < m_nFreeExtentCount; ++i ) {
		bytes += m_szFreeExtents[i].m_nSize;
	}
	return bytes;
}

t3dMemoryZoneStats_t Terrain3DMemoryZone::getStats(
	void ) const {
	Terrain3DScopedLock	 lock( m_mutex );
	t3dMemoryZoneStats_t stats;
	stats.m_nReservedBytes = m_nCapacity;
	stats.m_nLiveRequestedBytes = m_nLiveBytes;
	stats.m_nLivePhysicalBytes = m_nLivePhysicalBytes;
	stats.m_nLiveAllocationCount = m_nLiveAllocations;
	stats.m_nMetadataBytes = m_nLiveMetadataBytes;
	stats.m_nAlignmentWasteBytes = m_nLiveAlignmentWasteBytes;
	stats.m_nSizeClassWasteBytes = m_nLiveSizeClassWasteBytes;
	stats.m_nInternalFragmentationBytes =
		m_nLivePhysicalBytes >= m_nLiveBytes ? m_nLivePhysicalBytes - m_nLiveBytes : 0u;
	stats.m_nFailedAllocations = m_nFailedAllocations;
	stats.m_nExtentTableSaturationEvents = m_nExtentTableSaturationEvents;
	stats.m_nLostFreePreventionEvents = m_nLostFreePreventionEvents;
	stats.m_nActiveSmallPages = m_nActiveSmallPages;
	stats.m_nPeakActiveSmallPages = m_nPeakActiveSmallPages;
	stats.m_nFreeExtentCount = m_nFreeExtentCount;
	stats.m_nPeakFreeExtentCount = m_nPeakFreeExtentCount;
	for ( uint32_t i = 0u; i < m_nFreeExtentCount; ++i ) {
		stats.m_nExtentFreeBytes += m_szFreeExtents[i].m_nSize;
	}
	size_t small_live_physical = 0u;
	for ( uint32_t i = 0u; i < T3D_MAX_SMALL_PAGES; ++i ) {
		const t3dMemoryZoneSmallPage_t &page = m_szSmallPages[i];
		if ( !page.m_bActive ) {
			continue;
		}
		stats.m_nSmallPageReservedBytes += page.m_nBytes;
		const size_t free_blocks =
			page.m_nTotalBlocks >= page.m_nLiveBlocks ? ( size_t ) page.m_nTotalBlocks - page.m_nLiveBlocks : 0u;
		stats.m_nSmallPageReusablePayloadBytes += free_blocks * s_szSmallClasses[page.m_iClass];
		stats.m_nSmallPageReusablePhysicalBytes += free_blocks * page.m_nBlockStride;
		small_live_physical += ( size_t ) page.m_nLiveBlocks * page.m_nBlockStride;
	}
	stats.m_nLargeAllocationPhysicalBytes =
		m_nLivePhysicalBytes >= small_live_physical ? m_nLivePhysicalBytes - small_live_physical : 0u;
	return stats;
}

// -------- Consolidated from memory/arena/terrain_3d_memory_arena.cpp --------
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
Terrain3DMemoryArenaLayout::Terrain3DMemoryArenaLayout(
	const size_t nCapacityBytes ) {
	configure( nCapacityBytes );
}

bool Terrain3DMemoryArenaLayout::configure(
	const size_t nCapacityBytes ) {
	m_nCapacity = nCapacityBytes;
	m_nLowCursor = 0u;
	m_nMaxAlignment = T3D_ARENA_DEFAULT_BASE_ALIGNMENT;
	m_bValid = true;
	m_bSealed = false;
	return true;
}

bool Terrain3DMemoryArenaLayout::reserveLow(
	const size_t		   nBytes,
	const size_t		   nAlignment,
	t3dMemoryArenaSlice_t &rSlice ) {
	if ( m_bSealed || !m_bValid ) {
		return false;
	}
	if ( !t3dIsPowerOfTwo< size_t >( nAlignment ) ) {
		m_bValid = false;
		return false;
	}
	m_nMaxAlignment = MAX( m_nMaxAlignment, nAlignment );

	size_t aligned = 0u;
	if ( !t3dCheckedAlignUp< size_t >( m_nLowCursor, nAlignment, aligned ) || aligned > m_nCapacity ||
		 nBytes > m_nCapacity - aligned ) {
		m_bValid = false;
		return false;
	}
	rSlice = { aligned, nBytes, nAlignment, true };
	m_nLowCursor = aligned + nBytes;
	return true;
}

bool Terrain3DMemoryArenaLayout::seal(
	void ) {
	if ( !m_bValid ) {
		return false;
	}
	m_bSealed = true;
	return true;
}

Terrain3DMemoryArena::Terrain3DMemoryArena(
	Terrain3DMemoryArena &&rOther ) noexcept {
	swap( rOther );
}

Terrain3DMemoryArena &Terrain3DMemoryArena::operator=(
	Terrain3DMemoryArena &&rOther ) noexcept {
	if ( this != &rOther ) {
		release();
		swap( rOther );
	}
	return *this;
}

void Terrain3DMemoryArena::swap(
	Terrain3DMemoryArena &rOther ) noexcept {
	m_backing.swap( rOther.m_backing );
	eastl::swap( m_nBaseAlignment, rOther.m_nBaseAlignment );
	eastl::swap( m_bInitialized, rOther.m_bInitialized );
	eastl::swap( m_bCommitted, rOther.m_bCommitted );
	eastl::swap( m_bSealed, rOther.m_bSealed );
}

bool Terrain3DMemoryArena::initialize(
	const Terrain3DMemoryArenaLayout &rLayout ) {
	release();
	if ( !rLayout.valid() || !rLayout.sealed() ) {
		return false;
	}
	m_nBaseAlignment = MAX( rLayout.maxAlignment(), T3D_ARENA_DEFAULT_BASE_ALIGNMENT );
	if ( !m_backing.allocate( rLayout.capacity(), m_nBaseAlignment ) ) {
		release();
		return false;
	}
	m_bInitialized = true;
	return true;
}

bool Terrain3DMemoryArena::commit(
	void ) {
	if ( !initialized() || m_bSealed ) {
		return false;
	}
	if ( m_backing.capacity() > 0u ) {
		memset( m_backing.data(), 0, m_backing.capacity() );
	}
	m_bCommitted = true;
	return true;
}

bool Terrain3DMemoryArena::seal(
	void ) {
	if ( !initialized() || !m_bCommitted ) {
		return false;
	}
	m_bSealed = true;
	return true;
}

void Terrain3DMemoryArena::release(
	void ) {
	m_backing.release();
	m_nBaseAlignment = T3D_ARENA_DEFAULT_BASE_ALIGNMENT;
	m_bInitialized = false;
	m_bCommitted = false;
	m_bSealed = false;
}

bool Terrain3DMemoryArena::sliceInBounds(
	const t3dMemoryArenaSlice_t &rSlice ) const {
	if ( !rSlice.m_bReserved || rSlice.m_iOffset > m_backing.capacity() || rSlice.m_nAlignment == 0u ||
		 ( rSlice.m_nAlignment & ( rSlice.m_nAlignment - 1u ) ) != 0u ||
		 rSlice.m_iOffset % rSlice.m_nAlignment != 0u ) {
		return false;
	}
	return rSlice.m_nSize <= m_backing.capacity() - rSlice.m_iOffset;
}

t3dMemoryArenaSpan_t Terrain3DMemoryArena::resolve(
	const t3dMemoryArenaSlice_t &rSlice ) {
	if ( !initialized() || !sliceInBounds( rSlice ) ) {
		return {};
	}
	return { m_backing.data() ? m_backing.data() + rSlice.m_iOffset : NULL, rSlice.m_nSize };
}

t3dMemoryArenaSpan_t Terrain3DMemoryArena::resolve(
	const t3dMemoryArenaSlice_t &rSlice ) const {
	if ( !initialized() || !sliceInBounds( rSlice ) ) {
		return {};
	}
	return { m_backing.data() ? m_backing.data() + rSlice.m_iOffset : NULL, rSlice.m_nSize };
}
