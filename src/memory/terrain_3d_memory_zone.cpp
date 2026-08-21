// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "memory/terrain_3d_memory_zone.h"

#include <algorithm>
#include <cstring>
#include <limits>

constexpr uint32_t T3DMemoryZone::SMALL_CLASSES[T3DMemoryZone::SMALL_CLASS_COUNT];



bool T3DMemoryZone::alignUp( const size_t nValue, const size_t nAlignment, size_t &rValue ) {
	if ( !isPowerOfTwo( nAlignment ) || nValue > std::numeric_limits<size_t>::max() - ( nAlignment - 1u ) ) return false;
	rValue = ( nValue + nAlignment - 1u ) & ~( nAlignment - 1u );
	return true;
}

bool T3DMemoryZone::initialize( void *const pBacking, const size_t nBytes ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( !pBacking || nBytes < SMALL_PAGE_BYTES || m_state == ACTIVE || m_state == DRAINING ) return false;
	m_pBase = static_cast<uint8_t *>( pBacking );
	m_nCapacity = nBytes;
	m_nFreeExtentCount = 1u;
	m_szFreeExtents[0] = {0u, nBytes};
	for ( SmallPage &page : m_szSmallPages ) page = {};
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
	m_state = ACTIVE;
	return true;
}

int T3DMemoryZone::classIndexFor( const size_t nBytes, const size_t nAlignment ) const {
	if ( nAlignment > 16u ) return -1;
	for ( uint32_t i = 0u; i < SMALL_CLASS_COUNT; ++i ) if ( nBytes <= SMALL_CLASSES[i] ) return ( int ) i;
	return -1;
}

bool T3DMemoryZone::insertFreeExtentUnlocked( const size_t nOffset, const size_t nBytes ) {
	if ( nBytes == 0u ) return true;
	if ( nOffset > m_nCapacity || nBytes > m_nCapacity - nOffset || m_nFreeExtentCount >= MAX_EXTENTS ) return false;
	uint32_t index = 0u;
	while ( index < m_nFreeExtentCount && m_szFreeExtents[index].offset < nOffset ) ++index;
	for ( uint32_t move = m_nFreeExtentCount; move > index; --move ) m_szFreeExtents[move] = m_szFreeExtents[move - 1u];
	m_szFreeExtents[index] = {nOffset, nBytes};
	++m_nFreeExtentCount;
	if ( index > 0u ) {
		Extent &previous = m_szFreeExtents[index - 1u];
		if ( previous.offset + previous.size == nOffset ) {
			previous.size += nBytes;
			for ( uint32_t move = index; move + 1u < m_nFreeExtentCount; ++move ) m_szFreeExtents[move] = m_szFreeExtents[move + 1u];
			--m_nFreeExtentCount;
			index--;
		}
	}
	if ( index + 1u < m_nFreeExtentCount ) {
		Extent &current = m_szFreeExtents[index];
		if ( current.offset + current.size == m_szFreeExtents[index + 1u].offset ) {
			current.size += m_szFreeExtents[index + 1u].size;
			for ( uint32_t move = index + 1u; move + 1u < m_nFreeExtentCount; ++move ) m_szFreeExtents[move] = m_szFreeExtents[move + 1u];
			--m_nFreeExtentCount;
		}
	}
	m_nPeakFreeExtentCount = std::max( m_nPeakFreeExtentCount, m_nFreeExtentCount );
	return true;
}

