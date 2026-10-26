#include "hlod/runtime/terrain_3d_hlod_subcell_group.h"
#include <assert.h>

int main( void )
{
    assert( !t3dHlodSubcellMaskComplete( 0u, 0u ) );
    assert( t3dHlodSubcellMaskComplete( 1u, 1u ) );
    assert( !t3dHlodSubcellMaskComplete( 3u, 1u ) );
    assert( !t3dHlodSubcellMaskComplete( 15u, 7u ) );
    assert( t3dHlodSubcellMaskComplete( 15u, 15u ) );
    assert( t3dHlodSubcellMaskComplete( 10u, 15u ) );
    assert( t3dHlodSubcellMaskComplete( 10u, 10u ) );
    assert( !t3dHlodSubcellMaskComplete( 10u, 2u ) );
    return 0;
}
