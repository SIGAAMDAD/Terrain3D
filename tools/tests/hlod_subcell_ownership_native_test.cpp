#include "hlod/runtime/terrain_3d_hlod_subcell_ownership.h"
#include <assert.h>
#include <limits>
int main( void )
{
    assert( t3dHlodSubcellOwnerForBounds( 0, 0, 64, 64 ) == 0u );
    assert( t3dHlodSubcellOwnerForBounds( 64, 0, 128, 64 ) == 1u );
    assert( t3dHlodSubcellOwnerForBounds( 0, 64, 64, 128 ) == 2u );
    assert( t3dHlodSubcellOwnerForBounds( 64, 64, 128, 128 ) == 3u );
    assert( t3dHlodSubcellOwnerForBounds( 63, 63, 65, 65 ) == 0xffu );
    assert( t3dHlodSubcellOwnerForBounds( -1, 0, 10, 10 ) == 0xffu );
    assert( t3dHlodSubcellOwnerForBounds( 0, 0, 129, 10 ) == 0xffu );
    assert( t3dHlodSubcellOwnerForBounds( std::numeric_limits<float>::quiet_NaN(), 0, 10, 10 ) == 0xffu );
    assert( t3dHlodSubcellOwnedAndReady( 0x05u, 0x01u, 0u ) );
    assert( !t3dHlodSubcellOwnedAndReady( 0x05u, 0x01u, 2u ) );
    assert( !t3dHlodSubcellOwnedAndReady( 0x0fu, 0x0fu, 4u ) );
    return 0;
}