bool T3DMemoryZone::takeExtentUnlocked( const size_t nBytes, const size_t nAlignment,
	const size_t nPrefixBytes, size_t &rExtentOffset, size_t &rUserOffset ) {
	for ( uint32_t index = 0u; index < m_nFreeExtentCount; ++index ) {
		const Extent source = m_szFreeExtents[index];
		const uintptr_t base = reinterpret_cast<uintptr_t>( m_pBase );
		if ( source.offset > std::numeric_limits<uintptr_t>::max() - base - nPrefixBytes ) continue;
		size_t aligned_address = 0u;
		if ( !alignUp( base + source.offset + nPrefixBytes, nAlignment, aligned_address ) || aligned_address < base ) continue;
		const size_t user = aligned_address - base;
		const size_t start = user - nPrefixBytes;
		if ( start < source.offset || nBytes > source.size - ( start - source.offset ) ||
			user > std::numeric_limits<size_t>::max() - nBytes ) continue;
		const size_t end = user + nBytes;
		if ( end > source.offset + source.size ) continue;
		const uint32_t required_extent_slots = ( start > source.offset ? 1u : 0u ) +
			( end < source.offset + source.size ? 1u : 0u );
		if ( m_nFreeExtentCount - 1u + required_extent_slots > MAX_EXTENTS ) continue;
		for ( uint32_t move = index; move + 1u < m_nFreeExtentCount; ++move ) m_szFreeExtents[move] = m_szFreeExtents[move + 1u];
		--m_nFreeExtentCount;
		if ( start > source.offset ) insertFreeExtentUnlocked( source.offset, start - source.offset );
		if ( end < source.offset + source.size ) insertFreeExtentUnlocked( end, source.offset + source.size - end );
		rExtentOffset = start;
		rUserOffset = user;
		return true;
	}
	return false;
}

void *T3DMemoryZone::allocateSmallUnlocked( const int nClassIndex, const size_t nBytes ) {
	uint32_t page_index = MAX_SMALL_PAGES;
	for ( uint32_t i = 0u; i < MAX_SMALL_PAGES; ++i ) {
		if ( m_szSmallPages[i].active && m_szSmallPages[i].classIndex == nClassIndex && m_szSmallPages[i].firstFree != UINT32_MAX ) { page_index = i; break; }
	}
	if ( page_index == MAX_SMALL_PAGES ) {
		for ( uint32_t i = 0u; i < MAX_SMALL_PAGES; ++i ) if ( !m_szSmallPages[i].active ) { page_index = i; break; }
		if ( page_index == MAX_SMALL_PAGES ) return nullptr;
		size_t page_offset = 0u, ignored = 0u;
		if ( !takeExtentUnlocked( SMALL_PAGE_BYTES, 64u, 0u, page_offset, ignored ) ) return nullptr;
		SmallPage &page = m_szSmallPages[page_index];
		page = {};
		page.active = true;
		page.offset = page_offset;
		page.bytes = SMALL_PAGE_BYTES;
		page.classIndex = ( uint16_t ) nClassIndex;
		size_t stride = 0u;
		alignUp( sizeof( AllocationHeader ) + SMALL_CLASSES[nClassIndex], 16u, stride );
		page.blockStride = ( uint32_t ) stride;
		page.totalBlocks = ( uint16_t ) ( SMALL_PAGE_BYTES / stride );
		page.firstFree = 0u;
		++m_nActiveSmallPages;
		m_nPeakActiveSmallPages = std::max( m_nPeakActiveSmallPages, m_nActiveSmallPages );
		for ( uint32_t block = 0u; block < page.totalBlocks; ++block ) {
			uint8_t *payload = m_pBase + page.offset + ( size_t ) block * stride + sizeof( AllocationHeader );
			*reinterpret_cast<uint32_t *>( payload ) = block + 1u < page.totalBlocks ? block + 1u : UINT32_MAX;
		}
	}
	SmallPage &page = m_szSmallPages[page_index];
	const uint32_t block = page.firstFree;
	uint8_t *const block_start = m_pBase + page.offset + ( size_t ) block * page.blockStride;
	uint8_t *const payload = block_start + sizeof( AllocationHeader );
	page.firstFree = *reinterpret_cast<uint32_t *>( payload );
	++page.liveBlocks;
	AllocationHeader *const header = reinterpret_cast<AllocationHeader *>( block_start );
	*header = {};
	header->magic = ALLOCATION_MAGIC;
	header->extentOffset = page.offset + ( size_t ) block * page.blockStride;
	header->extentBytes = page.blockStride;
	header->requestedBytes = nBytes;
	header->alignment = 16u;
	header->pageIndex = ( uint16_t ) page_index;
	header->classIndex = ( uint16_t ) nClassIndex;
	++m_nLiveAllocations;
	m_nLiveBytes += nBytes;
	page.liveRequestedBytes += nBytes;
	m_nLivePhysicalBytes += page.blockStride;
	m_nLiveMetadataBytes += sizeof( AllocationHeader );
	m_nLiveSizeClassWasteBytes += SMALL_CLASSES[nClassIndex] - nBytes;
	m_nLiveAlignmentWasteBytes += page.blockStride - sizeof( AllocationHeader ) - SMALL_CLASSES[nClassIndex];
	return payload;
}

