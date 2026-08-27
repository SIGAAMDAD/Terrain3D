// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RENDER_COORDINATOR_H
#define TERRAIN3D_RENDER_COORDINATOR_H

#include "assets/generated_texture.h"
#include "rendering/rd/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/terrain_3d_rd_compositor_effect.h"
#include "rendering/rd/terrain_3d_rd_instance_renderer.h"
#include "rendering/rd/terrain_3d_rd_ocean_renderer.h"
#include "rendering/rd/terrain_3d_rd_shader_library.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <mutex>
#include <vector>

class Terrain3D;
class Terrain3DMaterial;
class Terrain3DRdForwardPlusBridgeAdapter;

class Terrain3DRenderCoordinator
{
	CLASS_NAME_STATIC( "Terrain3DRenderCoordinator" );

private:
	Terrain3D *m_pTerrain = nullptr;
	Ref<Terrain3DRdCompositorEffect> m_effect;
	Ref<Compositor> m_attachedCompositor;
	uint64_t m_nAttachedCameraId = 0u;
	bool m_bCreatedCompositor = false;

	typedef struct t3dRdPublishedResources_s
	{
		RID m_hHeightTextureRd;
		RID m_hDerivedTextureRd;
		RID m_hControlTextureRd;
		RID m_hColorTextureRd;
		RID m_hMiddleCompositeTextureRd;
		RID m_hTerrainShadowMaskTextureRd;
		RID m_hMiddleHeightTextureRd;
		RID m_hMiddleHeightRegionMapTextureRd;
		RID m_hFarHeightTextureRd;
		RID m_hFarColorTextureRd;
		RID m_hAlbedoTextureRd;
		RID m_hFarAlbedoTextureRd;
		RID m_hNormalTextureRd;
		RID m_hSurfaceTextureRd;
		RID m_hMaterialDisplacementTextureRd;
		RID m_hDisplacementTextureRd;
		RID m_hWorldNoiseTextureRd;
		RID m_hMacroNoiseTextureRd;
		RID m_hEditorBrushTextureRd;
	} t3dRdPublishedResources_t;

	typedef struct t3dRdFrameSnapshotSlot_s
	{
		t3dRdFrameSnapshot_t m_snapshot;
		PackedByteArray m_regionMapBytes;
		t3dRdPublishedResources_t m_resources;
		// Variable publication payloads live in the reclaimed atomic frame slot.
		// The render thread therefore observes stable storage without reference
		// counting, heap ownership transfers, or cross-frame lifetime ambiguity.
		t3dRdMeshSourceSnapshot_t m_instanceMeshes;
		t3dRdInstanceSceneSnapshot_t m_instanceScene;
		t3dRdOceanTopologySnapshot_t m_oceanTopology;
		bool m_bInstanceMeshesValid = false;
		bool m_bInstanceSceneValid = false;
		bool m_bOceanTopologyValid = false;
	} t3dRdFrameSnapshotSlot_t;

	std::array<t3dRdFrameSnapshotSlot_t, s_nRdFrameSnapshotSlotCount> m_snapshotSlots{};
	std::atomic_uint64_t m_nSnapshotExchangeToken{ t3dRdMakeFrameSnapshotToken( 1u, 0u, false ) };
	uint32_t m_nSnapshotWriteIndex = 2u;
	uint32_t m_nSnapshotRenderIndex = 0u;
	uint64_t m_nSnapshotRenderGeneration = 0u;
	bool m_bSnapshotRenderValid = false;
	uint64_t m_nNextSnapshotGeneration = 1u;
	std::atomic_uint32_t m_nPublishedSnapshotIndex{ s_nRdInvalidFrameSnapshotIndex };
	std::atomic_uint64_t m_nPublishedSnapshotGeneration{ 0u };
	std::atomic_uint64_t m_nConsumedSnapshotGeneration{ 0u };
	std::atomic_uint64_t m_nSnapshotPublications{ 0u };
	std::atomic_uint64_t m_nSnapshotConsumes{ 0u };
	std::atomic_uint64_t m_nSnapshotReuses{ 0u };
	std::atomic_uint64_t m_nSkippedSnapshotGenerations{ 0u };
	std::atomic_uint64_t m_nStaleSnapshots{ 0u };
	std::atomic_uint64_t m_nLastStaleSnapshotToken{ 0u };

