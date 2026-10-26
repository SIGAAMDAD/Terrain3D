// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/core/terrain_3d_diagnostics_publisher.h"
#include "rendering/rd/core/terrain_3d_diagnostic_inputs.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h"

#include "core/utility/terrain_3d_sync.h"

using namespace godot;

size_t Terrain3DDiagnosticsPublisher::getRetainedCpuBytes( const t3dDiagnosticInputs_t& rInputs ) const
{
	const t3dPreviewDiagnosticsView_t m_previewServiceView = rInputs.m_previewService.getDiagnosticsView();
	size_t nBytes = rInputs.m_publicationBuilder.getRetainedCpuBytes() + rInputs.m_forwardPassPreparer.getRetainedCpuBytes();
	nBytes += m_previewServiceView.m_cachedPreviewMeshes.getRetainedCpuBytes();
	nBytes += m_previewServiceView.m_pendingPreviewMeshes.getRetainedCpuBytes();
	nBytes += rInputs.m_instanceRenderer.getRetainedCpuBytes() + m_previewServiceView.m_previewRenderer.getRetainedCpuBytes();
	nBytes += rInputs.m_worldRuntime.getRetainedCpuBytes();
	nBytes += (size_t)( rInputs.m_virtualTexture.get_cpu_image_bytes() );
	nBytes += (size_t)( m_previewServiceView.m_previewVtPackageBytes.size() );
	nBytes += (size_t)( m_previewServiceView.m_previewVtPublishedPackageBytes.size() );
	return nBytes;
}

Dictionary Terrain3DDiagnosticsPublisher::getForwardLaneDiagnostics( const t3dDiagnosticInputs_t& rInputs ) const
{
	const t3dForwardDiagnosticsView_t forward = rInputs.m_forwardPassPreparer.getDiagnosticsView();
	Dictionary result;
	result[ "scene_pass" ] = (int64_t)( forward.m_nForwardLaneScenePass );
	result[ "pass_kind" ] = (int64_t)( forward.m_nForwardLanePassKind );
	result[ "prepare_requested_mask" ] = (int64_t)( forward.m_nForwardPrepareRequestedLaneMask );
	result[ "prepare_ready_mask" ] = (int64_t)( forward.m_nForwardPrepareReadyLaneMask );
	result[ "prepare_failed_mask" ] = (int64_t)( forward.m_nForwardPrepareFailedLaneMask );
	result[ "submit_attempted_mask" ] = (int64_t)( forward.m_nForwardSubmitAttemptedLaneMask );
	result[ "submit_succeeded_mask" ] = (int64_t)( forward.m_nForwardSubmitSucceededLaneMask );
	result[ "submit_failed_mask" ] = (int64_t)( forward.m_nForwardSubmitFailedLaneMask );
	result[ "lane_bit_terrain" ] = (int64_t)( T3D_RD_FORWARD_LANE_TERRAIN );
	result[ "lane_bit_instances" ] = (int64_t)( T3D_RD_FORWARD_LANE_INSTANCES );
	result[ "lane_bit_ocean" ] = (int64_t)( T3D_RD_FORWARD_LANE_OCEAN );
	result[ "instance_renderer_initialized" ] = rInputs.m_bInstanceRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	result[ "instance_published_records" ] = (int64_t)( forward.m_nForwardPublishedInstanceCount );
	result[ "instance_published_cells" ] = (int64_t)( forward.m_nForwardPublishedInstanceCellCount );
	result[ "instance_expected" ] = forward.m_bForwardInstanceLaneExpected;
	result[ "instance_prepare_attempted" ] = forward.m_bForwardInstancePrepareAttempted;
	result[ "instance_prepare_succeeded" ] = forward.m_bForwardInstancePrepareSucceeded;
	result[ "instance_prepared_has_draws" ] = forward.m_bForwardInstancePreparedHasDraws;
	result[ "instance_submit_attempted" ] = forward.m_bForwardInstanceSubmitAttempted;
	result[ "instance_submit_succeeded" ] = forward.m_bForwardInstanceSubmitSucceeded;
	result[ "instance_submit_draws" ] = (int64_t)( forward.m_nForwardInstanceSubmitDraws );
	result[ "instance_lane_unavailable_prepares" ] = (int64_t)( forward.m_nInstanceLaneUnavailablePrepares );
	result[ "instance_prepared_without_draws" ] = (int64_t)( forward.m_nInstancePreparedWithoutDraws );
	result[ "terrain_vt_residency_request_attempts" ] = (int64_t)( forward.m_nTerrainVtResidencyRequestAttempts );
	result[ "terrain_vt_residency_requests_accepted" ] = (int64_t)( forward.m_nTerrainVtResidencyRequestsAccepted );
	result[ "terrain_recoverable_prepare_failures" ] = (int64_t)( forward.m_nTerrainRecoverablePrepareFailures );
	result[ "strict_world_authority_failures" ] = (int64_t)( forward.m_nStrictWorldAuthorityFailures );
	result[ "editor_preview_prepares" ] = (int64_t)( forward.m_nEditorPreviewPrepares );
	result[ "shipping_unified_authority" ] = !rInputs.m_editorAuthoring;
	result[ "instance_prepare_failures" ] = (int64_t)( forward.m_nInstanceForwardPrepareFailures );
	result[ "instance_submit_failures" ] = (int64_t)( forward.m_nInstanceForwardSubmitFailures );
	result[ "instance_depth_submitted_draws" ] =
	    (int64_t)( rInputs.m_instanceRenderer.getForwardSubmittedDrawCount( T3D_RD_FORWARD_PASS_DEPTH ) );
	result[ "instance_opaque_submitted_draws" ] =
	    (int64_t)( rInputs.m_instanceRenderer.getForwardSubmittedDrawCount( T3D_RD_FORWARD_PASS_OPAQUE ) );
	result[ "instance_shadow_submitted_draws" ] =
	    (int64_t)( rInputs.m_instanceRenderer.getForwardSubmittedDrawCount( T3D_RD_FORWARD_PASS_SHADOW ) );
	result[ "instance_motion_submitted_draws" ] =
	    (int64_t)( rInputs.m_instanceRenderer.getForwardSubmittedDrawCount( T3D_RD_FORWARD_PASS_MOTION ) );
	result[ "instance_transparent_submitted_draws" ] =
	    (int64_t)( rInputs.m_instanceRenderer.getForwardSubmittedDrawCount( T3D_RD_FORWARD_PASS_TRANSPARENT ) );
	result[ "instance_last_error" ] = rInputs.m_instanceRenderer.getLastError();
	return result;
}

Dictionary Terrain3DDiagnosticsPublisher::getForwardDrawBaselineDiagnostics( const t3dDiagnosticInputs_t& rInputs ) const
{
	const t3dForwardDiagnosticsView_t forward = rInputs.m_forwardPassPreparer.getDiagnosticsView();
	Dictionary result;
	const t3dRdForwardPassKind_t szPasses[] = {
		T3D_RD_FORWARD_PASS_DEPTH,
		T3D_RD_FORWARD_PASS_OPAQUE,
		T3D_RD_FORWARD_PASS_MOTION,
		T3D_RD_FORWARD_PASS_SHADOW,
	};
	const char *const szNames[] = { "depth", "opaque", "motion", "shadow" };
	for ( uint32_t i = 0u; i < T3D_ARRAY_COUNT( szPasses ); ++i ) {
		const t3dRdForwardDrawBaseline_t& baseline =
		    forward.m_szForwardDrawBaseline[ (uint32_t)( szPasses[ i ] ) ];
		Dictionary pass;
		pass[ "submit_callbacks" ] = (int64_t)( baseline.m_nSubmitCallbacks );
		pass[ "terrain_draws" ] = (int64_t)( baseline.m_nTerrainDraws );
		pass[ "instance_hlod_draws" ] = (int64_t)( baseline.m_nInstanceDraws );
		pass[ "ocean_draws" ] = (int64_t)( baseline.m_nOceanDraws );
		pass[ "world_draws" ] = (int64_t)( baseline.m_nWorldDraws );
		pass[ "total_draws" ] = (int64_t)( baseline.m_nTerrainDraws + baseline.m_nInstanceDraws + baseline.m_nOceanDraws + baseline.m_nWorldDraws );
		result[ String( szNames[ i ] ) ] = pass;
	}
	return result;
}

