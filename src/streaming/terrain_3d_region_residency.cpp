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
	const T3DResidencyFrameState &rFrame,
	const bool bForce
) {
	if ( !m_pRepository || !m_pSamplingStorage || !m_pRuntimePageStore || !m_pActiveHeightResidency ||
		!m_pCpuRegionCache || !m_pInstanceRegionCache || !m_pMiddleHeightRegionCache || !m_pGpuRegionCache ) {
		return;
	}

	// Complete the previous union batch before consumers inspect prepared pages.
	rData.serviceGpuRuntimePagePrepare( rFrame );
	rData.updateCpuRegionResidencyInternal( rFrame, bForce );

	const T3DActiveHeightResidencyResult activeResult = m_pActiveHeightResidency->update( rFrame, bForce );
	for ( int index = 0; index < activeResult.nCpuSourceRequestCount; ++index ) {
		rData.requestCpuRegionSourceAsync( activeResult.cpuSourceRequests[( size_t ) ( index )] );
	}

	rData.updateInstanceRegionResidencyInternal( rFrame, bForce );

	if ( m_pRepository->getRegionMapSize() > 0 && !rFrame.bAuthoring && rFrame.nMiddleLods > 0 ) {
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
	const T3DRuntimePageStats runtimePageStats = m_pRuntimePageStore ?
		m_pRuntimePageStore->getStats() : T3DRuntimePageStats{};
	const T3DGpuRegionCacheStats gpu = m_pGpuRegionCache ? m_pGpuRegionCache->getStats() : T3DGpuRegionCacheStats{};
	const T3DCpuRegionCacheStats cpu = m_pCpuRegionCache ? m_pCpuRegionCache->getStats() : T3DCpuRegionCacheStats{};
	const T3DInstanceRegionCacheStats instance = m_pInstanceRegionCache ?
		m_pInstanceRegionCache->getStats() : T3DInstanceRegionCacheStats{};
	const T3DFarSurfaceAtlasStats far = m_pFarSurfaceAtlasCache ?
		m_pFarSurfaceAtlasCache->getStats() : T3DFarSurfaceAtlasStats{};

	rSample.stallFlags |= gpu.nStallFlagsLastTick | cpu.nStallFlagsLastTick | instance.nStallFlagsLastTick;
	rSample.gpuResident = gpu.nResident;
	rSample.nGpuCapacity = gpu.nCapacity;
	rSample.nGpuUploads = gpu.nUploadsLastTick;
	rSample.nGpuEvictions = gpu.nEvictionsLastTick;
	rSample.gpuMissingRequired = gpu.nMissingRequiredLastTick;
	rSample.nGpuUploadBytes = gpu.nUploadBytesLastTick;
	rSample.nGpuPreparedPages = gpu.nPreparedPagesLastTick;
	rSample.nGpuPrepareWorkers = gpu.nPrepareWorkersLastTick;
	rSample.gpuEffectiveUploadLimit = gpu.nEffectiveUploadLimitLastTick;
	rSample.gpuCameraMoved = gpu.bCameraMovedLastTick ? 1 : 0;
	rSample.nGpuRequestedPages = gpu.nRequestedPages;
	rSample.nGpuRequiredPages = gpu.nRequiredLastTick;
	rSample.nGpuGuardPages = gpu.nGuardLastTick;
	rSample.nGpuPrefetchPages = gpu.nPrefetchLastTick;
	rSample.nGpuHysteresisPages = gpu.nHysteresisLastTick;
	rSample.gpuPreparedQueue = runtimePageStats.nPreparedPages;
	rSample.nGpuPreparePendingPages = runtimePageStats.nPendingPages;
	rSample.gpuPrepareJobActive = runtimePageStats.bPrepareJobActive ? 1 : 0;
	rSample.nGpuCookedHits = gpu.nCookedHitsLastTick;
	rSample.nGpuCookedMisses = gpu.nCookedMissesLastTick;
	rSample.gpuRuntimePrepares = gpu.nRuntimePreparesLastTick;
	rSample.nGpuFarDirtyRegions = m_pFarSurfaceAtlasCache ? m_pFarSurfaceAtlasCache->getDirtyRegionCount() : 0;
	rSample.gpuFarRegionsProcessed = far.nHeightRegionsLastTick + far.nColorRegionsLastTick;
	rSample.nGpuPrepareLatencyUsec = runtimePageStats.nPrepareLatencyUsecLast;
	rSample.nGpuPrepareQueueWaitUsec = runtimePageStats.nPrepareQueueWaitUsecLast;
	rSample.nGpuPrepareFileOpenUsec = runtimePageStats.nPrepareFileOpenUsecLast;
	rSample.nGpuPrepareFileReadUsec = runtimePageStats.nPrepareFileReadUsecLast;
	rSample.nGpuPrepareValidationUsec = runtimePageStats.nPrepareValidationUsecLast;
	rSample.nGpuPrepareMaterializeUsec = runtimePageStats.nPrepareMaterializeUsecLast;
	rSample.nGpuPrepareWorkerUsec = runtimePageStats.nPrepareWorkerUsecLast;
	rSample.nGpuPrepareReadyWaitUsec = runtimePageStats.nPrepareReadyWaitUsecLast;
	rSample.nGpuPreparePublicationUsec = gpu.nPreparePublicationUsecLast;
	rSample.nGpuPrepareEndToEndUsec = gpu.nPrepareEndToEndUsecLast;
	rSample.nGpuCacheServiceUsec = gpu.nServiceUsecLast;
	rSample.cpuResident = cpu.nResident;
	rSample.cpuLoads = cpu.nLoadsLastTick;
	rSample.nCpuEvictions = cpu.nEvictionsLastTick;
	rSample.nCpuLoadBytes = cpu.nLoadBytesLastTick;
	rSample.nCpuCacheServiceUsec = cpu.nServiceUsecLast;
	rSample.cpuDesired = cpu.nDesiredLastTick;
	rSample.cpuSelectionPending = cpu.bSelectionPending ? 1 : 0;
	rSample.cpuPinned = cpu.nPinnedLastTick;
	rSample.instanceResident = instance.nResident;
	rSample.instanceLoads = instance.nLoadsLastTick;
	rSample.nInstanceEvictions = instance.nEvictionsLastTick;
	rSample.nInstanceLoadBytes = instance.nLoadBytesLastTick;
	rSample.nInstanceCacheServiceUsec = instance.nServiceUsecLast;
	rSample.instanceDesired = instance.nDesiredLastTick;
	rSample.instanceSelectionPending = instance.bSelectionPending ? 1 : 0;
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
