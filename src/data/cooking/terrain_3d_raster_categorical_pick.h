#ifndef TERRAIN3D_RASTER_CATEGORICAL_PICK_H
#define TERRAIN3D_RASTER_CATEGORICAL_PICK_H

#include <stdint.h>

// Counts the 32 possible control-word categories in linear time. When the
// modal category is tied, return the first *complete* original control word
// in input order, preserving the pre-P14 majority-vote representation.
static inline uint32_t t3dRasterCategoricalPick(
	const uint32_t *const pWords,
	const uint32_t nCount )
{
	// Four-source parent nodes are small enough for direct comparisons.
	// Avoid clearing a 32-bin table when the input has only four words.
	if ( nCount <= 4u ) {
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
	uint32_t counts[32] = {};
	for ( uint32_t i = 0u; i < nCount; ++i ) {
		++counts[( pWords[i] >> 27u ) & 31u];
	}
	uint32_t best = pWords[0];
	uint32_t bestCount = 0u;
	for ( uint32_t i = 0u; i < nCount; ++i ) {
		const uint32_t count = counts[( pWords[i] >> 27u ) & 31u];
		if ( count > bestCount ) {
			best = pWords[i];
			bestCount = count;
		}
	}
	return best;
}

#endif
