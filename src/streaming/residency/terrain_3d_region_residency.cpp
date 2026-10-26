// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "data/model/terrain_3d_region.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include <godot_cpp/variant/utility_functions.hpp>

#include "Terrain3DTypeSupport.h"
#include "streaming/residency/terrain_3d_region_residency.h"

#include "streaming/residency/terrain_3d_active_height_residency.h"
#include "streaming/cache/terrain_3d_cpu_region_cache.h"
#include "streaming/cache/terrain_3d_gpu_region_cache.h"
#include "streaming/cache/terrain_3d_instance_region_cache.h"
#include "streaming/cache/terrain_3d_middle_height_region_cache.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"
#include "streaming/hierarchy/terrain_3d_hierarchy_scheduler_io.h"

Terrain3DRegionResidency::Terrain3DRegionResidency(
	Terrain3DRegionRepository *const		pRepository,
	Terrain3DSamplingStorage *const			pSamplingStorage,
	Terrain3DRuntimePageStore *const		pRuntimePageStore,
	Terrain3DActiveHeightResidency *const	pActiveHeightResidency,
	Terrain3DCpuRegionCache *const			pCpuRegionCache,
	Terrain3DInstanceRegionCache *const		pInstanceRegionCache,
	Terrain3DMiddleHeightRegionCache *const pMiddleHeightRegionCache,
	Terrain3DGpuRegionCache *const			pGpuRegionCache ) :
	m_pRepository( pRepository ),
	m_pSamplingStorage( pSamplingStorage ),
	m_pRuntimePageStore( pRuntimePageStore ),
	m_pActiveHeightResidency( pActiveHeightResidency ),
	m_pCpuRegionCache( pCpuRegionCache ),
	m_pInstanceRegionCache( pInstanceRegionCache ),
	m_pMiddleHeightRegionCache( pMiddleHeightRegionCache ),
	m_pGpuRegionCache( pGpuRegionCache ) {
}

void Terrain3DRegionResidency::reset(
	void ) {
	m_gpuPagePlan.reset( m_gpuPagePlan.epoch() + 1u );
	if ( m_pActiveHeightResidency ) {
		m_pActiveHeightResidency->reset();
	}
}

void Terrain3DRegionResidency::update(
	const t3dResidencyFrameState_s			&rFrame,
	const bool								 bForce,
	const t3dRegionResidencyUpdateContext_t &rContext ) {
	if ( !m_pRepository || !m_pSamplingStorage || !m_pRuntimePageStore || !m_pActiveHeightResidency ||
		 !m_pCpuRegionCache || !m_pInstanceRegionCache || !m_pMiddleHeightRegionCache || !m_pGpuRegionCache ) {
		return;
	}

	auto service_runtime_pages = [this, &rFrame]() {
		t3dRuntimePageServiceState_t state;
		state.m_config = rFrame.m_runtimePageConfig;
		state.m_predictedCenter = rFrame.m_predictedCenter;
		state.m_nStreamingGeneration = rFrame.m_nStreamingGeneration;
		state.m_isWanted = &t3dResidencyFrameWantsRuntimePage;
		state.m_pWantedContext = const_cast< t3dResidencyFrameState_s * >( &rFrame );
		m_pRuntimePageStore->service( state );
	};

	// Complete the previous union batch before consumers inspect prepared pages.
	service_runtime_pages();
	m_pCpuRegionCache->update( rFrame, bForce, effectiveCpuLoadLayers( m_pCpuRegionCache->getLoadBudgetLayers() ) );

	const t3dActiveHeightResidencyResult_t activeResult = m_pActiveHeightResidency->update( rFrame, bForce );
	for ( int index = 0; index < activeResult.m_nCpuSourceRequestCount; ++index ) {
		m_pCpuRegionCache->requestSourceAsync(
			t3dUnpackRegionCoord( activeResult.m_szCpuSourceRequests[( size_t ) ( index )] ) );
	}

    // Exact height is admitted by the pre-existing residency owner first.
    // The optional hierarchy scheduler sees that reservation and only issues
    // spare-budget visual tile requests. F04 will install actual consumers.
    if ( rContext.m_bHierarchyDemandEnabled && !rFrame.m_bAuthoring &&
         rContext.m_pHierarchyScheduler && rContext.m_pHierarchyTileStore ) {
        t3dHierarchyView_t view = rContext.m_hierarchyView;
        view.m_fCameraX = rFrame.m_worldPosition.x;
        view.m_fCameraY = rFrame.m_worldPosition.y;
        view.m_fCameraZ = rFrame.m_worldPosition.z;
        rContext.m_pHierarchyScheduler->schedule( view, rContext.m_hierarchyLimits,
                                                   ( uint32_t ) activeResult.m_nCpuSourceRequestCount );
        t3dHierarchyDispatchToTileStore( *rContext.m_pHierarchyScheduler,
                                         *rContext.m_pHierarchyTileStore,
                                         rContext.m_hierarchyLimits );
    }

	t3dInstanceRegionCacheChanges_s instance_changes;
	m_pInstanceRegionCache->update( rFrame,
									bForce,
									effectiveInstanceLoadPages( m_pInstanceRegionCache->getLoadBudgetPages() ),
									instance_changes );
	if ( rContext.m_applyInstanceChanges ) {
		rContext.m_applyInstanceChanges( rContext.m_pOwnerContext, instance_changes );
	}

	if ( m_pRepository->getRegionMapSize() > 0 && !rFrame.m_bAuthoring && rFrame.m_nMiddleLods > 0 ) {
		m_pMiddleHeightRegionCache->update( rFrame,
											bForce,
											effectiveGpuUploadLayers( m_pGpuRegionCache->getUploadBudgetLayers() ),
											rContext.m_middleHeightConfig );
	} else if ( m_pMiddleHeightRegionCache->isReady() ) {
		m_pMiddleHeightRegionCache->reset();
	}

	const t3dGpuRegionCacheUpdateResult_t gpuResult =
		m_pGpuRegionCache->update( rFrame,
								   bForce,
								   effectiveGpuUploadLayers( m_pGpuRegionCache->getUploadBudgetLayers() ),
								   rContext.m_pCompute );
	if ( gpuResult.m_bPageTableChanged && rContext.m_gpuPageTableChanged ) {
		rContext.m_gpuPageTableChanged( rContext.m_pOwnerContext );
	}

	// Requests emitted by Active/Middle/GPU/instances form the next union batch.
	service_runtime_pages();
}

