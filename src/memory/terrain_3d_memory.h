// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Central allocation backend for Terrain3D-owned native memory.
#ifndef TERRAIN3D_MEMORY_H
#define TERRAIN3D_MEMORY_H

#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
#include "core/terrain_3d_compiler.h"
#ifndef s_nRuntimeAllocationPhaseCount
#define s_nRuntimeAllocationPhaseCount ( 6u )
#endif
#else
#include "Terrain3DConstants.h"
#endif
#include "core/terrain_3d_sync.h"
#include <stddef.h>
#include <stdint.h>


typedef enum t3dRuntimeAllocationPhase_e : uint8_t {
	TERRAIN3D_ALLOCATION_PHASE_INITIALIZATION = 0,
	TERRAIN3D_ALLOCATION_PHASE_COLD_REBUILD,
	TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME,
	TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER,
	TERRAIN3D_ALLOCATION_PHASE_STREAMING_WORKER,
	TERRAIN3D_ALLOCATION_PHASE_SHUTDOWN,
} t3dRuntimeAllocationPhase_t;


typedef struct t3dRuntimeAllocationPhaseStats_s {
	uint64_t m_szEastlEvents[s_nRuntimeAllocationPhaseCount] = {};
	uint64_t m_szEastlBytes[s_nRuntimeAllocationPhaseCount] = {};
	uint64_t m_szZoneEvents[s_nRuntimeAllocationPhaseCount] = {};
	uint64_t m_szZoneBytes[s_nRuntimeAllocationPhaseCount] = {};
	uint64_t m_szRawEvents[s_nRuntimeAllocationPhaseCount] = {};
	uint64_t m_szRawBytes[s_nRuntimeAllocationPhaseCount] = {};
} t3dRuntimeAllocationPhaseStats_t;

inline thread_local t3dRuntimeAllocationPhase_t g_t3dRuntimeAllocationPhase = TERRAIN3D_ALLOCATION_PHASE_COLD_REBUILD;
inline t3dAtomic_t<uint64_t> g_szT3dRuntimeEastlAllocationEvents[s_nRuntimeAllocationPhaseCount] = {};
inline t3dAtomic_t<uint64_t> g_szT3dRuntimeEastlAllocationBytes[s_nRuntimeAllocationPhaseCount] = {};
inline t3dAtomic_t<uint64_t> g_szT3dRuntimeZoneAllocationEvents[s_nRuntimeAllocationPhaseCount] = {};
inline t3dAtomic_t<uint64_t> g_szT3dRuntimeZoneAllocationBytes[s_nRuntimeAllocationPhaseCount] = {};
inline t3dAtomic_t<uint64_t> g_szT3dRuntimeRawAllocationEvents[s_nRuntimeAllocationPhaseCount] = {};
inline t3dAtomic_t<uint64_t> g_szT3dRuntimeRawAllocationBytes[s_nRuntimeAllocationPhaseCount] = {};

inline t3dRuntimeAllocationPhase_t t3dGetRuntimeAllocationPhase( void ) {
	return g_t3dRuntimeAllocationPhase;
}

EA_FORCE_INLINE void t3dRecordRuntimeEastlAllocation( const size_t nBytes ) {
	const uint32_t phase = ( uint32_t )( g_t3dRuntimeAllocationPhase );
	if ( phase < s_nRuntimeAllocationPhaseCount ) {
		g_szT3dRuntimeEastlAllocationEvents[phase].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		g_szT3dRuntimeEastlAllocationBytes[phase].fetch_add( ( uint64_t )( nBytes ), T3D_MEMORY_ORDER_RELAXED );
	}
}

EA_FORCE_INLINE void t3dRecordRuntimeZoneAllocation( const size_t nBytes ) {
	const uint32_t phase = ( uint32_t )( g_t3dRuntimeAllocationPhase );
	if ( phase < s_nRuntimeAllocationPhaseCount ) {
		g_szT3dRuntimeZoneAllocationEvents[phase].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		g_szT3dRuntimeZoneAllocationBytes[phase].fetch_add( ( uint64_t )( nBytes ), T3D_MEMORY_ORDER_RELAXED );
	}
}

