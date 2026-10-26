// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "terrain_3d_forward_pass_preparer.h"

#include "terrain_3d_render_coordinator.h"
#include "core/logger.h"
#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/terrain_3d_world_render_authority.h"
#include "rendering/rd/vt/terrain_3d_vt_streaming.h"

#include <math.h>
#include <string.h>

using namespace godot;

static bool t3dForwardPassShouldReportInstanceFailure( const uint64_t nCount ) {
	return nCount == 1u || ( nCount % 120u ) == 0u;
}

static void t3dTerrainRepeatFootprint( const float fMinimum, const float fMaximum, float &rMinimum, float &rMaximum ) {
	const float fMin = fMinimum < fMaximum ? fMinimum : fMaximum;
	const float fMax = fMinimum > fMaximum ? fMinimum : fMaximum;
	if ( fMax - fMin >= 1.0f || floorf( fMin ) != floorf( fMax ) ) {
		rMinimum = 0.0f;
		rMaximum = 1.0f;
		return;
	}
	const float fBase = floorf( fMin );
	rMinimum = fMin - fBase;
	rMaximum = fMax - fBase;
}

static uint32_t t3dStageTerrainMaterialFootprintHot( Terrain3DVirtualTexture &rVirtualTexture,
        const t3dRdFrameSnapshot_t &rSnapshot,
        const t3dRdMaterialLayer_t &rLayer,
        const uint32_t nFrameId ) {
	static const uint32_t s_nMaterialRequestCap = 64u;
	uint32_t nRemaining = s_nMaterialRequestCap;
	uint32_t nAttempted = 0u;
	const float fUvScale = fabsf( rLayer.m_fUvScale );
	for ( uint32_t nLod = 0u; nLod < 16u && nRemaining > 0u; ++nLod ) {
		for ( uint32_t iPatch = 0u; iPatch < rSnapshot.m_nPatchCount && nRemaining > 0u; ++iPatch ) {
			const t3dRdPatch_t &patch = rSnapshot.m_szPatches[iPatch];
			// FULL micro-VT reconstruction is a near-tier cost. Middle/far tiers use
			// the cheaper material paths and can fall back to the coarser residency
			// already established by these near footprints.
			if ( patch.m_nTier != 0u || patch.m_nLod != nLod ) {
				continue;
			}
			const float fHalfExtent = patch.m_fBoundsHalfExtent > 0.0f ? patch.m_fBoundsHalfExtent : patch.m_fScale;
			float uvMin[2];
			float uvMax[2];
			t3dTerrainRepeatFootprint(
			        ( patch.m_fOriginX - fHalfExtent ) * fUvScale, ( patch.m_fOriginX + fHalfExtent ) * fUvScale, uvMin[0], uvMax[0] );
			t3dTerrainRepeatFootprint(
			        ( patch.m_fOriginZ - fHalfExtent ) * fUvScale, ( patch.m_fOriginZ + fHalfExtent ) * fUvScale, uvMin[1], uvMax[1] );
			const uint32_t nChannelCount = ( rLayer.m_nMicroVTFlags & 1u ) != 0u ? T3D_VT_V3_RUNTIME_CHANNELS_PER_ALLOCATION : 1u;
			for ( uint32_t iChannel = 0u; iChannel < nChannelCount && nRemaining > 0u; ++iChannel ) {
				const uint32_t nRuntimeAllocation = rLayer.m_nMicroVTAllocation + iChannel;
				if ( nRuntimeAllocation >= rVirtualTexture.getActiveAllocationCount()
				        || rVirtualTexture.get_residency().Buffers.pAllocations[nRuntimeAllocation].m_nPageCount == 0u ) {
					continue;
				}
				const uint32_t nPatchAttempts = rVirtualTexture.stage_material_footprint_hot( nRuntimeAllocation,
				        nFrameId,
				        uvMin,
				        uvMax,
				        ( uint16_t )MIN( patch.m_nLod, 0xffffu ),
				        nRemaining,
				        T3D_VT_PRIORITY_NEAR_MIN,
				        T3D_VT_REQUEST_PRODUCER_TERRAIN );
				nAttempted += nPatchAttempts;
				nRemaining -= MIN( nRemaining, nPatchAttempts );
			}
		}
	}
	if ( nAttempted == 0u ) {
		const float uvMin[2] = { 0.0f, 0.0f };
		const float uvMax[2] = { 1.0f, 1.0f };
		const uint32_t nChannelCount = ( rLayer.m_nMicroVTFlags & 1u ) != 0u ? T3D_VT_V3_RUNTIME_CHANNELS_PER_ALLOCATION : 1u;
		for ( uint32_t iChannel = 0u; iChannel < nChannelCount && nAttempted < s_nMaterialRequestCap; ++iChannel ) {
			const uint32_t nRuntimeAllocation = rLayer.m_nMicroVTAllocation + iChannel;
			if ( nRuntimeAllocation >= rVirtualTexture.getActiveAllocationCount()
			        || rVirtualTexture.get_residency().Buffers.pAllocations[nRuntimeAllocation].m_nPageCount == 0u ) {
				continue;
			}
			nAttempted += rVirtualTexture.stage_material_footprint_hot( nRuntimeAllocation,
			        nFrameId,
			        uvMin,
			        uvMax,
			        1u,
			        s_nMaterialRequestCap - nAttempted,
			        T3D_VT_PRIORITY_NORMAL_MIN,
			        T3D_VT_REQUEST_PRODUCER_TERRAIN );
		}
	}
	return nAttempted;
}

