#ifndef TERRAIN3D_GPU_PAGE_STREAMER_H
#define TERRAIN3D_GPU_PAGE_STREAMER_H

#include "streaming/gpu/terrain_3d_gpu_page_store.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"

// Opt-in P05 adapter. Call after the existing request scheduler prepared RAW_GPU.
// The owner must retain completion tickets until a proven device completion epoch.
class Terrain3DGpuPageStreamer
{
public:
	struct Pending
	{
		t3dGpuPageTicket_t tickets[T3D_GPU_PAGE_CHANNEL_COUNT]{};
		uint8_t			   count = 0u;
		uint64_t		   submissionEpoch = 0u;
	};
	static bool submitPrepared( Terrain3DRuntimePageStore &runtime,
								Terrain3DGpuPageStore	  &store,
								Terrain3DRdTextureManager &manager,
								const Vector2i			  &location,
								const t3dGpuPageTicket_t  &baseTicket,
								uint64_t				   expectedSourceSignature,
								uint64_t				   submissionEpoch,
								Pending					  &pending );
	static bool
	publishCompleted( Terrain3DGpuPageStore &store, const Pending &pending, uint64_t confirmedCompletedEpoch );
	static void cancel( Terrain3DGpuPageStore &store, const Pending &pending );
};
#endif
