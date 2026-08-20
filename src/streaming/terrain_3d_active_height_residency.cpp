// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "streaming/terrain_3d_active_height_residency.h"

#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_sampling_storage.h"
#include "streaming/terrain_3d_runtime_page_store.h"

#include <godot_cpp/variant/utility_functions.hpp>

T3DActiveHeightResidency::T3DActiveHeightResidency(
	T3DRegionRepository *const pRepository,
	T3DSamplingStorage *const pSamplingStorage,
	T3DRuntimePageStore *const pRuntimePageStore
) :
		m_pRepository( pRepository ),
		m_pSamplingStorage( pSamplingStorage ),
		m_pRuntimePageStore( pRuntimePageStore ) {
}

void T3DActiveHeightResidency::reset( void ) {
	m_bDirty = true;
	m_nStreamingGenerationLast = 0u;
	m_nTopologyGenerationLast = 0u;
	m_center = V2I_MAX;
	m_stats = T3DActiveHeightResidencyStats{};
}

T3DActiveHeightResidencyResult T3DActiveHeightResidency::update(
	const T3DResidencyFrameState &rFrame,
	const bool bForce
) {
	T3DActiveHeightResidencyResult result;
	m_stats = T3DActiveHeightResidencyStats{};
	if ( !m_pRepository || !m_pSamplingStorage || !m_pRuntimePageStore ||
		!m_pSamplingStorage->isActiveOwner() ) {
		return result;
	}
	const uint64_t topologyGeneration = m_pRepository->getTopologyGeneration();
	if ( !bForce && !m_bDirty && rFrame.streamingGeneration == m_nStreamingGenerationLast &&
		topologyGeneration == m_nTopologyGenerationLast && rFrame.activeHeightCenter == m_center ) {
		return result;
	}

	m_stats.nReconciliationsLastTick = 1;
	const bool cookedRuntime = !rFrame.bAuthoring && rFrame.bCookedPagesEnabled;
	bool allRequiredResident = true;
	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		++m_stats.nSlotsExaminedLastTick;
		const Vector2i location = m_pRepository->getSlotsData()[slot];
		T3DRegion *const pRegion = location == V2I_MAX ? nullptr : m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}

		const Vector2i delta = location - rFrame.activeHeightCenter;
		const bool insideActiveBank = MAX( ABS( delta.x ), ABS( delta.y ) ) <= ACTIVE_RADIUS;
		const bool required = insideActiveBank && ( rFrame.bAuthoring ||
			rFrame.streamingState( location ) >= TERRAIN3D_REGION_STREAMING_HOT );
		const bool predicted = !rFrame.bAuthoring && rFrame.isPrefetch( location );
		const bool resident = m_pSamplingStorage->containsActive( location );

		if ( !required ) {
			if ( resident ) {
				m_pSamplingStorage->invalidateActive( location );
				++m_stats.nEvictedLastTick;
			} else if ( cookedRuntime && predicted ) {
				m_pRuntimePageStore->request(
					location, RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT, 3u, true );
				++m_stats.nPredictedRequestsLastTick;
			}
			continue;
		}

		++m_stats.nRequiredLastTick;
		if ( resident ) {
			continue;
		}

		bool published = false;
		if ( cookedRuntime ) {
			m_pRuntimePageStore->request(
				location, RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT, 1u, true );
			Ref<Image> preparedHeight;
			PackedByteArray preparedHeightBytes;
			if ( m_pRuntimePageStore->takeHeight(
				location, RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT,
				preparedHeight, false, &preparedHeightBytes
			) && preparedHeight.is_valid() ) {
				published = m_pSamplingStorage->syncActiveHeightPage(
					location, preparedHeightBytes.ptr(), ( size_t ) ( preparedHeightBytes.size() ) );
			}
		}

		if ( !published && pRegion->getHeightMap().is_valid() ) {
			published = m_pSamplingStorage->syncActiveRegion( location, pRegion );
		}
		if ( published ) {
			++m_stats.nPublishedLastTick;
			continue;
		}

		allRequiredResident = false;
		++m_stats.nMissingRequiredLastTick;
		if ( rFrame.bCpuRegionCacheEnabled && !rFrame.bAuthoring ) {
			result.requestCpuSource( location );
			continue;
		}
		if ( rFrame.bAuthoring && !m_pSamplingStorage->syncActiveRegion( location, pRegion ) ) {
			UtilityFunctions::push_error( vformat(
				"Terrain3D: Failed to publish required authoring height payload for region %s.", location ) );
		}
	}

	m_nStreamingGenerationLast = rFrame.streamingGeneration;
	m_nTopologyGenerationLast = topologyGeneration;
	m_center = rFrame.activeHeightCenter;
	m_bDirty = !allRequiredResident;
	return result;
}
