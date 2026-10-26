// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_PREVIEW_SERVICE_H
#define TERRAIN3D_RD_PREVIEW_SERVICE_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "core/utility/terrain_3d_owner.h"
#include "rendering/rd/core/terrain_3d_render_lifecycle_state.h"
#include "rendering/rd/core/terrain_3d_render_publication_builder.h"
#include "rendering/rd/compositor/terrain_3d_rd_compositor_effect.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"

#include "authoring/vt/io/terrain_3d_vt_godot_cooker.h"
#include "core/utility/terrain_3d_sync.h"
#include "memory/arena/terrain_3d_zone_allocator.h"
#include "rendering/rd/instances/terrain_3d_rd_instance_renderer.h"
#include "rendering/rd/preview/terrain_3d_rd_preview_result.h"
#include "rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include "assets/catalog/terrain_3d_assets.h"
#include "memory/arena/terrain_3d_memory_zone.h"


class Terrain3DPreviewHost {
protected:
	~Terrain3DPreviewHost( void ) = default;
public:
	virtual bool composePreviewTerrainPackageCold( PackedByteArray &rPackage, uint32_t &rGeneration, uint64_t &rFingerprint ) = 0;
	virtual bool loadPreviewPackage( const void *pBytes, uint64_t nBytes ) = 0;
	virtual bool ensurePreviewSharedVt( t3dVTSharedResourceClass_t eClass ) = 0;
	virtual void shutdownPreviewResources( void ) = 0;
	virtual void setPreviewReady( uint32_t nGeneration, uint32_t nBit ) = 0;
	virtual void clearPreviewReady( uint32_t nBit ) = 0;
	virtual t3dRenderLifecycleStatus_t previewLifecycleStatus( void ) const = 0;
};
struct t3dPreviewServices_t {
	Terrain3DOwner *m_pTerrain;
	Terrain3DVirtualTexture &m_virtualTexture;
	Terrain3DSharedVTRuntime &m_sharedVtRuntime;
	const Terrain3DRdShaderLibrary &m_shaderLibrary;
	const Terrain3DRenderPublicationBuilder &m_publicationBuilder;
	const Ref<Terrain3DRdCompositorEffect> &m_effect;
	const t3dAtomicU32_t &m_nStartupGeneration;
	Terrain3DMutex &m_diagnosticsMutex;
	Terrain3DPreviewHost &m_host;
};

// Owns all editor-preview renderer, VT authoring/republish, readback, retry,
// watchdog, and preview telemetry state. Terrain3DRenderCoordinator only
// orchestrates this service at lifecycle boundaries.
// Borrowed read-only diagnostics. Consume within the owner/lifecycle boundary.
struct t3dPreviewDiagnosticsView_t {
	const bool &m_bPreviewVtPackageValid;
	const bool &m_bPreviewVtPublishedNeutral;
	const bool &m_bPreviewVtRepublishInProgress;
	const bool &m_bPreviewVtRepublishPending;
	const bool &m_bPreviewVtRepublishQueued;
	const bool &m_bPreviewVtStageNeutral;
	const t3dRdMeshSourceSnapshot_t &m_cachedPreviewMeshes;
	const t3dRdPreviewFailure_t &m_lastPreviewFailure;
	const uint64_t &m_nPreviewAsyncCompletedCount;
	const uint64_t &m_nPreviewAsyncLateCallbackCount;
	const uint64_t &m_nPreviewAsyncLifecycleGeneration;
	const uint64_t &m_nPreviewAsyncScheduledCount;
	const uint64_t &m_nPreviewAsyncTimeoutCount;
	const uint64_t &m_nPreviewFailureCount;
	const uint64_t &m_nPreviewVtCookFailureCount;
	const uint64_t &m_nPreviewVtFailedFingerprint;
	const uint64_t &m_nPreviewVtFailedMaterialSourceGeneration;
	const uint64_t &m_nPreviewVtFingerprint;
	const uint64_t &m_nPreviewVtObservedMaterialSourceGeneration;
	const uint32_t &m_nPreviewVtPackageGeneration;
	const uint64_t &m_nPreviewVtPublishCount;
	const uint64_t &m_nPreviewVtPublishedFingerprint;
	const uint32_t &m_nPreviewVtPublishedPackageGeneration;
	const uint64_t &m_nPreviewVtRecookCount;
	const uint64_t &m_nPreviewVtRepublishFailureCount;
	const uint64_t &m_nPreviewVtRepublishRetryAfterUsec;
	const uint64_t &m_nPreviewVtRepublishRetryCount;
	const uint64_t &m_nPreviewVtRepublishSuccessCount;
	const uint64_t &m_nPreviewVtStagedMaterialSourceGeneration;
	const uint64_t &m_nSharedResourceClassTransitionCount;
	const t3dRdMeshSourceSnapshot_t &m_pendingPreviewMeshes;
	const String &m_previewLastError;
	const Terrain3DRdInstanceRenderer &m_previewRenderer;
	Terrain3DMutex &m_previewStateMutex;
	const t3dVTEditorCookTelemetryV3_t &m_previewVtEditorTelemetry;
	const String &m_previewVtLastCookError;
	const String &m_previewVtLastRepublishError;
	Terrain3DMutex &m_previewVtMutex;
	const PackedByteArray &m_previewVtPackageBytes;
	const PackedByteArray &m_previewVtPublishedPackageBytes;
};

