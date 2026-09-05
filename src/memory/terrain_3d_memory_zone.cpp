// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "memory/terrain_3d_memory_zone.h"
#include "core/terrain_3d_sync.h"

#include "memory/terrain_3d_memory.h"

#include <EASTL/numeric_limits.h>
#include <stdlib.h>
#include <string.h>
EA_FORCE_INLINE static void t3dMemoryZoneResetSmallPage( t3dMemoryZoneSmallPage_t &rPage ) {
	memset( &rPage, 0, sizeof( rPage ) );
	rPage.m_iFirstFree = UINT32_MAX;
	rPage.m_iClass = UINT16_MAX;
}

EA_FORCE_INLINE static void t3dMemoryZoneResetAllocationHeader( t3dMemoryZoneAllocationHeader_t &rHeader ) {
	memset( &rHeader, 0, sizeof( rHeader ) );
	rHeader.m_iPage = UINT16_MAX;
	rHeader.m_iClass = UINT16_MAX;
}

Terrain3DMemoryZone::~Terrain3DMemoryZone() {
	if ( !release() ) {
		abort();
	}
}

bool Terrain3DMemoryZone::alignUp( const size_t nValue, const size_t nAlignment, size_t &rValue ) {
	if ( !isPowerOfTwo( nAlignment ) || nValue > eastl::numeric_limits<size_t>::max() - ( nAlignment - 1u ) ) {
		return false;
	}
	rValue = ( nValue + nAlignment - 1u ) & ~( nAlignment - 1u );
	return true;
}

bool Terrain3DMemoryZone::initialize( void *const pBacking, const size_t nBytes ) {
	t3dScopedLock_t lock( m_mutex );
	if ( !pBacking || nBytes < s_nSmallPageBytes || m_state == T3D_MEMORY_ZONE_ACTIVE || m_state == T3D_MEMORY_ZONE_DRAINING ) {
		return false;
	}
	m_pBase = ( uint8_t * )( pBacking );
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
	m_nActiveSmallPages = 0u;
	m_nPeakActiveSmallPages = 0u;
	m_nPeakFreeExtentCount = 1u;
	m_state = T3D_MEMORY_ZONE_ACTIVE;
	return true;
}

int Terrain3DMemoryZone::classIndexFor( const size_t nBytes, const size_t nAlignment ) const {
	if ( nAlignment > 16u ) {
		return -1;
	}
	for ( uint32_t i = 0u; i < s_nSmallClassCount; ++i ) {
		if ( nBytes <= s_szSmallClasses[i] ) {
			return ( int )i;
		}
	}
	return -1;
}

bool Terrain3DMemoryZone::insertFreeExtentUnlocked( const size_t nOffset, const size_t nBytes ) {
	if ( nBytes == 0u ) {
		return true;
	}
	if ( nOffset > m_nCapacity || nBytes > m_nCapacity - nOffset || m_nFreeExtentCount >= s_nMaxExtents ) {
		return false;
	}
	uint32_t index = 0u;
	while ( index < m_nFreeExtentCount && m_szFreeExtents[index].m_iOffset < nOffset ) {
		++index;
	}
	if ( index < m_nFreeExtentCount ) {
		memmove( &m_szFreeExtents[index + 1u],
		        &m_szFreeExtents[index],
		        ( size_t )( m_nFreeExtentCount - index ) * sizeof( t3dMemoryZoneExtent_t ) );
	}
	m_szFreeExtents[index] = { nOffset, nBytes };
	++m_nFreeExtentCount;
	if ( index > 0u ) {
		t3dMemoryZoneExtent_t &previous = m_szFreeExtents[index - 1u];
		if ( previous.m_iOffset + previous.m_nSize == nOffset ) {
			previous.m_nSize += nBytes;
			if ( index + 1u < m_nFreeExtentCount ) {
				memmove( &m_szFreeExtents[index],
				        &m_szFreeExtents[index + 1u],
				        ( size_t )( m_nFreeExtentCount - index - 1u ) * sizeof( t3dMemoryZoneExtent_t ) );
			}
			--m_nFreeExtentCount;
			index--;
		}
	}
	if ( index + 1u < m_nFreeExtentCount ) {
		t3dMemoryZoneExtent_t &current = m_szFreeExtents[index];
		if ( current.m_iOffset + current.m_nSize == m_szFreeExtents[index + 1u].m_iOffset ) {
			current.m_nSize += m_szFreeExtents[index + 1u].m_nSize;
			if ( index + 2u < m_nFreeExtentCount ) {
				memmove( &m_szFreeExtents[index + 1u],
				        &m_szFreeExtents[index + 2u],
				        ( size_t )( m_nFreeExtentCount - index - 2u ) * sizeof( t3dMemoryZoneExtent_t ) );
			}
			--m_nFreeExtentCount;
		}
	}
	m_nPeakFreeExtentCount = MAX( m_nPeakFreeExtentCount, m_nFreeExtentCount );
	return true;
}