void Terrain3DForwardPassPreparer::reset( void ) {
	memset( m_szForwardDrawBaseline, 0, sizeof( m_szForwardDrawBaseline ) );
	for ( size_t iViewport = 0u; iViewport < T3D_ARRAY_COUNT( m_szForwardViewportStates ); iViewport++ ) {
		t3dRdForwardViewportState_t &state = ( m_szForwardViewportStates )[iViewport];
		state = t3dRdForwardViewportState_t{};
	}
	m_nForwardPreparedScenePass = 0u;
	m_nForwardPreparedViewportKey = 0u;
	m_bForwardPreparedHasTerrain = false;
	m_bForwardPreparedHasInstances = false;
	m_bForwardPreparedHasOcean = false;
	m_bForwardPreparedHasWorld = false;
	m_bForwardPreparedInstanceDepthPrepassActive = false;
	m_nForwardPrepareRequestedLaneMask = 0u;
	m_nForwardPrepareReadyLaneMask = 0u;
	m_nForwardPrepareFailedLaneMask = 0u;
	m_nForwardSubmitAttemptedLaneMask = 0u;
	m_nForwardSubmitSucceededLaneMask = 0u;
	m_nForwardSubmitFailedLaneMask = 0u;
	m_nForwardLaneScenePass = 0u;
	m_nForwardLanePassKind = T3D_RD_FORWARD_PASS_INVALID;
	m_nForwardPublishedInstanceCount = 0u;
	m_nForwardPublishedInstanceCellCount = 0u;
	m_bForwardInstanceLaneExpected = false;
	m_bForwardInstancePrepareAttempted = false;
	m_bForwardInstancePrepareSucceeded = false;
	m_bForwardInstancePreparedHasDraws = false;
	m_bForwardInstanceSubmitAttempted = false;
	m_bForwardInstanceSubmitSucceeded = false;
	m_nForwardInstanceSubmitDraws = 0u;
	m_nLastTerrainVtResidencyRequestFrame = UINT64_MAX;
}

bool Terrain3DForwardPassPreparer::prepareLifecycle(
        const t3dRdForwardPassContext_t &rContext, bool &rHistoryValid, bool &rViewportReconfigured ) {
	rHistoryValid = false;
	rViewportReconfigured = false;
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ) {
		return true;
	}
	if ( rContext.m_nViewportKey == 0u || rContext.m_nViewCount == 0u || rContext.m_nViewCount > s_nRdForwardMaxViews
	        || rContext.m_nSampleCount == 0u || !rContext.m_hNativeFramebuffer.is_valid()
	        || rContext.m_nFramebufferFormat == RenderingDevice::INVALID_ID ) {
		return false;
	}
	t3dRdForwardViewportState_t *slot = NULL;
	t3dRdForwardViewportState_t *oldest = &m_szForwardViewportStates[0];
	for ( size_t iViewport = 0u; iViewport < T3D_ARRAY_COUNT( m_szForwardViewportStates ); iViewport++ ) {
		t3dRdForwardViewportState_t &candidate = ( m_szForwardViewportStates )[iViewport];
		if ( candidate.m_bValid && candidate.m_nViewportKey == rContext.m_nViewportKey ) {
			slot = &candidate;
			break;
		}
		if ( !candidate.m_bValid && !slot ) {
			slot = &candidate;
		}
		if ( candidate.m_nLastRenderFrame < oldest->m_nLastRenderFrame ) {
			oldest = &candidate;
		}
	}
	if ( !slot ) {
		slot = oldest;
		++m_nForwardViewportEvictions;
	}
	// A Forward+ viewport is stable across several native pass framebuffers. Depth,
	// opaque, motion, and transparent passes may all use different framebuffer
	// RIDs/formats while still belonging to the same viewport lifecycle. Keep
	// framebuffer compatibility in the pass/pipeline keys; it must not invalidate
	// temporal history or borrowed renderer state for the viewport itself.
	const bool same = slot->m_bValid && slot->m_nViewportKey == rContext.m_nViewportKey && slot->m_renderSize == rContext.m_renderSize
	        && slot->m_renderRegion == rContext.m_renderRegion && slot->m_nViewCount == rContext.m_nViewCount
	        && slot->m_nSampleCount == rContext.m_nSampleCount;
	const bool reconfigured_this_frame = slot->m_bValid && slot->m_nReconfiguredRenderFrame == rContext.m_nRenderFrame;
	rHistoryValid = same && !reconfigured_this_frame && rContext.m_bMotionVectorsEnabled;
	rViewportReconfigured = !same;
	if ( !same ) {
		++m_nForwardViewportReconfigurations;
		slot->m_nReconfiguredRenderFrame = rContext.m_nRenderFrame;
		slot->m_nViewportKey = rContext.m_nViewportKey;
		slot->m_renderSize = rContext.m_renderSize;
		slot->m_renderRegion = rContext.m_renderRegion;
		slot->m_nViewCount = rContext.m_nViewCount;
		slot->m_nSampleCount = rContext.m_nSampleCount;
		slot->m_bValid = true;
	}
	slot->m_nLastRenderFrame = rContext.m_nRenderFrame;
	return true;
}

