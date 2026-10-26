// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_COORDINATOR_H
#define TERRAIN3D_RENDER_COORDINATOR_H

#include "Terrain3DConstants.h"
#include "authoring/vt/terrain_3d_vt_godot_cooker.h"
#include "rendering/rd/terrain_3d_rd_preview_result.h"
#include "rendering/rd/terrain_3d_render_lifecycle_state.h"
#include "core/terrain_3d_sync.h"
#include "core/terrain_3d_startup_state.h"
#include "rendering/rd/terrain_3d_material_state_builder.h"
#include "rendering/rd/terrain_3d_forward_pass_preparer.h"
#include "rendering/rd/terrain_3d_diagnostics_publisher.h"
#include "rendering/rd/terrain_3d_render_publication_builder.h"
#include "rendering/rd/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/terrain_3d_rd_compositor_effect.h"
#include "rendering/rd/terrain_3d_rd_instance_renderer.h"
#include "rendering/rd/terrain_3d_rd_ocean_renderer.h"
#include "rendering/rd/vt/terrain_3d_shared_vt_runtime.h"
#include "rendering/rd/render_world/terrain_3d_render_world_geometry.h"
#include "rendering/rd/world/terrain_3d_world_runtime.h"
#include "rendering/rd/terrain_3d_rd_shader_library.h"
#include "rendering/rd/terrain_3d_rd_types.h"
#include "rendering/rd/vt/terrain_3d_terrain_megavt.h"

#include "eastl_config.h"
#include <stddef.h>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
class Terrain3D;
class Terrain3DData;
class Terrain3DMaterial;
class Terrain3DRdForwardPlusBridgeAdapter;

class Terrain3DRenderCoordinator {
private:
	typedef enum t3dStartupGpuStage_e : uint32_t {
		T3D_STARTUP_GPU_VT_STORAGE = 0u,
		T3D_STARTUP_GPU_VT_EXECUTABLE,
		T3D_STARTUP_GPU_VT_PUBLICATION,
		T3D_STARTUP_GPU_TERRAIN_RENDERER,
		T3D_STARTUP_GPU_WORLD_RUNTIME,
		T3D_STARTUP_GPU_INSTANCE_RENDERER,
		T3D_STARTUP_GPU_OCEAN_RENDERER,
		T3D_STARTUP_GPU_COMPLETE,
		T3D_STARTUP_GPU_FAILED,
	} t3dStartupGpuStage_t;
	friend class Terrain3DMaterialStateBuilder;
	friend class Terrain3DRenderPublicationBuilder;
	friend class Terrain3DForwardPassPreparer;
	friend class Terrain3DDiagnosticsPublisher;

	CLASS_NAME_STATIC( "Terrain3DRenderCoordinator" );

private:
	// OWNER THREAD ONLY: live Godot/Terrain3D state. Render-thread code must never
	// dereference this pointer; render-visible policy belongs in the immutable
	// frame publication. Pointer lifetime is still coordinated by startup/shutdown.
	Terrain3D *m_pTerrain = NULL;
	Terrain3DMemoryZone *m_pRuntimeAllocatorZone = NULL;
	t3dAllocatorOverflowPolicy_t m_eRuntimeAllocatorOverflowPolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT;
	bool m_bRuntimeAllocatorBound = false;
	Ref<Terrain3DRdCompositorEffect> m_effect;
	Ref<Compositor> m_attachedCompositor;
	uint64_t m_nAttachedCameraId = 0u;
	bool m_bCreatedCompositor = false;


