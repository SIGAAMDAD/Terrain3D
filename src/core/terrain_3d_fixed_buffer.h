// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_FIXED_BUFFER_H
#define TERRAIN3D_FIXED_BUFFER_H

#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_native_backing.h"

#include <EASTL/type_traits.h>
#include <EASTL/utility.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Explicit-capacity runtime buffer. Backing ownership/allocation is centralized
// in Terrain3DNativeBacking; this type owns only logical element-count policy.
template <typename T>
class Terrain3DFixedBuffer {
	static_assert( eastl::is_trivially_copyable_v<T>, "Terrain3DFixedBuffer requires trivially copyable values." );
	static_assert( eastl::is_trivially_destructible_v<T>, "Terrain3DFixedBuffer requires trivial destruction." );

	Terrain3DNativeBacking<T> m_backing;
	size_t m_nCount = 0u;

public:
	Terrain3DFixedBuffer( void ) = default;
	explicit Terrain3DFixedBuffer( const Terrain3DEastlAllocator &rAllocator ) : m_backing( rAllocator ) {
	}
	~Terrain3DFixedBuffer( void ) = default;
	Terrain3DFixedBuffer( const Terrain3DFixedBuffer & ) = delete;
	Terrain3DFixedBuffer &operator=( const Terrain3DFixedBuffer & ) = delete;
	Terrain3DFixedBuffer( Terrain3DFixedBuffer &&rOther ) noexcept :
	        m_backing( eastl::move( rOther.m_backing ) ), m_nCount( rOther.m_nCount ) {
		rOther.m_nCount = 0u;
	}
	Terrain3DFixedBuffer &operator=( Terrain3DFixedBuffer &&rOther ) noexcept {
		if ( this != &rOther ) {
			m_backing = eastl::move( rOther.m_backing );
			m_nCount = rOther.m_nCount;
			rOther.m_nCount = 0u;
		}
		return *this;
	}

	EA_FORCE_INLINE bool requireRuntimeZone( void ) {
		return m_backing.requireRuntimeZone();
	}
	EA_FORCE_INLINE bool bindAllocatorZone(
	        Terrain3DMemoryZone *const pZone, const t3dAllocatorOverflowPolicy_t eOverflowPolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT ) {
		return m_backing.bindAllocatorZone( pZone, eOverflowPolicy );
	}
	EA_FORCE_INLINE bool bind( T *const pData, const size_t nCapacity, const size_t nCount = 0u ) {
		if ( nCount > nCapacity || ( nCapacity > 0u && !pData ) ) {
			return false;
		}
		m_nCount = 0u;
		if ( !m_backing.bindExternal( pData, nCapacity, true ) ) {
			return false;
		}
		m_nCount = nCount;
		return true;
	}

	EA_FORCE_INLINE bool reserveCold( const size_t nCapacity ) {
		return nCapacity <= m_backing.capacity() ? true : m_backing.growCold( nCapacity, m_nCount, false );
	}
	EA_FORCE_INLINE bool seal( void ) {
		return m_backing.seal();
	}
	EA_FORCE_INLINE void unsealCold( void ) {
		m_backing.unsealCold();
	}
	EA_FORCE_INLINE bool sealed( void ) const {
		return m_backing.sealed();
	}

