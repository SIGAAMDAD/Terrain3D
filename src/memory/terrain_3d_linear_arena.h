// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Fixed-capacity linear arenas and non-owning typed buffers for bulk-lifetime data.
#pragma once

#ifndef TERRAIN3D_LINEAR_ARENA_H
#define TERRAIN3D_LINEAR_ARENA_H

#include "memory/terrain_3d_memory_constants.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

#include "memory/terrain_3d_memory.h"


class T3DLinearArenaLayout {
public:
bool add( const size_t nBytes, const size_t nAlignment, size_t *pOffset = nullptr ) {
		if ( nBytes == 0u ) {
			if ( pOffset ) {
				*pOffset = m_nSize;
			}
			return true;
		}
		size_t aligned_offset = 0u;
		if ( !alignUp( m_nSize, nAlignment, aligned_offset ) || nBytes > std::numeric_limits<size_t>::max() - aligned_offset ) {
			m_bValid = false;
			return false;
		}
		if ( pOffset ) {
			*pOffset = aligned_offset;
		}
		m_nSize = aligned_offset + nBytes;
		return true;
	}

	template <typename T> bool addArray( const size_t nCount ) {
		static_assert( std::is_trivially_destructible_v<T>, "Linear arena arrays must not require destruction." );
		if ( nCount > std::numeric_limits<size_t>::max() / sizeof( T ) ) {
			m_bValid = false;
			return false;
		}
		return add( nCount * sizeof( T ), std::max<size_t>( alignof( T ), TERRAIN3D_ARENA_CACHELINE_ALIGNMENT ) );
	}

	size_t size( void ) const {
		return m_nSize;
	}

	bool valid( void ) const {
		return m_bValid;
	}

private:
	size_t m_nSize = 0u;
	bool m_bValid = true;

	static bool alignUp( const size_t nValue, const size_t nAlignment, size_t &nAligned ) {
		if ( nAlignment == 0u || ( nAlignment & ( nAlignment - 1u ) ) != 0u ) {
			return false;
		}
		const size_t mask = nAlignment - 1u;
		if ( nValue > std::numeric_limits<size_t>::max() - mask ) {
			return false;
		}
		nAligned = ( nValue + mask ) & ~mask;
		return true;
	}
};

class T3DLinearArena {
public:
	T3DLinearArena( void ) = default;
	~T3DLinearArena( void ) {
		release();
	}
	T3DLinearArena( const T3DLinearArena & ) = delete;
	T3DLinearArena &operator=( const T3DLinearArena & ) = delete;

	bool bind( void *const pBacking, const size_t nBytes ) {
		release();
		if ( nBytes == 0u ) {
			return true;
		}
		if ( !pBacking ) {
			return false;
		}
		m_pBase = ( uint8_t * ) ( pBacking );
		m_nCapacity = nBytes;
		m_iOffset = 0u;
		m_bOwnsBacking = false;
		return true;
	}

	bool configure( const size_t nBytes, const size_t nBaseAlignment = TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT ) {
		release();
		if ( nBytes == 0u ) {
			return true;
		}
		if ( nBaseAlignment == 0u || ( nBaseAlignment & ( nBaseAlignment - 1u ) ) != 0u ||
			nBytes > std::numeric_limits<size_t>::max() - ( nBaseAlignment - 1u ) ) {
			return false;
		}

		m_pRaw = terrain3d_memory::allocate( nBytes + nBaseAlignment - 1u );
		if ( !m_pRaw ) {
			return false;
		}
		const uintptr_t raw_address = ( uintptr_t ) ( m_pRaw );
		const uintptr_t mask = ( uintptr_t ) ( nBaseAlignment - 1u );
		m_pBase = ( uint8_t * ) ( ( raw_address + mask ) & ~mask );
		m_nCapacity = nBytes;
		m_iOffset = 0u;
		m_bOwnsBacking = true;
		return true;
	}

	void release( void ) {
		if ( m_bOwnsBacking && m_pRaw ) {
			terrain3d_memory::release( m_pRaw );
		}
		m_pRaw = nullptr;
		m_pBase = nullptr;
		m_nCapacity = 0u;
		m_iOffset = 0u;
		m_bOwnsBacking = false;
	}

	void reset( void ) {
		m_iOffset = 0u;
	}

