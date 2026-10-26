#include "streaming/gpu/terrain_3d_gpu_query_bridge.h"

bool Terrain3DGpuQueryBridge::validPayload(
	const PackedByteArray &bytes ) {
	return bytes.size() == int64_t( HEIGHT_BYTES );
}

bool Terrain3DGpuQueryBridge::matchesRegion(
	uint16_t		region,
	const Vector2i &location ) {
	const int x = location.x + T3D_REGION_MAP_HALF;
	const int y = location.y + T3D_REGION_MAP_HALF;
	return x >= 0 && x < T3D_REGION_MAP_SIZE && y >= 0 && y < T3D_REGION_MAP_SIZE &&
		   uint16_t( y * T3D_REGION_MAP_SIZE + x ) == region;
}

bool Terrain3DGpuQueryBridge::acceptClean(
	const t3dGpuPageTicket_t &ticket,
	const Vector2i			 &location,
	const PackedByteArray	 &r16Bytes ) {
	if ( !m_storage || !m_storage->isActiveOwner() || !ticket.m_key.valid() ||
		 ticket.m_key.m_nChannel != T3D_GPU_PAGE_HEIGHT || !matchesRegion( ticket.m_key.m_nRegionIndex, location ) ||
		 !validPayload( r16Bytes ) )
		return false;
	Record &entry = m_entries[ticket.m_key.m_nRegionIndex];
	if ( entry.generation && entry.ticket.m_nDeviceEpoch == ticket.m_nDeviceEpoch &&
		 ( entry.ticket.m_nSourceGeneration > ticket.m_nSourceGeneration ||
		   ( entry.ticket.m_nSourceGeneration == ticket.m_nSourceGeneration &&
			 entry.ticket.m_nEditGeneration > ticket.m_nEditGeneration ) ) )
		return false;
	if ( entry.pending && entry.ticket.m_nDeviceEpoch == ticket.m_nDeviceEpoch &&
		 entry.ticket.m_nSourceGeneration == ticket.m_nSourceGeneration &&
		 entry.ticket.m_nEditGeneration >= ticket.m_nEditGeneration )
		return false;
	if ( !m_storage->syncActiveHeightPage( location, r16Bytes.ptr(), HEIGHT_BYTES ) )
		return false;
	entry.ticket = ticket;
	entry.generation = ticket.m_nEditGeneration;
	entry.pending = false;
	entry.ready = true;
	return true;
}

bool Terrain3DGpuQueryBridge::markEditPending(
	const t3dGpuPageTicket_t &ticket,
	const Vector2i			 &location ) {
	if ( !m_storage || !ticket.m_key.valid() || ticket.m_key.m_nChannel != T3D_GPU_PAGE_HEIGHT ||
		 !matchesRegion( ticket.m_key.m_nRegionIndex, location ) )
		return false;
	Record &entry = m_entries[ticket.m_key.m_nRegionIndex];
	if ( entry.pending && t3dGpuPageTicketCurrent( entry.ticket, ticket ) )
		return true; // Same unfinished readback may be retried after cancellation.
	if ( entry.generation && entry.ticket.m_nDeviceEpoch == ticket.m_nDeviceEpoch &&
		 ( entry.ticket.m_nSourceGeneration > ticket.m_nSourceGeneration ||
		   ( entry.ticket.m_nSourceGeneration == ticket.m_nSourceGeneration &&
			 entry.ticket.m_nEditGeneration >= ticket.m_nEditGeneration ) ) )
		return false;
	entry.ticket = ticket;
	entry.generation = ticket.m_nEditGeneration;
	entry.pending = true;
	entry.ready = false;
	m_storage->invalidateActive( location );
	return true;
}

bool Terrain3DGpuQueryBridge::acceptDirtyReadback(
	const t3dGpuPageTicket_t &ticket,
	const Vector2i			 &location,
	const PackedByteArray	 &r16Bytes ) {
	if ( !m_storage || !m_storage->isActiveOwner() || !validPayload( r16Bytes ) || !ticket.m_key.valid() ||
		 ticket.m_key.m_nChannel != T3D_GPU_PAGE_HEIGHT || !matchesRegion( ticket.m_key.m_nRegionIndex, location ) )
		return false;
	Record &entry = m_entries[ticket.m_key.m_nRegionIndex];
	if ( !entry.pending || !t3dGpuPageTicketCurrent( ticket, entry.ticket ) )
		return false;
	if ( !m_storage->syncActiveHeightPage( location, r16Bytes.ptr(), HEIGHT_BYTES ) )
		return false;
	entry.pending = false;
	entry.ready = true;
	return true;
}

bool Terrain3DGpuQueryBridge::current(
	const t3dGpuPageTicket_t &ticket ) const {
	if ( !ticket.m_key.valid() || ticket.m_key.m_nChannel != T3D_GPU_PAGE_HEIGHT )
		return false;
	const Record &entry = m_entries[ticket.m_key.m_nRegionIndex];
	return entry.ready && !entry.pending && t3dGpuPageTicketCurrent( ticket, entry.ticket );
}

void Terrain3DGpuQueryBridge::reset() {
	for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i )
		m_entries[i] = Record{};
}
