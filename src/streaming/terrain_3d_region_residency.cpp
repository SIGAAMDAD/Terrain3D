// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "streaming/terrain_3d_region_residency.h"

#include "rendering/terrain_3d_far_surface_atlas_cache.h"
#include "streaming/terrain_3d_active_height_residency.h"
#include "streaming/terrain_3d_cpu_region_cache.h"
#include "streaming/terrain_3d_gpu_region_cache.h"
#include "streaming/terrain_3d_instance_region_cache.h"
#include "streaming/terrain_3d_middle_height_region_cache.h"
#include "streaming/terrain_3d_runtime_page_store.h"

Terrain3DRegionResidency::Terrain3DRegionResidency( Terrain3DRegionRepository *const pRepository,
        Terrain3DSamplingStorage *const pSamplingStorage,
        Terrain3DRuntimePageStore *const pRuntimePageStore,
        Terrain3DActiveHeightResidency *const pActiveHeightResidency,
        Terrain3DCpuRegionCache *const pCpuRegionCache,
        Terrain3DInstanceRegionCache *const pInstanceRegionCache,
        Terrain3DMiddleHeightRegionCache *const pMiddleHeightRegionCache,
        Terrain3DGpuRegionCache *const pGpuRegionCache,
        Terrain3DFarSurfaceAtlasCache *const pFarSurfaceAtlasCache ) :
        m_pRepository( pRepository ),
        m_pSamplingStorage( pSamplingStorage ),
        m_pRuntimePageStore( pRuntimePageStore ),
        m_pActiveHeightResidency( pActiveHeightResidency ),
        m_pCpuRegionCache( pCpuRegionCache ),
        m_pInstanceRegionCache( pInstanceRegionCache ),
        m_pMiddleHeightRegionCache( pMiddleHeightRegionCache ),
        m_pGpuRegionCache( pGpuRegionCache ),
        m_pFarSurfaceAtlasCache( pFarSurfaceAtlasCache ) {
}

void Terrain3DRegionResidency::reset( void ) {
	if ( m_pActiveHeightResidency ) {
		m_pActiveHeightResidency->reset();
	}
}

void Terrain3DRegionResidency::update( const t3dResidencyFrameState_t &rFrame, const bool bForce,
        const t3dRegionResidencyUpdateContext_t &rContext ) {
	if ( !m_pRepository || !m_pSamplingStorage || !m_pRuntimePageStore || !m_pActiveHeightResidency || !m_pCpuRegionCache
	        || !m_pInstanceRegionCache || !m_pMiddleHeightRegionCache || !m_pGpuRegionCache ) {
		return;
	}

	auto service_runtime_pages = [this, &rFrame]() {
		t3dRuntimePageServiceState_t state;
		state.m_config = rFrame.m_runtimePageConfig;
		state.m_predictedCenter = rFrame.m_predictedCenter;
		state.m_nStreamingGeneration = rFrame.m_nStreamingGeneration;
		state.m_isWanted = &t3dResidencyFrameWantsRuntimePage;
		state.m_pWantedContext = const_cast<t3dResidencyFrameState_t *>( &rFrame );
		m_pRuntimePageStore->service( state );
	};

	// Complete the previous union batch before consumers inspect prepared pages.
	service_runtime_pages();
	m_pCpuRegionCache->update( rFrame, bForce, effectiveCpuLoadLayers( m_pCpuRegionCache->getLoadBudgetLayers() ) );

	const t3dActiveHeightResidencyResult_t activeResult = m_pActiveHeightResidency->update( rFrame, bForce );
	for ( int index = 0; index < activeResult.m_nCpuSourceRequestCount; ++index ) {
		m_pCpuRegionCache->requestSourceAsync( t3dUnpackRegionCoord( activeResult.m_szCpuSourceRequests[( size_t )( index )] ) );
	}

	t3dInstanceRegionCacheChanges_t instance_changes;
	m_pInstanceRegionCache->update(
	        rFrame, bForce, effectiveInstanceLoadPages( m_pInstanceRegionCache->getLoadBudgetPages() ), instance_changes );
	if ( rContext.m_applyInstanceChanges ) {
		rContext.m_applyInstanceChanges( rContext.m_pOwnerContext, instance_changes );
	}

	if ( m_pRepository->getRegionMapSize() > 0 && !rFrame.m_bAuthoring && rFrame.m_nMiddleLods > 0 ) {
		m_pMiddleHeightRegionCache->update(
		        rFrame, bForce, effectiveGpuUploadLayers( m_pGpuRegionCache->getUploadBudgetLayers() ), rContext.m_middleHeightConfig );
	} else if ( m_pMiddleHeightRegionCache->isReady() ) {
		m_pMiddleHeightRegionCache->reset();
	}

	if ( m_pFarSurfaceAtlasCache ) {
		m_pFarSurfaceAtlasCache->resetFrameStats();
	}
	if ( rContext.m_prepareGpuFarSurface ) {
		rContext.m_prepareGpuFarSurface( rContext.m_pOwnerContext );
	}
	const t3dGpuRegionCacheUpdateResult_t gpuResult = m_pGpuRegionCache->update(
	        rFrame, bForce, effectiveGpuUploadLayers( m_pGpuRegionCache->getUploadBudgetLayers() ), rContext.m_pCompute );
	if ( gpuResult.m_bPageTableChanged && rContext.m_gpuPageTableChanged ) {
		rContext.m_gpuPageTableChanged( rContext.m_pOwnerContext );
	}

	// Requests emitted by Active/Middle/GPU/instances form the next union batch.
	service_runtime_pages();
}

