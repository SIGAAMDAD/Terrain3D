// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world.h"

#include <string.h>


bool Terrain3DRenderWorld::validate_frame_contract_hot( const T3DRenderWorldLimits &pLimits, const T3DVisibilityFrame &pFrame ) const {
	if ( !m_bInitialized || m_nLimitsSignature == 0u ) {
		return false;
	}
	if ( t3d_render_world_limits_signature( pLimits ) != m_nLimitsSignature ) {
		return false;
	}
	if ( pFrame.nHiZWidth == 0u || pFrame.nHiZHeight == 0u || pFrame.nHiZMipCount == 0u ) {
		return false;
	}
	if ( pFrame.nCellCount > m_SealedLimits.nMaxCells ) {
		return false;
	}
	if ( pFrame.nInstanceCount > m_SealedLimits.nMaxInstances ) {
		return false;
	}
	if ( pFrame.nMeshCount > m_SealedLimits.nMaxMeshes ) {
		return false;
	}
	if ( pFrame.nLodCount > m_SealedLimits.nMaxMeshLods ) {
		return false;
	}
	if ( pFrame.nClusterCount > m_SealedLimits.nMaxClusters ) {
		return false;
	}
	if ( pFrame.nVisibleCellCapacity != m_SealedLimits.nMaxVisibleCells ) {
		return false;
	}
	if ( pFrame.nVisibleInstanceCapacity != m_SealedLimits.nMaxVisibleInstances ) {
		return false;
	}
	if ( pFrame.nVisibleWorldCapacity != m_SealedLimits.nMaxVisibleClusters[T3D_RW_TIER_WORLD] ) {
		return false;
	}
	if ( pFrame.nVisibleClutterCapacity != m_SealedLimits.nMaxVisibleClusters[T3D_RW_TIER_CLUTTER] ) {
		return false;
	}
	if ( pFrame.nVisibleFoliageCapacity != m_SealedLimits.nMaxVisibleClusters[T3D_RW_TIER_FOLIAGE] ) {
		return false;
	}
	return true;
}

bool Terrain3DRenderWorld::capture_plateau_snapshot_reference_debug( T3DRenderWorldPlateauSnapshot &rSnapshot ) const {
	memset( &rSnapshot, 0, sizeof( rSnapshot ) );
	if ( !m_bInitialized || m_pResources == NULL || m_pVisibility == NULL || m_pVT == NULL || m_pRaster == NULL ) {
		return false;
	}
	const T3DRenderWorldGpuResourceStats &ResourceStats = m_pResources->get_stats();
	const T3DRenderWorldGpuResourcePlan &Plan = m_pResources->get_plan();
	const T3DGpuVisibilityStats &VisibilityStats = m_pVisibility->get_stats();
	const T3DHiZGpuStats HiZStats = m_pHiZ ? m_pHiZ->get_stats() : T3DHiZGpuStats{};
	const T3DVTGpuStats &VTStats = m_pVT->get_stats();
	const T3DRasterResourceStats &RasterStats = m_pRaster->get_stats();
	rSnapshot.nLimitsSignature = m_nLimitsSignature;
	rSnapshot.nGpuCreateCount = ResourceStats.nGpuCreateCount + VisibilityStats.nGpuCreateCount + HiZStats.nGpuCreateCount
	        + VTStats.nGpuCreateCount + RasterStats.nGpuCreateCount;
	rSnapshot.nGpuFreeCount = ResourceStats.nGpuFreeCount + VisibilityStats.nGpuFreeCount + HiZStats.nGpuFreeCount + VTStats.nGpuFreeCount
	        + RasterStats.nGpuFreeCount;
	if ( m_pAuxRaster != NULL ) {
		// Aux currently exposes draw telemetry only; its hot seal detects resource epoch changes internally.
	}
	rSnapshot.nGpuBufferBytes = Plan.nTotalBufferBytes;
	rSnapshot.nGpuTextureBytes = Plan.nTotalTextureBytes;
	for ( uint32_t iChannel = 0u; iChannel < T3D_RW_VT_CHANNEL_COUNT; ++iChannel ) {
		rSnapshot.nVTPhysicalBytes += Plan.nVtPhysicalBytes[iChannel];
	}
	rSnapshot.nVisibilityCapacity = ( uint64_t )m_SealedLimits.nMaxVisibleCells + ( uint64_t )m_SealedLimits.nMaxVisibleInstances;
	for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
		rSnapshot.nVisibilityCapacity += m_SealedLimits.nMaxVisibleClusters[iTier];
	}
	rSnapshot.nCpuAllocationCount = 0u;
	rSnapshot.nSyncWaitCount = m_Stats.nSyncWaitCount;
	rSnapshot.nSyncReadbackCount = m_Stats.nSyncReadbackCount;
	rSnapshot.nLiveResourceCount = ResourceStats.nLiveResourceCount;
	rSnapshot.nHotViolationCount = ResourceStats.nHotSealViolationCount + VisibilityStats.nHotViolationCount + HiZStats.nHotViolationCount
	        + VTStats.nHotViolationCount + RasterStats.nHotViolationCount;
	return true;
}

