// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/utility/terrain_3d_object_bytes.h"
#include "Terrain3DConstants.h"
#include "terrain_3d_forward_pass_preparer.h"
#include "rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.h"

#include "core/utility/logger.h"
#include "data/formats/terrain_3d_half.h"
#include "rendering/rd/instances/terrain_3d_rd_frustum.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
// Consolidated from rendering/rd/world/core/terrain_3d_world_render_authority.h
#include "Terrain3DTypes.h"

// Unified World owns native depth/opaque/motion/shadow rendering for editor and runtime.
// Editor startup waits for World publication instead of submitting legacy raster lanes.
// t3dWorldRenderAuthorityPolicy_s centralized in Terrain3DTypes.h.

static inline t3dWorldRenderAuthorityPolicy_t t3dResolveWorldRenderAuthority(
	const bool bWorldPassKind,
	const bool bEditorAuthoringMode,
	const bool bWorldPublicationAvailable,
	const bool bTransparentPass ) {
	t3dWorldRenderAuthorityPolicy_t policy = {};
	policy.bShippingUnifiedRequired = bWorldPassKind;
	policy.bUnifiedWorldPass = bWorldPassKind && !bTransparentPass && bWorldPublicationAvailable;
	policy.bEditorPreviewPass = false;
	return policy;
}
#include "rendering/rd/vt/streaming/terrain_3d_vt_streaming.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"

#include <math.h>
#include <string.h>

using namespace godot;