	// OWNER THREAD MUTATION: publication/material builders. The render thread may
	// consume only acquired immutable snapshots and immutable banks selected by a
	// published bank index; it must not inspect producer-side mutable counters.
	Terrain3DRenderPublicationBuilder m_publicationBuilder;
	Terrain3DMaterialStateBuilder m_materialStateBuilder;
	Terrain3DForwardPassPreparer m_forwardPassPreparer;
	Terrain3DDiagnosticsPublisher m_diagnosticsPublisher;
	Terrain3DRdShaderLibrary m_shaderLibrary;
	Terrain3DVirtualTexture m_virtualTexture;
	Terrain3DTerrainMegaVT m_terrainMegaVT;
	// RENDER THREAD OWNED: RD renderer/runtime resource creation, mutation, and
	// destruction. Owner-thread lifecycle code may control policy but must not
	// inspect or mutate these resource internals as a synchronization mechanism.
	Terrain3DRdClipmapRenderer m_renderer;
	Terrain3DRdInstanceRenderer m_instanceRenderer;
	Terrain3DRdOceanRenderer m_oceanRenderer;
	Terrain3DSharedVtRuntime m_sharedVtRuntime;
	Terrain3DWorldRuntime m_worldRuntime;
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
	t3dRenderWorldSharedResourceClass_t m_ePreviewVtRepublishResourceClass = T3D_RW_SHARED_RESOURCES_NONE;
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
	Ref<Image> failMeshPreview( t3dRdPreviewFailure_t eFailure, const String &rError, t3dRdPreviewFailure_t *pFailure );
	bool prepareMeshPreviewSourceCold( uint16_t nAssetId, const Vector2i &rSize, t3dRdPreviewFailure_t *pFailure );
	bool queuePreviewVtRepublishCold( void );
	void servicePreviewReadbackWatchdogCold( void );
	void dispatchPreviewAsyncCompletionCold(
	        const t3dPreviewReadbackSlot_t &rSlot, t3dRdPreviewFailure_t eFailure, const Ref<Image> &rImage );
	void completePreviewAsyncOnRenderThread( uint32_t nSlot, t3dRdPreviewFailure_t eFailure, const Ref<Image> &rImage );
	void cancelPreviewAsyncRequestsOnRenderThread( void );

	t3dAtomicBool_t m_bRendererInitialized{ false };
	t3dAtomicBool_t m_bShaderGpuStartupQueued{ false };
	t3dAtomicBool_t m_bShaderGpuStartupService{ false };
	t3dAtomicBool_t m_bInstanceRendererInitialized{ false };
	t3dAtomicBool_t m_bOceanRendererInitialized{ false };
	t3dAtomicBool_t m_bRenderEnabled{ false };
	// CONTROL-PLANE HANDOFF: render-owned lifecycle state is copied coherently
	// through this record. Owner code must consume this snapshot instead of
	// interrogating RD/runtime objects for readiness. Never acquire this mutex
	// from steady-state Forward+ draw submission.
	mutable t3dMutex_t m_renderLifecycleMutex;
	t3dRenderLifecycleStatus_t m_renderLifecycleStatus{};

