// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_REGION_CACHE_COMMON_H
#define TERRAIN3D_REGION_CACHE_COMMON_H

#include "Terrain3DTypes.h"
#include "core/config/terrain_3d_compiler.h"
#include <stddef.h>
#include <stdint.h>

// CPU and instance caches have different admission/eviction policies, but
// maintain the same per-tick counters and residency-selection bookkeeping.
// Non-virtual internal storage only: no shared cache owner or renderer.
typedef struct t3dRegionCacheCommon_s
{
	bool	 m_bEnabled = false;
	bool	 m_bSelectionDirty = true;
	int		 m_nBudgetMb = 16;
	int		 m_nLoadBudgetMb = 2;
	int		 m_nCapacity = 0;
	int		 m_nResidentCount = 0;
	int		 m_nDesiredLastTick = 0;
	int		 m_nLoadsLastTick = 0;
	uint32_t m_nStallFlagsLastTick = 0u;
	int		 m_nEvictionsLastTick = 0;
	int		 m_nDemandLoads = 0;
	size_t	 m_nLoadBytesLastTick = 0u;
	uint64_t m_nServiceUsecLast = 0u;
	uint64_t m_nPrefetchStreamingGenerationLast = 0u;
	Vector2i m_center = s_v2iMax;

	EA_FORCE_INLINE void beginServiceTick(
		void ) {
		m_nLoadsLastTick = 0;
		m_nStallFlagsLastTick = 0u;
		m_nEvictionsLastTick = 0;
		m_nLoadBytesLastTick = 0u;
	}
	EA_FORCE_INLINE void resetResidencyMetrics(
		const int nDefaultCapacity ) {
		m_nCapacity = nDefaultCapacity;
		m_nResidentCount = 0;
		m_nDesiredLastTick = 0;
		m_nLoadsLastTick = 0;
		m_nEvictionsLastTick = 0;
		m_nLoadBytesLastTick = 0u;
		m_nPrefetchStreamingGenerationLast = 0u;
		m_center = s_v2iMax;
		m_bSelectionDirty = true;
	}
} t3dRegionCacheCommon_t;

#endif // TERRAIN3D_REGION_CACHE_COMMON_H
