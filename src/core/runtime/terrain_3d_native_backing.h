// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_NATIVE_BACKING_H
#define TERRAIN3D_NATIVE_BACKING_H

#include "core/runtime/terrain_3d_allocation_domain.h"
#include "core/config/terrain_3d_compiler.h"
#include "eastl_config.h"

#include <EASTL/type_traits.h>
#include <EASTL/utility.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Shared ownership/lifetime primitive for trivially-copyable native backing
// stores. Containers retain their own logical count/suballocation policy while
// this type owns allocator binding, backing growth, sealing and telemetry.
template <typename T>
class Terrain3DNativeBacking {
	static_assert( eastl::is_trivially_copyable_v<T>, "Terrain3DNativeBacking requires trivially copyable values." );

	Terrain3DEastlAllocator m_allocator;
	t3dNativeAllocationDomain_t m_eAllocationDomain = TERRAIN3D_NATIVE_ALLOCATION_PERSISTENT_RESOURCE;
	T *m_pData = NULL;
	size_t m_nCapacity = 0u;
	bool m_bSealed = false;
	bool m_bOwnsData = false;
	uint64_t m_nGrowthRequests = 0u;
	uint64_t m_nGrowthDenied = 0u;
	uint64_t m_nCapacityMisses = 0u;
	size_t m_nGrowCount = 0u;

	EA_FORCE_INLINE void resetMovedFrom( void ) {
		m_eAllocationDomain = TERRAIN3D_NATIVE_ALLOCATION_PERSISTENT_RESOURCE;
		m_pData = NULL;
		m_nCapacity = 0u;
		m_bSealed = false;
		m_bOwnsData = false;
		m_nGrowthRequests = 0u;
		m_nGrowthDenied = 0u;
		m_nCapacityMisses = 0u;
		m_nGrowCount = 0u;
	}

public:
	Terrain3DNativeBacking( void ) = default;
	explicit Terrain3DNativeBacking( const Terrain3DEastlAllocator &rAllocator ) : m_allocator( rAllocator ) {
	}
	~Terrain3DNativeBacking( void ) {
		release();
	}
	Terrain3DNativeBacking( const Terrain3DNativeBacking & ) = delete;
	Terrain3DNativeBacking &operator=( const Terrain3DNativeBacking & ) = delete;
	Terrain3DNativeBacking( Terrain3DNativeBacking &&rOther ) noexcept :
	        m_allocator( eastl::move( rOther.m_allocator ) ),
	        m_eAllocationDomain( rOther.m_eAllocationDomain ),
	        m_pData( rOther.m_pData ),
	        m_nCapacity( rOther.m_nCapacity ),
	        m_bSealed( rOther.m_bSealed ),
	        m_bOwnsData( rOther.m_bOwnsData ),
	        m_nGrowthRequests( rOther.m_nGrowthRequests ),
	        m_nGrowthDenied( rOther.m_nGrowthDenied ),
	        m_nCapacityMisses( rOther.m_nCapacityMisses ),
	        m_nGrowCount( rOther.m_nGrowCount ) {
		rOther.resetMovedFrom();
	}
	Terrain3DNativeBacking &operator=( Terrain3DNativeBacking &&rOther ) noexcept {
		if ( this != &rOther ) {
			release();
			m_allocator = eastl::move( rOther.m_allocator );
			m_eAllocationDomain = rOther.m_eAllocationDomain;
			m_pData = rOther.m_pData;
			m_nCapacity = rOther.m_nCapacity;
			m_bSealed = rOther.m_bSealed;
			m_bOwnsData = rOther.m_bOwnsData;
			m_nGrowthRequests = rOther.m_nGrowthRequests;
			m_nGrowthDenied = rOther.m_nGrowthDenied;
			m_nCapacityMisses = rOther.m_nCapacityMisses;
			m_nGrowCount = rOther.m_nGrowCount;
			rOther.resetMovedFrom();
		}
		return *this;
	}