static void t3dTerrainRepeatFootprint(
	const float fMinimum,
	const float fMaximum,
	float	   &rMinimum,
	float	   &rMaximum ) {
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

// The unified world lane owns its own VT demand: the legacy instance lane is not prepared here.
// Scan a bounded rotating candidate window and deduplicate identical cluster/channel footprints.
static void t3dStageWorldMaterialFootprintsHot(
	Terrain3DVirtualTexture			&rVT,
	const Terrain3DWorldPublication &rWorld,
	const Terrain3DWorldResources	&rResources,
	const t3dRdFrameSnapshot_t		&rSnapshot,
	const t3dRdForwardPassContext_t &rContext,
	size_t							&rCursor,
	uint32_t						&rSweep ) {
	const t3dVTResidencyState_t &residency = rVT.get_residency();
	const uint32_t				 diagnostic_mode = Terrain3DRdShaderLibrary::getInstanceGpuDiagnosticMode();
	if ( diagnostic_mode == 1u ) {
		return;
	}
	if ( rWorld.m_visibilityCandidates.empty() || !residency.bInitialized || !residency.Buffers.pAllocations ) {
		return;
	}
	uint64_t seenKeys[256];
	uint16_t seenMips[256];
	memset( seenKeys, 0xff, sizeof( seenKeys ) );
	const uint32_t nViews = CLAMP( rContext.m_nViewCount, 1u, T3D_RD_FORWARD_MAX_VIEWS );
	t3dRdFrustum_t frustums[T3D_RD_FORWARD_MAX_VIEWS];
	for ( uint32_t v = 0u; v < nViews; ++v ) {
		frustums[v] = t3dRdExtractFrustum( rContext.m_szViewProjection[v] );
	}
	const size_t nCandidates = rWorld.m_visibilityCandidates.size();
	const size_t nScanCount = MIN( nCandidates, ( size_t ) 4096u );
	for ( size_t scan = 0u; scan < nScanCount; ++scan ) {
		if ( rCursor >= nCandidates ) {
			rCursor = 0u;
			++rSweep;
		}
		const t3dWorldVisibleItem_t &item = rWorld.m_visibilityCandidates[rCursor++];
		if ( item.m_nGeometryKind == T3D_WORLD_GEOMETRY_TERRAIN ||
			 item.m_nClusterOrTerrainId >= rWorld.clusters().size() ||
			 item.m_nClusterOrTerrainId >= rWorld.clusterVtMetadata().size() ||
			 item.m_nInstanceOrPatchId >= rWorld.m_instances.size() ) {
			continue;
		}
		const t3dWorldInstance_t &instance = rWorld.m_instances[item.m_nInstanceOrPatchId];
		if ( ( instance.m_nVisibilityLayers & rContext.m_nCameraVisibleLayers ) == 0u ||
			 instance.m_nTransformIndex >= rWorld.m_transforms.size() ) {
			continue;
		}
		const t3dWorldCluster_t &cluster = rWorld.clusters()[item.m_nClusterOrTerrainId];
		const uint32_t			 nMaterial =
			  instance.m_nMaterialOverride == T3D_WORLD_INVALID_ID ? cluster.m_nMaterial : instance.m_nMaterialOverride;
		t3dRenderingMaterialGpu_t material{};
		if ( !rResources.getResolvedMaterialHot( nMaterial, material ) ||
			 material.m_nVtAllocation == T3D_VT_INVALID_ALLOCATION ) {
			continue;
		}
		const t3dWorldTransform_t &tr = rWorld.m_transforms[instance.m_nTransformIndex];
		const Vector3			   local( cluster.m_fCenter[0], cluster.m_fCenter[1], cluster.m_fCenter[2] );
		const Vector3			   center(
			 tr.m_fRow0[0] * local.x + tr.m_fRow0[1] * local.y + tr.m_fRow0[2] * local.z + tr.m_fRow0[3],
			 tr.m_fRow1[0] * local.x + tr.m_fRow1[1] * local.y + tr.m_fRow1[2] * local.z + tr.m_fRow1[3],
			 tr.m_fRow2[0] * local.x + tr.m_fRow2[1] * local.y + tr.m_fRow2[2] * local.z + tr.m_fRow2[3] );
		float		 fScale = 0.0001f;
		const float *rows[3] = { tr.m_fRow0, tr.m_fRow1, tr.m_fRow2 };
		for ( uint32_t row = 0u; row < 3u; ++row ) {
			fScale = MAX( fScale, ( float ) Vector3( rows[row][0], rows[row][1], rows[row][2] ).length() );
		}
		const float fRadius = MAX( cluster.m_fRadius * fScale, 0.001f );
		float		fPixels = 0.0f;
		for ( uint32_t v = 0u; v < nViews; ++v ) {
			bool bVisible = true;
			for ( uint32_t plane = 0u; plane < 6u; ++plane ) {
				const Vector4 &p = frustums[v].m_szPlanes[plane];
				if ( p.x * center.x + p.y * center.y + p.z * center.z + p.w < -fRadius ) {
					bVisible = false;
					break;
				}
			}
			if ( !bVisible ) {
				continue;
			}
			const Vector3 eye =
				rContext.m_cameraTransform.origin + rContext.m_cameraTransform.basis.xform( rContext.m_szEyeOffset[v] );
			const float fDistance = ( float ) center.distance_to( eye );
			if ( fDistance + fRadius < instance.m_fLodBegin - instance.m_fFadeMargin ||
				 ( instance.m_fLodEnd > 0.0f && fDistance - fRadius > instance.m_fLodEnd + instance.m_fFadeMargin ) ) {
				continue;
			}
			// The length of the VP Y row is invariant to camera rotation; its Y component alone is not.
			const Projection &vp = rContext.m_szViewProjection[v];
			const float		  fProjectionScale = ( float ) Vector3( vp[0].y, vp[1].y, vp[2].y ).length();
			fPixels = MAX( fPixels,
						   fRadius * fProjectionScale * ( float ) MAX( rContext.m_renderSize.y, 1 ) /
							   MAX( fDistance - fRadius, 0.25f ) );
		}
		if ( fPixels <= 0.0f ) {
			continue;
		}
		const t3dWorldClusterVTMetadata_t &meta = rWorld.clusterVtMetadata()[item.m_nClusterOrTerrainId];
		float							   uvMin[2], uvMax[2];
		float							   fUvSpan = 0.0f;
		for ( uint32_t axis = 0u; axis < 2u; ++axis ) {
			const float a = t3dHalfDecode( ( uint16_t ) ( meta.m_nPackedUvMin >> ( axis * 16u ) ) ) *
								material.m_vUvScaleBias[axis] +
							material.m_vUvScaleBias[axis + 2u];
			const float b = t3dHalfDecode( ( uint16_t ) ( meta.m_nPackedUvMax >> ( axis * 16u ) ) ) *
								material.m_vUvScaleBias[axis] +
							material.m_vUvScaleBias[axis + 2u];
			fUvSpan = MAX( fUvSpan, fabsf( b - a ) );
			if ( material.m_nSamplerClass == T3D_VT_SAMPLER_LINEAR_REPEAT ||
				 material.m_nSamplerClass == T3D_VT_SAMPLER_NEAREST_REPEAT ) {
				t3dTerrainRepeatFootprint( a, b, uvMin[axis], uvMax[axis] );
			} else {
				uvMin[axis] = CLAMP( MIN( a, b ), 0.0f, 1.0f );
				uvMax[axis] = CLAMP( MAX( a, b ), 0.0f, 1.0f );
			}
		}
		const uint32_t nCategory =
			instance.m_nGeometryKind == T3D_WORLD_GEOMETRY_STATIC_CLUSTER
				? 2u
				: MIN( ( instance.m_nFlags & T3D_RD_INSTANCE_CATEGORY_MASK ) >> T3D_RD_INSTANCE_CATEGORY_SHIFT, 2u );
		const uint16_t nPriority = ( uint16_t ) CLAMP( fPixels * 32.0f, ( float ) T3D_VT_PRIORITY_NEAR_MIN, 65535.0f );
		const uint32_t nFilter = t3dRdInstanceTextureFilterMode(
			( material.m_nPackedScalars1 & T3D_VT_MATERIAL_FILTER_MASK ) >> T3D_VT_MATERIAL_FILTER_SHIFT,
			rSnapshot.m_szInstanceTextureMipBias[3],
			nCategory );
		const bool bAnisotropic =
			nFilter == T3D_VT_FILTER_NEAREST_MIPMAP_ANISOTROPIC || nFilter == T3D_VT_FILTER_LINEAR_MIPMAP_ANISOTROPIC;
		const bool bTrilinear =
			nFilter == T3D_VT_FILTER_LINEAR_MIPMAP || nFilter == T3D_VT_FILTER_LINEAR_MIPMAP_ANISOTROPIC;
		// Sphere-based demand cannot measure the fragment ellipse: conservatively cover its bounded anisotropic LOD.
		const float fFilterScale = bAnisotropic ? ( float ) T3D_VT_MAX_ANISOTROPY : 1.0f;
		const bool	bMipmapped = nFilter >= T3D_VT_FILTER_NEAREST_MIPMAP;
		for ( uint32_t channel = 0u; channel < T3D_VT_V1_RUNTIME_CHANNELS_PER_ALLOCATION; ++channel ) {
			if ( diagnostic_mode == 2u && channel != 0u ) {
				continue;
			}
			if ( channel == 1u && ( material.m_nFlags & T3D_VT_MATERIAL_NORMAL_MAPPING ) == 0u ) {
				continue;
			}
			if ( channel == 2u && !t3d_vt_material_has_surface_texture( material.m_nFlags ) ) {
				continue;
			}
			if ( material.m_nVtAllocation >= residency.nAllocationCount ||
				 channel >= residency.nAllocationCount - material.m_nVtAllocation ) {
				break;
			}
			const uint32_t			 nAllocation = material.m_nVtAllocation + channel;
			const t3dVTAllocation_t &allocation = residency.Buffers.pAllocations[nAllocation];
			if ( allocation.m_nMipCount == 0u || !t3d_vt_allocation_quality_enabled( allocation.m_nFlags ) ) {
				continue;
			}
			const uint32_t nBaseMip = t3d_vt_allocation_quality_base_mip( allocation.m_nFlags );
			const float	   fExtent = ( float ) MAX( allocation.m_nMip0WidthPages, allocation.m_nMip0HeightPages ) *
								  ( float ) T3D_VT_PAGE_INTERIOR / exp2f( ( float ) nBaseMip );
			const float fAutomaticLod =
				bMipmapped ? log2f( MAX( fUvSpan * fExtent / ( MAX( fPixels, 1.0f ) * fFilterScale ), 1.0f ) ) : 0.0f;
			const uint16_t nMip =
				( uint16_t ) CLAMP( floorf( fAutomaticLod + rSnapshot.m_szInstanceTextureMipBias[nCategory] ),
									0.0f,
									( float ) ( allocation.m_nMipCount - 1u ) );
			const uint64_t nKey = ( ( uint64_t ) item.m_nClusterOrTerrainId << 32u ) | nAllocation;
			uint32_t	   slot = ( uint32_t ) ( ( nKey ^ ( nKey >> 32u ) ) * 2654435761u ) & 255u;
			uint32_t	   probes = 0u;
			while ( probes < 256u && seenKeys[slot] != UINT64_MAX && seenKeys[slot] != nKey ) {
				slot = ( slot + 1u ) & 255u;
				++probes;
			}
			if ( probes == 256u ) {
				--rCursor;
				return;
			} // Retry this candidate with an empty dedup table next frame.
			if ( seenKeys[slot] == nKey && seenMips[slot] <= nMip ) {
				continue;
			}
			seenKeys[slot] = nKey;
			seenMips[slot] = nMip;
			rVT.stage_material_footprint_hot( nAllocation,
											  ( uint32_t ) rContext.m_nRenderFrame,
											  uvMin,
											  uvMax,
											  nMip,
											  T3D_FORWARD_PASS_MATERIAL_REQUEST_CAP,
											  nPriority,
											  T3D_VT_REQUEST_PRODUCER_INSTANCE,
											  rSweep );
			if ( bTrilinear && nBaseMip + nMip + 1u < allocation.m_nMipCount ) {
				rVT.stage_material_footprint_hot( nAllocation,
												  ( uint32_t ) rContext.m_nRenderFrame,
												  uvMin,
												  uvMax,
												  ( uint16_t ) ( nMip + 1u ),
												  T3D_FORWARD_PASS_MATERIAL_REQUEST_CAP,
												  nPriority,
												  T3D_VT_REQUEST_PRODUCER_INSTANCE,
												  rSweep );
			}
		}
	}
}

static uint32_t t3dStageTerrainMaterialFootprintHot(
	Terrain3DVirtualTexture	   &rVirtualTexture,
	const t3dRdFrameSnapshot_t &rSnapshot,
	const t3dRdMaterialLayer_t &rLayer,
	const uint32_t				nFrameId ) {
	uint32_t	nRemaining = T3D_FORWARD_PASS_MATERIAL_REQUEST_CAP;
	uint32_t	nAttempted = 0u;
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
			float		uvMin[2];
			float		uvMax[2];
			t3dTerrainRepeatFootprint( ( patch.m_fOriginX - fHalfExtent ) * fUvScale,
									   ( patch.m_fOriginX + fHalfExtent ) * fUvScale,
									   uvMin[0],
									   uvMax[0] );
			t3dTerrainRepeatFootprint( ( patch.m_fOriginZ - fHalfExtent ) * fUvScale,
									   ( patch.m_fOriginZ + fHalfExtent ) * fUvScale,
									   uvMin[1],
									   uvMax[1] );
			const uint32_t nChannelCount =
				( rLayer.m_nMicroVTFlags & 1u ) != 0u ? T3D_VT_V1_RUNTIME_CHANNELS_PER_ALLOCATION : 1u;
			for ( uint32_t iChannel = 0u; iChannel < nChannelCount && nRemaining > 0u; ++iChannel ) {
				const uint32_t nRuntimeAllocation = rLayer.m_nMicroVTAllocation + iChannel;
				if ( nRuntimeAllocation >= rVirtualTexture.getActiveAllocationCount() ||
					 rVirtualTexture.get_residency().Buffers.pAllocations[nRuntimeAllocation].m_nPageCount == 0u ) {
					continue;
				}
				const uint32_t nPatchAttempts =
					rVirtualTexture.stage_material_footprint_hot( nRuntimeAllocation,
																  nFrameId,
																  uvMin,
																  uvMax,
																  ( uint16_t ) MIN( patch.m_nLod, 0xffffu ),
																  nRemaining,
																  T3D_VT_PRIORITY_NEAR_MIN,
																  T3D_VT_REQUEST_PRODUCER_TERRAIN );
				nAttempted += nPatchAttempts;
				nRemaining -= MIN( nRemaining, nPatchAttempts );
			}
		}
	}
	if ( nAttempted == 0u ) {
		const float	   uvMin[2] = { 0.0f, 0.0f };
		const float	   uvMax[2] = { 1.0f, 1.0f };
		const uint32_t nChannelCount =
			( rLayer.m_nMicroVTFlags & 1u ) != 0u ? T3D_VT_V1_RUNTIME_CHANNELS_PER_ALLOCATION : 1u;
		for ( uint32_t iChannel = 0u; iChannel < nChannelCount && nAttempted < T3D_FORWARD_PASS_MATERIAL_REQUEST_CAP;
			  ++iChannel ) {
			const uint32_t nRuntimeAllocation = rLayer.m_nMicroVTAllocation + iChannel;
			if ( nRuntimeAllocation >= rVirtualTexture.getActiveAllocationCount() ||
				 rVirtualTexture.get_residency().Buffers.pAllocations[nRuntimeAllocation].m_nPageCount == 0u ) {
				continue;
			}
			nAttempted +=
				rVirtualTexture.stage_material_footprint_hot( nRuntimeAllocation,
															  nFrameId,
															  uvMin,
															  uvMax,
															  1u,
															  T3D_FORWARD_PASS_MATERIAL_REQUEST_CAP - nAttempted,
															  T3D_VT_PRIORITY_NORMAL_MIN,
															  T3D_VT_REQUEST_PRODUCER_TERRAIN );
		}
	}
	return nAttempted;
}

