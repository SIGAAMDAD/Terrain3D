#ifndef TERRAIN3D_RD_VIEW_STATE_RELEASE_H
#define TERRAIN3D_RD_VIEW_STATE_RELEASE_H
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <stddef.h>

// Operates on caller-owned per-view objects without changing their lifetimes.
template< typename TPipelineCache, typename TFramebuffer, size_t N >
EA_FORCE_INLINE void t3dRdReleaseViewState(
	TPipelineCache	   &rCache,
	Terrain3DRdContext &rContext,
	godot::RID ( &rPipelines )[N],
	TFramebuffer						   &rFramebuffer,
	godot::RID							   &rColor,
	godot::RID							   &rDepth,
	godot::RenderingDevice::TextureSamples &rSamples ) {
	for ( size_t i = 0u; i < N; ++i ) {
		rCache.erase( rPipelines[i] );
		rPipelines[i] = godot::RID();
	}
	rFramebuffer.releaseTracked( rContext );
	rColor = godot::RID();
	rDepth = godot::RID();
	rSamples = godot::RenderingDevice::TEXTURE_SAMPLES_1;
}
#endif // TERRAIN3D_RD_VIEW_STATE_RELEASE_H
