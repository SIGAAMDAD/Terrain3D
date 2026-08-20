// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "streaming/terrain_3d_region_residency.h"

#include "data/terrain_3d_data.h"
#include "streaming/terrain_3d_active_height_residency.h"

T3DRegionResidency::T3DRegionResidency(
	T3DRegionRepository *const pRepository,
	T3DSamplingStorage *const pSamplingStorage,
	T3DRuntimePageStore *const pRuntimePageStore,
	T3DActiveHeightResidency *const pActiveHeightResidency
) :
		m_pRepository( pRepository ),
		m_pSamplingStorage( pSamplingStorage ),
		m_pRuntimePageStore( pRuntimePageStore ),
		m_pActiveHeightResidency( pActiveHeightResidency ) {
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
	if ( !m_pRepository || !m_pSamplingStorage || !m_pRuntimePageStore || !m_pActiveHeightResidency ) {
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

	// Middle height is a runtime-only geometry cache. Authoring reads authoritative
	// full-resolution maps and intentionally does not duplicate this storage.
	if ( m_pRepository->getRegionMapSize() > 0 && !rFrame.bAuthoring && rFrame.nMiddleLods > 0 ) {
		rData.updateMiddleHeightResidencyInternal( rFrame, bForce );
	} else if ( rData.m_bMiddleHeightTexturesReady ) {
		rData.resetMiddleHeightCache();
	}

	rData.updateGpuRegionResidencyInternal( rFrame, bForce );

	// Requests emitted by Active/Middle/GPU/instances form the next union batch.
	rData.serviceGpuRuntimePagePrepare( rFrame );
}

void T3DRegionResidency::resetRuntimeCaches( T3DData &rData ) {
	rData.resetCpuRegionCache( true );
	rData.resetInstanceRegionCache( false );
	rData.resetMiddleHeightCache();
	rData.resetGpuRegionCache( true );
	if ( m_pActiveHeightResidency ) {
		m_pActiveHeightResidency->reset();
	}
	rData.configureCpuRegionCache(
		rData.m_bCpuRegionCacheEnabled, rData.m_nCpuRegionCacheBudgetMb,
		rData.m_nCpuRegionLoadBudgetLayers, rData.m_nCpuRegionLoadBudgetMb );
	rData.configureInstanceRegionCache(
		rData.m_bInstanceRegionCacheEnabled, rData.m_nInstanceRegionCacheBudgetMb,
		rData.m_nInstanceRegionLoadBudgetPages, rData.m_nInstanceRegionLoadBudgetMb );
	rData.configureGpuRegionCache(
		rData.m_bGpuRegionCacheEnabled, rData.m_nGpuRegionCacheBudgetMb,
		rData.m_nGpuRegionUploadBudgetLayers, rData.m_nGpuRegionUploadBudgetMb,
		rData.m_nGpuRegionSparePages, rData.m_nGpuRegionRetireFrames );
}

void T3DRegionResidency::appendRuntimeProfileSample(
	const T3DData &rData,
	t3druntimeProfileSample_t &rSample
) const {
	const T3DRuntimePageStats runtimePageStats = m_pRuntimePageStore ?
		m_pRuntimePageStore->getStats() : T3DRuntimePageStats{};
	rSample.stallFlags |= rData.m_nGpuRegionStallFlagsLastTick | rData.m_nCpuRegionStallFlagsLastTick |
		rData.m_nInstanceRegionStallFlagsLastTick;
	rSample.gpuResident = rData.m_nGpuRegionResidentCount;
	rSample.nGpuCapacity = rData.m_nGpuRegionCacheCapacity;
	rSample.nGpuUploads = rData.m_nGpuRegionUploadsLastTick;
	rSample.nGpuEvictions = rData.m_nGpuRegionEvictionsLastTick;
	rSample.gpuMissingRequired = rData.m_nGpuRegionMissingRequiredLastTick;
	rSample.nGpuUploadBytes = rData.m_nGpuRegionUploadBytesLastTick;
	rSample.nGpuPreparedPages = rData.m_nGpuRegionPreparedPagesLastTick;
	rSample.nGpuPrepareWorkers = rData.m_nGpuRegionPrepareWorkersLastTick;
	rSample.gpuEffectiveUploadLimit = rData.m_nGpuRegionEffectiveUploadLimitLastTick;
	rSample.gpuCameraMoved = rData.m_bGpuRegionCameraMovedLastTick ? 1 : 0;
	rSample.nGpuRequestedPages = rData.m_nGpuRegionRequestedPages;
	rSample.nGpuRequiredPages = rData.m_nGpuRegionRequiredLastTick;
	rSample.nGpuGuardPages = rData.m_nGpuRegionGuardLastTick;
	rSample.nGpuPrefetchPages = rData.m_nGpuRegionPrefetchLastTick;
	rSample.nGpuHysteresisPages = rData.m_nGpuRegionHysteresisLastTick;
	rSample.gpuPreparedQueue = runtimePageStats.nPreparedPages;
	rSample.nGpuPreparePendingPages = runtimePageStats.nPendingPages;
	rSample.gpuPrepareJobActive = runtimePageStats.bPrepareJobActive ? 1 : 0;
	rSample.nGpuCookedHits = rData.m_nGpuRegionCookedHitsLastTick;
	rSample.nGpuCookedMisses = rData.m_nGpuRegionCookedMissesLastTick;
	rSample.gpuRuntimePrepares = rData.m_nGpuRegionRuntimePreparesLastTick;
	rSample.nGpuFarDirtyRegions = ( rData.m_bGpuFarHeightDirtyAll || rData.m_bGpuFarColorDirtyAll ) ?
		( m_pRepository ? m_pRepository->getActiveRegionCount() : 0 ) :
		( int ) ( ( rData.m_gpuFarHeightDirtyRegions | rData.m_gpuFarColorDirtyRegions ).count() );
	rSample.gpuFarRegionsProcessed = rData.m_nGpuFarHeightRegionsLastTick + rData.m_nGpuFarColorRegionsLastTick;
	rSample.nGpuPrepareLatencyUsec = runtimePageStats.nPrepareLatencyUsecLast;
	rSample.nGpuPrepareQueueWaitUsec = runtimePageStats.nPrepareQueueWaitUsecLast;
	rSample.nGpuPrepareFileOpenUsec = runtimePageStats.nPrepareFileOpenUsecLast;
	rSample.nGpuPrepareFileReadUsec = runtimePageStats.nPrepareFileReadUsecLast;
	rSample.nGpuPrepareValidationUsec = runtimePageStats.nPrepareValidationUsecLast;
	rSample.nGpuPrepareMaterializeUsec = runtimePageStats.nPrepareMaterializeUsecLast;
	rSample.nGpuPrepareWorkerUsec = runtimePageStats.nPrepareWorkerUsecLast;
	rSample.nGpuPrepareReadyWaitUsec = runtimePageStats.nPrepareReadyWaitUsecLast;
	rSample.nGpuPreparePublicationUsec = rData.m_nGpuPreparePublicationUsecLast;
	rSample.nGpuPrepareEndToEndUsec = rData.m_nGpuPrepareEndToEndUsecLast;
	rSample.nGpuCacheServiceUsec = rData.m_nGpuRegionServiceUsecLast;
	rSample.cpuResident = rData.m_nCpuRegionResidentCount;
	rSample.cpuLoads = rData.m_nCpuRegionLoadsLastTick;
	rSample.nCpuEvictions = rData.m_nCpuRegionEvictionsLastTick;
	rSample.nCpuLoadBytes = rData.m_nCpuRegionLoadBytesLastTick;
	rSample.nCpuCacheServiceUsec = rData.m_nCpuRegionServiceUsecLast;
	rSample.cpuDesired = rData.m_nCpuRegionDesiredLastTick;
	rSample.cpuSelectionPending = rData.m_bCpuRegionCacheSelectionDirty ? 1 : 0;
	rSample.cpuPinned = rData.m_nCpuRegionPinnedLastTick;
	rSample.instanceResident = rData.m_nInstanceRegionResidentCount;
	rSample.instanceLoads = rData.m_nInstanceRegionLoadsLastTick;
	rSample.nInstanceEvictions = rData.m_nInstanceRegionEvictionsLastTick;
	rSample.nInstanceLoadBytes = rData.m_nInstanceRegionLoadBytesLastTick;
	rSample.nInstanceCacheServiceUsec = rData.m_nInstanceRegionServiceUsecLast;
	rSample.instanceDesired = rData.m_nInstanceRegionDesiredLastTick;
	rSample.instanceSelectionPending = rData.m_bInstanceRegionCacheSelectionDirty ? 1 : 0;
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
