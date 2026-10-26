// BR10-BR13: deterministic native Forward+ ordering oracle; C++17 standalone.
#include "rendering/rd/forward/terrain_3d_cluster_compact_contract.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define T3D_BR21_TEST_CASES 64000u
#define T3D_BR21_TEST_BUFFER_WORDS 128u
#define T3D_BR21_TEST_MAX_INDICES 2048u

static uint32_t s_nSeed = 0x5abc1721u;
static uint32_t nextRandom() {
	s_nSeed ^= s_nSeed << 13u;
	s_nSeed ^= s_nSeed >> 17u;
	s_nSeed ^= s_nSeed << 5u;
	return s_nSeed;
}

static uint32_t originalRangeMask( uint32_t word, uint32_t minimum, uint32_t maximum ) {
	int local_min = ( int ) minimum - ( int ) word * 32;
	if ( local_min < 0 ) local_min = 0;
	if ( local_min > 31 ) local_min = 31;
	int width = ( int ) maximum - ( int ) minimum;
	if ( width > 32 - local_min ) width = 32 - local_min;
	if ( width <= 0 ) return 0u;
	const uint32_t low_bits = width == 32 ? 0xffffffffu : ( ( 1u << width ) - 1u );
	return low_bits << local_min;
}

int main() {
	uint32_t words[T3D_BR21_TEST_BUFFER_WORDS];
	uint32_t actual[T3D_BR21_TEST_MAX_INDICES];
	uint32_t reference[T3D_BR21_TEST_MAX_INDICES];
	for ( uint32_t trial = 0u; trial < T3D_BR21_TEST_CASES; ++trial ) {
		memset( words, 0, sizeof( words ) );
		const uint32_t max_words = 1u + nextRandom() % 64u;
		const uint32_t slice = nextRandom() % T3D_BR21_DEPTH_SLICES;
		const uint32_t base = nextRandom() % 16u;
		for ( uint32_t i = 0u; i < max_words; ++i ) words[base + i] = nextRandom();
		const uint32_t minimum = nextRandom() % ( max_words * 32u + 1u );
		const uint32_t maximum = nextRandom() % ( max_words * 32u + 1u );
		words[base + max_words + slice] = minimum | ( maximum << 16u );
		uint32_t actual_count = 0u;
		assert( t3dBr21DecodeReference( words, T3D_BR21_TEST_BUFFER_WORDS, base,
			max_words, slice, actual, T3D_BR21_TEST_MAX_INDICES, actual_count ) );
		const uint32_t first = minimum / 32u;
		const uint32_t end = maximum == 0u ? 0u : ( ( maximum - 1u ) / 32u + 1u );
		uint32_t reference_count = 0u;
		for ( uint32_t word = first; word < end && word < max_words; ++word ) {
			const uint32_t mask = words[base + word] & originalRangeMask( word, minimum, maximum );
			for ( int bit = 31; bit >= 0; --bit ) {
				if ( mask & ( 1u << bit ) ) reference[reference_count++] = word * 32u + ( uint32_t ) bit;
			}
		}
		assert( actual_count == reference_count );
		assert( memcmp( actual, reference, actual_count * sizeof( uint32_t ) ) == 0 );
	}
	puts( "BR21 CPU ordering parity: 64000 PASS" );
	return 0;
}
