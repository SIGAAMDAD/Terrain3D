#include "Terrain3DConstants.h"
#include "core/terrain_3d_fixed_buffer.h"
#include "core/terrain_3d_slab_array.h"
#include "memory/terrain_3d_linear_arena.h"
#include "memory/terrain_3d_memory_zone.h"

#include <stdio.h>
#include <stdlib.h>

static int t3dOwnerAllocationFail( const char *const pMessage ) {
	fprintf( stderr, "FAIL: %s\n", pMessage );
	return 1;
}

int main( void ) {
	unsigned char *const pBacking = ( unsigned char * )malloc( s_nBackingBytes );
	if ( !pBacking ) {
		return t3dOwnerAllocationFail( "zone backing allocation" );
	}

	Terrain3DMemoryZone zone;
	if ( !zone.initialize( pBacking, s_nBackingBytes ) ) {
		free( pBacking );
		return t3dOwnerAllocationFail( "zone initialization" );
	}

	Terrain3DLinearArena arena;
	if ( !arena.configure( 4096u ) || !arena.sealCapacity() ) {
		return t3dOwnerAllocationFail( "linear arena setup" );
	}

	Terrain3DFixedBuffer<uint32_t> fixed;
	if ( !fixed.bindAllocatorZone( &zone ) || !fixed.reserveCold( 16u ) || !fixed.seal() ) {
		return t3dOwnerAllocationFail( "fixed buffer setup" );
	}

	Terrain3DHighWaterSlab<uint32_t> slab;
	if ( !slab.bindAllocatorZone( &zone ) || !slab.reserveCold( 16u ) || !slab.seal() ) {
		return t3dOwnerAllocationFail( "slab setup" );
	}

	const t3dRuntimeAllocationPhaseStats_t before = t3dGetRuntimeAllocationPhaseStats();
	{
		Terrain3DAllocationPhaseScope hot_phase( TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME );

		// Reusing already-owned arena bytes is legal in a hot phase.
		if ( !arena.allocate( 64u, 16u ) ) {
			return t3dOwnerAllocationFail( "hot linear-arena cursor allocation" );
		}
		// Physical arena growth is owner-controlled and remains denied while sealed.
		if ( arena.ensureCapacity( 8192u ) ) {
			return t3dOwnerAllocationFail( "sealed linear arena grew backing" );
		}

		for ( uint32_t value = 0u; value < 16u; ++value ) {
			if ( !fixed.pushBack( value ) ) {
				return t3dOwnerAllocationFail( "fixed buffer failed within established capacity" );
			}
		}
		if ( fixed.pushBack( 17u ) || fixed.reserveCold( 32u ) ) {
			return t3dOwnerAllocationFail( "sealed fixed buffer changed backing" );
		}

		if ( slab.allocate( 16u ) == SIZE_MAX ) {
			return t3dOwnerAllocationFail( "slab failed within established capacity" );
		}
		if ( slab.allocate( 1u ) != SIZE_MAX || slab.reserveCold( 32u ) ) {
			return t3dOwnerAllocationFail( "sealed slab changed backing" );
		}

		// The memory zone is mechanical: a valid request is serviced even in HOT_FRAME.
		void *const pZoneAllocation = zone.allocate( 128u, 16u );
		if ( !pZoneAllocation ) {
			return t3dOwnerAllocationFail( "hot memory-zone allocation was phase-denied" );
		}
		zone.deallocate( pZoneAllocation );
	}
	const t3dRuntimeAllocationPhaseStats_t after = t3dGetRuntimeAllocationPhaseStats();
	if ( after.m_szZoneEvents[TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME]
	        <= before.m_szZoneEvents[TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME] ) {
		return t3dOwnerAllocationFail( "hot allocation telemetry did not record zone activity" );
	}

	if ( fixed.growthDenied() == 0u || fixed.capacityMisses() == 0u ) {
		return t3dOwnerAllocationFail( "fixed-buffer owner diagnostics did not record denied growth/capacity miss" );
	}
	if ( slab.growthDenied() == 0u || slab.capacityMisses() == 0u ) {
		return t3dOwnerAllocationFail( "slab owner diagnostics did not record denied growth/capacity miss" );
	}

	fixed.shutdown();
	slab.release();
	arena.release();
	if ( zone.liveAllocations() != 0u || !zone.beginDraining() || !zone.release() ) {
		return t3dOwnerAllocationFail( "owner allocation test zone did not drain" );
	}
	free( pBacking );
	printf( "PASS: phase telemetry is non-authoritative and storage owners control backing growth\n" );
	return 0;
}
