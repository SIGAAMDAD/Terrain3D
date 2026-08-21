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

inline void *t3dMemoryAllocate( const size_t nBytes ) {
#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
	return std::malloc( nBytes );
#else
	return memalloc( nBytes );
#endif
}

inline void t3dMemoryRelease( void *pPointer ) {
	if ( !pPointer ) {
		return;
	}
#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
	std::free( pPointer );
#else
	memfree( pPointer );
#endif
}

#endif // TERRAIN3D_MEMORY_H
