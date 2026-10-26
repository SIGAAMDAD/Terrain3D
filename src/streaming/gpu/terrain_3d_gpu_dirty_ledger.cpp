#include "streaming/gpu/terrain_3d_gpu_dirty_ledger.h"
#include "streaming/gpu/terrain_3d_gpu_page_store.h"

uint8_t Terrain3DGpuDirtyLedger::maskFor(
	uint8_t channel ) {
	if ( channel == T3D_GPU_PAGE_HEIGHT )
		return DERIVED | COMPOSITE | SHADOW | MIDDLE_HEIGHT | FAR_VT | CPU_COLLISION;
	if ( channel == T3D_GPU_PAGE_COLOR || channel == T3D_GPU_PAGE_CONTROL )
		return COMPOSITE | FAR_VT | MATERIAL_VT;
	if ( channel == T3D_GPU_PAGE_DERIVED )
		return COMPOSITE;
	return 0u;
}
bool Terrain3DGpuDirtyLedger::invalidate(
	const t3dGpuPageTicket_t &source ) {
	if ( !source.m_key.valid() || !source.m_nDeviceEpoch || !source.m_nEditGeneration )
		return false;
	const uint8_t channel = source.m_key.m_nChannel;
	const uint8_t mask = maskFor( channel );
	if ( !mask )
		return false;
	Entry					 &entry = m_entries[source.m_key.m_nRegionIndex];
	const t3dGpuPageTicket_t &previous = entry.sources[channel];
	if ( previous.m_nDeviceEpoch == source.m_nDeviceEpoch &&
		 ( previous.m_nSourceGeneration > source.m_nSourceGeneration ||
		   ( previous.m_nSourceGeneration == source.m_nSourceGeneration &&
			 ( previous.m_nEditGeneration > source.m_nEditGeneration ||
			   ( previous.m_nEditGeneration == source.m_nEditGeneration &&
				 previous.m_nPublicationGeneration > source.m_nPublicationGeneration ) ) ) ) )
		return false;
	entry.sources[channel] = source;
	entry.pendingByChannel[channel] |= mask;
	return true;
}
bool Terrain3DGpuDirtyLedger::next(
	uint8_t				consumer,
	uint16_t		   &region,
	t3dGpuPageTicket_t &source ) const {
	return nextFrom( consumer, 0u, region, source );
}
bool Terrain3DGpuDirtyLedger::nextFrom(
	uint8_t				consumer,
	uint32_t			firstRegion,
	uint16_t		   &region,
	t3dGpuPageTicket_t &source ) const {
	if ( !consumer || ( consumer & ( consumer - 1u ) ) || consumer > CPU_COLLISION || firstRegion >= T3D_REGION_SLOTS )
		return false;
	for ( uint32_t r = firstRegion; r < T3D_REGION_SLOTS; ++r ) {
		for ( uint32_t c = 0u; c < T3D_GPU_PAGE_CHANNEL_COUNT; ++c ) {
			if ( !( m_entries[r].pendingByChannel[c] & consumer ) )
				continue;
			region = uint16_t( r );
			source = m_entries[r].sources[c];
			return true;
		}
	}
	return false;
}
bool Terrain3DGpuDirtyLedger::acknowledge(
	uint8_t					  consumer,
	const t3dGpuPageTicket_t &source ) {
	if ( !source.m_key.valid() || !consumer || ( consumer & ( consumer - 1u ) ) || consumer > CPU_COLLISION )
		return false;
	Entry		  &entry = m_entries[source.m_key.m_nRegionIndex];
	const uint32_t channel = source.m_key.m_nChannel;
	if ( !( entry.pendingByChannel[channel] & consumer ) || !t3dGpuPageTicketCurrent( source, entry.sources[channel] ) )
		return false;
	entry.pendingByChannel[channel] &= uint8_t( ~consumer );
	return true;
}
bool Terrain3DGpuDirtyLedger::isPending(
	uint8_t					  consumer,
	const t3dGpuPageTicket_t &source ) const {
	if ( !source.m_key.valid() || !consumer || ( consumer & ( consumer - 1u ) ) || consumer > CPU_COLLISION )
		return false;
	const Entry	  &entry = m_entries[source.m_key.m_nRegionIndex];
	const uint32_t channel = source.m_key.m_nChannel;
	return ( entry.pendingByChannel[channel] & consumer ) && t3dGpuPageTicketCurrent( source, entry.sources[channel] );
}
bool Terrain3DGpuDirtyLedger::invalidateOnly(
	uint8_t					  consumer,
	const t3dGpuPageTicket_t &source ) {
	if ( !source.m_key.valid() || !source.m_nDeviceEpoch || !source.m_nEditGeneration || !consumer ||
		 ( consumer & ( consumer - 1u ) ) || consumer > CPU_COLLISION ||
		 !( maskFor( source.m_key.m_nChannel ) & consumer ) )
		return false;
	Entry					 &entry = m_entries[source.m_key.m_nRegionIndex];
	const uint32_t			  channel = source.m_key.m_nChannel;
	const t3dGpuPageTicket_t &prior = entry.sources[channel];
	if ( prior.m_nDeviceEpoch == source.m_nDeviceEpoch &&
		 ( prior.m_nSourceGeneration > source.m_nSourceGeneration ||
		   ( prior.m_nSourceGeneration == source.m_nSourceGeneration &&
			 ( prior.m_nEditGeneration > source.m_nEditGeneration ||
			   ( prior.m_nEditGeneration == source.m_nEditGeneration &&
				 prior.m_nPublicationGeneration > source.m_nPublicationGeneration ) ) ) ) )
		return false;
	entry.sources[channel] = source;
	entry.pendingByChannel[channel] |= consumer;
	return true;
}
uint8_t Terrain3DGpuDirtyLedger::pending(
	uint16_t region ) const {
	if ( region >= T3D_REGION_SLOTS )
		return 0u;
	uint8_t mask = 0u;
	for ( uint32_t c = 0u; c < T3D_GPU_PAGE_CHANNEL_COUNT; ++c )
		mask |= m_entries[region].pendingByChannel[c];
	return mask;
}
uint8_t Terrain3DGpuDirtyLedger::pendingFor(
	uint16_t region,
	uint8_t	 channel ) const {
	return region < T3D_REGION_SLOTS && channel < T3D_GPU_PAGE_CHANNEL_COUNT
			   ? m_entries[region].pendingByChannel[channel]
			   : 0u;
}
void Terrain3DGpuDirtyLedger::reset() {
	for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i )
		m_entries[i] = Entry{};
}