bool Terrain3DMemoryZone::takeExtentUnlocked(
        const size_t nBytes, const size_t nAlignment, const size_t nPrefixBytes, size_t &rExtentOffset, size_t &rUserOffset ) {
	for ( uint32_t index = 0u; index < m_nFreeExtentCount; ++index ) {
		const t3dMemoryZoneExtent_t source = m_szFreeExtents[index];
		const uintptr_t base = ( uintptr_t )( m_pBase );
		const uintptr_t address_max = eastl::numeric_limits<uintptr_t>::max();
		if ( base > address_max - source.m_iOffset ) {
			continue;
		}
		const uintptr_t source_address = base + source.m_iOffset;
		if ( source_address > address_max - nPrefixBytes ) {
			continue;
		}
		size_t aligned_address = 0u;
		if ( !alignUp( source_address + nPrefixBytes, nAlignment, aligned_address ) || aligned_address < base ) {
			continue;
		}
		const size_t user = aligned_address - base;
		const size_t start = user - nPrefixBytes;
		if ( start < source.m_iOffset || nBytes > source.m_nSize - ( start - source.m_iOffset )
		        || user > eastl::numeric_limits<size_t>::max() - nBytes ) {
			continue;
		}
		const size_t end = user + nBytes;
		if ( end > source.m_iOffset + source.m_nSize ) {
			continue;
		}
		const uint32_t required_extent_slots =
		        ( start > source.m_iOffset ? 1u : 0u ) + ( end < source.m_iOffset + source.m_nSize ? 1u : 0u );
		if ( m_nFreeExtentCount - 1u + required_extent_slots > s_nMaxExtents ) {
			continue;
		}
		if ( index + 1u < m_nFreeExtentCount ) {
			memmove( &m_szFreeExtents[index],
			        &m_szFreeExtents[index + 1u],
			        ( size_t )( m_nFreeExtentCount - index - 1u ) * sizeof( t3dMemoryZoneExtent_t ) );
		}
		--m_nFreeExtentCount;
		if ( start > source.m_iOffset ) {
			insertFreeExtentUnlocked( source.m_iOffset, start - source.m_iOffset );
		}
		if ( end < source.m_iOffset + source.m_nSize ) {
			insertFreeExtentUnlocked( end, source.m_iOffset + source.m_nSize - end );
		}
		rExtentOffset = start;
		rUserOffset = user;
		return true;
	}
	return false;
}

