// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_FORWARD_PASS_PREPARER_H
#define TERRAIN3D_FORWARD_PASS_PREPARER_H

#include "Terrain3DTypes.h"

#include "rendering/rd/core/terrain_3d_render_publication_builder.h"
#include "rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/instances/terrain_3d_rd_instance_renderer.h"
#include "rendering/rd/environment/terrain_3d_rd_environment_field.h"
#include "rendering/rd/ocean/terrain_3d_rd_ocean_renderer.h"
#include "rendering/rd/world/runtime/terrain_3d_world_runtime.h"
#include "rendering/rd/vt/runtime/terrain_3d_virtual_texture.h"
class Terrain3DSharedVTRuntime;
class Terrain3DSharedVTRuntime;

// Lifecycle operations remain with the coordinator. The preparer cannot reach
// owner-thread scene state or control-plane storage through this contract.
class Terrain3DForwardPassHost
{
public:
	virtual bool							isForwardShaderReady( void ) const = 0;
	virtual bool							isForwardRendererReady( void ) const = 0;
	virtual const t3dRdFrameSnapshotSlot_t *acquireForwardSnapshot( void ) = 0;
	virtual t3dVTAcquireStatus_t			acquireForwardSharedVt( t3dSharedVTBindings_t &rBindings ) const = 0;
	virtual void publishForwardWorldReadiness( const Terrain3DWorldPublication &rPublication, bool bReady ) = 0;

protected:
	~Terrain3DForwardPassHost( void ) = default;
};

// Borrowed render-thread services for one callback. Policy is captured once;
// renderer lifetime and mutable status stay with the composition root.
typedef struct t3dForwardPassServices_s
{
	Terrain3DRenderPublicationBuilder &m_publicationBuilder;
	Terrain3DRdClipmapRenderer		  &m_renderer;
	Terrain3DRdOceanRenderer		  &m_oceanRenderer;
    Terrain3DRdEnvironmentField     &m_environmentField;
	Terrain3DWorldRuntime			  &m_worldRuntime;
	Terrain3DVirtualTexture			  &m_virtualTexture;
	Terrain3DSharedVTRuntime &m_sharedVtRuntime;
	t3dAtomicBool_t					  &m_bRenderEnabled;
	t3dAtomicBool_t					  &m_bRendererInitialized;
	t3dAtomicBool_t					  &m_bInstanceRendererInitialized;
	t3dAtomicBool_t					  &m_bOceanRendererInitialized;
	t3dAtomicBool_t					  &m_bRendererFailed;
	t3dAtomicU32_t					  &m_nConsecutiveRenderFailures;
	uint32_t						   m_nWorldUploadBudgetKbPolicy;
	uint32_t						   m_nWorldClusterBudgetPolicy;
	bool							   m_bWorldBoundaryDeferredPolicy;
	uint32_t						   m_nVtRequestBudgetPolicy;
	uint32_t						   m_nVtAdmissionBudgetPolicy;
} t3dForwardPassServices_t;

#include "Terrain3DConstants.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"

#include <godot_cpp/classes/rendering_device.hpp>

typedef struct t3dForwardLanePreparation_s
{
	const t3dRdFrameSnapshotSlot_t	*m_pPublication = NULL;
	const t3dRdRegionMapBank_t		*m_pRegionMap = NULL;
	const t3dRdInstanceMeshBank_t	*m_pMeshes = NULL;
	const t3dRdInstanceSceneBank_t	*m_pScene = NULL;
	const t3dWorldPublicationBank_t *m_pWorld = NULL;
	uint32_t						 m_nVisibilityMask = 0u;
	bool							 m_bHistoryValid = false;
	bool							 m_bViewportReconfigured = false;
	bool							 m_bUnifiedWorldPass = false;
	bool							 m_bEditorAuthoringMode = false;
	bool							 m_bSharedVtReady = false;
	const t3dSharedVTBindings_t		*m_pSharedVtBindings = NULL;
	bool							 m_bHasTerrain = false;
} t3dForwardLanePreparation_t;

