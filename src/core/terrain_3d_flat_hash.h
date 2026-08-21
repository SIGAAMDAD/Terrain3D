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

// Small subsystem-oriented open-addressing table. It intentionally exposes only
// the operations Terrain3D's bounded runtime state needs. Storage is contiguous,
// reserve() is high-water only, clear() retains all capacity, and no node
// allocation occurs after the bucket array has been established.
template <typename Key, typename Value, typename Hash, typename Equal = eastl::equal_to<Key>>
class T3DFlatHashMap {
private:
	typedef enum t3dbucketState_e : uint8_t {
		EMPTY = 0u,
		OCCUPIED = 1u,
	} t3dbucketState_t;

	typedef struct t3dbucket_s {
		Key key{};
		Value value{};
		uint8_t state = EMPTY;
	} t3dbucket_t;

	eastl::vector<t3dbucket_t> m_buckets;
	size_t m_nSize = 0u;
	Hash m_hash{};
	Equal m_equal{};

	EA_FORCE_INLINE static size_t nextPowerOfTwo( size_t value ) {
		if ( value <= 8u ) {
			return 8u;
		}
		--value;
		value |= value >> 1u;
		value |= value >> 2u;
		value |= value >> 4u;
		value |= value >> 8u;
		value |= value >> 16u;
		if constexpr ( sizeof( size_t ) >= 8u ) {
			value |= value >> 32u;
		}
		return value + 1u;
	}

	EA_FORCE_INLINE static size_t bucketCountForEntries( const size_t nEntries ) {
		// <= 70% occupancy keeps probe lengths stable without tombstones.
		return nextPowerOfTwo( ( nEntries * 10u + 6u ) / 7u + 1u );
	}

	EA_FORCE_INLINE size_t findBucketIndex( const Key &rKey ) const {
		if ( m_buckets.empty() ) {
			return SIZE_MAX;
		}
		const size_t mask = m_buckets.size() - 1u;
		size_t index = m_hash( rKey ) & mask;
		for ( size_t probe = 0u; probe < m_buckets.size(); ++probe ) {
			const t3dbucket_t &bucket = m_buckets[index];
			if ( bucket.state == EMPTY ) {
				return SIZE_MAX;
			}
			if ( bucket.state == OCCUPIED && m_equal( bucket.key, rKey ) ) {
				return index;
			}
			index = ( index + 1u ) & mask;
		}
		return SIZE_MAX;
	}

	EA_FORCE_INLINE size_t findInsertIndex( const Key &rKey, bool &rExisting ) const {
		rExisting = false;
		if ( m_buckets.empty() ) return SIZE_MAX;
		const size_t mask = m_buckets.size() - 1u;
		size_t index = m_hash( rKey ) & mask;
		for ( size_t probe = 0u; probe < m_buckets.size(); ++probe ) {
			const t3dbucket_t &bucket = m_buckets[index];
			if ( bucket.state == EMPTY ) return index;
			if ( m_equal( bucket.key, rKey ) ) { rExisting = true; return index; }
			index = ( index + 1u ) & mask;
		}
		return SIZE_MAX;
	}

	void rehash( const size_t nBucketCount ) {
		eastl::vector<t3dbucket_t> old;
		old.swap( m_buckets );
		m_buckets.resize( nextPowerOfTwo( nBucketCount ) );
		m_nSize = 0u;
		for ( t3dbucket_t &bucket : old ) {
			if ( bucket.state != OCCUPIED ) {
				continue;
			}
			insertOrAssignNoGrow( eastl::move( bucket.key ), eastl::move( bucket.value ) );
		}
	}

	EA_FORCE_INLINE Value *insertOrAssignNoGrow( Key &&rKey, Value &&rValue ) {
		bool bExisting = false;
		const size_t index = findInsertIndex( rKey, bExisting );
		if ( index == SIZE_MAX ) {
			return nullptr;
		}
		t3dbucket_t &bucket = m_buckets[index];
		if ( !bExisting ) {
			bucket.key = eastl::move( rKey );
			bucket.state = OCCUPIED;
			++m_nSize;
		}
		bucket.value = eastl::move( rValue );
		return &bucket.value;
	}

