// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_FIXED_BUFFER_H
#define TERRAIN3D_FIXED_BUFFER_H

#include "core/terrain_3d_compiler.h"
#include "eastl_config.h"

#include <EASTL/type_traits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

// Explicit-capacity runtime buffer. By default the buffer owns its backing through
// Terrain3DEastlAllocator so cold growth remains inside the Terrain3D allocator
// domain. bind() is retained for the few layouts that deliberately carve this
// view from parent-owned storage; externally bound buffers never free that data.
template <typename T>
class Terrain3DFixedBuffer {
	static_assert( eastl::is_trivially_copyable_v<T>, "Terrain3DFixedBuffer requires trivially copyable values." );
	static_assert( eastl::is_trivially_destructible_v<T>, "Terrain3DFixedBuffer requires trivial destruction." );

	Terrain3DEastlAllocator m_allocator;
	T *m_pData = NULL;
	size_t m_nCount = 0u;
	size_t m_nCapacity = 0u;
	bool m_bOwnsData = false;

	bool reallocateCold( const size_t nCapacity ) {
		if ( nCapacity == m_nCapacity ) {
			return true;
		}
		if ( nCapacity < m_nCount || ( nCapacity > 0u && nCapacity > SIZE_MAX / sizeof( T ) ) ) {
			return false;
		}
		// Parent-bound storage is an explicit non-owning contract. Do not silently
		// detach from it when a caller asks for more capacity.
		if ( m_pData && !m_bOwnsData ) {
			return false;
		}
		T *pReplacement = NULL;
		if ( nCapacity > 0u ) {
			pReplacement = ( T * )m_allocator.allocate( nCapacity * sizeof( T ), alignof( T ), 0u );
			if ( !pReplacement ) {
				return false;
			}
			if ( m_nCount > 0u && m_pData ) {
				memcpy( pReplacement, m_pData, m_nCount * sizeof( T ) );
			}
		}
		if ( m_pData && m_bOwnsData ) {
			m_allocator.deallocate( m_pData );
		}
		m_pData = pReplacement;
		m_nCapacity = nCapacity;
		m_bOwnsData = pReplacement != NULL;
		return true;
	}

public:
	Terrain3DFixedBuffer( void ) = default;
	~Terrain3DFixedBuffer( void ) {
		shutdown();
	}
	Terrain3DFixedBuffer( const Terrain3DFixedBuffer & ) = delete;
	Terrain3DFixedBuffer &operator=( const Terrain3DFixedBuffer & ) = delete;
	Terrain3DFixedBuffer( Terrain3DFixedBuffer &&rOther ) noexcept :
	        m_allocator( eastl::move( rOther.m_allocator ) ),
	        m_pData( rOther.m_pData ),
	        m_nCount( rOther.m_nCount ),
	        m_nCapacity( rOther.m_nCapacity ),
	        m_bOwnsData( rOther.m_bOwnsData ) {
		rOther.m_pData = NULL;
		rOther.m_nCount = 0u;
		rOther.m_nCapacity = 0u;
		rOther.m_bOwnsData = false;
	}
	Terrain3DFixedBuffer &operator=( Terrain3DFixedBuffer &&rOther ) noexcept {
		if ( this != &rOther ) {
			shutdown();
			m_allocator = eastl::move( rOther.m_allocator );
			m_pData = rOther.m_pData;
			m_nCount = rOther.m_nCount;
			m_nCapacity = rOther.m_nCapacity;
			m_bOwnsData = rOther.m_bOwnsData;
			rOther.m_pData = NULL;
			rOther.m_nCount = 0u;
			rOther.m_nCapacity = 0u;
			rOther.m_bOwnsData = false;
		}
		return *this;
	}
	EA_FORCE_INLINE bool bind( T *const pData, const size_t nCapacity, const size_t nCount = 0u ) {
		if ( nCount > nCapacity || ( nCapacity > 0u && !pData ) ) {
			return false;
		}
		shutdown();
		m_pData = pData;
		m_nCapacity = nCapacity;
		m_nCount = nCount;
		m_bOwnsData = false;
		return true;
	}

	EA_FORCE_INLINE bool reserveCold( const size_t nCapacity ) {
		return nCapacity <= m_nCapacity ? true : reallocateCold( nCapacity );
	}