void Terrain3DForwardPassPreparer::reset(
	void ) {
	t3dClearTrivialArrayBytes( m_szForwardDrawBaseline );
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
	m_eLastSharedVtAcquireStatus = T3D_VT_ACQUIRE_UNPUBLISHED;
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
	m_nLastWorldVtResidencyRequestFrame = UINT64_MAX;
	m_nWorldVtCandidateCursor = 0u;
	m_nWorldVtCandidateSweep = 0u;
	m_nWorldPrepareFailureStage = 0u;
	m_nWorldPrepareFailurePassKind = T3D_RD_FORWARD_PASS_INVALID;
	memset( m_szLoggedWorldPrepareStage, 0, sizeof( m_szLoggedWorldPrepareStage ) );
}

void Terrain3DForwardPassPreparer::logWorldPrepareStateHot(
	const t3dRdForwardPassContext_t &rContext,
	const uint32_t					 nStage,
	const Terrain3DRdClipmapRenderer *const pTerrainProvider,
	const t3dRdFrameSnapshot_t *const pSnapshot ) {
	const uint32_t nPass = ( uint32_t ) rContext.m_passKind;
	if ( nPass >= T3D_RD_FORWARD_PASS_MAX ) {
		return;
	}
	if ( m_szLoggedWorldPrepareStage[nPass] && m_szLastLoggedWorldPrepareStage[nPass] == nStage ) {
		return;
	}
	m_szLoggedWorldPrepareStage[nPass] = true;
	m_szLastLoggedWorldPrepareStage[nPass] = nStage;
	const char *pPass = "unknown";
	switch ( rContext.m_passKind ) {
	case T3D_RD_FORWARD_PASS_DEPTH:
		pPass = "depth";
		break;
	case T3D_RD_FORWARD_PASS_OPAQUE:
		pPass = "opaque";
		break;
	case T3D_RD_FORWARD_PASS_MOTION:
		pPass = "motion";
		break;
	case T3D_RD_FORWARD_PASS_SHADOW:
		pPass = "shadow";
		break;
	default:
		return;
	}
	static const char *const s_pStages[] = { "ready",
											 "world_publication_missing",
											 "shared_vt_unavailable",
											 "region_map_empty",
											 "shared_vt_binding_snapshot_failed",
											 "terrain_resource_provider_failed",
											 "terrain_resource_export_failed",
											 "world_raster_prepare_failed",
											 "world_publication_prepare_failed",
											 "world_visibility_prepare_failed",
											 "forward_viewport_invalid",
											 "forward_renderer_init_failed",
											 "forward_snapshot_unavailable",
											 "forward_shader_unavailable" };
	const char				*pReason = nStage < T3D_ARRAY_COUNT( s_pStages ) ? s_pStages[nStage] : "unknown";
	if ( nStage == 0u ) {
		UtilityFunctions::print( "[Terrain3D][World][Prepare] READY pass=",
								 pPass,
								 " frame=",
								 rContext.m_nRenderFrame,
								 " scene_pass=",
								 rContext.m_nScenePass,
								 " cpu_patches=", pSnapshot ? pSnapshot->m_nPatchCount : 0u,
								 " region_map_generation=", pSnapshot ? pSnapshot->m_nRegionMapGeneration : 0u,
								 " prepared_world=", m_bForwardPreparedHasWorld,
								 " shared_vt_status=", ( uint32_t ) m_eLastSharedVtAcquireStatus );
	} else if ( nStage == 5u && pTerrainProvider ) {
		UtilityFunctions::push_warning( "[Terrain3D][World][Prepare] FAIL pass=", pPass,
										" stage=", pReason, " stage_id=", nStage,
										" provider_stage=", pTerrainProvider->getForwardPrepareFailureStage(),
										" provider_error=", pTerrainProvider->getLastError(),
										" patch_count=", pSnapshot ? pSnapshot->m_nPatchCount : 0u,
										" material_backend=", pSnapshot ? ( uint32_t ) pSnapshot->m_nTerrainMaterialBackend : 0u,
											" frame=", rContext.m_nRenderFrame,
										" scene_pass=", rContext.m_nScenePass );
	} else {
		UtilityFunctions::push_warning( "[Terrain3D][World][Prepare] FAIL pass=",
										pPass,
										" stage=",
										pReason,
										" stage_id=",
										nStage,
										" frame=",
										rContext.m_nRenderFrame,
										" scene_pass=",
										rContext.m_nScenePass );
	}
}

