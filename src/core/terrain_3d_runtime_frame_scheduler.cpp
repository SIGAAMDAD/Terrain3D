// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "terrain_3d_runtime_frame_scheduler.h"
#include "terrain_3d.h"
#include "logger.h"
#include "memory/terrain_3d_memory.h"
#include "rendering/rd/terrain_3d_render_coordinator.h"
#include "instances/terrain_3d_instance_occlusion.h"
#include "instances/terrain_3d_instance_physics.h"
#include "streaming/terrain_3d_node_streamer.h"
#include "lighting/terrain_3d_lightmap_streamer.h"
#include "hlod/terrain_3d_hlod_streamer.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/time.hpp>

void Terrain3DRuntimeFrameScheduler::serviceColdRuntimeOwners( Terrain3D &rTerrain, const Vector3 &rTargetPosition ) {
	if ( rTerrain.m_pData ) {
		rTerrain.m_pData->serviceColdInstanceResidency( rTargetPosition, false );
	}
	if ( rTerrain.m_pInstancer ) {
		rTerrain.m_pInstancer->serviceColdCapacity();
	}
	if ( rTerrain.m_pInstanceOcclusion ) {
		rTerrain.m_pInstanceOcclusion->serviceColdCapacity();
	}
	if ( rTerrain.m_pInstancePhysics ) {
		rTerrain.m_pInstancePhysics->serviceColdCapacity();
		rTerrain.m_pInstancePhysics->updateResidency( rTargetPosition, false );
	}
	if ( rTerrain.m_pNodeStreamer ) {
		rTerrain.m_pNodeStreamer->serviceColdRebuild();
	}
	if ( rTerrain.m_pCollision ) {
		rTerrain.m_pCollision->serviceColdRebuild();
	}
	if ( rTerrain.m_pRenderCoordinator ) {
		rTerrain.m_pRenderCoordinator->serviceColdCapacity();
	}
}

void Terrain3DRuntimeFrameScheduler::updateRuntimeBoundaryPhasing(
        Terrain3D &rTerrain, const Vector3 &rTargetPosition, t3dRuntimeProfileSample_t *const pProfileSample ) {
	++rTerrain.m_iRuntimePhysicsFrameIndex;
	const Vector2i target_region = rTerrain.m_pData ? rTerrain.m_pData->getRegionLocation( rTargetPosition ) : s_v2iMax;
	if ( target_region != rTerrain.m_runtimeBoundaryTargetRegion ) {
		rTerrain.m_runtimeBoundaryTargetRegion = target_region;
		rTerrain.m_nRuntimeBoundaryTransitionFrame = rTerrain.m_iRuntimePhysicsFrameIndex;
	}
	const uint64_t boundary_age_frames = rTerrain.m_iRuntimePhysicsFrameIndex - rTerrain.m_nRuntimeBoundaryTransitionFrame;
	rTerrain.m_bRuntimeBoundaryNodeDeferredLastTick = t3dBoundaryShouldDefer( rTerrain.m_bRuntimeBoundaryWorkPhasingEnabled,
	        rTerrain.effectiveDynamicNodeStreamingEnabled(),
	        boundary_age_frames,
	        rTerrain.m_nRuntimeBoundaryNodeDelayFrames,
	        rTerrain.m_nRuntimeBoundaryNodeDeferralStreak );
	rTerrain.m_bRuntimeBoundaryInstancerDeferredLastTick = t3dBoundaryShouldDefer( rTerrain.m_bRuntimeBoundaryWorkPhasingEnabled,
	        rTerrain.effectiveInstancerCellStreamingEnabled(),
	        boundary_age_frames,
	        rTerrain.m_nRuntimeBoundaryInstancerDelayFrames,
	        rTerrain.m_nRuntimeBoundaryInstancerDeferralStreak );
	rTerrain.m_bRuntimeBoundaryWorldDeferredLastTick = t3dBoundaryShouldDefer( rTerrain.m_bRuntimeBoundaryWorkPhasingEnabled,
	        true,
	        boundary_age_frames,
	        rTerrain.m_nRuntimeBoundaryWorldDelayFrames,
	        rTerrain.m_nRuntimeBoundaryWorldDeferralStreak );
	rTerrain.m_nRuntimeBoundaryNodeDeferralStreak =
	        rTerrain.m_bRuntimeBoundaryNodeDeferredLastTick ? rTerrain.m_nRuntimeBoundaryNodeDeferralStreak + 1 : 0;
	rTerrain.m_nRuntimeBoundaryInstancerDeferralStreak =
	        rTerrain.m_bRuntimeBoundaryInstancerDeferredLastTick ? rTerrain.m_nRuntimeBoundaryInstancerDeferralStreak + 1 : 0;
	rTerrain.m_nRuntimeBoundaryWorldDeferralStreak =
	        rTerrain.m_bRuntimeBoundaryWorldDeferredLastTick ? rTerrain.m_nRuntimeBoundaryWorldDeferralStreak + 1 : 0;
	if ( rTerrain.m_bRuntimeBoundaryWorldDeferredLastTick ) {
		++rTerrain.m_nRuntimeBoundaryWorldDeferralsTotal;
	}
	if ( pProfileSample ) {
		pProfileSample->m_fTargetX = ( float )( rTargetPosition.x );
		pProfileSample->m_fTargetZ = ( float )( rTargetPosition.z );
		if ( rTerrain.m_nRuntimeProfileSampleCount > 0u ) {
			pProfileSample->m_fMovementX = pProfileSample->m_fTargetX - rTerrain.m_runtimeProfileLastSample.m_fTargetX;
			pProfileSample->m_fMovementZ = pProfileSample->m_fTargetZ - rTerrain.m_runtimeProfileLastSample.m_fTargetZ;
		}
		pProfileSample->m_nTargetRegionX = target_region.x;
		pProfileSample->m_nTargetRegionZ = target_region.y;
	}
}