	EA_FORCE_INLINE void ensureInsertCapacity( void ) {
		if ( m_buckets.empty() ) { rehash( 8u ); return; }
		if ( ( m_nSize + 1u ) * 10u >= m_buckets.size() * 7u ) rehash( m_buckets.size() * 2u );
	}

public:
	EA_FORCE_INLINE void reserve( const size_t nEntries ) {
		const size_t required = bucketCountForEntries( nEntries );
		if ( required > m_buckets.size() ) {
			rehash( required );
		}
	}

	EA_FORCE_INLINE Value *find( const Key &rKey ) {
		const size_t index = findBucketIndex( rKey );
		return index == SIZE_MAX ? nullptr : &m_buckets[index].value;
	}

	EA_FORCE_INLINE const Value *find( const Key &rKey ) const {
		const size_t index = findBucketIndex( rKey );
		return index == SIZE_MAX ? nullptr : &m_buckets[index].value;
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
		const size_t found = findBucketIndex( rKey );
		if ( found == SIZE_MAX ) return false;
		const size_t mask = m_buckets.size() - 1u;
		m_buckets[found].state = EMPTY;
		--m_nSize;
		// Reinsert the remainder of this probe cluster in place. This avoids tombstones
		// and therefore avoids allocation-only cleanup rehashes during residency churn.
		size_t index = ( found + 1u ) & mask;
		while ( m_buckets[index].state == OCCUPIED ) {
			Key key = eastl::move( m_buckets[index].key );
			Value value = eastl::move( m_buckets[index].value );
			m_buckets[index].state = EMPTY;
			--m_nSize;
			insertOrAssignNoGrow( eastl::move( key ), eastl::move( value ) );
			index = ( index + 1u ) & mask;
		}
		return true;
	}

	EA_FORCE_INLINE void clear( void ) {
		for ( t3dbucket_t &bucket : m_buckets ) {
			bucket.state = EMPTY;
		}
		m_nSize = 0u;
	}

	EA_FORCE_INLINE size_t size( void ) const { return m_nSize; }
	EA_FORCE_INLINE size_t bucketCount( void ) const { return m_buckets.size(); }
	EA_FORCE_INLINE size_t reservedBytes( void ) const { return m_buckets.capacity() * sizeof( t3dbucket_t ); }
	EA_FORCE_INLINE bool empty( void ) const { return m_nSize == 0u; }

	EA_FORCE_INLINE void swap( T3DFlatHashMap &rOther ) {
		m_buckets.swap( rOther.m_buckets );
		std::swap( m_nSize, rOther.m_nSize );
	}

	template <typename Fn>
	EA_FORCE_INLINE void forEach( Fn &&rFn ) {
		for ( t3dbucket_t &bucket : m_buckets ) {
			if ( bucket.state == OCCUPIED ) {
				rFn( bucket.key, bucket.value );
			}
		}
	}

	template <typename Fn>
	EA_FORCE_INLINE void forEach( Fn &&rFn ) const {
		for ( const t3dbucket_t &bucket : m_buckets ) {
			if ( bucket.state == OCCUPIED ) {
				rFn( bucket.key, bucket.value );
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
	template <typename Fn> EA_FORCE_INLINE void forEach( Fn &&rFn ) { m_map.forEach( [&]( const Key &rKey, uint8_t & ) { rFn( rKey ); } ); }
	template <typename Fn> EA_FORCE_INLINE void forEach( Fn &&rFn ) const { m_map.forEach( [&]( const Key &rKey, const uint8_t & ) { rFn( rKey ); } ); }
};

#endif // TERRAIN3D_FLAT_HASH_H