class Terrain3DRdPreviewService {
	CLASS_NAME_STATIC( "Terrain3DRdPreviewService" );

private:
	Terrain3DRdInstanceRenderer m_previewRenderer;
	bool m_bPreviewRendererInitialized = false;
	t3dRdMeshSourceSnapshot_t m_cachedPreviewMeshes;
	bool m_bCachedPreviewMeshesValid = false;
	uint64_t m_nPreviewMeshFingerprint = 0u;
	uint16_t m_nCachedPreviewAssetId = UINT16_MAX;
	uint64_t m_nPreviewMeshGeneration = 0u;
	t3dAtomicU64_t m_nPublishedGpuFingerprint{ 0u };
	PackedByteArray m_previewVtPackageBytes;
	PackedByteArray m_previewVtPublishedPackageBytes;
	uint64_t m_nPreviewVtFingerprint = 0u;
	uint64_t m_nPreviewVtPublishedFingerprint = 0u;
	uint32_t m_nPreviewVtPackageGeneration = 0u;
	uint32_t m_nPreviewVtPublishedPackageGeneration = 0u;
	uint64_t m_nPreviewVtRecookCount = 0u;
	uint64_t m_nPreviewVtObservedMaterialSourceGeneration = 0u;
	uint64_t m_nPreviewVtStagedMaterialSourceGeneration = 0u;
	uint64_t m_nPreviewVtFailedMaterialSourceGeneration = 0u;
	uint64_t m_nPreviewVtPublishCount = 0u;
	uint64_t m_nPreviewVtRepublishSuccessCount = 0u;
	uint64_t m_nPreviewVtRepublishFailureCount = 0u;
	uint64_t m_nPreviewVtRepublishRetryCount = 0u;
	uint64_t m_nPreviewVtRepublishRetryAfterUsec = 0u;
	uint64_t m_nSharedResourceClassTransitionCount = 0u;
	uint64_t m_nPreviewVtCookFailureCount = 0u;
	uint64_t m_nPreviewVtFailedFingerprint = 0u;
	t3dVTEditorCookCacheV3_t m_previewVtEditorCache;
	t3dVTEditorCookTelemetryV3_t m_previewVtEditorTelemetry{};
	String m_previewVtLastCookError;
	String m_previewVtLastRepublishError;
	bool m_bPreviewVtPackageValid = false;
	bool m_bPreviewVtStageNeutral = true;
	bool m_bPreviewVtPublishedNeutral = true;
	bool m_bPreviewVtRepublishPending = false;
	bool m_bPreviewVtRepublishQueued = false;
	bool m_bPreviewVtRepublishInProgress = false;
	t3dVTSharedResourceClass_t m_ePreviewVtRepublishResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
	mutable Terrain3DMutex m_previewVtMutex;
	Terrain3DMutex m_previewCallMutex;
	mutable Terrain3DMutex m_previewStateMutex;
	t3dRdMeshSourceSnapshot_t m_pendingPreviewMeshes;
	bool m_bPendingPreviewMeshesValid = false;
	uint16_t m_nPendingPreviewAssetId = 0u;
	Vector2i m_pendingPreviewSize;
	Ref<Image> m_pendingPreviewImage;
	String m_pendingPreviewError;
	t3dRdPreviewFailure_t m_pendingPreviewFailure = T3D_RD_PREVIEW_NONE;
	String m_previewLastError;
	uint64_t m_nPreviewFailureCount = 0u;
	t3dRdPreviewFailure_t m_lastPreviewFailure = T3D_RD_PREVIEW_NONE;
	t3dPreviewReadbackSlot_t m_szPreviewReadbackSlots[T3D_PREVIEW_READBACK_SLOT_COUNT]{};
	uint32_t m_nPreviewReadbackCursor = 0u;
	uint32_t m_nPendingPreviewReadbackSlot = UINT32_MAX;
	uint64_t m_nPreviewAsyncLifecycleGeneration = 1u;
	uint64_t m_nPreviewAsyncRequestGeneration = 0u;
	uint64_t m_nPreviewAsyncScheduledCount = 0u;
	uint64_t m_nPreviewAsyncCompletedCount = 0u;
	uint64_t m_nPreviewAsyncLateCallbackCount = 0u;
	uint64_t m_nPreviewAsyncTimeoutCount = 0u;