bool Terrain3DForwardPassPreparer::prepareLifecycle(
	const t3dRdForwardPassContext_t &rContext,
	bool							&rHistoryValid,
	bool							&rViewportReconfigured ) {
	rHistoryValid = false;
	rViewportReconfigured = false;
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ) {
		return true;
	}
	if ( rContext.m_nViewportKey == 0u || rContext.m_nViewCount == 0u ||
		 rContext.m_nViewCount > T3D_RD_FORWARD_MAX_VIEWS || rContext.m_nSampleCount == 0u ||
		 !rContext.m_hNativeFramebuffer.is_valid() || rContext.m_nFramebufferFormat == RenderingDevice::INVALID_ID ) {
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
	const bool same = slot->m_bValid && slot->m_nViewportKey == rContext.m_nViewportKey &&
					  slot->m_renderSize == rContext.m_renderSize && slot->m_renderRegion == rContext.m_renderRegion &&
					  slot->m_nViewCount == rContext.m_nViewCount && slot->m_nSampleCount == rContext.m_nSampleCount;
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

bool Terrain3DForwardPassPreparer::render(
	Terrain3DForwardPassHost		&rHost,
	t3dForwardPassServices_t		&rServices,
	const t3dRdForwardPassContext_t &rContext ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), false );
#endif
	if ( !rServices.m_bRenderEnabled.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return false;
	}
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
	++m_nForwardPassCallbacks;
	m_nLastForwardScenePass = rContext.m_nScenePass;
	m_nLastForwardDrawList = rContext.m_nDrawList;
	m_nLastForwardPassKind = ( uint32_t ) ( rContext.m_passKind );
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
	if ( rContext.m_passKind != T3D_RD_FORWARD_PASS_DEPTH && rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE &&
		 rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW && rContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION &&
		 rContext.m_passKind != T3D_RD_FORWARD_PASS_TRANSPARENT ) {
		return false;
	}

	if ( rContext.m_phase == T3D_RD_FORWARD_PHASE_PREPARE ) {
		return prepare( rHost, rServices, rContext );
	}
	return submit( rServices, rContext );
}