void Terrain3DRuntimeFrameScheduler::updateRuntimeStreamingOwners( Terrain3D &rTerrain,
        const Vector3 &rTargetPosition,
        t3dRuntimeProfileSample_t *const pProfileSample,
        uint64_t &rPhaseStartedUsec ) {
	if ( rTerrain.m_pData ) {
		rTerrain.m_pData->updateRegionResidency( rTargetPosition, false );
	}
	if ( pProfileSample ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		pProfileSample->m_nDataResidencyUsec = now - rPhaseStartedUsec;
		rPhaseStartedUsec = now;
	}
	if ( rTerrain.m_pInstanceOcclusion ) {
		rTerrain.m_pInstanceOcclusion->updateResidency( rTargetPosition, false );
	}
	const bool hlod_active = rTerrain.m_bHlodEnabled
	        && ( !Engine::get_singleton()->is_editor_hint() || !rTerrain.isEditorAuthoringModeInternal() || rTerrain.m_bEditorHlodPreview );
	if ( rTerrain.m_pHlodStreamer ) {
		if ( hlod_active ) {
			rTerrain.m_pHlodStreamer->updateResidency( rTargetPosition, false );
		} else {
			rTerrain.m_pHlodStreamer->suspend();
		}
	}
	if ( pProfileSample ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		pProfileSample->m_nHlodStreamingUsec = now - rPhaseStartedUsec;
		rPhaseStartedUsec = now;
	}
	if ( rTerrain.m_pNodeStreamer && rTerrain.effectiveDynamicNodeStreamingEnabled() ) {
		if ( rTerrain.m_bRuntimeBoundaryNodeDeferredLastTick ) {
			rTerrain.m_pNodeStreamer->deferUpdateForFrame();
			++rTerrain.m_nRuntimeBoundaryNodeDeferralsTotal;
		} else {
			rTerrain.m_pNodeStreamer->updateResidency( rTargetPosition, false );
		}
	}
	if ( pProfileSample ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		pProfileSample->m_nNodeStreamingUsec = now - rPhaseStartedUsec;
		rPhaseStartedUsec = now;
	}
	if ( rTerrain.m_pLightmapStreamer && rTerrain.m_bLightmapStreamingEnabled ) {
		rTerrain.m_pLightmapStreamer->updateResidency( rTargetPosition, false );
	}
	if ( pProfileSample ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		pProfileSample->m_nLightmapStreamingUsec = now - rPhaseStartedUsec;
		rPhaseStartedUsec = now;
	}
}

