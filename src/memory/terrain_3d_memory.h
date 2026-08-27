// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Central allocation backend for Terrain3D-owned native memory.
#pragma once

#ifndef TERRAIN3D_MEMORY_H
#define TERRAIN3D_MEMORY_H

#include <cassert>
#include <cstddef>
#include <cstdint>


typedef enum t3dRuntimeAllocationPhase_e : uint8_t {
	TERRAIN3D_ALLOCATION_PHASE_INITIALIZATION = 0,
	TERRAIN3D_ALLOCATION_PHASE_COLD_REBUILD,
	TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME,
	TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER,
	TERRAIN3D_ALLOCATION_PHASE_STREAMING_WORKER,
	TERRAIN3D_ALLOCATION_PHASE_SHUTDOWN,
} t3dRuntimeAllocationPhase_t;

inline thread_local t3dRuntimeAllocationPhase_t g_t3dRuntimeAllocationPhase = TERRAIN3D_ALLOCATION_PHASE_COLD_REBUILD;

inline t3dRuntimeAllocationPhase_t t3dGetRuntimeAllocationPhase( void ) {
	return g_t3dRuntimeAllocationPhase;
}

inline bool t3dRuntimeAllocationPhaseAllowsAllocation( const t3dRuntimeAllocationPhase_t ePhase ) {
	return ePhase == TERRAIN3D_ALLOCATION_PHASE_INITIALIZATION || ePhase == TERRAIN3D_ALLOCATION_PHASE_COLD_REBUILD ||
			ePhase == TERRAIN3D_ALLOCATION_PHASE_SHUTDOWN;
}

inline bool t3dValidateRuntimeAllocationRequest( void ) {
	if ( t3dRuntimeAllocationPhaseAllowsAllocation( g_t3dRuntimeAllocationPhase ) ) {
		return true;
	}
#if !defined( NDEBUG )
	assert( false && "Terrain3D-owned allocation attempted from a hot runtime phase" );
#endif
	return false;
}

class Terrain3DAllocationPhaseScope {
public:
	explicit Terrain3DAllocationPhaseScope( const t3dRuntimeAllocationPhase_t ePhase ) :
			m_ePrevious( g_t3dRuntimeAllocationPhase ) {
		g_t3dRuntimeAllocationPhase = ePhase;
	}
	~Terrain3DAllocationPhaseScope( void ) {
		g_t3dRuntimeAllocationPhase = m_ePrevious;
	}
	Terrain3DAllocationPhaseScope( const Terrain3DAllocationPhaseScope & ) = delete;
	Terrain3DAllocationPhaseScope &operator=( const Terrain3DAllocationPhaseScope & ) = delete;

private:
	t3dRuntimeAllocationPhase_t m_ePrevious;
};

#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
#include <cstdlib>
#else
#include <godot_cpp/core/memory.hpp>
#endif

inline void *t3dMemoryAllocate( const size_t nBytes ) {
	if ( nBytes == 0u || !t3dValidateRuntimeAllocationRequest() ) {
		return nullptr;
	}
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