bool Terrain3DRenderWorld::validate_plateau_reference_debug(
        const T3DRenderWorldPlateauSnapshot &pBaseline, const T3DRenderWorldPlateauSnapshot &pCurrent ) const {
	return t3d_render_world_plateau_snapshot_equal_reference_debug( pBaseline, pCurrent );
}

Terrain3DRenderWorld::Terrain3DRenderWorld() {
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	m_pVisibility = NULL;
	m_pHiZ = NULL;
	m_pVT = NULL;
	m_pRaster = NULL;
	m_pStreaming = NULL;
	m_pResidency = NULL;
	m_pForwardBindings = NULL;
	m_pAuxRaster = NULL;
	memset( &m_Stats, 0, sizeof( m_Stats ) );
	memset( &m_SealedLimits, 0, sizeof( m_SealedLimits ) );
	m_nLimitsSignature = 0u;
	m_bInitialized = false;
	m_bHotSealed = false;
	m_bSubmissionEnabled = true;
}

bool Terrain3DRenderWorld::initialize_cold( const Terrain3DRenderWorldResourceImage *pResources,
        Terrain3DRenderWorldGpuVisibility *pVisibility,
        Terrain3DRenderWorldHiZGpu *pHiZ,
        Terrain3DRenderWorldVTGpu *pVT,
        Terrain3DRenderWorldThreeTierRaster *pRaster,
        T3DVTStreamingState *pStreaming,
        T3DVTResidencyState *pResidency ) {
	Terrain3DRdContext *const pContext = pResources ? pResources->get_rd_context() : NULL;
	godot::RenderingDevice *const pRenderingDevice = pContext ? pContext->getDevice() : NULL;
	if ( m_bInitialized || pRenderingDevice == NULL || pResources == NULL || !pResources->is_initialized() || pVisibility == NULL
	        || !pVisibility->is_initialized() || ( pHiZ != NULL && !pHiZ->is_initialized() ) || pVT == NULL || !pVT->is_initialized()
	        || pRaster == NULL || !pRaster->is_initialized() || pStreaming == NULL || !pStreaming->bInitialized || pResidency == NULL
	        || !pResidency->bInitialized || !pResidency->bNeutralPublished || pStreaming->pResidency != pResidency ) {
		return false;
	}
	m_pRenderingDevice = pRenderingDevice;
	m_pResources = pResources;
	m_pVisibility = pVisibility;
	m_pHiZ = pHiZ;
	m_pVT = pVT;
	m_pRaster = pRaster;
	m_pStreaming = pStreaming;
	m_pResidency = pResidency;
	m_SealedLimits = pResources->get_limits();
	m_nLimitsSignature = t3d_render_world_limits_signature( m_SealedLimits );
	if ( m_nLimitsSignature == 0u ) {
		return false;
	}
	m_bInitialized = true;
	return true;
}

void Terrain3DRenderWorld::set_submission_enabled_cold( bool pEnabled ) {
	m_bSubmissionEnabled = pEnabled;
}

bool Terrain3DRenderWorld::is_submission_enabled() const {
	return m_bSubmissionEnabled;
}

void Terrain3DRenderWorld::seal_hot_runtime() {
	if ( !m_bInitialized ) {
		return;
	}
	m_pVisibility->seal_hot_runtime();
	if ( m_pHiZ != NULL ) {
		m_pHiZ->seal_hot_runtime();
	}
	m_pVT->seal_hot_runtime();
	m_pRaster->seal_hot_runtime();
	if ( m_pAuxRaster != NULL ) {
		m_pAuxRaster->seal_hot_runtime();
	}
	m_bHotSealed = true;
}


