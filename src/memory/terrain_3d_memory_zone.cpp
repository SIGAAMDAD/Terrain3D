// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "memory/terrain_3d_memory_zone.h"

#include <algorithm>
#include <cstring>
#include <limits>

constexpr uint32_t T3DMemoryZone::SMALL_CLASSES[T3DMemoryZone::SMALL_CLASS_COUNT];

T3DMemoryZone::~T3DMemoryZone() { release(); }

bool T3DMemoryZone::isPowerOfTwo( const size_t nValue ) { return nValue && ( nValue & ( nValue - 1u ) ) == 0u; }

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
	return m_pBase + user_offset;
}

void *T3DMemoryZone::allocateUnlocked( const size_t nBytes, const size_t nAlignment ) {
	if ( m_state != ACTIVE || nBytes == 0u || !isPowerOfTwo( nAlignment ) || nAlignment > UINT32_MAX ) return nullptr;
	const int class_index = classIndexFor( nBytes, nAlignment );
	return class_index >= 0 ? allocateSmallUnlocked( class_index, nBytes ) : allocateLargeUnlocked( nBytes, nAlignment );
}

void *T3DMemoryZone::allocate( const size_t nBytes, const size_t nAlignment ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	return allocateUnlocked( nBytes, std::max( nAlignment, ( size_t ) alignof( void * ) ) );
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
	header->magic = 0u;
	if ( header->pageIndex != UINT16_MAX ) {
		const uint16_t page_index = header->pageIndex;
		SmallPage &page = m_szSmallPages[page_index];
		const uint32_t block = ( uint32_t ) ( ( header->extentOffset - page.offset ) / page.blockStride );
		*reinterpret_cast<uint32_t *>( pMemory ) = page.firstFree;
		page.firstFree = block;
		if ( page.liveBlocks > 0u ) --page.liveBlocks;
		if ( page.liveBlocks == 0u ) {
			insertFreeExtentUnlocked( page.offset, page.bytes );
			page = {};
		}
	} else {
		insertFreeExtentUnlocked( header->extentOffset, header->extentBytes );
	}
	--m_nLiveAllocations;
	m_nLiveBytes -= requested;
	if ( m_state == DRAINING && m_nLiveAllocations == 0u ) m_drained.notify_all();
}

void T3DMemoryZone::deallocate( void *const pMemory ) {
	if ( !pMemory ) return;
	std::lock_guard<std::mutex> lock( m_mutex );
	freeUnlocked( pMemory );
}

void *T3DMemoryZone::reallocate( void *const pMemory, const size_t nBytes ) {
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( !pMemory ) return allocateUnlocked( nBytes, alignof( std::max_align_t ) );
	if ( nBytes == 0u ) { freeUnlocked( pMemory ); return nullptr; }
	AllocationHeader *const header = headerFor( pMemory );
	if ( !header || header->magic != ALLOCATION_MAGIC || m_state != ACTIVE ) return nullptr;
	const size_t old_bytes = header->requestedBytes;
	const size_t usable = header->pageIndex != UINT16_MAX ? SMALL_CLASSES[header->classIndex] :
		header->extentOffset + header->extentBytes - ( ( uint8_t * ) pMemory - m_pBase );
	if ( nBytes <= usable ) {
		header->requestedBytes = nBytes;
		m_nLiveBytes = m_nLiveBytes - old_bytes + nBytes;
		if ( header->pageIndex == UINT16_MAX && nBytes < usable ) {
			const size_t new_end = ( size_t ) ( ( uint8_t * ) pMemory - m_pBase ) + nBytes;
			const size_t old_end = header->extentOffset + header->extentBytes;
			if ( old_end > new_end ) { insertFreeExtentUnlocked( new_end, old_end - new_end ); header->extentBytes -= old_end - new_end; }
		}
		return pMemory;
	}
	if ( header->pageIndex == UINT16_MAX ) {
		const size_t old_end = header->extentOffset + header->extentBytes;
		const size_t needed = nBytes - usable;
		for ( uint32_t i = 0u; i < m_nFreeExtentCount; ++i ) {
			if ( m_szFreeExtents[i].offset == old_end && m_szFreeExtents[i].size >= needed ) {
				m_szFreeExtents[i].offset += needed; m_szFreeExtents[i].size -= needed;
				if ( m_szFreeExtents[i].size == 0u ) { for ( uint32_t m = i; m + 1u < m_nFreeExtentCount; ++m ) m_szFreeExtents[m] = m_szFreeExtents[m + 1u]; --m_nFreeExtentCount; }
				header->extentBytes += needed; header->requestedBytes = nBytes; m_nLiveBytes += nBytes - old_bytes; return pMemory;
			}
		}
	}
	void *const replacement = allocateUnlocked( nBytes, header->alignment );
	if ( !replacement ) return nullptr;
	std::memcpy( replacement, pMemory, std::min( old_bytes, nBytes ) );
	freeUnlocked( pMemory );
	return replacement;
}

bool T3DMemoryZone::beginDraining() { std::lock_guard<std::mutex> lock( m_mutex ); if ( m_state != ACTIVE ) return false; m_state = DRAINING; return true; }

bool T3DMemoryZone::waitUntilDrained( const std::chrono::milliseconds timeout ) {
	std::unique_lock<std::mutex> lock( m_mutex );
	if ( m_state != DRAINING ) return m_nLiveAllocations == 0u;
	return m_drained.wait_for( lock, timeout, [this]() { return m_nLiveAllocations == 0u; } );
}

bool T3DMemoryZone::release() {
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( m_state == UNINITIALIZED || m_state == DEAD ) return true;
	if ( m_nLiveAllocations != 0u ) { m_state = DRAINING; return false; }
	m_pBase = nullptr; m_nCapacity = 0u; m_nFreeExtentCount = 0u; m_state = DEAD; return true;
}

T3DMemoryZone::State T3DMemoryZone::state() const { std::lock_guard<std::mutex> lock( m_mutex ); return m_state; }
size_t T3DMemoryZone::liveAllocations() const { std::lock_guard<std::mutex> lock( m_mutex ); return m_nLiveAllocations; }
size_t T3DMemoryZone::liveBytes() const { std::lock_guard<std::mutex> lock( m_mutex ); return m_nLiveBytes; }
size_t T3DMemoryZone::freeBytes() const { std::lock_guard<std::mutex> lock( m_mutex ); size_t bytes = 0u; for ( uint32_t i = 0u; i < m_nFreeExtentCount; ++i ) bytes += m_szFreeExtents[i].size; return bytes; }
bool T3DMemoryZone::owns( const void *const pMemory ) const { return pMemory && m_pBase && static_cast<const uint8_t *>( pMemory ) > m_pBase && static_cast<const uint8_t *>( pMemory ) < m_pBase + m_nCapacity; }