	t3dAtomicBool_t m_bRendererFailed{ false };
	bool m_bFailureLatched = false;
	uint64_t m_nFailureLatchTerrainTextureGeneration = 0u;
	uint64_t m_nFailureLatchMaterialTextureGeneration = 0u;
	uint64_t m_nRendererRecoveryCount = 0u;
	uint64_t m_nRecoveryRequestRevision = 0u;
	uint64_t m_nRecoveryQueuedRevision = 0u;
	t3dAtomicU64_t m_nRecoveryActiveRevision{ 0u };
	t3dAtomicBool_t m_bRecoveryInProgress{ false };
	bool m_bForwardBridgeAttached = false;
	bool m_bCompositorDebugEnabled = false;
	bool m_bMotionDebugEnabled = false;
	t3dAtomicU64_t m_nCallbackCount{ 0u };
	uint64_t m_nInstanceInitializationAttempts = 0u;
	uint64_t m_nInstanceInitializationFailures = 0u;
	uint64_t m_nInstanceInitializationRecoveries = 0u;
	uint64_t m_nLastInstanceInitializationRenderFrame = UINT64_MAX;
	uint64_t m_nLastOceanInitializationRenderFrame = UINT64_MAX;
	struct t3dStaticScenePublication_t {
		const T3DGeometryDatabase *pGeometry = NULL;
		const T3DSceneCell *pCells = NULL;
		const T3DInstanceRecord *pInstances = NULL;
		const t3dRenderingMaterialGpu_t *pMaterials = NULL;
		uint32_t nCellCount = 0u;
		uint32_t nInstanceCount = 0u;
		uint32_t nMaterialCount = 0u;
		uint64_t nGeneration = 0u;
		bool bValid = false;
	};
	t3dMutex_t m_staticScenePublicationMutex;
	t3dStaticScenePublication_t m_staticScenePublication{};
	t3dAtomicU32_t m_nConsecutiveRenderFailures{ 0u };
	t3dAtomicU32_t m_nWorldUploadBudgetKbPolicy{ 4096u };
	t3dAtomicU32_t m_nWorldClusterBudgetPolicy{ 8192u };
	t3dAtomicU32_t m_nVtRequestBudgetPolicy{ 2048u };
	t3dAtomicU32_t m_nVtAdmissionBudgetPolicy{ 64u };
	t3dAtomicBool_t m_bWorldBoundaryDeferredPolicy{ false };
	t3dAtomicU32_t m_nStartupGeneration{ 0u };
	uint64_t m_nStartupPublicationPrepareCount = 0u;
	uint64_t m_nStartupPublicationPrepareUsecLast = 0u;
	uint64_t m_nStartupRendererInitializeAttemptCount = 0u;
	uint64_t m_nStartupRendererInitializeSuccessCount = 0u;
	uint64_t m_nStartupRendererInitializeUsecLast = 0u;
	uint64_t m_nStartupRendererInitializeUsecTotal = 0u;
	uint64_t m_nStartupGpuServiceCount = 0u;
	uint64_t m_nStartupGpuServiceUsecLast = 0u;
	t3dAtomicU64_t m_nStartupStaleCompletionRejectCount{ 0u };
	uint64_t m_nStartupCancellationCount = 0u;
	uint64_t m_nStartupRenderDrainCount = 0u;
	uint64_t m_nStartupActivationBarrierCount = 0u;
	uint64_t m_nStartupActivationBarrierUsecLast = 0u;
	uint64_t m_nStartupActivationSnapshotGeneration = 0u;
	uint64_t m_nStartupFinalSyncSuccessCount = 0u;
	uint64_t m_nStartupFinalSyncFailureCount = 0u;
	uint64_t m_nStartupFinalSyncBeginUsec = 0u;
	uint64_t m_nStartupFinalSyncElapsedUsecLast = 0u;
	t3dAtomicU32_t m_nStartupGpuStage{ T3D_STARTUP_GPU_VT_STORAGE };
	bool m_bStartupTerrainRendererPrepared = false;
	bool m_bStartupWorldRuntimePrepared = false;
	t3dAtomicBool_t m_bStartupWorldPublicationQueued{ false };
	t3dAtomicBool_t m_bStartupWorldPublicationPrepared{ false };
	t3dAtomicU64_t m_nStartupWorldPublicationGeneration{ 0u };
	t3dAtomicU64_t m_nStartupWorldPublicationPrepareUsecLast{ 0u };
	t3dAtomicBool_t m_bStartupGpuServiceComplete{ false };
	bool m_bStartupActivationPrepared = false;
	bool m_bStartupActivationCommitted = false;
	t3dAtomicBool_t m_bStartupColdBegun{ false };
	t3dAtomicBool_t m_bStartupColdPrepared{ false };
	t3dAtomicBool_t m_bStartupColdFailed{ false };
	mutable t3dMutex_t m_diagnosticsMutex;
	String m_initializationError;

