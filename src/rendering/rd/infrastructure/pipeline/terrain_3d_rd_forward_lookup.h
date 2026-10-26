#ifndef TERRAIN3D_RD_FORWARD_LOOKUP_H
#define TERRAIN3D_RD_FORWARD_LOOKUP_H
#include <stdint.h>
#include <godot_cpp/variant/rid.hpp>

// Cache lookup is atomic from the caller's perspective: a missing tier does
// not overwrite the public cached pipeline entry with a partial result.
template< typename TLookup >
EA_FORCE_INLINE bool t3dRdFindForwardPipelineVariants(
	godot::RID ( &rPipelines )[3],
	TLookup lookup ) {
	godot::RID found[3];
	for ( uint32_t i = 0u; i < 3u; ++i ) {
		const auto *const pEntry = lookup( i );
		if ( !pEntry ) {
			return false;
		}
		found[i] = pEntry->m_pipeline.rid();
	}
	for ( uint32_t i = 0u; i < 3u; ++i ) {
		rPipelines[i] = found[i];
	}
	return true;
}
#endif // TERRAIN3D_RD_FORWARD_LOOKUP_H
