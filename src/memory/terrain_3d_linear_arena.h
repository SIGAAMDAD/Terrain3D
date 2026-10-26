// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Fixed-capacity linear arenas and non-owning typed buffers for bulk-lifetime data.
#ifndef TERRAIN3D_LINEAR_ARENA_H
#define TERRAIN3D_LINEAR_ARENA_H

#include "core/terrain_3d_compiler.h"
#include "memory/terrain_3d_memory.h"
#include "memory/terrain_3d_memory_constants.h"

#include <EASTL/type_traits.h>
#include <EASTL/numeric_limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

class Terrain3DLinearArenaLayout
{
public:
	bool add( const size_t nBytes, const size_t nAlignment, size_t *pOffset = NULL )
	{
		if ( nBytes == 0u ) {
			if ( pOffset ) {
				*pOffset = m_nSize;
			}
			return true;
		}
		size_t aligned_offset = 0u;
		if ( !alignUp( m_nSize, nAlignment, aligned_offset ) || nBytes > eastl::numeric_limits<size_t>::max() - aligned_offset ) {
			m_bValid = false;
			return false;
		}
		if ( pOffset ) {
			*pOffset = aligned_offset;
		}
		m_nSize = aligned_offset + nBytes;
		return true;
	}

	template <typename T>
	bool addArray( const size_t nCount )
	{
		static_assert( eastl::is_trivially_destructible_v<T>, "Linear arena arrays must not require destruction." );
		if ( nCount > eastl::numeric_limits<size_t>::max() / sizeof( T ) ) {
			m_bValid = false;
			return false;
		}
		return add( nCount * sizeof( T ), MAX( alignof( T ), s_nArenaCachelineAlignment ) );
	}

	EA_FORCE_INLINE size_t size( void ) const
	{
		return m_nSize;
	}

	EA_FORCE_INLINE bool valid( void ) const
	{
		return m_bValid;
	}

private:
	size_t m_nSize = 0u;
	bool m_bValid = true;

	static bool alignUp( const size_t nValue, const size_t nAlignment, size_t& nAligned )
	{
		if ( nAlignment == 0u || ( nAlignment & ( nAlignment - 1u ) ) != 0u ) {
			return false;
		}
		const size_t mask = nAlignment - 1u;
		if ( nValue > eastl::numeric_limits<size_t>::max() - mask ) {
			return false;
		}
		nAligned = ( nValue + mask ) & ~mask;
		return true;
	}
};

class Terrain3DLinearArena
{
public:
	Terrain3DLinearArena( void ) = default;
	EA_FORCE_INLINE ~Terrain3DLinearArena( void )
	{
		release();
	}
	Terrain3DLinearArena( const Terrain3DLinearArena& ) = delete;
	Terrain3DLinearArena& operator=( const Terrain3DLinearArena& ) = delete;

	bool configure( const size_t nBytes, const size_t nBaseAlignment = s_nArenaDefaultBaseAlignment )
	{
		if ( m_bCapacitySealed ) {
			return false;
		}
		releaseBacking();
		if ( nBytes == 0u ) {
			return true;
		}
		if ( nBaseAlignment == 0u || ( nBaseAlignment & ( nBaseAlignment - 1u ) ) != 0u || nBytes > eastl::numeric_limits<size_t>::max() - ( nBaseAlignment - 1u ) ) {
			return false;
		}

		m_pRaw = t3dMemoryAllocate( nBytes + nBaseAlignment - 1u );
		if ( !m_pRaw ) {
			return false;
		}
		const uintptr_t raw_address = (uintptr_t)( m_pRaw );
		const uintptr_t mask = (uintptr_t)( nBaseAlignment - 1u );
		m_pBase = (uint8_t *)( ( raw_address + mask ) & ~mask );
		m_nCapacity = nBytes;
		m_iOffset = 0u;
		return true;
	}

	bool ensureCapacity( const size_t nBytes, const size_t nBaseAlignment = s_nArenaDefaultBaseAlignment )
	{
		if ( nBytes <= m_nCapacity && ( m_pBase || nBytes == 0u ) ) {
			m_iOffset = 0u;
			return true;
		}
		if ( m_bCapacitySealed ) {
			return false;
		}
		return configure( nBytes, nBaseAlignment );
	}

	EA_FORCE_INLINE bool sealCapacity( void )
	{
		m_bCapacitySealed = true;
		return true;
	}

	EA_FORCE_INLINE void unsealCapacityCold( void )
	{
		m_bCapacitySealed = false;
	}

	EA_FORCE_INLINE bool capacitySealed( void ) const
	{
		return m_bCapacitySealed;
	}

	void release( void )
	{
		releaseBacking();
		m_bCapacitySealed = false;
	}

	EA_FORCE_INLINE void reset( void )
	{
		m_iOffset = 0u;
	}

