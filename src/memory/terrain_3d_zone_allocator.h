// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_ZONE_ALLOCATOR_H
#define TERRAIN3D_ZONE_ALLOCATOR_H

#include "core/terrain_3d_compiler.h"
#include "memory/terrain_3d_memory_zone.h"

#include <atomic>
#include <cstdint>
#include <thread>

typedef enum t3dAllocatorAllocationOrigin_e : uint8_t {
	TERRAIN3D_ALLOCATION_BOOTSTRAP = 0,
	TERRAIN3D_ALLOCATION_ZONE,
	TERRAIN3D_ALLOCATION_OVERFLOW,
} t3dAllocatorAllocationOrigin_t;

typedef enum t3dAllocatorOverflowPolicy_e : uint8_t {
	TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT = 0,
	TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP,
} t3dAllocatorOverflowPolicy_t;

class Terrain3DZoneAllocatorRouter {
public:
	EA_FORCE_INLINE static bool canInstall( const Terrain3DMemoryZone *const pZone ) {
		Terrain3DMemoryZone *const pCurrent = s_pZone.load( std::memory_order_acquire );
		return pZone && ( !pCurrent || pCurrent == pZone );
	}
	EA_FORCE_INLINE static bool reserve( Terrain3DMemoryZone *const pZone ) {
		if ( !pZone || pZone->state() != T3D_MEMORY_ZONE_ACTIVE ) {
			return false;
		}
		Terrain3DMemoryZone *pExpected = nullptr;
		if ( !s_pZone.compare_exchange_strong(
				pExpected, pZone, std::memory_order_acq_rel, std::memory_order_acquire ) && pExpected != pZone ) {
			return false;
		}
		return ( s_nRouteGate.load( std::memory_order_acquire ) & s_nRouteGateClosed ) != 0u;
	}
	EA_FORCE_INLINE static bool startRouting( Terrain3DMemoryZone *const pZone ) {
		if ( !pZone || s_pZone.load( std::memory_order_acquire ) != pZone ||
				pZone->state() != T3D_MEMORY_ZONE_ACTIVE ) {
			return false;
		}
		uint32_t expected = s_nRouteGateClosed;
		return s_nRouteGate.compare_exchange_strong(
				expected, 0u, std::memory_order_acq_rel, std::memory_order_acquire );
	}
	EA_FORCE_INLINE static bool install( Terrain3DMemoryZone *const pZone ) {
		return reserve( pZone ) && startRouting( pZone );
	}
	EA_FORCE_INLINE static Terrain3DMemoryZone *zone( void ) {
		return s_pZone.load( std::memory_order_acquire );
	}
	EA_FORCE_INLINE static Terrain3DMemoryZone *beginAllocationRoute( void ) {
		uint32_t gate = s_nRouteGate.load( std::memory_order_acquire );
		for ( ;; ) {
			if ( ( gate & s_nRouteGateClosed ) != 0u || ( gate & s_nRouteReaderMask ) == s_nRouteReaderMask ) {
				return nullptr;
			}
			if ( s_nRouteGate.compare_exchange_weak(
					gate, gate + 1u, std::memory_order_acq_rel, std::memory_order_acquire ) ) {
				Terrain3DMemoryZone *const pZone = s_pZone.load( std::memory_order_acquire );
				if ( pZone ) {
					return pZone;
				}
				endAllocationRoute();
				return nullptr;
			}
		}
	}
	EA_FORCE_INLINE static void endAllocationRoute( void ) {
		s_nRouteGate.fetch_sub( 1u, std::memory_order_release );
	}
	EA_FORCE_INLINE static bool stopRouting( Terrain3DMemoryZone *const pZone ) {
		if ( !pZone || s_pZone.load( std::memory_order_acquire ) != pZone ) {
			return false;
		}
		s_nRouteGate.fetch_or( s_nRouteGateClosed, std::memory_order_acq_rel );
		while ( ( s_nRouteGate.load( std::memory_order_acquire ) & s_nRouteReaderMask ) != 0u ) {
			std::this_thread::yield();
		}
		return s_pZone.load( std::memory_order_acquire ) == pZone;
	}
	EA_FORCE_INLINE static bool uninstall( Terrain3DMemoryZone *const pZone ) {
		const uint32_t gate = s_nRouteGate.load( std::memory_order_acquire );
		if ( !pZone || ( gate & s_nRouteGateClosed ) == 0u || ( gate & s_nRouteReaderMask ) != 0u ) {
			return false;
		}
		Terrain3DMemoryZone *pExpected = pZone;
		return s_pZone.compare_exchange_strong(
				pExpected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire );
	}
	EA_FORCE_INLINE static void setOverflowPolicy( const t3dAllocatorOverflowPolicy_t policy ) {
		s_nOverflowPolicy.store( ( uint8_t )policy, std::memory_order_release );
	}
	EA_FORCE_INLINE static t3dAllocatorOverflowPolicy_t overflowPolicy( void ) {
		return ( t3dAllocatorOverflowPolicy_t )s_nOverflowPolicy.load( std::memory_order_acquire );
	}

private:
	static constexpr uint32_t s_nRouteGateClosed = UINT32_C( 0x80000000 );
	static constexpr uint32_t s_nRouteReaderMask = ~s_nRouteGateClosed;
	inline static std::atomic<Terrain3DMemoryZone *> s_pZone{ nullptr };
	inline static std::atomic<uint32_t> s_nRouteGate{ s_nRouteGateClosed };
	inline static std::atomic<uint8_t> s_nOverflowPolicy{ TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT };
};

#endif // TERRAIN3D_ZONE_ALLOCATOR_H