	PackedByteArray m_regionMapBytes;
	uint64_t m_nRegionTopologyGeneration = 0u;
	uint64_t m_nRegionLightingSignature = 0u;
	uint64_t m_nRegionMapGeneration = 0u;
	uint64_t m_nPhysicalRegionMapGeneration = 0u;
	uint64_t m_nLastRegionCacheSourceGeneration = 0u;
	uint64_t m_nRegionCacheGeneration = 0u;
	uint64_t m_nLastPatchFingerprint = 0u;
	uint64_t m_nPatchGeneration = 0u;
	std::array<t3dRdPatch_t, s_nMaxClipmapLevels> m_previousPatches{};
	uint32_t m_nPreviousPatchCount = 0u;
	uint64_t m_nLastMaterialFingerprint = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nTerrainTextureGeneration = 0u;
	uint64_t m_nMaterialTextureGeneration = 0u;
	t3dRdPublishedResources_t m_lastPublishedResources;
	bool m_bRegionMapInitialized = false;
	bool m_bRegionCacheInitialized = false;
	bool m_bPatchStateInitialized = false;
	bool m_bMaterialStateInitialized = false;
	bool m_bPublishedResourcesInitialized = false;

	Terrain3DGeneratedTexture m_worldNoiseCache;
	Terrain3DGeneratedTexture m_macroNoiseTexture;
	Terrain3DGeneratedTexture m_editorBrushTexture;
	uint64_t m_nMacroNoiseSourceId = 0u;
	uint64_t m_nEditorBrushSourceId = 0u;
	Terrain3DRdShaderLibrary m_shaderLibrary;
	Terrain3DRdClipmapRenderer m_renderer;
	Terrain3DRdInstanceRenderer m_instanceRenderer;
	Terrain3DRdOceanRenderer m_oceanRenderer;
	Terrain3DRdInstanceRenderer m_previewRenderer;
	bool m_bPreviewRendererInitialized = false;
	t3dRdMeshSourceSnapshot_t m_cachedPreviewMeshes;
	bool m_bCachedPreviewMeshesValid = false;
	uint64_t m_nPreviewMeshFingerprint = 0u;
	uint64_t m_nPreviewMeshGeneration = 0u;
	std::mutex m_previewCallMutex;
	std::mutex m_previewStateMutex;
	t3dRdMeshSourceSnapshot_t m_pendingPreviewMeshes;
	bool m_bPendingPreviewMeshesValid = false;
	uint16_t m_nPendingPreviewAssetId = 0u;
	Vector2i m_pendingPreviewSize;
	Ref<Image> m_pendingPreviewImage;
	String m_pendingPreviewError;
	t3dRdOceanTopologySnapshot_t m_cachedOceanTopology;
	bool m_bCachedOceanTopologyValid = false;
	uint64_t m_nOceanTopologyGeneration = 0u;
	t3dRdMeshSourceSnapshot_t m_cachedInstanceMeshes;
	t3dRdInstanceSceneSnapshot_t m_cachedRegularInstanceScene;
	t3dRdInstanceSceneSnapshot_t m_cachedHlodInstanceScene;
	t3dRdInstanceSceneSnapshot_t m_cachedInstanceScene;
	bool m_bCachedInstanceMeshesValid = false;
	bool m_bCachedRegularInstanceSceneValid = false;
	bool m_bCachedHlodInstanceSceneValid = false;
	bool m_bCachedInstanceSceneValid = false;
	std::vector<t3dRdExternalMeshSource_t> m_cachedHlodMeshSources;
	uint64_t m_nInstanceMeshFingerprint = 0u;
	uint64_t m_nInstanceMeshGeneration = 0u;
	uint64_t m_nInstanceSceneGeneration = 0u;
	uint64_t m_nHlodSceneGeneration = 0u;
	uint64_t m_nCombinedInstanceSceneGeneration = 0u;
	std::atomic_bool m_bRendererInitialized{ false };
	std::atomic_bool m_bRenderEnabled{ false };
	std::atomic_bool m_bRendererFailed{ false };
	bool m_bFailureLatched = false;
	bool m_bForwardBridgeAttached = false;
	bool m_bCompositorDebugEnabled = false;
	bool m_bMotionDebugEnabled = false;
	std::atomic_uint64_t m_nCallbackCount{ 0u };
	uint64_t m_nForwardPassCallbacks = 0u;
	uint64_t m_nDepthPassCallbacks = 0u;
	uint64_t m_nOpaquePassCallbacks = 0u;
	uint64_t m_nShadowPassCallbacks = 0u;
	uint64_t m_nMotionPassCallbacks = 0u;
	uint64_t m_nTransparentPassCallbacks = 0u;
	uint64_t m_nLastForwardScenePass = 0u;
	int64_t m_nLastForwardDrawList = -1;
	uint32_t m_nLastForwardPassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint32_t m_nLastForwardViewCount = 0u;
	uint32_t m_nLastForwardSampleCount = 0u;
	int64_t m_nLastForwardFramebufferFormat = RenderingDevice::INVALID_ID;
	static constexpr uint32_t s_nForwardViewportSlotCount = 8u;
	typedef struct t3dRdForwardViewportState_s
	{
		uint64_t m_nViewportKey = 0u;
		RID m_hFramebuffer;
		int64_t m_nFramebufferFormat = RenderingDevice::INVALID_ID;
		Vector2i m_renderSize;
		Rect2i m_renderRegion;
		uint64_t m_nLastRenderFrame = UINT64_MAX;
		uint64_t m_nReconfiguredRenderFrame = UINT64_MAX;
		uint32_t m_nViewCount = 0u;
		uint32_t m_nSampleCount = 0u;
		bool m_bValid = false;
	} t3dRdForwardViewportState_t;
	t3dRdForwardViewportState_t m_szForwardViewportStates[s_nForwardViewportSlotCount]{};
	uint64_t m_nForwardPreparedScenePass = 0u;
	uint64_t m_nForwardPreparedViewportKey = 0u;
	uint64_t m_nForwardViewportReconfigurations = 0u;
	uint64_t m_nForwardViewportEvictions = 0u;
	bool m_bForwardPreparedHasTerrain = false;
	bool m_bForwardPreparedHasOcean = false;
	std::atomic_uint32_t m_nConsecutiveRenderFailures{ 0u };
	mutable std::mutex m_diagnosticsMutex;
	String m_initializationError;

