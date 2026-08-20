// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_ACTIVE_HEIGHT_RESIDENCY_H
#define TERRAIN3D_ACTIVE_HEIGHT_RESIDENCY_H

#include "core/terrain_3d_compiler.h"
#include "Terrain3DConstants.h"
#include "streaming/terrain_3d_residency_state.h"

#include <EASTL/array.h>
#include <cstddef>
#include <cstdint>

class T3DRegionRepository;
class T3DRuntimePageStore;
class T3DSamplingStorage;

struct T3DActiveHeightResidencyStats {
	int nReconciliationsLastTick = 0;
	int nSlotsExaminedLastTick = 0;
	int nRequiredLastTick = 0;
	int nMissingRequiredLastTick = 0;
	int nPublishedLastTick = 0;
	int nEvictedLastTick = 0;
	int nPredictedRequestsLastTick = 0;
};

struct T3DActiveHeightResidencyResult {
	static constexpr int CPU_REQUEST_CAPACITY = 25;
	eastl::array<Vector2i, CPU_REQUEST_CAPACITY> cpuSourceRequests{};
	int nCpuSourceRequestCount = 0;

	void requestCpuSource( const Vector2i &rLocation ) {
		if ( nCpuSourceRequestCount < CPU_REQUEST_CAPACITY ) {
			cpuSourceRequests[( size_t ) ( nCpuSourceRequestCount++ )] = rLocation;
		}
	}
};

// Owns only the spatial policy for the fixed active-height sampling bank.
// T3DSamplingStorage owns publication/leases; T3DRuntimePageStore owns cooked
// page preparation. Missing authoritative CPU sources are returned to the
// region-residency coordinator instead of introducing a cache/facade back-edge.
class T3DActiveHeightResidency {
public:
	T3DActiveHeightResidency(
		T3DRegionRepository *pRepository,
		T3DSamplingStorage *pSamplingStorage,
		T3DRuntimePageStore *pRuntimePageStore
	);

	void reset( void );
	EA_FORCE_INLINE void markDirty( void ) { m_bDirty = true; }
	EA_FORCE_INLINE bool isDirty( void ) const { return m_bDirty; }
	T3DActiveHeightResidencyResult update( const T3DResidencyFrameState &rFrame, bool bForce );
	EA_FORCE_INLINE const T3DActiveHeightResidencyStats &getStats( void ) const { return m_stats; }

private:
	static constexpr int ACTIVE_RADIUS = 2;

	T3DRegionRepository *m_pRepository = nullptr;
	T3DSamplingStorage *m_pSamplingStorage = nullptr;
	T3DRuntimePageStore *m_pRuntimePageStore = nullptr;
	bool m_bDirty = true;
	uint64_t m_nStreamingGenerationLast = 0u;
	uint64_t m_nTopologyGenerationLast = 0u;
	Vector2i m_center = V2I_MAX;
	T3DActiveHeightResidencyStats m_stats;
};

#endif // TERRAIN3D_ACTIVE_HEIGHT_RESIDENCY_H