	uint64_t hashBytes( const void *pData, size_t nBytes ) const;
	uint64_t advanceGeneration( uint64_t nGeneration ) const;
	void publishSnapshot( void );
	const t3dRdFrameSnapshotSlot_t *acquireSnapshotOnRenderThread( void );
	void resetSnapshotPublication( void );
	void attachEffectToActiveCamera( void );
	void detachEffect( void );
	bool initializeRendererOnRenderThread( uint64_t nRenderFrame = UINT64_MAX );
	void serviceShaderStartupCold( void );
	bool serviceStartupGpuOnRenderThreadBudgeted( uint32_t nStartupGeneration );
	bool queueStartupGpuServiceOnRenderThread( uint32_t nStartupGeneration );
	bool queueRendererRecoveryCold( uint32_t nStartupGeneration, uint64_t nRecoveryRevision );
	bool completeRendererRecoveryOnRenderThread( uint32_t nStartupGeneration, uint64_t nRecoveryRevision );
	bool isStartupGenerationCurrent( uint32_t nStartupGeneration ) const;
	bool isStartupActivationBaseReady( uint32_t nStartupGeneration ) const;
	void publishRenderLifecycleStatusOnRenderThread( const t3dRenderLifecycleStatus_t &rStatus );
	void publishRenderLifecycleStatusCold( const t3dRenderLifecycleStatus_t &rStatus );
	t3dRenderLifecycleStatus_t snapshotRenderLifecycleStatusOnOwnerThread( void ) const;
	void setRenderLifecycleReadyBitOnRenderThread( uint32_t nStartupGeneration, uint32_t nReadyBit );
	void clearRenderLifecycleReadyBitOnRenderThread( uint32_t nReadyBit );
	void publishRenderSharedResourceReadyOnRenderThread( t3dRenderWorldSharedResourceClass_t eResourceClass );
	void publishRenderLifecycleFailureOnRenderThread( uint32_t nStartupGeneration, t3dRenderLifecycleFailure_t eFailure );
	bool queueStartupCancellationAckCold( uint32_t nCancelledGeneration, bool bForceDrain );
	bool ensureSharedVtRuntimeCold( t3dRenderWorldSharedResourceClass_t eResourceClass = T3D_RW_SHARED_RESOURCES_PRODUCTION );
	bool snapshotSharedVtBindingsHot( T3DSharedVTBindings &rBindings ) const;
	const T3DSharedVTBindings &getSharedVtBindingsHot( void ) const;
	bool reloadPublishedPreviewVtPackageCold( void );
	bool preparePreviewAuthoringPackageCold( const Ref<Terrain3DAssets> &rAssets, String *pError );
	bool loadRuntimeVirtualTexturePackageFileCold( String *pError );
	bool attachForwardBridge( void );
	void detachForwardBridge( void );
	bool isForwardBridgeReady( void ) const;
	void logStartupActivationFailureState( const char *pReason, uint32_t nStartupGeneration ) const;
	void updateRenderRoute( void );
	void resetForwardLifecycle( void );
	void importTerrainMegaVTPersistentStateHot( void );
	void persistTerrainMegaVTEditorStateHot( void );

public:
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	bool loadVirtualTexturePackageCold( const void *pPackageBytes, uint64_t nPackageBytes );
	bool setVirtualTextureQualityProfile( uint32_t nProfile );
	bool resolveVirtualTextureMaterialAllocationCold( uint64_t nStableKey, uint32_t &rAllocationId ) const;
	bool bakePreviewVirtualTexturesCold( godot::String *pError = NULL );
	void serviceColdCapacity( void );
	Terrain3DRenderCoordinator( void ) = default;
	~Terrain3DRenderCoordinator( void );