	void *allocate( const size_t nBytes, const size_t nAlignment ) {
		if ( nBytes == 0u ) {
			return nullptr;
		}
		if ( !m_pBase || nAlignment == 0u || ( nAlignment & ( nAlignment - 1u ) ) != 0u ) {
			return nullptr;
		}
		const size_t mask = nAlignment - 1u;
		if ( m_iOffset > std::numeric_limits<size_t>::max() - mask ) {
			return nullptr;
		}
		const size_t aligned_offset = ( m_iOffset + mask ) & ~mask;
		if ( aligned_offset > m_nCapacity || nBytes > m_nCapacity - aligned_offset ) {
			return nullptr;
		}
		void *result = m_pBase + aligned_offset;
		m_iOffset = aligned_offset + nBytes;
		return result;
	}

	template <typename T> T *allocateArray( const size_t nCount ) {
		static_assert( std::is_trivially_destructible_v<T>, "Linear arena arrays must not require destruction." );
		if ( nCount == 0u || nCount > std::numeric_limits<size_t>::max() / sizeof( T ) ) {
			return nullptr;
		}
		return ( T * ) ( allocate(
			nCount * sizeof( T ), std::max<size_t>( alignof( T ), TERRAIN3D_ARENA_CACHELINE_ALIGNMENT ) ) );
	}

	size_t mark( void ) const {
		return m_iOffset;
	}

	bool rewind( const size_t nMark ) {
		if ( nMark > m_iOffset ) {
			return false;
		}
		m_iOffset = nMark;
		return true;
	}

	size_t capacity( void ) const {
		return m_nCapacity;
	}

	size_t used( void ) const {
		return m_iOffset;
	}

	size_t remaining( void ) const {
		return m_nCapacity - m_iOffset;
	}

	bool valid( void ) const {
		return m_pBase != nullptr || m_nCapacity == 0u;
	}

private:
	void *m_pRaw = nullptr;
	uint8_t *m_pBase = nullptr;
	size_t m_nCapacity = 0u;
	size_t m_iOffset = 0u;
	bool m_bOwnsBacking = false;
};

template <typename T> class Terrain3DArenaBuffer {
	static_assert( std::is_trivially_copyable_v<T>, "Arena buffers support trivially copyable values only." );
	static_assert( std::is_trivially_destructible_v<T>, "Arena buffers support trivially destructible values only." );

public:
	using value_type = T;
	using iterator = T *;
	using const_iterator = const T *;

	Terrain3DArenaBuffer() = default;
	Terrain3DArenaBuffer( const Terrain3DArenaBuffer & ) = delete;
	Terrain3DArenaBuffer &operator=( const Terrain3DArenaBuffer & ) = delete;

	bool allocateUninitialized( T3DLinearArena &rArena, const size_t nCount ) {
		clear();
		if ( nCount == 0u ) {
			return true;
		}
		T *storage = rArena.allocateArray<T>( nCount );
		if ( !storage ) {
			return false;
		}
		m_pData = storage;
		m_nSize = nCount;
		return true;
	}

	bool assign( T3DLinearArena &rArena, const size_t nCount, const T &rValue ) {
		if ( !allocateUninitialized( rArena, nCount ) ) {
			return false;
		}
		if ( m_nSize > 0u ) {
			std::fill_n( m_pData, m_nSize, rValue );
		}
		return true;
	}

	void clear( void ) {
		m_pData = nullptr;
		m_nSize = 0u;
	}

	void swap( Terrain3DArenaBuffer &rOther ) noexcept {
		std::swap( m_pData, rOther.m_pData );
		std::swap( m_nSize, rOther.m_nSize );
	}

	bool copyFrom( const Terrain3DArenaBuffer &rOther ) {
		if ( m_nSize != rOther.m_nSize ) {
			return false;
		}
		if ( m_nSize > 0u ) {
			std::memcpy( m_pData, rOther.m_pData, m_nSize * sizeof( T ) );
		}
		return true;
	}

	T &operator[]( const size_t iIndex ) {
		return m_pData[iIndex];
	}

	const T &operator[]( const size_t iIndex ) const {
		return m_pData[iIndex];
	}

	T *data( void ) {
		return m_pData;
	}

	const T *data( void ) const {
		return m_pData;
	}

	iterator begin( void ) {
		return m_pData;
	}

	const_iterator begin( void ) const {
		return m_pData;
	}

	iterator end( void ) {
		return m_pData ? m_pData + m_nSize : nullptr;
	}

	const_iterator end( void ) const {
		return m_pData ? m_pData + m_nSize : nullptr;
	}

	size_t size( void ) const {
		return m_nSize;
	}

	bool empty( void ) const {
		return m_nSize == 0u;
	}

private:
	T *m_pData = nullptr;
	size_t m_nSize = 0u;
};

#endif // TERRAIN3D_LINEAR_ARENA_H