void *T3DMemoryZone::allocateLargeUnlocked( const size_t nBytes, const size_t nAlignment ) {
	if ( nBytes > std::numeric_limits<size_t>::max() - sizeof( AllocationHeader ) - ( nAlignment - 1u ) ) return nullptr;
	size_t extent_offset = 0u, user_offset = 0u;
	if ( !takeExtentUnlocked( nBytes, nAlignment, sizeof( AllocationHeader ), extent_offset, user_offset ) ) return nullptr;
	const size_t used = user_offset + nBytes - extent_offset;
	AllocationHeader *const header = reinterpret_cast<AllocationHeader *>( m_pBase + user_offset - sizeof( AllocationHeader ) );
	*header = {};
	header->magic = ALLOCATION_MAGIC;
	header->extentOffset = extent_offset;
	header->extentBytes = used;
	header->requestedBytes = nBytes;
	header->alignment = ( uint32_t ) nAlignment;
	++m_nLiveAllocations;
	m_nLiveBytes += nBytes;
	m_nLivePhysicalBytes += used;
	m_nLiveMetadataBytes += sizeof( AllocationHeader );
	m_nLiveAlignmentWasteBytes += used - sizeof( AllocationHeader ) - nBytes;
	return m_pBase + user_offset;
}

void *T3DMemoryZone::allocateUnlocked( const size_t nBytes, const size_t nAlignment ) {
	if ( m_state != ACTIVE || nBytes == 0u || !isPowerOfTwo( nAlignment ) || nAlignment > UINT32_MAX ) return nullptr;
	const int class_index = classIndexFor( nBytes, nAlignment );
	return class_index >= 0 ? allocateSmallUnlocked( class_index, nBytes ) : allocateLargeUnlocked( nBytes, nAlignment );
}

void *T3DMemoryZone::allocate( const size_t nBytes, const size_t nAlignment ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	void *const pMemory = allocateUnlocked( nBytes, std::max( nAlignment, ( size_t ) alignof( void * ) ) );
	if ( !pMemory ) ++m_nFailedAllocations;
	return pMemory;
}

T3DMemoryZone::AllocationHeader *T3DMemoryZone::headerFor( void *const pMemory ) const {
	return owns( pMemory ) ? reinterpret_cast<AllocationHeader *>( static_cast<uint8_t *>( pMemory ) - sizeof( AllocationHeader ) ) : nullptr;
}
const T3DMemoryZone::AllocationHeader *T3DMemoryZone::headerFor( const void *const pMemory ) const {
	return owns( pMemory ) ? reinterpret_cast<const AllocationHeader *>( static_cast<const uint8_t *>( pMemory ) - sizeof( AllocationHeader ) ) : nullptr;
}

void T3DMemoryZone::freeUnlocked( void *const pMemory ) {
	AllocationHeader *const header = headerFor( pMemory );
	if ( !header || header->magic != ALLOCATION_MAGIC ) return;
	const size_t requested = header->requestedBytes;
	const size_t physical = header->extentBytes;
	const uint16_t page_index = header->pageIndex;
	const uint16_t class_index = header->classIndex;
	header->magic = 0u;
	if ( page_index != UINT16_MAX ) {
		SmallPage &page = m_szSmallPages[page_index];
		const uint32_t block = ( uint32_t ) ( ( header->extentOffset - page.offset ) / page.blockStride );
		*reinterpret_cast<uint32_t *>( pMemory ) = page.firstFree;
		page.firstFree = block;
		if ( page.liveBlocks > 0u ) --page.liveBlocks;
		page.liveRequestedBytes -= requested;
		m_nLiveSizeClassWasteBytes -= SMALL_CLASSES[class_index] - requested;
		m_nLiveAlignmentWasteBytes -= page.blockStride - sizeof( AllocationHeader ) - SMALL_CLASSES[class_index];
		if ( page.liveBlocks == 0u ) {
			insertFreeExtentUnlocked( page.offset, page.bytes );
			page = {};
			if ( m_nActiveSmallPages > 0u ) --m_nActiveSmallPages;
		}
	} else {
		m_nLiveAlignmentWasteBytes -= physical - sizeof( AllocationHeader ) - requested;
		insertFreeExtentUnlocked( header->extentOffset, physical );
	}
	--m_nLiveAllocations;
	m_nLiveBytes -= requested;
	m_nLivePhysicalBytes -= physical;
	m_nLiveMetadataBytes -= sizeof( AllocationHeader );
}