bool Terrain3DForwardPassPreparer::render( Terrain3DRenderCoordinator &rCoordinator, const t3dRdForwardPassContext_t &rContext ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), false );
#endif
	if ( !rCoordinator.m_bRenderEnabled.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return false;
	}
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
	++m_nForwardPassCallbacks;
	m_nLastForwardScenePass = rContext.m_nScenePass;
	m_nLastForwardDrawList = rContext.m_nDrawList;
	m_nLastForwardPassKind = ( uint32_t )( rContext.m_passKind );
	m_nLastForwardViewCount = rContext.m_nViewCount;
	m_nLastForwardSampleCount = rContext.m_nSampleCount;
	m_nLastForwardFramebufferFormat = rContext.m_nFramebufferFormat;
	switch ( rContext.m_passKind ) {
		case T3D_RD_FORWARD_PASS_DEPTH:
			++m_nDepthPassCallbacks;
			break;
		case T3D_RD_FORWARD_PASS_OPAQUE:
			++m_nOpaquePassCallbacks;
			break;
		case T3D_RD_FORWARD_PASS_SHADOW:
			++m_nShadowPassCallbacks;
			break;
		case T3D_RD_FORWARD_PASS_MOTION:
			++m_nMotionPassCallbacks;
			break;
		case T3D_RD_FORWARD_PASS_TRANSPARENT:
			++m_nTransparentPassCallbacks;
			break;
		default:
			break;
	}
