#include "streaming/gpu/terrain_3d_gpu_query_refresh.h"

Vector2i Terrain3DGpuQueryRefresh::regionLocation(
	uint16_t region ) {
	return Vector2i( int( region % T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_HALF,
					 int( region / T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_HALF );
}

bool Terrain3DGpuQueryRefresh::beginNext(
	Terrain3DGpuReadbackService &readback,
	Terrain3DGpuPageStore		&store,
	Terrain3DGpuDirtyLedger		&dirty,
	Terrain3DGpuQueryBridge		&query ) {
	if ( pending() || !store.active() )
		return false;
	uint16_t		   region = UINT16_MAX;
	t3dGpuPageTicket_t source{};
	if ( !dirty.next( Terrain3DGpuDirtyLedger::CPU_COLLISION, region, source ) ||
		 source.m_key.m_nChannel != T3D_GPU_PAGE_HEIGHT )
		return false;
	t3dGpuPageTicket_t published{};
	t3dGpuPageHandle_t handle{};
	if ( !store.publishedSnapshot( source.m_key, published, handle ) || !t3dGpuPageTicketCurrent( source, published ) )
		return false;
	const uint64_t transaction = readback.begin( store, region, uint8_t( 1u << T3D_GPU_PAGE_HEIGHT ) );
	if ( !transaction )
		return false;
	const Vector2i location = regionLocation( region );
	if ( !query.markEditPending( source, location ) ) {
		readback.cancel( transaction );
		return false;
	}
	m_transaction = transaction;
	m_ticket = source;
	m_location = location;
	m_dispatched = false;
	return true;
}

bool Terrain3DGpuQueryRefresh::dispatch(
	Terrain3DGpuReadbackService &readback,
	Terrain3DRdContext			&context,
	Terrain3DGpuPageStore		&store,
	uint64_t					 submissionEpoch,
	const Callable				&callback ) {
	if ( !pending() || m_dispatched || !submissionEpoch || !callback.is_valid() )
		return false;
	if ( !readback.dispatch( m_transaction, T3D_GPU_PAGE_HEIGHT, context, store, submissionEpoch, callback ) )
		return false;
	m_dispatched = true;
	return true;
}

bool Terrain3DGpuQueryRefresh::accept(
	Terrain3DGpuReadbackService &readback,
	Terrain3DGpuPageStore		&store,
	Terrain3DGpuDirtyLedger		&dirty,
	Terrain3DGpuQueryBridge		&query,
	const PackedByteArray		&completedBytes ) {
	if ( !pending() || !m_dispatched || !dirty.isPending( Terrain3DGpuDirtyLedger::CPU_COLLISION, m_ticket ) ||
		 !readback.acceptChannel( m_transaction, T3D_GPU_PAGE_HEIGHT, store, completedBytes ) ||
		 !query.acceptDirtyReadback( m_ticket, m_location, completedBytes ) ||
		 !dirty.acknowledge( Terrain3DGpuDirtyLedger::CPU_COLLISION, m_ticket ) )
		return false;
	cancel( readback );
	return true;
}

void Terrain3DGpuQueryRefresh::cancel(
	Terrain3DGpuReadbackService &readback ) {
	if ( m_transaction )
		readback.cancel( m_transaction );
	m_transaction = 0u;
	m_ticket = t3dGpuPageTicket_t{};
	m_location = Vector2i{};
	m_dispatched = false;
}
