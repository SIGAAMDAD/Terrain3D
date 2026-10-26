// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_MEMORY_ZONE_H
#define TERRAIN3D_MEMORY_ZONE_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_sync.h"
#include "core/config/terrain_3d_compiler.h"

#include <stddef.h>
#include <stdint.h>

static_assert( sizeof( t3dMemoryZoneAllocationHeader_t ) % 16u == 0u,
			   "Zone payload headers must preserve 16-byte small-block alignment" );

class Terrain3DMemoryZone
{
public:
	Terrain3DMemoryZone() = default;
	~Terrain3DMemoryZone();
	Terrain3DMemoryZone( const Terrain3DMemoryZone & ) = delete;
	Terrain3DMemoryZone &operator=( const Terrain3DMemoryZone & ) = delete;

	bool  initialize( void *pBacking, size_t nBytes );
	void *allocate( size_t nBytes, size_t nAlignment = alignof( max_align_t ) );
	bool  canAllocate( size_t nBytes, size_t nAlignment = alignof( max_align_t ) ) const;
	void  deallocate( void *pMemory );
#if defined( TERRAIN3D_STORAGE_FAULT_TEST )
	void   testFailAllocationAt( size_t nCall );
	void   testClearAllocationFailure( void );
	size_t testAllocationCallCount( void ) const;
#endif
	EA_FORCE_INLINE bool beginDraining() {
		Terrain3DScopedLock lock( m_mutex );
		if ( m_state != T3D_MEMORY_ZONE_ACTIVE ) {
			return false;
		}
		m_state = T3D_MEMORY_ZONE_DRAINING;
		return true;
	}
	bool release();

	EA_FORCE_INLINE t3dMemoryZoneState_t state() const {
		Terrain3DScopedLock lock( m_mutex );
		return m_state;
	}
	EA_FORCE_INLINE size_t capacity() const {
		return m_nCapacity;
	}
	EA_FORCE_INLINE size_t liveAllocations() const {
		Terrain3DScopedLock lock( m_mutex );
		return m_nLiveAllocations;
	}
	EA_FORCE_INLINE size_t liveBytes() const {
		Terrain3DScopedLock lock( m_mutex );
		return m_nLiveBytes;
	}
	size_t				   freeExtentBytes( void ) const;
	EA_FORCE_INLINE size_t freeBytes(
		void ) const {
		return freeExtentBytes();
	}
	t3dMemoryZoneStats_t getStats( void ) const;
	EA_FORCE_INLINE bool owns(
		const void *pMemory ) const {
		if ( !pMemory || !m_pBase || m_nCapacity < sizeof( t3dMemoryZoneAllocationHeader_t ) ) {
			return false;
		}
		const uint8_t *const pBytes = ( const uint8_t * ) ( pMemory );
		return pBytes >= m_pBase + sizeof( t3dMemoryZoneAllocationHeader_t ) && pBytes < m_pBase + m_nCapacity;
	}

private:
	uint8_t					*m_pBase = NULL;
	size_t					 m_nCapacity = 0u;
	t3dMemoryZoneExtent_t	 m_szFreeExtents[T3D_MAX_EXTENTS] = {};
	uint32_t				 m_nFreeExtentCount = 0u;
	t3dMemoryZoneSmallPage_t m_szSmallPages[T3D_MAX_SMALL_PAGES] = {};
	size_t					 m_nLiveAllocations = 0u;
	size_t					 m_nLiveBytes = 0u;
	size_t					 m_nLivePhysicalBytes = 0u;
	size_t					 m_nLiveMetadataBytes = 0u;
	size_t					 m_nLiveAlignmentWasteBytes = 0u;
	size_t					 m_nLiveSizeClassWasteBytes = 0u;
	size_t					 m_nFailedAllocations = 0u;
#if defined( TERRAIN3D_STORAGE_FAULT_TEST )
	size_t m_nTestAllocationCallCount = 0u;
	size_t m_nTestFailAllocationAt = 0u;
#endif
	size_t				   m_nExtentTableSaturationEvents = 0u;
	size_t				   m_nLostFreePreventionEvents = 0u;
	uint32_t			   m_nActiveSmallPages = 0u;
	uint32_t			   m_nPeakActiveSmallPages = 0u;
	uint32_t			   m_nPeakFreeExtentCount = 0u;
	t3dMemoryZoneState_t   m_state = T3D_MEMORY_ZONE_UNINITIALIZED;
	mutable Terrain3DMutex m_mutex;

	int	  classIndexFor( size_t nBytes, size_t nAlignment ) const;
	bool  insertFreeExtentUnlocked( size_t nOffset, size_t nBytes );
	bool  canTakeExtentUnlocked( size_t nBytes, size_t nAlignment, size_t nPrefixBytes ) const;
	bool  takeExtentUnlocked( size_t  nBytes,
							  size_t  nAlignment,
							  size_t  nPrefixBytes,
							  size_t &rExtentOffset,
							  size_t &rUserOffset );
	void *allocateUnlocked( size_t nBytes, size_t nAlignment );
	void *allocateSmallUnlocked( int iClass, size_t nBytes );
	void *allocateLargeUnlocked( size_t nBytes, size_t nAlignment );
	bool  freeUnlocked( void *pMemory );
	t3dMemoryZoneAllocationHeader_t		  *headerFor( void *pMemory ) const;
	const t3dMemoryZoneAllocationHeader_t *headerFor( const void *pMemory ) const;
};

#endif // TERRAIN3D_MEMORY_ZONE_H