#endif
	if ( rContext.m_passKind != T3D_RD_FORWARD_PASS_DEPTH && rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE
	        && rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW && rContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION
	        && rContext.m_passKind != T3D_RD_FORWARD_PASS_TRANSPARENT ) {
		return false;
	}

	if ( rContext.m_phase == T3D_RD_FORWARD_PHASE_PREPARE ) {
		m_nForwardPrepareRequestedLaneMask = 0u;
		m_nForwardPrepareReadyLaneMask = 0u;
		m_nForwardPrepareFailedLaneMask = 0u;
		m_nForwardSubmitAttemptedLaneMask = 0u;
		m_nForwardSubmitSucceededLaneMask = 0u;
		m_nForwardSubmitFailedLaneMask = 0u;
		m_nForwardLaneScenePass = rContext.m_nScenePass;
		m_nForwardLanePassKind = ( uint32_t )( rContext.m_passKind );
		m_nForwardPublishedInstanceCount = 0u;
		m_nForwardPublishedInstanceCellCount = 0u;
		m_bForwardInstanceLaneExpected = false;
		m_bForwardInstancePrepareAttempted = false;
		m_bForwardInstancePrepareSucceeded = false;
		m_bForwardInstancePreparedHasDraws = false;
		m_bForwardInstanceSubmitAttempted = false;
		m_bForwardInstanceSubmitSucceeded = false;
		m_nForwardInstanceSubmitDraws = 0u;
		bool history_valid = false;
		bool viewport_reconfigured = false;
		if ( !prepareLifecycle( rContext, history_valid, viewport_reconfigured )
		        || !rCoordinator.initializeRendererOnRenderThread( rContext.m_nRenderFrame ) ) {
			return false;
		}
		const t3dRdFrameSnapshotSlot_t *const publication = rCoordinator.acquireSnapshotOnRenderThread();
		if ( !publication ) {
			return false;
		}
		const t3dRdFrameSnapshot_t &snapshot = publication->m_snapshot;
		const t3dRdRegionMapBank_t *const pRegionMap = rCoordinator.m_publicationBuilder.getRegionMapBank( publication->m_nRegionMapBank );
		const t3dRdInstanceMeshBank_t *const pMeshes =
		        rCoordinator.m_publicationBuilder.getInstanceMeshBank( publication->m_nInstanceMeshBank );
		const t3dRdInstanceSceneBank_t *const pScene =
		        rCoordinator.m_publicationBuilder.getInstanceSceneBank( publication->m_nInstanceSceneBank );
		const t3dRdOceanStripBank_t *const pOcean =
		        rCoordinator.m_publicationBuilder.getOceanStripBank( publication->m_nOceanStripBank );
		const t3dWorldPublicationBank_t *const pWorld =
		        rCoordinator.m_publicationBuilder.getWorldPublicationBank( publication->m_nWorldPublicationBank );
		const bool world_pass_kind = rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH || rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE
		        || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
		const bool shadow_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
		const bool transparent_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
		const bool editor_authoring_mode = snapshot.m_bEditorAuthoringMode;
		const bool world_publication_available = pWorld && pWorld->m_publication.m_nGeneration != 0u;
		const t3dWorldRenderAuthorityPolicy_t world_authority = t3dResolveWorldRenderAuthority(
		        world_pass_kind, editor_authoring_mode, world_publication_available, transparent_pass );
		if ( world_authority.bShippingUnifiedRequired && !world_publication_available ) {
			++m_nStrictWorldAuthorityFailures;
			m_nForwardPrepareRequestedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
			m_nForwardPrepareFailedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
			return false;
		}
		rCoordinator.m_worldRuntime.setFramePolicyHot(
		        ( uint64_t )rCoordinator.m_nWorldUploadBudgetKbPolicy.load( T3D_MEMORY_ORDER_ACQUIRE ) * 1024u,
		        rCoordinator.m_nWorldClusterBudgetPolicy.load( T3D_MEMORY_ORDER_ACQUIRE ),
		        rCoordinator.m_bWorldBoundaryDeferredPolicy.load( T3D_MEMORY_ORDER_ACQUIRE ) );
		rCoordinator.m_virtualTexture.set_frame_budgets_hot( rCoordinator.m_nVtRequestBudgetPolicy.load( T3D_MEMORY_ORDER_ACQUIRE ),
		        rCoordinator.m_nVtAdmissionBudgetPolicy.load( T3D_MEMORY_ORDER_ACQUIRE ) );
		if ( world_publication_available ) {
			Terrain3DColdPhaseScope world_prepare_scope;
			if ( !rCoordinator.m_worldRuntime.preparePublicationCold( pWorld->m_publication, rContext.m_nRenderFrame ) ) {
				return false;
			}
		}
		const uint32_t visibility_mask =
		        shadow_pass ? ( rContext.m_nShadowCasterMask & rContext.m_nShadowLightCullMask ) : rContext.m_nCameraVisibleLayers;
		if ( world_publication_available && !transparent_pass ) {
			Terrain3DColdPhaseScope world_visibility_scope;
			if ( !rCoordinator.m_worldRuntime.prepareVisibilityPass( rContext, snapshot, visibility_mask ) ) {
				return false;
			}
		}
		const bool has_terrain = !transparent_pass && snapshot.m_nPatchCount > 0u && pRegionMap && !pRegionMap->m_bytes.is_empty()
		        && ( !shadow_pass || ( snapshot.m_bCastShadows && ( snapshot.m_nRenderLayers & visibility_mask ) != 0u ) );
		const bool unified_world_pass = world_authority.bUnifiedWorldPass;
		if ( world_authority.bEditorLegacyPass ) {
			++m_nEditorLegacyPreviewPrepares;
		}
		m_nForwardPublishedInstanceCount = pScene ? ( uint32_t )( pScene->m_snapshot.m_instances.size() ) : 0u;
		m_nForwardPublishedInstanceCellCount = pScene ? ( uint32_t )( pScene->m_snapshot.m_cells.size() ) : 0u;
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
		const bool bInstancePassEnabled = rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE;
#else
		const bool bInstancePassEnabled = true;
#endif
		m_bForwardInstanceLaneExpected = m_nForwardPublishedInstanceCount > 0u && ( unified_world_pass || editor_authoring_mode );
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
		m_bForwardInstanceLaneExpected = m_bForwardInstanceLaneExpected && bInstancePassEnabled;
#endif
		const bool shared_vt_needed = has_terrain || m_bForwardInstanceLaneExpected || unified_world_pass;
		const bool shared_vt_ready = !shared_vt_needed || rCoordinator.ensureSharedVtRuntimeCold();
		if ( has_terrain && shared_vt_ready && m_nLastTerrainVtResidencyRequestFrame != rContext.m_nRenderFrame
		        && ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) ) {
			m_nLastTerrainVtResidencyRequestFrame = rContext.m_nRenderFrame;
			const uint32_t nFrameId = ( uint32_t )rContext.m_nRenderFrame;
			const uint32_t nMaterialCount = MIN( snapshot.m_nMaterialLayerCount, ( uint32_t )s_nRdMaterialLayerCount );
			for ( uint32_t iMaterial = 0u; iMaterial < nMaterialCount; ++iMaterial ) {
				const t3dRdMaterialLayer_t &layer = snapshot.m_szMaterials[iMaterial];
				if ( ( layer.m_nFlags & T3D_RD_MATERIAL_LAYER_MICRO_VT ) == 0u
				        || layer.m_nMicroVTAllocation == T3D_VT_INVALID_ALLOCATION ) {
					continue;
				}
				m_nTerrainVtResidencyRequestAttempts +=
				        t3dStageTerrainMaterialFootprintHot( rCoordinator.m_virtualTexture, snapshot, layer, nFrameId );
			}
		}
		if ( unified_world_pass ) {
			m_nForwardPrepareRequestedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
		} else if ( editor_authoring_mode ) {
			if ( has_terrain ) {
				m_nForwardPrepareRequestedLaneMask |= T3D_RD_FORWARD_LANE_TERRAIN;
			}
			if ( m_bForwardInstanceLaneExpected ) {
				m_nForwardPrepareRequestedLaneMask |= T3D_RD_FORWARD_LANE_INSTANCES;
			}
		}
		m_nForwardPreparedScenePass = rContext.m_nScenePass;
		m_nForwardPreparedViewportKey = rContext.m_nViewportKey;
		m_bForwardPreparedHasTerrain = has_terrain;
		m_bForwardPreparedHasInstances = false;
		m_bForwardPreparedHasOcean = false;
		m_bForwardPreparedHasWorld = false;
		Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER );
		const bool instance_lane_ready = !unified_world_pass && editor_authoring_mode && shared_vt_ready && bInstancePassEnabled
		        && rCoordinator.m_bInstanceRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
		m_bForwardInstancePrepareAttempted = instance_lane_ready;
		if ( !unified_world_pass && m_bForwardInstanceLaneExpected && !instance_lane_ready ) {
			++m_nInstanceLaneUnavailablePrepares;
			m_nForwardPrepareFailedLaneMask |= T3D_RD_FORWARD_LANE_INSTANCES;
		}
		t3dRdForwardPassContext_t instance_context = rContext;
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
		if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) {
			instance_context.m_bDepthPrepassActive = false;
		}
