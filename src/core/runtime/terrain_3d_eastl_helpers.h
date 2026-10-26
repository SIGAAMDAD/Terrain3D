// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Terrain3D container policy: call sites use EASTL container types directly.
// Terrain3D supplies allocator routing plus small fail-closed helper functions
// for places where hot runtime code must not grow established backing implicitly.
#ifndef TERRAIN3D_EASTL_HELPERS_H
#define TERRAIN3D_EASTL_HELPERS_H

#include "eastl_config.h"

#include <EASTL/algorithm.h>
#include <EASTL/sort.h>
#include <EASTL/array.h>
#include <EASTL/bitset.h>
#include <EASTL/fixed_vector.h>
#include <EASTL/vector.h>
#include <EASTL/vector_map.h>
#include <EASTL/vector_set.h>
#include <EASTL/utility.h>
#include <stddef.h>

// EASTL helper functions -----------------------------------------------------
// Call sites spell EASTL container types directly. This header provides only
// allocator-aware capacity/policy helpers; it defines no container aliases.
// EASTL vector helpers -------------------------------------------------------
// These helpers preserve the former explicit-capacity/fail-closed call sites
// without maintaining a second vector implementation.
template< typename C >
EA_FORCE_INLINE bool t3dContainerReserveCold(
	C			&rContainer,
	const size_t nCapacity ) {
	if ( nCapacity <= rContainer.capacity() ) {
		return true;
	}
	if ( nCapacity > SIZE_MAX / sizeof( typename C::value_type ) ) {
		return false;
	}
	// Bootstrap admission below is atomic and records budget denials. A separate
	// advisory headroom probe would hide those denials from authoring retries.
	if ( !rContainer.get_allocator().isBootstrap() &&
		 !rContainer.get_allocator().canAllocate( nCapacity * sizeof( typename C::value_type ),
												  alignof( typename C::value_type ) ) ) {
		return false;
	}
	const size_t alignment = MAX( size_t( alignof( typename C::value_type ) ), size_t( T3D_EASTL_DEFAULT_ALIGNMENT ) );
	const size_t overhead = sizeof( t3dEastlAllocationHeader_t ) + alignment - 1u + sizeof( t3dNativeMemoryHeader );
	const size_t requested = nCapacity * sizeof( typename C::value_type );
	if ( requested > SIZE_MAX - overhead ) {
		return false;
	}
	Terrain3DMemoryAllocationPermit permit( rContainer.get_allocator().isBootstrap() ? requested + overhead : 0u );
	if ( !permit.valid() ) {
		return false;
	}
	rContainer.reserve( nCapacity );
	return rContainer.capacity() >= nCapacity;
}

template< typename V >
EA_FORCE_INLINE bool t3dVectorResizeCold(
	V			&rVector,
	const size_t nCount ) {
	if ( !t3dContainerReserveCold( rVector, nCount ) ) {
		return false;
	}
	rVector.resize( nCount );
	return rVector.size() == nCount;
}

template< typename V >
EA_FORCE_INLINE bool t3dVectorResizeWithinCapacity(
	V			&rVector,
	const size_t nCount ) {
	if ( nCount > rVector.capacity() ) {
		return false;
	}
	rVector.resize( nCount );
	return rVector.size() == nCount;
}

template< typename V, typename T >
EA_FORCE_INLINE bool t3dVectorResizeCold(
	V			&rVector,
	const size_t nCount,
	const T		&rValue ) {
	if ( !t3dContainerReserveCold( rVector, nCount ) ) {
		return false;
	}
	rVector.resize( nCount, rValue );
	return rVector.size() == nCount;
}

template< typename V, typename T >
EA_FORCE_INLINE bool t3dVectorPushBackWithinCapacity(
	V  &rVector,
	T &&rValue ) {
	if ( rVector.size() >= rVector.capacity() ) {
		return false;
	}
	rVector.push_back( eastl::forward< T >( rValue ) );
	return true;
}

template< typename V >
EA_FORCE_INLINE bool t3dVectorAppendWithinCapacity(
	V							 &rVector,
	const typename V::value_type *pValues,
	const size_t				  nCount ) {
	if ( nCount == 0u ) {
		return true;
	}
	if ( !pValues || nCount > rVector.capacity() - rVector.size() ) {
		return false;
	}
	for ( size_t i = 0u; i < nCount; ++i ) {
		rVector.push_back( pValues[i] );
	}
	return true;
}

