// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_SLAB_ARRAY_H
#define TERRAIN3D_SLAB_ARRAY_H

#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_allocation_domain.h"
#include "eastl_config.h"

#include <EASTL/type_traits.h>
#include <EASTL/numeric_limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

// High-water contiguous storage shared by many small logical arrays. The slab
// owns its default backing through Terrain3DEastlAllocator; bind() remains for
// explicit parent-carved layouts. Ordinary logical arrays hold only
// offset/count/capacity tuples and never allocate individually.
template <typename T>
class Terrain3DHighWaterSlab {
	static_assert( eastl::is_trivially_copyable_v<T>, "Runtime slab storage requires trivially copyable values." );

	Terrain3DEastlAllocator m_allocator;
	t3dNativeAllocationDomain_t m_eAllocationDomain = TERRAIN3D_NATIVE_ALLOCATION_PERSISTENT_RESOURCE;
	T *m_pData = NULL;
	size_t m_nCapacity = 0u;
	size_t m_nUsed = 0u;
	size_t m_nGrowCount = 0u;
	bool m_bSealed = false;
	bool m_bOwnsData = false;
	uint64_t m_nGrowthRequests = 0u;
	uint64_t m_nGrowthDenied = 0u;
	uint64_t m_nCapacityMisses = 0u;

	bool reallocateCold( const size_t nCapacity ) {
		if ( nCapacity <= m_nCapacity ) {
			return true;
		}
		++m_nGrowthRequests;
		if ( m_bSealed || nCapacity > SIZE_MAX / sizeof( T ) ) {
			++m_nGrowthDenied;
			return false;
		}
		if ( m_eAllocationDomain == TERRAIN3D_NATIVE_ALLOCATION_RUNTIME_ZONE_REQUIRED && m_allocator.zone() == NULL ) {
			++m_nGrowthDenied;
			return false;
		}
		// External parent storage has an explicit fixed extent. Growing it here
		// would silently change ownership, so fail closed instead.
		if ( m_pData && !m_bOwnsData ) {
			return false;
		}
		T *const pReplacement = ( T * )m_allocator.allocate( nCapacity * sizeof( T ), alignof( T ), 0u );
		if ( !pReplacement ) {
			return false;
		}
		if ( m_nUsed > 0u && m_pData ) {
			memcpy( pReplacement, m_pData, m_nUsed * sizeof( T ) );
		}
		if ( m_pData && m_bOwnsData ) {
			m_allocator.deallocate( m_pData );
		}
		m_pData = pReplacement;
		m_nCapacity = nCapacity;
		m_bOwnsData = true;
		++m_nGrowCount;
		return true;
	}

public:
	Terrain3DHighWaterSlab( void ) = default;
	explicit Terrain3DHighWaterSlab( const Terrain3DEastlAllocator &rAllocator ) : m_allocator( rAllocator ) {
	}
	~Terrain3DHighWaterSlab( void ) {
		release();
	}
	Terrain3DHighWaterSlab( const Terrain3DHighWaterSlab & ) = delete;
	Terrain3DHighWaterSlab &operator=( const Terrain3DHighWaterSlab & ) = delete;
	Terrain3DHighWaterSlab( Terrain3DHighWaterSlab &&rOther ) noexcept :
	        m_allocator( eastl::move( rOther.m_allocator ) ),
	        m_eAllocationDomain( rOther.m_eAllocationDomain ),
	        m_pData( rOther.m_pData ),
	        m_nCapacity( rOther.m_nCapacity ),
	        m_nUsed( rOther.m_nUsed ),
	        m_nGrowCount( rOther.m_nGrowCount ),
	        m_bSealed( rOther.m_bSealed ),
	        m_bOwnsData( rOther.m_bOwnsData ),
	        m_nGrowthRequests( rOther.m_nGrowthRequests ),
	        m_nGrowthDenied( rOther.m_nGrowthDenied ),
	        m_nCapacityMisses( rOther.m_nCapacityMisses ) {
		rOther.m_eAllocationDomain = TERRAIN3D_NATIVE_ALLOCATION_PERSISTENT_RESOURCE;
		rOther.m_pData = NULL;
		rOther.m_nCapacity = 0u;
		rOther.m_nUsed = 0u;
		rOther.m_nGrowCount = 0u;
		rOther.m_bSealed = false;
		rOther.m_bOwnsData = false;
		rOther.m_nGrowthRequests = 0u;
		rOther.m_nGrowthDenied = 0u;
		rOther.m_nCapacityMisses = 0u;
	}
	Terrain3DHighWaterSlab &operator=( Terrain3DHighWaterSlab &&rOther ) noexcept {
		if ( this != &rOther ) {
			release();
			m_allocator = eastl::move( rOther.m_allocator );
			m_eAllocationDomain = rOther.m_eAllocationDomain;
			m_pData = rOther.m_pData;
			m_nCapacity = rOther.m_nCapacity;
			m_nUsed = rOther.m_nUsed;
			m_nGrowCount = rOther.m_nGrowCount;
			m_bSealed = rOther.m_bSealed;
			m_bOwnsData = rOther.m_bOwnsData;
			m_nGrowthRequests = rOther.m_nGrowthRequests;
			m_nGrowthDenied = rOther.m_nGrowthDenied;
			m_nCapacityMisses = rOther.m_nCapacityMisses;
			rOther.m_eAllocationDomain = TERRAIN3D_NATIVE_ALLOCATION_PERSISTENT_RESOURCE;
			rOther.m_pData = NULL;
			rOther.m_nCapacity = 0u;
			rOther.m_nUsed = 0u;
			rOther.m_nGrowCount = 0u;
			rOther.m_bSealed = false;
			rOther.m_bOwnsData = false;
			rOther.m_nGrowthRequests = 0u;
			rOther.m_nGrowthDenied = 0u;
			rOther.m_nCapacityMisses = 0u;
		}
		return *this;
	}


