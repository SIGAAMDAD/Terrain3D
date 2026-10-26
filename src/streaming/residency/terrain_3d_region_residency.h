// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_REGION_RESIDENCY_H
#define TERRAIN3D_REGION_RESIDENCY_H

#include "Terrain3DTypes.h"

#include "streaming/residency/terrain_3d_residency_state.h"
#include "streaming/residency/terrain_3d_residency_plan.h"
#include "streaming/policy/terrain_3d_runtime_profile.h"
#include "streaming/cache/terrain_3d_instance_region_cache.h"
#include "streaming/cache/terrain_3d_middle_height_region_cache.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "rendering/surface/terrain_3d_far_regional_cooker.h"
#include "streaming/residency/terrain_3d_active_height_residency.h"
#include "streaming/cache/terrain_3d_cpu_region_cache.h"
#include "streaming/cache/terrain_3d_gpu_region_cache.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"
#include "streaming/hierarchy/terrain_3d_hierarchy_scheduler.h"
class Terrain3DHierarchicalTileStore;

typedef struct t3dRegionResidencyUpdateContext_s
{
	typedef void ( *PrepareGpuFarSurfaceFn )( void *pContext );
	typedef void ( *ApplyInstanceChangesFn )( void *pContext, const t3dInstanceRegionCacheChanges_s &rChanges );
	typedef void ( *GpuPageTableChangedFn )( void *pContext );

	Terrain3DCompute				  *m_pCompute = NULL;
	t3dMiddleHeightRegionCacheConfig_t m_middleHeightConfig{};
	void							  *m_pOwnerContext = NULL;
	ApplyInstanceChangesFn			   m_applyInstanceChanges = NULL;
	GpuPageTableChangedFn			   m_gpuPageTableChanged = NULL;
    // Optional F03 owner-thread demand producer. Only F04 attaches its GPU
    // publication consumers; null means the original residency path is intact.
    Terrain3DHierarchyScheduler *m_pHierarchyScheduler = NULL;
    Terrain3DHierarchicalTileStore *m_pHierarchyTileStore = NULL;
    t3dHierarchyView_t m_hierarchyView{};
    t3dHierarchyScheduleLimits_t m_hierarchyLimits{};
    bool m_bHierarchyDemandEnabled = false;
} t3dRegionResidencyUpdateContext_t;

// Coordinates the ordered runtime residency pipeline. Individual cache classes
// own their storage, replacement policy, statistics, and reset/configuration state.
class Terrain3DRegionResidency
{
public:
	Terrain3DRegionResidency( Terrain3DRegionRepository		   *pRepository,
							  Terrain3DSamplingStorage		   *pSamplingStorage,
							  Terrain3DRuntimePageStore		   *pRuntimePageStore,
							  Terrain3DActiveHeightResidency   *pActiveHeightResidency,
							  Terrain3DCpuRegionCache		   *pCpuRegionCache,
							  Terrain3DInstanceRegionCache	   *pInstanceRegionCache,
							  Terrain3DMiddleHeightRegionCache *pMiddleHeightRegionCache,
							  Terrain3DGpuRegionCache		   *pGpuRegionCache,
							  Terrain3DFarRegionalCooker	   *pFarRegionalCooker );

	void reset( void );
	// The GPU-authoritative migration stays off until P03-P13 consumers exist.
	void setGpuAuthoritativePages(
		bool enabled ) {
		m_gpuPagePlan.setEnabled( enabled );
	}
	bool gpuAuthoritativePages() const {
		return m_gpuPagePlan.enabled();
	}
	const Terrain3DResidencyPlan &gpuPagePlan() const {
		return m_gpuPagePlan;
	}
	godot::Dictionary getSingleCopyBaselineStats() const;
	void
	update( const t3dResidencyFrameState_s &rFrame, bool bForce, const t3dRegionResidencyUpdateContext_t &rContext );
	void resetRuntimeCaches( const t3dRegionResidencyUpdateContext_t &rContext );
	void appendRuntimeProfileSample( t3dRuntimeProfileSample_t &rSample ) const;

	void setBudgetOverrides( int nGpuUploadLayers, int nCpuLoadLayers, int nInstanceLoadPages );
	void clearBudgetOverrides( void );
	int	 effectiveGpuUploadLayers( int nConfigured ) const;
	int	 effectiveCpuLoadLayers( int nConfigured ) const;
	int	 effectiveInstanceLoadPages( int nConfigured ) const;

private:
	Terrain3DRegionRepository		 *m_pRepository = NULL;
	Terrain3DSamplingStorage		 *m_pSamplingStorage = NULL;
	Terrain3DRuntimePageStore		 *m_pRuntimePageStore = NULL;
	Terrain3DActiveHeightResidency	 *m_pActiveHeightResidency = NULL;
	Terrain3DCpuRegionCache			 *m_pCpuRegionCache = NULL;
	Terrain3DInstanceRegionCache	 *m_pInstanceRegionCache = NULL;
	Terrain3DMiddleHeightRegionCache *m_pMiddleHeightRegionCache = NULL;
	Terrain3DGpuRegionCache			 *m_pGpuRegionCache = NULL;
	Terrain3DFarRegionalCooker	 *m_pFarRegionalCooker = NULL;
	Terrain3DResidencyPlan			  m_gpuPagePlan;
	mutable uint64_t				  m_szBaselineChannelPeakBytes[T3D_GPU_PAGE_CHANNEL_COUNT]{};
	mutable uint64_t				  m_nBaselineGpuPeakBytes = 0u;
	mutable uint64_t				  m_nBaselineCpuPeakBytes = 0u;
	int								  m_nGpuUploadBudgetOverride = -1;
	int								  m_nCpuLoadBudgetOverride = -1;
	int								  m_nInstanceLoadBudgetOverride = -1;
};

#endif // TERRAIN3D_REGION_RESIDENCY_H