void Terrain3DRegionResidency::resetRuntimeCaches(
	const t3dRegionResidencyUpdateContext_t &rContext ) {
	m_gpuPagePlan.reset( m_gpuPagePlan.epoch() + 1u );
	if ( m_pCpuRegionCache ) {
		m_pCpuRegionCache->reset( true );
		m_pCpuRegionCache->refreshCapacity();
	}
	if ( m_pInstanceRegionCache ) {
		t3dInstanceRegionCacheChanges_s instance_changes;
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

void Terrain3DRegionResidency::appendRuntimeProfileSample(
	t3dRuntimeProfileSample_t &rSample ) const {
	const t3dRuntimePageStats_t runtimePageStats =
		m_pRuntimePageStore ? m_pRuntimePageStore->getStats() : t3dRuntimePageStats_t{};
	const t3dGpuRegionCacheStats_t gpu = m_pGpuRegionCache ? m_pGpuRegionCache->getStats() : t3dGpuRegionCacheStats_t{};
	const t3dCpuRegionCacheStats_t cpu = m_pCpuRegionCache ? m_pCpuRegionCache->getStats() : t3dCpuRegionCacheStats_t{};
	const t3dInstanceRegionCacheStats_t instance =
		m_pInstanceRegionCache ? m_pInstanceRegionCache->getStats() : t3dInstanceRegionCacheStats_t{};

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
	rSample.m_nGpuFarDirtyRegions = 0;
	rSample.m_nGpuFarRegionsProcessed = 0;
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

void Terrain3DRegionResidency::setBudgetOverrides(
	const int nGpuUploadLayers,
	const int nCpuLoadLayers,
	const int nInstanceLoadPages ) {
	m_nGpuUploadBudgetOverride = nGpuUploadLayers < 0 ? -1 : CLAMP( nGpuUploadLayers, 0, 64 );
	m_nCpuLoadBudgetOverride = nCpuLoadLayers < 0 ? -1 : CLAMP( nCpuLoadLayers, 0, 64 );
	m_nInstanceLoadBudgetOverride = nInstanceLoadPages < 0 ? -1 : CLAMP( nInstanceLoadPages, 0, 64 );
}

void Terrain3DRegionResidency::clearBudgetOverrides(
	void ) {
	m_nGpuUploadBudgetOverride = -1;
	m_nCpuLoadBudgetOverride = -1;
	m_nInstanceLoadBudgetOverride = -1;
}

int Terrain3DRegionResidency::effectiveGpuUploadLayers(
	const int nConfigured ) const {
	return m_nGpuUploadBudgetOverride < 0 ? nConfigured : MIN( nConfigured, m_nGpuUploadBudgetOverride );
}

int Terrain3DRegionResidency::effectiveCpuLoadLayers(
	const int nConfigured ) const {
	return m_nCpuLoadBudgetOverride < 0 ? nConfigured : MIN( nConfigured, m_nCpuLoadBudgetOverride );
}

int Terrain3DRegionResidency::effectiveInstanceLoadPages(
	const int nConfigured ) const {
	return m_nInstanceLoadBudgetOverride < 0 ? nConfigured : MIN( nConfigured, m_nInstanceLoadBudgetOverride );
}

// -------- Consolidated from streaming/residency/terrain_3d_active_height_residency.cpp --------
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
Terrain3DActiveHeightResidency::Terrain3DActiveHeightResidency(
	Terrain3DRegionRepository *const pRepository,
	Terrain3DSamplingStorage *const	 pSamplingStorage,
	Terrain3DRuntimePageStore *const pRuntimePageStore ) :
	m_pRepository( pRepository ),
	m_pSamplingStorage( pSamplingStorage ),
	m_pRuntimePageStore( pRuntimePageStore ) {
}

void Terrain3DActiveHeightResidency::reset(
	void ) {
	m_bDirty = true;
	m_nStreamingGenerationLast = 0u;
	m_nTopologyGenerationLast = 0u;
	m_center = s_v2iMax;
	m_stats = t3dActiveHeightResidencyStats_t{};
}

t3dActiveHeightResidencyResult_t Terrain3DActiveHeightResidency::update(
	const t3dResidencyFrameState_s &rFrame,
	const bool						bForce ) {
	t3dActiveHeightResidencyResult_t result;
	m_stats = t3dActiveHeightResidencyStats_t{};
	if ( !m_pRepository || !m_pSamplingStorage || !m_pRuntimePageStore || !m_pSamplingStorage->isActiveOwner() ) {
		return result;
	}
	const uint64_t topologyGeneration = m_pRepository->getTopologyGeneration();
	if ( !bForce && !m_bDirty && rFrame.m_nStreamingGeneration == m_nStreamingGenerationLast &&
		 topologyGeneration == m_nTopologyGenerationLast && rFrame.m_activeHeightCenter == m_center ) {
		return result;
	}

	m_stats.m_nReconciliationsLastTick = 1;
	const bool cookedRuntime = !rFrame.m_bAuthoring && rFrame.m_bCookedPagesEnabled;
	bool	   allRequiredResident = true;
	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		++m_stats.m_nSlotsExaminedLastTick;
		const Vector2i		   location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
		Terrain3DRegion *const pRegion = location == s_v2iMax ? NULL : m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}

		const Vector2i delta = location - rFrame.m_activeHeightCenter;
		const int	   deltaX = delta.x < 0 ? -delta.x : delta.x;
		const int	   deltaY = delta.y < 0 ? -delta.y : delta.y;
		const bool	   insideActiveBank = MAX( deltaX, deltaY ) <= T3D_ACTIVE_RADIUS;
		const bool	   required = insideActiveBank && ( rFrame.m_bAuthoring || rFrame.streamingState( location ) >=
																			   TERRAIN3D_REGION_STREAMING_HOT );
		const bool	   predicted = !rFrame.m_bAuthoring && rFrame.isPrefetch( location );
		const bool	   resident = m_pSamplingStorage->containsActive( location );

		if ( !required ) {
			if ( resident ) {
				m_pSamplingStorage->invalidateActive( location );
				++m_stats.m_nEvictedLastTick;
			} else if ( cookedRuntime && predicted ) {
				m_pRuntimePageStore->request( location, RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT, 3u, true );
				++m_stats.m_nPredictedRequestsLastTick;
			}
			continue;
		}

		++m_stats.m_nRequiredLastTick;
		if ( resident ) {
			continue;
		}

		bool published = false;
		if ( cookedRuntime ) {
			m_pRuntimePageStore->request( location, RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT, 1u, true );
			Ref< Image >	preparedHeight;
			PackedByteArray preparedHeightBytes;
			if ( m_pRuntimePageStore->takeHeight( location,
												  RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT,
												  preparedHeight,
												  false,
												  &preparedHeightBytes ) &&
				 preparedHeight.is_valid() ) {
				published = m_pSamplingStorage->syncActiveHeightPage( location,
																	  preparedHeightBytes.ptr(),
																	  ( size_t ) ( preparedHeightBytes.size() ) );
			}
		}

		if ( !published && pRegion->getHeightMap().is_valid() ) {
			published = m_pSamplingStorage->syncActiveRegion( location, pRegion );
		}
		if ( published ) {
			++m_stats.m_nPublishedLastTick;
			continue;
		}

		allRequiredResident = false;
		++m_stats.m_nMissingRequiredLastTick;
		if ( rFrame.m_bCpuRegionCacheEnabled && !rFrame.m_bAuthoring ) {
			result.requestCpuSource( location );
			continue;
		}
		if ( rFrame.m_bAuthoring && !m_pSamplingStorage->syncActiveRegion( location, pRegion ) ) {
			UtilityFunctions::push_error(
				vformat( "Terrain3D: Failed to publish required authoring height payload for region %s.", location ) );
		}
	}

	m_nStreamingGenerationLast = rFrame.m_nStreamingGeneration;
	m_nTopologyGenerationLast = topologyGeneration;
	m_center = rFrame.m_activeHeightCenter;
	m_bDirty = !allRequiredResident;
	return result;
}
