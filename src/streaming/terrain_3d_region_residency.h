// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_REGION_RESIDENCY_H
#define TERRAIN3D_REGION_RESIDENCY_H

#include "streaming/terrain_3d_residency_state.h"
#include "streaming/terrain_3d_runtime_profile.h"

class T3DActiveHeightResidency;
class T3DCpuRegionCache;
class T3DData;
class T3DFarSurfaceAtlasCache;
class T3DGpuRegionCache;
class T3DInstanceRegionCache;
class T3DMiddleHeightRegionCache;
class T3DRegionRepository;
class T3DRuntimePageStore;
class T3DSamplingStorage;

// Coordinates the ordered runtime residency pipeline. Individual cache classes
// own their storage, replacement policy, statistics, and reset/configuration state.
class T3DRegionResidency {
public:
	T3DRegionResidency( T3DRegionRepository *pRepository,
			T3DSamplingStorage *pSamplingStorage,
			T3DRuntimePageStore *pRuntimePageStore,
			T3DActiveHeightResidency *pActiveHeightResidency,
			T3DCpuRegionCache *pCpuRegionCache,
			T3DInstanceRegionCache *pInstanceRegionCache,
			T3DMiddleHeightRegionCache *pMiddleHeightRegionCache,
			T3DGpuRegionCache *pGpuRegionCache,
			T3DFarSurfaceAtlasCache *pFarSurfaceAtlasCache );

	void reset( void );
	void update( T3DData &rData, const t3dResidencyFrameState_t &rFrame, bool bForce );
	void resetRuntimeCaches( T3DData &rData );
	void appendRuntimeProfileSample( const T3DData &rData, t3dRuntimeProfileSample_t &rSample ) const;

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
	T3DCpuRegionCache *m_pCpuRegionCache = nullptr;
	T3DInstanceRegionCache *m_pInstanceRegionCache = nullptr;
	T3DMiddleHeightRegionCache *m_pMiddleHeightRegionCache = nullptr;
	T3DGpuRegionCache *m_pGpuRegionCache = nullptr;
	T3DFarSurfaceAtlasCache *m_pFarSurfaceAtlasCache = nullptr;
	int m_nGpuUploadBudgetOverride = -1;
	int m_nCpuLoadBudgetOverride = -1;
	int m_nInstanceLoadBudgetOverride = -1;
};

#endif // TERRAIN3D_REGION_RESIDENCY_H
