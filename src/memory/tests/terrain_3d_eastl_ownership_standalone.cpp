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
	unsigned char *const pBacking = ( unsigned char * )malloc( s_nBackingBytes );
	if ( !pBacking ) {
		return t3dEastlOwnershipFail( "test backing allocation" );
	}

	Terrain3DMemoryZone zone;
	if ( !zone.initialize( pBacking, s_nBackingBytes ) || !Terrain3DZoneAllocatorRouter::reserve( &zone )
	        || !Terrain3DZoneAllocatorRouter::startRouting( &zone ) ) {
		free( pBacking );
		return t3dEastlOwnershipFail( "zone routing initialization" );
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

		eastl::vector<uint32_t, Terrain3DEastlAllocator> values;
		if ( !t3dContainerReserveCold( values, 64u ) ) {
			return t3dEastlOwnershipFail( "zone vector reserve" );
		}
		values.resize( 64u, 3u );
		if ( zone.liveAllocations() == 0u || !zone.owns( values.data() ) ) {
			return t3dEastlOwnershipFail( "EASTL vector did not route into terrain zone" );
		}
	}

	if ( zone.liveAllocations() != 0u ) {
		return t3dEastlOwnershipFail( "zone allocations did not drain" );
	}
	if ( !Terrain3DZoneAllocatorRouter::stopRouting( &zone ) || !Terrain3DZoneAllocatorRouter::uninstall( &zone ) || !zone.beginDraining()
	        || !zone.release() ) {
		return t3dEastlOwnershipFail( "zone shutdown" );
	}
	free( pBacking );
	printf( "PASS: EASTL terrain-zone ownership routing\n" );
	return 0;
}