	bool growCold( const size_t nCapacity, const size_t nCopyCount, const bool bPreflightAllocation ) {
		if ( nCapacity <= m_nCapacity ) {
			return true;
		}
		++m_nGrowthRequests;
		if ( m_bSealed || nCopyCount > m_nCapacity || nCapacity > SIZE_MAX / sizeof( T ) ) {
			++m_nGrowthDenied;
			return false;
		}
		if ( m_eAllocationDomain == TERRAIN3D_NATIVE_ALLOCATION_RUNTIME_ZONE_REQUIRED && m_allocator.zone() == NULL ) {
			++m_nGrowthDenied;
			return false;
		}
		// Parent-bound storage has a fixed external extent. Growing it would
		// silently transfer ownership, so fail closed.
		if ( m_pData && !m_bOwnsData ) {
			return false;
		}
		const size_t nBytes = nCapacity * sizeof( T );
		if ( bPreflightAllocation && !m_allocator.canAllocate( nBytes, alignof( T ), 0u ) ) {
			return false;
		}
		T *const pReplacement = ( T * )m_allocator.allocate( nBytes, alignof( T ), 0u );
		if ( !pReplacement ) {
			return false;
		}
		if ( nCopyCount > 0u && m_pData ) {
			memcpy( pReplacement, m_pData, nCopyCount * sizeof( T ) );
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
	EA_FORCE_INLINE bool bindExternal( T *const pData, const size_t nCapacity, const bool bSealed ) {
		if ( nCapacity > 0u && !pData ) {
			return false;
		}
		release();
		m_eAllocationDomain = TERRAIN3D_NATIVE_ALLOCATION_PARENT_BOUND;
		m_pData = pData;
		m_nCapacity = nCapacity;
		m_bSealed = bSealed;
		m_bOwnsData = false;
		return true;
	}
	EA_FORCE_INLINE bool seal( void ) {
		if ( m_eAllocationDomain == TERRAIN3D_NATIVE_ALLOCATION_RUNTIME_ZONE_REQUIRED && m_allocator.zone() == NULL && m_pData == NULL ) {
			++m_nGrowthDenied;
			return false;
		}
		m_bSealed = true;
		return true;
	}
	EA_FORCE_INLINE void unsealCold( void ) {
		m_bSealed = false;
	}
	EA_FORCE_INLINE bool sealed( void ) const {
		return m_bSealed;
	}
	EA_FORCE_INLINE void release( void ) {
		if ( m_pData && m_bOwnsData ) {
			m_allocator.deallocate( m_pData );
		}
		m_pData = NULL;
		m_nCapacity = 0u;
		m_bSealed = false;
		m_bOwnsData = false;
		m_nGrowthRequests = 0u;
		m_nGrowthDenied = 0u;
		m_nCapacityMisses = 0u;
		m_nGrowCount = 0u;
	}
	EA_FORCE_INLINE void recordCapacityMiss( void ) {
		++m_nCapacityMisses;
	}
	EA_FORCE_INLINE T *data( void ) {
		return m_pData;
	}
	EA_FORCE_INLINE const T *data( void ) const {
		return m_pData;
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
	EA_FORCE_INLINE void swap( Terrain3DNativeBacking<T> &rOther ) {
		using eastl::swap;
		swap( m_allocator, rOther.m_allocator );
		swap( m_eAllocationDomain, rOther.m_eAllocationDomain );
		swap( m_pData, rOther.m_pData );
		swap( m_nCapacity, rOther.m_nCapacity );
		swap( m_bSealed, rOther.m_bSealed );
		swap( m_bOwnsData, rOther.m_bOwnsData );
		swap( m_nGrowthRequests, rOther.m_nGrowthRequests );
		swap( m_nGrowthDenied, rOther.m_nGrowthDenied );
		swap( m_nCapacityMisses, rOther.m_nCapacityMisses );
		swap( m_nGrowCount, rOther.m_nGrowCount );
	}
};

#endif // TERRAIN3D_NATIVE_BACKING_H
