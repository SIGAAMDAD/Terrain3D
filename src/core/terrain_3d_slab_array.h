// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_SLAB_ARRAY_H
#define TERRAIN3D_SLAB_ARRAY_H

#include "eastl_config.h"
#include <EASTL/algorithm.h>
#include <EASTL/vector.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

// High-water contiguous storage shared by many small logical arrays. Individual
// arrays own only an offset/count/capacity tuple. Clearing the owner resets the
// bump cursor while retaining the backing allocation, so cache-slot reuse does
// not recreate hundreds of tiny vector allocations.
template <typename T>
class T3DHighWaterSlab {
	eastl::vector<T> m_storage;
	size_t m_nUsed = 0u;
	size_t m_nGrowCount = 0u;

public:
	T3DHighWaterSlab( void ) = default;
	T3DHighWaterSlab( const T3DHighWaterSlab & ) = delete;
	T3DHighWaterSlab &operator=( const T3DHighWaterSlab & ) = delete;
	T3DHighWaterSlab( T3DHighWaterSlab && ) noexcept = default;
	T3DHighWaterSlab &operator=( T3DHighWaterSlab && ) noexcept = default;

	EA_FORCE_INLINE void reserve( const size_t nCount ) { if ( nCount > m_storage.capacity() ) m_storage.reserve( nCount ); }

	EA_FORCE_INLINE size_t allocate( const size_t nCount ) {
		if ( nCount == 0u || nCount > std::numeric_limits<size_t>::max() - m_nUsed ) return SIZE_MAX;
		const size_t offset = m_nUsed;
		const size_t required = offset + nCount;
		if ( required > m_storage.size() ) {
			m_storage.resize( required );
			++m_nGrowCount;
		}
		m_nUsed = required;
		return offset;
	}

	EA_FORCE_INLINE void reset( void ) { m_nUsed = 0u; }
	EA_FORCE_INLINE void release( void ) { eastl::vector<T>().swap( m_storage ); m_nUsed = 0u; m_nGrowCount = 0u; }
	EA_FORCE_INLINE void compactBacking( void ) { if ( m_storage.size() != m_nUsed ) m_storage.resize( m_nUsed ); m_storage.shrink_to_fit(); }
	EA_FORCE_INLINE T *data( void ) { return m_storage.data(); }
	EA_FORCE_INLINE const T *data( void ) const { return m_storage.data(); }
	EA_FORCE_INLINE size_t used( void ) const { return m_nUsed; }
	EA_FORCE_INLINE size_t size( void ) const { return m_storage.size(); }
	EA_FORCE_INLINE size_t capacity( void ) const { return m_storage.capacity(); }
	EA_FORCE_INLINE size_t growCount( void ) const { return m_nGrowCount; }
	EA_FORCE_INLINE size_t residentBytes( void ) const { return m_storage.capacity() * sizeof( T ); }
	EA_FORCE_INLINE void swap( T3DHighWaterSlab<T> &rOther ) { m_storage.swap( rOther.m_storage ); std::swap( m_nUsed, rOther.m_nUsed ); std::swap( m_nGrowCount, rOther.m_nGrowCount ); }
};

// Non-owning logical array backed by a T3DHighWaterSlab. Growth relocates the
// logical array into a new high-water segment and copies its live elements.
// Old segments are reclaimed wholesale by owner reset/compaction; normal
// runtime mutation never performs per-cell frees.
template <typename T>
class T3DSlabArray {
	T3DHighWaterSlab<T> *m_pSlab = nullptr;
	size_t m_nOffset = 0u;
	size_t m_nCount = 0u;
	size_t m_nCapacity = 0u;

	EA_FORCE_INLINE size_t growthCapacity( const size_t nRequired ) const {
		if ( nRequired <= m_nCapacity ) return m_nCapacity;
		const size_t growth = eastl::max<size_t>( 32u, m_nCapacity / 2u );
		return eastl::max( nRequired, m_nCapacity + growth );
	}

public:
	T3DSlabArray( void ) = default;
	T3DSlabArray( const T3DSlabArray & ) = delete;
	T3DSlabArray &operator=( const T3DSlabArray & ) = delete;
	EA_FORCE_INLINE T3DSlabArray( T3DSlabArray &&rOther ) noexcept : m_pSlab( rOther.m_pSlab ), m_nOffset( rOther.m_nOffset ), m_nCount( rOther.m_nCount ), m_nCapacity( rOther.m_nCapacity ) { rOther.m_pSlab = nullptr; rOther.m_nOffset = 0u; rOther.m_nCount = 0u; rOther.m_nCapacity = 0u; }
	EA_FORCE_INLINE T3DSlabArray &operator=( T3DSlabArray &&rOther ) noexcept { if ( this != &rOther ) { m_pSlab = rOther.m_pSlab; m_nOffset = rOther.m_nOffset; m_nCount = rOther.m_nCount; m_nCapacity = rOther.m_nCapacity; rOther.m_pSlab = nullptr; rOther.m_nOffset = 0u; rOther.m_nCount = 0u; rOther.m_nCapacity = 0u; } return *this; }