void Terrain3DRenderWorld::shutdown_cold() {
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	m_pVisibility = NULL;
	m_pHiZ = NULL;
	m_pVT = NULL;
	m_pRaster = NULL;
	m_pStreaming = NULL;
	m_pResidency = NULL;
	m_pForwardBindings = NULL;
	m_pAuxRaster = NULL;
	memset( &m_Stats, 0, sizeof( m_Stats ) );
	memset( &m_SealedLimits, 0, sizeof( m_SealedLimits ) );
	m_nLimitsSignature = 0u;
	m_bInitialized = false;
	m_bHotSealed = false;
	m_bSubmissionEnabled = true;
}

bool Terrain3DRenderWorld::attach_forward_bindings_cold( T3DForwardBorrowedBindingState *pForwardBindings ) {
	if ( !m_bInitialized || m_bHotSealed || pForwardBindings == NULL || !pForwardBindings->bInitialized ) {
		return false;
	}
	m_pForwardBindings = pForwardBindings;
	return true;
}

bool Terrain3DRenderWorld::attach_aux_raster_cold( Terrain3DRenderWorldAuxRaster *pAuxRaster ) {
	if ( !m_bInitialized || m_bHotSealed || pAuxRaster == NULL || !pAuxRaster->is_initialized() ) {
		return false;
	}
	m_pAuxRaster = pAuxRaster;
	return true;
}

bool Terrain3DRenderWorld::submit_aux_pass_hot( T3DRenderWorldAuxPass pPass,
        int64_t pDrawList,
        uint32_t pShadowViewCount,
        uint32_t pShadowVisibleClusterCount,
        uint32_t &rDrawCount ) {
	rDrawCount = 0u;
	if ( !m_bSubmissionEnabled ) {
		return true;
	}
	if ( !m_bInitialized || !m_bHotSealed || m_pAuxRaster == NULL || m_pForwardBindings == NULL || pDrawList < 0 ) {
		++m_Stats.nFailureCount;
		return false;
	}
	const uint32_t nRequired = T3D_RW_FORWARD_ENVIRONMENT | T3D_RW_FORWARD_DIRECTIONAL_LIGHTS;
	if ( !t3d_render_world_forward_bindings_validate_reference_debug( *m_pForwardBindings, nRequired ) ) {
		++m_Stats.nFailureCount;
		return false;
	}
	T3DRenderWorldAuxBindings Bindings = {};
	if ( !m_pAuxRaster->get_bindings_hot( Bindings )
	        || !t3d_render_world_submit_aux_indirect_hot( pDrawList, pPass, *m_pResources, Bindings, rDrawCount ) ) {
		++m_Stats.nFailureCount;
		return false;
	}
	if ( pPass == T3D_RW_AUX_PASS_SHADOW && !m_pAuxRaster->record_shadow_visibility_hot( pShadowViewCount, pShadowVisibleClusterCount ) ) {
		++m_Stats.nFailureCount;
		return false;
	}
	if ( !m_pAuxRaster->record_pass_draws_hot( pPass, rDrawCount ) ) {
		++m_Stats.nFailureCount;
		return false;
	}
	return true;
}

bool Terrain3DRenderWorld::consume_vt_readback_owner_hot(
        uint32_t pSlot, uint32_t pFrameId, const uint8_t *pReadbackBytes, uint32_t pReadbackByteCount, uint32_t &rAcceptedRequestCount ) {
	rAcceptedRequestCount = 0u;
	if ( !m_bInitialized || !m_bHotSealed || m_pVT == NULL || m_pStreaming == NULL ) {
		++m_Stats.nFailureCount;
		return false;
	}
	if ( !m_pVT->consume_request_readback_owner_hot(
	             pSlot, pFrameId, pReadbackBytes, pReadbackByteCount, *m_pStreaming, rAcceptedRequestCount ) ) {
		++m_Stats.nFailureCount;
		return false;
	}
	++m_Stats.nVTReadbackConsumedCount;
	m_Stats.nVTAcceptedRequestCount += rAcceptedRequestCount;
	return true;
}

bool Terrain3DRenderWorld::claim_vt_stream_job_owner_hot( uint32_t &rJobIndex ) {
	if ( !m_bInitialized || !m_bHotSealed || m_pStreaming == NULL ) {
		return false;
	}
	return t3d_render_world_vt_streaming_claim_job_owner_hot( *m_pStreaming, rJobIndex );
}

