// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_REGION_RESIDENCY_H
#define TERRAIN3D_REGION_RESIDENCY_H

#include "streaming/terrain_3d_residency_state.h"
#include "streaming/terrain_3d_runtime_profile.h"

class T3DActiveHeightResidency;
class T3DData;
class T3DRegionRepository;
class T3DRuntimePageStore;
class T3DSamplingStorage;

// Coordinates the ordered runtime residency pipeline and owns temporary adaptive
// budget overrides shared by the individual caches. Cache internals remain with
// their current owners until their dedicated extractions land.
class T3DRegionResidency {
public:
	T3DRegionResidency(
		T3DRegionRepository *pRepository,
		T3DSamplingStorage *pSamplingStorage,
		T3DRuntimePageStore *pRuntimePageStore,
		T3DActiveHeightResidency *pActiveHeightResidency
	);

	void reset( void );
	void update( T3DData &rData, const T3DResidencyFrameState &rFrame, bool bForce );
	void resetRuntimeCaches( T3DData &rData );
	void appendRuntimeProfileSample( const T3DData &rData, t3druntimeProfileSample_t &rSample ) const;

	void setBudgetOverrides( int nGpuUploadLayers, int nCpuLoadLayers, int nInstanceLoadPages );
	void clearBudgetOverrides( void );
	int effectiveGpuUploadLayers( int nConfigured ) const;
	int effectiveCpuLoadLayers( int nConfigured ) const;
	int effectiveInstanceLoadPages( int nConfigured ) const;

private:
	T3DRegionRepository *m_pRepository = nullptr;
	T3DSamplingStorage *m_pSamplingStorage = nullptr;
	T3DRuntimePageStore *m_pRuntimePageStore = nullptr;
	T3DActiveHeightResidency *m_pActiveHeightResidency = nullptr;
	int m_nGpuUploadBudgetOverride = -1;
	int m_nCpuLoadBudgetOverride = -1;
	int m_nInstanceLoadBudgetOverride = -1;
};

#endif // TERRAIN3D_REGION_RESIDENCY_H