void T3DMemoryZone::deallocate( void *const pMemory ) {
	if ( !pMemory ) return;
	std::lock_guard<std::mutex> lock( m_mutex );
	freeUnlocked( pMemory );
}

bool T3DMemoryZone::release() {
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( m_state == UNINITIALIZED || m_state == DEAD ) return true;
	if ( m_nLiveAllocations != 0u ) { m_state = DRAINING; return false; }
	m_pBase = nullptr; m_nCapacity = 0u; m_nFreeExtentCount = 0u; m_state = DEAD; return true;
}

size_t T3DMemoryZone::freeExtentBytes( void ) const {
	std::lock_guard<std::mutex> lock( m_mutex );
	size_t bytes = 0u;
	for ( uint32_t i = 0u; i < m_nFreeExtentCount; ++i ) bytes += m_szFreeExtents[i].size;
	return bytes;
}

t3dMemoryZoneStats_t T3DMemoryZone::getStats( void ) const {
	std::lock_guard<std::mutex> lock( m_mutex );
	t3dMemoryZoneStats_t stats;
	stats.nReservedBytes = m_nCapacity;
	stats.nLiveRequestedBytes = m_nLiveBytes;
	stats.nLivePhysicalBytes = m_nLivePhysicalBytes;
	stats.nLiveAllocationCount = m_nLiveAllocations;
	stats.nMetadataBytes = m_nLiveMetadataBytes;
	stats.nAlignmentWasteBytes = m_nLiveAlignmentWasteBytes;
	stats.nSizeClassWasteBytes = m_nLiveSizeClassWasteBytes;
	stats.nInternalFragmentationBytes = m_nLivePhysicalBytes >= m_nLiveBytes ? m_nLivePhysicalBytes - m_nLiveBytes : 0u;
	stats.nFailedAllocations = m_nFailedAllocations;
	stats.nActiveSmallPages = m_nActiveSmallPages;
	stats.nPeakActiveSmallPages = m_nPeakActiveSmallPages;
	stats.nFreeExtentCount = m_nFreeExtentCount;
	stats.nPeakFreeExtentCount = m_nPeakFreeExtentCount;
	for ( uint32_t i = 0u; i < m_nFreeExtentCount; ++i ) stats.nExtentFreeBytes += m_szFreeExtents[i].size;
	size_t small_live_physical = 0u;
	for ( uint32_t i = 0u; i < MAX_SMALL_PAGES; ++i ) {
		const SmallPage &page = m_szSmallPages[i];
		if ( !page.active ) continue;
		stats.nSmallPageReservedBytes += page.bytes;
		const size_t free_blocks = page.totalBlocks >= page.liveBlocks ? ( size_t ) page.totalBlocks - page.liveBlocks : 0u;
		stats.nSmallPageReusablePayloadBytes += free_blocks * SMALL_CLASSES[page.classIndex];
		stats.nSmallPageReusablePhysicalBytes += free_blocks * page.blockStride;
		small_live_physical += ( size_t ) page.liveBlocks * page.blockStride;
	}
	stats.nLargeAllocationPhysicalBytes = m_nLivePhysicalBytes >= small_live_physical ? m_nLivePhysicalBytes - small_live_physical : 0u;
	return stats;
}