template< typename V, typename T >
EA_FORCE_INLINE bool t3dVectorInsertWithinCapacity(
	V			&rVector,
	const size_t iIndex,
	T		   &&rValue ) {
	if ( iIndex > rVector.size() || rVector.size() >= rVector.capacity() ) {
		return false;
	}
	rVector.insert( rVector.begin() + ( typename V::difference_type )( iIndex ), eastl::forward< T >( rValue ) );
	return true;
}

template< typename V >
EA_FORCE_INLINE void t3dVectorEraseOrdered(
	V			&rVector,
	const size_t iIndex ) {
	if ( iIndex < rVector.size() ) {
		rVector.erase( rVector.begin() + ( typename V::difference_type )( iIndex ) );
	}
}

template< typename V >
EA_FORCE_INLINE bool t3dVectorCopyWithinCapacity(
	V		&rDestination,
	const V &rSource ) {
	if ( rSource.size() > rDestination.capacity() ) {
		return false;
	}
	rDestination.assign( rSource.begin(), rSource.end() );
	return rDestination.size() == rSource.size();
}

template< typename V >
EA_FORCE_INLINE bool t3dVectorCopyCold(
	V		&rDestination,
	const V &rSource ) {
	if ( !t3dContainerReserveCold( rDestination, rSource.size() ) ) {
		return false;
	}
	rDestination.assign( rSource.begin(), rSource.end() );
	return rDestination.size() == rSource.size();
}

template< typename V >
EA_FORCE_INLINE void t3dVectorRelease(
	V &rVector ) {
	rVector.clear();
	rVector.set_capacity( 0u );
}

template< typename V >
EA_FORCE_INLINE size_t t3dContainerResidentBytes(
	const V &rContainer ) {
	return rContainer.capacity() * sizeof( typename V::value_type );
}

// EASTL vector_map/vector_set helpers ---------------------------------------
template< typename M, typename K >
EA_FORCE_INLINE typename M::mapped_type *t3dMapFind(
	M		&rMap,
	const K &rKey ) {
	typename M::iterator it = rMap.find( rKey );
	return it == rMap.end() ? NULL : &it->second;
}

template< typename M, typename K >
EA_FORCE_INLINE const typename M::mapped_type *t3dMapFind(
	const M &rMap,
	const K &rKey ) {
	typename M::const_iterator it = rMap.find( rKey );
	return it == rMap.end() ? NULL : &it->second;
}

template< typename M, typename K, typename V >
EA_FORCE_INLINE typename M::mapped_type *t3dMapInsertOrAssignWithinCapacity(
	M		&rMap,
	const K &rKey,
	V	   &&rValue ) {
	typename M::iterator it = rMap.find( rKey );
	if ( it != rMap.end() ) {
		it->second = eastl::forward< V >( rValue );
		return &it->second;
	}
	if ( rMap.size() >= rMap.capacity() ) {
		return NULL;
	}
	eastl::pair< typename M::iterator, bool > result =
		rMap.insert( typename M::value_type( rKey, eastl::forward< V >( rValue ) ) );
	return result.second ? &result.first->second : NULL;
}

template< typename M, typename K >
EA_FORCE_INLINE typename M::mapped_type *t3dMapGetOrInsertWithinCapacity(
	M		&rMap,
	const K &rKey ) {
	typename M::iterator it = rMap.find( rKey );
	if ( it != rMap.end() ) {
		return &it->second;
	}
	if ( rMap.size() >= rMap.capacity() ) {
		return NULL;
	}
	eastl::pair< typename M::iterator, bool > result =
		rMap.insert( typename M::value_type( rKey, typename M::mapped_type{} ) );
	return result.second ? &result.first->second : NULL;
}

template< typename C, typename K >
EA_FORCE_INLINE bool t3dContainerContains(
	const C &rContainer,
	const K &rKey ) {
	return rContainer.find( rKey ) != rContainer.end();
}

template< typename S, typename K >
EA_FORCE_INLINE bool t3dSetInsertWithinCapacity(
	S		&rSet,
	const K &rKey ) {
	if ( rSet.find( rKey ) != rSet.end() ) {
		return true;
	}
	if ( rSet.size() >= rSet.capacity() ) {
		return false;
	}
	return rSet.insert( rKey ).second;
}

#endif // TERRAIN3D_EASTL_HELPERS_H
