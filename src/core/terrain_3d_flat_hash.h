// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_FLAT_HASH_H
#define TERRAIN3D_FLAT_HASH_H

#include "eastl_config.h"

#include <EASTL/functional.h>
#include <EASTL/utility.h>
#include <EASTL/vector.h>
#include <cstddef>
#include <cstdint>
#include <utility>

typedef enum t3dFlatHashBucketState_e : uint8_t {
	T3D_FLAT_HASH_BUCKET_EMPTY = 0u,
	T3D_FLAT_HASH_BUCKET_OCCUPIED = 1u,
} t3dFlatHashBucketState_t;

// Compact open-addressing storage for bounded Terrain3D runtime tables. The map
// retains capacity after clear() and performs no node allocations.
template <typename Key, typename Value, typename Hash, typename Equal = eastl::equal_to<Key>>
class T3DFlatHashMap {
private:
	eastl::vector<Key> m_keys;
	eastl::vector<Value> m_values;
	eastl::vector<uint8_t> m_states;
	size_t m_nSize = 0u;
	Hash m_hash{};
	Equal m_equal{};

	EA_FORCE_INLINE static size_t nextPowerOfTwo( size_t nValue ) {
		if ( nValue <= 8u ) {
			return 8u;
		}
		--nValue;
		nValue |= nValue >> 1u;
		nValue |= nValue >> 2u;
		nValue |= nValue >> 4u;
		nValue |= nValue >> 8u;
		nValue |= nValue >> 16u;
		if constexpr ( sizeof( size_t ) >= 8u ) {
			nValue |= nValue >> 32u;
		}
		return nValue + 1u;
	}

	EA_FORCE_INLINE static size_t bucketCountForEntries( const size_t nEntries ) {
		// Keep occupancy at or below 70% so linear-probe chains remain short.
		return nextPowerOfTwo( ( nEntries * 10u + 6u ) / 7u + 1u );
	}

	EA_FORCE_INLINE size_t findBucketIndex( const Key &rKey ) const {
		if ( m_states.empty() ) {
			return SIZE_MAX;
		}
		const size_t nMask = m_states.size() - 1u;
		size_t iBucket = m_hash( rKey ) & nMask;
		for ( size_t nProbe = 0u; nProbe < m_states.size(); ++nProbe ) {
			if ( m_states[iBucket] == T3D_FLAT_HASH_BUCKET_EMPTY ) {
				return SIZE_MAX;
			}
			if ( m_states[iBucket] == T3D_FLAT_HASH_BUCKET_OCCUPIED && m_equal( m_keys[iBucket], rKey ) ) {
				return iBucket;
			}
			iBucket = ( iBucket + 1u ) & nMask;
		}
		return SIZE_MAX;
	}

	EA_FORCE_INLINE size_t findInsertIndex( const Key &rKey, bool &rExisting ) const {
		rExisting = false;
		if ( m_states.empty() ) {
			return SIZE_MAX;
		}
		const size_t nMask = m_states.size() - 1u;
		size_t iBucket = m_hash( rKey ) & nMask;
		for ( size_t nProbe = 0u; nProbe < m_states.size(); ++nProbe ) {
			if ( m_states[iBucket] == T3D_FLAT_HASH_BUCKET_EMPTY ) {
				return iBucket;
			}
			if ( m_equal( m_keys[iBucket], rKey ) ) {
				rExisting = true;
				return iBucket;
			}
			iBucket = ( iBucket + 1u ) & nMask;
		}
		return SIZE_MAX;
	}