	EA_FORCE_INLINE bool requireRuntimeZone( void ) {
		if ( m_pData || m_nCapacity != 0u ) {
			return m_eAllocationDomain == TERRAIN3D_NATIVE_ALLOCATION_RUNTIME_ZONE_REQUIRED && m_allocator.zone() != NULL;
		}
		m_eAllocationDomain = TERRAIN3D_NATIVE_ALLOCATION_RUNTIME_ZONE_REQUIRED;
		return true;
	}
	EA_FORCE_INLINE bool canBindAllocatorZone( Terrain3DMemoryZone *const pZone ) const {
		if ( !pZone ) {
			return false;
		}
		if ( !m_pData && m_nCapacity == 0u ) {
			return true;
		}
		return m_eAllocationDomain == TERRAIN3D_NATIVE_ALLOCATION_RUNTIME_ZONE_REQUIRED && m_allocator.zone() == pZone;
	}
	EA_FORCE_INLINE bool bindAllocatorZone(
	        Terrain3DMemoryZone *const pZone, const t3dAllocatorOverflowPolicy_t eOverflowPolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT ) {
		if ( m_pData || m_nCapacity != 0u ) {
			return m_allocator.zone() == pZone;
		}
		if ( !pZone ) {
			return false;
		}
		m_eAllocationDomain = TERRAIN3D_NATIVE_ALLOCATION_RUNTIME_ZONE_REQUIRED;
		m_allocator = Terrain3DEastlAllocator::zoneAllocator( pZone, eOverflowPolicy );
		return true;
	}
	EA_FORCE_INLINE bool bind( T *const pData, const size_t nCapacity, const size_t nUsed = 0u ) {
		if ( nUsed > nCapacity || ( nCapacity > 0u && !pData ) ) {
			return false;
		}
		release();
		m_eAllocationDomain = TERRAIN3D_NATIVE_ALLOCATION_PARENT_BOUND;
		m_pData = pData;
		m_nCapacity = nCapacity;
		m_nUsed = nUsed;
		m_nGrowCount = 0u;
		m_bSealed = false;
		m_bOwnsData = false;
		return true;
	}

	EA_FORCE_INLINE bool reserveCold( const size_t nCount ) {
		return nCount <= m_nCapacity ? true : reallocateCold( nCount );
	}

