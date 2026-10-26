// Shared, deterministic demand ranking for CPU source tiles and far GPU pages.
// The streaming policy owns desired priority; physical caches own their grants,
// leases, retirements, and publication. No allocations or global resets.
#ifndef TERRAIN3D_RASTER_ADMISSION_H
#define TERRAIN3D_RASTER_ADMISSION_H

#include "streaming/residency/terrain_3d_residency_state.h"
#include <stdint.h>

EA_FORCE_INLINE uint32_t t3dRasterRegionPriority(
	const t3dResidencyFrameState_s &rFrame,
	const godot::Vector2i		   &rLocation ) {
	if ( !rFrame.containsLocation( rLocation ) ) {
		return 0u;
	}
	godot::Vector2i center = rFrame.m_residencyCenter;
	if ( center == s_v2iMax ) {
		center = rFrame.m_streamingCenter;
	}
	if ( center == s_v2iMax ) {
		center = godot::Vector2i( 0, 0 );
	}
	const uint32_t dx = ( uint32_t ) abs( rLocation.x - center.x );
	const uint32_t dy = ( uint32_t ) abs( rLocation.y - center.y );
	const uint32_t distance = godot::MAX( dx, dy );
	const uint32_t state = ( uint32_t ) rFrame.streamingState( rLocation );
	const uint32_t tier = rFrame.m_bAuthoring ? 3u : state;
	uint32_t	   score = tier * 8192u + ( 2048u - godot::MIN( distance * 64u, 2048u ) );
	if ( rFrame.m_predictedCenter != s_v2iMax ) {
		const uint32_t pdx = ( uint32_t ) abs( rLocation.x - rFrame.m_predictedCenter.x );
		const uint32_t pdy = ( uint32_t ) abs( rLocation.y - rFrame.m_predictedCenter.y );
		score += 512u - godot::MIN( godot::MAX( pdx, pdy ) * 16u, 512u );
	}
	if ( rFrame.isPrefetch( rLocation ) ) {
		score += 1024u;
	}
	return score;
}

#endif // TERRAIN3D_RASTER_ADMISSION_H