EA_FORCE_INLINE void t3dRecordRuntimeRawAllocation( const size_t nBytes ) {
	const uint32_t phase = ( uint32_t )( g_t3dRuntimeAllocationPhase );
	if ( phase < s_nRuntimeAllocationPhaseCount ) {
		g_szT3dRuntimeRawAllocationEvents[phase].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		g_szT3dRuntimeRawAllocationBytes[phase].fetch_add( ( uint64_t )( nBytes ), T3D_MEMORY_ORDER_RELAXED );
	}
}


inline t3dRuntimeAllocationPhaseStats_t t3dGetRuntimeAllocationPhaseStats( void ) {
	t3dRuntimeAllocationPhaseStats_t stats;
	for ( uint32_t phase = 0u; phase < s_nRuntimeAllocationPhaseCount; ++phase ) {
		stats.m_szEastlEvents[phase] = g_szT3dRuntimeEastlAllocationEvents[phase].load( T3D_MEMORY_ORDER_RELAXED );
		stats.m_szEastlBytes[phase] = g_szT3dRuntimeEastlAllocationBytes[phase].load( T3D_MEMORY_ORDER_RELAXED );
		stats.m_szZoneEvents[phase] = g_szT3dRuntimeZoneAllocationEvents[phase].load( T3D_MEMORY_ORDER_RELAXED );
		stats.m_szZoneBytes[phase] = g_szT3dRuntimeZoneAllocationBytes[phase].load( T3D_MEMORY_ORDER_RELAXED );
		stats.m_szRawEvents[phase] = g_szT3dRuntimeRawAllocationEvents[phase].load( T3D_MEMORY_ORDER_RELAXED );
		stats.m_szRawBytes[phase] = g_szT3dRuntimeRawAllocationBytes[phase].load( T3D_MEMORY_ORDER_RELAXED );
	}
	return stats;
}

class Terrain3DAllocationPhaseScope {
public:
	explicit Terrain3DAllocationPhaseScope( const t3dRuntimeAllocationPhase_t ePhase ) : m_ePrevious( g_t3dRuntimeAllocationPhase ) {
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

// Diagnostic phase marker for explicit owner-controlled rebuild/materialization work.
// Phase labels never grant or deny allocation. Fixed buffers/slabs and their owning
// subsystems decide whether backing may grow; allocator layers only service requests,
// enforce their bound domain/capacity, and record phase telemetry.
class Terrain3DColdPhaseScope {
public:
	Terrain3DColdPhaseScope( void ) : m_ePrevious( g_t3dRuntimeAllocationPhase ) {
		g_t3dRuntimeAllocationPhase = TERRAIN3D_ALLOCATION_PHASE_COLD_REBUILD;
	}
	~Terrain3DColdPhaseScope( void ) {
		g_t3dRuntimeAllocationPhase = m_ePrevious;
	}
	Terrain3DColdPhaseScope( const Terrain3DColdPhaseScope & ) = delete;
	Terrain3DColdPhaseScope &operator=( const Terrain3DColdPhaseScope & ) = delete;

private:
	t3dRuntimeAllocationPhase_t m_ePrevious;
};

#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
#include <stdlib.h>
#else
#include <godot_cpp/core/memory.hpp>
#endif

inline void *t3dMemoryAllocate( const size_t nBytes ) {
	if ( nBytes == 0u ) {
		return NULL;
	}
#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
	void *const pMemory = malloc( nBytes );
#else
	void *const pMemory = memalloc( nBytes );
#endif
	if ( pMemory ) {
		t3dRecordRuntimeRawAllocation( nBytes );
	}
	return pMemory;
}

inline void t3dMemoryRelease( void *pPointer ) {
	if ( !pPointer ) {
		return;
	}
#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
	free( pPointer );
#else
	memfree( pPointer );
#endif
}

#endif // TERRAIN3D_MEMORY_H