// Borrowed read-only diagnostics. Consume within the owner/lifecycle boundary.
struct t3dForwardDiagnosticsView_t
{
	const bool						 &m_bForwardInstanceLaneExpected;
	const bool						 &m_bForwardInstancePrepareAttempted;
	const bool						 &m_bForwardInstancePrepareSucceeded;
	const bool						 &m_bForwardInstancePreparedHasDraws;
	const bool						 &m_bForwardInstanceSubmitAttempted;
	const bool						 &m_bForwardInstanceSubmitSucceeded;
	const bool						 &m_bForwardPreparedHasInstances;
	const bool						 &m_bForwardPreparedHasOcean;
	const bool						 &m_bForwardPreparedHasTerrain;
	const bool						 &m_bForwardPreparedInstanceDepthPrepassActive;
	const uint64_t					 &m_nDepthPassCallbacks;
	const uint64_t					 &m_nEditorPreviewPrepares;
	const uint64_t					 &m_nForwardInstanceSubmitDraws;
	const uint32_t					 &m_nForwardLanePassKind;
	const uint64_t					 &m_nForwardLaneScenePass;
	const uint64_t					 &m_nForwardPassCallbacks;
	const uint32_t					 &m_nForwardPrepareFailedLaneMask;
	const uint32_t					 &m_nForwardPrepareReadyLaneMask;
	const uint32_t					 &m_nForwardPrepareRequestedLaneMask;
	const uint64_t					 &m_nForwardPreparedViewportKey;
	const uint32_t					 &m_nForwardPublishedInstanceCellCount;
	const uint32_t					 &m_nForwardPublishedInstanceCount;
	const uint32_t					 &m_nForwardSubmitAttemptedLaneMask;
	const uint32_t					 &m_nForwardSubmitFailedLaneMask;
	const uint32_t					 &m_nForwardSubmitSucceededLaneMask;
	const uint64_t					 &m_nForwardViewportEvictions;
	const uint64_t					 &m_nForwardViewportReconfigurations;
	const uint64_t					 &m_nInstanceDepthPrepareAttempts;
	const uint64_t					 &m_nInstanceDepthPrepareSuccesses;
	const uint64_t					 &m_nInstanceDepthPrepassFallbacks;
	const uint64_t					 &m_nInstanceDepthSubmitAttempts;
	const uint64_t					 &m_nInstanceDepthSubmitSuccesses;
	const uint64_t					 &m_nInstanceForwardPrepareFailures;
	const uint64_t					 &m_nInstanceForwardSubmitFailures;
	const uint64_t					 &m_nInstanceLaneUnavailablePrepares;
	const uint64_t					 &m_nInstanceLastSuccessfulDepthScenePass;
	const uint64_t					 &m_nInstanceOpaqueDepthPrepassRequested;
	const uint64_t					 &m_nInstanceOpaqueDepthPrepassUsed;
	const uint64_t					 &m_nInstanceOpaquePrepareAttempts;
	const uint64_t					 &m_nInstanceOpaquePrepareSuccesses;
	const uint64_t					 &m_nInstanceOpaqueSubmitAttempts;
	const uint64_t					 &m_nInstanceOpaqueSubmitSuccesses;
	const uint64_t					 &m_nInstancePreparedWithoutDraws;
	const int64_t					 &m_nLastForwardDrawList;
	const int64_t					 &m_nLastForwardFramebufferFormat;
	const uint32_t					 &m_nLastForwardPassKind;
	const uint32_t					 &m_nLastForwardSampleCount;
	const uint64_t					 &m_nLastForwardScenePass;
	const uint32_t					 &m_nLastForwardViewCount;
	const uint64_t					 &m_nMotionPassCallbacks;
	const uint64_t					 &m_nOpaquePassCallbacks;
	const uint64_t					 &m_nShadowPassCallbacks;
	const uint64_t					 &m_nStrictWorldAuthorityFailures;
	const uint64_t					 &m_nTerrainRecoverablePrepareFailures;
	const t3dVTAcquireStatus_t		 &m_eLastSharedVtAcquireStatus;
	const uint64_t					 &m_nSharedVtAcquireFailures;
	const uint32_t					 &m_nWorldPrepareFailureStage;
	const uint32_t					 &m_nWorldPrepareFailurePassKind;
	const uint64_t					 &m_nTerrainVtResidencyRequestAttempts;
	const uint64_t					 &m_nTerrainVtResidencyRequestsAccepted;
	const uint64_t					 &m_nTransparentPassCallbacks;
	const t3dRdForwardDrawBaseline_t *m_szForwardDrawBaseline;
};

