// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RESIDENCY_STATE_H
#define TERRAIN3D_RESIDENCY_STATE_H

#include "data/persistence/terrain_3d_region_repository.h"
#include "Terrain3DConstants.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"
#include "streaming/policy/terrain_3d_streaming_policy.h"

#include <stdint.h>

typedef struct t3dResidencyFrameState_s {

	Vector3 m_worldPosition = s_v3Zero;
	Vector2i m_streamingCenter = s_v2iMax;
	Vector2i m_residencyCenter = s_v2iMax;
	Vector2i m_predictedCenter = s_v2iMax;
	Vector2i m_activeHeightCenter = s_v2iMax;
	const uint8_t *m_pStreamingStates = NULL;
	const uint8_t *m_pStreamingPrefetch = NULL;
	uint64_t m_nStreamingGeneration = 0u;
	int m_nStreamingActiveRadius = 0;
	int m_nMiddleLods = 0;
	int m_nGpuHysteresisTicks = T3D_GPU_CACHE_DEFAULT_HYSTERESIS_TICKS;
	bool m_bAuthoring = false;
	bool m_bRuntimeTiering = false;
	bool m_bCookedPagesEnabled = false;
	bool m_bProfilingEnabled = false;
	bool m_bCpuRegionCacheEnabled = false;
	t3dRuntimePageConfig_s m_runtimePageConfig;

	bool containsLocation( const Vector2i &rLocation ) const {
		return t3dRegionLocationValid( rLocation, T3D_REGION_MAP_SIZE );
	}

	t3dRegionStreamingState_t streamingState( const Vector2i &rLocation ) const {
		if ( !m_pStreamingStates || !containsLocation( rLocation ) ) {
			return TERRAIN3D_REGION_STREAMING_UNLOADED;
		}
		const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
		return mapIndex >= 0 ? ( t3dRegionStreamingState_t )( m_pStreamingStates[mapIndex] ) : TERRAIN3D_REGION_STREAMING_UNLOADED;
	}

	bool isPrefetch( const Vector2i &rLocation ) const {
		if ( !m_pStreamingPrefetch || !containsLocation( rLocation ) ) {
			return false;
		}
		const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
		return mapIndex >= 0 && m_pStreamingPrefetch[mapIndex] != 0u;
	}
} t3dResidencyFrameState_t;

inline bool t3dResidencyFrameWantsRuntimePage( void *const pContext, const Vector2i& rLocation )
{
	const t3dResidencyFrameState_s *const pFrame = ( const t3dResidencyFrameState_s * )( pContext );
	return pFrame && ( pFrame->streamingState( rLocation ) >= TERRAIN3D_REGION_STREAMING_HOT || pFrame->isPrefetch( rLocation ) );
}

#endif // TERRAIN3D_RESIDENCY_STATE_H