	EA_FORCE_INLINE void bind( T3DHighWaterSlab<T> *const pSlab ) { m_pSlab = pSlab; m_nOffset = 0u; m_nCount = 0u; m_nCapacity = 0u; }
	EA_FORCE_INLINE void rebind( T3DHighWaterSlab<T> *const pSlab ) { m_pSlab = pSlab; }
	EA_FORCE_INLINE bool isBound( void ) const { return m_pSlab != nullptr; }

	void reserve( const size_t nCount ) {
		if ( nCount <= m_nCapacity || !m_pSlab ) return;
		const size_t new_capacity = growthCapacity( nCount );
		const size_t old_offset = m_nOffset;
		const size_t old_count = m_nCount;
		const size_t new_offset = m_pSlab->allocate( new_capacity );
		if ( new_offset == SIZE_MAX ) return;
		T *const pData = m_pSlab->data();
		if ( old_count > 0u ) eastl::copy_n( pData + old_offset, old_count, pData + new_offset );
		m_nOffset = new_offset;
		m_nCapacity = new_capacity;
	}

	void reserveExact( const size_t nCount ) {
		if ( nCount <= m_nCapacity || !m_pSlab ) return;
		const size_t old_offset = m_nOffset;
		const size_t old_count = m_nCount;
		const size_t new_offset = m_pSlab->allocate( nCount );
		if ( new_offset == SIZE_MAX ) return;
		T *const pData = m_pSlab->data();
		if ( old_count > 0u ) eastl::copy_n( pData + old_offset, old_count, pData + new_offset );
		m_nOffset = new_offset;
		m_nCapacity = nCount;
	}

	void resize( const size_t nCount ) {
		const size_t old_count = m_nCount;
		reserve( nCount );
		if ( nCount > m_nCapacity ) return;
		if ( nCount > old_count ) eastl::fill( data() + old_count, data() + nCount, T{} );
		m_nCount = nCount;
	}

	void resize( const size_t nCount, const T &rValue ) {
		const size_t old_count = m_nCount;
		reserve( nCount );
		if ( nCount > m_nCapacity ) return;
		if ( nCount > old_count ) eastl::fill( data() + old_count, data() + nCount, rValue );
		m_nCount = nCount;
	}

	EA_FORCE_INLINE void clear( void ) { m_nCount = 0u; }
	EA_FORCE_INLINE void push_back( const T &rValue ) { const size_t index = m_nCount; reserve( index + 1u ); if ( index < m_nCapacity ) { data()[index] = rValue; m_nCount = index + 1u; } }
	EA_FORCE_INLINE void push_back( T &&rValue ) { const size_t index = m_nCount; reserve( index + 1u ); if ( index < m_nCapacity ) { data()[index] = eastl::move( rValue ); m_nCount = index + 1u; } }
	EA_FORCE_INLINE size_t size( void ) const { return m_nCount; }
	EA_FORCE_INLINE size_t capacity( void ) const { return m_nCapacity; }
	EA_FORCE_INLINE bool empty( void ) const { return m_nCount == 0u; }
	EA_FORCE_INLINE T *data( void ) { return m_pSlab && m_nCapacity > 0u ? m_pSlab->data() + m_nOffset : nullptr; }
	EA_FORCE_INLINE const T *data( void ) const { return m_pSlab && m_nCapacity > 0u ? m_pSlab->data() + m_nOffset : nullptr; }
	EA_FORCE_INLINE T &operator[]( const size_t nIndex ) { return data()[nIndex]; }
	EA_FORCE_INLINE const T &operator[]( const size_t nIndex ) const { return data()[nIndex]; }
	EA_FORCE_INLINE T *begin( void ) { return data(); }
	EA_FORCE_INLINE const T *begin( void ) const { return data(); }
	EA_FORCE_INLINE T *end( void ) { return data() ? data() + m_nCount : nullptr; }
	EA_FORCE_INLINE const T *end( void ) const { return data() ? data() + m_nCount : nullptr; }
	EA_FORCE_INLINE size_t offset( void ) const { return m_nOffset; }

	void relocateExact( T3DHighWaterSlab<T> *const pDestination ) {
		if ( !pDestination ) return;
		if ( m_nCount == 0u ) { bind( pDestination ); return; }
		const T *const pSource = data();
		const size_t count = m_nCount;
		const size_t offset = pDestination->allocate( count );
		if ( offset == SIZE_MAX ) return;
		T *const pDestinationData = pDestination->data();
		eastl::copy_n( pSource, count, pDestinationData + offset );
		m_pSlab = pDestination;
		m_nOffset = offset;
		m_nCount = count;
		m_nCapacity = count;
	}
};

#endif // TERRAIN3D_SLAB_ARRAY_H
