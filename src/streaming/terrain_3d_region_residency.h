// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_REGION_RESIDENCY_H
#define TERRAIN3D_REGION_RESIDENCY_H

#include "streaming/terrain_3d_residency_state.h"
#include "streaming/terrain_3d_runtime_profile.h"

class Terrain3DActiveHeightResidency;
class Terrain3DCpuRegionCache;
class Terrain3DData;
class Terrain3DFarSurfaceAtlasCache;
class Terrain3DGpuRegionCache;
class Terrain3DInstanceRegionCache;
class Terrain3DMiddleHeightRegionCache;
class Terrain3DRegionRepository;
class Terrain3DRuntimePageStore;
class Terrain3DSamplingStorage;

// Coordinates the ordered runtime residency pipeline. Individual cache classes
// own their storage, replacement policy, statistics, and reset/configuration state.
class Terrain3DRegionResidency {
public:
	Terrain3DRegionResidency( Terrain3DRegionRepository *pRepository,
	        Terrain3DSamplingStorage *pSamplingStorage,
	        Terrain3DRuntimePageStore *pRuntimePageStore,
	        Terrain3DActiveHeightResidency *pActiveHeightResidency,
	        Terrain3DCpuRegionCache *pCpuRegionCache,
	        Terrain3DInstanceRegionCache *pInstanceRegionCache,
	        Terrain3DMiddleHeightRegionCache *pMiddleHeightRegionCache,
	        Terrain3DGpuRegionCache *pGpuRegionCache,
	        Terrain3DFarSurfaceAtlasCache *pFarSurfaceAtlasCache );

	void reset( void );
	void update( Terrain3DData &rData, const t3dResidencyFrameState_t &rFrame, bool bForce );
	void resetRuntimeCaches( Terrain3DData &rData );
	void appendRuntimeProfileSample( const Terrain3DData &rData, t3dRuntimeProfileSample_t &rSample ) const;

	void setBudgetOverrides( int nGpuUploadLayers, int nCpuLoadLayers, int nInstanceLoadPages );
	void clearBudgetOverrides( void );
	int effectiveGpuUploadLayers( int nConfigured ) const;
	int effectiveCpuLoadLayers( int nConfigured ) const;
	int effectiveInstanceLoadPages( int nConfigured ) const;

private:
	Terrain3DRegionRepository *m_pRepository = NULL;
	Terrain3DSamplingStorage *m_pSamplingStorage = NULL;
	Terrain3DRuntimePageStore *m_pRuntimePageStore = NULL;
	Terrain3DActiveHeightResidency *m_pActiveHeightResidency = NULL;
	Terrain3DCpuRegionCache *m_pCpuRegionCache = NULL;
	Terrain3DInstanceRegionCache *m_pInstanceRegionCache = NULL;
	Terrain3DMiddleHeightRegionCache *m_pMiddleHeightRegionCache = NULL;
	Terrain3DGpuRegionCache *m_pGpuRegionCache = NULL;
	Terrain3DFarSurfaceAtlasCache *m_pFarSurfaceAtlasCache = NULL;
	int m_nGpuUploadBudgetOverride = -1;
	int m_nCpuLoadBudgetOverride = -1;
	int m_nInstanceLoadBudgetOverride = -1;
};

#endif // TERRAIN3D_REGION_RESIDENCY_H
