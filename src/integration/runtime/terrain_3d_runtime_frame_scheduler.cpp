// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "integration/runtime/terrain_3d_runtime_frame_scheduler.h"
#include "core/utility/logger.h"
#include "memory/core/terrain_3d_memory.h"
#include "rendering/rd/core/terrain_3d_render_coordinator.h"
#include "instances/physics/terrain_3d_instance_occlusion.h"
#include "instances/physics/terrain_3d_instance_physics.h"
#include "streaming/runtime/terrain_3d_node_streamer.h"
#include "lighting/runtime/terrain_3d_lightmap_streamer.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/time.hpp>

void Terrain3DRuntimeFrameScheduler::serviceColdRuntimeOwners( const t3dRuntimeFrameServices_t &rServices, const Vector3 &rTargetPosition ) {
	if ( rServices.m_pData ) {
		rServices.m_pData->serviceColdInstanceResidency( rTargetPosition, false );
	}
	if ( rServices.m_pInstancer ) {
		rServices.m_pInstancer->serviceColdCapacity();
	}
	if ( rServices.m_pInstanceOcclusion ) {
		rServices.m_pInstanceOcclusion->serviceColdCapacity();
	}
	if ( rServices.m_pInstancePhysics ) {
		rServices.m_pInstancePhysics->serviceColdCapacity();
		rServices.m_pInstancePhysics->updateResidency( rTargetPosition, false );
	}
	if ( rServices.m_pNodeStreamer ) {
		rServices.m_pNodeStreamer->serviceColdRebuild();
	}
	if ( rServices.m_pCollision ) {
		rServices.m_pCollision->serviceColdRebuild();
	}
	if ( rServices.m_pRenderCoordinator ) {
		rServices.m_pRenderCoordinator->serviceColdCapacity();
	}
}

void Terrain3DRuntimeFrameScheduler::updateRuntimeBoundaryPhasing(
        const t3dRuntimeFrameServices_t &rServices, const Vector3 &rTargetPosition, t3dRuntimeProfileSample_t *const pProfileSample ) {
	++rServices.m_iRuntimePhysicsFrameIndex;
	const Vector2i target_region = rServices.m_pData ? rServices.m_pData->getRegionLocation( rTargetPosition ) : s_v2iMax;
	if ( target_region != rServices.m_runtimeBoundaryTargetRegion ) {
		rServices.m_runtimeBoundaryTargetRegion = target_region;
		rServices.m_nRuntimeBoundaryTransitionFrame = rServices.m_iRuntimePhysicsFrameIndex;
	}
	const uint64_t boundary_age_frames = rServices.m_iRuntimePhysicsFrameIndex - rServices.m_nRuntimeBoundaryTransitionFrame;
	rServices.m_bRuntimeBoundaryNodeDeferredLastTick = t3dBoundaryShouldDefer( rServices.m_bRuntimeBoundaryWorkPhasingEnabled,
	        rServices.m_host.effectiveDynamicNodeStreamingEnabled(),
	        boundary_age_frames,
	        rServices.m_nRuntimeBoundaryNodeDelayFrames,
	        rServices.m_nRuntimeBoundaryNodeDeferralStreak );
	rServices.m_bRuntimeBoundaryInstancerDeferredLastTick = t3dBoundaryShouldDefer( rServices.m_bRuntimeBoundaryWorkPhasingEnabled,
	        rServices.m_host.effectiveInstancerCellStreamingEnabled(),
	        boundary_age_frames,
	        rServices.m_nRuntimeBoundaryInstancerDelayFrames,
	        rServices.m_nRuntimeBoundaryInstancerDeferralStreak );
	rServices.m_bRuntimeBoundaryWorldDeferredLastTick = t3dBoundaryShouldDefer( rServices.m_bRuntimeBoundaryWorkPhasingEnabled,
	        true,
	        boundary_age_frames,
	        rServices.m_nRuntimeBoundaryWorldDelayFrames,
	        rServices.m_nRuntimeBoundaryWorldDeferralStreak );
	rServices.m_nRuntimeBoundaryNodeDeferralStreak =
	        rServices.m_bRuntimeBoundaryNodeDeferredLastTick ? rServices.m_nRuntimeBoundaryNodeDeferralStreak + 1 : 0;
	rServices.m_nRuntimeBoundaryInstancerDeferralStreak =
	        rServices.m_bRuntimeBoundaryInstancerDeferredLastTick ? rServices.m_nRuntimeBoundaryInstancerDeferralStreak + 1 : 0;
	rServices.m_nRuntimeBoundaryWorldDeferralStreak =
	        rServices.m_bRuntimeBoundaryWorldDeferredLastTick ? rServices.m_nRuntimeBoundaryWorldDeferralStreak + 1 : 0;
	if ( rServices.m_bRuntimeBoundaryWorldDeferredLastTick ) {
		++rServices.m_nRuntimeBoundaryWorldDeferralsTotal;
	}
	if ( pProfileSample ) {
		pProfileSample->m_fTargetX = ( float )( rTargetPosition.x );
		pProfileSample->m_fTargetZ = ( float )( rTargetPosition.z );
		if ( rServices.m_nRuntimeProfileSampleCount > 0u ) {
			pProfileSample->m_fMovementX = pProfileSample->m_fTargetX - rServices.m_runtimeProfileLastSample.m_fTargetX;
			pProfileSample->m_fMovementZ = pProfileSample->m_fTargetZ - rServices.m_runtimeProfileLastSample.m_fTargetZ;
		}
		pProfileSample->m_nTargetRegionX = target_region.x;
		pProfileSample->m_nTargetRegionZ = target_region.y;
	}
}