void *Terrain3DMemoryZone::allocateSmallUnlocked( const int nClassIndex, const size_t nBytes ) {
	uint32_t page_index = s_nMaxSmallPages;
	for ( uint32_t i = 0u; i < s_nMaxSmallPages; ++i ) {
		if ( m_szSmallPages[i].m_bActive && m_szSmallPages[i].m_iClass == nClassIndex && m_szSmallPages[i].m_iFirstFree != UINT32_MAX ) {
			page_index = i;
			break;
		}
	}
	if ( page_index == s_nMaxSmallPages ) {
		for ( uint32_t i = 0u; i < s_nMaxSmallPages; ++i ) {
			if ( !m_szSmallPages[i].m_bActive ) {
				page_index = i;
				break;
			}
		}
		if ( page_index == s_nMaxSmallPages ) {
			return NULL;
		}
		size_t page_offset = 0u, ignored = 0u;
		if ( !takeExtentUnlocked( s_nSmallPageBytes, 64u, 0u, page_offset, ignored ) ) {
			return NULL;
		}
		t3dMemoryZoneSmallPage_t &page = m_szSmallPages[page_index];
		t3dMemoryZoneResetSmallPage( page );
		page.m_bActive = true;
		page.m_iOffset = page_offset;
		page.m_nBytes = s_nSmallPageBytes;
		page.m_iClass = ( uint16_t )nClassIndex;
		size_t stride = 0u;
		alignUp( sizeof( t3dMemoryZoneAllocationHeader_t ) + s_szSmallClasses[nClassIndex], 16u, stride );
		page.m_nBlockStride = ( uint32_t )stride;
		page.m_nTotalBlocks = ( uint16_t )( s_nSmallPageBytes / stride );
		page.m_iFirstFree = 0u;
		++m_nActiveSmallPages;
		m_nPeakActiveSmallPages = MAX( m_nPeakActiveSmallPages, m_nActiveSmallPages );
		for ( uint32_t block = 0u; block < page.m_nTotalBlocks; ++block ) {
			uint8_t *payload = m_pBase + page.m_iOffset + ( size_t )block * stride + sizeof( t3dMemoryZoneAllocationHeader_t );
			*( uint32_t * )( payload ) = block + 1u < page.m_nTotalBlocks ? block + 1u : UINT32_MAX;
		}
	}
	t3dMemoryZoneSmallPage_t &page = m_szSmallPages[page_index];
	const uint32_t block = page.m_iFirstFree;
	uint8_t *const block_start = m_pBase + page.m_iOffset + ( size_t )block * page.m_nBlockStride;
	uint8_t *const payload = block_start + sizeof( t3dMemoryZoneAllocationHeader_t );
	page.m_iFirstFree = *( uint32_t * )( payload );
	++page.m_nLiveBlocks;
	t3dMemoryZoneAllocationHeader_t *const header = ( t3dMemoryZoneAllocationHeader_t * )( block_start );
	t3dMemoryZoneResetAllocationHeader( *header );
	header->m_nMagic = s_nAllocationMagic;
	header->m_iExtentOffset = page.m_iOffset + ( size_t )block * page.m_nBlockStride;
	header->m_nExtentBytes = page.m_nBlockStride;
	header->m_nRequestedBytes = nBytes;
	header->m_nAlignment = 16u;
	header->m_iPage = ( uint16_t )page_index;
	header->m_iClass = ( uint16_t )nClassIndex;
	++m_nLiveAllocations;
	m_nLiveBytes += nBytes;
	page.m_nLiveRequestedBytes += nBytes;
	m_nLivePhysicalBytes += page.m_nBlockStride;
	m_nLiveMetadataBytes += sizeof( t3dMemoryZoneAllocationHeader_t );
	m_nLiveSizeClassWasteBytes += s_szSmallClasses[nClassIndex] - nBytes;
	m_nLiveAlignmentWasteBytes += page.m_nBlockStride - sizeof( t3dMemoryZoneAllocationHeader_t ) - s_szSmallClasses[nClassIndex];
	return payload;
}

void *Terrain3DMemoryZone::allocateLargeUnlocked( const size_t nBytes, const size_t nAlignment ) {
	if ( nBytes > eastl::numeric_limits<size_t>::max() - sizeof( t3dMemoryZoneAllocationHeader_t ) - ( nAlignment - 1u ) ) {
		return NULL;
	}
	size_t extent_offset = 0u, user_offset = 0u;
	if ( !takeExtentUnlocked( nBytes, nAlignment, sizeof( t3dMemoryZoneAllocationHeader_t ), extent_offset, user_offset ) ) {
		return NULL;
	}
	const size_t used = user_offset + nBytes - extent_offset;
	t3dMemoryZoneAllocationHeader_t *const header =
	        ( t3dMemoryZoneAllocationHeader_t * )( m_pBase + user_offset - sizeof( t3dMemoryZoneAllocationHeader_t ) );
	t3dMemoryZoneResetAllocationHeader( *header );
	header->m_nMagic = s_nAllocationMagic;
	header->m_iExtentOffset = extent_offset;
	header->m_nExtentBytes = used;
	header->m_nRequestedBytes = nBytes;
	header->m_nAlignment = ( uint32_t )nAlignment;
	++m_nLiveAllocations;
	m_nLiveBytes += nBytes;
	m_nLivePhysicalBytes += used;
	m_nLiveMetadataBytes += sizeof( t3dMemoryZoneAllocationHeader_t );
	m_nLiveAlignmentWasteBytes += used - sizeof( t3dMemoryZoneAllocationHeader_t ) - nBytes;
	return m_pBase + user_offset;
}