bool Terrain3DForwardPassPreparer::prepare(
	Terrain3DForwardPassHost		&rHost,
	t3dForwardPassServices_t		&rServices,
	const t3dRdForwardPassContext_t &rContext ) {
	Terrain3DRdContext::getShared().beginFrame( rContext.m_nRenderFrame, 0u );
	m_nForwardPrepareRequestedLaneMask = 0u;
	m_nForwardPrepareReadyLaneMask = 0u;
	m_nForwardPrepareFailedLaneMask = 0u;
	m_nForwardSubmitAttemptedLaneMask = 0u;
	m_nForwardSubmitSucceededLaneMask = 0u;
	m_nForwardSubmitFailedLaneMask = 0u;
	m_nForwardLaneScenePass = rContext.m_nScenePass;
	m_nForwardLanePassKind = ( uint32_t ) ( rContext.m_passKind );
	m_nForwardPublishedInstanceCount = 0u;
	m_nForwardPublishedInstanceCellCount = 0u;
	m_bForwardInstanceLaneExpected = false;
	m_bForwardInstancePrepareAttempted = false;
	m_bForwardInstancePrepareSucceeded = false;
	m_bForwardInstancePreparedHasDraws = false;
	m_bForwardInstanceSubmitAttempted = false;
	m_bForwardInstanceSubmitSucceeded = false;
	m_nForwardInstanceSubmitDraws = 0u;
	bool	   history_valid = false;
	bool	   viewport_reconfigured = false;
	const bool bWorldPass =
		rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH || rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ||
		rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
	if ( !prepareLifecycle( rContext, history_valid, viewport_reconfigured ) ) {
		if ( bWorldPass ) {
			m_nWorldPrepareFailureStage = 10u;
			m_nWorldPrepareFailurePassKind = ( uint32_t ) rContext.m_passKind;
			logWorldPrepareStateHot( rContext, 10u );
		}
		return false;
	}
	if ( !rHost.isForwardShaderReady() ) {
		if ( bWorldPass ) {
			m_nWorldPrepareFailureStage = 13u;
			m_nWorldPrepareFailurePassKind = ( uint32_t ) rContext.m_passKind;
			logWorldPrepareStateHot( rContext, 13u );
		}
		return false;
	}
	// Renderer creation/recovery belongs to startup or queued lifecycle work.
	// Native PREPARE only reads the state prepared by that control plane.
	if ( !rHost.isForwardRendererReady() ) {
		if ( bWorldPass ) {
			m_nWorldPrepareFailureStage = 11u;
			m_nWorldPrepareFailurePassKind = ( uint32_t ) rContext.m_passKind;
			logWorldPrepareStateHot( rContext, 11u );
		}
		return false;
	}
	const t3dRdFrameSnapshotSlot_t *const publication = rHost.acquireForwardSnapshot();
	if ( !publication ) {
		if ( bWorldPass ) {
			m_nWorldPrepareFailureStage = 12u;
			m_nWorldPrepareFailurePassKind = ( uint32_t ) rContext.m_passKind;
			logWorldPrepareStateHot( rContext, 12u );
		}
		return false;
	}
	const t3dRdFrameSnapshot_t &snapshot = publication->m_snapshot;
	Terrain3DRdContext::getShared().beginFrame( rContext.m_nRenderFrame, snapshot.m_nPublicationGeneration );
	const t3dRdRegionMapBank_t *const pRegionMap =
		rServices.m_publicationBuilder.getRegionMapBank( publication->m_nRegionMapBank );
	const t3dRdInstanceMeshBank_t *const pMeshes =
		rServices.m_publicationBuilder.getInstanceMeshBank( publication->m_nInstanceMeshBank );
	const t3dRdInstanceSceneBank_t *const pScene =
		rServices.m_publicationBuilder.getInstanceSceneBank( publication->m_nInstanceSceneBank );
	const t3dRdOceanStripBank_t *const pOcean =
		rServices.m_publicationBuilder.getOceanStripBank( publication->m_nOceanStripBank );
	const t3dWorldPublicationBank_t *const pWorld =
		rServices.m_publicationBuilder.getWorldPublicationBank( publication->m_nWorldPublicationBank );
	const bool world_pass_kind =
		rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH || rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ||
		rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
	const bool shadow_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
	const bool transparent_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
	const bool editor_authoring_mode = snapshot.m_bEditorAuthoringMode;
	const bool world_publication_available = pWorld && pWorld->m_publication.m_nGeneration != 0u;
	const t3dWorldRenderAuthorityPolicy_t world_authority = t3dResolveWorldRenderAuthority( world_pass_kind,
																							editor_authoring_mode,
																							world_publication_available,
																							transparent_pass );
	if ( world_authority.bShippingUnifiedRequired && !world_publication_available ) {
		m_nWorldPrepareFailureStage = 1u;
		m_nWorldPrepareFailurePassKind = ( uint32_t ) rContext.m_passKind;
		++m_nStrictWorldAuthorityFailures;
		m_nForwardPrepareRequestedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
		m_nForwardPrepareFailedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
		logWorldPrepareStateHot( rContext, 1u );
		return false;
	}
	rServices.m_worldRuntime.setFramePolicyHot( ( uint64_t ) rServices.m_nWorldUploadBudgetKbPolicy * 1024u,
												rServices.m_nWorldClusterBudgetPolicy,
												rServices.m_bWorldBoundaryDeferredPolicy );
	rServices.m_virtualTexture.set_frame_budgets_hot( rServices.m_nVtRequestBudgetPolicy,
													  rServices.m_nVtAdmissionBudgetPolicy );
	if ( world_publication_available ) {
		if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ) {
			rHost.publishForwardWorldReadiness( pWorld->m_publication, false );
		}
		Terrain3DColdPhaseScope world_prepare_scope;
		if ( !rServices.m_worldRuntime.preparePublicationCold( pWorld->m_publication,
															   rContext.m_nRenderFrame,
															   &rServices.m_virtualTexture ) ) {
			if ( bWorldPass ) {
				m_nWorldPrepareFailureStage = 8u;
				m_nWorldPrepareFailurePassKind = ( uint32_t ) rContext.m_passKind;
				logWorldPrepareStateHot( rContext, 8u );
			}
			return false;
		}
	}
	const uint32_t visibility_mask = shadow_pass ? ( rContext.m_nShadowCasterMask & rContext.m_nShadowLightCullMask )
												 : rContext.m_nCameraVisibleLayers;
	if ( world_publication_available && !transparent_pass ) {
		T3D_STREAM_PROFILE_SCOPE( T3D_SP_FORWARD_WORLD_VISIBILITY );
		Terrain3DColdPhaseScope world_visibility_scope;
		if ( !rServices.m_worldRuntime.prepareVisibilityPass( rContext, snapshot, visibility_mask ) ) {
			m_nWorldPrepareFailureStage = 9u;
			m_nWorldPrepareFailurePassKind = ( uint32_t ) rContext.m_passKind;
			logWorldPrepareStateHot( rContext, 9u );
			return false;
		}
	}
	// Cold-only resource materialization happens before the hot-frame allocation scope.
	// Failed optional initialization is latched until a renderer lifecycle reset.
    const bool environment_relevant = world_authority.bUnifiedWorldPass || snapshot.m_ocean.m_nEnabled != 0u;
    if ( environment_relevant && Terrain3DRdShaderLibrary::isEnvironmentFieldsEnabledCold() ) {
        {
            Terrain3DColdPhaseScope environment_initialization_scope;
            ( void ) rServices.m_environmentField.initializeCold();
        }
        // Run once per native frame before WORLD visibility/material and ocean consumers.
        // This allows a shadow or transparent pass to be the first environment reader.
        rServices.m_environmentField.advanceFrameHot( snapshot, rContext );
    }
	const bool has_terrain =
		!transparent_pass && snapshot.m_nPatchCount > 0u && pRegionMap && !pRegionMap->m_bytes.is_empty() &&
		( !shadow_pass || ( snapshot.m_bCastShadows && ( snapshot.m_nRenderLayers & visibility_mask ) != 0u ) );
	const bool unified_world_pass = world_authority.bUnifiedWorldPass;
	if ( world_authority.bEditorPreviewPass ) {
		++m_nEditorPreviewPrepares;
	}
	m_nForwardPublishedInstanceCount = pScene ? ( uint32_t ) ( pScene->m_snapshot.m_instances.size() ) : 0u;
	m_nForwardPublishedInstanceCellCount = pScene ? ( uint32_t ) ( pScene->m_snapshot.m_cells.size() ) : 0u;
	m_bForwardInstanceLaneExpected = false; // World publication owns instance draws.
	const bool			  shared_vt_needed = has_terrain || m_bForwardInstanceLaneExpected || unified_world_pass;
	t3dSharedVTBindings_t shared_vt_bindings = {};
	m_eLastSharedVtAcquireStatus =
		shared_vt_needed ? rHost.acquireForwardSharedVt( shared_vt_bindings ) : T3D_VT_ACQUIRE_READY;
	const bool shared_vt_ready = m_eLastSharedVtAcquireStatus == T3D_VT_ACQUIRE_READY;
	if ( !shared_vt_ready ) {
		++m_nSharedVtAcquireFailures;
	}
	if ( shared_vt_ready &&
		 ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) ) {
		const Vector3 forward = -rContext.m_cameraTransform.basis.get_column( 2 );
		rServices.m_virtualTexture.update_feedback_view_hot( ( float ) rContext.m_cameraTransform.origin.x,
			( float ) rContext.m_cameraTransform.origin.z, ( float ) forward.x,
			( float ) forward.z, ( uint32_t ) rContext.m_nRenderFrame );
		rServices.m_virtualTexture.stage_spatial_demand_hot( ( float ) rContext.m_cameraTransform.origin.x,
															 ( float ) rContext.m_cameraTransform.origin.z,
															 ( float ) forward.x,
															 ( float ) forward.z,
															 ( uint32_t ) rContext.m_nRenderFrame,
                                         rContext.m_szViewProjection[0].is_orthogonal() ? 0.0f :
                                         ( float ) rContext.m_szViewProjection[0].get_fov(),
                                         ( uint32_t ) MAX( 0, rContext.m_renderSize.y ) );
	}
	if ( shared_vt_ready &&
		 ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) ) {
		uint32_t predicted_terrain_mask = 0u;
		uint32_t feedback_material_mask = 0u;
		if ( !editor_authoring_mode && has_terrain &&
			 ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ||
			   rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) ) {
			const uint32_t nCount = MIN( snapshot.m_nMaterialLayerCount, ( uint32_t ) T3D_RD_MATERIAL_LAYER_COUNT );
			const uint32_t frame = ( uint32_t ) rContext.m_nRenderFrame;
			for ( uint32_t i = 0u; i < nCount; ++i ) {
				const t3dRdMaterialLayer_t &layer = snapshot.m_szMaterials[i];
				if ( ( layer.m_nFlags & T3D_RD_MATERIAL_LAYER_MICRO_VT ) != 0u &&
					 layer.m_nMicroVTAllocation != T3D_VT_INVALID_ALLOCATION ) {
					feedback_material_mask |= 1u << i;
				}
				if ( ( feedback_material_mask & ( 1u << i ) ) != 0u &&
					 !rServices.m_virtualTexture.shouldScanTerrainAllocationHot(
						 ( float ) rContext.m_cameraTransform.origin.x,
						 ( float ) rContext.m_cameraTransform.origin.z,
						 layer.m_nMicroVTAllocation,
						 frame ) ) {
					predicted_terrain_mask |= 1u << i;
				}
			}
		}
		Terrain3DVTGpu &feedback = rServices.m_virtualTexture.get_gpu();
		feedback.set_predicted_terrain_materials_hot( predicted_terrain_mask );
		const t3dVTResidencyState_t &vtResidency = rServices.m_virtualTexture.get_residency();
		if ( vtResidency.pDatabase && vtResidency.pDatabase->pHeader ) {
			( void ) feedback.service_feedback_grid_capture_hot(
				( uint32_t ) rContext.m_nRenderFrame, vtResidency.nDatabaseGeneration,
				( uint32_t ) Terrain3DRdContext::getShared().getDeviceGeneration(), rContext.m_nViewportKey,
				vtResidency.pDatabase->pHeader->nPageCount, vtResidency.nAllocationCount,
				( uint32_t ) MAX( rContext.m_renderSize.x, 0 ),
				( uint32_t ) MAX( rContext.m_renderSize.y, 0 ) );
		}
		const bool no_world_mesh_feedback =
			!world_publication_available || pWorld->m_publication.clusters().size() == 0u;
		const bool all_terrain_predicted =
			feedback_material_mask != 0u &&
			( predicted_terrain_mask & feedback_material_mask ) == feedback_material_mask;
		// P03's every-fourth-frame corrective scan clears these bits. While
		// coverage is complete and no meshes need discovery, only publish the
		// small mask UBO: the previous feedback hash can be collected on the
		// next corrective frame instead of reset/read back every raster frame.
		if ( !( no_world_mesh_feedback && all_terrain_predicted && feedback.publish_predicted_terrain_mask_hot() ) ) {
			rServices.m_virtualTexture.service_fragment_feedback_hot( ( uint32_t ) rContext.m_nRenderFrame );
		}
	}
	if ( has_terrain && shared_vt_ready && m_nLastTerrainVtResidencyRequestFrame != rContext.m_nRenderFrame &&
		 ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) &&
		 rServices.m_virtualTexture.shouldRefreshMaterialDemandHot(
			 0u,
			 rContext,
			 snapshot.m_nMaterialGeneration ^ ( snapshot.m_nRegionMapGeneration << 1u ) ^
				 ( snapshot.m_generations.m_nPatchGeneration << 2u ) ) ) {
		m_nLastTerrainVtResidencyRequestFrame = rContext.m_nRenderFrame;
		const uint32_t nFrameId = ( uint32_t ) rContext.m_nRenderFrame;
		const uint32_t nMaterialCount = MIN( snapshot.m_nMaterialLayerCount, ( uint32_t ) T3D_RD_MATERIAL_LAYER_COUNT );
		for ( uint32_t iMaterial = 0u; iMaterial < nMaterialCount; ++iMaterial ) {
			const t3dRdMaterialLayer_t &layer = snapshot.m_szMaterials[iMaterial];
			if ( !rServices.m_virtualTexture.shouldScanTerrainAllocationHot(
					 ( float ) rContext.m_cameraTransform.origin.x,
					 ( float ) rContext.m_cameraTransform.origin.z,
					 layer.m_nMicroVTAllocation,
					 nFrameId ) ) {
				continue;
			}
			if ( ( layer.m_nFlags & T3D_RD_MATERIAL_LAYER_MICRO_VT ) == 0u ||
				 layer.m_nMicroVTAllocation == T3D_VT_INVALID_ALLOCATION ) {
				continue;
			}
			m_nTerrainVtResidencyRequestAttempts +=
				t3dStageTerrainMaterialFootprintHot( rServices.m_virtualTexture, snapshot, layer, nFrameId );
		}
	}
	if ( unified_world_pass ) {
		m_nForwardPrepareRequestedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
	}
	m_nForwardPreparedScenePass = rContext.m_nScenePass;
	m_nForwardPreparedViewportKey = rContext.m_nViewportKey;
	m_bForwardPreparedHasTerrain = has_terrain;
	m_bForwardPreparedHasInstances = false;
	m_bForwardPreparedHasOcean = false;
	m_bForwardPreparedHasWorld = false;
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER );
	t3dForwardLanePreparation_t	  lanes;
	lanes.m_pPublication = publication;
	lanes.m_pRegionMap = pRegionMap;
	lanes.m_pMeshes = pMeshes;
	lanes.m_pScene = pScene;
	lanes.m_pWorld = pWorld;
	lanes.m_nVisibilityMask = visibility_mask;
	lanes.m_bHistoryValid = history_valid;
	lanes.m_bViewportReconfigured = viewport_reconfigured;
	lanes.m_bUnifiedWorldPass = unified_world_pass;
	lanes.m_bEditorAuthoringMode = editor_authoring_mode;
	lanes.m_bSharedVtReady = shared_vt_ready;
	lanes.m_pSharedVtBindings = shared_vt_ready ? &shared_vt_bindings : NULL;
	lanes.m_bHasTerrain = has_terrain;
	// World publication prepares both terrain and instance geometry.

	if ( shared_vt_ready && unified_world_pass && pWorld &&
		 rServices.m_worldRuntime.getPreparedPublicationGeneration() == pWorld->m_publication.m_nGeneration &&
		 m_nLastWorldVtResidencyRequestFrame != rContext.m_nRenderFrame &&
		 ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) &&
		 rServices.m_virtualTexture.shouldRefreshMaterialDemandHot( 2u,
																	rContext,
																	pWorld->m_publication.m_nGeneration ) ) {
		m_nLastWorldVtResidencyRequestFrame = rContext.m_nRenderFrame;
		t3dStageWorldMaterialFootprintsHot( rServices.m_virtualTexture,
											pWorld->m_publication,
											rServices.m_worldRuntime.getResources(),
											snapshot,
											rContext,
											m_nWorldVtCandidateCursor,
											m_nWorldVtCandidateSweep );
	}
	if ( shared_vt_ready ) {
		( void ) rServices.m_virtualTexture.service_feedback_grid_hot(
			( uint32_t ) rContext.m_nRenderFrame, rContext.m_nViewportKey,
			( uint32_t ) Terrain3DRdContext::getShared().getDeviceGeneration() );
		// V1 diagnostic capture does not stage requests or change residency.
		// Always drain it after V2, including when the V2 grid is disabled.
		rServices.m_virtualTexture.service_multi_feedback_shadow_hot(
			( uint32_t ) rContext.m_nRenderFrame, rContext.m_nViewportKey,
			( uint32_t ) Terrain3DRdContext::getShared().getDeviceGeneration() );
		uint32_t nTerrainAccepted = 0u;
		uint32_t nInstanceAccepted = 0u;
		if ( rServices.m_virtualTexture.flush_staged_material_requests_hot( ( uint32_t ) rContext.m_nRenderFrame,
																			nTerrainAccepted,
																			nInstanceAccepted ) ) {
			m_nTerrainVtResidencyRequestsAccepted += nTerrainAccepted;
			// World material demand is serviced by the shared VT runtime.
		}
		{
			T3D_STREAM_PROFILE_SCOPE( T3D_SP_FORWARD_VT_SERVICE );
			rServices.m_virtualTexture.service_streaming_hot( ( uint32_t ) rContext.m_nRenderFrame );
		}
	}

	// Terrain and instance/HLOD resources are independent. Snapshot shared VT
	// bindings after companion World PREPARE so a companion teardown cannot
	// mutate descriptor state underneath the terrain lane in this transaction.
	bool terrain_and_world_ok = false;
	{
		T3D_STREAM_PROFILE_SCOPE( T3D_SP_FORWARD_WORLD_RASTER );
		terrain_and_world_ok = prepareTerrainAndWorldLanes( rHost, rServices, rContext, lanes );
	}

	const bool ocean_lane_ready = rServices.m_bOceanRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	const bool ocean_requested = snapshot.m_ocean.m_nEnabled != 0u && snapshot.m_ocean.m_nPatchCount > 0u &&
								 ( transparent_pass || ( shadow_pass && snapshot.m_ocean.m_nCastShadows != 0u ) );
	if ( ocean_requested ) {
		m_nForwardPrepareRequestedLaneMask |= T3D_RD_FORWARD_LANE_OCEAN;
	}
    t3dRdEnvironmentBindings_t oceanBindings{};
    const t3dRdEnvironmentBindings_t *pOceanEnvironment = NULL;
    if ( Terrain3DRdShaderLibrary::isEnvironmentFieldsEnabledCold() &&
         rServices.m_environmentField.acquireBindingsHot( oceanBindings ) ) {
        pOceanEnvironment = &oceanBindings;
    }
    const bool ocean_ok = !ocean_lane_ready || rServices.m_oceanRenderer.prepareForwardPass(
        rContext, snapshot, pOcean ? &pOcean->m_snapshot : NULL, visibility_mask, pOceanEnvironment );
	m_bForwardPreparedHasOcean = ocean_lane_ready && ocean_ok && ocean_requested;
	if ( ocean_requested ) {
		if ( m_bForwardPreparedHasOcean ) {
			m_nForwardPrepareReadyLaneMask |= T3D_RD_FORWARD_LANE_OCEAN;
		} else {
			m_nForwardPrepareFailedLaneMask |= T3D_RD_FORWARD_LANE_OCEAN;
		}
	}
	if ( terrain_and_world_ok ) {
		rServices.m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
		return true;
	}
	// PREPARE failures are lane-local and recoverable. Keep stable renderer and
	// shared VT resources alive; a subsequent PREPARE may recover after the
	// relevant bindings/pipeline generation changes. Full cold teardown is
	// reserved for actual renderer initialization/device failures.
	rServices.m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
	++m_nTerrainRecoverablePrepareFailures;
	return false;
}