	// Backing growth is owner-controlled. Logical suballocation may use existing
	// capacity while sealed, but it never implicitly unseals the backing store.
	EA_FORCE_INLINE bool reserve( const size_t nCount ) {
		return nCount <= m_nCapacity ? true : reserveCold( nCount );
	}

	EA_FORCE_INLINE bool seal( void ) {
		if ( m_eAllocationDomain == TERRAIN3D_NATIVE_ALLOCATION_RUNTIME_ZONE_REQUIRED && m_allocator.zone() == NULL && m_pData == NULL ) {
			++m_nGrowthDenied;
			return false;
		}
		m_bSealed = true;
		return true;
	}

	EA_FORCE_INLINE void beginColdRebuild( void ) {
		m_bSealed = false;
		m_nUsed = 0u;
	}

	EA_FORCE_INLINE bool sealed( void ) const {
		return m_bSealed;
	}

	EA_FORCE_INLINE size_t allocate( const size_t nCount ) {
		if ( nCount == 0u || nCount > eastl::numeric_limits<size_t>::max() - m_nUsed ) {
			return SIZE_MAX;
		}
		const size_t offset = m_nUsed;
		const size_t required = offset + nCount;
		if ( required > m_nCapacity && !reserve( required ) ) {
			++m_nCapacityMisses;
			return SIZE_MAX;
		}
		m_nUsed = required;
		return offset;
	}

	EA_FORCE_INLINE void reset( void ) {
		m_nUsed = 0u;
	}
	EA_FORCE_INLINE void release( void ) {
		if ( m_pData && m_bOwnsData ) {
			m_allocator.deallocate( m_pData );
		}
		m_pData = NULL;
		m_nCapacity = 0u;
		m_nUsed = 0u;
		m_nGrowCount = 0u;
		m_bSealed = false;
		m_bOwnsData = false;
		m_nGrowthRequests = 0u;
		m_nGrowthDenied = 0u;
		m_nCapacityMisses = 0u;
	}
	EA_FORCE_INLINE bool compactBacking( void ) {
		return m_nUsed == m_nCapacity;
	}
	EA_FORCE_INLINE T *data( void ) {
		return m_pData;
	}
	EA_FORCE_INLINE const T *data( void ) const {
		return m_pData;
	}
	EA_FORCE_INLINE size_t used( void ) const {
		return m_nUsed;
	}
	EA_FORCE_INLINE size_t size( void ) const {
		return m_nCapacity;
	}
	EA_FORCE_INLINE size_t capacity( void ) const {
		return m_nCapacity;
	}
	EA_FORCE_INLINE size_t growCount( void ) const {
		return m_nGrowCount;
	}
	EA_FORCE_INLINE t3dNativeAllocationDomain_t allocationDomain( void ) const {
		return m_eAllocationDomain;
	}
	EA_FORCE_INLINE bool runtimeBindingSatisfied( void ) const {
		return m_eAllocationDomain != TERRAIN3D_NATIVE_ALLOCATION_RUNTIME_ZONE_REQUIRED || m_allocator.zone() != NULL
		        || ( m_pData != NULL && !m_bOwnsData );
	}
	EA_FORCE_INLINE bool ownsBacking( void ) const {
		return m_bOwnsData;
	}
	EA_FORCE_INLINE size_t residentBytes( void ) const {
		return m_nCapacity * sizeof( T );
	}
	EA_FORCE_INLINE uint64_t growthRequests( void ) const {
		return m_nGrowthRequests;
	}
	EA_FORCE_INLINE uint64_t growthDenied( void ) const {
		return m_nGrowthDenied;
	}
	EA_FORCE_INLINE uint64_t capacityMisses( void ) const {
		return m_nCapacityMisses;
	}
	EA_FORCE_INLINE void swap( Terrain3DHighWaterSlab<T> &rOther ) {
		using eastl::swap;
		swap( m_allocator, rOther.m_allocator );
		swap( m_eAllocationDomain, rOther.m_eAllocationDomain );
		swap( m_pData, rOther.m_pData );
		swap( m_nCapacity, rOther.m_nCapacity );
		swap( m_nUsed, rOther.m_nUsed );
		swap( m_nGrowCount, rOther.m_nGrowCount );
		swap( m_bSealed, rOther.m_bSealed );
		swap( m_bOwnsData, rOther.m_bOwnsData );
		swap( m_nGrowthRequests, rOther.m_nGrowthRequests );
		swap( m_nGrowthDenied, rOther.m_nGrowthDenied );
		swap( m_nCapacityMisses, rOther.m_nCapacityMisses );
	}
};