	bool beginStartupCold( Terrain3D *pTerrain, uint32_t nStartupGeneration );
	void cancelStartupCold( uint32_t nStartupGeneration );
	void serviceStartupCold( uint32_t nStartupGeneration );
	bool isStartupActivationReady( uint32_t nStartupGeneration ) const;
	bool prepareStartupActivationCold( uint32_t nStartupGeneration );
	EA_FORCE_INLINE bool isStartupActivationPrepared( const uint32_t nStartupGeneration ) const {
		return m_bStartupActivationPrepared && !m_bStartupColdFailed && m_nStartupGeneration == nStartupGeneration;
	}
	EA_FORCE_INLINE bool hasStartupActivationSnapshot( const uint32_t nStartupGeneration ) const {
		return m_nStartupGeneration == nStartupGeneration && m_nStartupActivationSnapshotGeneration != 0u;
	}
	bool commitStartupActivationCold( uint32_t nStartupGeneration );
	EA_FORCE_INLINE bool isStartupPrepared( const uint32_t nStartupGeneration ) const {
		return m_bStartupColdPrepared && !m_bStartupColdFailed && m_nStartupGeneration == nStartupGeneration;
	}
	EA_FORCE_INLINE bool isStartupFailed( const uint32_t nStartupGeneration ) const {
		return m_bStartupColdFailed && m_nStartupGeneration == nStartupGeneration;
	}
	bool prepareStartupCold( uint32_t nStartupGeneration );
	void initializeOcean( void );
	void shutdown( void );
	void update( void );
	void onTerrainMapsEdited( const AABB &rEditedArea );
	bool claimTerrainMegaVTIncrementalCook(
	        uint32_t nRegionMapIndex, t3dTerrainMegaVTIncrementalCookRequest_t &rRequest, t3dTerrainMegaVTCookTicket_t &rTicket );
	bool validateTerrainMegaVTIncrementalCookCompletion( const t3dTerrainMegaVTCookTicket_t &rTicket );
	bool acceptTerrainMegaVTIncrementalCookCompletion( const t3dTerrainMegaVTCookTicket_t &rTicket );
	void render( RenderData *pRenderData );
	bool renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	Dictionary getForwardLaneDiagnostics( void ) const;
	void shutdownRendererOnRenderThread( void );
	void shutdownRendererLifecycleOnRenderThread( uint32_t nShutdownGeneration );
	void recoverRendererLifecycleOnRenderThread( uint32_t nStartupGeneration, uint64_t nRecoveryRevision );
	void renderPreviewOnRenderThread( void );
	void renderPreviewAsyncOnRenderThread( void );
	void servicePreviewVtRepublishOnRenderThread( void );
	void serviceShaderStartupOnRenderThread( uint32_t nStartupGeneration );
	void acknowledgeStartupCancellationOnRenderThread( uint32_t nStartupGeneration );
	void serviceStartupWorldPublicationOnRenderThread( uint32_t nStartupGeneration, uint64_t nSnapshotGeneration );
	void consumePreviewReadbackOnRenderThread( uint32_t nSlot, uint64_t nRequestGeneration, const PackedByteArray &rReadbackBytes );
	bool publishStaticSceneDatabaseCold( const T3DGeometryDatabase &rGeometry,
	        const T3DSceneCell *pCells,
	        uint32_t nCellCount,
	        const T3DInstanceRecord *pInstances,
	        uint32_t nInstanceCount,
	        const t3dRenderingMaterialGpu_t *pMaterials,
	        uint32_t nMaterialCount );
	void releaseStaticSceneDatabaseCold( void );
	Ref<Image> createMeshPreview( uint16_t nAssetId, const Vector2i &rSize, t3dRdPreviewFailure_t *pFailure = NULL );
	bool requestMeshPreviewAsync( uint16_t nAssetId,
	        const Vector2i &rSize,
	        uint64_t nCompletionOwnerId,
	        uint64_t nCompletionGeneration,
	        t3dRdPreviewFailure_t *pFailure = NULL );
	Dictionary getDiagnostics( void ) const;
	Dictionary getStartupDiagnostics( void ) const;
	EA_FORCE_INLINE t3dStartupDomainSnapshot_t getShaderStartupState( void ) const {
		return m_shaderLibrary.getStartupSnapshot();
	}
	EA_FORCE_INLINE t3dStartupDomainSnapshot_t getVirtualTextureStartupState( void ) const {
		return m_virtualTexture.get_startup_snapshot();
	}
	size_t getRetainedCpuBytes( void ) const;
	void setUnifiedWorldFramePolicy(
	        uint32_t nWorldUploadKb, uint32_t nWorldClusters, uint32_t nVtRequests, uint32_t nVtAdmissions, bool bBoundaryDeferred );
	uint64_t getWorldGpuBufferBytes( void ) const;
	uint64_t getWorldHiZGpuBytes( void ) const;
	size_t getWorldRetainedCpuBytes( void ) const;
	uint64_t getWorldBudgetDeferralCount( void ) const;
	uint64_t getVirtualTextureBudgetDeferralCount( void ) const;
	uint64_t getVirtualTextureCpuBytes( void ) const;
	bool isEditorAuthoringMode( void ) const;
};

#endif // TERRAIN3D_RENDER_COORDINATOR_H
