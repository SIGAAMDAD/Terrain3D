// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_FORWARD_PASS_PREPARER_H
#define TERRAIN3D_FORWARD_PASS_PREPARER_H

#include "Terrain3DConstants.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <godot_cpp/classes/rendering_device.hpp>

class Terrain3DRenderCoordinator;
class Terrain3DDiagnosticsPublisher;

class Terrain3DForwardPassPreparer {
	friend class Terrain3DRenderCoordinator;
	friend class Terrain3DDiagnosticsPublisher;

	CLASS_NAME_STATIC( "Terrain3DForwardPassPreparer" );

private:
	typedef enum t3dRdForwardLaneMask_e : uint32_t {
		T3D_RD_FORWARD_LANE_TERRAIN = 1u << 0u,
		T3D_RD_FORWARD_LANE_INSTANCES = 1u << 1u,
		T3D_RD_FORWARD_LANE_OCEAN = 1u << 2u,
		T3D_RD_FORWARD_LANE_WORLD = 1u << 4u,
	} t3dRdForwardLaneMask_t;

	typedef struct t3dRdForwardViewportState_s {
		uint64_t m_nViewportKey = 0u;
		Vector2i m_renderSize;
		Rect2i m_renderRegion;
		uint64_t m_nLastRenderFrame = UINT64_MAX;
		uint64_t m_nReconfiguredRenderFrame = UINT64_MAX;
		uint32_t m_nViewCount = 0u;
		uint32_t m_nSampleCount = 0u;
		bool m_bValid = false;
	} t3dRdForwardViewportState_t;

	typedef struct t3dRdForwardDrawBaseline_s {
		uint64_t m_nSubmitCallbacks = 0u;
		uint64_t m_nTerrainDraws = 0u;
		uint64_t m_nInstanceDraws = 0u;
		uint64_t m_nOceanDraws = 0u;
		uint64_t m_nWorldDraws = 0u;
	} t3dRdForwardDrawBaseline_t;

	uint64_t m_nForwardPassCallbacks = 0u;
	uint64_t m_nDepthPassCallbacks = 0u;
	uint64_t m_nOpaquePassCallbacks = 0u;
	uint64_t m_nShadowPassCallbacks = 0u;
	uint64_t m_nMotionPassCallbacks = 0u;
	uint64_t m_nTransparentPassCallbacks = 0u;
	uint64_t m_nInstanceForwardPrepareFailures = 0u;
	uint64_t m_nInstanceForwardSubmitFailures = 0u;
	uint64_t m_nInstanceDepthPrepareAttempts = 0u;
	uint64_t m_nInstanceDepthPrepareSuccesses = 0u;
	uint64_t m_nInstanceDepthSubmitAttempts = 0u;
	uint64_t m_nInstanceDepthSubmitSuccesses = 0u;
	uint64_t m_nInstanceOpaquePrepareAttempts = 0u;
	uint64_t m_nInstanceOpaquePrepareSuccesses = 0u;
	uint64_t m_nInstanceOpaqueSubmitAttempts = 0u;
	uint64_t m_nInstanceOpaqueSubmitSuccesses = 0u;
	uint64_t m_nInstanceDepthPrepassFallbacks = 0u;
	uint64_t m_nInstanceOpaqueDepthPrepassRequested = 0u;
	uint64_t m_nInstanceOpaqueDepthPrepassUsed = 0u;
	uint64_t m_nInstanceLastSuccessfulDepthScenePass = UINT64_MAX;
	bool m_bForwardPreparedInstanceDepthPrepassActive = false;
	uint64_t m_nLastForwardScenePass = 0u;
	int64_t m_nLastForwardDrawList = -1;
	uint32_t m_nLastForwardPassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint32_t m_nLastForwardViewCount = 0u;
	uint32_t m_nLastForwardSampleCount = 0u;
	int64_t m_nLastForwardFramebufferFormat = RenderingDevice::INVALID_ID;
	uint32_t m_nForwardPrepareRequestedLaneMask = 0u;
	uint32_t m_nForwardPrepareReadyLaneMask = 0u;
	uint32_t m_nForwardPrepareFailedLaneMask = 0u;
	uint32_t m_nForwardSubmitAttemptedLaneMask = 0u;
	uint32_t m_nForwardSubmitSucceededLaneMask = 0u;
	uint32_t m_nForwardSubmitFailedLaneMask = 0u;
	uint64_t m_nForwardLaneScenePass = 0u;
	uint32_t m_nForwardLanePassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint32_t m_nForwardPublishedInstanceCount = 0u;
	uint32_t m_nForwardPublishedInstanceCellCount = 0u;
	bool m_bForwardInstanceLaneExpected = false;
	bool m_bForwardInstancePrepareAttempted = false;
	bool m_bForwardInstancePrepareSucceeded = false;
	bool m_bForwardInstancePreparedHasDraws = false;
	bool m_bForwardInstanceSubmitAttempted = false;
	bool m_bForwardInstanceSubmitSucceeded = false;
	uint64_t m_nForwardInstanceSubmitDraws = 0u;
	uint64_t m_nInstanceLaneUnavailablePrepares = 0u;
	uint64_t m_nInstancePreparedWithoutDraws = 0u;
	uint64_t m_nLastTerrainVtResidencyRequestFrame = UINT64_MAX;
	uint64_t m_nTerrainVtResidencyRequestAttempts = 0u;
	uint64_t m_nTerrainVtResidencyRequestsAccepted = 0u;
	uint64_t m_nTerrainRecoverablePrepareFailures = 0u;
	uint64_t m_nStrictWorldAuthorityFailures = 0u;
	uint64_t m_nEditorLegacyPreviewPrepares = 0u;
	uint64_t m_nRuntimeLegacySubmitAttempts = 0u;
	t3dRdForwardDrawBaseline_t m_szForwardDrawBaseline[T3D_RD_FORWARD_PASS_MAX]{};
	t3dRdForwardViewportState_t m_szForwardViewportStates[s_nForwardViewportSlotCount]{};
	uint64_t m_nForwardPreparedScenePass = 0u;
	uint64_t m_nForwardPreparedViewportKey = 0u;
	uint64_t m_nForwardViewportReconfigurations = 0u;
	uint64_t m_nForwardViewportEvictions = 0u;
	bool m_bForwardPreparedHasTerrain = false;
	bool m_bForwardPreparedHasInstances = false;
	bool m_bForwardPreparedHasOcean = false;
	bool m_bForwardPreparedHasWorld = false;

	bool prepareLifecycle( const t3dRdForwardPassContext_t &rContext, bool &rHistoryValid, bool &rViewportReconfigured );

public:
	void reset( void );
	bool render( Terrain3DRenderCoordinator &rCoordinator, const t3dRdForwardPassContext_t &rContext );
	size_t getRetainedCpuBytes( void ) const {
		return sizeof( m_szForwardViewportStates );
	}
};

#endif // TERRAIN3D_FORWARD_PASS_PREPARER_H