	EA_FORCE_INLINE bool resizeUninitialized( const size_t nCount ) {
		if ( nCount > m_nCapacity ) {
			return false;
		}
		m_nCount = nCount;
		return true;
	}
	EA_FORCE_INLINE bool resizeZeroed( const size_t nCount ) {
		if ( nCount > m_nCapacity ) {
			return false;
		}
		if ( nCount > m_nCount ) {
			memset( m_pData + m_nCount, 0, ( nCount - m_nCount ) * sizeof( T ) );
		}
		m_nCount = nCount;
		return true;
	}
	EA_FORCE_INLINE bool copyFrom( const T *pValues, const size_t nCount ) {
		if ( nCount > m_nCapacity || ( nCount > 0u && pValues == NULL ) ) {
			return false;
		}
		if ( nCount > 0u ) {
			memcpy( m_pData, pValues, nCount * sizeof( T ) );
		}
		m_nCount = nCount;
		return true;
	}
	EA_FORCE_INLINE bool append( const T *pValues, const size_t nCount ) {
		if ( nCount > m_nCapacity - m_nCount || ( nCount > 0u && pValues == NULL ) ) {
			return false;
		}
		if ( nCount > 0u ) {
			memcpy( m_pData + m_nCount, pValues, nCount * sizeof( T ) );
			m_nCount += nCount;
		}
		return true;
	}
	EA_FORCE_INLINE bool insertOrdered( const size_t iIndex, const T &rValue ) {
		if ( iIndex > m_nCount || m_nCount >= m_nCapacity ) {
			return false;
		}
		const size_t nMoveCount = m_nCount - iIndex;
		if ( nMoveCount > 0u ) {
			memmove( m_pData + iIndex + 1u, m_pData + iIndex, nMoveCount * sizeof( T ) );
		}
		m_pData[iIndex] = rValue;
		m_nCount++;
		return true;
	}
	EA_FORCE_INLINE void eraseOrdered( const size_t iIndex ) {
		if ( iIndex >= m_nCount ) {
			return;
		}
		const size_t nMoveCount = m_nCount - iIndex - 1u;
		if ( nMoveCount > 0u ) {
			memmove( m_pData + iIndex, m_pData + iIndex + 1u, nMoveCount * sizeof( T ) );
		}
		m_nCount--;
	}
	EA_FORCE_INLINE bool pushBack( const T &rValue ) {
		if ( m_nCount >= m_nCapacity ) {
			return false;
		}
		m_pData[m_nCount++] = rValue;
		return true;
	}
	EA_FORCE_INLINE bool pushBack( T &&rValue ) {
		if ( m_nCount >= m_nCapacity ) {
			return false;
		}
		m_pData[m_nCount++] = eastl::move( rValue );
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
		if ( m_pData && m_bOwnsData ) {
			m_allocator.deallocate( m_pData );
		}
		m_pData = NULL;
		m_nCount = 0u;
		m_nCapacity = 0u;
		m_bOwnsData = false;
	}

	EA_FORCE_INLINE T *data( void ) {
		return m_pData;
	}
	EA_FORCE_INLINE const T *data( void ) const {
		return m_pData;
	}
	EA_FORCE_INLINE T *begin( void ) {
		return m_pData;
	}
	EA_FORCE_INLINE const T *begin( void ) const {
		return m_pData;
	}
	EA_FORCE_INLINE T *end( void ) {
		return m_pData ? m_pData + m_nCount : NULL;
	}
	EA_FORCE_INLINE const T *end( void ) const {
		return m_pData ? m_pData + m_nCount : NULL;
	}
	EA_FORCE_INLINE T &operator[]( const size_t iIndex ) {
		return m_pData[iIndex];
	}
	EA_FORCE_INLINE const T &operator[]( const size_t iIndex ) const {
		return m_pData[iIndex];
	}
	EA_FORCE_INLINE T &back( void ) {
		return m_pData[m_nCount - 1u];
	}
	EA_FORCE_INLINE const T &back( void ) const {
		return m_pData[m_nCount - 1u];
	}
	EA_FORCE_INLINE bool empty( void ) const {
		return m_nCount == 0u;
	}
	EA_FORCE_INLINE size_t size( void ) const {
		return m_nCount;
	}
	EA_FORCE_INLINE size_t capacity( void ) const {
		return m_nCapacity;
	}
	EA_FORCE_INLINE bool ownsBacking( void ) const {
		return m_bOwnsData;
	}
	EA_FORCE_INLINE size_t residentBytes( void ) const {
		return m_nCapacity * sizeof( T );
	}
};

#endif // TERRAIN3D_FIXED_BUFFER_H