class Terrain3DForwardPassPreparer
{
	CLASS_NAME_STATIC( "Terrain3DForwardPassPreparer" );

private:
	uint64_t					m_nForwardPassCallbacks = 0u;
	uint64_t					m_nDepthPassCallbacks = 0u;
	uint64_t					m_nOpaquePassCallbacks = 0u;
	uint64_t					m_nShadowPassCallbacks = 0u;
	uint64_t					m_nMotionPassCallbacks = 0u;
	uint64_t					m_nTransparentPassCallbacks = 0u;
	uint64_t					m_nInstanceForwardPrepareFailures = 0u;
	uint64_t					m_nInstanceForwardSubmitFailures = 0u;
	uint64_t					m_nInstanceDepthPrepareAttempts = 0u;
	uint64_t					m_nInstanceDepthPrepareSuccesses = 0u;
	uint64_t					m_nInstanceDepthSubmitAttempts = 0u;
	uint64_t					m_nInstanceDepthSubmitSuccesses = 0u;
	uint64_t					m_nInstanceOpaquePrepareAttempts = 0u;
	uint64_t					m_nInstanceOpaquePrepareSuccesses = 0u;
	uint64_t					m_nInstanceOpaqueSubmitAttempts = 0u;
	uint64_t					m_nInstanceOpaqueSubmitSuccesses = 0u;
	uint64_t					m_nInstanceDepthPrepassFallbacks = 0u;
	uint64_t					m_nInstanceOpaqueDepthPrepassRequested = 0u;
	uint64_t					m_nInstanceOpaqueDepthPrepassUsed = 0u;
	uint64_t					m_nInstanceLastSuccessfulDepthScenePass = UINT64_MAX;
	bool						m_bForwardPreparedInstanceDepthPrepassActive = false;
	uint64_t					m_nLastForwardScenePass = 0u;
	int64_t						m_nLastForwardDrawList = -1;
	uint32_t					m_nLastForwardPassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint32_t					m_nLastForwardViewCount = 0u;
	uint32_t					m_nLastForwardSampleCount = 0u;
	int64_t						m_nLastForwardFramebufferFormat = RenderingDevice::INVALID_ID;
	uint32_t					m_nForwardPrepareRequestedLaneMask = 0u;
	uint32_t					m_nForwardPrepareReadyLaneMask = 0u;
	uint32_t					m_nForwardPrepareFailedLaneMask = 0u;
	uint32_t					m_nForwardSubmitAttemptedLaneMask = 0u;
	uint32_t					m_nForwardSubmitSucceededLaneMask = 0u;
	uint32_t					m_nForwardSubmitFailedLaneMask = 0u;
	uint64_t					m_nForwardLaneScenePass = 0u;
	uint32_t					m_nForwardLanePassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint32_t					m_nForwardPublishedInstanceCount = 0u;
	uint32_t					m_nForwardPublishedInstanceCellCount = 0u;
	bool						m_bForwardInstanceLaneExpected = false;
	bool						m_bForwardInstancePrepareAttempted = false;
	bool						m_bForwardInstancePrepareSucceeded = false;
	bool						m_bForwardInstancePreparedHasDraws = false;
	bool						m_bForwardInstanceSubmitAttempted = false;
	bool						m_bForwardInstanceSubmitSucceeded = false;
	uint64_t					m_nForwardInstanceSubmitDraws = 0u;
	uint64_t					m_nInstanceLaneUnavailablePrepares = 0u;
	uint64_t					m_nInstancePreparedWithoutDraws = 0u;
	uint64_t					m_nLastWorldVtResidencyRequestFrame = UINT64_MAX;
	size_t						m_nWorldVtCandidateCursor = 0u;
	uint32_t					m_nWorldVtCandidateSweep = 0u;
	uint64_t					m_nLastTerrainVtResidencyRequestFrame = UINT64_MAX;
	uint64_t					m_nTerrainVtResidencyRequestAttempts = 0u;
	uint64_t					m_nTerrainVtResidencyRequestsAccepted = 0u;
	uint64_t					m_nTerrainRecoverablePrepareFailures = 0u;
	t3dVTAcquireStatus_t		m_eLastSharedVtAcquireStatus = T3D_VT_ACQUIRE_UNPUBLISHED;
	uint64_t					m_nSharedVtAcquireFailures = 0u;
	uint32_t					m_nWorldPrepareFailureStage = 0u;
	uint32_t					m_nWorldPrepareFailurePassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint32_t					m_szLastLoggedWorldPrepareStage[T3D_RD_FORWARD_PASS_MAX]{};
	bool						m_szLoggedWorldPrepareStage[T3D_RD_FORWARD_PASS_MAX]{};
	uint64_t					m_nStrictWorldAuthorityFailures = 0u;
	uint64_t					m_nEditorPreviewPrepares = 0u;
	t3dRdForwardDrawBaseline_t	m_szForwardDrawBaseline[T3D_RD_FORWARD_PASS_MAX]{};
	t3dRdForwardViewportState_t m_szForwardViewportStates[T3D_FORWARD_VIEWPORT_SLOT_COUNT]{};
	uint64_t					m_nForwardPreparedScenePass = 0u;
	uint64_t					m_nForwardPreparedViewportKey = 0u;
	uint64_t					m_nForwardViewportReconfigurations = 0u;
	uint64_t					m_nForwardViewportEvictions = 0u;
	bool						m_bForwardPreparedHasTerrain = false;
	bool						m_bForwardPreparedHasInstances = false;
	bool						m_bForwardPreparedHasOcean = false;
	bool						m_bForwardPreparedHasWorld = false;