// Non-owning logical array backed by a Terrain3DHighWaterSlab. Growth relocates the
// logical array into a new high-water segment and copies its live elements.
// Old segments are reclaimed wholesale by owner reset/compaction; normal
// runtime mutation never performs per-cell frees.
template <typename T>
class Terrain3DSlabArray {
	Terrain3DHighWaterSlab<T> *m_pSlab = NULL;
	size_t m_iOffset = 0u;
	size_t m_nCount = 0u;
	size_t m_nCapacity = 0u;

	EA_FORCE_INLINE size_t growthCapacity( const size_t nRequired ) const {
		if ( nRequired <= m_nCapacity ) {
			return m_nCapacity;
		}
		const size_t growth = MAX( 32u, m_nCapacity / 2u );
		return MAX( nRequired, m_nCapacity + growth );
	}

public:
	Terrain3DSlabArray( void ) = default;
	Terrain3DSlabArray( const Terrain3DSlabArray & ) = delete;
	Terrain3DSlabArray &operator=( const Terrain3DSlabArray & ) = delete;
	EA_FORCE_INLINE Terrain3DSlabArray( Terrain3DSlabArray &&rOther ) noexcept :
	        m_pSlab( rOther.m_pSlab ), m_iOffset( rOther.m_iOffset ), m_nCount( rOther.m_nCount ), m_nCapacity( rOther.m_nCapacity ) {
		rOther.m_pSlab = NULL;
		rOther.m_iOffset = 0u;
		rOther.m_nCount = 0u;
		rOther.m_nCapacity = 0u;
	}
	EA_FORCE_INLINE Terrain3DSlabArray &operator=( Terrain3DSlabArray &&rOther ) noexcept {
		if ( this != &rOther ) {
			m_pSlab = rOther.m_pSlab;
			m_iOffset = rOther.m_iOffset;
			m_nCount = rOther.m_nCount;
			m_nCapacity = rOther.m_nCapacity;
			rOther.m_pSlab = NULL;
			rOther.m_iOffset = 0u;
			rOther.m_nCount = 0u;
			rOther.m_nCapacity = 0u;
		}
		return *this;
	}

	EA_FORCE_INLINE void bind( Terrain3DHighWaterSlab<T> *const pSlab ) {
		m_pSlab = pSlab;
		m_iOffset = 0u;
		m_nCount = 0u;
		m_nCapacity = 0u;
	}
	EA_FORCE_INLINE void rebind( Terrain3DHighWaterSlab<T> *const pSlab ) {
		m_pSlab = pSlab;
	}
	EA_FORCE_INLINE bool isBound( void ) const {
		return m_pSlab != NULL;
	}

	bool reserve( const size_t nCount ) {
		if ( nCount <= m_nCapacity ) {
			return true;
		}
		if ( !m_pSlab ) {
			return false;
		}
		const size_t new_capacity = growthCapacity( nCount );
		const size_t old_offset = m_iOffset;
		const size_t old_count = m_nCount;
		const size_t new_offset = m_pSlab->allocate( new_capacity );
		if ( new_offset == SIZE_MAX ) {
			return false;
		}
		T *const pData = m_pSlab->data();
		if ( old_count > 0u ) {
			memmove( pData + new_offset, pData + old_offset, old_count * sizeof( T ) );
		}
		m_iOffset = new_offset;
		m_nCapacity = new_capacity;
		return true;
	}