	void rehash( const size_t nBucketCount ) {
		eastl::vector<Key> oldKeys;
		eastl::vector<Value> oldValues;
		eastl::vector<uint8_t> oldStates;
		oldKeys.swap( m_keys );
		oldValues.swap( m_values );
		oldStates.swap( m_states );

		const size_t nNewCount = nextPowerOfTwo( nBucketCount );
		m_keys.resize( nNewCount );
		m_values.resize( nNewCount );
		m_states.resize( nNewCount, T3D_FLAT_HASH_BUCKET_EMPTY );
		m_nSize = 0u;

		for ( size_t iBucket = 0u; iBucket < oldStates.size(); ++iBucket ) {
			if ( oldStates[iBucket] != T3D_FLAT_HASH_BUCKET_OCCUPIED ) {
				continue;
			}
			insertOrAssignNoGrow( eastl::move( oldKeys[iBucket] ), eastl::move( oldValues[iBucket] ) );
		}
	}

	EA_FORCE_INLINE Value *insertOrAssignNoGrow( Key &&rKey, Value &&rValue ) {
		bool bExisting = false;
		const size_t iBucket = findInsertIndex( rKey, bExisting );
		if ( iBucket == SIZE_MAX ) {
			return nullptr;
		}
		if ( !bExisting ) {
			m_keys[iBucket] = eastl::move( rKey );
			m_states[iBucket] = T3D_FLAT_HASH_BUCKET_OCCUPIED;
			++m_nSize;
		}
		m_values[iBucket] = eastl::move( rValue );
		return &m_values[iBucket];
	}

	EA_FORCE_INLINE void ensureInsertCapacity( void ) {
		if ( m_states.empty() ) {
			rehash( 8u );
			return;
		}
		if ( ( m_nSize + 1u ) * 10u >= m_states.size() * 7u ) {
			rehash( m_states.size() * 2u );
		}
	}

public:
	EA_FORCE_INLINE void reserve( const size_t nEntries ) {
		const size_t nRequired = bucketCountForEntries( nEntries );
		if ( nRequired > m_states.size() ) {
			rehash( nRequired );
		}
	}

	EA_FORCE_INLINE Value *find( const Key &rKey ) {
		const size_t iBucket = findBucketIndex( rKey );
		return iBucket == SIZE_MAX ? nullptr : &m_values[iBucket];
	}

	EA_FORCE_INLINE const Value *find( const Key &rKey ) const {
		const size_t iBucket = findBucketIndex( rKey );
		return iBucket == SIZE_MAX ? nullptr : &m_values[iBucket];
	}

	EA_FORCE_INLINE bool contains( const Key &rKey ) const { return findBucketIndex( rKey ) != SIZE_MAX; }

	EA_FORCE_INLINE Value *insertOrAssign( const Key &rKey, const Value &rValue ) {
		ensureInsertCapacity();
		Key key = rKey;
		Value value = rValue;
		return insertOrAssignNoGrow( eastl::move( key ), eastl::move( value ) );
	}

	EA_FORCE_INLINE Value *insertOrAssign( Key &&rKey, Value &&rValue ) {
		ensureInsertCapacity();
		return insertOrAssignNoGrow( eastl::move( rKey ), eastl::move( rValue ) );
	}

	EA_FORCE_INLINE Value *getOrInsert( const Key &rKey, const Value &rDefault = Value{} ) {
		if ( Value *const pValue = find( rKey ) ) {
			return pValue;
		}
		return insertOrAssign( rKey, rDefault );
	}

	EA_FORCE_INLINE bool erase( const Key &rKey ) {
		const size_t iFound = findBucketIndex( rKey );
		if ( iFound == SIZE_MAX ) {
			return false;
		}

		const size_t nMask = m_states.size() - 1u;
		m_states[iFound] = T3D_FLAT_HASH_BUCKET_EMPTY;
		--m_nSize;

		// Reinsert the remainder of the probe cluster so no tombstone cleanup is required.
		size_t iBucket = ( iFound + 1u ) & nMask;
		while ( m_states[iBucket] == T3D_FLAT_HASH_BUCKET_OCCUPIED ) {
			Key key = eastl::move( m_keys[iBucket] );
			Value value = eastl::move( m_values[iBucket] );
			m_states[iBucket] = T3D_FLAT_HASH_BUCKET_EMPTY;
			--m_nSize;
			insertOrAssignNoGrow( eastl::move( key ), eastl::move( value ) );
			iBucket = ( iBucket + 1u ) & nMask;
		}
		return true;
	}

