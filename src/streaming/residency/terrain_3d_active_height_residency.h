// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_ACTIVE_HEIGHT_RESIDENCY_H
#define TERRAIN3D_ACTIVE_HEIGHT_RESIDENCY_H

#include "Terrain3DTypeSupport.h"
#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"
#include "eastl_config.h"
#include "streaming/residency/terrain_3d_residency_state.h"

#include <stddef.h>
#include <stdint.h>
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"

typedef struct t3dActiveHeightResidencyResult_s
{
	t3dRegionCoord_t m_szCpuSourceRequests[T3D_CPU_REQUEST_CAPACITY]{};
	int				 m_nCpuSourceRequestCount = 0;

	void requestCpuSource(
		const Vector2i &rLocation ) {
		if ( m_nCpuSourceRequestCount < T3D_CPU_REQUEST_CAPACITY ) {
			m_szCpuSourceRequests[( size_t ) ( m_nCpuSourceRequestCount++ )] = t3dPackRegionCoord( rLocation );
		}
	}
} t3dActiveHeightResidencyResult_t;

// Owns only the spatial policy for the fixed active-height sampling bank.
// Terrain3DSamplingStorage owns publication/leases; Terrain3DRuntimePageStore owns cooked
// page preparation. Missing authoritative CPU sources are returned to the
// region-residency coordinator instead of introducing a cache/facade back-edge.
class Terrain3DActiveHeightResidency
{
public:
	Terrain3DActiveHeightResidency( Terrain3DRegionRepository *pRepository,
									Terrain3DSamplingStorage  *pSamplingStorage,
									Terrain3DRuntimePageStore *pRuntimePageStore );

	void				 reset( void );
	EA_FORCE_INLINE void markDirty(
		void ) {
		m_bDirty = true;
	}
	EA_FORCE_INLINE bool isDirty(
		void ) const {
		return m_bDirty;
	}
	t3dActiveHeightResidencyResult_t update( const t3dResidencyFrameState_s &rFrame, bool bForce );
	EA_FORCE_INLINE const t3dActiveHeightResidencyStats_t &getStats(
		void ) const {
		return m_stats;
	}

private:
	Terrain3DRegionRepository	   *m_pRepository = NULL;
	Terrain3DSamplingStorage	   *m_pSamplingStorage = NULL;
	Terrain3DRuntimePageStore	   *m_pRuntimePageStore = NULL;
	bool							m_bDirty = true;
	uint64_t						m_nStreamingGenerationLast = 0u;
	uint64_t						m_nTopologyGenerationLast = 0u;
	Vector2i						m_center = s_v2iMax;
	t3dActiveHeightResidencyStats_t m_stats;
};

#endif // TERRAIN3D_ACTIVE_HEIGHT_RESIDENCY_H
