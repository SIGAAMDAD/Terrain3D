// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "streaming/terrain_3d_region_residency.h"

#include "data/terrain_3d_data.h"
#include "rendering/terrain_3d_far_surface_atlas_cache.h"
#include "streaming/terrain_3d_active_height_residency.h"
#include "streaming/terrain_3d_cpu_region_cache.h"
#include "streaming/terrain_3d_gpu_region_cache.h"
#include "streaming/terrain_3d_instance_region_cache.h"
#include "streaming/terrain_3d_middle_height_region_cache.h"
#include "streaming/terrain_3d_runtime_page_store.h"

T3DRegionResidency::T3DRegionResidency(
	T3DRegionRepository *const pRepository,
	T3DSamplingStorage *const pSamplingStorage,
	T3DRuntimePageStore *const pRuntimePageStore,
	T3DActiveHeightResidency *const pActiveHeightResidency,
	T3DCpuRegionCache *const pCpuRegionCache,
	T3DInstanceRegionCache *const pInstanceRegionCache,
	T3DMiddleHeightRegionCache *const pMiddleHeightRegionCache,
	T3DGpuRegionCache *const pGpuRegionCache,
	T3DFarSurfaceAtlasCache *const pFarSurfaceAtlasCache
) :
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

void T3DRegionResidency::reset( void ) {
	if ( m_pActiveHeightResidency ) {
		m_pActiveHeightResidency->reset();
	}
}

void T3DRegionResidency::update(
	T3DData &rData,
	const t3dResidencyFrameState_t &rFrame,
	const bool bForce
) {
	if ( !m_pRepository || !m_pSamplingStorage || !m_pRuntimePageStore || !m_pActiveHeightResidency ||
		!m_pCpuRegionCache || !m_pInstanceRegionCache || !m_pMiddleHeightRegionCache || !m_pGpuRegionCache ) {
		return;
	}

	// Complete the previous union batch before consumers inspect prepared pages.
	rData.serviceGpuRuntimePagePrepare( rFrame );
	rData.updateCpuRegionResidencyInternal( rFrame, bForce );

	const t3dActiveHeightResidencyResult_t activeResult = m_pActiveHeightResidency->update( rFrame, bForce );
	for ( int index = 0; index < activeResult.m_nCpuSourceRequestCount; ++index ) {
		rData.requestCpuRegionSourceAsync( activeResult.cpuSourceRequests[( size_t ) ( index )] );
	}

	rData.updateInstanceRegionResidencyInternal( rFrame, bForce );

	if ( m_pRepository->getRegionMapSize() > 0 && !rFrame.m_bAuthoring && rFrame.m_nMiddleLods > 0 ) {
		rData.updateMiddleHeightResidencyInternal( rFrame, bForce );
	} else if ( m_pMiddleHeightRegionCache->isReady() ) {
		rData.resetMiddleHeightCache();
	}

	rData.updateGpuRegionResidencyInternal( rFrame, bForce );

	// Requests emitted by Active/Middle/GPU/instances form the next union batch.
	rData.serviceGpuRuntimePagePrepare( rFrame );
}

void T3DRegionResidency::resetRuntimeCaches( T3DData &rData ) {
	rData.resetCpuRegionCache( true );
	if ( m_pCpuRegionCache ) {
		m_pCpuRegionCache->refreshCapacity();
	}
	rData.resetInstanceRegionCache( false );
	rData.resetMiddleHeightCache();
	rData.resetGpuRegionCache( true );
	if ( m_pRuntimePageStore ) {
		m_pRuntimePageStore->reset();
	}
	if ( m_pActiveHeightResidency ) {
		m_pActiveHeightResidency->reset();
	}
}

void T3DRegionResidency::appendRuntimeProfileSample(
	const T3DData &,
	t3dRuntimeProfileSample_t &rSample
) const {
	const t3dRuntimePageStats_t runtimePageStats = m_pRuntimePageStore ?
		m_pRuntimePageStore->getStats() : t3dRuntimePageStats_t{};
	const t3dGpuRegionCacheStats_t gpu = m_pGpuRegionCache ? m_pGpuRegionCache->getStats() : t3dGpuRegionCacheStats_t{};
	const t3dCpuRegionCacheStats_t cpu = m_pCpuRegionCache ? m_pCpuRegionCache->getStats() : t3dCpuRegionCacheStats_t{};
	const t3dInstanceRegionCacheStats_t instance = m_pInstanceRegionCache ?
		m_pInstanceRegionCache->getStats() : t3dInstanceRegionCacheStats_t{};
	const t3dFarSurfaceAtlasStats_t far = m_pFarSurfaceAtlasCache ?
		m_pFarSurfaceAtlasCache->getStats() : t3dFarSurfaceAtlasStats_t{};

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

void T3DRegionResidency::setBudgetOverrides(
	const int nGpuUploadLayers,
	const int nCpuLoadLayers,
	const int nInstanceLoadPages
) {
	m_nGpuUploadBudgetOverride = nGpuUploadLayers < 0 ? -1 : CLAMP( nGpuUploadLayers, 0, 64 );
	m_nCpuLoadBudgetOverride = nCpuLoadLayers < 0 ? -1 : CLAMP( nCpuLoadLayers, 0, 64 );
	m_nInstanceLoadBudgetOverride = nInstanceLoadPages < 0 ? -1 : CLAMP( nInstanceLoadPages, 0, 64 );
}

void T3DRegionResidency::clearBudgetOverrides( void ) {
	m_nGpuUploadBudgetOverride = -1;
	m_nCpuLoadBudgetOverride = -1;
	m_nInstanceLoadBudgetOverride = -1;
}

int T3DRegionResidency::effectiveGpuUploadLayers( const int nConfigured ) const {
	return m_nGpuUploadBudgetOverride < 0 ? nConfigured : MIN( nConfigured, m_nGpuUploadBudgetOverride );
}

int T3DRegionResidency::effectiveCpuLoadLayers( const int nConfigured ) const {
	return m_nCpuLoadBudgetOverride < 0 ? nConfigured : MIN( nConfigured, m_nCpuLoadBudgetOverride );
}

int T3DRegionResidency::effectiveInstanceLoadPages( const int nConfigured ) const {
	return m_nInstanceLoadBudgetOverride < 0 ? nConfigured : MIN( nConfigured, m_nInstanceLoadBudgetOverride );
}
