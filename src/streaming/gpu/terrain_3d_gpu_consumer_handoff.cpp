#include "streaming/gpu/terrain_3d_gpu_consumer_handoff.h"

int Terrain3DGpuConsumerHandoff::consumerIndex(
	uint8_t consumer ) {
	if ( !consumer || consumer > Terrain3DGpuDirtyLedger::CPU_COLLISION || ( consumer & ( consumer - 1u ) ) )
		return -1;
	for ( int i = 0; i < int( SLOTS ); ++i )
		if ( consumer == uint8_t( 1u << i ) )
			return i;
	return -1;
}

const Terrain3DGpuConsumerHandoff::Work *Terrain3DGpuConsumerHandoff::pending(
	uint8_t consumer ) const {
	const int index = consumerIndex( consumer );
	return index >= 0 && m_work[index].active ? &m_work[index] : nullptr;
}

bool Terrain3DGpuConsumerHandoff::claim(
	uint8_t					 consumer,
	Terrain3DGpuDirtyLedger &dirty,
	Terrain3DGpuPageStore	&store,
	uint64_t				 readerEpoch ) {
	const int index = consumerIndex( consumer );
	if ( index < 0 || !readerEpoch || !store.active() || m_work[index].active )
		return false;
	uint16_t		   region = UINT16_MAX;
	t3dGpuPageTicket_t ticket{};
	if ( !dirty.next( consumer, region, ticket ) )
		return false;
	t3dGpuPageTicket_t current{};
	t3dGpuPageHandle_t handle{};
	if ( !store.publishedSnapshot( ticket.m_key, current, handle ) || !t3dGpuPageTicketCurrent( ticket, current ) ||
		 !store.pool( ticket.m_key.m_nChannel ).pinUntil( handle, readerEpoch ) )
		return false;
	m_work[index].source = ticket;
	m_work[index].page = handle;
	m_work[index].consumer = consumer;
	m_work[index].active = true;
	m_readerEpoch[index] = readerEpoch;
	return true;
}

bool Terrain3DGpuConsumerHandoff::complete(
	uint8_t					 consumer,
	Terrain3DGpuDirtyLedger &dirty,
	Terrain3DGpuPageStore	&store,
	uint64_t				 completedEpoch ) {
	const int index = consumerIndex( consumer );
	if ( index < 0 || !m_work[index].active || !completedEpoch || completedEpoch < m_readerEpoch[index] )
		return false;
	Work	  &work = m_work[index];
	const bool current = store.snapshotCurrent( work.source, work.page ) && dirty.isPending( consumer, work.source );
	// Even an obsolete submission is no longer in flight after this epoch.
	const t3dGpuPageTicket_t ticket = work.source;
	work = Work{};
	m_readerEpoch[index] = 0u;
	return current && dirty.acknowledge( consumer, ticket );
}

bool Terrain3DGpuConsumerHandoff::reject(
	uint8_t consumer ) {
	const int index = consumerIndex( consumer );
	if ( index < 0 || !m_work[index].active )
		return false;
	// The caller must have confirmed that no GPU dispatch occurred.
	m_work[index] = Work{};
	m_readerEpoch[index] = 0u;
	return true;
}

void Terrain3DGpuConsumerHandoff::reset() {
	for ( uint32_t i = 0u; i < SLOTS; ++i ) {
		m_work[i] = Work{};
		m_readerEpoch[i] = 0u;
	}
}