Dictionary Terrain3DDiagnosticsPublisher::getDiagnostics( const t3dDiagnosticInputs_t& rInputs ) const
{
	const t3dPreviewDiagnosticsView_t m_previewServiceView = rInputs.m_previewService.getDiagnosticsView();
	const t3dPublicationDiagnosticsView_t m_publicationBuilderView = rInputs.m_publicationBuilder.getDiagnosticsView();
	const t3dForwardDiagnosticsView_t m_forwardPassPreparerView = rInputs.m_forwardPassPreparer.getDiagnosticsView();
	Dictionary result;
	const t3dRenderLifecycleStatus_t renderStatus = rInputs.m_lifecycle;
	const Dictionary renderer = rInputs.m_renderer.getDiagnostics();
	const Dictionary instances = rInputs.m_instanceRenderer.getDiagnostics();
	if ( rInputs.m_pTerrain ) {
		result[ "configured_mesh_size" ] = (int64_t)( rInputs.m_pTerrain->getMeshSize() );
		result[ "configured_mesh_lods" ] = (int64_t)( rInputs.m_pTerrain->getMeshLods() );
		result[ "configured_tessellation_level" ] = (int64_t)( rInputs.m_pTerrain->getTessellationLevel() );
		result[ "clipmap_target_position" ] = rInputs.m_pTerrain->getClipmapTargetPosition();
		Terrain3DClipmapSource *const pMesher = rInputs.m_pTerrain->getMesher();
		if ( pMesher ) {
			result[ "mesher_mesh_size" ] = (int64_t)( pMesher->getMeshSize() );
			result[ "mesher_tessellation_level" ] = (int64_t)( pMesher->getTessellationLevel() );
			t3dRdPatch_t patches[ T3D_MAX_CLIPMAP_LEVELS ] {};
			const int patch_count = pMesher->fillRdPatches( patches, T3D_MAX_CLIPMAP_LEVELS );
			result[ "cpu_clipmap_patch_count" ] = (int64_t)( patch_count );
			if ( patch_count > 0 ) {
				result[ "cpu_clipmap_lod0_origin" ] = Vector2( patches[ 0 ].m_fOriginX, patches[ 0 ].m_fOriginZ );
				result[ "cpu_clipmap_lod0_scale" ] = patches[ 0 ].m_fScale;
			}
		}
	}
	result[ "rd_retained_cpu_bytes" ] = (int64_t)( getRetainedCpuBytes( rInputs ) );
	result[ "owner_capacity_misses" ] = (int64_t)( m_publicationBuilderView.m_nCapacityMissCount );
	result[ "owner_growth_denied" ] = (int64_t)( m_publicationBuilderView.m_nOwnerGrowthDeniedCount );
	result[ "owner_cold_capacity_rebuilds" ] = (int64_t)( m_publicationBuilderView.m_nColdCapacityRebuildCount );
	result[ "owner_publication_capacity_pending" ] = m_publicationBuilderView.m_bInstancePublicationCapacityPending;
	result[ "owner_mesh_bank_capacity_pending" ] = m_publicationBuilderView.m_bInstanceMeshBankCapacityPending;
	result[ "owner_cold_capacity_service_active" ] = m_publicationBuilderView.m_bColdCapacityServiceActive;
	result[ "rd_live_gpu_buffer_bytes" ] = (int64_t)( Terrain3DRdContext::getShared().getLiveGpuBufferBytes() );
	result[ "rd_live_gpu_texture_bytes" ] = (int64_t)( Terrain3DRdContext::getShared().getLiveGpuTextureBytes() );
	result[ "native_shadow_proxy_active" ] = false;
	result[ "render_enabled" ] = rInputs.m_bRenderEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );
	result[ "render_lifecycle_generation" ] = (int64_t)renderStatus.m_nStartupGeneration;
	result[ "render_lifecycle_stage" ] = (int64_t)renderStatus.m_eStage;
	result[ "render_lifecycle_ready_mask" ] = (int64_t)renderStatus.m_nReadyMask;
	result[ "render_lifecycle_failure_code" ] = (int64_t)renderStatus.m_nFailureCode;
	result[ "render_lifecycle_recovery_revision" ] = (int64_t)renderStatus.m_nRecoveryRevision;
	result[ "render_lifecycle_revision" ] = (int64_t)renderStatus.m_nRevision;
	result[ "forward_plus_bridge_attached" ] = rInputs.m_bForwardBridgeAttached;
	result[ "forward_plus_bridge_ready" ] = rInputs.m_forwardBridgeReady;
	result[ "uwr_p00_draw_baseline" ] = getForwardDrawBaselineDiagnostics( rInputs );
	const Terrain3DWorldResources& world_resources = rInputs.m_worldRuntime.getResources();
	const Terrain3DWorldHiZ& world_hiz = rInputs.m_worldRuntime.getHiZ();
	const Terrain3DWorldVisibilityGpu& world_visibility = rInputs.m_worldRuntime.getVisibility();
	const Terrain3DWorldRaster& world_raster = rInputs.m_worldRuntime.getRaster();
	Dictionary world_renderer;
	world_renderer[ "ready" ] = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_WORLD ) != 0u;
	world_renderer[ "publication_generation" ] = (int64_t)renderStatus.m_nPreparedWorldPublicationGeneration;
	world_renderer[ "prepare_count" ] = (int64_t)rInputs.m_worldRuntime.getPrepareCount();
	world_renderer[ "upload_count" ] = (int64_t)rInputs.m_worldRuntime.getUploadCount();
	world_renderer[ "prepare_failures" ] = (int64_t)rInputs.m_worldRuntime.getPrepareFailureCount();
	world_renderer[ "visibility_prepares" ] = (int64_t)rInputs.m_worldRuntime.getVisibilityPrepareCount();
	world_renderer[ "gpu_buffer_bytes" ] = (int64_t)world_resources.getGpuBufferBytes();
	world_renderer[ "upload_bytes" ] = (int64_t)world_resources.getUploadBytes();
	world_renderer[ "skipped_publication_upload_bytes" ] = (int64_t)world_resources.getSkippedPublicationUploadBytes();
	world_renderer[ "resource_generation" ] = (int64_t)world_resources.getResourceGeneration();
	world_renderer[ "hiz_width" ] = (int64_t)world_hiz.getWidth();
	world_renderer[ "hiz_height" ] = (int64_t)world_hiz.getHeight();
	world_renderer[ "hiz_mips" ] = (int64_t)world_hiz.getMipCount();
	world_renderer[ "hiz_builds" ] = (int64_t)world_hiz.getBuildCount();
	world_renderer[ "hiz_build_dispatches" ] = (int64_t)world_hiz.getBuildDispatches();
	world_renderer[ "hiz_source_rebinds" ] = (int64_t)world_hiz.getSourceRebinds();
	world_renderer[ "hiz_unavailable_frames" ] = (int64_t)world_hiz.getUnavailableFrames();
	world_renderer[ "hiz_invalid_history_frames" ] = (int64_t)rInputs.m_worldRuntime.getHiZInvalidHistoryFrames();
	world_renderer[ "hiz_gpu_bytes" ] = (int64_t)world_hiz.getGpuBytes();
	world_renderer[ "publication_upload_budget_bytes" ] = (int64_t)rInputs.m_worldRuntime.getPublicationUploadBudgetBytes();
	world_renderer[ "publication_cluster_budget" ] = (int64_t)rInputs.m_worldRuntime.getPublicationClusterBudget();
	world_renderer[ "publication_budget_deferrals" ] = (int64_t)rInputs.m_worldRuntime.getPublicationBudgetDeferralCount();
	world_renderer[ "publication_boundary_deferrals" ] = (int64_t)rInputs.m_worldRuntime.getPublicationBoundaryDeferralCount();
	world_renderer[ "shipping_unified_authority" ] = !rInputs.m_editorAuthoring;
	world_renderer[ "visibility_dispatches" ] = (int64_t)world_visibility.getDispatchCount();
	world_renderer[ "visibility_prepared_frames" ] = (int64_t)world_visibility.getPreparedFrameCount();
	world_renderer[ "visibility_set_rebuilds" ] = (int64_t)world_visibility.getSetRebuildCount();
	world_renderer[ "visibility_failures" ] = (int64_t)world_visibility.getFailureCount();
	world_renderer[ "visibility_candidates" ] = (int64_t)world_visibility.getLastCandidateCount();
	world_renderer[ "raster_prepares" ] = (int64_t)world_raster.getPrepareCount();
	world_renderer[ "draw_calls_depth" ] = (int64_t)world_raster.getSubmitCount( T3D_RD_FORWARD_PASS_DEPTH );
	world_renderer[ "draw_calls_opaque" ] = (int64_t)world_raster.getSubmitCount( T3D_RD_FORWARD_PASS_OPAQUE );
	world_renderer[ "draw_calls_motion" ] = (int64_t)world_raster.getSubmitCount( T3D_RD_FORWARD_PASS_MOTION );
	world_renderer[ "draw_calls_shadow" ] = (int64_t)world_raster.getSubmitCount( T3D_RD_FORWARD_PASS_SHADOW );
	result[ "world_renderer" ] = world_renderer;
	result[ "forward_pass_callbacks" ] = (int64_t)( m_forwardPassPreparerView.m_nForwardPassCallbacks );
	result[ "depth_pass_callbacks" ] = (int64_t)( m_forwardPassPreparerView.m_nDepthPassCallbacks );
	result[ "opaque_pass_callbacks" ] = (int64_t)( m_forwardPassPreparerView.m_nOpaquePassCallbacks );
	result[ "shadow_pass_callbacks" ] = (int64_t)( m_forwardPassPreparerView.m_nShadowPassCallbacks );
	result[ "motion_pass_callbacks" ] = (int64_t)( m_forwardPassPreparerView.m_nMotionPassCallbacks );
	result[ "transparent_pass_callbacks" ] = (int64_t)( m_forwardPassPreparerView.m_nTransparentPassCallbacks );
	result[ "terrain_recoverable_prepare_failures" ] =
	    (int64_t)( m_forwardPassPreparerView.m_nTerrainRecoverablePrepareFailures );
	result[ "instance_forward_prepare_failures" ] = (int64_t)( m_forwardPassPreparerView.m_nInstanceForwardPrepareFailures );
	result[ "instance_forward_submit_failures" ] = (int64_t)( m_forwardPassPreparerView.m_nInstanceForwardSubmitFailures );
	result[ "instance_initialization_attempts" ] = (int64_t)( rInputs.m_nInstanceInitializationAttempts );
	result[ "instance_initialization_failures" ] = (int64_t)( rInputs.m_nInstanceInitializationFailures );
	result[ "instance_initialization_recoveries" ] = (int64_t)( rInputs.m_nInstanceInitializationRecoveries );
	result[ "instance_depth_prepare_attempts" ] = (int64_t)( m_forwardPassPreparerView.m_nInstanceDepthPrepareAttempts );
	result[ "instance_depth_prepare_successes" ] = (int64_t)( m_forwardPassPreparerView.m_nInstanceDepthPrepareSuccesses );
	result[ "instance_depth_submit_attempts" ] = (int64_t)( m_forwardPassPreparerView.m_nInstanceDepthSubmitAttempts );
	result[ "instance_depth_submit_successes" ] = (int64_t)( m_forwardPassPreparerView.m_nInstanceDepthSubmitSuccesses );
	result[ "instance_opaque_prepare_attempts" ] = (int64_t)( m_forwardPassPreparerView.m_nInstanceOpaquePrepareAttempts );
	result[ "instance_opaque_prepare_successes" ] = (int64_t)( m_forwardPassPreparerView.m_nInstanceOpaquePrepareSuccesses );
	result[ "instance_opaque_submit_attempts" ] = (int64_t)( m_forwardPassPreparerView.m_nInstanceOpaqueSubmitAttempts );
	result[ "instance_opaque_submit_successes" ] = (int64_t)( m_forwardPassPreparerView.m_nInstanceOpaqueSubmitSuccesses );
	result[ "instance_depth_prepass_fallbacks" ] = (int64_t)( m_forwardPassPreparerView.m_nInstanceDepthPrepassFallbacks );
	result[ "instance_opaque_depth_prepass_requested_prepares" ] =
	    (int64_t)( m_forwardPassPreparerView.m_nInstanceOpaqueDepthPrepassRequested );
	result[ "instance_opaque_depth_prepass_used_prepares" ] =
	    (int64_t)( m_forwardPassPreparerView.m_nInstanceOpaqueDepthPrepassUsed );
	result[ "instance_last_successful_depth_scene_pass" ] =
	    m_forwardPassPreparerView.m_nInstanceLastSuccessfulDepthScenePass == UINT64_MAX
	        ? (int64_t)-1
	        : (int64_t)( m_forwardPassPreparerView.m_nInstanceLastSuccessfulDepthScenePass );
	result[ "instance_forward_prepared_depth_prepass_active" ] =
	    m_forwardPassPreparerView.m_bForwardPreparedInstanceDepthPrepassActive;
	result[ "instance_forward_last_error" ] = rInputs.m_instanceRenderer.getLastError();
	result[ "last_forward_scene_pass" ] = (int64_t)( m_forwardPassPreparerView.m_nLastForwardScenePass );
	result[ "last_forward_draw_list" ] = m_forwardPassPreparerView.m_nLastForwardDrawList;
	result[ "last_forward_pass_kind" ] = (int64_t)( m_forwardPassPreparerView.m_nLastForwardPassKind );
	result[ "current_forward_view_count" ] = (int64_t)( m_forwardPassPreparerView.m_nLastForwardViewCount );
	result[ "current_forward_sample_count" ] = (int64_t)( m_forwardPassPreparerView.m_nLastForwardSampleCount );
	result[ "current_forward_framebuffer_format" ] = m_forwardPassPreparerView.m_nLastForwardFramebufferFormat;
	result[ "native_depth_prepass_participation" ] = m_forwardPassPreparerView.m_nDepthPassCallbacks > 0u;
	result[ "native_opaque_participation" ] = m_forwardPassPreparerView.m_nOpaquePassCallbacks > 0u;
	result[ "native_shadow_participation" ] = m_forwardPassPreparerView.m_nShadowPassCallbacks > 0u;
	result[ "native_motion_vectors_active" ] = m_forwardPassPreparerView.m_nMotionPassCallbacks > 0u;
	result[ "native_transparent_participation" ] = m_forwardPassPreparerView.m_nTransparentPassCallbacks > 0u;
	result.merge( rInputs.m_oceanRenderer.getDiagnostics(), true );
	result[ "forward_viewport_reconfigurations" ] = (int64_t)( m_forwardPassPreparerView.m_nForwardViewportReconfigurations );
	result[ "forward_viewport_evictions" ] = (int64_t)( m_forwardPassPreparerView.m_nForwardViewportEvictions );
	result[ "forward_prepared_viewport_key" ] = (int64_t)( m_forwardPassPreparerView.m_nForwardPreparedViewportKey );
	result[ "motion_debug_enabled" ] = rInputs.m_bMotionDebugEnabled;
	result[ "compositor_debug_lane" ] = rInputs.m_bCompositorDebugEnabled;
	Terrain3DRdForwardPlusBridgeAdapter *const pForwardBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
	result[ "forward_plus_bridge" ] = pForwardBridge ? pForwardBridge->getDiagnostics() : Dictionary();
	result[ "forward_plus_bridge_abi" ] = pForwardBridge ? (int64_t)( pForwardBridge->getObservedAbiVersion() ) : 0;
	result[ "forward_plus_callback_registered" ] = pForwardBridge && pForwardBridge->isCallbackRegistered();
	result[ "shared_vt_storage_ready" ] = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_VT_STORAGE ) != 0u;
	result[ "shared_vt_resources_ready" ] = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_VT_PUBLISHED ) != 0u;
	result[ "virtual_texture_initialized" ] = rInputs.m_virtualTexture.is_initialized();
	result[ "virtual_texture_initialize_count" ] = (int64_t)( rInputs.m_virtualTexture.get_stats().nInitializeCount );
	result[ "virtual_texture_database_kind" ] = rInputs.m_virtualTexture.getDatabaseKind() == T3D_VT_RUNTIME_DATABASE_PACK_V3
	                                                ? String( "shipping-v3" )
	                                                : String( "neutral" );
	result[ "virtual_texture_database_generation" ] = (int64_t)( rInputs.m_virtualTexture.getDatabaseGeneration() );
	result[ "virtual_texture_package_generation" ] = (int64_t)( rInputs.m_virtualTexture.getPackageGeneration() );
	result[ "virtual_texture_publication_generation" ] = (int64_t)rInputs.m_virtualTexture.getPackagePublicationGeneration();
	result[ "virtual_texture_active_allocation_count" ] = (int64_t)( rInputs.m_virtualTexture.getActiveAllocationCount() );
	result[ "virtual_texture_active_virtual_page_count" ] = (int64_t)( rInputs.m_virtualTexture.getActiveVirtualPageCount() );
	result[ "virtual_texture_package_content_signature" ] =
	    String::num_uint64( rInputs.m_virtualTexture.getPackageContentSignature(), 16 );
	result[ "vt_pack_version" ] = (int64_t)( rInputs.m_virtualTexture.getDatabaseKind() == T3D_VT_RUNTIME_DATABASE_PACK_V3 ? T3D_VT_FILE_VERSION_V3 : 0u );
	result[ "vt_requested_profile" ] = (int64_t)rInputs.m_virtualTexture.get_requested_quality_profile();
	result[ "vt_package_fully_supported_profile_mask" ] = (int64_t)rInputs.m_virtualTexture.get_package_fully_supported_profile_mask();
	result[ "vt_quality_fallback_allocations" ] = (int64_t)rInputs.m_virtualTexture.get_quality_fallback_allocation_count();
	result[ "vt_quality_generation" ] = (int64_t)rInputs.m_virtualTexture.get_quality_generation();
	result[ "vt_quality_demand_resets" ] = (int64_t)rInputs.m_virtualTexture.get_stats().nQualityDemandResetCount;
	Array vtHighestProfiles;
	Array vtResolvedProfiles;
	for ( uint32_t iProfile = 0u; iProfile < T3D_VT_QUALITY_PROFILE_COUNT; ++iProfile ) {
		vtHighestProfiles.push_back( (int64_t)rInputs.m_virtualTexture.get_allocations_by_highest_profile( iProfile ) );
		vtResolvedProfiles.push_back( (int64_t)rInputs.m_virtualTexture.get_allocations_by_resolved_profile( iProfile ) );
	}
	result[ "vt_allocations_by_highest_profile" ] = vtHighestProfiles;
	result[ "vt_allocations_by_resolved_profile" ] = vtResolvedProfiles;
	const Dictionary previewDiagnostics = m_previewServiceView.m_previewRenderer.getDiagnostics();
	result[ "preview_virtual_texture_attached" ] = previewDiagnostics.get( "virtual_texture_attached", false );
	result[ "preview_virtual_texture_initialized" ] = previewDiagnostics.get( "virtual_texture_initialized", false );
	{
		Terrain3DScopedLock preview_vt_lock( m_previewServiceView.m_previewVtMutex );
		result[ "preview_vt_authoring_package_valid" ] = m_previewServiceView.m_bPreviewVtPackageValid;
		result[ "preview_vt_authoring_fingerprint" ] = String::num_uint64( m_previewServiceView.m_nPreviewVtFingerprint, 16 );
		result[ "preview_vt_authoring_package_generation" ] = (int64_t)( m_previewServiceView.m_nPreviewVtPackageGeneration );
		result[ "preview_vt_authoring_published_fingerprint" ] = String::num_uint64( m_previewServiceView.m_nPreviewVtPublishedFingerprint, 16 );
		result[ "preview_vt_authoring_published_package_generation" ] = (int64_t)( m_previewServiceView.m_nPreviewVtPublishedPackageGeneration );
		result[ "preview_vt_republish_pending" ] = m_previewServiceView.m_bPreviewVtRepublishPending;
		result[ "preview_vt_republish_queued" ] = m_previewServiceView.m_bPreviewVtRepublishQueued;
		result[ "preview_vt_republish_in_progress" ] = m_previewServiceView.m_bPreviewVtRepublishInProgress;
		result[ "preview_vt_republish_retry_count" ] = (int64_t)( m_previewServiceView.m_nPreviewVtRepublishRetryCount );
		result[ "preview_vt_republish_retry_after_usec" ] = (int64_t)( m_previewServiceView.m_nPreviewVtRepublishRetryAfterUsec );
		result[ "preview_vt_authoring_stage_neutral" ] = m_previewServiceView.m_bPreviewVtStageNeutral;
		result[ "preview_vt_authoring_published_neutral" ] = m_previewServiceView.m_bPreviewVtPublishedNeutral;
		result[ "preview_vt_material_source_generation" ] = (int64_t)( m_previewServiceView.m_nPreviewVtObservedMaterialSourceGeneration );
		result[ "preview_vt_staged_material_source_generation" ] = (int64_t)( m_previewServiceView.m_nPreviewVtStagedMaterialSourceGeneration );
		result[ "preview_vt_failed_material_source_generation" ] = (int64_t)( m_previewServiceView.m_nPreviewVtFailedMaterialSourceGeneration );
		result[ "preview_vt_authoring_recooks" ] = (int64_t)( m_previewServiceView.m_nPreviewVtRecookCount );
		result[ "preview_vt_authoring_publications" ] = (int64_t)( m_previewServiceView.m_nPreviewVtPublishCount );
		result[ "preview_vt_republish_successes" ] = (int64_t)( m_previewServiceView.m_nPreviewVtRepublishSuccessCount );
		result[ "preview_vt_republish_failures" ] = (int64_t)( m_previewServiceView.m_nPreviewVtRepublishFailureCount );
		result[ "preview_vt_republish_last_error" ] = m_previewServiceView.m_previewVtLastRepublishError;
		result[ "preview_vt_authoring_cook_failures" ] = (int64_t)( m_previewServiceView.m_nPreviewVtCookFailureCount );
		result[ "preview_vt_authoring_failed_fingerprint" ] = String::num_uint64( m_previewServiceView.m_nPreviewVtFailedFingerprint, 16 );
		result[ "preview_vt_authoring_last_cook_error" ] = m_previewServiceView.m_previewVtLastCookError;
		result[ "preview_vt_authoring_package_bytes" ] = (int64_t)( m_previewServiceView.m_previewVtPackageBytes.size() );
		result[ "vt_editor_dirty_allocations" ] = (int64_t)( m_previewServiceView.m_previewVtEditorTelemetry.m_nDirtyAllocations );
		result[ "vt_editor_cache_hits" ] = (int64_t)( m_previewServiceView.m_previewVtEditorTelemetry.m_nAllocationCacheHits + m_previewServiceView.m_previewVtEditorTelemetry.m_nChannelCacheHits );
		result[ "vt_editor_cache_misses" ] = (int64_t)( m_previewServiceView.m_previewVtEditorTelemetry.m_nAllocationCacheMisses + m_previewServiceView.m_previewVtEditorTelemetry.m_nChannelCacheMisses );
		result[ "vt_editor_allocation_cache_hits" ] = (int64_t)( m_previewServiceView.m_previewVtEditorTelemetry.m_nAllocationCacheHits );
		result[ "vt_editor_allocation_cache_misses" ] = (int64_t)( m_previewServiceView.m_previewVtEditorTelemetry.m_nAllocationCacheMisses );
		result[ "vt_editor_channel_cache_hits" ] = (int64_t)( m_previewServiceView.m_previewVtEditorTelemetry.m_nChannelCacheHits );
		result[ "vt_editor_channel_cache_misses" ] = (int64_t)( m_previewServiceView.m_previewVtEditorTelemetry.m_nChannelCacheMisses );
		result[ "vt_editor_cook_stage" ] = (int64_t)( m_previewServiceView.m_previewVtEditorTelemetry.m_nStage );
		result[ "vt_editor_cook_ms_extract" ] = (double)m_previewServiceView.m_previewVtEditorTelemetry.m_nExtractUsec / 1000.0;
		result[ "vt_editor_cook_ms_filter" ] = (double)m_previewServiceView.m_previewVtEditorTelemetry.m_nFilterUsec / 1000.0;
		result[ "vt_editor_cook_ms_page" ] = (double)m_previewServiceView.m_previewVtEditorTelemetry.m_nPageUsec / 1000.0;
		result[ "vt_editor_cook_ms_encode" ] = (double)m_previewServiceView.m_previewVtEditorTelemetry.m_nEncodeUsec / 1000.0;
		result[ "vt_editor_cook_ms_pack" ] = (double)m_previewServiceView.m_previewVtEditorTelemetry.m_nPackUsec / 1000.0;
		result[ "shared_resource_class_transitions" ] = (int64_t)( m_previewServiceView.m_nSharedResourceClassTransitionCount );
	}
	result[ "shared_resource_class" ] = (int64_t)( rInputs.m_sharedVtRuntime.get_shared_resource_class() );
	{
		Terrain3DScopedLock preview_state_lock( m_previewServiceView.m_previewStateMutex );
		result[ "preview_async_scheduled" ] = (int64_t)( m_previewServiceView.m_nPreviewAsyncScheduledCount );
		result[ "preview_async_completed" ] = (int64_t)( m_previewServiceView.m_nPreviewAsyncCompletedCount );
		result[ "preview_async_late_callbacks" ] = (int64_t)( m_previewServiceView.m_nPreviewAsyncLateCallbackCount );
		result[ "preview_async_timeouts" ] = (int64_t)( m_previewServiceView.m_nPreviewAsyncTimeoutCount );
		result[ "preview_async_lifecycle_generation" ] = (int64_t)( m_previewServiceView.m_nPreviewAsyncLifecycleGeneration );
	}
	result[ "virtual_texture_cpu_image_bytes" ] = (int64_t)( rInputs.m_virtualTexture.get_cpu_image_bytes() );
	result[ "virtual_texture_physical_page_set_capacity" ] =
	    (int64_t)( rInputs.m_virtualTexture.get_stats().nPhysicalPageSetCapacity );
	result[ "virtual_texture_physical_cache_allocations" ] =
	    (int64_t)( rInputs.m_virtualTexture.get_stats().nPhysicalCacheAllocationEvents );
	result[ "virtual_texture_physical_cache_allocations_after_seal" ] =
	    (int64_t)( rInputs.m_virtualTexture.get_stats().nPhysicalCacheAllocationEventsAfterSeal );
	t3dVTFinalTelemetry_t VtTelemetry = rInputs.m_virtualTexture.get_final_telemetry_reference_debug();
	VtTelemetry.nDescriptorBindsInstancerPass = rInputs.m_instanceRenderer.getVtDescriptorBindsInstancerPass();
	VtTelemetry.nDescriptorRebuildsRuntime = rInputs.m_instanceRenderer.getVtDescriptorRebuildsRuntime();
	result[ "vt_allocations" ] = (int64_t)( VtTelemetry.nAllocations );
	result[ "vt_virtual_pages" ] = (int64_t)( VtTelemetry.nVirtualPages );
	result[ "vt_physical_capacity" ] = (int64_t)( VtTelemetry.nPhysicalCapacity );
	result[ "vt_physical_resident" ] = (int64_t)( VtTelemetry.nPhysicalResident );
	result[ "vt_physical_free" ] = (int64_t)( VtTelemetry.nPhysicalFree );
	result[ "vt_physical_retiring" ] = (int64_t)( VtTelemetry.nPhysicalRetiring );
	result[ "vt_request_capacity" ] = (int64_t)( VtTelemetry.nRequestCapacity );
	result[ "vt_request_attempts" ] = (int64_t)( VtTelemetry.nRequestAttempts );
	result[ "vt_request_unique" ] = (int64_t)( VtTelemetry.nRequestUnique );
	result[ "vt_request_same_key" ] = (int64_t)( VtTelemetry.nRequestSameKey );
	result[ "vt_request_hash_overflow" ] = (int64_t)( VtTelemetry.nRequestHashOverflow );
	result[ "vt_request_list_overflow" ] = (int64_t)( VtTelemetry.nRequestListOverflow );
	result[ "vt_request_dropped" ] = (int64_t)( VtTelemetry.nRequestDropped );
	result[ "vt_page_hits" ] = (int64_t)( VtTelemetry.nPageHits );
	result[ "vt_page_misses" ] = (int64_t)( VtTelemetry.nPageMisses );
	result[ "vt_parent_fallback_hits" ] = (int64_t)( VtTelemetry.nParentFallbackHits );
	result[ "vt_tail_fallback_hits" ] = (int64_t)( VtTelemetry.nTailFallbackHits );
	result[ "vt_resolver_depth_0_samples" ] = (int64_t)( VtTelemetry.nResolverDepth0Samples );
	result[ "vt_resolver_depth_1_samples" ] = (int64_t)( VtTelemetry.nResolverDepth1Samples );
	result[ "vt_resolver_depth_2_3_samples" ] = (int64_t)( VtTelemetry.nResolverDepth2To3Samples );
	result[ "vt_resolver_depth_4_plus_samples" ] = (int64_t)( VtTelemetry.nResolverDepth4PlusSamples );
	result[ "vt_resolver_tail_samples" ] = (int64_t)( VtTelemetry.nResolverTailSamples );
	const uint64_t nVtPageLookups = VtTelemetry.nPageHits + VtTelemetry.nPageMisses;
	const uint64_t nVtResolverSamples = VtTelemetry.nResolverDepth0Samples + VtTelemetry.nResolverDepth1Samples + VtTelemetry.nResolverDepth2To3Samples + VtTelemetry.nResolverDepth4PlusSamples + VtTelemetry.nResolverTailSamples;
	const uint64_t nVtResolverParentSamples = VtTelemetry.nResolverDepth1Samples + VtTelemetry.nResolverDepth2To3Samples + VtTelemetry.nResolverDepth4PlusSamples;
	result[ "vt_page_miss_ratio" ] = nVtPageLookups != 0u ? (double)VtTelemetry.nPageMisses / (double)nVtPageLookups : 0.0;
	result[ "vt_resolver_parent_fallback_ratio" ] = nVtResolverSamples != 0u
	                                                    ? (double)nVtResolverParentSamples / (double)nVtResolverSamples
	                                                    : 0.0;
	result[ "vt_resolver_tail_fallback_ratio" ] = nVtResolverSamples != 0u
	                                                  ? (double)VtTelemetry.nResolverTailSamples / (double)nVtResolverSamples
	                                                  : 0.0;
	result[ "vt_physical_occupancy_ratio" ] = VtTelemetry.nPhysicalCapacity != 0u
	                                              ? (double)VtTelemetry.nPhysicalResident / (double)VtTelemetry.nPhysicalCapacity
	                                              : 0.0;
	result[ "vt_request_saturation_ratio" ] = VtTelemetry.nRequestCapacity != 0u
	                                              ? (double)VtTelemetry.nRequestUnique / (double)VtTelemetry.nRequestCapacity
	                                              : 0.0;
	result[ "vt_io_jobs_pending" ] = (int64_t)( VtTelemetry.nIoJobsPending );
	result[ "vt_io_bytes_total" ] = (int64_t)( VtTelemetry.nIoBytesTotal );
	result[ "vt_io_bytes_per_second" ] = (int64_t)( VtTelemetry.nIoBytesPerSecond );
	result[ "vt_upload_page_sets_frame" ] = (int64_t)( VtTelemetry.nUploadPageSetsFrame );
	result[ "vt_upload_bytes_frame" ] = (int64_t)( VtTelemetry.nUploadBytesFrame );
	result[ "vt_upload_bytes_total" ] = (int64_t)( VtTelemetry.nUploadBytesTotal );
	result[ "vt_evictions_frame" ] = (int64_t)( VtTelemetry.nEvictionsFrame );
	result[ "vt_evictions_total" ] = (int64_t)( VtTelemetry.nEvictionsTotal );
	result[ "vt_thrash_count" ] = (int64_t)( VtTelemetry.nThrashCount );
	result[ "vt_resident_terrain" ] = (int64_t)( VtTelemetry.nResidentTerrain );
	result[ "vt_resident_clutter" ] = (int64_t)( VtTelemetry.nResidentClutter );
	result[ "vt_resident_ground_cover" ] = (int64_t)( VtTelemetry.nResidentGroundCover );
	result[ "vt_resident_occluder" ] = (int64_t)( VtTelemetry.nResidentOccluder );
	result[ "vt_resident_hlod" ] = (int64_t)( VtTelemetry.nResidentHlod );
	result[ "vt_descriptor_binds_instancer_pass" ] = (int64_t)( VtTelemetry.nDescriptorBindsInstancerPass );
	result[ "vt_descriptor_rebuilds_runtime" ] = (int64_t)( VtTelemetry.nDescriptorRebuildsRuntime );
	result[ "vt_gpu_resource_creates_after_seal" ] = (int64_t)( VtTelemetry.nGpuResourceCreatesAfterSeal );
	result[ "instance_submissions_opaque" ] =
	    (int64_t)( rInputs.m_instanceRenderer.getForwardSubmittedDrawCount( T3D_RD_FORWARD_PASS_OPAQUE ) );
	result[ "instance_submissions_depth" ] =
	    (int64_t)( rInputs.m_instanceRenderer.getForwardSubmittedDrawCount( T3D_RD_FORWARD_PASS_DEPTH ) );
	result[ "instance_submissions_motion" ] =
	    (int64_t)( rInputs.m_instanceRenderer.getForwardSubmittedDrawCount( T3D_RD_FORWARD_PASS_MOTION ) );
	result[ "instance_submissions_shadow" ] =
	    (int64_t)( rInputs.m_instanceRenderer.getForwardSubmittedDrawCount( T3D_RD_FORWARD_PASS_SHADOW ) );
	const t3dVTResidencyState_t& VtResidency = rInputs.m_virtualTexture.get_residency();
	result[ "virtual_texture_residency_thrash" ] = (int64_t)( VtResidency.Stats.nThrashCount );
	result[ "virtual_texture_residency_reservation_failures" ] = (int64_t)( VtResidency.Stats.nReservationFailureCount );
	result[ "virtual_texture_residency_evictions" ] = (int64_t)( VtResidency.Stats.nEvictionCount );
	result[ "virtual_texture_residency_retire_deferred" ] = (int64_t)( VtResidency.Stats.nRetireDeferredCount );
	result[ "virtual_texture_residency_hot_scans" ] = (int64_t)( VtResidency.Stats.nHotScanCount );
	result[ "virtual_texture_residency_warm_scans" ] = (int64_t)( VtResidency.Stats.nWarmScanCount );
	result[ "virtual_texture_residency_cold_scans" ] = (int64_t)( VtResidency.Stats.nColdScanCount );
	result[ "virtual_texture_residency_pinned_protections" ] = (int64_t)( VtResidency.Stats.nPinnedProtectionCount );
	result[ "virtual_texture_residency_thrash_window_frames" ] = (int64_t)( VtResidency.Policy.nThrashWindowFrames );
	result[ "virtual_texture_residency_retire_delay_frames" ] = (int64_t)( VtResidency.Policy.nRetireDelayFrames );
	const t3dVTStreamingState_t& VtStreaming = rInputs.m_virtualTexture.get_streaming();
	result[ "virtual_texture_stream_upload_bytes_budget" ] = (int64_t)( VtStreaming.Limits.nMaxUploadBytesPerFrame );
	result[ "virtual_texture_stream_page_sets_budget" ] = (int64_t)( VtStreaming.Limits.nMaxPageSetsPerFrame );
	result[ "virtual_texture_stream_in_flight_io_budget" ] = (int64_t)( VtStreaming.Limits.nMaxInFlightIoJobs );
	result[ "virtual_texture_stream_admission_budget" ] = (int64_t)( VtStreaming.Limits.nMaxAdmittedRequestsPerFrame );
	result[ "virtual_texture_stream_active_jobs" ] = (int64_t)( VtStreaming.Stats.nActiveJobCount );
	result[ "virtual_texture_stream_in_flight_io_jobs" ] = (int64_t)( VtStreaming.nInFlightIoJobCount );
	result[ "virtual_texture_stream_in_flight_io_high_water" ] = (int64_t)( VtStreaming.Stats.nInFlightIoHighWater );
	result[ "virtual_texture_stream_admission_budget_deferrals" ] = (int64_t)( VtStreaming.Stats.nAdmissionBudgetDeferredCount );
	result[ "virtual_texture_stream_io_budget_deferrals" ] = (int64_t)( VtStreaming.Stats.nIoBudgetDeferredCount );
	result[ "virtual_texture_stream_page_set_budget_deferrals" ] = (int64_t)( VtStreaming.Stats.nPageSetBudgetDeferredCount );
	result[ "virtual_texture_stream_upload_byte_budget_deferrals" ] = (int64_t)( VtStreaming.Stats.nUploadByteBudgetDeferredCount );
	result[ "virtual_texture_stream_oversized_reads" ] = (int64_t)( VtStreaming.Stats.nOversizedReadCount );
	result[ "virtual_texture_stream_stale_completions" ] = (int64_t)( VtStreaming.Stats.nStaleCompletionCount );
	result[ "virtual_texture_stream_decode_failures" ] = (int64_t)( VtStreaming.Stats.nDecodeFailureCount );
	result[ "virtual_texture_stream_uploaded_bytes" ] = (int64_t)( VtStreaming.Stats.nUploadedBytes );
	result[ "virtual_texture_scheduler_duplicate_merges" ] = (int64_t)( VtStreaming.Stats.nDuplicateMergeCount );
	result[ "virtual_texture_scheduler_resident_request_merges" ] = (int64_t)( VtStreaming.Stats.nResidentRequestCount );
	result[ "virtual_texture_scheduler_priority_promotions" ] = (int64_t)( VtStreaming.Stats.nPriorityPromotionCount );
	result[ "virtual_texture_scheduler_stale_readbacks" ] = (int64_t)( VtStreaming.Stats.nStaleReadbackCount );
	result[ "virtual_texture_scheduler_invalid_readbacks" ] = (int64_t)( VtStreaming.Stats.nInvalidReadbackCount );
	static const char *s_szPriorityBucketNames[ T3D_VT_PRIORITY_BUCKET_COUNT ] = {
		"critical", "near", "normal", "prefetch", "transition", "background"
	};
	for ( uint32_t iBucket = 0u; iBucket < T3D_VT_PRIORITY_BUCKET_COUNT; ++iBucket ) {
		const String Prefix = String( "virtual_texture_scheduler_" ) + s_szPriorityBucketNames[ iBucket ];
		result[ Prefix + String( "_queued" ) ] = (int64_t)( VtStreaming.nPriorityBucketCount[ iBucket ] );
		result[ Prefix + String( "_high_water" ) ] = (int64_t)( VtStreaming.Stats.nPriorityBucketHighWater[ iBucket ] );
		result[ Prefix + String( "_claims" ) ] = (int64_t)( VtStreaming.Stats.nPriorityBucketClaimCount[ iBucket ] );
	}
	const t3dVTGpuStats_t& VtGpuStats = rInputs.m_virtualTexture.get_gpu().get_stats();
	result[ "vt_prediction_attempts" ] = (int64_t)( VtGpuStats.nRequestAttempts );
	result[ "vt_fragment_faults" ] = (int64_t)( VtGpuStats.nFragmentFaultCount );
	result[ "vt_feedback_accepted_requests" ] = (int64_t)( VtGpuStats.nAcceptedRequestCount );
	result[ "vt_page_table_update_calls" ] = (int64_t)( VtGpuStats.nPageTableUpdateCalls );
	result[ "vt_page_table_update_bytes" ] = (int64_t)( VtGpuStats.nPageTableUpdateBytes );
	result[ "vt_page_table_batch_count" ] = (int64_t)( VtGpuStats.nPageTableBatchCount );
	result[ "vt_page_table_batch_entries" ] = (int64_t)( VtGpuStats.nPageTableBatchEntries );
	result[ "virtual_texture_readback_enabled" ] = rInputs.m_virtualTexture.is_readback_enabled();
	result[ "virtual_texture_readback_stale_generation" ] = (int64_t)( VtGpuStats.nReadbackStaleGenerationCount );
	result[ "virtual_texture_readback_invalid_payload" ] = (int64_t)( VtGpuStats.nReadbackInvalidPayloadCount );
	result[ "virtual_texture_readback_ring_busy" ] = (int64_t)( VtGpuStats.nReadbackRingBusyCount );
	result[ "terrain_micro_vt_sparse_v3" ] = rInputs.m_virtualTexture.supportsSparseV3();
	result[ "terrain_micro_vt_material_allocation_zero" ] = (int64_t)( rInputs.m_virtualTexture.getMaterialVTAllocationId( 0u ) );
	result[ "terrain_micro_vt_shared_resource_generation" ] =
	    (int64_t)( rInputs.m_sharedBindings.nResourceGeneration );
	result[ "terrain_micro_vt_shared_page_table_generation" ] =
	    (int64_t)( rInputs.m_sharedBindings.nVTPageTableGeneration );
	result[ "terrain_megavt_ready" ] = rInputs.m_terrainMegaVT.isReady();
	result[ "terrain_megavt_texels_per_meter" ] = (int64_t)( rInputs.m_terrainMegaVT.getTexelsPerMeter() );
	result[ "terrain_megavt_publication_generation" ] = (int64_t)( rInputs.m_terrainMegaVT.getPublicationGeneration() );
	result[ "terrain_megavt_editor_dirty_generation" ] = (int64_t)( rInputs.m_terrainMegaVT.getEditorDirtyGeneration() );
	result[ "terrain_megavt_editor_dirty_regions" ] = (int64_t)( rInputs.m_terrainMegaVT.getDirtyRegionCount() );
	result[ "terrain_megavt_stale_completion_rejects" ] = (int64_t)( rInputs.m_terrainMegaVT.getStaleCompletionRejectCount() );
	result[ "terrain_megavt_allocation_capacity" ] = (int64_t)( T3D_TERRAIN_MEGAVT_ALLOCATION_CAPACITY );
	result[ "terrain_megavt_allocation_base" ] = rInputs.m_virtualTexture.is_initialized()
	                                                 ? (int64_t)( rInputs.m_virtualTexture.getTerrainMegaVTAllocationBase() )
	                                                 : (int64_t)( T3D_VT_INVALID_ALLOCATION );
	result[ "coordinator_material_state_builds" ] = (int64_t)( rInputs.m_materialStateBuilder.getBuildCount() );
	result[ "coordinator_region_lighting_scans" ] = (int64_t)( m_publicationBuilderView.m_nRegionLightingScanCount );
	result[ "coordinator_mesh_source_builds" ] = (int64_t)( m_publicationBuilderView.m_nMeshSourceBuildCount );
	result[ "coordinator_ocean_static_state_builds" ] = (int64_t)( m_publicationBuilderView.m_nOceanStaticStateBuildCount );
	result[ "published_instance_records" ] = (int64_t)( m_publicationBuilderView.m_cachedInstanceScene.m_instances.size() );
	result[ "published_instance_cells" ] = (int64_t)( m_publicationBuilderView.m_cachedInstanceScene.m_cells.size() );
	result[ "published_instance_scene_generation" ] = (int64_t)( m_publicationBuilderView.m_cachedInstanceScene.m_nGeneration );
	result[ "published_mesh_lods" ] = (int64_t)( m_publicationBuilderView.m_cachedInstanceMeshes.m_lods.size() );
	result[ "published_mesh_surfaces" ] = (int64_t)( m_publicationBuilderView.m_cachedInstanceMeshes.m_surfaces.size() );
	result[ "published_mesh_vertex_bytes" ] = (int64_t)( m_publicationBuilderView.m_cachedInstanceMeshes.m_vertexBytes.size() );
	result[ "published_mesh_index32_bytes" ] = (int64_t)( m_publicationBuilderView.m_cachedInstanceMeshes.m_index32Bytes.size() );
	result[ "published_mesh_generation" ] = (int64_t)( m_publicationBuilderView.m_cachedInstanceMeshes.m_nGeneration );
	result[ "renderer_initialized" ] = rInputs.m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	result[ "terrain_renderer_initialized" ] = rInputs.m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	result[ "instance_renderer_initialized" ] = rInputs.m_bInstanceRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	result[ "ocean_renderer_initialized" ] = rInputs.m_bOceanRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	result[ "forward_prepared_has_terrain" ] = m_forwardPassPreparerView.m_bForwardPreparedHasTerrain;
	result[ "forward_prepared_has_instances" ] = m_forwardPassPreparerView.m_bForwardPreparedHasInstances;
	result[ "forward_prepared_has_ocean" ] = m_forwardPassPreparerView.m_bForwardPreparedHasOcean;
	result[ "forward_lane_state" ] = getForwardLaneDiagnostics( rInputs );
	result[ "renderer_failure_latched" ] = rInputs.m_bFailureLatched;
	result[ "region_map_height_texture_ready" ] = m_publicationBuilderView.m_bRegionMapHeightTextureReady;
	result[ "published_height_texture_valid" ] = m_publicationBuilderView.m_lastPublishedResources.m_hHeightTextureRd.is_valid();
	result[ "renderer_recoveries" ] = (int64_t)( rInputs.m_nRendererRecoveryCount );
	result[ "callbacks" ] = (int64_t)( rInputs.m_nCallbackCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result[ "consecutive_render_failures" ] = (int64_t)( rInputs.m_nConsecutiveRenderFailures.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	const uint64_t snapshot_token = m_publicationBuilderView.m_nSnapshotExchangeToken.load( T3D_MEMORY_ORDER_ACQUIRE );
	const uint32_t published_snapshot_index = m_publicationBuilderView.m_nPublishedSnapshotIndex.load( T3D_MEMORY_ORDER_ACQUIRE );
	const uint64_t published_snapshot_generation =
	    m_publicationBuilderView.m_nPublishedSnapshotGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	const uint64_t consumed_snapshot_generation =
	    m_publicationBuilderView.m_nConsumedSnapshotGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	result[ "snapshot_slots" ] = (int64_t)( T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT );
	result[ "snapshot_exchange_lock_free" ] = m_publicationBuilderView.m_nSnapshotExchangeToken.is_lock_free();
	result[ "snapshot_exchange_index" ] = (int64_t)( t3dRdFrameSnapshotTokenIndex( snapshot_token ) );
	result[ "snapshot_exchange_published" ] = t3dRdFrameSnapshotTokenIsPublished( snapshot_token );
	result[ "snapshot_exchange_generation" ] = (int64_t)( t3dRdFrameSnapshotTokenGeneration( snapshot_token ) );
	result[ "snapshot_published_index" ] = (int64_t)( published_snapshot_index );
	result[ "snapshot_published_generation" ] = (int64_t)( published_snapshot_generation );
	result[ "snapshot_consumed_generation" ] = (int64_t)( consumed_snapshot_generation );
	result[ "snapshot_generation_lag" ] = (int64_t)( consumed_snapshot_generation == 0u
	                                                     ? ( published_snapshot_generation == 0u ? 0u : 1u )
	                                                     : t3dRdFrameSnapshotGenerationDistance( published_snapshot_generation, consumed_snapshot_generation ) );
	result[ "snapshot_publications" ] =
	    (int64_t)( m_publicationBuilderView.m_nSnapshotPublications.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result[ "snapshot_consumes" ] = (int64_t)( m_publicationBuilderView.m_nSnapshotConsumes.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result[ "snapshot_reuses" ] = (int64_t)( m_publicationBuilderView.m_nSnapshotReuses.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result[ "snapshot_skipped_generations" ] =
	    (int64_t)( m_publicationBuilderView.m_nSkippedSnapshotGenerations.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result[ "stale_snapshots" ] = (int64_t)( m_publicationBuilderView.m_nStaleSnapshots.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result[ "renderer" ] = renderer;
	result[ "instances" ] = instances;
	result[ "shaders" ] = rInputs.m_shaderLibrary.getDiagnostics();
	result[ "frames_rendered" ] = renderer.get( "frames_rendered", 0 );
	result[ "draws_rendered" ] = (int64_t)( renderer.get( "draws_rendered", 0 ) ) + (int64_t)( instances.get( "instance_draws", 0 ) );
	result[ "static_instances" ] = instances.get( "static_instances", 0 );
	result[ "visible_instances" ] = instances.get( "visible_instances", 0 );
	result[ "instance_draw_packets" ] = instances.get( "instance_draw_packets", 0 );
	result[ "instance_depth_packets" ] = instances.get( "depth_packets", 0 );
	result[ "instance_shadow_packets" ] = instances.get( "shadow_packets", 0 );
	result[ "indirect_draws" ] = renderer.get( "indirect_draws", 0 );
	result[ "cull_dispatches" ] = renderer.get( "cull_dispatches", 0 );
	result[ "patch_uploads" ] = renderer.get( "patch_uploads", 0 );
	result[ "topology_bytes" ] = renderer.get( "topology_bytes", 0 );
	result[ "lod0_index_bits" ] = renderer.get( "lod0_index_bits", 0 );
	result[ "outer_index_bits" ] = renderer.get( "outer_index_bits", 0 );
	result[ "gpu_culling" ] = renderer.get( "gpu_culling", false );
	result[ "resource_creates" ] = renderer.get( "resource_creates", 0 );
	result[ "resource_frees" ] = renderer.get( "resource_frees", 0 );
	result[ "live_buffer_bytes" ] = renderer.get( "live_buffer_bytes", 0 );
	result[ "peak_buffer_bytes" ] = renderer.get( "peak_buffer_bytes", 0 );
	result[ "live_texture_bytes" ] = renderer.get( "live_texture_bytes", 0 );
	result[ "peak_texture_bytes" ] = renderer.get( "peak_texture_bytes", 0 );
	result[ "buffer_updates" ] = renderer.get( "buffer_updates", 0 );
	result[ "bytes_uploaded" ] = renderer.get( "bytes_uploaded", 0 );
	result[ "last_render_error" ] = renderer.get( "last_error", "" );
	{
		Terrain3DScopedLock lock( rInputs.m_diagnosticsMutex );
		result[ "initialization_error" ] = rInputs.m_initializationError;
		result[ "preview_last_error" ] = m_previewServiceView.m_previewLastError;
		result[ "preview_failure_class" ] = (int64_t)m_previewServiceView.m_lastPreviewFailure;
		result[ "preview_failure_count" ] = (int64_t)m_previewServiceView.m_nPreviewFailureCount;
	}
	result[ "native_shadow_proxy_available" ] = false;
	result[ "multiview_supported" ] = true;
	result[ "single_view_only" ] = false;
	result[ "native_surface_enabled" ] = false;
	result[ "color_write_enabled" ] = true;
	result[ "hybrid_depth_only" ] = false;
	const Dictionary forward_lighting = renderer.get( "forward_lighting", Dictionary() );
	result[ "clustered_light_path_active" ] = forward_lighting.get( "forward_light_resources_ready", false );
	result[ "borrowed_scene_resource_changes" ] = forward_lighting.get( "forward_scene_resource_changes", 0 );
	return result;
}