void Terrain3DGpuDirtyLedger::invalidateHeightHaloNeighbors(
	Terrain3DGpuPageStore		 &store,
	const t3dGpuPageLogicalKey_t &edited,
	bool						  touchedWest,
	bool						  touchedNorth ) {
	if ( !edited.valid() || edited.m_nChannel != T3D_GPU_PAGE_HEIGHT || !store.active() )
		return;
	const uint32_t x = edited.m_nRegionIndex % T3D_REGION_MAP_SIZE;
	const uint32_t y = edited.m_nRegionIndex / T3D_REGION_MAP_SIZE;
	const uint16_t indices[2] = { uint16_t( edited.m_nRegionIndex - ( x > 0u ? 1u : 0u ) ),
								  uint16_t( edited.m_nRegionIndex - ( y > 0u ? T3D_REGION_MAP_SIZE : 0u ) ) };
	const bool	   need[2] = { touchedWest && x > 0u, touchedNorth && y > 0u };
	for ( uint32_t i = 0u; i < 2u; ++i ) {
		if ( !need[i] )
			continue;
		const t3dGpuPageLogicalKey_t neighbor{ indices[i], T3D_GPU_PAGE_HEIGHT, 0u };
		t3dGpuPageTicket_t			 current{};
		t3dGpuPageHandle_t			 page{};
		if ( store.publishedSnapshot( neighbor, current, page ) )
			invalidateOnly( DERIVED, current );
	}
}
