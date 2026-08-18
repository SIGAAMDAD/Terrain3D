// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Central allocation backend for Terrain3D-owned native memory.
#pragma once

#ifndef TERRAIN3D_MEMORY_H
#define TERRAIN3D_MEMORY_H

#include <cstddef>

#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
#include <cstdlib>
#else
#include <godot_cpp/core/memory.hpp>
#endif

namespace terrain3d_memory {
inline void *allocate( const size_t nBytes ) {
#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
	return std::malloc( nBytes );
#else
	return memalloc( nBytes );
#endif
}

inline void *reallocate( void *pPointer, const size_t nBytes ) {
#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
	return std::realloc( pPointer, nBytes );
#else
	return memrealloc( pPointer, nBytes );
#endif
}

inline void release( void *pPointer ) {
	if ( !pPointer ) {
		return;
	}
#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
	std::free( pPointer );
#else
	memfree( pPointer );
#endif
}
} // namespace terrain3d_memory

#endif // TERRAIN3D_MEMORY_H
