// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_PREVIEW_SERVICE_H
#define TERRAIN3D_RD_PREVIEW_SERVICE_H

#include "authoring/vt/terrain_3d_vt_godot_cooker.h"
#include "core/terrain_3d_sync.h"
#include "memory/terrain_3d_zone_allocator.h"
#include "rendering/rd/terrain_3d_rd_instance_renderer.h"
#include "rendering/rd/terrain_3d_rd_preview_result.h"
#include "rendering/rd/vt/terrain_3d_shared_vt_runtime.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

class Terrain3DAssets;
class Terrain3DDiagnosticsPublisher;
class Terrain3DMemoryZone;
class Terrain3DRenderCoordinator;

// Owns all editor-preview renderer, VT authoring/republish, readback, retry,
// watchdog, and preview telemetry state. Terrain3DRenderCoordinator only
// orchestrates this service at lifecycle boundaries.
class Terrain3DRdPreviewService {
	friend class Terrain3DDiagnosticsPublisher;
	CLASS_NAME_STATIC( "Terrain3DRdPreviewService" );

private:
	Terrain3DRdInstanceRenderer m_previewRenderer;
	bool m_bPreviewRendererInitialized = false;
	t3dRdMeshSourceSnapshot_t m_cachedPreviewMeshes;
	bool m_bCachedPreviewMeshesValid = false;
	uint64_t m_nPreviewMeshFingerprint = 0u;
	uint16_t m_nCachedPreviewAssetId = UINT16_MAX;
	uint64_t m_nPreviewMeshGeneration = 0u;
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
	t3dVtSharedResourceClass_t m_ePreviewVtRepublishResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
	mutable t3dMutex_t m_previewVtMutex;
	t3dMutex_t m_previewCallMutex;
	mutable t3dMutex_t m_previewStateMutex;
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

	static const uint32_t s_nPreviewReadbackSlotCount = 3u;
	static const uint64_t s_nPreviewReadbackTimeoutUsec = 2000000u;
	typedef struct t3dPreviewReadbackSlot_s {
		bool bActive = false;
		bool bReadbackInFlight = false;
		uint16_t nAssetId = 0u;
		Vector2i size;
		uint64_t nRequestGeneration = 0u;
		uint64_t nLifecycleGeneration = 0u;
		uint64_t nMeshSourceGeneration = 0u;
		uint64_t nAssetObjectId = 0u;
		uint64_t nReadbackSubmitUsec = 0u;
		uint64_t nCompletionOwnerId = 0u;
		uint64_t nCompletionGeneration = 0u;
	} t3dPreviewReadbackSlot_t;
	t3dPreviewReadbackSlot_t m_szPreviewReadbackSlots[s_nPreviewReadbackSlotCount]{};
	uint32_t m_nPreviewReadbackCursor = 0u;
	uint32_t m_nPendingPreviewReadbackSlot = UINT32_MAX;
	uint64_t m_nPreviewAsyncLifecycleGeneration = 1u;
	uint64_t m_nPreviewAsyncRequestGeneration = 0u;
	uint64_t m_nPreviewAsyncScheduledCount = 0u;
	uint64_t m_nPreviewAsyncCompletedCount = 0u;
	uint64_t m_nPreviewAsyncLateCallbackCount = 0u;
	uint64_t m_nPreviewAsyncTimeoutCount = 0u;

	Ref<Image> failMeshPreview( Terrain3DRenderCoordinator &rCoordinator,
	        t3dRdPreviewFailure_t eFailure,
	        const String &rError,
	        t3dRdPreviewFailure_t *pFailure );
	bool prepareMeshPreviewSourceCold( Terrain3DRenderCoordinator &rCoordinator,
	        uint16_t nAssetId,
	        const Vector2i &rSize,
	        t3dRdPreviewFailure_t *pFailure );
	void dispatchPreviewAsyncCompletionCold( const t3dPreviewReadbackSlot_t &rSlot,
	        t3dRdPreviewFailure_t eFailure,
	        const Ref<Image> &rImage );
	void completePreviewAsyncOnRenderThread( uint32_t nSlot, t3dRdPreviewFailure_t eFailure, const Ref<Image> &rImage );

public:
	bool bindRuntimeAllocator( Terrain3DRenderCoordinator &rCoordinator,
	        Terrain3DMemoryZone *pZone,
	        t3dAllocatorOverflowPolicy_t ePolicy );
	bool bakePreviewVirtualTexturesCold( Terrain3DRenderCoordinator &rCoordinator, String *pError );
	bool preparePreviewAuthoringPackageCold(
	        Terrain3DRenderCoordinator &rCoordinator, const Ref<Terrain3DAssets> &rAssets, String *pError );
	bool queuePreviewVtRepublishCold( Terrain3DRenderCoordinator &rCoordinator );
	bool reloadPublishedPreviewVtPackageCold( Terrain3DRenderCoordinator &rCoordinator );
	void servicePreviewVtRepublishOnRenderThread( Terrain3DRenderCoordinator &rCoordinator );
	void renderPreviewOnRenderThread( Terrain3DRenderCoordinator &rCoordinator );
	Ref<Image> createMeshPreview( Terrain3DRenderCoordinator &rCoordinator,
	        uint16_t nAssetId,
	        const Vector2i &rSize,
	        t3dRdPreviewFailure_t *pFailure );
	bool requestMeshPreviewAsync( Terrain3DRenderCoordinator &rCoordinator,
	        uint16_t nAssetId,
	        const Vector2i &rSize,
	        uint64_t nCompletionOwnerId,
	        uint64_t nCompletionGeneration,
	        t3dRdPreviewFailure_t *pFailure );
	void servicePreviewReadbackWatchdogCold( Terrain3DRenderCoordinator &rCoordinator );
	void cancelPreviewAsyncRequestsOnRenderThread( void );
	void renderPreviewAsyncOnRenderThread( Terrain3DRenderCoordinator &rCoordinator );
	void consumePreviewReadbackOnRenderThread( Terrain3DRenderCoordinator &rCoordinator,
	        uint32_t nSlot,
	        uint64_t nRequestGeneration,
	        const PackedByteArray &rReadbackBytes );
	void updateAuthoringCold( Terrain3DRenderCoordinator &rCoordinator );
	void shutdownRendererOnRenderThread( Terrain3DRenderCoordinator &rCoordinator );
	void resetOwnerCold( void );
	bool hasReadbackInFlight( void ) const;
	void noteSharedResourceClassTransition( void );
};

#endif // TERRAIN3D_RD_PREVIEW_SERVICE_H
