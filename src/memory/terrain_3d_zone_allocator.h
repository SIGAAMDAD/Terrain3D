// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_ZONE_ALLOCATOR_H
#define TERRAIN3D_ZONE_ALLOCATOR_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "core/terrain_3d_compiler.h"
#include "memory/terrain_3d_memory_zone.h"

#include <stdint.h>
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
		Terrain3DMemoryZone *const pCurrent = s_pZone.load( T3D_MEMORY_ORDER_ACQUIRE );
		return pZone && ( !pCurrent || pCurrent == pZone );
	}
	EA_FORCE_INLINE static bool reserve( Terrain3DMemoryZone *const pZone ) {
		if ( !pZone || pZone->state() != T3D_MEMORY_ZONE_ACTIVE ) {
			return false;
		}
		Terrain3DMemoryZone *pExpected = NULL;
		if ( !s_pZone.compare_exchange_strong( pExpected, pZone, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_ACQUIRE )
		        && pExpected != pZone ) {
			return false;
		}
		return ( s_nRouteGate.load( T3D_MEMORY_ORDER_ACQUIRE ) & s_nRouteGateClosed ) != 0u;
	}
	EA_FORCE_INLINE static bool startRouting( Terrain3DMemoryZone *const pZone ) {
		if ( !pZone || s_pZone.load( T3D_MEMORY_ORDER_ACQUIRE ) != pZone || pZone->state() != T3D_MEMORY_ZONE_ACTIVE ) {
			return false;
		}
		uint32_t expected = s_nRouteGateClosed;
		return s_nRouteGate.compare_exchange_strong( expected, 0u, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE static bool install( Terrain3DMemoryZone *const pZone ) {
		return reserve( pZone ) && startRouting( pZone );
	}
	EA_FORCE_INLINE static Terrain3DMemoryZone *zone( void ) {
		return s_pZone.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE static bool isRouting( const Terrain3DMemoryZone *const pZone ) {
		return pZone && s_pZone.load( T3D_MEMORY_ORDER_ACQUIRE ) == pZone
		        && ( s_nRouteGate.load( T3D_MEMORY_ORDER_ACQUIRE ) & s_nRouteGateClosed ) == 0u;
	}
	EA_FORCE_INLINE static Terrain3DMemoryZone *beginAllocationRoute( void ) {
		uint32_t gate = s_nRouteGate.load( T3D_MEMORY_ORDER_ACQUIRE );
		for ( ;; ) {
			if ( ( gate & s_nRouteGateClosed ) != 0u || ( gate & s_nRouteReaderMask ) == s_nRouteReaderMask ) {
				return NULL;
			}
			if ( s_nRouteGate.compare_exchange_weak( gate, gate + 1u, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_ACQUIRE ) ) {
				Terrain3DMemoryZone *const pZone = s_pZone.load( T3D_MEMORY_ORDER_ACQUIRE );
				if ( pZone ) {
					return pZone;
				}
				endAllocationRoute();
				return NULL;
			}
		}
	}
	EA_FORCE_INLINE static void endAllocationRoute( void ) {
		s_nRouteGate.fetch_sub( 1u, T3D_MEMORY_ORDER_RELEASE );
	}
	EA_FORCE_INLINE static bool stopRouting( Terrain3DMemoryZone *const pZone ) {
		if ( !pZone || s_pZone.load( T3D_MEMORY_ORDER_ACQUIRE ) != pZone ) {
			return false;
		}
		s_nRouteGate.fetch_or( s_nRouteGateClosed, T3D_MEMORY_ORDER_ACQ_REL );
		while ( ( s_nRouteGate.load( T3D_MEMORY_ORDER_ACQUIRE ) & s_nRouteReaderMask ) != 0u ) {
			t3dThreadYield();
		}
		return s_pZone.load( T3D_MEMORY_ORDER_ACQUIRE ) == pZone;
	}
	EA_FORCE_INLINE static bool uninstall( Terrain3DMemoryZone *const pZone ) {
		const uint32_t gate = s_nRouteGate.load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( !pZone || ( gate & s_nRouteGateClosed ) == 0u || ( gate & s_nRouteReaderMask ) != 0u ) {
			return false;
		}
		Terrain3DMemoryZone *pExpected = pZone;
		return s_pZone.compare_exchange_strong( pExpected, NULL, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE static void setOverflowPolicy( const t3dAllocatorOverflowPolicy_t policy ) {
		s_nOverflowPolicy.store( ( uint8_t )policy, T3D_MEMORY_ORDER_RELEASE );
	}
	EA_FORCE_INLINE static t3dAllocatorOverflowPolicy_t overflowPolicy( void ) {
		return ( t3dAllocatorOverflowPolicy_t )s_nOverflowPolicy.load( T3D_MEMORY_ORDER_ACQUIRE );
	}

private:
	inline static t3dAtomic_t<Terrain3DMemoryZone *> s_pZone{ NULL };
	inline static t3dAtomic_t<uint32_t> s_nRouteGate{ s_nRouteGateClosed };
	inline static t3dAtomic_t<uint8_t> s_nOverflowPolicy{ TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT };
};

#endif // TERRAIN3D_ZONE_ALLOCATOR_H
