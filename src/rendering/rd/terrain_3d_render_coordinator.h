// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_COORDINATOR_H
#define TERRAIN3D_RENDER_COORDINATOR_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "assets/generated_texture.h"
#include "rendering/rd/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/terrain_3d_rd_compositor_effect.h"
#include "rendering/rd/terrain_3d_rd_instance_renderer.h"
#include "rendering/rd/terrain_3d_rd_ocean_renderer.h"
#include "rendering/rd/render_world/terrain_3d_render_world_runtime.h"
#include "rendering/rd/terrain_3d_rd_shader_library.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include "eastl_config.h"
#include <stddef.h>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/variant/dictionary.hpp>
class Terrain3D;
class Terrain3DMaterial;
class Terrain3DRdForwardPlusBridgeAdapter;

class Terrain3DRenderCoordinator {
	CLASS_NAME_STATIC( "Terrain3DRenderCoordinator" );

private:
	Terrain3D *m_pTerrain = NULL;
	Terrain3DMemoryZone *m_pRuntimeAllocatorZone = NULL;
	t3dAllocatorOverflowPolicy_t m_eRuntimeAllocatorOverflowPolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT;
	bool m_bRuntimeAllocatorBound = false;
	Ref<Terrain3DRdCompositorEffect> m_effect;
	Ref<Compositor> m_attachedCompositor;
	uint64_t m_nAttachedCameraId = 0u;
	bool m_bCreatedCompositor = false;

	typedef struct t3dRdFrameSnapshotSlot_s {
		t3dRdFrameSnapshot_t m_snapshot;
		t3dRdTerrainBindings_t m_bindings;
		uint32_t m_nRegionMapBank = s_nRdInvalidFrameSnapshotIndex;
		uint32_t m_nInstanceMeshBank = s_nRdInvalidFrameSnapshotIndex;
		uint32_t m_nInstanceSceneBank = s_nRdInvalidFrameSnapshotIndex;
		uint32_t m_nOceanTopologyBank = s_nRdInvalidFrameSnapshotIndex;
	} t3dRdFrameSnapshotSlot_t;

	typedef struct t3dRdRegionMapBank_s {
		uint64_t m_nGeneration = 0u;
		PackedByteArray m_bytes;
	} t3dRdRegionMapBank_t;

	typedef struct t3dRdInstanceMeshBank_s {
		uint64_t m_nGeneration = 0u;
		t3dRdMeshSourceSnapshot_t m_snapshot;
	} t3dRdInstanceMeshBank_t;

	typedef struct t3dRdInstanceSceneBank_s {
		uint64_t m_nGeneration = 0u;
		t3dRdInstanceSceneSnapshot_t m_snapshot;
	} t3dRdInstanceSceneBank_t;

	typedef struct t3dRdOceanTopologyBank_s {
		uint64_t m_nGeneration = 0u;
		t3dRdOceanTopologySnapshot_t m_snapshot;
	} t3dRdOceanTopologyBank_t;