	bool reserveExact( const size_t nCount ) {
		if ( nCount <= m_nCapacity ) {
			return true;
		}
		if ( !m_pSlab ) {
			return false;
		}
		const size_t old_offset = m_iOffset;
		const size_t old_count = m_nCount;
		const size_t new_offset = m_pSlab->allocate( nCount );
		if ( new_offset == SIZE_MAX ) {
			return false;
		}
		T *const pData = m_pSlab->data();
		if ( old_count > 0u ) {
			memmove( pData + new_offset, pData + old_offset, old_count * sizeof( T ) );
		}
		m_iOffset = new_offset;
		m_nCapacity = nCount;
		return true;
	}

	bool resizeUninitialized( const size_t nCount ) {
		if ( !reserve( nCount ) ) {
			return false;
		}
		m_nCount = nCount;
		return true;
	}

	bool resizeZeroed( const size_t nCount ) {
		const size_t old_count = m_nCount;
		if ( !reserve( nCount ) ) {
			return false;
		}
		if ( nCount > old_count ) {
			memset( data() + old_count, 0, ( nCount - old_count ) * sizeof( T ) );
		}
		m_nCount = nCount;
		return true;
	}

	EA_FORCE_INLINE bool resize( const size_t nCount ) {
		return resizeZeroed( nCount );
	}

	bool resize( const size_t nCount, const T &rValue ) {
		const size_t old_count = m_nCount;
		if ( !reserve( nCount ) ) {
			return false;
		}
		for ( size_t index = old_count; index < nCount; ++index ) {
			data()[index] = rValue;
		}
		m_nCount = nCount;
		return true;
	}

	EA_FORCE_INLINE void clear( void ) {
		m_nCount = 0u;
	}
	EA_FORCE_INLINE bool pushBack( const T &rValue ) {
		const size_t index = m_nCount;
		if ( !reserve( index + 1u ) ) {
			return false;
		}
		data()[index] = rValue;
		m_nCount = index + 1u;
		return true;
	}
	EA_FORCE_INLINE bool pushBack( T &&rValue ) {
		const size_t index = m_nCount;
		if ( !reserve( index + 1u ) ) {
			return false;
		}
		data()[index] = eastl::move( rValue );
		m_nCount = index + 1u;
		return true;
	}
	EA_FORCE_INLINE size_t size( void ) const {
		return m_nCount;
	}
	EA_FORCE_INLINE size_t capacity( void ) const {
		return m_nCapacity;
	}
	EA_FORCE_INLINE bool empty( void ) const {
		return m_nCount == 0u;
	}
	EA_FORCE_INLINE T *data( void ) {
		return m_pSlab && m_nCapacity > 0u ? m_pSlab->data() + m_iOffset : NULL;
	}
	EA_FORCE_INLINE const T *data( void ) const {
		return m_pSlab && m_nCapacity > 0u ? m_pSlab->data() + m_iOffset : NULL;
	}
	EA_FORCE_INLINE T &operator[]( const size_t nIndex ) {
		return data()[nIndex];
	}
	EA_FORCE_INLINE const T &operator[]( const size_t nIndex ) const {
		return data()[nIndex];
	}
	EA_FORCE_INLINE T *begin( void ) {
		return data();
	}
	EA_FORCE_INLINE const T *begin( void ) const {
		return data();
	}
	EA_FORCE_INLINE T *end( void ) {
		return data() ? data() + m_nCount : NULL;
	}
	EA_FORCE_INLINE const T *end( void ) const {
		return data() ? data() + m_nCount : NULL;
	}
	EA_FORCE_INLINE size_t offset( void ) const {
		return m_iOffset;
	}

	bool relocateExact( Terrain3DHighWaterSlab<T> *const pDestination ) {
		if ( !pDestination ) {
			return false;
		}
		if ( m_nCount == 0u ) {
			bind( pDestination );
			return true;
		}
		const T *const pSource = data();
		const size_t count = m_nCount;
		const size_t offset = pDestination->allocate( count );
		if ( offset == SIZE_MAX ) {
			return false;
		}
		T *const pDestinationData = pDestination->data();
		memcpy( pDestinationData + offset, pSource, count * sizeof( T ) );
		m_pSlab = pDestination;
		m_iOffset = offset;
		m_nCount = count;
		m_nCapacity = count;
		return true;
	}
};

#endif // TERRAIN3D_SLAB_ARRAY_H
