// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RESIDENCY_STATE_H
#define TERRAIN3D_RESIDENCY_STATE_H

#include "data/terrain_3d_region_repository.h"
#include "streaming/terrain_3d_gpu_cache_constants.h"
#include "streaming/terrain_3d_runtime_page_store.h"
#include "streaming/terrain_3d_streaming_policy.h"

#include <cstdint>

struct T3DResidencyFrameState {
	Vector3 worldPosition = V3_ZERO;
	Vector2i streamingCenter = V2I_MAX;
	Vector2i residencyCenter = V2I_MAX;
	Vector2i predictedCenter = V2I_MAX;
	Vector2i activeHeightCenter = V2I_MAX;
	const uint8_t *pStreamingStates = nullptr;
	const uint8_t *pStreamingPrefetch = nullptr;
	uint64_t streamingGeneration = 0u;
	int nRegionMapSize = 0;
	int nStreamingActiveRadius = 0;
	int nMiddleLods = 0;
	int nGpuHysteresisTicks = TERRAIN3D_GPU_CACHE_DEFAULT_HYSTERESIS_TICKS;
	bool bAuthoring = false;
	bool bRuntimeTiering = false;
	bool bCookedPagesEnabled = false;
	bool bProfilingEnabled = false;
	bool bCpuRegionCacheEnabled = false;
	T3DRuntimePageConfig runtimePageConfig;

	bool containsLocation( const Vector2i &rLocation ) const {
		if ( nRegionMapSize <= 0 ) {
			return false;
		}
		const int half = nRegionMapSize / 2;
		return rLocation.x >= -half && rLocation.x < half &&
			rLocation.y >= -half && rLocation.y < half;
	}

	t3dRegionStreamingState_t streamingState( const Vector2i &rLocation ) const {
		if ( !pStreamingStates || !containsLocation( rLocation ) ) {
			return TERRAIN3D_REGION_STREAMING_UNLOADED;
		}
		const int mapIndex = T3DRegionRepository::getRegionMapIndex( rLocation );
		return mapIndex >= 0 ?
			( t3dRegionStreamingState_t ) ( pStreamingStates[mapIndex] ) :
			TERRAIN3D_REGION_STREAMING_UNLOADED;
	}

	bool isPrefetch( const Vector2i &rLocation ) const {
		if ( !pStreamingPrefetch || !containsLocation( rLocation ) ) {
			return false;
		}
		const int mapIndex = T3DRegionRepository::getRegionMapIndex( rLocation );
		return mapIndex >= 0 && pStreamingPrefetch[mapIndex] != 0u;
	}
};

#endif // TERRAIN3D_RESIDENCY_STATE_H
