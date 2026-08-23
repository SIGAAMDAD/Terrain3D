// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RENDER_COORDINATOR_H
#define TERRAIN3D_RENDER_COORDINATOR_H

#include "assets/generated_texture.h"
#include "rendering/rd/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/terrain_3d_rd_instance_renderer.h"
#include "rendering/rd/terrain_3d_rd_compositor_effect.h"
#include "rendering/rd/terrain_3d_rd_shader_library.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <array>
#include <cstddef>

#include <atomic>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <mutex>
#include <memory>
#include <vector>

class Terrain3D;
class Terrain3DMaterial;
class Terrain3DRdForwardPlusBridgeAdapter;

class Terrain3DRenderCoordinator {
	CLASS_NAME_STATIC( "Terrain3DRenderCoordinator" );

private:
	Terrain3D *m_pTerrain = nullptr;
	Ref<Terrain3DRdCompositorEffect> m_effect;
	Ref<Compositor> m_attachedCompositor;
	uint64_t m_nAttachedCameraId = 0u;
	bool m_bCreatedCompositor = false;
	std::atomic<t3dRenderBackend_t> m_backend{ T3D_RENDER_BACKEND_RENDERING_SERVER };
	std::atomic<t3dRdNativeProxyMode_t> m_proxyMode{ T3D_RD_NATIVE_PROXY_SHADOWS_ONLY };

	typedef struct t3dRdPublishedResources_s {
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
		RID m_hDisplacementTextureRd;
		RID m_hWorldNoiseTextureRd;
		RID m_hMacroNoiseTextureRd;
		RID m_hEditorBrushTextureRd;
	} t3dRdPublishedResources_t;

	typedef struct t3dRdFrameSnapshotSlot_s {
		t3dRdFrameSnapshot_t m_snapshot;
		PackedByteArray m_regionMapBytes;
		t3dRdPublishedResources_t m_resources;
		std::shared_ptr<const t3dRdMeshSourceSnapshot_t> m_instanceMeshes;
		std::shared_ptr<const t3dRdInstanceSceneSnapshot_t> m_instanceScene;
	} t3dRdFrameSnapshotSlot_t;

	std::array<t3dRdFrameSnapshotSlot_t, s_nRdFrameSnapshotSlotCount> m_snapshotSlots{};
	std::atomic_uint64_t m_nSnapshotExchangeToken{
		t3dRdMakeFrameSnapshotToken( 1u, 0u, false )
	};
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
	std::shared_ptr<const t3dRdMeshSourceSnapshot_t> m_cachedInstanceMeshes;
	std::shared_ptr<const t3dRdInstanceSceneSnapshot_t> m_cachedRegularInstanceScene;
	std::shared_ptr<const t3dRdInstanceSceneSnapshot_t> m_cachedHlodInstanceScene;
	std::shared_ptr<const t3dRdInstanceSceneSnapshot_t> m_cachedInstanceScene;
	std::vector<t3dRdExternalMeshSource_t> m_cachedHlodMeshSources;
	uint64_t m_nInstanceMeshFingerprint = 0u;
	uint64_t m_nInstanceMeshGeneration = 0u;
	uint64_t m_nInstanceSceneGeneration = 0u;
	uint64_t m_nHlodSceneGeneration = 0u;
	uint64_t m_nCombinedInstanceSceneGeneration = 0u;
	std::atomic_bool m_bRendererInitialized{ false };
	std::atomic_bool m_bRenderEnabled{ false };
	std::atomic_bool m_bRendererFailed{ false };
	bool m_bFallbackActive = false;
	bool m_bForwardBridgeAttached = false;
	bool m_bCompositorDebugEnabled = false;
	bool m_bMotionDebugEnabled = false;
	std::atomic_uint64_t m_nCallbackCount{ 0u };
	std::atomic_uint64_t m_nForwardPassCallbacks{ 0u };
	std::atomic_uint64_t m_nMotionPassCallbacks{ 0u };
	std::atomic_uint64_t m_nLastForwardScenePass{ 0u };
	std::atomic_int64_t m_nLastForwardDrawList{ -1 };
	std::atomic_uint32_t m_nLastForwardPassKind{ T3D_RD_FORWARD_PASS_INVALID };
	uint64_t m_nForwardPreparedScenePass = 0u;
	bool m_bForwardPreparedHasTerrain = false;
	std::atomic_uint32_t m_nConsecutiveRenderFailures{ 0u };
	mutable std::mutex m_diagnosticsMutex;
	String m_initializationError;

	uint64_t hashBytes( const void *pData, size_t nBytes ) const;
	uint64_t advanceGeneration( uint64_t nGeneration ) const;
	void buildMaterialState( t3dRdFrameSnapshot_t &rSnapshot ) const;
	void buildEditorOverlayState( t3dRdFrameSnapshot_t &rSnapshot, const Terrain3DMaterial *pMaterial ) const;
	bool ensureWorldNoiseCache( const Terrain3DMaterial *pMaterial );
	bool ensureMacroNoiseTexture( const Terrain3DMaterial *pMaterial );
	bool ensureEditorBrushTexture( const Terrain3DMaterial *pMaterial );
	void publishSnapshot( void );
	const t3dRdFrameSnapshotSlot_t *acquireSnapshotOnRenderThread( void );
	void resetSnapshotPublication( void );
	void recordStaleSnapshot( uint64_t nToken );
	void updateNativeProxy( void );
	void attachEffectToActiveCamera( void );
	void detachEffect( void );
	bool initializeRendererOnRenderThread( void );
	bool attachForwardBridge( void );
	void detachForwardBridge( void );
	bool isForwardBridgeReady( void ) const;
	void updateRenderRoute( void );

public:
	Terrain3DRenderCoordinator( void ) = default;
	~Terrain3DRenderCoordinator( void );

	void initialize( Terrain3D *pTerrain );
	void shutdown( void );
	void update( void );
	void setBackend( t3dRenderBackend_t backend );
	void setProxyMode( t3dRdNativeProxyMode_t mode );
	void render( RenderData *pRenderData );
	void renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	void shutdownRendererOnRenderThread( void );
	Dictionary getDiagnostics( void ) const;

	EA_FORCE_INLINE t3dRenderBackend_t getBackend( void ) const { return m_backend.load( std::memory_order_acquire ); }
	EA_FORCE_INLINE t3dRdNativeProxyMode_t getProxyMode( void ) const { return m_proxyMode.load( std::memory_order_acquire ); }
};

#endif // TERRAIN3D_RENDER_COORDINATOR_H