void Terrain3DRuntimeFrameScheduler::updateRuntimeClipmap( Terrain3D &rTerrain, const Vector3 &rTargetPosition ) {
	if ( rTerrain.m_nTessellationLevel > 0 ) {
		if ( rTerrain.m_pTerrainMesher && rTerrain.m_material.is_valid() ) {
			const Vector2 target_position_2d = t3dVector3ToVector2( rTargetPosition );
			const real_t fTessellationDensity = 1.0f / ( real_t )( 1u << ( uint32_t )rTerrain.m_nTessellationLevel );
			const real_t fBufferVertexSpacing = s_fRegionVertexSpacing * fTessellationDensity;
			const real_t fUpdateDistance = MAX( fBufferVertexSpacing, rTerrain.m_fDisplacementUpdateDistance );
			const bool bMoved = MAX( abs( rTerrain.m_lastBufferPosition.x - target_position_2d.x ),
			                            abs( rTerrain.m_lastBufferPosition.y - target_position_2d.y ) )
			        >= fUpdateDistance;
			const uint64_t nPageTableGeneration = rTerrain.m_pData ? rTerrain.m_pData->getPhysicalRegionMapGenerationInternal() : 0u;
			const bool bPageTableChanged = nPageTableGeneration != rTerrain.m_nComputeDisplacementPageTableGeneration;
			const bool bDisplacementDirty = rTerrain.m_bComputeDisplacementDirty.load( T3D_MEMORY_ORDER_ACQUIRE );

			if ( bMoved ) {
				rTerrain.m_lastBufferPosition = target_position_2d;
				rTerrain.m_pTerrainMesher->snap( rTargetPosition );
			}
			if ( bMoved || bPageTableChanged || bDisplacementDirty ) {
				if ( rTerrain.tryUpdateComputeDisplacement( rTargetPosition ) ) {
					rTerrain.m_nComputeDisplacementPageTableGeneration = nPageTableGeneration;
					rTerrain.m_bComputeDisplacementDirty.store( false, T3D_MEMORY_ORDER_RELEASE );
				} else {
					rTerrain.m_bComputeDisplacementDirty.store( true, T3D_MEMORY_ORDER_RELEASE );
				}
			}
		}
	} else if ( rTerrain.m_pTerrainMesher ) {
		rTerrain.m_pTerrainMesher->snap( rTargetPosition );
	}
	if ( rTerrain.m_pRenderCoordinator ) {
		rTerrain.m_pRenderCoordinator->update();
	}
}

void Terrain3DRuntimeFrameScheduler::updateRuntimeInstancer( Terrain3D &rTerrain, const Vector3 &rTargetPosition ) {
	const bool bInstanceCellStreaming = rTerrain.effectiveInstancerCellStreamingEnabled();
	const bool bInstanceTargetTracking = bInstanceCellStreaming || rTerrain.m_bInstancerSingleLodBuffers;
	if ( rTerrain.m_pInstancer && bInstanceTargetTracking ) {
		if ( bInstanceCellStreaming && rTerrain.m_bRuntimeBoundaryInstancerDeferredLastTick ) {
			rTerrain.m_pInstancer->deferStreamingSelectionForFrame();
			++rTerrain.m_nRuntimeBoundaryInstancerDeferralsTotal;
		} else {
			rTerrain.m_pInstancer->updateStreaming( rTargetPosition );
		}
	}
	if ( rTerrain.m_pInstancer ) {
		rTerrain.m_pInstancer->servicePendingUpdates();
	}
}

