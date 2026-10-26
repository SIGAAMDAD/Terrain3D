// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_DATA_RESIDENCY_ADAPTER_H
#define TERRAIN3D_DATA_RESIDENCY_ADAPTER_H

#include "rendering/terrain_3d_material.h"
#include "rendering/terrain_3d_far_surface_atlas_cache.h"
#include "streaming/terrain_3d_active_height_residency.h"
#include "streaming/terrain_3d_cpu_region_cache.h"
#include "streaming/terrain_3d_gpu_region_cache.h"
#include "streaming/terrain_3d_instance_region_cache.h"
#include "streaming/terrain_3d_middle_height_region_cache.h"
#include "streaming/terrain_3d_region_residency.h"

class Terrain3DMapEditService;
class Terrain3DRegionPersistence;
class Terrain3DRegionRepository;
class Terrain3DRuntimeArtifactService;
class Terrain3DSamplingStorage;

// Owns the mutable residency/cache domain for Terrain3DData. The adapter keeps
// one canonical instance of every cache and exposes borrowed references to the
// Terrain3DData facade; no duplicate page tables or residency state are created.
class Terrain3DDataResidencyAdapter {
public:
	Terrain3DDataResidencyAdapter( Terrain3DRegionRepository &rRepository,
	        Terrain3DSamplingStorage &rSamplingStorage,
	        Terrain3DMapEditService &rMapEditService,
	        Terrain3DRegionPersistence &rPersistence,
	        Terrain3DRuntimeArtifactService &rRuntimeArtifacts );
	Terrain3DDataResidencyAdapter( const Terrain3DDataResidencyAdapter & ) = delete;
	Terrain3DDataResidencyAdapter &operator=( const Terrain3DDataResidencyAdapter & ) = delete;

	EA_FORCE_INLINE Terrain3DCpuRegionCache &cpuRegionCache( void ) {
		return m_cpuRegionCache;
	}
	EA_FORCE_INLINE const Terrain3DCpuRegionCache &cpuRegionCache( void ) const {
		return m_cpuRegionCache;
	}
	EA_FORCE_INLINE Terrain3DInstanceRegionCache &instanceRegionCache( void ) {
		return m_instanceRegionCache;
	}
	EA_FORCE_INLINE const Terrain3DInstanceRegionCache &instanceRegionCache( void ) const {
		return m_instanceRegionCache;
	}
	EA_FORCE_INLINE Terrain3DMiddleHeightRegionCache &middleHeightRegionCache( void ) {
		return m_middleHeightRegionCache;
	}
	EA_FORCE_INLINE const Terrain3DMiddleHeightRegionCache &middleHeightRegionCache( void ) const {
		return m_middleHeightRegionCache;
	}
	EA_FORCE_INLINE Terrain3DGpuRegionCache &gpuRegionCache( void ) {
		return m_gpuRegionCache;
	}
	EA_FORCE_INLINE const Terrain3DGpuRegionCache &gpuRegionCache( void ) const {
		return m_gpuRegionCache;
	}
	EA_FORCE_INLINE Terrain3DFarSurfaceAtlasCache &farSurfaceAtlasCache( void ) {
		return m_farSurfaceAtlasCache;
	}
	EA_FORCE_INLINE const Terrain3DFarSurfaceAtlasCache &farSurfaceAtlasCache( void ) const {
		return m_farSurfaceAtlasCache;
	}
	EA_FORCE_INLINE Terrain3DActiveHeightResidency &activeHeightResidency( void ) {
		return m_activeHeightResidency;
	}
	EA_FORCE_INLINE const Terrain3DActiveHeightResidency &activeHeightResidency( void ) const {
		return m_activeHeightResidency;
	}
	EA_FORCE_INLINE Terrain3DRegionResidency &regionResidency( void ) {
		return m_regionResidency;
	}
	EA_FORCE_INLINE const Terrain3DRegionResidency &regionResidency( void ) const {
		return m_regionResidency;
	}

	void resetCpu( bool bReleaseCleanMaps );
	void resetMiddleHeight( void );
	void resetGpu( bool bClearTextures );
	void resetFarSurface( void );
	void resetRegionResidency( void );
	void deactivateCaches( bool bClearGpuTextures );

	void setBudgetOverrides( int nGpuUploadLayers, int nCpuLoadLayers, int nInstanceLoadPages );
	void clearBudgetOverrides( void );
	int effectiveGpuUploadLayers( void ) const;
	int effectiveCpuLoadLayers( void ) const;
	int effectiveInstanceLoadPages( void ) const;

	void updateRegionResidency( const t3dResidencyFrameState_t &rFrame, bool bForce, const t3dRegionResidencyUpdateContext_t &rContext );
	void resetRuntimeCaches( const t3dRegionResidencyUpdateContext_t &rContext );
	void appendRuntimeProfileSample( t3dRuntimeProfileSample_t &rSample ) const;

private:
	Terrain3DCpuRegionCache m_cpuRegionCache;
	Terrain3DInstanceRegionCache m_instanceRegionCache;
	Terrain3DMiddleHeightRegionCache m_middleHeightRegionCache;
	Terrain3DGpuRegionCache m_gpuRegionCache;
	Terrain3DFarSurfaceAtlasCache m_farSurfaceAtlasCache;
	Terrain3DActiveHeightResidency m_activeHeightResidency;
	Terrain3DRegionResidency m_regionResidency;
};

#endif // TERRAIN3D_DATA_RESIDENCY_ADAPTER_H