void *Terrain3DMemoryZone::allocateUnlocked( const size_t nBytes, const size_t nAlignment ) {
	if ( m_state != T3D_MEMORY_ZONE_ACTIVE || nBytes == 0u || !isPowerOfTwo( nAlignment ) || nAlignment > UINT32_MAX ) {
		return NULL;
	}
	const int class_index = classIndexFor( nBytes, nAlignment );
	return class_index >= 0 ? allocateSmallUnlocked( class_index, nBytes ) : allocateLargeUnlocked( nBytes, nAlignment );
}

void *Terrain3DMemoryZone::allocate( const size_t nBytes, const size_t nAlignment ) {
	if ( nBytes == 0u ) {
		return NULL;
	}
	t3dScopedLock_t lock( m_mutex );
	void *const pMemory = allocateUnlocked( nBytes, MAX( nAlignment, ( size_t )alignof( void * ) ) );
	if ( !pMemory ) {
		++m_nFailedAllocations;
		return NULL;
	}
	t3dRecordRuntimeZoneAllocation( nBytes );
	return pMemory;
}

t3dMemoryZoneAllocationHeader_t *Terrain3DMemoryZone::headerFor( void *const pMemory ) const {
	return owns( pMemory ) ? ( t3dMemoryZoneAllocationHeader_t * )( ( uint8_t * )( pMemory ) - sizeof( t3dMemoryZoneAllocationHeader_t ) )
	                       : NULL;
}
const t3dMemoryZoneAllocationHeader_t *Terrain3DMemoryZone::headerFor( const void *const pMemory ) const {
	return owns( pMemory )
	        ? ( const t3dMemoryZoneAllocationHeader_t * )( ( const uint8_t * )( pMemory ) - sizeof( t3dMemoryZoneAllocationHeader_t ) )
	        : NULL;
}

void Terrain3DMemoryZone::freeUnlocked( void *const pMemory ) {
	t3dMemoryZoneAllocationHeader_t *const header = headerFor( pMemory );
	if ( !header || header->m_nMagic != s_nAllocationMagic ) {
		return;
	}
	const size_t requested = header->m_nRequestedBytes;
	const size_t physical = header->m_nExtentBytes;
	const size_t extent_offset = header->m_iExtentOffset;
	const uint16_t page_index = header->m_iPage;
	const uint16_t class_index = header->m_iClass;
	if ( requested == 0u || physical < sizeof( t3dMemoryZoneAllocationHeader_t ) || extent_offset > m_nCapacity
	        || physical > m_nCapacity - extent_offset || requested > physical - sizeof( t3dMemoryZoneAllocationHeader_t ) ) {
		return;
	}
	if ( page_index != UINT16_MAX ) {
		if ( page_index >= s_nMaxSmallPages || class_index >= s_nSmallClassCount ) {
			return;
		}
		t3dMemoryZoneSmallPage_t &page = m_szSmallPages[page_index];
		if ( !page.m_bActive || page.m_iClass != class_index || page.m_nBlockStride == 0u || extent_offset < page.m_iOffset
		        || extent_offset >= page.m_iOffset + page.m_nBytes || ( extent_offset - page.m_iOffset ) % page.m_nBlockStride != 0u ) {
			return;
		}
		const uint32_t block = ( uint32_t )( ( extent_offset - page.m_iOffset ) / page.m_nBlockStride );
		if ( block >= page.m_nTotalBlocks || requested > s_szSmallClasses[class_index] ) {
			return;
		}
		header->m_nMagic = 0u;
		*( uint32_t * )( pMemory ) = page.m_iFirstFree;
		page.m_iFirstFree = block;
		if ( page.m_nLiveBlocks > 0u ) {
			--page.m_nLiveBlocks;
		}
		page.m_nLiveRequestedBytes -= requested;
		m_nLiveSizeClassWasteBytes -= s_szSmallClasses[class_index] - requested;
		m_nLiveAlignmentWasteBytes -= page.m_nBlockStride - sizeof( t3dMemoryZoneAllocationHeader_t ) - s_szSmallClasses[class_index];
		if ( page.m_nLiveBlocks == 0u ) {
			insertFreeExtentUnlocked( page.m_iOffset, page.m_nBytes );
			t3dMemoryZoneResetSmallPage( page );
			if ( m_nActiveSmallPages > 0u ) {
				--m_nActiveSmallPages;
			}
		}
	} else {
		if ( class_index != UINT16_MAX ) {
			return;
		}
		header->m_nMagic = 0u;
		m_nLiveAlignmentWasteBytes -= physical - sizeof( t3dMemoryZoneAllocationHeader_t ) - requested;
		insertFreeExtentUnlocked( extent_offset, physical );
	}
	--m_nLiveAllocations;
	m_nLiveBytes -= requested;
	m_nLivePhysicalBytes -= physical;
	m_nLiveMetadataBytes -= sizeof( t3dMemoryZoneAllocationHeader_t );
}