#endif
		if ( instance_lane_ready && rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_bDepthPrepassActive ) {
			++m_nInstanceOpaqueDepthPrepassRequested;
		}
		if ( instance_lane_ready
		        && ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION )
		        && rContext.m_bDepthPrepassActive && m_nInstanceLastSuccessfulDepthScenePass != rContext.m_nScenePass ) {
			// The native renderer may have a depth prepass without Terrain3D having
			// successfully submitted its own instance depth for this scene pass. In
			// that case COMPARE_OP_EQUAL would reject every instance color fragment.
			// Fall back to the normal depth-tested color pipeline until the instance
			// depth lane has demonstrably submitted for this exact scene pass.
			instance_context.m_bDepthPrepassActive = false;
			++m_nInstanceDepthPrepassFallbacks;
			if ( !T3D_INSTANCER_MINIMAL_RENDER_PATH && t3dForwardPassShouldReportInstanceFailure( m_nInstanceDepthPrepassFallbacks ) ) {
				LOG( WARN,
				        "[RD instances] Native depth prepass active without a successful instance depth submit for scene_pass=",
				        rContext.m_nScenePass,
				        "; using depth-tested color fallback" );
			}
		}
		m_bForwardPreparedInstanceDepthPrepassActive = instance_context.m_bDepthPrepassActive;
		if ( instance_lane_ready && rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE && instance_context.m_bDepthPrepassActive ) {
			++m_nInstanceOpaqueDepthPrepassUsed;
		}
		if ( instance_lane_ready && rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH ) {
			++m_nInstanceDepthPrepareAttempts;
		} else if ( instance_lane_ready && rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ) {
			++m_nInstanceOpaquePrepareAttempts;
		}
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
		const uint32_t instance_visibility_mask = UINT32_MAX;
#else
		const uint32_t instance_visibility_mask = visibility_mask;