void Terrain3DRuntimeFrameScheduler::tick( Terrain3D &rTerrain, const double fDelta ) {
	if ( !rTerrain.m_bInitialized ) {
		return;
	}
	rTerrain.serviceMemoryBudgetPolicy( false );
	if ( !rTerrain.m_hCamera.isValid() ) {
		LOG( DEBUG, "Camera is null, getting the current one" );
		rTerrain.grabCamera();
	}
	const Vector3 clipmap_target_position = rTerrain.getClipmapTargetPosition();
	rTerrain.updateRegionStreamingPolicy( clipmap_target_position, fDelta, false );
	serviceColdRuntimeOwners( rTerrain, clipmap_target_position );
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME );
	if ( rTerrain.m_bComputeDisplacementFallbackRequested.exchange( false, T3D_MEMORY_ORDER_ACQ_REL ) ) {
		rTerrain.m_lastBufferPosition = s_v2Max;
	}

	t3dRuntimeProfileSample_t profile_sample;
	const bool bProfileFrame = rTerrain.m_bRuntimeProfilingEnabled;
	const bool bAdaptiveFrame = rTerrain.m_bRuntimeAdaptiveWorkBudgetsEnabled;
	const bool bMeasureFrame = bProfileFrame || bAdaptiveFrame;
	const uint64_t frame_started_usec = bMeasureFrame ? Time::get_singleton()->get_ticks_usec() : 0u;
	uint64_t phase_started_usec = frame_started_usec;
	rTerrain.m_nRuntimeAdaptiveControllerUsecLast = 0u;
	if ( bAdaptiveFrame ) {
		rTerrain.updateRuntimeAdaptiveWorkBudgets();
		const uint64_t adaptive_completed_usec = Time::get_singleton()->get_ticks_usec();
		rTerrain.m_nRuntimeAdaptiveControllerUsecLast = adaptive_completed_usec - frame_started_usec;
		if ( bProfileFrame ) {
			phase_started_usec = adaptive_completed_usec;
		}
	}

	updateRuntimeBoundaryPhasing( rTerrain, clipmap_target_position, bProfileFrame ? &profile_sample : NULL );
	// Boundary state is part of the same frame policy as adaptive budgets.
	rTerrain.applyRuntimeAdaptiveWorkBudgets();
	updateRuntimeStreamingOwners( rTerrain, clipmap_target_position, bProfileFrame ? &profile_sample : NULL, phase_started_usec );
	updateRuntimeClipmap( rTerrain, clipmap_target_position );
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nClipmapUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( rTerrain.m_bOceanEnabled && rTerrain.m_pOceanMesher ) {
		rTerrain.m_pOceanMesher->snap( clipmap_target_position );
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nOceanUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	updateRuntimeInstancer( rTerrain, clipmap_target_position );
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nInstancerUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	rTerrain.syncLightShaderParameters();
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nLightSyncUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( rTerrain.m_pCollision ) {
		rTerrain.m_pCollision->servicePendingRegionMapUpdate();
		if ( rTerrain.m_pCollision->isEnabled() ) {
			rTerrain.m_pCollision->update();
		}
	}
	if ( bMeasureFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		const uint64_t measured_usec = now - frame_started_usec;
		if ( bAdaptiveFrame ) {
			rTerrain.m_nRuntimeAdaptiveLastMeasuredUsec = measured_usec;
		}
		if ( bProfileFrame ) {
			profile_sample.m_nCollisionUsec = now - phase_started_usec;
			profile_sample.m_nPhysicsTotalUsec = measured_usec;
			profile_sample.m_nStreamingUsec = profile_sample.m_nDataResidencyUsec + profile_sample.m_nHlodStreamingUsec
			        + profile_sample.m_nNodeStreamingUsec + profile_sample.m_nLightmapStreamingUsec + profile_sample.m_nInstancerUsec;
			rTerrain.sampleRuntimeProfile( profile_sample );
		}
	}
}