	void logWorldPrepareStateHot( const t3dRdForwardPassContext_t &rContext, uint32_t nStage );
	bool
	prepareLifecycle( const t3dRdForwardPassContext_t &rContext, bool &rHistoryValid, bool &rViewportReconfigured );
	bool prepare( Terrain3DForwardPassHost		  &rHost,
				  t3dForwardPassServices_t		  &rServices,
				  const t3dRdForwardPassContext_t &rContext );
	bool submit( t3dForwardPassServices_t &rServices, const t3dRdForwardPassContext_t &rContext );
	bool prepareTerrainAndWorldLanes( Terrain3DForwardPassHost			&rHost,
									  t3dForwardPassServices_t			&rServices,
									  const t3dRdForwardPassContext_t	&rContext,
									  const t3dForwardLanePreparation_t &rPreparation );

public:
	t3dForwardDiagnosticsView_t getDiagnosticsView(
		void ) const {
		return { m_bForwardInstanceLaneExpected,
				 m_bForwardInstancePrepareAttempted,
				 m_bForwardInstancePrepareSucceeded,
				 m_bForwardInstancePreparedHasDraws,
				 m_bForwardInstanceSubmitAttempted,
				 m_bForwardInstanceSubmitSucceeded,
				 m_bForwardPreparedHasInstances,
				 m_bForwardPreparedHasOcean,
				 m_bForwardPreparedHasTerrain,
				 m_bForwardPreparedInstanceDepthPrepassActive,
				 m_nDepthPassCallbacks,
				 m_nEditorPreviewPrepares,
				 m_nForwardInstanceSubmitDraws,
				 m_nForwardLanePassKind,
				 m_nForwardLaneScenePass,
				 m_nForwardPassCallbacks,
				 m_nForwardPrepareFailedLaneMask,
				 m_nForwardPrepareReadyLaneMask,
				 m_nForwardPrepareRequestedLaneMask,
				 m_nForwardPreparedViewportKey,
				 m_nForwardPublishedInstanceCellCount,
				 m_nForwardPublishedInstanceCount,
				 m_nForwardSubmitAttemptedLaneMask,
				 m_nForwardSubmitFailedLaneMask,
				 m_nForwardSubmitSucceededLaneMask,
				 m_nForwardViewportEvictions,
				 m_nForwardViewportReconfigurations,
				 m_nInstanceDepthPrepareAttempts,
				 m_nInstanceDepthPrepareSuccesses,
				 m_nInstanceDepthPrepassFallbacks,
				 m_nInstanceDepthSubmitAttempts,
				 m_nInstanceDepthSubmitSuccesses,
				 m_nInstanceForwardPrepareFailures,
				 m_nInstanceForwardSubmitFailures,
				 m_nInstanceLaneUnavailablePrepares,
				 m_nInstanceLastSuccessfulDepthScenePass,
				 m_nInstanceOpaqueDepthPrepassRequested,
				 m_nInstanceOpaqueDepthPrepassUsed,
				 m_nInstanceOpaquePrepareAttempts,
				 m_nInstanceOpaquePrepareSuccesses,
				 m_nInstanceOpaqueSubmitAttempts,
				 m_nInstanceOpaqueSubmitSuccesses,
				 m_nInstancePreparedWithoutDraws,
				 m_nLastForwardDrawList,
				 m_nLastForwardFramebufferFormat,
				 m_nLastForwardPassKind,
				 m_nLastForwardSampleCount,
				 m_nLastForwardScenePass,
				 m_nLastForwardViewCount,
				 m_nMotionPassCallbacks,
				 m_nOpaquePassCallbacks,
				 m_nShadowPassCallbacks,
				 m_nStrictWorldAuthorityFailures,
				 m_nTerrainRecoverablePrepareFailures,
				 m_eLastSharedVtAcquireStatus,
				 m_nSharedVtAcquireFailures,
				 m_nWorldPrepareFailureStage,
				 m_nWorldPrepareFailurePassKind,
				 m_nTerrainVtResidencyRequestAttempts,
				 m_nTerrainVtResidencyRequestsAccepted,
				 m_nTransparentPassCallbacks,
				 m_szForwardDrawBaseline };
	}

	void   reset( void );
	bool   render( Terrain3DForwardPassHost		   &rHost,
				   t3dForwardPassServices_t		   &rServices,
				   const t3dRdForwardPassContext_t &rContext );
	size_t getRetainedCpuBytes(
		void ) const {
		return sizeof( m_szForwardViewportStates );
	}
};

#endif // TERRAIN3D_FORWARD_PASS_PREPARER_H