	t3dRdFrameSnapshotSlot_t m_snapshotSlots[s_nRdFrameSnapshotSlotCount]{};
	t3dRdRegionMapBank_t m_regionMapBanks[s_nRdFrameSnapshotSlotCount]{};
	t3dRdInstanceMeshBank_t m_instanceMeshBanks[s_nRdFrameSnapshotSlotCount]{};
	t3dRdInstanceSceneBank_t m_instanceSceneBanks[s_nRdFrameSnapshotSlotCount]{};
	t3dRdOceanTopologyBank_t m_oceanTopologyBanks[s_nRdFrameSnapshotSlotCount]{};
	t3dAtomicU32_t m_nRegionMapReaderBank{ s_nRdInvalidFrameSnapshotIndex };
	t3dAtomicU32_t m_nInstanceMeshReaderBank{ s_nRdInvalidFrameSnapshotIndex };
	t3dAtomicU32_t m_nInstanceSceneReaderBank{ s_nRdInvalidFrameSnapshotIndex };
	t3dAtomicU32_t m_nOceanTopologyReaderBank{ s_nRdInvalidFrameSnapshotIndex };
	uint32_t m_nCurrentRegionMapBank = s_nRdInvalidFrameSnapshotIndex;
	uint32_t m_nCurrentInstanceMeshBank = s_nRdInvalidFrameSnapshotIndex;
	uint32_t m_nCurrentInstanceSceneBank = s_nRdInvalidFrameSnapshotIndex;
	uint32_t m_nCurrentOceanTopologyBank = s_nRdInvalidFrameSnapshotIndex;
	t3dAtomicU64_t m_nSnapshotExchangeToken{ t3dRdMakeFrameSnapshotToken( 1u, 0u, false ) };
	uint32_t m_nSnapshotWriteIndex = 2u;
	uint32_t m_nSnapshotRenderIndex = 0u;
	uint64_t m_nSnapshotRenderGeneration = 0u;
	bool m_bSnapshotRenderValid = false;
	uint64_t m_nNextSnapshotGeneration = 1u;
	t3dAtomicU32_t m_nPublishedSnapshotIndex{ s_nRdInvalidFrameSnapshotIndex };
	t3dAtomicU64_t m_nPublishedSnapshotGeneration{ 0u };
	t3dAtomicU64_t m_nConsumedSnapshotGeneration{ 0u };
	t3dAtomicU64_t m_nSnapshotPublications{ 0u };
	t3dAtomicU64_t m_nSnapshotConsumes{ 0u };
	t3dAtomicU64_t m_nSnapshotReuses{ 0u };
	t3dAtomicU64_t m_nSkippedSnapshotGenerations{ 0u };
	t3dAtomicU64_t m_nStaleSnapshots{ 0u };
	t3dAtomicU64_t m_nLastStaleSnapshotToken{ 0u };

	uint64_t m_nRegionTopologyGeneration = 0u;
	uint64_t m_nRegionLightingSignature = 0u;
	uint64_t m_nRegionMapGeneration = 0u;
	uint64_t m_nPhysicalRegionMapGeneration = 0u;
	uint64_t m_nLastRegionCacheSourceGeneration = 0u;
	uint64_t m_nRegionCacheGeneration = 0u;
	uint64_t m_nLastPatchFingerprint = 0u;
	uint64_t m_nPatchGeneration = 0u;
	t3dRdPatch_t m_previousPatches[s_nMaxClipmapLevels]{};
	uint32_t m_nPreviousPatchCount = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nMaterialSourceGeneration = 0u;
	t3dRdMaterialLayer_t m_cachedMaterialLayers[s_nRdMaterialLayerCount]{};
	t3dRdMaterialConfig_t m_cachedMaterialConfig{};
	uint32_t m_nCachedMaterialLayerCount = 1u;
	uint64_t m_nTerrainTextureGeneration = 0u;
	uint64_t m_nMaterialTextureGeneration = 0u;
	t3dRdTerrainBindings_t m_lastPublishedResources;
	bool m_bRegionMapInitialized = false;
	bool m_bRegionCacheInitialized = false;
	bool m_bPatchStateInitialized = false;
	bool m_bMaterialStateInitialized = false;
	uint64_t m_nRegionLightingSourceGeneration = 0u;
	uint64_t m_nRegionLightingScanCount = 0u;
	uint64_t m_nMaterialStateBuildCount = 0u;
	uint64_t m_nMeshSourceBuildCount = 0u;
	uint64_t m_nOceanStaticStateBuildCount = 0u;
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
	Terrain3DRenderWorldRuntime m_renderWorldRuntime;
	Terrain3DRdInstanceRenderer m_previewRenderer;
	bool m_bPreviewRendererInitialized = false;
	t3dRdMeshSourceSnapshot_t m_cachedPreviewMeshes;
	bool m_bCachedPreviewMeshesValid = false;
	uint64_t m_nPreviewMeshFingerprint = 0u;
	uint64_t m_nPreviewMeshGeneration = 0u;
	t3dMutex_t m_previewCallMutex;
	t3dMutex_t m_previewStateMutex;
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
	eastl::vector<t3dRdExternalMeshSource_t, Terrain3DEastlAllocator> m_cachedHlodMeshSources;
	uint64_t m_nInstanceMeshSourceGeneration = 0u;
	uint64_t m_nInstanceMeshGeneration = 0u;
	uint64_t m_nInstanceSceneGeneration = 0u;
	uint64_t m_nHlodSceneGeneration = 0u;
	uint64_t m_nCombinedInstanceSceneGeneration = 0u;
	t3dRdOceanState_t m_cachedOceanStaticState{};
	uint64_t m_nOceanStaticSourceGeneration = 0u;
	bool m_bOceanStaticStateInitialized = false;
	bool m_bColdCapacityServiceActive = false;
	bool m_bInstancePublicationCapacityPending = true;
	bool m_bInstanceMeshBankCapacityPending = true;
	uint64_t m_nColdCapacityRebuildCount = 0u;
	uint64_t m_nCapacityMissCount = 0u;
	uint64_t m_nOwnerGrowthDeniedCount = 0u;
	t3dAtomicBool_t m_bRendererInitialized{ false };
	t3dAtomicBool_t m_bRenderEnabled{ false };
	t3dAtomicBool_t m_bRendererFailed{ false };
	bool m_bFailureLatched = false;
	bool m_bForwardBridgeAttached = false;
	bool m_bCompositorDebugEnabled = false;
	bool m_bMotionDebugEnabled = false;
	t3dAtomicU64_t m_nCallbackCount{ 0u };
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

