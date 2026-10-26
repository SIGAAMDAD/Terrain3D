#ifndef TERRAIN3D_GPU_CONSUMER_HANDOFF_H
#define TERRAIN3D_GPU_CONSUMER_HANDOFF_H

#include "streaming/gpu/terrain_3d_gpu_dirty_ledger.h"
#include "streaming/gpu/terrain_3d_gpu_page_store.h"

// Bounded handoff from the authoritative page directory to rebuild consumers.
// A ticket stays pending until the consumer explicitly reports its completed
// rebuild. Taking a ticket never acknowledges it; stale completions fail.
class Terrain3DGpuConsumerHandoff
{
public:
	struct Work
	{
		t3dGpuPageTicket_t source{};
		t3dGpuPageHandle_t page{};
		uint8_t			   consumer = 0u;
		bool			   active = false;
	};
	static constexpr uint32_t SLOTS = 7u;
	bool claim( uint8_t consumer, Terrain3DGpuDirtyLedger &dirty, Terrain3DGpuPageStore &store, uint64_t readerEpoch );
	bool
	complete( uint8_t consumer, Terrain3DGpuDirtyLedger &dirty, Terrain3DGpuPageStore &store, uint64_t completedEpoch );
	bool		reject( uint8_t consumer );
	const Work *pending( uint8_t consumer ) const;
	void		reset(); // Only after GPU readers quiesce, or on device loss.
	static int	consumerIndex( uint8_t consumer );

private:
	Work	 m_work[SLOTS]{};
	uint64_t m_readerEpoch[SLOTS]{};
};
#endif
