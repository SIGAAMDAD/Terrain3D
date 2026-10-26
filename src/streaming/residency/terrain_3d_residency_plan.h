#ifndef TERRAIN3D_RESIDENCY_PLAN_H
#define TERRAIN3D_RESIDENCY_PLAN_H

#include "streaming/residency/terrain_3d_gpu_page_contract.h"
#include <stdint.h>

// P01 is a dormant shadow ledger; it does not grant physical GPU ownership.
// All writes occur on the residency owner's update thread.
class Terrain3DResidencyPlan
{
public:
	static constexpr uint32_t CHANNELS = T3D_GPU_PAGE_CHANNEL_COUNT;
	static constexpr uint32_t SLOTS = T3D_REGION_SLOTS * CHANNELS;
	static constexpr uint32_t QUEUE_CAPACITY = 512u;

	struct Entry
	{
		t3dGpuPageTicket_t m_ticket{};
		uint64_t		   m_nLastReaderEpoch = 0u;
		uint32_t		   m_nRetryAfterTick = 0u;
		uint8_t			   m_nPriority = 0u;
		uint8_t			   m_nState = T3D_GPU_PAGE_REQUEST_EMPTY;
	};

	Terrain3DResidencyPlan() = default;
	void reset( uint32_t deviceEpoch );
	void setEnabled(
		bool enabled ) {
		m_bEnabled = enabled;
	}
	bool enabled() const {
		return m_bEnabled;
	}
	bool		 request( const t3dGpuPageTicket_t &ticket, t3dGpuPagePriority_t priority, uint32_t tick );
	bool		 dequeue( t3dGpuPageTicket_t &ticket );
	bool		 complete( const t3dGpuPageTicket_t &ticket, bool succeeded, uint32_t tick );
	void		 markReader( const t3dGpuPageLogicalKey_t &key, uint64_t submissionEpoch );
	const Entry *find( const t3dGpuPageLogicalKey_t &key ) const;
	bool		 current( const t3dGpuPageTicket_t &ticket ) const;
	uint32_t	 queued() const {
		return m_nQueued;
	}
	uint32_t dropped() const {
		return m_nDropped;
	}
	uint32_t epoch() const {
		return m_nDeviceEpoch;
	}

private:
	static uint32_t index( const t3dGpuPageLogicalKey_t &key );
	Entry			m_entries[SLOTS]{};
	uint16_t		m_queue[QUEUE_CAPACITY]{};
	uint32_t		m_nQueued = 0u;
	uint32_t		m_nDropped = 0u;
	uint32_t		m_nDeviceEpoch = 1u;
	bool			m_bEnabled = false;
};

#endif // TERRAIN3D_RESIDENCY_PLAN_H
