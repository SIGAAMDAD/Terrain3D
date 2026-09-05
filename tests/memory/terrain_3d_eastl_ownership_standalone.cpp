#include "Terrain3DConstants.h"
#include "core/terrain_3d_eastl_helpers.h"
#include "eastl_config.h"

#include <EASTL/fixed_vector.h>
#include <EASTL/vector.h>

#include <stdio.h>
#include <stdlib.h>

static_assert( sizeof( eastl::array<uint32_t, 8u> ) == sizeof( uint32_t ) * 8u, "eastl::array must not add storage overhead" );

static int t3dEastlOwnershipFail( const char *const pMessage ) {
	fprintf( stderr, "FAIL: %s\n", pMessage );
	return 1;
}

int main( void ) {
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_INITIALIZATION );
	unsigned char *const pBackingA = ( unsigned char * )malloc( s_nBackingBytes );
	unsigned char *const pBackingB = ( unsigned char * )malloc( s_nBackingBytes );
	if ( !pBackingA || !pBackingB ) {
		free( pBackingA );
		free( pBackingB );
		return t3dEastlOwnershipFail( "test backing allocation" );
	}

	Terrain3DMemoryZone zoneA;
	Terrain3DMemoryZone zoneB;
	if ( !zoneA.initialize( pBackingA, s_nBackingBytes ) || !zoneB.initialize( pBackingB, s_nBackingBytes ) ) {
		free( pBackingA );
		free( pBackingB );
		return t3dEastlOwnershipFail( "zone initialization" );
	}

	const size_t total_before = Terrain3DEastlAllocator::getAllocationStats().m_nTotalAllocations;
	{
		eastl::fixed_vector<int, 32u, false> fixed_values;
		for ( int value = 0; value < 32; ++value ) {
			fixed_values.push_back( value );
		}
		if ( fixed_values.size() != 32u || !fixed_values.full()
		        || Terrain3DEastlAllocator::getAllocationStats().m_nTotalAllocations != total_before ) {
			return t3dEastlOwnershipFail( "fixed_vector allocated backing storage" );
		}

		eastl::vector<uint32_t, Terrain3DEastlAllocator> valuesA;
		eastl::vector<uint32_t, Terrain3DEastlAllocator> valuesB;
		eastl::vector<uint32_t, Terrain3DEastlAllocator> bootstrapValues;
		valuesA.set_allocator( Terrain3DEastlAllocator::zoneAllocator( &zoneA ) );
		valuesB.set_allocator( Terrain3DEastlAllocator::zoneAllocator( &zoneB ) );
		bootstrapValues.set_allocator( Terrain3DEastlAllocator::bootstrapAllocator() );
		if ( !t3dContainerReserveCold( valuesA, 64u ) || !t3dContainerReserveCold( valuesB, 64u )
		        || !t3dContainerReserveCold( bootstrapValues, 64u ) ) {
			return t3dEastlOwnershipFail( "explicit allocator vector reserve" );
		}
		valuesA.resize( 64u, 3u );
		valuesB.resize( 64u, 5u );
		bootstrapValues.resize( 64u, 7u );
		if ( !zoneA.owns( valuesA.data() ) || zoneB.owns( valuesA.data() ) ) {
			return t3dEastlOwnershipFail( "zone A vector ownership" );
		}
		if ( !zoneB.owns( valuesB.data() ) || zoneA.owns( valuesB.data() ) ) {
			return t3dEastlOwnershipFail( "zone B vector ownership" );
		}
		if ( zoneA.owns( bootstrapValues.data() ) || zoneB.owns( bootstrapValues.data() ) ) {
			return t3dEastlOwnershipFail( "bootstrap vector captured a Terrain3D zone" );
		}
	}

	if ( zoneA.liveAllocations() != 0u || zoneB.liveAllocations() != 0u ) {
		return t3dEastlOwnershipFail( "zone allocations did not drain" );
	}
	if ( !zoneA.beginDraining() || !zoneA.release() || !zoneB.beginDraining() || !zoneB.release() ) {
		return t3dEastlOwnershipFail( "zone shutdown" );
	}
	free( pBackingA );
	free( pBackingB );
	printf( "PASS: explicit EASTL owner-zone binding and bootstrap lifetime separation\n" );
	return 0;
}
