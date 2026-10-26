#ifndef TERRAIN3D_RENDER_TELEMETRY_H
#define TERRAIN3D_RENDER_TELEMETRY_H

#include "Terrain3DTypes.h"

#include "core/utility/terrain_3d_sync.h"
#include <stddef.h>
#include <stdint.h>

namespace t3d_render_telemetry_contract {
}

// Cumulative infrastructure upload counters. Domain algorithms retain their
// own visibility and publication counters; generic traffic is recorded here.
template <size_t OwnerCount, size_t PolicyCount>
class Terrain3DRenderTelemetry
{
private:
	t3dAtomicU64_t m_uploadCalls[OwnerCount][PolicyCount]{};
	t3dAtomicU64_t m_uploadBytes[OwnerCount][PolicyCount]{};
	t3dAtomicU64_t m_events[OwnerCount][EVENT_COUNT]{};
	t3dAtomicU64_t m_capacityHighWater[OwnerCount][PolicyCount]{};
	t3dAtomicU64_t m_indirectCommandsSubmitted[OwnerCount]{};
	t3dAtomicU64_t m_dirtyRangesUploaded[OwnerCount]{};

public:
	void recordUpload( size_t nOwner, size_t nPolicy, uint64_t nBytes ) {
		if ( nOwner >= OwnerCount || nPolicy >= PolicyCount || nBytes == 0u ) {
			return;
		}
		m_uploadCalls[nOwner][nPolicy].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		m_uploadBytes[nOwner][nPolicy].fetch_add( nBytes, T3D_MEMORY_ORDER_RELAXED );
	}

	uint64_t getUploadCalls( size_t nOwner, size_t nPolicy ) const {
		return nOwner < OwnerCount && nPolicy < PolicyCount
		        ? m_uploadCalls[nOwner][nPolicy].load( T3D_MEMORY_ORDER_ACQUIRE )
		        : 0u;
	}

	uint64_t getUploadBytes( size_t nOwner, size_t nPolicy ) const {
		return nOwner < OwnerCount && nPolicy < PolicyCount
		        ? m_uploadBytes[nOwner][nPolicy].load( T3D_MEMORY_ORDER_ACQUIRE )
		        : 0u;
	}

	void recordCapacity( size_t nOwner, size_t nPolicy, uint64_t nBytes ) {
		if ( nOwner >= OwnerCount || nPolicy >= PolicyCount ) {
			return;
		}
		t3dAtomicU64_t &high_water = m_capacityHighWater[nOwner][nPolicy];
		uint64_t observed = high_water.load( T3D_MEMORY_ORDER_RELAXED );
		while ( observed < nBytes && !high_water.compare_exchange_weak(
		                       observed, nBytes, T3D_MEMORY_ORDER_RELEASE, T3D_MEMORY_ORDER_RELAXED ) ) {
		}
	}

	uint64_t getCapacityHighWater( size_t nOwner, size_t nPolicy ) const {
		return nOwner < OwnerCount && nPolicy < PolicyCount
		        ? m_capacityHighWater[nOwner][nPolicy].load( T3D_MEMORY_ORDER_ACQUIRE )
		        : 0u;
	}

	void recordIndirectCommandsSubmitted( size_t nOwner, uint64_t nCommands ) {
		if ( nOwner < OwnerCount && nCommands > 0u ) {
			m_indirectCommandsSubmitted[nOwner].fetch_add( nCommands, T3D_MEMORY_ORDER_RELAXED );
		}
	}

	uint64_t getIndirectCommandsSubmitted( size_t nOwner ) const {
		return nOwner < OwnerCount ? m_indirectCommandsSubmitted[nOwner].load( T3D_MEMORY_ORDER_ACQUIRE ) : 0u;
	}

	void recordDirtyRangeUploaded( size_t nOwner ) {
		if ( nOwner < OwnerCount ) {
			m_dirtyRangesUploaded[nOwner].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		}
	}

	uint64_t getDirtyRangesUploaded( size_t nOwner ) const {
		return nOwner < OwnerCount ? m_dirtyRangesUploaded[nOwner].load( T3D_MEMORY_ORDER_ACQUIRE ) : 0u;
	}

	void recordEvent( size_t nOwner, t3dRenderTelemetryEvent_t event ) {
		if ( nOwner < OwnerCount && event < EVENT_COUNT ) {
			m_events[nOwner][event].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		}
	}

	uint64_t getEvent( size_t nOwner, t3dRenderTelemetryEvent_t event ) const {
		return nOwner < OwnerCount && event < EVENT_COUNT
		        ? m_events[nOwner][event].load( T3D_MEMORY_ORDER_ACQUIRE )
		        : 0u;
	}
};

#endif // TERRAIN3D_RENDER_TELEMETRY_H
