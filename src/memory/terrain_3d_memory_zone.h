// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_MEMORY_ZONE_H
#define TERRAIN3D_MEMORY_ZONE_H

#include "core/terrain_3d_compiler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>

typedef struct t3dMemoryZoneStats_s {
	size_t m_nReservedBytes = 0u;
	size_t m_nLiveRequestedBytes = 0u;
	size_t m_nLivePhysicalBytes = 0u;
	size_t m_nLiveAllocationCount = 0u;
	size_t m_nExtentFreeBytes = 0u;
	size_t m_nSmallPageReservedBytes = 0u;
	size_t m_nSmallPageReusablePayloadBytes = 0u;
	size_t m_nSmallPageReusablePhysicalBytes = 0u;
	size_t m_nLargeAllocationPhysicalBytes = 0u;
	size_t m_nMetadataBytes = 0u;
	size_t m_nAlignmentWasteBytes = 0u;
	size_t m_nSizeClassWasteBytes = 0u;
	size_t m_nInternalFragmentationBytes = 0u;
	size_t m_nFailedAllocations = 0u;
	uint32_t m_nActiveSmallPages = 0u;
	uint32_t m_nPeakActiveSmallPages = 0u;
	uint32_t m_nFreeExtentCount = 0u;
	uint32_t m_nPeakFreeExtentCount = 0u;
} t3dMemoryZoneStats_t;

typedef enum t3dMemoryZoneState_e : uint8_t {
	T3D_MEMORY_ZONE_UNINITIALIZED,
	T3D_MEMORY_ZONE_ACTIVE,
	T3D_MEMORY_ZONE_DRAINING,
	T3D_MEMORY_ZONE_DEAD,
} t3dMemoryZoneState_t;

typedef struct t3dMemoryZoneExtent_s {
	size_t m_iOffset = 0u;
	size_t m_nSize = 0u;
} t3dMemoryZoneExtent_t;

typedef struct t3dMemoryZoneSmallPage_s {
	size_t m_iOffset = 0u;
	uint32_t m_nBytes = 0u;
	uint32_t m_nBlockStride = 0u;
	uint32_t m_iFirstFree = UINT32_MAX;
	uint16_t m_iClass = UINT16_MAX;
	uint16_t m_nLiveBlocks = 0u;
	uint16_t m_nTotalBlocks = 0u;
	size_t m_nLiveRequestedBytes = 0u;
	bool m_bActive = false;
} t3dMemoryZoneSmallPage_t;

typedef struct t3dMemoryZoneAllocationHeader_s {
	uint64_t m_nMagic = 0u;
	size_t m_iExtentOffset = 0u;
	size_t m_nExtentBytes = 0u;
	size_t m_nRequestedBytes = 0u;
	uint32_t m_nAlignment = 0u;
	uint16_t m_iPage = UINT16_MAX;
	uint16_t m_iClass = UINT16_MAX;
	uint64_t m_nReserved = 0u;
} t3dMemoryZoneAllocationHeader_t;

static_assert( sizeof( t3dMemoryZoneAllocationHeader_t ) % 16u == 0u,
		"Zone payload headers must preserve 16-byte small-block alignment" );

class Terrain3DMemoryZone {
public:
	static constexpr uint32_t s_nSmallClassCount = 9u;
	static constexpr uint32_t
			s_szSmallClasses[s_nSmallClassCount] = { 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u };
	static constexpr uint32_t s_nSmallPageBytes = 64u * 1024u;
	static constexpr uint32_t s_nMaxExtents = 8192u;
	static constexpr uint32_t s_nMaxSmallPages = 1024u;
	static constexpr uint64_t s_nAllocationMagic = 0x5A4F4E453344544FULL;

	Terrain3DMemoryZone() = default;
	EA_FORCE_INLINE ~Terrain3DMemoryZone() {
		release();
	}
	Terrain3DMemoryZone( const Terrain3DMemoryZone & ) = delete;
	Terrain3DMemoryZone &operator=( const Terrain3DMemoryZone & ) = delete;

	bool initialize( void *pBacking, size_t nBytes );
	void *allocate( size_t nBytes, size_t nAlignment = alignof( std::max_align_t ) );
	void deallocate( void *pMemory );
	EA_FORCE_INLINE bool beginDraining() {
		std::lock_guard<std::mutex> lock( m_mutex );
		if ( m_state != T3D_MEMORY_ZONE_ACTIVE ) {
			return false;
		}
		m_state = T3D_MEMORY_ZONE_DRAINING;
		return true;
	}
	bool release();

	EA_FORCE_INLINE t3dMemoryZoneState_t state() const {
		std::lock_guard<std::mutex> lock( m_mutex );
		return m_state;
	}
	EA_FORCE_INLINE size_t capacity() const {
		return m_nCapacity;
	}
	EA_FORCE_INLINE size_t liveAllocations() const {
		std::lock_guard<std::mutex> lock( m_mutex );
		return m_nLiveAllocations;
	}
	EA_FORCE_INLINE size_t liveBytes() const {
		std::lock_guard<std::mutex> lock( m_mutex );
		return m_nLiveBytes;
	}
	size_t freeExtentBytes( void ) const;
	EA_FORCE_INLINE size_t freeBytes( void ) const {
		return freeExtentBytes();
	}
	t3dMemoryZoneStats_t getStats( void ) const;
	EA_FORCE_INLINE bool owns( const void *pMemory ) const {
		return pMemory && m_pBase && static_cast<const uint8_t *>( pMemory ) > m_pBase &&
				static_cast<const uint8_t *>( pMemory ) < m_pBase + m_nCapacity;
	}

private:
	uint8_t *m_pBase = nullptr;
	size_t m_nCapacity = 0u;
	t3dMemoryZoneExtent_t m_szFreeExtents[s_nMaxExtents] = {};
	uint32_t m_nFreeExtentCount = 0u;
	t3dMemoryZoneSmallPage_t m_szSmallPages[s_nMaxSmallPages] = {};
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
	t3dMemoryZoneState_t m_state = T3D_MEMORY_ZONE_UNINITIALIZED;
	mutable std::mutex m_mutex;

	EA_FORCE_INLINE static bool isPowerOfTwo( size_t nValue ) {
		return nValue && ( nValue & ( nValue - 1u ) ) == 0u;
	}
	static bool alignUp( size_t nValue, size_t nAlignment, size_t &rValue );
	int classIndexFor( size_t nBytes, size_t nAlignment ) const;
	bool insertFreeExtentUnlocked( size_t nOffset, size_t nBytes );
	bool takeExtentUnlocked( size_t nBytes,
			size_t nAlignment,
			size_t nPrefixBytes,
			size_t &rExtentOffset,
			size_t &rUserOffset );
	void *allocateUnlocked( size_t nBytes, size_t nAlignment );
	void *allocateSmallUnlocked( int iClass, size_t nBytes );
	void *allocateLargeUnlocked( size_t nBytes, size_t nAlignment );
	void freeUnlocked( void *pMemory );
	t3dMemoryZoneAllocationHeader_t *headerFor( void *pMemory ) const;
	const t3dMemoryZoneAllocationHeader_t *headerFor( const void *pMemory ) const;
};

#endif