bool Terrain3DRenderWorld::execute_vt_stream_job_async( uint32_t pJobIndex, T3DVTStreamReadDecodeFn pReadDecode, void *pUser ) {
	if ( !m_bInitialized || !m_bHotSealed || m_pStreaming == NULL ) {
		return false;
	}
	return t3d_render_world_vt_streaming_execute_job_async( *m_pStreaming, pJobIndex, pReadDecode, pUser );
}

bool Terrain3DRenderWorld::render_main_camera_hot(
        const T3DRenderWorldLimits &pLimits, const T3DVisibilityFrame &pFrame, int64_t pDrawList, T3DRenderWorldFrameResult &rResult ) {
	memset( &rResult, 0, sizeof( rResult ) );
	if ( !m_bSubmissionEnabled ) {
		return true;
	}
	if ( !m_bInitialized || !m_bHotSealed || pDrawList < 0 || !validate_frame_contract_hot( pLimits, pFrame ) ) {
		++m_Stats.nFailureCount;
		return false;
	}

	// Retired VT slots keep their old backing and GPU mapping until invalidation is
	// accepted. Service those retirements before claiming the permanent upload spare.
	m_pVT->service_retired_slots_hot( *m_pResidency );

	// Bounded owner-side VT publication is the only CPU->GPU mutation here beyond the
	// fixed 320-byte frame packet. Visibility, counts, tier lists and indirect args remain GPU-owned.
	rResult.nVTUploadBytes = t3d_render_world_vt_streaming_service_uploads_hot(
	        *m_pStreaming, pFrame.nFrameId, Terrain3DRenderWorldVTGpu::upload_commit_callback, m_pVT );

	const bool bVisibilityOk = m_pHiZ != NULL
	        ? ( m_pVisibility->publish_frame_hot( pFrame )
	                  && m_pHiZ->dispatch_hiz_hot( pFrame.nHiZWidth, pFrame.nHiZHeight, pFrame.nHiZMipCount )
	                  && m_pHiZ->dispatch_visibility_m05_hot( *m_pVisibility, pLimits ) )
	        : ( m_pVisibility->publish_frame_hot( pFrame ) && m_pVisibility->dispatch_m04_hot( pLimits ) );
	if ( !bVisibilityOk || !m_pVT->dispatch_prediction_hot( pLimits ) ) {
		++m_Stats.nFailureCount;
		return false;
	}

	if ( m_pVT->is_readback_enabled() ) {
		const uint32_t nBusyBefore = m_pVT->get_stats().nReadbackRingBusyCount;
		if ( !m_pVT->schedule_request_readback_hot( pFrame.nFrameId ) ) {
			if ( m_pVT->get_stats().nReadbackRingBusyCount == nBusyBefore ) {
				++m_Stats.nFailureCount;
				return false;
			}
			rResult.nStatusBits |= T3D_RW_FRAME_VT_READBACK_DEFERRED;
			++m_Stats.nVTReadbackDeferredCount;
		}
	}

	T3DThreeTierRasterBindings Bindings;
	if ( !m_pRaster->get_bindings_hot( Bindings ) ) {
		++m_Stats.nFailureCount;
		return false;
	}
	T3DThreeTierUploadStats DrawStats = {};
	if ( !t3d_render_world_submit_three_tier_indirect_hot( pDrawList, *m_pResources, Bindings, DrawStats )
	        || DrawStats.nDrawCount != T3D_RW_TIER_COUNT ) {
		++m_Stats.nFailureCount;
		return false;
	}

	rResult.nDrawCount = DrawStats.nDrawCount;
	++m_Stats.nFrameCount;
	m_Stats.nDrawCount += DrawStats.nDrawCount;
	m_Stats.nVTUploadBytes += rResult.nVTUploadBytes;
	if ( Terrain3DRdContext *const pContext = m_pResources->get_rd_context() ) {
		pContext->recordSuccessfulPass( Terrain3DRdContext::PASS_RENDER_WORLD, pFrame.nFrameId );
	}
	return true;
}

bool Terrain3DRenderWorld::is_initialized() const {
	return m_bInitialized;
}
const T3DRenderWorldProductionStats &Terrain3DRenderWorld::get_stats() const {
	return m_Stats;
}
