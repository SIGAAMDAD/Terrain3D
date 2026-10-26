// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Central allocation backend for Terrain3D-owned native memory.
#ifndef TERRAIN3D_MEMORY_H
#define TERRAIN3D_MEMORY_H

#include "Terrain3DTypes.h"

#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
#include "core/config/terrain_3d_compiler.h"
#ifndef T3D_RUNTIME_ALLOCATION_PHASE_COUNT
#define T3D_RUNTIME_ALLOCATION_PHASE_COUNT ( 6u )
#endif
#else
#include "Terrain3DConstants.h"
#endif
#include "core/utility/terrain_3d_sync.h"
#include "memory/policy/terrain_3d_memory_account.h"
#include <stddef.h>
#include <stdint.h>
#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
#include <stdlib.h>
#else
#include <godot_cpp/core/memory.hpp>
#endif


// t3dRuntimeAllocationPhase_e centralized in Terrain3DTypes.h.



// t3dRuntimeAllocationPhaseStats_s centralized in Terrain3DTypes.h.


inline thread_local t3dRuntimeAllocationPhase_t g_t3dRuntimeAllocationPhase = TERRAIN3D_ALLOCATION_PHASE_COLD_REBUILD;
inline Terrain3DAtomic<uint64_t> g_szT3dRuntimeEastlAllocationEvents[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
inline Terrain3DAtomic<uint64_t> g_szT3dRuntimeEastlAllocationBytes[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
inline Terrain3DAtomic<uint64_t> g_szT3dRuntimeZoneAllocationEvents[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
inline Terrain3DAtomic<uint64_t> g_szT3dRuntimeZoneAllocationBytes[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
inline Terrain3DAtomic<uint64_t> g_szT3dRuntimeRawAllocationEvents[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
inline Terrain3DAtomic<uint64_t> g_szT3dRuntimeRawAllocationBytes[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};

inline t3dRuntimeAllocationPhase_t t3dGetRuntimeAllocationPhase( void ) {
	return g_t3dRuntimeAllocationPhase;
}

EA_FORCE_INLINE void t3dRecordRuntimeEastlAllocation( const size_t nBytes ) {
	const uint32_t phase = ( uint32_t )( g_t3dRuntimeAllocationPhase );
	if ( phase < T3D_RUNTIME_ALLOCATION_PHASE_COUNT ) {
		g_szT3dRuntimeEastlAllocationEvents[phase].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		g_szT3dRuntimeEastlAllocationBytes[phase].fetch_add( ( uint64_t )( nBytes ), T3D_MEMORY_ORDER_RELAXED );
	}
}

EA_FORCE_INLINE void t3dRecordRuntimeZoneAllocation( const size_t nBytes ) {
	const uint32_t phase = ( uint32_t )( g_t3dRuntimeAllocationPhase );
	if ( phase < T3D_RUNTIME_ALLOCATION_PHASE_COUNT ) {
		g_szT3dRuntimeZoneAllocationEvents[phase].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		g_szT3dRuntimeZoneAllocationBytes[phase].fetch_add( ( uint64_t )( nBytes ), T3D_MEMORY_ORDER_RELAXED );
	}
}

EA_FORCE_INLINE void t3dRecordRuntimeRawAllocation( const size_t nBytes ) {
	const uint32_t phase = ( uint32_t )( g_t3dRuntimeAllocationPhase );
	if ( phase < T3D_RUNTIME_ALLOCATION_PHASE_COUNT ) {
		g_szT3dRuntimeRawAllocationEvents[phase].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		g_szT3dRuntimeRawAllocationBytes[phase].fetch_add( ( uint64_t )( nBytes ), T3D_MEMORY_ORDER_RELAXED );
	}
}


inline t3dRuntimeAllocationPhaseStats_t t3dGetRuntimeAllocationPhaseStats( void ) {
	t3dRuntimeAllocationPhaseStats_t stats;
	for ( uint32_t phase = 0u; phase < T3D_RUNTIME_ALLOCATION_PHASE_COUNT; ++phase ) {
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

struct alignas(max_align_t) t3dNativeMemoryHeader {
	Terrain3DMemoryAccount *account;
	size_t charged_bytes;
};

inline void *t3dMemoryAllocate( const size_t nBytes ) {
	if ( nBytes == 0u || nBytes > SIZE_MAX - sizeof(t3dNativeMemoryHeader) ) {
		return NULL;
	}
	const size_t charged_bytes = nBytes + sizeof(t3dNativeMemoryHeader);
	Terrain3DMemoryAccount *const account = g_t3dMemoryAccount;
	if (account && !Terrain3DMemoryAllocationPermit::consume(account, charged_bytes)
	        && !account->reserve(charged_bytes)) { return NULL; }
#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
	void *const pMemory = malloc( charged_bytes );
#else
	void *const pMemory = memalloc( charged_bytes );
#endif
	if ( pMemory ) {
		if (account) { account->retain(); }
		auto *header = static_cast<t3dNativeMemoryHeader *>(pMemory);
		header->account = account;
		header->charged_bytes = charged_bytes;
		t3dRecordRuntimeRawAllocation( nBytes );
		return header + 1;
	}
	if (account) { account->release(charged_bytes); }
	return NULL;
}

inline void t3dMemoryRelease( void *pPointer ) {
	if ( !pPointer ) {
		return;
	}
	auto *header = static_cast<t3dNativeMemoryHeader *>(pPointer) - 1;
	Terrain3DMemoryAccount *const account = header->account;
	const size_t charged_bytes = header->charged_bytes;
#if defined( TERRAIN3D_STANDALONE_ALLOCATOR_TEST )
	free( header );
#else
	memfree( header );
#endif
	if (account) { account->release(charged_bytes); account->releaseReference(); }
}

#endif // TERRAIN3D_MEMORY_H
