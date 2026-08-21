// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_MEMORY_ZONE_H
#define TERRAIN3D_MEMORY_ZONE_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include "core/terrain_3d_compiler.h"

typedef struct t3dmemoryZoneStats_s {
	size_t nReservedBytes = 0u;
	size_t nLiveRequestedBytes = 0u;
	size_t nLivePhysicalBytes = 0u;
	size_t nLiveAllocationCount = 0u;
	size_t nExtentFreeBytes = 0u;
	size_t nSmallPageReservedBytes = 0u;
	size_t nSmallPageReusablePayloadBytes = 0u;
	size_t nSmallPageReusablePhysicalBytes = 0u;
	size_t nLargeAllocationPhysicalBytes = 0u;
	size_t nMetadataBytes = 0u;
	size_t nAlignmentWasteBytes = 0u;
	size_t nSizeClassWasteBytes = 0u;
	size_t nInternalFragmentationBytes = 0u;
	size_t nFailedAllocations = 0u;
	uint32_t nActiveSmallPages = 0u;
	uint32_t nPeakActiveSmallPages = 0u;
	uint32_t nFreeExtentCount = 0u;
	uint32_t nPeakFreeExtentCount = 0u;
} t3dmemoryZoneStats_t;

class T3DMemoryZone {
public:
	enum State : uint8_t { UNINITIALIZED, ACTIVE, DRAINING, DEAD };
	static constexpr uint32_t SMALL_CLASS_COUNT = 9u;
	static constexpr uint32_t SMALL_CLASSES[SMALL_CLASS_COUNT] = {16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u};
	static constexpr uint32_t SMALL_PAGE_BYTES = 64u * 1024u;
	static constexpr uint32_t MAX_EXTENTS = 8192u;
	static constexpr uint32_t MAX_SMALL_PAGES = 1024u;

	T3DMemoryZone() = default;
	EA_FORCE_INLINE ~T3DMemoryZone() { release(); }
	T3DMemoryZone( const T3DMemoryZone & ) = delete;
	T3DMemoryZone &operator=( const T3DMemoryZone & ) = delete;

	bool initialize( void *pBacking, size_t nBytes );
	void *allocate( size_t nBytes, size_t nAlignment = alignof( std::max_align_t ) );
	void deallocate( void *pMemory );
	EA_FORCE_INLINE bool beginDraining() { std::lock_guard<std::mutex> lock( m_mutex ); if ( m_state != ACTIVE ) { return false; } m_state = DRAINING; return true; }
	bool release();

	EA_FORCE_INLINE State state() const { std::lock_guard<std::mutex> lock( m_mutex ); return m_state; }
	EA_FORCE_INLINE size_t capacity() const { return m_nCapacity; }
	EA_FORCE_INLINE size_t liveAllocations() const { std::lock_guard<std::mutex> lock( m_mutex ); return m_nLiveAllocations; }
	EA_FORCE_INLINE size_t liveBytes() const { std::lock_guard<std::mutex> lock( m_mutex ); return m_nLiveBytes; }
	size_t freeExtentBytes( void ) const;
	EA_FORCE_INLINE size_t freeBytes( void ) const { return freeExtentBytes(); }
	t3dmemoryZoneStats_t getStats( void ) const;
	EA_FORCE_INLINE bool owns( const void *pMemory ) const { return pMemory && m_pBase && static_cast<const uint8_t *>( pMemory ) > m_pBase && static_cast<const uint8_t *>( pMemory ) < m_pBase + m_nCapacity; }

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
		size_t liveRequestedBytes = 0u;
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
	size_t m_nLivePhysicalBytes = 0u;
	size_t m_nLiveMetadataBytes = 0u;
	size_t m_nLiveAlignmentWasteBytes = 0u;
	size_t m_nLiveSizeClassWasteBytes = 0u;
	size_t m_nFailedAllocations = 0u;
	uint32_t m_nActiveSmallPages = 0u;
	uint32_t m_nPeakActiveSmallPages = 0u;
	uint32_t m_nPeakFreeExtentCount = 0u;
	State m_state = UNINITIALIZED;
	mutable std::mutex m_mutex;

	EA_FORCE_INLINE static bool isPowerOfTwo( size_t nValue ) { return nValue && ( nValue & ( nValue - 1u ) ) == 0u; }
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