	typedef struct t3dRdForwardViewportState_s {
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
	bool m_bForwardPreparedHasRenderWorld = false;
	typedef struct t3dRenderWorldPendingPublication_s {
		const T3DGeometryDatabase *pGeometry = NULL;
		const T3DSceneCell *pCells = NULL;
		const T3DInstanceRecord *pInstances = NULL;
		const T3DMaterialRecord *pMaterials = NULL;
		uint32_t nCellCount = 0u;
		uint32_t nInstanceCount = 0u;
		uint32_t nMaterialCount = 0u;
		bool bRelease = false;
		bool bPending = false;
		bool bResult = false;
	} t3dRenderWorldPendingPublication_t;
	t3dMutex_t m_renderWorldPublicationMutex;
	t3dRenderWorldPendingPublication_t m_renderWorldPendingPublication{};
	t3dAtomicU32_t m_nConsecutiveRenderFailures{ 0u };
	mutable t3dMutex_t m_diagnosticsMutex;
	String m_initializationError;

	uint64_t hashBytes( const void *pData, size_t nBytes ) const;
	uint64_t advanceGeneration( uint64_t nGeneration ) const;
	void buildMaterialState( t3dRdFrameSnapshot_t &rSnapshot ) const;
	void buildEditorOverlayState( t3dRdFrameSnapshot_t &rSnapshot, const Terrain3DMaterial *pMaterial ) const;
	bool ensureWorldNoiseCache( const Terrain3DMaterial *pMaterial );
	bool ensureMacroNoiseTexture( const Terrain3DMaterial *pMaterial );
	bool ensureEditorBrushTexture( const Terrain3DMaterial *pMaterial );
	uint32_t selectWritablePublicationBank( uint32_t nReaderBank, uint32_t nPendingBank ) const;
	bool hasInstancePublicationCapacity( void ) const;
	bool hasInstanceMeshBankCapacity( void ) const;
	bool ensureInstancePublicationStorage( void );
	bool ensureInstanceMeshBankStorage( void );
	void recordCapacityMiss( const char *pszOwner, size_t nRequired, size_t nCapacity, bool bMeshBank );
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
	bool prepareForwardLifecycle( const t3dRdForwardPassContext_t &rContext, bool &rHistoryValid, bool &rViewportReconfigured );
	void resetForwardLifecycle( void );

public:
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	void serviceColdCapacity( void );
	Terrain3DRenderCoordinator( void ) = default;
	~Terrain3DRenderCoordinator( void );

	void initialize( Terrain3D *pTerrain );
	void initializeOcean( void );
	void shutdown( void );
	void update( void );
	void render( RenderData *pRenderData );
	bool renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	void shutdownRendererOnRenderThread( void );
	void renderPreviewOnRenderThread( void );
	void serviceRenderWorldPublicationOnRenderThread( void );
	bool publishRenderWorldDatabaseCold( const T3DGeometryDatabase &rGeometry,
	        const T3DSceneCell *pCells,
	        uint32_t nCellCount,
	        const T3DInstanceRecord *pInstances,
	        uint32_t nInstanceCount,
	        const T3DMaterialRecord *pMaterials,
	        uint32_t nMaterialCount );
	void releaseRenderWorldDatabaseCold( void );
	Ref<Image> createMeshPreview( uint16_t nAssetId, const Vector2i &rSize );
	Dictionary getDiagnostics( void ) const;
	size_t getRetainedCpuBytes( void ) const;
};

#endif // TERRAIN3D_RENDER_COORDINATOR_H