bool Terrain3DForwardPassPreparer::submit(
	t3dForwardPassServices_t		&rServices,
	const t3dRdForwardPassContext_t &rContext ) {
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT ||
		 !rServices.m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE ) ||
		 m_nForwardPreparedScenePass != rContext.m_nScenePass ||
		 m_nForwardPreparedViewportKey != rContext.m_nViewportKey ) {
		return false;
	}
	Terrain3DAllocationPhaseScope	  allocation_phase( TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER );
	t3dRdForwardDrawBaseline_t *const pDrawBaseline =
		rContext.m_passKind > T3D_RD_FORWARD_PASS_INVALID && rContext.m_passKind < T3D_RD_FORWARD_PASS_MAX
			? &m_szForwardDrawBaseline[( uint32_t ) ( rContext.m_passKind )]
			: NULL;
	if ( pDrawBaseline ) {
		++pDrawBaseline->m_nSubmitCallbacks;
	}
	if ( m_bForwardPreparedHasWorld &&
		 ( rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH || rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ||
		   rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ) ) {
		const uint64_t nWorldDrawsBefore = rServices.m_worldRuntime.getRaster().getSubmitCount( rContext.m_passKind );
		const bool	   world_ok = rServices.m_worldRuntime.submitRasterPass( rContext );
		const uint64_t nWorldDrawsAfter = rServices.m_worldRuntime.getRaster().getSubmitCount( rContext.m_passKind );
		if ( pDrawBaseline && nWorldDrawsAfter >= nWorldDrawsBefore ) {
			pDrawBaseline->m_nWorldDraws += nWorldDrawsAfter - nWorldDrawsBefore;
		}
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
		if ( pDrawBaseline && pDrawBaseline->m_nSubmitCallbacks == 1u &&
			 ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ||
			   rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH ) ) {
			UtilityFunctions::print( "[Terrain3D][World][Submit] FIRST pass_kind=",
				( uint32_t ) rContext.m_passKind, " frame=", rContext.m_nRenderFrame,
				" scene_pass=", rContext.m_nScenePass, " success=", world_ok,
				" indirect_submission_delta=", nWorldDrawsAfter - nWorldDrawsBefore,
				" draw_list=", rContext.m_nDrawList,
				" ready_lane_mask=", m_nForwardPrepareReadyLaneMask );
		}