void Terrain3DMemoryZone::deallocate( void *const pMemory ) {
	if ( !pMemory ) {
		return;
	}
	t3dScopedLock_t lock( m_mutex );
	freeUnlocked( pMemory );
}

bool Terrain3DMemoryZone::release() {
	t3dScopedLock_t lock( m_mutex );
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

size_t Terrain3DMemoryZone::freeExtentBytes( void ) const {
	t3dScopedLock_t lock( m_mutex );
	size_t bytes = 0u;
	for ( uint32_t i = 0u; i < m_nFreeExtentCount; ++i ) {
		bytes += m_szFreeExtents[i].m_nSize;
	}
	return bytes;
}

t3dMemoryZoneStats_t Terrain3DMemoryZone::getStats( void ) const {
	t3dScopedLock_t lock( m_mutex );
	t3dMemoryZoneStats_t stats;
	stats.m_nReservedBytes = m_nCapacity;
	stats.m_nLiveRequestedBytes = m_nLiveBytes;
	stats.m_nLivePhysicalBytes = m_nLivePhysicalBytes;
	stats.m_nLiveAllocationCount = m_nLiveAllocations;
	stats.m_nMetadataBytes = m_nLiveMetadataBytes;
	stats.m_nAlignmentWasteBytes = m_nLiveAlignmentWasteBytes;
	stats.m_nSizeClassWasteBytes = m_nLiveSizeClassWasteBytes;
	stats.m_nInternalFragmentationBytes = m_nLivePhysicalBytes >= m_nLiveBytes ? m_nLivePhysicalBytes - m_nLiveBytes : 0u;
	stats.m_nFailedAllocations = m_nFailedAllocations;
	stats.m_nActiveSmallPages = m_nActiveSmallPages;
	stats.m_nPeakActiveSmallPages = m_nPeakActiveSmallPages;
	stats.m_nFreeExtentCount = m_nFreeExtentCount;
	stats.m_nPeakFreeExtentCount = m_nPeakFreeExtentCount;
	for ( uint32_t i = 0u; i < m_nFreeExtentCount; ++i ) {
		stats.m_nExtentFreeBytes += m_szFreeExtents[i].m_nSize;
	}
	size_t small_live_physical = 0u;
	for ( uint32_t i = 0u; i < s_nMaxSmallPages; ++i ) {
		const t3dMemoryZoneSmallPage_t &page = m_szSmallPages[i];
		if ( !page.m_bActive ) {
			continue;
		}
		stats.m_nSmallPageReservedBytes += page.m_nBytes;
		const size_t free_blocks = page.m_nTotalBlocks >= page.m_nLiveBlocks ? ( size_t )page.m_nTotalBlocks - page.m_nLiveBlocks : 0u;
		stats.m_nSmallPageReusablePayloadBytes += free_blocks * s_szSmallClasses[page.m_iClass];
		stats.m_nSmallPageReusablePhysicalBytes += free_blocks * page.m_nBlockStride;
		small_live_physical += ( size_t )page.m_nLiveBlocks * page.m_nBlockStride;
	}
	stats.m_nLargeAllocationPhysicalBytes = m_nLivePhysicalBytes >= small_live_physical ? m_nLivePhysicalBytes - small_live_physical : 0u;
	return stats;
}
