#include "hlod/runtime/terrain_3d_hlod_spatial_bucket.h"
#include <assert.h>
#include <stdio.h>

int main( void )
{
    assert( t3dHlodSpatialBucket( 0.0f ) == 0 );
    assert( t3dHlodSpatialBucket( 63.99f ) == 0 );
    assert( t3dHlodSpatialBucket( 64.0f ) == 1 );
    assert( t3dHlodSpatialBucket( 127.99f ) == 1 );
    assert( t3dHlodSpatialBucket( 128.0f ) == 2 );
    assert( t3dHlodSpatialBucket( -0.01f ) == -1 );
    assert( t3dHlodSpatialBucket( -64.0f ) == -1 );
    assert( t3dHlodSpatialBucket( -64.01f ) == -2 );
    assert( t3dHlodSpatialBucket( 1.0e9f ) == INT16_MAX );
    assert( t3dHlodSpatialBucket( -1.0e9f ) == INT16_MIN );
    puts( "HLOD 64m spatial bucket math passed" );
    return 0;
}