#endif
		const bool instances_ok = !instance_lane_ready
		        || rCoordinator.m_instanceRenderer.prepareForwardPass( instance_context,
		                pMeshes ? &pMeshes->m_snapshot : NULL,
		                pScene ? &pScene->m_snapshot : NULL,
		                instance_visibility_mask,
		                history_valid,
		                viewport_reconfigured );
		m_bForwardPreparedHasInstances = instance_lane_ready && instances_ok;
		m_bForwardInstancePrepareSucceeded = m_bForwardPreparedHasInstances;
		m_bForwardInstancePreparedHasDraws = m_bForwardPreparedHasInstances && rCoordinator.m_instanceRenderer.hasForwardPreparedDraws();
		if ( m_bForwardInstanceLaneExpected && instance_lane_ready ) {
			if ( instances_ok ) {
				m_nForwardPrepareReadyLaneMask |= T3D_RD_FORWARD_LANE_INSTANCES;
				if ( !m_bForwardInstancePreparedHasDraws ) {
					// A non-empty publication does not imply that every native pass must
					// produce a draw. Shadow/depth/transparent eligibility is pass-specific,
					// and a successful PREPARE with zero packets is therefore a valid no-op.
					// Keep the counter for diagnostics; actual prepare failures are reported
					// by the instances_ok == false path below.
					++m_nInstancePreparedWithoutDraws;
				}
			} else {
				m_nForwardPrepareFailedLaneMask |= T3D_RD_FORWARD_LANE_INSTANCES;
			}
		}
		if ( instance_lane_ready && instances_ok ) {
			if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH ) {
				++m_nInstanceDepthPrepareSuccesses;
			} else if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ) {
				++m_nInstanceOpaquePrepareSuccesses;
			}
		}
		if ( instance_lane_ready && !instances_ok ) {
			const uint64_t failures = ++m_nInstanceForwardPrepareFailures;
			if ( t3dForwardPassShouldReportInstanceFailure( failures ) ) {
				LOG( ERROR,
				        "[RD instances] Forward+ PREPARE failed; pass=",
				        ( uint32_t )( rContext.m_passKind ),
				        ", scene_pass=",
				        rContext.m_nScenePass,
				        ", records=",
				        m_nForwardPublishedInstanceCount,
				        ", cells=",
				        m_nForwardPublishedInstanceCellCount,
				        ", published_vertex_bytes=",
				        pMeshes ? pMeshes->m_snapshot.m_vertexBytes.size() : 0,
				        ", published_lods=",
				        pMeshes ? pMeshes->m_snapshot.m_lods.size() : 0u,
				        ", published_surfaces=",
				        pMeshes ? pMeshes->m_snapshot.m_surfaces.size() : 0u,
				        ", packet_input_cells=",
				        rCoordinator.m_instanceRenderer.getPacketBuildInputCellCount(),
				        ", resolved_cells=",
				        rCoordinator.m_instanceRenderer.getResolvedMeshCellCount(),
				        ", unresolved_cells=",
				        rCoordinator.m_instanceRenderer.getUnresolvedMeshCellCount(),
				        ", draw_packets=",
				        rCoordinator.m_instanceRenderer.getDrawPacketCount(),
				        ", mesh_upload_failures=",
				        rCoordinator.m_instanceRenderer.getMeshUploadFailureCount(),
				        ", texture_fallbacks=",
				        rCoordinator.m_instanceRenderer.getMeshTextureFallbackCount(),
				        ", error=",
				        rCoordinator.m_instanceRenderer.getLastError() );
			}
		}
		if ( shared_vt_ready ) {
			uint32_t nTerrainAccepted = 0u;
			uint32_t nInstanceAccepted = 0u;
			if ( rCoordinator.m_virtualTexture.flush_staged_material_requests_hot(
			             ( uint32_t )rContext.m_nRenderFrame, nTerrainAccepted, nInstanceAccepted ) ) {
				m_nTerrainVtResidencyRequestsAccepted += nTerrainAccepted;
				rCoordinator.m_instanceRenderer.recordVtResidencyAcceptedHot( nInstanceAccepted );
			}
		}

		// Terrain and instance/HLOD resources are independent. Snapshot shared VT
		// bindings after companion RenderWorld PREPARE so a companion teardown cannot
		// mutate descriptor state underneath the terrain lane in this transaction.
		T3DSharedVTBindings shared_vt_bindings = {};
		const bool world_raster_bindings_needed = has_terrain || unified_world_pass;
		const bool terrain_vt_ready = !world_raster_bindings_needed
		        || ( shared_vt_ready && rCoordinator.snapshotSharedVtBindingsHot( shared_vt_bindings ) );
		const bool terrain_ok = !world_raster_bindings_needed
		        || ( terrain_vt_ready
		                && rCoordinator.m_renderer.prepareForwardPass( rContext,
		                        snapshot,
		                        pRegionMap->m_bytes,
		                        snapshot.m_nRegionMapGeneration,
		                        publication->m_bindings,
		                        &shared_vt_bindings,
		                        history_valid,
		                        viewport_reconfigured,
		                        snapshot.m_bDominantSunOnly ) );
		bool world_ok = !unified_world_pass;
		if ( unified_world_pass && terrain_ok ) {
			t3dRdWorldTerrainRasterBindings_t world_terrain_bindings = {};
			world_ok = rCoordinator.m_renderer.snapshotWorldRasterBindingsHot( world_terrain_bindings )
			        && rCoordinator.m_worldRuntime.prepareRasterPass( rContext, world_terrain_bindings, shared_vt_bindings );
			m_bForwardPreparedHasWorld = world_ok;
		}
		if ( unified_world_pass ) {
			if ( world_ok ) {
				m_nForwardPrepareReadyLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
			} else {
				m_nForwardPrepareFailedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
			}
		}
		m_bForwardPreparedHasTerrain = !unified_world_pass && editor_authoring_mode && has_terrain;
		if ( has_terrain && !unified_world_pass && editor_authoring_mode ) {
			if ( terrain_ok ) {
				m_nForwardPrepareReadyLaneMask |= T3D_RD_FORWARD_LANE_TERRAIN;
			} else {
				m_nForwardPrepareFailedLaneMask |= T3D_RD_FORWARD_LANE_TERRAIN;
			}
		}
		const bool ocean_lane_ready = rCoordinator.m_bOceanRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
		const bool ocean_requested = snapshot.m_ocean.m_nEnabled != 0u && snapshot.m_ocean.m_nPatchCount > 0u
		        && ( transparent_pass || ( shadow_pass && snapshot.m_ocean.m_nCastShadows != 0u ) );
		if ( ocean_requested ) {
			m_nForwardPrepareRequestedLaneMask |= T3D_RD_FORWARD_LANE_OCEAN;
		}
		const bool ocean_ok = !ocean_lane_ready
		        || rCoordinator.m_oceanRenderer.prepareForwardPass(
		                rContext, snapshot, pOcean ? &pOcean->m_snapshot : NULL, visibility_mask );
		m_bForwardPreparedHasOcean = ocean_lane_ready && ocean_ok && ocean_requested;
		if ( ocean_requested ) {
			if ( m_bForwardPreparedHasOcean ) {
				m_nForwardPrepareReadyLaneMask |= T3D_RD_FORWARD_LANE_OCEAN;
			} else {
				m_nForwardPrepareFailedLaneMask |= T3D_RD_FORWARD_LANE_OCEAN;
			}
		}
		if ( terrain_ok && world_ok ) {
			rCoordinator.m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
			return true;
		}
		// PREPARE failures are lane-local and recoverable. Keep stable renderer and
		// shared VT resources alive; a subsequent PREPARE may recover after the
		// relevant bindings/pipeline generation changes. Full cold teardown is
		// reserved for actual renderer initialization/device failures.
		rCoordinator.m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
		++m_nTerrainRecoverablePrepareFailures;
		return false;
	}

	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT || !rCoordinator.m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE )
	        || m_nForwardPreparedScenePass != rContext.m_nScenePass || m_nForwardPreparedViewportKey != rContext.m_nViewportKey ) {
		return false;
	}
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER );
	t3dRdForwardDrawBaseline_t *const pDrawBaseline =
	        rContext.m_passKind > T3D_RD_FORWARD_PASS_INVALID && rContext.m_passKind < T3D_RD_FORWARD_PASS_MAX
	        ? &m_szForwardDrawBaseline[( uint32_t )( rContext.m_passKind )]
	        : NULL;
	if ( pDrawBaseline ) {
		++pDrawBaseline->m_nSubmitCallbacks;
	}
	if ( m_bForwardPreparedHasWorld
	        && ( rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH || rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE
	                || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ) ) {
		const uint64_t nWorldDrawsBefore = rCoordinator.m_worldRuntime.getRaster().getSubmitCount( rContext.m_passKind );
		const bool world_ok = rCoordinator.m_worldRuntime.submitRasterPass( rContext );
		const uint64_t nWorldDrawsAfter = rCoordinator.m_worldRuntime.getRaster().getSubmitCount( rContext.m_passKind );
		if ( pDrawBaseline && nWorldDrawsAfter >= nWorldDrawsBefore ) {
			pDrawBaseline->m_nWorldDraws += nWorldDrawsAfter - nWorldDrawsBefore;
		}
		if ( world_ok ) {
			m_nForwardSubmitAttemptedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
			m_nForwardSubmitSucceededLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
			rCoordinator.m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
			if ( rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW ) {
				return true;
			}
		}
		if ( !world_ok ) {
			m_nForwardSubmitAttemptedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
			m_nForwardSubmitFailedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
			rCoordinator.m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
			return false;
		}
	}
	bool instances_ok = true;
	if ( m_bForwardPreparedHasInstances ) {
		m_bForwardInstanceSubmitAttempted = true;
		if ( m_bForwardInstanceLaneExpected ) {
			m_nForwardSubmitAttemptedLaneMask |= T3D_RD_FORWARD_LANE_INSTANCES;
		}
		t3dRdForwardPassContext_t instance_context = rContext;
		if ( ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION )
		        && rContext.m_bDepthPrepassActive && !m_bForwardPreparedInstanceDepthPrepassActive ) {
			instance_context.m_bDepthPrepassActive = false;
		}
		if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH ) {
			++m_nInstanceDepthSubmitAttempts;
		} else if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ) {
			++m_nInstanceOpaqueSubmitAttempts;
		}
		const uint64_t submitted_draws_before = rCoordinator.m_instanceRenderer.getForwardSubmittedDrawCount( rContext.m_passKind );
		instances_ok = rCoordinator.m_instanceRenderer.renderForwardPass( instance_context );
		const uint64_t submitted_draws_after = rCoordinator.m_instanceRenderer.getForwardSubmittedDrawCount( rContext.m_passKind );
		m_nForwardInstanceSubmitDraws =
		        submitted_draws_after >= submitted_draws_before ? submitted_draws_after - submitted_draws_before : 0u;
		if ( pDrawBaseline ) {
			pDrawBaseline->m_nInstanceDraws += m_nForwardInstanceSubmitDraws;
		}
		m_bForwardInstanceSubmitSucceeded = instances_ok;
		if ( m_bForwardInstanceLaneExpected ) {
			if ( instances_ok ) {
				m_nForwardSubmitSucceededLaneMask |= T3D_RD_FORWARD_LANE_INSTANCES;
			} else {
				m_nForwardSubmitFailedLaneMask |= T3D_RD_FORWARD_LANE_INSTANCES;
			}
		}
		if ( instances_ok ) {
			if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH ) {
				++m_nInstanceDepthSubmitSuccesses;
				m_nInstanceLastSuccessfulDepthScenePass = rContext.m_nScenePass;
			} else if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ) {
				++m_nInstanceOpaqueSubmitSuccesses;
			}
		}
		if ( !instances_ok ) {
			const uint64_t failures = ++m_nInstanceForwardSubmitFailures;
			if ( t3dForwardPassShouldReportInstanceFailure( failures ) ) {
				LOG( ERROR,
				        "[RD instances] Forward+ SUBMIT failed; pass=",
				        ( uint32_t )( rContext.m_passKind ),
				        ", scene_pass=",
				        rContext.m_nScenePass,
				        ", error=",
				        rCoordinator.m_instanceRenderer.getLastError() );
			}
			m_bForwardPreparedHasInstances = false;
		}
	}
	if ( m_bForwardPreparedHasTerrain ) {
		m_nForwardSubmitAttemptedLaneMask |= T3D_RD_FORWARD_LANE_TERRAIN;
	}
	const uint64_t nTerrainDrawsBefore = rCoordinator.m_renderer.getDrawsRendered();
	const bool terrain_ok = !m_bForwardPreparedHasTerrain || rCoordinator.m_renderer.renderForwardPass( rContext );
	const uint64_t nTerrainDrawsAfter = rCoordinator.m_renderer.getDrawsRendered();
	if ( pDrawBaseline && nTerrainDrawsAfter >= nTerrainDrawsBefore ) {
		pDrawBaseline->m_nTerrainDraws += nTerrainDrawsAfter - nTerrainDrawsBefore;
	}
	if ( m_bForwardPreparedHasTerrain ) {
		if ( terrain_ok ) {
			m_nForwardSubmitSucceededLaneMask |= T3D_RD_FORWARD_LANE_TERRAIN;
		} else {
			m_nForwardSubmitFailedLaneMask |= T3D_RD_FORWARD_LANE_TERRAIN;
		}
	}
	bool ocean_submit_ok = true;
	if ( m_bForwardPreparedHasOcean ) {
		m_nForwardSubmitAttemptedLaneMask |= T3D_RD_FORWARD_LANE_OCEAN;
		const uint64_t nOceanDrawsBefore = rCoordinator.m_oceanRenderer.getDrawsRendered();
		ocean_submit_ok = rCoordinator.m_oceanRenderer.renderForwardPass( rContext );
		const uint64_t nOceanDrawsAfter = rCoordinator.m_oceanRenderer.getDrawsRendered();
		if ( pDrawBaseline && nOceanDrawsAfter >= nOceanDrawsBefore ) {
			pDrawBaseline->m_nOceanDraws += nOceanDrawsAfter - nOceanDrawsBefore;
		}
		if ( ocean_submit_ok ) {
			m_nForwardSubmitSucceededLaneMask |= T3D_RD_FORWARD_LANE_OCEAN;
		} else {
			m_nForwardSubmitFailedLaneMask |= T3D_RD_FORWARD_LANE_OCEAN;
		}
	}
	if ( terrain_ok ) {
		rCoordinator.m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
		return true;
	}
	// The native draw list is open here. Do not destroy or rebuild RD resources
	// until a later PREPARE callback, where resource mutation is legal.
	const uint32_t failures = rCoordinator.m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
	if ( failures >= 8u ) {
		rCoordinator.m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
	}
	return false;
}
