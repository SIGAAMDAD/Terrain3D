// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Fixed-capacity linear arenas and non-owning typed buffers for bulk-lifetime data.
#ifndef TERRAIN3D_LINEAR_ARENA_H
#define TERRAIN3D_LINEAR_ARENA_H

#include "core/config/terrain_3d_compiler.h"
#include "memory/arena/terrain_3d_aligned_backing.h"
#include "core/utility/terrain_3d_checked_math.h"
#include "Terrain3DConstants.h"

#include <EASTL/type_traits.h>
#include <EASTL/numeric_limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <new>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

class Terrain3DLinearArenaLayout
{
public:
	bool add(
		const size_t nBytes,
		const size_t nAlignment,
		size_t		*pOffset = NULL ) {
		if ( nBytes == 0u ) {
			if ( pOffset ) {
				*pOffset = m_nSize;
			}
			return true;
		}
		size_t aligned_offset = 0u;
		if ( !t3dCheckedAlignUp< size_t >( m_nSize, nAlignment, aligned_offset ) ||
			 nBytes > eastl::numeric_limits< size_t >::max() - aligned_offset ) {
			m_bValid = false;
			return false;
		}
		if ( pOffset ) {
			*pOffset = aligned_offset;
		}
		m_nSize = aligned_offset + nBytes;
		return true;
	}

	template< typename T >
	bool addArray(
		const size_t nCount ) {
		static_assert( eastl::is_trivially_destructible_v< T >, "Linear arena arrays must not require destruction." );
		if ( nCount > eastl::numeric_limits< size_t >::max() / sizeof( T ) ) {
			m_bValid = false;
			return false;
		}
		return add( nCount * sizeof( T ), MAX( alignof( T ), T3D_ARENA_CACHELINE_ALIGNMENT ) );
	}

	EA_FORCE_INLINE size_t size(
		void ) const {
		return m_nSize;
	}

	EA_FORCE_INLINE bool valid(
		void ) const {
		return m_bValid;
	}

private:
	size_t m_nSize = 0u;
	bool   m_bValid = true;
};

class Terrain3DLinearArena
{
public:
	Terrain3DLinearArena( void ) = default;
	~Terrain3DLinearArena( void ) = default;
	Terrain3DLinearArena( const Terrain3DLinearArena & ) = delete;
	Terrain3DLinearArena &operator=( const Terrain3DLinearArena & ) = delete;

	bool configure(
		const size_t nBytes,
		const size_t nBaseAlignment = T3D_ARENA_DEFAULT_BASE_ALIGNMENT ) {
		if ( m_bCapacitySealed || !t3dIsPowerOfTwo( nBaseAlignment ) ) {
			return false;
		}
		m_iOffset = 0u;
		return m_backing.allocate( nBytes, nBaseAlignment );
	}

	bool ensureCapacity(
		const size_t nBytes,
		const size_t nBaseAlignment = T3D_ARENA_DEFAULT_BASE_ALIGNMENT ) {
		if ( !t3dIsPowerOfTwo( nBaseAlignment ) ) {
			return false;
		}
		if ( nBytes <= m_backing.capacity() && nBaseAlignment <= m_backing.alignment() &&
			 ( m_backing.data() || nBytes == 0u ) ) {
			m_iOffset = 0u;
			return true;
		}
		if ( m_bCapacitySealed ) {
			return false;
		}
		return configure( nBytes, nBaseAlignment );
	}

	EA_FORCE_INLINE bool sealCapacity(
		void ) {
		m_bCapacitySealed = true;
		return true;
	}

	EA_FORCE_INLINE void unsealCapacityCold(
		void ) {
		m_bCapacitySealed = false;
	}

	EA_FORCE_INLINE bool capacitySealed(
		void ) const {
		return m_bCapacitySealed;
	}

	void release(
		void ) {
		releaseBacking();
		m_bCapacitySealed = false;
	}

	EA_FORCE_INLINE void reset(
		void ) {
		m_iOffset = 0u;
	}

	void *allocate(
		const size_t nBytes,
		const size_t nAlignment ) {
		if ( nBytes == 0u ) {
			return NULL;
		}
		size_t aligned_offset = 0u;
		if ( !m_backing.data() || nAlignment > m_backing.alignment() ||
			 !t3dCheckedAlignUp( m_iOffset, nAlignment, aligned_offset ) ||
			 !t3dByteRangeFits( aligned_offset, nBytes, m_backing.capacity() ) ) {
			return NULL;
		}
		void *result = m_backing.data() + aligned_offset;
		m_iOffset = aligned_offset + nBytes;
		return result;
	}

