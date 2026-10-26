#include "core/utility/terrain_3d_object_bytes.h"
#include "streaming/residency/terrain_3d_residency_plan.h"
#include <string.h>

uint32_t Terrain3DResidencyPlan::index(
	const t3dGpuPageLogicalKey_t &key ) {
	return ( uint32_t ) key.m_nRegionIndex * CHANNELS + key.m_nChannel;
}
void Terrain3DResidencyPlan::reset(
	uint32_t deviceEpoch ) {
	t3dClearTrivialArrayBytes( m_entries );
	m_nQueued = 0u;
	m_nDropped = 0u;
	m_nDeviceEpoch = deviceEpoch ? deviceEpoch : 1u;
}
const Terrain3DResidencyPlan::Entry *Terrain3DResidencyPlan::find(
	const t3dGpuPageLogicalKey_t &key ) const {
	return key.valid() ? &m_entries[index( key )] : nullptr;
}
bool Terrain3DResidencyPlan::current(
	const t3dGpuPageTicket_t &ticket ) const {
	const Entry *entry = find( ticket.m_key );
	return entry && t3dGpuPageTicketCurrent( ticket, entry->m_ticket );
}
bool Terrain3DResidencyPlan::request(
	const t3dGpuPageTicket_t &ticket,
	t3dGpuPagePriority_t	  priority,
	uint32_t				  tick ) {
	if ( !m_bEnabled || !ticket.m_key.valid() || ticket.m_nDeviceEpoch != m_nDeviceEpoch || priority == 0u )
		return false;
	const uint32_t slot = index( ticket.m_key );
	Entry		  &entry = m_entries[slot];
	if ( entry.m_nState == T3D_GPU_PAGE_REQUEST_FAILED && tick < entry.m_nRetryAfterTick &&
		 t3dGpuPageTicketCurrent( ticket, entry.m_ticket ) )
		return false;
	if ( t3dGpuPageTicketCurrent( ticket, entry.m_ticket ) ) {
		if ( priority > entry.m_nPriority )
			entry.m_nPriority = priority;
		if ( entry.m_nState != T3D_GPU_PAGE_REQUEST_EMPTY && entry.m_nState != T3D_GPU_PAGE_REQUEST_FAILED )
			return true;
	}
	if ( entry.m_nState == T3D_GPU_PAGE_REQUEST_QUEUED ) {
		// Stale queued work is superseded in place: one queue slot per logical key.
		entry.m_ticket = ticket;
		entry.m_nPriority = priority;
		entry.m_nRetryAfterTick = 0u;
		return true;
	}
	if ( m_nQueued >= QUEUE_CAPACITY ) {
		++m_nDropped;
		return false;
	}
	entry.m_ticket = ticket;
	entry.m_nPriority = priority;
	entry.m_nState = T3D_GPU_PAGE_REQUEST_QUEUED;
	entry.m_nRetryAfterTick = 0u;
	m_queue[m_nQueued++] = ( uint16_t ) slot;
	return true;
}
bool Terrain3DResidencyPlan::dequeue(
	t3dGpuPageTicket_t &ticket ) {
	if ( !m_bEnabled || m_nQueued == 0u )
		return false;
	uint32_t best = UINT32_MAX;
	uint8_t	 bestPriority = 0u;
	for ( uint32_t i = 0u; i < m_nQueued; ++i ) {
		const Entry &entry = m_entries[m_queue[i]];
		if ( entry.m_nState == T3D_GPU_PAGE_REQUEST_QUEUED &&
			 ( best == UINT32_MAX || entry.m_nPriority > bestPriority ) ) {
			best = i;
			bestPriority = entry.m_nPriority;
		}
	}
	if ( best == UINT32_MAX ) {
		m_nQueued = 0u;
		return false;
	}
	Entry &entry = m_entries[m_queue[best]];
	ticket = entry.m_ticket;
	entry.m_nState = T3D_GPU_PAGE_REQUEST_PREPARING;
	m_queue[best] = m_queue[--m_nQueued];
	return true;
}
bool Terrain3DResidencyPlan::complete(
	const t3dGpuPageTicket_t &ticket,
	bool					  succeeded,
	uint32_t				  tick ) {
	if ( !current( ticket ) )
		return false;
	Entry &entry = m_entries[index( ticket.m_key )];
	if ( entry.m_nState != T3D_GPU_PAGE_REQUEST_PREPARING )
		return false;
	entry.m_nState = succeeded ? T3D_GPU_PAGE_REQUEST_READY : T3D_GPU_PAGE_REQUEST_FAILED;
	if ( !succeeded )
		entry.m_nRetryAfterTick = tick > UINT32_MAX - 60u ? UINT32_MAX : tick + 60u;
	return true;
}
void Terrain3DResidencyPlan::markReader(
	const t3dGpuPageLogicalKey_t &key,
	uint64_t					  submissionEpoch ) {
	if ( !key.valid() )
		return;
	Entry &entry = m_entries[index( key )];
	if ( submissionEpoch > entry.m_nLastReaderEpoch )
		entry.m_nLastReaderEpoch = submissionEpoch;
}
