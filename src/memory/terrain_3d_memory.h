// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Central allocation backend for Terrain3D-owned native memory.
#ifndef TERRAIN3D_MEMORY_H
#define TERRAIN3D_MEMORY_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include <assert.h>
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
	uint64_t m_szValidationChecks[s_nRuntimeAllocationPhaseCount] = {};
	uint64_t m_szDeniedChecks[s_nRuntimeAllocationPhaseCount] = {};
	uint64_t m_nExplicitColdOverrides = 0u;
	uint64_t m_nRestrictedColdOverrides = 0u;
} t3dRuntimeAllocationPhaseStats_t;

inline thread_local t3dRuntimeAllocationPhase_t g_t3dRuntimeAllocationPhase = TERRAIN3D_ALLOCATION_PHASE_COLD_REBUILD;
inline thread_local bool g_t3dRuntimeAllocationPermission = true;
inline t3dAtomic_t<uint64_t> g_szT3dRuntimeAllocationValidationChecks[s_nRuntimeAllocationPhaseCount] = {};
inline t3dAtomic_t<uint64_t> g_szT3dRuntimeAllocationDeniedChecks[s_nRuntimeAllocationPhaseCount] = {};
inline t3dAtomic_t<uint64_t> g_nT3dRuntimeExplicitColdOverrides{ 0u };
inline t3dAtomic_t<uint64_t> g_nT3dRuntimeRestrictedColdOverrides{ 0u };

inline t3dRuntimeAllocationPhase_t t3dGetRuntimeAllocationPhase( void ) {
	return g_t3dRuntimeAllocationPhase;
}

inline bool t3dRuntimeAllocationPhaseAllowsAllocation( const t3dRuntimeAllocationPhase_t ePhase ) {
	return ePhase == TERRAIN3D_ALLOCATION_PHASE_INITIALIZATION || ePhase == TERRAIN3D_ALLOCATION_PHASE_COLD_REBUILD
	        || ePhase == TERRAIN3D_ALLOCATION_PHASE_SHUTDOWN;
}

inline bool t3dValidateRuntimeAllocationRequest( void ) {
	const uint32_t phase = ( uint32_t )( g_t3dRuntimeAllocationPhase );
	if ( phase < s_nRuntimeAllocationPhaseCount ) {
		g_szT3dRuntimeAllocationValidationChecks[phase].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	if ( g_t3dRuntimeAllocationPermission ) {
		return true;
	}
	if ( phase < s_nRuntimeAllocationPhaseCount ) {
		g_szT3dRuntimeAllocationDeniedChecks[phase].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
#if !defined( NDEBUG )
	assert( false && "Terrain3D-owned allocation attempted from a hot runtime phase" );
#endif
	return false;
}

inline t3dRuntimeAllocationPhaseStats_t t3dGetRuntimeAllocationPhaseStats( void ) {
	t3dRuntimeAllocationPhaseStats_t stats;
	for ( uint32_t phase = 0u; phase < s_nRuntimeAllocationPhaseCount; ++phase ) {
		stats.m_szValidationChecks[phase] = g_szT3dRuntimeAllocationValidationChecks[phase].load( T3D_MEMORY_ORDER_RELAXED );
		stats.m_szDeniedChecks[phase] = g_szT3dRuntimeAllocationDeniedChecks[phase].load( T3D_MEMORY_ORDER_RELAXED );
	}
	stats.m_nExplicitColdOverrides = g_nT3dRuntimeExplicitColdOverrides.load( T3D_MEMORY_ORDER_RELAXED );
	stats.m_nRestrictedColdOverrides = g_nT3dRuntimeRestrictedColdOverrides.load( T3D_MEMORY_ORDER_RELAXED );
	return stats;
}

class Terrain3DAllocationPhaseScope {
public:
	explicit Terrain3DAllocationPhaseScope( const t3dRuntimeAllocationPhase_t ePhase ) :
	        m_ePrevious( g_t3dRuntimeAllocationPhase ), m_bPreviousPermission( g_t3dRuntimeAllocationPermission ) {
		g_t3dRuntimeAllocationPhase = ePhase;
		// A nested phase may restrict allocation permission, but it may not
		// silently elevate a hot/worker phase back into an allocation-capable
		// phase. Explicit cold rebuild boundaries use Terrain3DColdAllocationScope.
		g_t3dRuntimeAllocationPermission = m_bPreviousPermission && t3dRuntimeAllocationPhaseAllowsAllocation( ePhase );
	}
	~Terrain3DAllocationPhaseScope( void ) {
		g_t3dRuntimeAllocationPhase = m_ePrevious;
		g_t3dRuntimeAllocationPermission = m_bPreviousPermission;
	}
	Terrain3DAllocationPhaseScope( const Terrain3DAllocationPhaseScope & ) = delete;
	Terrain3DAllocationPhaseScope &operator=( const Terrain3DAllocationPhaseScope & ) = delete;

private:
	t3dRuntimeAllocationPhase_t m_ePrevious;
	bool m_bPreviousPermission;
};

// Explicit allocation boundary for known cold rebuild/resource-publication work.
// This is intentionally a separate type so code review can distinguish a real
// allocation permission override from a diagnostic phase label.
class Terrain3DColdAllocationScope {
public:
	Terrain3DColdAllocationScope( void ) :
	        m_ePrevious( g_t3dRuntimeAllocationPhase ), m_bPreviousPermission( g_t3dRuntimeAllocationPermission ) {
		g_nT3dRuntimeExplicitColdOverrides.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		if ( !m_bPreviousPermission ) {
			g_nT3dRuntimeRestrictedColdOverrides.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		}
		g_t3dRuntimeAllocationPhase = TERRAIN3D_ALLOCATION_PHASE_COLD_REBUILD;
		g_t3dRuntimeAllocationPermission = true;
	}
	~Terrain3DColdAllocationScope( void ) {
		g_t3dRuntimeAllocationPhase = m_ePrevious;
		g_t3dRuntimeAllocationPermission = m_bPreviousPermission;
	}
	Terrain3DColdAllocationScope( const Terrain3DColdAllocationScope & ) = delete;
	Terrain3DColdAllocationScope &operator=( const Terrain3DColdAllocationScope & ) = delete;

private:
	t3dRuntimeAllocationPhase_t m_ePrevious;
	bool m_bPreviousPermission;
};

#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
#include <stdlib.h>
#else
#include <godot_cpp/core/memory.hpp>
#endif

inline void *t3dMemoryAllocate( const size_t nBytes ) {
	if ( nBytes == 0u || !t3dValidateRuntimeAllocationRequest() ) {
		return NULL;
	}
#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
	return malloc( nBytes );
#else
	return memalloc( nBytes );
#endif
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