	void *allocate( const size_t nBytes, const size_t nAlignment )
	{
		if ( nBytes == 0u ) {
			return NULL;
		}
		if ( !m_pBase || nAlignment == 0u || ( nAlignment & ( nAlignment - 1u ) ) != 0u ) {
			return NULL;
		}
		const size_t mask = nAlignment - 1u;
		if ( m_iOffset > eastl::numeric_limits<size_t>::max() - mask ) {
			return NULL;
		}
		const size_t aligned_offset = ( m_iOffset + mask ) & ~mask;
		if ( aligned_offset > m_nCapacity || nBytes > m_nCapacity - aligned_offset ) {
			return NULL;
		}
		void *result = m_pBase + aligned_offset;
		m_iOffset = aligned_offset + nBytes;
		return result;
	}

	template <typename T>
	T *allocateArray( const size_t nCount )
	{
		static_assert( eastl::is_trivially_destructible_v<T>, "Linear arena arrays must not require destruction." );
		if ( nCount == 0u || nCount > eastl::numeric_limits<size_t>::max() / sizeof( T ) ) {
			return NULL;
		}
		return (T *)( allocate( nCount * sizeof( T ), MAX( alignof( T ), s_nArenaCachelineAlignment ) ) );
	}

	EA_FORCE_INLINE size_t capacity( void ) const
	{
		return m_nCapacity;
	}

	EA_FORCE_INLINE size_t used( void ) const
	{
		return m_iOffset;
	}

	EA_FORCE_INLINE size_t remaining( void ) const
	{
		return m_nCapacity - m_iOffset;
	}

	EA_FORCE_INLINE bool valid( void ) const
	{
		return m_pBase != NULL || m_nCapacity == 0u;
	}

private:
	void releaseBacking( void )
	{
		if ( m_pRaw ) {
			t3dMemoryRelease( m_pRaw );
		}
		m_pRaw = NULL;
		m_pBase = NULL;
		m_nCapacity = 0u;
		m_iOffset = 0u;
	}

	void *m_pRaw = NULL;
	uint8_t *m_pBase = NULL;
	size_t m_nCapacity = 0u;
	size_t m_iOffset = 0u;
	bool m_bCapacitySealed = false;
};

template <typename T>
class Terrain3DArenaBuffer
{
	static_assert( eastl::is_trivially_copyable_v<T>, "Arena buffers support trivially copyable values only." );
	static_assert( eastl::is_trivially_destructible_v<T>, "Arena buffers support trivially destructible values only." );

public:
	typedef T value_type;
	typedef T *iterator;
	typedef const T *const_iterator;

	Terrain3DArenaBuffer() = default;
	Terrain3DArenaBuffer( const Terrain3DArenaBuffer& ) = delete;
	Terrain3DArenaBuffer& operator=( const Terrain3DArenaBuffer& ) = delete;

	bool allocateUninitialized( Terrain3DLinearArena& rArena, const size_t nCount )
	{
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

	bool assign( Terrain3DLinearArena& rArena, const size_t nCount, const T& rValue )
	{
		if ( !allocateUninitialized( rArena, nCount ) ) {
			return false;
		}
		for ( size_t index = 0u; index < m_nSize; ++index ) {
			m_pData[ index ] = rValue;
		}
		return true;
	}

	EA_FORCE_INLINE void clear( void )
	{
		m_pData = NULL;
		m_nSize = 0u;
	}

	EA_FORCE_INLINE void swap( Terrain3DArenaBuffer& rOther ) noexcept
	{
		eastl::swap( m_pData, rOther.m_pData );
		eastl::swap( m_nSize, rOther.m_nSize );
	}

	bool copyFrom( const Terrain3DArenaBuffer& rOther )
	{
		if ( m_nSize != rOther.m_nSize ) {
			return false;
		}
		if ( m_nSize > 0u ) {
			memcpy( m_pData, rOther.m_pData, m_nSize * sizeof( T ) );
		}
		return true;
	}

	T& operator[]( const size_t iIndex )
	{
		return m_pData[ iIndex ];
	}

	const T& operator[]( const size_t iIndex ) const
	{
		return m_pData[ iIndex ];
	}

	EA_FORCE_INLINE T *data( void )
	{
		return m_pData;
	}

	EA_FORCE_INLINE const T *data( void ) const
	{
		return m_pData;
	}

	EA_FORCE_INLINE iterator begin( void )
	{
		return m_pData;
	}

	EA_FORCE_INLINE const_iterator begin( void ) const
	{
		return m_pData;
	}

	EA_FORCE_INLINE iterator end( void )
	{
		return m_pData ? m_pData + m_nSize : NULL;
	}

	EA_FORCE_INLINE const_iterator end( void ) const
	{
		return m_pData ? m_pData + m_nSize : NULL;
	}

	EA_FORCE_INLINE size_t size( void ) const
	{
		return m_nSize;
	}

	EA_FORCE_INLINE bool empty( void ) const
	{
		return m_nSize == 0u;
	}

private:
	T *m_pData = NULL;
	size_t m_nSize = 0u;
};

#endif // TERRAIN3D_LINEAR_ARENA_H
