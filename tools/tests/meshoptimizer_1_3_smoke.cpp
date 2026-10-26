// Standalone editor-toolchain smoke test. Compile against the pinned vendor sources.
// Does not require Godot or its runtime renderer.
#include "meshoptimizer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <vector>

int main( void )
{
    enum { GRID = 17 };
    std::vector< float > positions( GRID * GRID * 3u );
    std::vector< float > attributes( GRID * GRID * 5u );
    std::vector< unsigned int > indices;
    for ( unsigned int y = 0; y < GRID; ++y ) {
        for ( unsigned int x = 0; x < GRID; ++x ) {
            const unsigned int n = y * GRID + x;
            positions[n * 3u] = (float) x;
            positions[n * 3u + 1u] = (float) y;
            positions[n * 3u + 2u] = 0.0f;
            attributes[n * 5u] = 0.0f;
            attributes[n * 5u + 1u] = 0.0f;
            attributes[n * 5u + 2u] = 1.0f;
            attributes[n * 5u + 3u] = (float) x / (float) ( GRID - 1 );
            attributes[n * 5u + 4u] = (float) y / (float) ( GRID - 1 );
        }
    }
    for ( unsigned int y = 0; y < GRID - 1; ++y ) {
        for ( unsigned int x = 0; x < GRID - 1; ++x ) {
            const unsigned int n = y * GRID + x;
            indices.push_back( n ); indices.push_back( n + 1 ); indices.push_back( n + GRID );
            indices.push_back( n + 1 ); indices.push_back( n + GRID + 1 ); indices.push_back( n + GRID );
        }
    }
    const float weights[] = { 0.5f, 0.5f, 0.5f, 0.2f, 0.2f };
    std::vector< unsigned int > reduced1( indices.size() ), reduced2( indices.size() );
    float error1 = -1.0f, error2 = -1.0f;
    const unsigned int options = meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute;
    const size_t count1 = meshopt_simplifyWithAttributes( reduced1.data(), indices.data(), indices.size(),
        positions.data(), GRID * GRID, 3u * sizeof(float), attributes.data(),
        5u * sizeof(float), weights, 5u, NULL, indices.size() / 2u, 0.1f, options, &error1 );
    const size_t count2 = meshopt_simplifyWithAttributes( reduced2.data(), indices.data(), indices.size(),
        positions.data(), GRID * GRID, 3u * sizeof(float), attributes.data(),
        5u * sizeof(float), weights, 5u, NULL, indices.size() / 2u, 0.1f, options, &error2 );
    assert( count1 > 0u && count1 <= indices.size() && count1 % 3u == 0u );
    assert( count1 == count2 && error1 == error2 );
    assert( isfinite( error1 ) && error1 <= 0.10001f );
    for ( size_t n = 0u; n < count1; ++n ) { assert( reduced1[n] == reduced2[n] && reduced1[n] < GRID * GRID ); }
    std::vector< unsigned int > reordered( count1 );
    meshopt_optimizeVertexCache( reordered.data(), reduced1.data(), count1, GRID * GRID );
    std::vector< unsigned int > remap( GRID * GRID );
    size_t unique = meshopt_optimizeVertexFetchRemap( remap.data(), reordered.data(), count1, GRID * GRID );
    assert( unique >= 3u && unique <= GRID * GRID );
    for ( size_t n = 0u; n < count1; ++n ) { assert( remap[reordered[n]] < unique ); }
    printf( "meshoptimizer v%d: %zu -> %zu indices, %zu used vertices, error=%.6f; deterministic\n",
        MESHOPTIMIZER_VERSION, indices.size(), count1, unique, error1 );
    return 0;
}