	template< typename T >
	T *allocateArray(
		const size_t nCount ) {
		static_assert( eastl::is_trivially_destructible_v< T >, "Linear arena arrays must not require destruction." );
		if ( nCount == 0u || nCount > eastl::numeric_limits< size_t >::max() / sizeof( T ) ) {
			return NULL;
		}
		T *const pStorage = ( T * ) ( allocate( nCount * sizeof( T ), MAX( alignof( T ), T3D_ARENA_CACHELINE_ALIGNMENT ) ) );
		if ( !pStorage ) {
			return NULL;
		}
		// C++17: raw arena storage does not start the lifetime of a class object.
		// Default initialization preserves the uninitialized scalar-array contract.
		for ( size_t i = 0u; i < nCount; ++i ) {
			::new ( static_cast< void * >( pStorage + i ) ) T;
		}
		return pStorage;
	}

	EA_FORCE_INLINE size_t capacity(
		void ) const {
		return m_backing.capacity();
	}

	EA_FORCE_INLINE size_t used(
		void ) const {
		return m_iOffset;
	}

	EA_FORCE_INLINE size_t remaining(
		void ) const {
		return m_backing.capacity() - m_iOffset;
	}

	EA_FORCE_INLINE bool valid(
		void ) const {
		return m_backing.data() != NULL || m_backing.capacity() == 0u;
	}

private:
	void releaseBacking(
		void ) {
		m_backing.release();
		m_iOffset = 0u;
	}

	Terrain3DAlignedBacking m_backing;
	size_t					m_iOffset = 0u;
	bool					m_bCapacitySealed = false;
};

template< typename T > class Terrain3DArenaBuffer
{
	static_assert( eastl::is_trivially_copyable_v< T >, "Arena buffers support trivially copyable values only." );
	static_assert( eastl::is_trivially_destructible_v< T >,
				   "Arena buffers support trivially destructible values only." );

public:
	Terrain3DArenaBuffer() = default;
	Terrain3DArenaBuffer( const Terrain3DArenaBuffer & ) = delete;
	Terrain3DArenaBuffer &operator=( const Terrain3DArenaBuffer & ) = delete;

	bool allocateUninitialized(
		Terrain3DLinearArena &rArena,
		const size_t		  nCount ) {
		clear();
		if ( nCount == 0u ) {
			return true;
		}
		T *storage = rArena.allocateArray< T >( nCount );
		if ( !storage ) {
			return false;
		}
		m_pData = storage;
		m_nSize = nCount;
		return true;
	}

	bool assign(
		Terrain3DLinearArena &rArena,
		const size_t		  nCount,
		const T				 &rValue ) {
		if ( !allocateUninitialized( rArena, nCount ) ) {
			return false;
		}
		for ( size_t index = 0u; index < m_nSize; ++index ) {
			m_pData[index] = rValue;
		}
		return true;
	}

	EA_FORCE_INLINE void clear(
		void ) {
		m_pData = NULL;
		m_nSize = 0u;
	}

	EA_FORCE_INLINE void swap(
		Terrain3DArenaBuffer &rOther ) noexcept {
		eastl::swap( m_pData, rOther.m_pData );
		eastl::swap( m_nSize, rOther.m_nSize );
	}

	bool copyFrom(
		const Terrain3DArenaBuffer &rOther ) {
		if ( m_nSize != rOther.m_nSize ) {
			return false;
		}
		if ( m_nSize > 0u ) {
			memcpy( m_pData, rOther.m_pData, m_nSize * sizeof( T ) );
		}
		return true;
	}

	T &operator[](
		const size_t iIndex ) {
		return m_pData[iIndex];
	}

	const T &operator[](
		const size_t iIndex ) const {
		return m_pData[iIndex];
	}

	EA_FORCE_INLINE T *data(
		void ) {
		return m_pData;
	}

	EA_FORCE_INLINE const T *data(
		void ) const {
		return m_pData;
	}

	EA_FORCE_INLINE T *begin(
		void ) {
		return m_pData;
	}

	EA_FORCE_INLINE const T *begin(
		void ) const {
		return m_pData;
	}

	EA_FORCE_INLINE T *end(
		void ) {
		return m_pData ? m_pData + m_nSize : NULL;
	}

	EA_FORCE_INLINE const T *end(
		void ) const {
		return m_pData ? m_pData + m_nSize : NULL;
	}

	EA_FORCE_INLINE size_t size(
		void ) const {
		return m_nSize;
	}

	EA_FORCE_INLINE bool empty(
		void ) const {
		return m_nSize == 0u;
	}

private:
	T	  *m_pData = NULL;
	size_t m_nSize = 0u;
};

#endif // TERRAIN3D_LINEAR_ARENA_H
