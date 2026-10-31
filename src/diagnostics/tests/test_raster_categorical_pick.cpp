// Portable parity test: the *actual* production linear-time helper is
// compared against the former quadratic majority-vote reference.
#include "data/cooking/terrain_3d_raster_categorical_pick.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <chrono>
#include <initializer_list>

static uint32_t oldPick( const uint32_t *pWords, uint32_t nCount )
{
	uint32_t best = pWords[0];
	uint32_t bestCount = 0u;
	for ( uint32_t i = 0u; i < nCount; ++i ) {
		const uint32_t category = ( pWords[i] >> 27u ) & 31u;
		uint32_t count = 0u;
		for ( uint32_t j = 0u; j < nCount; ++j ) {
			count += ( ( pWords[j] >> 27u ) & 31u ) == category ? 1u : 0u;
		}
		if ( count > bestCount ) {
			best = pWords[i];
			bestCount = count;
		}
	}
	return best;
}

static uint32_t nextRandom( uint32_t &seed )
{
	seed ^= seed << 13u;
	seed ^= seed >> 17u;
	seed ^= seed << 5u;
	return seed;
}

int main()
{
	uint32_t seed = 0xa17d3621u;
	uint32_t words[16] = {};
	for ( uint32_t n : { 4u, 16u } ) {
		for ( uint32_t testcase = 0u; testcase < 200000u; ++testcase ) {
			for ( uint32_t i = 0u; i < n; ++i ) {
				words[i] = nextRandom( seed );
				// Many ties and exact category collisions.
				if ( ( testcase & 1u ) == 0u ) {
					words[i] = ( words[i] & 0x07ffffffu ) | ( ( i % 3u ) << 27u );
				}
			}
			assert( oldPick( words, n ) == t3dRasterCategoricalPick( words, n ) );
		}
	}
	volatile uint64_t guard = 0u;
	for ( uint32_t n : { 4u, 16u } ) {
		for ( uint32_t i = 0; i < n; ++i ) words[i] = nextRandom( seed );
		const auto baselineStart = std::chrono::steady_clock::now();
		for ( uint32_t i = 0u; i < 2000000u; ++i ) {
			words[0] = ( words[0] & 0x07ffffffu ) | ( ( i & 31u ) << 27u );
			guard += oldPick( words, n );
		}
		const auto baselineEnd = std::chrono::steady_clock::now();
		for ( uint32_t i = 0u; i < n; ++i ) words[i] = nextRandom( seed );
		const auto optimizedStart = std::chrono::steady_clock::now();
		for ( uint32_t i = 0u; i < 2000000u; ++i ) {
			words[0] = ( words[0] & 0x07ffffffu ) | ( ( i & 31u ) << 27u );
			guard += t3dRasterCategoricalPick( words, n );
		}
		const auto optimizedEnd = std::chrono::steady_clock::now();
		const auto oldUs = std::chrono::duration_cast< std::chrono::microseconds >( baselineEnd - baselineStart ).count();
		const auto newUs = std::chrono::duration_cast< std::chrono::microseconds >( optimizedEnd - optimizedStart ).count();
		printf( "raster_count=%u reference_us=%lld optimized_us=%lld guard=%llu\n",
			n, ( long long ) oldUs, ( long long ) newUs, ( unsigned long long ) guard );
	}
	puts( "raster categorical parity: passed 400000 randomized cases" );
	return 0;
}