#endif
		if ( world_ok ) {
			m_nForwardSubmitAttemptedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
			m_nForwardSubmitSucceededLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
			rServices.m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
			if ( rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW ) {
				return true;
			}
		}
		if ( !world_ok ) {
			m_nForwardSubmitAttemptedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
			m_nForwardSubmitFailedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
			rServices.m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
			return false;
		}
	}
	// Native World owns depth, opaque, shadow, motion, terrain, and mesh draws.
	bool ocean_submit_ok = true;
	if ( m_bForwardPreparedHasOcean ) {
		m_nForwardSubmitAttemptedLaneMask |= T3D_RD_FORWARD_LANE_OCEAN;
		const uint64_t nOceanDrawsBefore = rServices.m_oceanRenderer.getDrawsRendered();
		ocean_submit_ok = rServices.m_oceanRenderer.renderForwardPass( rContext );
		const uint64_t nOceanDrawsAfter = rServices.m_oceanRenderer.getDrawsRendered();
		if ( pDrawBaseline && nOceanDrawsAfter >= nOceanDrawsBefore ) {
			pDrawBaseline->m_nOceanDraws += nOceanDrawsAfter - nOceanDrawsBefore;
		}
		if ( ocean_submit_ok ) {
			m_nForwardSubmitSucceededLaneMask |= T3D_RD_FORWARD_LANE_OCEAN;
		} else {
			m_nForwardSubmitFailedLaneMask |= T3D_RD_FORWARD_LANE_OCEAN;
		}
	}
	if ( ocean_submit_ok ) {
		rServices.m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
		return true;
	}
	// The native draw list is open here. Do not destroy or rebuild RD resources
	// until a later PREPARE callback, where resource mutation is legal.
	const uint32_t failures = rServices.m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
	if ( failures >= 8u ) {
		rServices.m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
	}
	return false;
}