void Terrain3DRuntimeFrameScheduler::updateRuntimeStreamingOwners( const t3dRuntimeFrameServices_t &rServices,
        const Vector3 &rTargetPosition,
        t3dRuntimeProfileSample_t *const pProfileSample,
        uint64_t &rPhaseStartedUsec ) {
	if ( rServices.m_pData ) {
		rServices.m_pData->updateRegionResidency( rTargetPosition, false );
	}
	if ( pProfileSample ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		pProfileSample->m_nDataResidencyUsec = now - rPhaseStartedUsec;
		rPhaseStartedUsec = now;
	}
	if ( rServices.m_pInstanceOcclusion ) {
		rServices.m_pInstanceOcclusion->updateResidency( rTargetPosition, false );
	}
	const bool hlod_active = rServices.m_bHlodEnabled
	        && ( !Engine::get_singleton()->is_editor_hint() || !rServices.m_host.isEditorAuthoringModeInternal() || rServices.m_bEditorHlodPreview );
	if ( rServices.m_pHlodStreamer ) {
		if ( hlod_active ) {
			rServices.m_pHlodStreamer->updateResidency( rTargetPosition, false );
		} else {
			rServices.m_pHlodStreamer->suspend();
		}
	}
	if ( pProfileSample ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		pProfileSample->m_nHlodStreamingUsec = now - rPhaseStartedUsec;
		rPhaseStartedUsec = now;
	}
	if ( rServices.m_pNodeStreamer && rServices.m_host.effectiveDynamicNodeStreamingEnabled() ) {
		if ( rServices.m_bRuntimeBoundaryNodeDeferredLastTick ) {
			rServices.m_pNodeStreamer->deferUpdateForFrame();
			++rServices.m_nRuntimeBoundaryNodeDeferralsTotal;
		} else {
			rServices.m_pNodeStreamer->updateResidency( rTargetPosition, false );
		}
	}
	if ( pProfileSample ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		pProfileSample->m_nNodeStreamingUsec = now - rPhaseStartedUsec;
		rPhaseStartedUsec = now;
	}
	if ( rServices.m_pLightmapStreamer && rServices.m_bLightmapStreamingEnabled ) {
		rServices.m_pLightmapStreamer->updateResidency( rTargetPosition, false );
	}
	if ( pProfileSample ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		pProfileSample->m_nLightmapStreamingUsec = now - rPhaseStartedUsec;
		rPhaseStartedUsec = now;
	}
}

void Terrain3DRuntimeFrameScheduler::updateRuntimeClipmap( const t3dRuntimeFrameServices_t &rServices, const Vector3 &rTargetPosition ) {
	if ( rServices.m_nTessellationLevel > 0 ) {
		if ( rServices.m_pTerrainMesher && rServices.m_material.is_valid() ) {
			const Vector2 target_position_2d = t3dVector3ToVector2( rTargetPosition );
			const real_t fTessellationDensity = 1.0f / ( real_t )( 1u << ( uint32_t )rServices.m_nTessellationLevel );
			const real_t fBufferVertexSpacing = s_fRegionVertexSpacing * fTessellationDensity;
			const real_t fUpdateDistance = MAX( fBufferVertexSpacing, rServices.m_fDisplacementUpdateDistance );
			const bool bMoved = MAX( abs( rServices.m_lastBufferPosition.x - target_position_2d.x ),
			                            abs( rServices.m_lastBufferPosition.y - target_position_2d.y ) )
			        >= fUpdateDistance;
			const uint64_t nPageTableGeneration = rServices.m_pData ? rServices.m_pData->getPhysicalRegionMapGenerationInternal() : 0u;
			const bool bPageTableChanged = nPageTableGeneration != rServices.m_nComputeDisplacementPageTableGeneration;
			const bool bDisplacementDirty = rServices.m_bComputeDisplacementDirty.load( T3D_MEMORY_ORDER_ACQUIRE );

			if ( bMoved ) {
				rServices.m_lastBufferPosition = target_position_2d;
				rServices.m_pTerrainMesher->snap( rTargetPosition );
			}
			if ( bMoved || bPageTableChanged || bDisplacementDirty ) {
				if ( rServices.m_host.tryUpdateComputeDisplacement( rTargetPosition ) ) {
					rServices.m_nComputeDisplacementPageTableGeneration = nPageTableGeneration;
					rServices.m_bComputeDisplacementDirty.store( false, T3D_MEMORY_ORDER_RELEASE );
				} else {
					rServices.m_bComputeDisplacementDirty.store( true, T3D_MEMORY_ORDER_RELEASE );
				}
			}
		}
	} else if ( rServices.m_pTerrainMesher ) {
		rServices.m_pTerrainMesher->snap( rTargetPosition );
	}
	if ( rServices.m_pRenderCoordinator ) {
		rServices.m_pRenderCoordinator->update();
	}
}

