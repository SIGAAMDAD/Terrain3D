// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_MEMORY_ZONE_H
#define TERRAIN3D_MEMORY_ZONE_H

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <mutex>

class T3DMemoryZone {
public:
	enum State : uint8_t { UNINITIALIZED, ACTIVE, DRAINING, DEAD };
	static constexpr uint32_t SMALL_CLASS_COUNT = 9u;
	static constexpr uint32_t SMALL_CLASSES[SMALL_CLASS_COUNT] = {16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u};
	static constexpr uint32_t SMALL_PAGE_BYTES = 64u * 1024u;
	static constexpr uint32_t MAX_EXTENTS = 8192u;
	static constexpr uint32_t MAX_SMALL_PAGES = 1024u;

	T3DMemoryZone() = default;
	~T3DMemoryZone();
	T3DMemoryZone( const T3DMemoryZone & ) = delete;
	T3DMemoryZone &operator=( const T3DMemoryZone & ) = delete;

	bool initialize( void *pBacking, size_t nBytes );
	void *allocate( size_t nBytes, size_t nAlignment = alignof( std::max_align_t ) );
	void deallocate( void *pMemory );
	void *reallocate( void *pMemory, size_t nBytes );
	bool beginDraining();
	bool waitUntilDrained( std::chrono::milliseconds timeout );
	bool release();

	State state() const;
	size_t capacity() const { return m_nCapacity; }
	size_t liveAllocations() const;
	size_t liveBytes() const;
	size_t freeBytes() const;
	bool owns( const void *pMemory ) const;

private:
	struct Extent { size_t offset = 0u; size_t size = 0u; };
	struct SmallPage {
		size_t offset = 0u;
		uint32_t bytes = 0u;
		uint32_t blockStride = 0u;
		uint32_t firstFree = UINT32_MAX;
		uint16_t classIndex = UINT16_MAX;
		uint16_t liveBlocks = 0u;
		uint16_t totalBlocks = 0u;
		bool active = false;
	};
	struct AllocationHeader {
		uint64_t magic = 0u;
		size_t extentOffset = 0u;
		size_t extentBytes = 0u;
		size_t requestedBytes = 0u;
		uint32_t alignment = 0u;
		uint16_t pageIndex = UINT16_MAX;
		uint16_t classIndex = UINT16_MAX;
		uint64_t reserved = 0u;
	};
	static_assert( sizeof( AllocationHeader ) % 16u == 0u, "Zone payload headers must preserve 16-byte small-block alignment" );

	static constexpr uint64_t ALLOCATION_MAGIC = 0x5A4F4E453344544FULL;
	uint8_t *m_pBase = nullptr;
	size_t m_nCapacity = 0u;
	Extent m_szFreeExtents[MAX_EXTENTS] = {};
	uint32_t m_nFreeExtentCount = 0u;
	SmallPage m_szSmallPages[MAX_SMALL_PAGES] = {};
	size_t m_nLiveAllocations = 0u;
	size_t m_nLiveBytes = 0u;
	State m_state = UNINITIALIZED;
	mutable std::mutex m_mutex;
	std::condition_variable m_drained;

	static bool isPowerOfTwo( size_t nValue );
	static bool alignUp( size_t nValue, size_t nAlignment, size_t &rValue );
	int classIndexFor( size_t nBytes, size_t nAlignment ) const;
	bool insertFreeExtentUnlocked( size_t nOffset, size_t nBytes );
	bool takeExtentUnlocked( size_t nBytes, size_t nAlignment, size_t nPrefixBytes,
		size_t &rExtentOffset, size_t &rUserOffset );
	void *allocateUnlocked( size_t nBytes, size_t nAlignment );
	void *allocateSmallUnlocked( int nClassIndex, size_t nBytes );
	void *allocateLargeUnlocked( size_t nBytes, size_t nAlignment );
	void freeUnlocked( void *pMemory );
	AllocationHeader *headerFor( void *pMemory ) const;
	const AllocationHeader *headerFor( const void *pMemory ) const;
};

#endif
