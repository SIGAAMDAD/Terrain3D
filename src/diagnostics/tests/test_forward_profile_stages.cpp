#include "diagnostics/core/terrain_3d_streaming_profiler.h"

#include <assert.h>
#include <string.h>

int main() {
	// Legacy indices remain stable for existing tooling and collected snapshots.
	static_assert( T3D_SP_FORWARD_PASS_PREPARE == 13u, "forward prepare legacy index moved" );
	static_assert( T3D_SP_WORLD_BUFFER_UPLOAD == 19u, "legacy stage indices moved" );
	static_assert( T3D_SP_H01_PACK_RESOLVE == 28u, "legacy stage catalog prefix changed" );
	static_assert( T3D_SP_COUNT > T3D_SP_H01_PACK_RESOLVE, "missing P12 profile stages" );
	const char				   *names[] = { "forward_pass_submit",
											"forward_bridge_packet",
											"forward_world_visibility",
											"forward_hiz_build",
											"forward_visibility_dispatch",
											"forward_world_raster_prepare",
											"forward_vt_service",
											"forward_diagnostics" };
	Terrain3DStreamingProfiler &profiler = Terrain3DStreamingProfiler::get();
	profiler.reset();
	profiler.setEnabled( true, 1u );
	for ( uint32_t i = 0u; i < 8u; ++i ) {
		const t3dStreamProfileStage_t stage = ( t3dStreamProfileStage_t ) ( T3D_SP_FORWARD_PASS_SUBMIT + i );
		assert( strcmp( Terrain3DStreamingProfiler::name( stage ), names[i] ) == 0 );
		{
			Terrain3DStreamingProfileScope scope( stage );
		}
		const Terrain3DStreamingProfiler::Snapshot sample = profiler.snapshot( stage );
		assert( sample.calls == 1u && sample.samples == 1u );
	}
	profiler.setEnabled( false );
	return 0;
}