void Terrain3DRuntimeFrameScheduler::updateRuntimeInstancer( const t3dRuntimeFrameServices_t &rServices, const Vector3 &rTargetPosition ) {
	const bool bInstanceCellStreaming = rServices.m_host.effectiveInstancerCellStreamingEnabled();
	const bool bInstanceTargetTracking = bInstanceCellStreaming || rServices.m_bInstancerSingleLodBuffers;
	if ( rServices.m_pInstancer && bInstanceTargetTracking ) {
		if ( bInstanceCellStreaming && rServices.m_bRuntimeBoundaryInstancerDeferredLastTick ) {
			rServices.m_pInstancer->deferStreamingSelectionForFrame();
			++rServices.m_nRuntimeBoundaryInstancerDeferralsTotal;
		} else {
			rServices.m_pInstancer->updateStreaming( rTargetPosition );
		}
	}
	if ( rServices.m_pInstancer ) {
		rServices.m_pInstancer->servicePendingUpdates();
	}
}

void Terrain3DRuntimeFrameScheduler::tick( const t3dRuntimeFrameServices_t &rServices, const double fDelta ) {
	if ( !rServices.m_bInitialized ) {
		return;
	}
	rServices.m_host.serviceMemoryBudgetPolicy( false );
	if ( !rServices.m_hCamera.isValid() ) {
		LOG( DEBUG, "Camera is null, getting the current one" );
		rServices.m_host.grabCamera();
	}
	const Vector3 clipmap_target_position = rServices.m_host.getClipmapTargetPosition();
	rServices.m_host.updateRegionStreamingPolicy( clipmap_target_position, fDelta, false );
	serviceColdRuntimeOwners( rServices, clipmap_target_position );
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME );
	if ( rServices.m_bComputeDisplacementFallbackRequested.exchange( false, T3D_MEMORY_ORDER_ACQ_REL ) ) {
		rServices.m_lastBufferPosition = s_v2Max;
	}

	t3dRuntimeProfileSample_t profile_sample;
	const bool bProfileFrame = rServices.m_bRuntimeProfilingEnabled;
	const bool bAdaptiveFrame = rServices.m_bRuntimeAdaptiveWorkBudgetsEnabled;
	const bool bMeasureFrame = bProfileFrame || bAdaptiveFrame;
	const uint64_t frame_started_usec = bMeasureFrame ? Time::get_singleton()->get_ticks_usec() : 0u;
	uint64_t phase_started_usec = frame_started_usec;
	rServices.m_nRuntimeAdaptiveControllerUsecLast = 0u;
	if ( bAdaptiveFrame ) {
		rServices.m_host.updateRuntimeAdaptiveWorkBudgets();
		const uint64_t adaptive_completed_usec = Time::get_singleton()->get_ticks_usec();
		rServices.m_nRuntimeAdaptiveControllerUsecLast = adaptive_completed_usec - frame_started_usec;
		if ( bProfileFrame ) {
			phase_started_usec = adaptive_completed_usec;
		}
	}

	updateRuntimeBoundaryPhasing( rServices, clipmap_target_position, bProfileFrame ? &profile_sample : NULL );
	// Boundary state is part of the same frame policy as adaptive budgets.
	rServices.m_host.applyRuntimeAdaptiveWorkBudgets();
	updateRuntimeStreamingOwners( rServices, clipmap_target_position, bProfileFrame ? &profile_sample : NULL, phase_started_usec );
	updateRuntimeClipmap( rServices, clipmap_target_position );
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nClipmapUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( rServices.m_bOceanEnabled && rServices.m_pOceanMesher ) {
		rServices.m_pOceanMesher->snap( clipmap_target_position );
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nOceanUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	updateRuntimeInstancer( rServices, clipmap_target_position );
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nInstancerUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	rServices.m_host.syncLightShaderParameters();
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nLightSyncUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( rServices.m_pCollision ) {
		rServices.m_pCollision->servicePendingRegionMapUpdate();
		if ( rServices.m_pCollision->isEnabled() ) {
			rServices.m_pCollision->update();
		}
	}
	if ( bMeasureFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		const uint64_t measured_usec = now - frame_started_usec;
		if ( bAdaptiveFrame ) {
			rServices.m_nRuntimeAdaptiveLastMeasuredUsec = measured_usec;
		}
		if ( bProfileFrame ) {
			profile_sample.m_nCollisionUsec = now - phase_started_usec;
			profile_sample.m_nPhysicsTotalUsec = measured_usec;
			profile_sample.m_nStreamingUsec = profile_sample.m_nDataResidencyUsec + profile_sample.m_nHlodStreamingUsec
			        + profile_sample.m_nNodeStreamingUsec + profile_sample.m_nLightmapStreamingUsec + profile_sample.m_nInstancerUsec;
			rServices.m_host.sampleRuntimeProfile( profile_sample );
		}
	}
}
