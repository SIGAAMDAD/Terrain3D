#include "streaming/gpu/terrain_3d_gpu_page_streamer.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"

bool Terrain3DGpuPageStreamer::submitPrepared(
	Terrain3DRuntimePageStore &runtime,
	Terrain3DGpuPageStore	  &store,
	Terrain3DRdTextureManager &manager,
	const Vector2i			  &location,
	const t3dGpuPageTicket_t  &baseTicket,
	uint64_t				   expectedSourceSignature,
	uint64_t				   submissionEpoch,
	Pending					  &pending ) {
	pending = Pending{};
	if ( !store.active() || !baseTicket.m_key.valid() || !submissionEpoch || !expectedSourceSignature ||
		 !Terrain3DRdContext::isOnRenderThread() )
		return false;
	t3dRuntimeRawGpuPageResult_t lease;
	if ( !runtime.takeRawGpuPage( location, lease ) || lease.m_nSourceSignature != expectedSourceSignature )
		return false;
	// A raw cooked request must carry mandatory height + derived + composite.
	const PackedByteArray *bytes[T3D_GPU_PAGE_CHANNEL_COUNT] =
		{ &lease.m_height, &lease.m_control, &lease.m_color, &lease.m_derived, &lease.m_composite, &lease.m_shadow };
	for ( uint32_t channel = 0u; channel < T3D_GPU_PAGE_CHANNEL_COUNT; ++channel ) {
		if ( bytes[channel]->is_empty() )
			continue; // implicit/default optional channel
		if ( ( uint64_t ) bytes[channel]->size() != store.pool( ( uint8_t ) channel ).layerBytes() )
			return false;
		if ( store.pool( ( uint8_t ) channel ).deviceEpoch() != baseTicket.m_nDeviceEpoch )
			return false;
	}
	if ( lease.m_height.is_empty() || lease.m_derived.is_empty() || lease.m_composite.is_empty() )
		return false;
	for ( uint32_t channel = 0u; channel < T3D_GPU_PAGE_CHANNEL_COUNT; ++channel ) {
		if ( bytes[channel]->is_empty() )
			continue;
		t3dGpuPageTicket_t ticket = baseTicket;
		ticket.m_key.m_nChannel = ( uint8_t ) channel;
		if ( !store.stage( manager, ticket, *bytes[channel], submissionEpoch ) ) {
			cancel( store, pending );
			return false;
		}
		pending.tickets[pending.count++] = ticket;
	}
	pending.submissionEpoch = submissionEpoch;
	return true;
}

bool Terrain3DGpuPageStreamer::publishCompleted(
	Terrain3DGpuPageStore &store,
	const Pending		  &pending,
	uint64_t			   confirmedCompletedEpoch ) {
	return pending.count > 0u && confirmedCompletedEpoch >= pending.submissionEpoch &&
		   store.commitBatch( pending.tickets, pending.count, confirmedCompletedEpoch );
}

void Terrain3DGpuPageStreamer::cancel(
	Terrain3DGpuPageStore &store,
	const Pending		  &pending ) {
	for ( uint32_t i = 0u; i < pending.count; ++i )
		store.cancelStaged( pending.tickets[i].m_key );
}