	EA_FORCE_INLINE void clear( void ) {
		for ( uint8_t &nState : m_states ) {
			nState = T3D_FLAT_HASH_BUCKET_EMPTY;
		}
		m_nSize = 0u;
	}

	EA_FORCE_INLINE size_t size( void ) const { return m_nSize; }
	EA_FORCE_INLINE size_t bucketCount( void ) const { return m_states.size(); }
	EA_FORCE_INLINE size_t reservedBytes( void ) const {
		return m_keys.capacity() * sizeof( Key ) + m_values.capacity() * sizeof( Value ) +
			m_states.capacity() * sizeof( uint8_t );
	}
	EA_FORCE_INLINE bool empty( void ) const { return m_nSize == 0u; }

	EA_FORCE_INLINE void swap( T3DFlatHashMap &rOther ) {
		m_keys.swap( rOther.m_keys );
		m_values.swap( rOther.m_values );
		m_states.swap( rOther.m_states );
		std::swap( m_nSize, rOther.m_nSize );
		std::swap( m_hash, rOther.m_hash );
		std::swap( m_equal, rOther.m_equal );
	}

	template <typename Fn>
	EA_FORCE_INLINE void forEach( Fn &&rFn ) {
		for ( size_t iBucket = 0u; iBucket < m_states.size(); ++iBucket ) {
			if ( m_states[iBucket] == T3D_FLAT_HASH_BUCKET_OCCUPIED ) {
				rFn( m_keys[iBucket], m_values[iBucket] );
			}
		}
	}

	template <typename Fn>
	EA_FORCE_INLINE void forEach( Fn &&rFn ) const {
		for ( size_t iBucket = 0u; iBucket < m_states.size(); ++iBucket ) {
			if ( m_states[iBucket] == T3D_FLAT_HASH_BUCKET_OCCUPIED ) {
				rFn( m_keys[iBucket], m_values[iBucket] );
			}
		}
	}
};

template <typename Key, typename Hash, typename Equal = eastl::equal_to<Key>>
class T3DFlatHashSet {
private:
	T3DFlatHashMap<Key, uint8_t, Hash, Equal> m_map;

public:
	EA_FORCE_INLINE void reserve( const size_t nEntries ) { m_map.reserve( nEntries ); }
	EA_FORCE_INLINE bool insert( const Key &rKey ) {
		if ( m_map.contains( rKey ) ) {
			return false;
		}
		m_map.insertOrAssign( rKey, 1u );
		return true;
	}
	EA_FORCE_INLINE bool erase( const Key &rKey ) { return m_map.erase( rKey ); }
	EA_FORCE_INLINE bool contains( const Key &rKey ) const { return m_map.contains( rKey ); }
	EA_FORCE_INLINE void clear( void ) { m_map.clear(); }
	EA_FORCE_INLINE size_t size( void ) const { return m_map.size(); }
	EA_FORCE_INLINE size_t bucketCount( void ) const { return m_map.bucketCount(); }
	EA_FORCE_INLINE size_t reservedBytes( void ) const { return m_map.reservedBytes(); }
	EA_FORCE_INLINE bool empty( void ) const { return m_map.empty(); }
	EA_FORCE_INLINE void swap( T3DFlatHashSet &rOther ) { m_map.swap( rOther.m_map ); }

	template <typename Fn>
	EA_FORCE_INLINE void forEach( Fn &&rFn ) {
		m_map.forEach( [&]( const Key &rKey, uint8_t & ) { rFn( rKey ); } );
	}

	template <typename Fn>
	EA_FORCE_INLINE void forEach( Fn &&rFn ) const {
		m_map.forEach( [&]( const Key &rKey, const uint8_t & ) { rFn( rKey ); } );
	}
};

#endif // TERRAIN3D_FLAT_HASH_H