bool Terrain3DForwardPassPreparer::prepareTerrainAndWorldLanes(
	Terrain3DForwardPassHost		  &rHost,
	t3dForwardPassServices_t		  &rServices,
	const t3dRdForwardPassContext_t	  &rContext,
	const t3dForwardLanePreparation_t &rPreparation ) {
	// Transparent belongs to the ocean lane, not World. Never overwrite a
	// real World failure with the intentional absence of a transparent World pass.
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT ) {
		return true;
	}
	if ( !rPreparation.m_bUnifiedWorldPass || !rPreparation.m_pWorld || !rPreparation.m_pPublication ) {
		m_nWorldPrepareFailureStage = 1u;
		m_nWorldPrepareFailurePassKind = ( uint32_t ) rContext.m_passKind;
		logWorldPrepareStateHot( rContext, 1u );
		return false;
	}
	uint32_t					nFailureStage = 0u;
	const t3dRdFrameSnapshot_t &snapshot = rPreparation.m_pPublication->m_snapshot;
	t3dRdEnvironmentBindings_t environmentBindings{};
	const t3dRdEnvironmentBindings_t *pEnvironment = NULL;
	if ( Terrain3DRdShaderLibrary::isEnvironmentFieldsEnabledCold() ) {
        if ( rServices.m_environmentField.acquireBindingsHot( environmentBindings ) ) {
			pEnvironment = &environmentBindings;
		}
	}
	bool						world_ok = false;
	if ( !rPreparation.m_bSharedVtReady ) {
		nFailureStage = 2u;
	} else if ( !rPreparation.m_pRegionMap || rPreparation.m_pRegionMap->m_bytes.is_empty() ) {
		nFailureStage = 3u;
	} else if ( rPreparation.m_pSharedVtBindings == NULL ) {
		nFailureStage = 4u;
	} else if ( !rServices.m_renderer.prepareForwardPass( rContext,
														  snapshot,
														  rPreparation.m_pRegionMap->m_bytes,
														  snapshot.m_nRegionMapGeneration,
														  rPreparation.m_pPublication->m_bindings,
														  rPreparation.m_pSharedVtBindings,
														  rPreparation.m_bHistoryValid,
														  rPreparation.m_bViewportReconfigured,
														  snapshot.m_bDominantSunOnly,
														  false ) ) {
		nFailureStage = 5u;
	} else {
		t3dRdWorldTerrainRasterBindings_t world_bindings = {};
		if ( !rServices.m_renderer.snapshotWorldRasterBindingsHot( world_bindings ) ) {
			nFailureStage = 6u;
		} else {
			if ( !nFailureStage && !rServices.m_worldRuntime.prepareRasterPass( rContext,
														 world_bindings,
														 *rPreparation.m_pSharedVtBindings,
														 &rServices.m_virtualTexture.get_gpu(), NULL, pEnvironment ) ) {
				nFailureStage = 7u;
			} else if ( !nFailureStage ) {
				world_ok = true;
			}
		}
	}
	m_bForwardPreparedHasWorld = world_ok;
	m_bForwardPreparedHasTerrain = false;
	m_bForwardPreparedHasInstances = false;
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ) {
		rHost.publishForwardWorldReadiness( rPreparation.m_pWorld->m_publication, world_ok );
	}
	if ( world_ok ) {
		m_nForwardPrepareReadyLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
		logWorldPrepareStateHot( rContext, 0u, NULL, &snapshot );
	} else {
		m_nWorldPrepareFailureStage = nFailureStage;
		m_nWorldPrepareFailurePassKind = ( uint32_t ) rContext.m_passKind;
		m_nForwardPrepareFailedLaneMask |= T3D_RD_FORWARD_LANE_WORLD;
		logWorldPrepareStateHot( rContext, nFailureStage,
								 nFailureStage == 5u ? &rServices.m_renderer : NULL,
								 &snapshot );
	}
	return world_ok;
}
