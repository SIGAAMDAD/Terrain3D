// Copyright 2026 Terrain3D contributors.
#ifndef TERRAIN3D_CLUSTER_COMPACT_CONTRACT_H
#define TERRAIN3D_CLUSTER_COMPACT_CONTRACT_H
#include <stdint.h>
#include <stddef.h>
#define T3D_BR21_KIND_COUNT 5u
#define T3D_BR21_DEPTH_SLICES 32u
#define T3D_BR21_LISTS_PER_CLUSTER ( T3D_BR21_KIND_COUNT * T3D_BR21_DEPTH_SLICES )
#define T3D_BR21_SCAN_GROUP_SIZE 128u
#define T3D_BR21_MAX_CLUSTERS 8192u
#define T3D_BR21_INDEX_CAPACITY 1048576u
#define T3D_BR21_HEADER_VALID 0x42523131u

typedef struct t3dBr21Header_s
{
	uint32_t m_nOffset;
	uint32_t m_nCount;
	uint32_t m_nValidity;
	uint32_t m_nReserved;
} t3dBr21Header_t;
static_assert( sizeof( t3dBr21Header_t ) == 16u, "BR21 header std430" );

typedef struct t3dBr21Push_s
{
	uint32_t m_nPhase;
	uint32_t m_nListCount;
	uint32_t m_nClusterCount;
	uint32_t m_nIndexCapacity;
	uint32_t m_nMaxWords;
	uint32_t m_nTypeStride;
	uint32_t m_nClusterWidth;
	uint32_t m_nNativeWordCount;
} t3dBr21Push_t;
static_assert( sizeof( t3dBr21Push_t ) == 32u, "BR21 push ABI" );

// BR10 reference decoder. Caller supplies a verified backing array and enough
// destination capacity; failure leaves the destination count at zero.
static inline bool t3dBr21DecodeReference( const uint32_t *pWords, size_t nWords,
	uint32_t nTypeBase, uint32_t nMaxWords, uint32_t nSlice,
	uint32_t *pIndices, uint32_t nCapacity, uint32_t &rCount )
{
	rCount = 0u;
	if ( !pWords || !pIndices || nSlice >= T3D_BR21_DEPTH_SLICES ||
		 nTypeBase > nWords || nMaxWords > nWords - nTypeBase ||
		 nSlice >= nWords - nTypeBase - nMaxWords ) return false;
	const uint32_t bounds = pWords[nTypeBase + nMaxWords + nSlice];
	const uint32_t minimum = bounds & 0xffffu;
	const uint32_t maximum = bounds >> 16u;
	const uint32_t first = minimum >> 5u < nMaxWords ? minimum >> 5u : nMaxWords;
	const uint32_t last = maximum == 0u ? 0u : ( ( maximum - 1u ) >> 5u ) + 1u;
	const uint32_t end = last < nMaxWords ? last : nMaxWords;
	for ( uint32_t word = first; word < end; ++word ) {
		uint32_t mask = pWords[nTypeBase + word];
		while ( mask ) {
			uint32_t bit = 31u;
			while ( bit && ( mask & ( 1u << bit ) ) == 0u ) --bit;
			mask &= ~( 1u << bit );
			const uint32_t item = word * 32u + bit;
			const int localMin = ( int ) minimum - ( int ) ( word * 32u );
			const int from = localMin < 0 ? 0 : ( localMin > 31 ? 31 : localMin );
			const int span = ( int ) maximum - ( int ) minimum;
			const int width = span < 32 - from ? span : 32 - from;
			if ( width > 0 && bit >= ( uint32_t ) from && bit < ( uint32_t ) ( from + width ) ) {
				if ( rCount >= nCapacity ) return false;
				pIndices[rCount++] = item;
			}
		}
	}
	return true;
}
#endif // TERRAIN3D_CLUSTER_COMPACT_CONTRACT_H