void Terrain3DRegionResidency::resetRuntimeCaches( const t3dRegionResidencyUpdateContext_t &rContext ) {
	if ( m_pCpuRegionCache ) {
		m_pCpuRegionCache->reset( true );
		m_pCpuRegionCache->refreshCapacity();
	}
	if ( m_pInstanceRegionCache ) {
		t3dInstanceRegionCacheChanges_t instance_changes;
		m_pInstanceRegionCache->reset( false, &instance_changes );
		if ( rContext.m_applyInstanceChanges ) {
			rContext.m_applyInstanceChanges( rContext.m_pOwnerContext, instance_changes );
		}
	}
	if ( m_pMiddleHeightRegionCache ) {
		m_pMiddleHeightRegionCache->reset();
	}
	if ( m_pGpuRegionCache ) {
		m_pGpuRegionCache->reset( true );
	}
	if ( m_pRuntimePageStore ) {
		m_pRuntimePageStore->reset();
	}
	if ( m_pActiveHeightResidency ) {
		m_pActiveHeightResidency->reset();
	}
}

void Terrain3DRegionResidency::appendRuntimeProfileSample( t3dRuntimeProfileSample_t &rSample ) const {

	const t3dRuntimePageStats_t runtimePageStats = m_pRuntimePageStore ? m_pRuntimePageStore->getStats() : t3dRuntimePageStats_t{};
	const t3dGpuRegionCacheStats_t gpu = m_pGpuRegionCache ? m_pGpuRegionCache->getStats() : t3dGpuRegionCacheStats_t{};
	const t3dCpuRegionCacheStats_t cpu = m_pCpuRegionCache ? m_pCpuRegionCache->getStats() : t3dCpuRegionCacheStats_t{};
	const t3dInstanceRegionCacheStats_t instance =
	        m_pInstanceRegionCache ? m_pInstanceRegionCache->getStats() : t3dInstanceRegionCacheStats_t{};
	const t3dFarSurfaceAtlasStats_t far = m_pFarSurfaceAtlasCache ? m_pFarSurfaceAtlasCache->getStats() : t3dFarSurfaceAtlasStats_t{};

	rSample.m_nStallFlags |= gpu.m_nStallFlagsLastTick | cpu.m_nStallFlagsLastTick | instance.m_nStallFlagsLastTick;
	rSample.m_nGpuResident = gpu.m_nResident;
	rSample.m_nGpuCapacity = gpu.m_nCapacity;
	rSample.m_nGpuUploads = gpu.m_nUploadsLastTick;
	rSample.m_nGpuEvictions = gpu.m_nEvictionsLastTick;
	rSample.m_nGpuMissingRequired = gpu.m_nMissingRequiredLastTick;
	rSample.m_nGpuUploadBytes = gpu.m_nUploadBytesLastTick;
	rSample.m_nGpuPreparedPages = gpu.m_nPreparedPagesLastTick;
	rSample.m_nGpuPrepareWorkers = gpu.m_nPrepareWorkersLastTick;
	rSample.m_nGpuEffectiveUploadLimit = gpu.m_nEffectiveUploadLimitLastTick;
	rSample.m_nGpuCameraMoved = gpu.m_bCameraMovedLastTick ? 1 : 0;
	rSample.m_nGpuRequestedPages = gpu.m_nRequestedPages;
	rSample.m_nGpuRequiredPages = gpu.m_nRequiredLastTick;
	rSample.m_nGpuGuardPages = gpu.m_nGuardLastTick;
	rSample.m_nGpuPrefetchPages = gpu.m_nPrefetchLastTick;
	rSample.m_nGpuHysteresisPages = gpu.m_nHysteresisLastTick;
	rSample.m_nGpuPreparedQueue = runtimePageStats.m_nPreparedPages;
	rSample.m_nGpuPreparePendingPages = runtimePageStats.m_nPendingPages;
	rSample.m_nGpuPrepareJobActive = runtimePageStats.m_bPrepareJobActive ? 1 : 0;
	rSample.m_nGpuCookedHits = gpu.m_nCookedHitsLastTick;
	rSample.m_nGpuCookedMisses = gpu.m_nCookedMissesLastTick;
	rSample.m_nGpuRuntimePrepares = gpu.m_nRuntimePreparesLastTick;
	rSample.m_nGpuFarDirtyRegions = m_pFarSurfaceAtlasCache ? m_pFarSurfaceAtlasCache->getDirtyRegionCount() : 0;
	rSample.m_nGpuFarRegionsProcessed = far.m_nHeightRegionsLastTick + far.m_nColorRegionsLastTick;
	rSample.m_nGpuPrepareLatencyUsec = runtimePageStats.m_nPrepareLatencyUsecLast;
	rSample.m_nGpuPrepareQueueWaitUsec = runtimePageStats.m_nPrepareQueueWaitUsecLast;
	rSample.m_nGpuPrepareFileOpenUsec = runtimePageStats.m_nPrepareFileOpenUsecLast;
	rSample.m_nGpuPrepareFileReadUsec = runtimePageStats.m_nPrepareFileReadUsecLast;
	rSample.m_nGpuPrepareValidationUsec = runtimePageStats.m_nPrepareValidationUsecLast;
	rSample.m_nGpuPrepareMaterializeUsec = runtimePageStats.m_nPrepareMaterializeUsecLast;
	rSample.m_nGpuPrepareWorkerUsec = runtimePageStats.m_nPrepareWorkerUsecLast;
	rSample.m_nGpuPrepareReadyWaitUsec = runtimePageStats.m_nPrepareReadyWaitUsecLast;
	rSample.m_nGpuPreparePublicationUsec = gpu.m_nPreparePublicationUsecLast;
	rSample.m_nGpuPrepareEndToEndUsec = gpu.m_nPrepareEndToEndUsecLast;
	rSample.m_nGpuCacheServiceUsec = gpu.m_nServiceUsecLast;
	rSample.m_nCpuResident = cpu.m_nResident;
	rSample.m_nCpuLoads = cpu.m_nLoadsLastTick;
	rSample.m_nCpuEvictions = cpu.m_nEvictionsLastTick;
	rSample.m_nCpuLoadBytes = cpu.m_nLoadBytesLastTick;
	rSample.m_nCpuCacheServiceUsec = cpu.m_nServiceUsecLast;
	rSample.m_nCpuDesired = cpu.m_nDesiredLastTick;
	rSample.m_nCpuSelectionPending = cpu.m_bSelectionPending ? 1 : 0;
	rSample.m_nCpuPinned = cpu.m_nPinnedLastTick;
	rSample.m_nInstanceResident = instance.m_nResident;
	rSample.m_nInstanceLoads = instance.m_nLoadsLastTick;
	rSample.m_nInstanceEvictions = instance.m_nEvictionsLastTick;
	rSample.m_nInstanceLoadBytes = instance.m_nLoadBytesLastTick;
	rSample.m_nInstanceCacheServiceUsec = instance.m_nServiceUsecLast;
	rSample.m_nInstanceDesired = instance.m_nDesiredLastTick;
	rSample.m_nInstanceSelectionPending = instance.m_bSelectionPending ? 1 : 0;
}

