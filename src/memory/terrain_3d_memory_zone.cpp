// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "memory/terrain_3d_memory_zone.h"

#include <algorithm>
#include <cstring>
#include <limits>

constexpr uint32_t T3DMemoryZone::s_szSmallClasses[T3DMemoryZone::s_nSmallClassCount];

bool T3DMemoryZone::alignUp( const size_t nValue, const size_t nAlignment, size_t &rValue ) {
	if ( !isPowerOfTwo( nAlignment ) || nValue > std::numeric_limits<size_t>::max() - ( nAlignment - 1u ) ) {
		return false;
	}
	rValue = ( nValue + nAlignment - 1u ) & ~( nAlignment - 1u );
	return true;
}

bool T3DMemoryZone::initialize( void *const pBacking, const size_t nBytes ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( !pBacking || nBytes < s_nSmallPageBytes || m_state == T3D_MEMORY_ZONE_ACTIVE ||
			m_state == T3D_MEMORY_ZONE_DRAINING ) {
		return false;
	}
	m_pBase = static_cast<uint8_t *>( pBacking );
	m_nCapacity = nBytes;
	m_nFreeExtentCount = 1u;
	m_szFreeExtents[0] = { 0u, nBytes };
	for ( t3dMemoryZoneSmallPage_t &page : m_szSmallPages ) {
		page = {};
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

int T3DMemoryZone::classIndexFor( const size_t nBytes, const size_t nAlignment ) const {
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

bool T3DMemoryZone::insertFreeExtentUnlocked( const size_t nOffset, const size_t nBytes ) {
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
	for ( uint32_t move = m_nFreeExtentCount; move > index; --move ) {
		m_szFreeExtents[move] = m_szFreeExtents[move - 1u];
	}
	m_szFreeExtents[index] = { nOffset, nBytes };
	++m_nFreeExtentCount;
	if ( index > 0u ) {
		t3dMemoryZoneExtent_t &previous = m_szFreeExtents[index - 1u];
		if ( previous.m_iOffset + previous.m_nSize == nOffset ) {
			previous.m_nSize += nBytes;
			for ( uint32_t move = index; move + 1u < m_nFreeExtentCount; ++move ) {
				m_szFreeExtents[move] = m_szFreeExtents[move + 1u];
			}
			--m_nFreeExtentCount;
			index--;
		}
	}
	if ( index + 1u < m_nFreeExtentCount ) {
		t3dMemoryZoneExtent_t &current = m_szFreeExtents[index];
		if ( current.m_iOffset + current.m_nSize == m_szFreeExtents[index + 1u].m_iOffset ) {
			current.m_nSize += m_szFreeExtents[index + 1u].m_nSize;
			for ( uint32_t move = index + 1u; move + 1u < m_nFreeExtentCount; ++move ) {
				m_szFreeExtents[move] = m_szFreeExtents[move + 1u];
			}
			--m_nFreeExtentCount;
		}
	}
	m_nPeakFreeExtentCount = std::max( m_nPeakFreeExtentCount, m_nFreeExtentCount );
	return true;
}

bool T3DMemoryZone::takeExtentUnlocked( const size_t nBytes,
		const size_t nAlignment,
		const size_t nPrefixBytes,
		size_t &rExtentOffset,
		size_t &rUserOffset ) {
	for ( uint32_t index = 0u; index < m_nFreeExtentCount; ++index ) {
		const t3dMemoryZoneExtent_t source = m_szFreeExtents[index];
		const uintptr_t base = reinterpret_cast<uintptr_t>( m_pBase );
		if ( source.m_iOffset > std::numeric_limits<uintptr_t>::max() - base - nPrefixBytes ) {
			continue;
		}
		size_t aligned_address = 0u;
		if ( !alignUp( base + source.m_iOffset + nPrefixBytes, nAlignment, aligned_address ) ||
				aligned_address < base ) {
			continue;
		}
		const size_t user = aligned_address - base;
		const size_t start = user - nPrefixBytes;
		if ( start < source.m_iOffset || nBytes > source.m_nSize - ( start - source.m_iOffset ) ||
				user > std::numeric_limits<size_t>::max() - nBytes ) {
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
		for ( uint32_t move = index; move + 1u < m_nFreeExtentCount; ++move ) {
			m_szFreeExtents[move] = m_szFreeExtents[move + 1u];
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

void *T3DMemoryZone::allocateSmallUnlocked( const int nClassIndex, const size_t nBytes ) {
	uint32_t page_index = s_nMaxSmallPages;
	for ( uint32_t i = 0u; i < s_nMaxSmallPages; ++i ) {
		if ( m_szSmallPages[i].m_bActive && m_szSmallPages[i].m_iClass == nClassIndex &&
				m_szSmallPages[i].m_iFirstFree != UINT32_MAX ) {
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
			return nullptr;
		}
		size_t page_offset = 0u, ignored = 0u;
		if ( !takeExtentUnlocked( s_nSmallPageBytes, 64u, 0u, page_offset, ignored ) ) {
			return nullptr;
		}
		t3dMemoryZoneSmallPage_t &page = m_szSmallPages[page_index];
		page = {};
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
		m_nPeakActiveSmallPages = std::max( m_nPeakActiveSmallPages, m_nActiveSmallPages );
		for ( uint32_t block = 0u; block < page.m_nTotalBlocks; ++block ) {
			uint8_t *payload =
					m_pBase + page.m_iOffset + ( size_t )block * stride + sizeof( t3dMemoryZoneAllocationHeader_t );
			*reinterpret_cast<uint32_t *>( payload ) = block + 1u < page.m_nTotalBlocks ? block + 1u : UINT32_MAX;
		}
	}
	t3dMemoryZoneSmallPage_t &page = m_szSmallPages[page_index];
	const uint32_t block = page.m_iFirstFree;
	uint8_t *const block_start = m_pBase + page.m_iOffset + ( size_t )block * page.m_nBlockStride;
	uint8_t *const payload = block_start + sizeof( t3dMemoryZoneAllocationHeader_t );
	page.m_iFirstFree = *reinterpret_cast<uint32_t *>( payload );
	++page.m_nLiveBlocks;
	t3dMemoryZoneAllocationHeader_t *const header = reinterpret_cast<t3dMemoryZoneAllocationHeader_t *>( block_start );
	*header = {};
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
	m_nLiveAlignmentWasteBytes +=
			page.m_nBlockStride - sizeof( t3dMemoryZoneAllocationHeader_t ) - s_szSmallClasses[nClassIndex];
	return payload;
}

void *T3DMemoryZone::allocateLargeUnlocked( const size_t nBytes, const size_t nAlignment ) {
	if ( nBytes >
			std::numeric_limits<size_t>::max() - sizeof( t3dMemoryZoneAllocationHeader_t ) - ( nAlignment - 1u ) ) {
		return nullptr;
	}
	size_t extent_offset = 0u, user_offset = 0u;
	if ( !takeExtentUnlocked(
				 nBytes, nAlignment, sizeof( t3dMemoryZoneAllocationHeader_t ), extent_offset, user_offset ) ) {
		return nullptr;
	}
	const size_t used = user_offset + nBytes - extent_offset;
	t3dMemoryZoneAllocationHeader_t *const header = reinterpret_cast<t3dMemoryZoneAllocationHeader_t *>(
			m_pBase + user_offset - sizeof( t3dMemoryZoneAllocationHeader_t ) );
	*header = {};
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

void *T3DMemoryZone::allocateUnlocked( const size_t nBytes, const size_t nAlignment ) {
	if ( m_state != T3D_MEMORY_ZONE_ACTIVE || nBytes == 0u || !isPowerOfTwo( nAlignment ) || nAlignment > UINT32_MAX ) {
		return nullptr;
	}
	const int class_index = classIndexFor( nBytes, nAlignment );
	return class_index >= 0 ? allocateSmallUnlocked( class_index, nBytes )
							: allocateLargeUnlocked( nBytes, nAlignment );
}

void *T3DMemoryZone::allocate( const size_t nBytes, const size_t nAlignment ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	void *const pMemory = allocateUnlocked( nBytes, std::max( nAlignment, ( size_t )alignof( void * ) ) );
	if ( !pMemory ) {
		++m_nFailedAllocations;
	}
	return pMemory;
}

t3dMemoryZoneAllocationHeader_t *T3DMemoryZone::headerFor( void *const pMemory ) const {
	return owns( pMemory ) ? reinterpret_cast<t3dMemoryZoneAllocationHeader_t *>(
									 static_cast<uint8_t *>( pMemory ) - sizeof( t3dMemoryZoneAllocationHeader_t ) )
						   : nullptr;
}
const t3dMemoryZoneAllocationHeader_t *T3DMemoryZone::headerFor( const void *const pMemory ) const {
	return owns( pMemory )
			? reinterpret_cast<const t3dMemoryZoneAllocationHeader_t *>(
					  static_cast<const uint8_t *>( pMemory ) - sizeof( t3dMemoryZoneAllocationHeader_t ) )
			: nullptr;
}

void T3DMemoryZone::freeUnlocked( void *const pMemory ) {
	t3dMemoryZoneAllocationHeader_t *const header = headerFor( pMemory );
	if ( !header || header->m_nMagic != s_nAllocationMagic ) {
		return;
	}
	const size_t requested = header->m_nRequestedBytes;
	const size_t physical = header->m_nExtentBytes;
	const uint16_t page_index = header->m_iPage;
	const uint16_t class_index = header->m_iClass;
	header->m_nMagic = 0u;
	if ( page_index != UINT16_MAX ) {
		t3dMemoryZoneSmallPage_t &page = m_szSmallPages[page_index];
		const uint32_t block = ( uint32_t )( ( header->m_iExtentOffset - page.m_iOffset ) / page.m_nBlockStride );
		*reinterpret_cast<uint32_t *>( pMemory ) = page.m_iFirstFree;
		page.m_iFirstFree = block;
		if ( page.m_nLiveBlocks > 0u ) {
			--page.m_nLiveBlocks;
		}
		page.m_nLiveRequestedBytes -= requested;
		m_nLiveSizeClassWasteBytes -= s_szSmallClasses[class_index] - requested;
		m_nLiveAlignmentWasteBytes -=
				page.m_nBlockStride - sizeof( t3dMemoryZoneAllocationHeader_t ) - s_szSmallClasses[class_index];
		if ( page.m_nLiveBlocks == 0u ) {
			insertFreeExtentUnlocked( page.m_iOffset, page.m_nBytes );
			page = {};
			if ( m_nActiveSmallPages > 0u ) {
				--m_nActiveSmallPages;
			}
		}
	} else {
		m_nLiveAlignmentWasteBytes -= physical - sizeof( t3dMemoryZoneAllocationHeader_t ) - requested;
		insertFreeExtentUnlocked( header->m_iExtentOffset, physical );
	}
	--m_nLiveAllocations;
	m_nLiveBytes -= requested;
	m_nLivePhysicalBytes -= physical;
	m_nLiveMetadataBytes -= sizeof( t3dMemoryZoneAllocationHeader_t );
}

void T3DMemoryZone::deallocate( void *const pMemory ) {
	if ( !pMemory ) {
		return;
	}
	std::lock_guard<std::mutex> lock( m_mutex );
	freeUnlocked( pMemory );
}

bool T3DMemoryZone::release() {
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( m_state == T3D_MEMORY_ZONE_UNINITIALIZED || m_state == T3D_MEMORY_ZONE_DEAD ) {
		return true;
	}
	if ( m_nLiveAllocations != 0u ) {
		m_state = T3D_MEMORY_ZONE_DRAINING;
		return false;
	}
	m_pBase = nullptr;
	m_nCapacity = 0u;
	m_nFreeExtentCount = 0u;
	m_state = T3D_MEMORY_ZONE_DEAD;
	return true;
}

size_t T3DMemoryZone::freeExtentBytes( void ) const {
	std::lock_guard<std::mutex> lock( m_mutex );
	size_t bytes = 0u;
	for ( uint32_t i = 0u; i < m_nFreeExtentCount; ++i ) {
		bytes += m_szFreeExtents[i].m_nSize;
	}
	return bytes;
}

t3dMemoryZoneStats_t T3DMemoryZone::getStats( void ) const {
	std::lock_guard<std::mutex> lock( m_mutex );
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
		const size_t free_blocks =
				page.m_nTotalBlocks >= page.m_nLiveBlocks ? ( size_t )page.m_nTotalBlocks - page.m_nLiveBlocks : 0u;
		stats.m_nSmallPageReusablePayloadBytes += free_blocks * s_szSmallClasses[page.m_iClass];
		stats.m_nSmallPageReusablePhysicalBytes += free_blocks * page.m_nBlockStride;
		small_live_physical += ( size_t )page.m_nLiveBlocks * page.m_nBlockStride;
	}
	stats.m_nLargeAllocationPhysicalBytes =
			m_nLivePhysicalBytes >= small_live_physical ? m_nLivePhysicalBytes - small_live_physical : 0u;
	return stats;
}