	uint64_t hashBytes( const void *pData, size_t nBytes ) const;
	uint64_t advanceGeneration( uint64_t nGeneration ) const;
	void buildMaterialState( t3dRdFrameSnapshot_t& rSnapshot ) const;
	void buildEditorOverlayState( t3dRdFrameSnapshot_t& rSnapshot, const Terrain3DMaterial *pMaterial ) const;
	bool ensureWorldNoiseCache( const Terrain3DMaterial *pMaterial );
	bool ensureMacroNoiseTexture( const Terrain3DMaterial *pMaterial );
	bool ensureEditorBrushTexture( const Terrain3DMaterial *pMaterial );
	void publishSnapshot( void );
	const t3dRdFrameSnapshotSlot_t *acquireSnapshotOnRenderThread( void );
	void resetSnapshotPublication( void );
	void recordStaleSnapshot( uint64_t nToken );
	void attachEffectToActiveCamera( void );
	void detachEffect( void );
	bool initializeRendererOnRenderThread( void );
	bool attachForwardBridge( void );
	void detachForwardBridge( void );
	bool isForwardBridgeReady( void ) const;
	void updateRenderRoute( void );
	bool prepareForwardLifecycle( const t3dRdForwardPassContext_t& rContext,
			bool& rHistoryValid,
			bool& rViewportReconfigured );
	void resetForwardLifecycle( void );

public:
	Terrain3DRenderCoordinator( void ) = default;
	~Terrain3DRenderCoordinator( void );

	void initialize( Terrain3D *pTerrain );
	void initializeOcean( void );
	void shutdown( void );
	void update( void );
	void render( RenderData *pRenderData );
	bool renderForwardPass( const t3dRdForwardPassContext_t& rContext );
	void shutdownRendererOnRenderThread( void );
	void renderPreviewOnRenderThread( void );
	Ref<Image> createMeshPreview( uint16_t nAssetId, const Vector2i& rSize );
	Dictionary getDiagnostics( void ) const;
};

#endif // TERRAIN3D_RENDER_COORDINATOR_H
