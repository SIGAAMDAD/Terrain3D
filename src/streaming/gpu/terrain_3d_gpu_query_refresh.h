#ifndef TERRAIN3D_GPU_QUERY_REFRESH_H
#define TERRAIN3D_GPU_QUERY_REFRESH_H

#include "streaming/gpu/terrain_3d_gpu_readback_service.h"
#include "streaming/gpu/terrain_3d_gpu_query_bridge.h"
#include "streaming/gpu/terrain_3d_gpu_dirty_ledger.h"

// Owner-thread, one-request-at-a-time handoff from an asynchronous P07
// height readback into the bounded CPU collision/query window. The callback
// supplies the actual completed bytes to accept(); no synchronous readback.
class Terrain3DGpuQueryRefresh
{
public:
	bool beginNext( Terrain3DGpuReadbackService &readback,
					Terrain3DGpuPageStore		&store,
					Terrain3DGpuDirtyLedger		&dirty,
					Terrain3DGpuQueryBridge		&query );
	bool dispatch( Terrain3DGpuReadbackService &readback,
				   Terrain3DRdContext		   &context,
				   Terrain3DGpuPageStore	   &store,
				   uint64_t						submissionEpoch,
				   const Callable			   &callback );
	bool accept( Terrain3DGpuReadbackService &readback,
				 Terrain3DGpuPageStore		 &store,
				 Terrain3DGpuDirtyLedger	 &dirty,
				 Terrain3DGpuQueryBridge	 &query,
				 const PackedByteArray		 &completedBytes );
	void cancel( Terrain3DGpuReadbackService &readback );
	bool pending() const {
		return m_transaction != 0u;
	}
	uint64_t transactionId() const {
		return m_transaction;
	}
	const t3dGpuPageTicket_t &ticket() const {
		return m_ticket;
	}
	const Vector2i &location() const {
		return m_location;
	}
	static Vector2i regionLocation( uint16_t region );

private:
	t3dGpuPageTicket_t m_ticket{};
	Vector2i		   m_location{};
	uint64_t		   m_transaction = 0u;
	bool			   m_dispatched = false;
};
#endif