	EA_FORCE_INLINE bool resizeUninitialized( const size_t nCount ) {
		if ( nCount > m_backing.capacity() ) {
			m_backing.recordCapacityMiss();
			return false;
		}
		m_nCount = nCount;
		return true;
	}
	EA_FORCE_INLINE bool resizeZeroed( const size_t nCount ) {
		if ( nCount > m_backing.capacity() ) {
			return false;
		}
		if ( nCount > m_nCount ) {
			memset( m_backing.data() + m_nCount, 0, ( nCount - m_nCount ) * sizeof( T ) );
		}
		m_nCount = nCount;
		return true;
	}
	EA_FORCE_INLINE bool copyFrom( const T *pValues, const size_t nCount ) {
		if ( nCount > m_backing.capacity() || ( nCount > 0u && pValues == NULL ) ) {
			return false;
		}
		if ( nCount > 0u ) {
			memcpy( m_backing.data(), pValues, nCount * sizeof( T ) );
		}
		m_nCount = nCount;
		return true;
	}
	EA_FORCE_INLINE bool append( const T *pValues, const size_t nCount ) {
		if ( nCount > m_backing.capacity() - m_nCount || ( nCount > 0u && pValues == NULL ) ) {
			return false;
		}
		if ( nCount > 0u ) {
			memcpy( m_backing.data() + m_nCount, pValues, nCount * sizeof( T ) );
			m_nCount += nCount;
		}
		return true;
	}
	EA_FORCE_INLINE bool insertOrdered( const size_t iIndex, const T &rValue ) {
		if ( iIndex > m_nCount || m_nCount >= m_backing.capacity() ) {
			return false;
		}
		const size_t nMoveCount = m_nCount - iIndex;
		if ( nMoveCount > 0u ) {
			memmove( m_backing.data() + iIndex + 1u, m_backing.data() + iIndex, nMoveCount * sizeof( T ) );
		}
		m_backing.data()[iIndex] = rValue;
		m_nCount++;
		return true;
	}
	EA_FORCE_INLINE void eraseOrdered( const size_t iIndex ) {
		if ( iIndex >= m_nCount ) {
			return;
		}
		const size_t nMoveCount = m_nCount - iIndex - 1u;
		if ( nMoveCount > 0u ) {
			memmove( m_backing.data() + iIndex, m_backing.data() + iIndex + 1u, nMoveCount * sizeof( T ) );
		}
		m_nCount--;
	}
	EA_FORCE_INLINE bool pushBack( const T &rValue ) {
		if ( m_nCount >= m_backing.capacity() ) {
			m_backing.recordCapacityMiss();
			return false;
		}
		m_backing.data()[m_nCount++] = rValue;
		return true;
	}
	EA_FORCE_INLINE bool pushBack( T &&rValue ) {
		if ( m_nCount >= m_backing.capacity() ) {
			m_backing.recordCapacityMiss();
			return false;
		}
		m_backing.data()[m_nCount++] = eastl::move( rValue );
		return true;
	}
	EA_FORCE_INLINE void popBack( void ) {
		if ( m_nCount > 0u ) {
			--m_nCount;
		}
	}
	EA_FORCE_INLINE void reset( void ) {
		m_nCount = 0u;
	}
	EA_FORCE_INLINE void shutdown( void ) {
		m_nCount = 0u;
		m_backing.release();
	}

	EA_FORCE_INLINE T *data( void ) {
		return m_backing.data();
	}
	EA_FORCE_INLINE const T *data( void ) const {
		return m_backing.data();
	}
	EA_FORCE_INLINE T *begin( void ) {
		return m_backing.data();
	}
	EA_FORCE_INLINE const T *begin( void ) const {
		return m_backing.data();
	}
	EA_FORCE_INLINE T *end( void ) {
		return m_backing.data() ? m_backing.data() + m_nCount : NULL;
	}
	EA_FORCE_INLINE const T *end( void ) const {
		return m_backing.data() ? m_backing.data() + m_nCount : NULL;
	}
	EA_FORCE_INLINE T &operator[]( const size_t iIndex ) {
		return m_backing.data()[iIndex];
	}
	EA_FORCE_INLINE const T &operator[]( const size_t iIndex ) const {
		return m_backing.data()[iIndex];
	}
	EA_FORCE_INLINE T &back( void ) {
		return m_backing.data()[m_nCount - 1u];
	}
	EA_FORCE_INLINE const T &back( void ) const {
		return m_backing.data()[m_nCount - 1u];
	}
	EA_FORCE_INLINE bool empty( void ) const {
		return m_nCount == 0u;
	}
	EA_FORCE_INLINE size_t size( void ) const {
		return m_nCount;
	}
	EA_FORCE_INLINE size_t capacity( void ) const {
		return m_backing.capacity();
	}
	EA_FORCE_INLINE t3dNativeAllocationDomain_t allocationDomain( void ) const {
		return m_backing.allocationDomain();
	}
	EA_FORCE_INLINE bool runtimeBindingSatisfied( void ) const {
		return m_backing.runtimeBindingSatisfied();
	}
	EA_FORCE_INLINE bool ownsBacking( void ) const {
		return m_backing.ownsBacking();
	}
	EA_FORCE_INLINE size_t residentBytes( void ) const {
		return m_backing.residentBytes();
	}
	EA_FORCE_INLINE uint64_t growthRequests( void ) const {
		return m_backing.growthRequests();
	}
	EA_FORCE_INLINE uint64_t growthDenied( void ) const {
		return m_backing.growthDenied();
	}
	EA_FORCE_INLINE uint64_t capacityMisses( void ) const {
		return m_backing.capacityMisses();
	}
};

#endif // TERRAIN3D_FIXED_BUFFER_H