void Terrain3DRegionResidency::setBudgetOverrides( const int nGpuUploadLayers, const int nCpuLoadLayers, const int nInstanceLoadPages ) {
	m_nGpuUploadBudgetOverride = nGpuUploadLayers < 0 ? -1 : CLAMP( nGpuUploadLayers, 0, 64 );
	m_nCpuLoadBudgetOverride = nCpuLoadLayers < 0 ? -1 : CLAMP( nCpuLoadLayers, 0, 64 );
	m_nInstanceLoadBudgetOverride = nInstanceLoadPages < 0 ? -1 : CLAMP( nInstanceLoadPages, 0, 64 );
}

void Terrain3DRegionResidency::clearBudgetOverrides( void ) {
	m_nGpuUploadBudgetOverride = -1;
	m_nCpuLoadBudgetOverride = -1;
	m_nInstanceLoadBudgetOverride = -1;
}

int Terrain3DRegionResidency::effectiveGpuUploadLayers( const int nConfigured ) const {
	return m_nGpuUploadBudgetOverride < 0 ? nConfigured : MIN( nConfigured, m_nGpuUploadBudgetOverride );
}

int Terrain3DRegionResidency::effectiveCpuLoadLayers( const int nConfigured ) const {
	return m_nCpuLoadBudgetOverride < 0 ? nConfigured : MIN( nConfigured, m_nCpuLoadBudgetOverride );
}

int Terrain3DRegionResidency::effectiveInstanceLoadPages( const int nConfigured ) const {
	return m_nInstanceLoadBudgetOverride < 0 ? nConfigured : MIN( nConfigured, m_nInstanceLoadBudgetOverride );
}
