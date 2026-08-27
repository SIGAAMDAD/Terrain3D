// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_SLAB_ARRAY_H
#define TERRAIN3D_SLAB_ARRAY_H

#include "eastl_config.h"

#include <EASTL/algorithm.h>
#include <EASTL/vector.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

// High-water contiguous storage shared by many small logical arrays. Individual
// arrays own only an offset/count/capacity tuple. Clearing the owner resets the
// bump cursor while retaining the backing allocation, so cache-slot reuse does
// not recreate hundreds of tiny vector allocations.
template <typename T> class Terrain3DHighWaterSlab {
	static_assert( std::is_trivially_copyable_v<T>, "Runtime slab storage requires trivially copyable values." );

	eastl::vector<T> m_storage;
	size_t m_nUsed = 0u;
	size_t m_nGrowCount = 0u;
	bool m_bSealed = false;

public:
	Terrain3DHighWaterSlab( void ) = default;
	Terrain3DHighWaterSlab( const Terrain3DHighWaterSlab & ) = delete;
	Terrain3DHighWaterSlab &operator=( const Terrain3DHighWaterSlab & ) = delete;
	Terrain3DHighWaterSlab( Terrain3DHighWaterSlab && ) noexcept = default;
	Terrain3DHighWaterSlab &operator=( Terrain3DHighWaterSlab && ) noexcept = default;

	EA_FORCE_INLINE bool reserve( const size_t nCount ) {
		if ( nCount <= m_storage.size() ) {
			return true;
		}
		if ( m_bSealed ) {
			return false;
		}
		m_storage.resize( nCount );
		++m_nGrowCount;
		return true;
	}

	EA_FORCE_INLINE bool seal( void ) {
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
		if ( nCount == 0u || nCount > std::numeric_limits<size_t>::max() - m_nUsed ) {
			return SIZE_MAX;
		}
		const size_t offset = m_nUsed;
		const size_t required = offset + nCount;
		if ( required > m_storage.size() ) {
			if ( m_bSealed ) {
				return SIZE_MAX;
			}
			m_storage.resize( required );
			++m_nGrowCount;
		}
		m_nUsed = required;
		return offset;
	}

	EA_FORCE_INLINE void reset( void ) {
		m_nUsed = 0u;
	}
	EA_FORCE_INLINE void release( void ) {
		eastl::vector<T>().swap( m_storage );
		m_nUsed = 0u;
		m_nGrowCount = 0u;
		m_bSealed = false;
	}
	EA_FORCE_INLINE bool compactBacking( void ) {
		if ( m_bSealed ) {
			return false;
		}
		if ( m_storage.size() != m_nUsed ) {
			m_storage.resize( m_nUsed );
		}
		m_storage.shrink_to_fit();
		return true;
	}
	EA_FORCE_INLINE T *data( void ) {
		return m_storage.data();
	}
	EA_FORCE_INLINE const T *data( void ) const {
		return m_storage.data();
	}
	EA_FORCE_INLINE size_t used( void ) const {
		return m_nUsed;
	}
	EA_FORCE_INLINE size_t size( void ) const {
		return m_storage.size();
	}
	EA_FORCE_INLINE size_t capacity( void ) const {
		return m_storage.capacity();
	}
	EA_FORCE_INLINE size_t growCount( void ) const {
		return m_nGrowCount;
	}
	EA_FORCE_INLINE size_t residentBytes( void ) const {
		return m_storage.capacity() * sizeof( T );
	}
	EA_FORCE_INLINE void swap( Terrain3DHighWaterSlab<T> &rOther ) {
		m_storage.swap( rOther.m_storage );
		std::swap( m_nUsed, rOther.m_nUsed );
		std::swap( m_nGrowCount, rOther.m_nGrowCount );
		std::swap( m_bSealed, rOther.m_bSealed );
	}
};

// Non-owning logical array backed by a Terrain3DHighWaterSlab. Growth relocates the
// logical array into a new high-water segment and copies its live elements.
// Old segments are reclaimed wholesale by owner reset/compaction; normal
// runtime mutation never performs per-cell frees.
template <typename T> class Terrain3DSlabArray {
	Terrain3DHighWaterSlab<T> *m_pSlab = nullptr;
	size_t m_iOffset = 0u;
	size_t m_nCount = 0u;
	size_t m_nCapacity = 0u;

	EA_FORCE_INLINE size_t growthCapacity( const size_t nRequired ) const {
		if ( nRequired <= m_nCapacity ) {
			return m_nCapacity;
		}
		const size_t growth = eastl::max<size_t>( 32u, m_nCapacity / 2u );
		return eastl::max( nRequired, m_nCapacity + growth );
	}

public:
	Terrain3DSlabArray( void ) = default;
	Terrain3DSlabArray( const Terrain3DSlabArray & ) = delete;
	Terrain3DSlabArray &operator=( const Terrain3DSlabArray & ) = delete;
	EA_FORCE_INLINE Terrain3DSlabArray( Terrain3DSlabArray &&rOther ) noexcept :
			m_pSlab( rOther.m_pSlab ),
			m_iOffset( rOther.m_iOffset ),
			m_nCount( rOther.m_nCount ),
			m_nCapacity( rOther.m_nCapacity ) {
		rOther.m_pSlab = nullptr;
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
			rOther.m_pSlab = nullptr;
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
		return m_pSlab != nullptr;
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
			std::memmove( pData + new_offset, pData + old_offset, old_count * sizeof( T ) );
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
			std::memmove( pData + new_offset, pData + old_offset, old_count * sizeof( T ) );
		}
		m_iOffset = new_offset;
		m_nCapacity = nCount;
		return true;
	}

	bool resize( const size_t nCount ) {
		const size_t old_count = m_nCount;
		if ( !reserve( nCount ) ) {
			return false;
		}
		if ( nCount > old_count ) {
			std::memset( data() + old_count, 0, ( nCount - old_count ) * sizeof( T ) );
		}
		m_nCount = nCount;
		return true;
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
		return m_pSlab && m_nCapacity > 0u ? m_pSlab->data() + m_iOffset : nullptr;
	}
	EA_FORCE_INLINE const T *data( void ) const {
		return m_pSlab && m_nCapacity > 0u ? m_pSlab->data() + m_iOffset : nullptr;
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
		return data() ? data() + m_nCount : nullptr;
	}
	EA_FORCE_INLINE const T *end( void ) const {
		return data() ? data() + m_nCount : nullptr;
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
		std::memcpy( pDestinationData + offset, pSource, count * sizeof( T ) );
		m_pSlab = pDestination;
		m_iOffset = offset;
		m_nCount = count;
		m_nCapacity = count;
		return true;
	}
};

#endif // TERRAIN3D_SLAB_ARRAY_H