	Ref<Image> failMeshPreview( const t3dPreviewServices_t &rServices,
	        t3dRdPreviewFailure_t eFailure,
	        const String &rError,
	        t3dRdPreviewFailure_t *pFailure );
	bool prepareMeshPreviewSourceCold( const t3dPreviewServices_t &rServices,
	        uint16_t nAssetId,
	        const Vector2i &rSize,
	        t3dRdPreviewFailure_t *pFailure );
	void dispatchPreviewAsyncCompletionCold( const t3dPreviewReadbackSlot_t &rSlot,
	        t3dRdPreviewFailure_t eFailure,
	        const Ref<Image> &rImage );
	void completePreviewAsyncOnRenderThread( uint32_t nSlot, t3dRdPreviewFailure_t eFailure, const Ref<Image> &rImage );

public:
	t3dPreviewDiagnosticsView_t getDiagnosticsView( void ) const {
		return { m_bPreviewVtPackageValid, m_bPreviewVtPublishedNeutral, m_bPreviewVtRepublishInProgress, m_bPreviewVtRepublishPending, m_bPreviewVtRepublishQueued, m_bPreviewVtStageNeutral, m_cachedPreviewMeshes, m_lastPreviewFailure, m_nPreviewAsyncCompletedCount, m_nPreviewAsyncLateCallbackCount, m_nPreviewAsyncLifecycleGeneration, m_nPreviewAsyncScheduledCount, m_nPreviewAsyncTimeoutCount, m_nPreviewFailureCount, m_nPreviewVtCookFailureCount, m_nPreviewVtFailedFingerprint, m_nPreviewVtFailedMaterialSourceGeneration, m_nPreviewVtFingerprint, m_nPreviewVtObservedMaterialSourceGeneration, m_nPreviewVtPackageGeneration, m_nPreviewVtPublishCount, m_nPreviewVtPublishedFingerprint, m_nPreviewVtPublishedPackageGeneration, m_nPreviewVtRecookCount, m_nPreviewVtRepublishFailureCount, m_nPreviewVtRepublishRetryAfterUsec, m_nPreviewVtRepublishRetryCount, m_nPreviewVtRepublishSuccessCount, m_nPreviewVtStagedMaterialSourceGeneration, m_nSharedResourceClassTransitionCount, m_pendingPreviewMeshes, m_previewLastError, m_previewRenderer, m_previewStateMutex, m_previewVtEditorTelemetry, m_previewVtLastCookError, m_previewVtLastRepublishError, m_previewVtMutex, m_previewVtPackageBytes, m_previewVtPublishedPackageBytes };
	}

	bool bindRuntimeAllocator( const t3dPreviewServices_t &rServices,
	        Terrain3DMemoryZone *pZone,
	        t3dAllocatorOverflowPolicy_t ePolicy );
	bool bakePreviewVirtualTexturesCold( const t3dPreviewServices_t &rServices, String *pError );
	bool preparePreviewAuthoringPackageCold(
	        const t3dPreviewServices_t &rServices, const Ref<Terrain3DAssets> &rAssets, String *pError );
	bool queuePreviewVtRepublishCold( const t3dPreviewServices_t &rServices );
	bool reloadPublishedPreviewVtPackageCold( const t3dPreviewServices_t &rServices );
	void servicePreviewVtRepublishOnRenderThread( const t3dPreviewServices_t &rServices );
	void renderPreviewOnRenderThread( const t3dPreviewServices_t &rServices );
	Ref<Image> createMeshPreview( const t3dPreviewServices_t &rServices,
	        uint16_t nAssetId,
	        const Vector2i &rSize,
	        t3dRdPreviewFailure_t *pFailure );
	bool requestMeshPreviewAsync( const t3dPreviewServices_t &rServices,
	        uint16_t nAssetId,
	        const Vector2i &rSize,
	        uint64_t nCompletionOwnerId,
	        uint64_t nCompletionGeneration,
	        t3dRdPreviewFailure_t *pFailure );
	void servicePreviewReadbackWatchdogCold( const t3dPreviewServices_t &rServices );
	void cancelPreviewAsyncRequestsOnRenderThread( void );
	void renderPreviewAsyncOnRenderThread( const t3dPreviewServices_t &rServices );
	void consumePreviewReadbackOnRenderThread( const t3dPreviewServices_t &rServices,
	        uint32_t nSlot,
	        uint64_t nRequestGeneration,
	        const PackedByteArray &rReadbackBytes );
	void requestPackageRecomposeCold( void );
	void noteGpuPackageOnRenderThread( uint32_t nGeneration, uint64_t nSignature );
	uint64_t publishedGpuFingerprintCold( void ) const { return m_nPublishedGpuFingerprint.load( T3D_MEMORY_ORDER_ACQUIRE ); }
	void updateAuthoringCold( const t3dPreviewServices_t &rServices );
	void shutdownRendererOnRenderThread( const t3dPreviewServices_t &rServices );
	void resetOwnerCold( void );
	bool hasReadbackInFlight( void ) const;
	void noteSharedResourceClassTransition( void );
};

#endif // TERRAIN3D_RD_PREVIEW_SERVICE_H
