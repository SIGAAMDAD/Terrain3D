// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_COORDINATOR_H
#define TERRAIN3D_RENDER_COORDINATOR_H

#include "Terrain3DTypes.h"
#include "authoring/vt/build/terrain_3d_incremental_megavt_cooker.h"

#include "Terrain3DConstants.h"
#include "rendering/rd/world/runtime/terrain_3d_static_scene_publication.h"
#include "core/utility/terrain_3d_owner.h"
#include "rendering/rd/preview/terrain_3d_rd_preview_result.h"
#include "rendering/rd/core/terrain_3d_render_coordinator_access.h"
#include "rendering/rd/preview/terrain_3d_rd_preview_service.h"
#include "rendering/rd/core/terrain_3d_render_lifecycle_state.h"
#include "core/utility/terrain_3d_sync.h"
#include "core/config/terrain_3d_startup_state.h"
#include "rendering/rd/material/terrain_3d_material_state_builder.h"
#include "rendering/rd/forward/terrain_3d_forward_pass_preparer.h"
#include "rendering/rd/core/terrain_3d_diagnostics_publisher.h"
#include "rendering/rd/core/terrain_3d_render_publication_builder.h"
#include "rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/compositor/terrain_3d_rd_compositor_effect.h"
#include "rendering/rd/instances/terrain_3d_rd_instance_renderer.h"
#include "rendering/rd/ocean/terrain_3d_rd_ocean_renderer.h"
#include "rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.h"
#include "rendering/rd/world/runtime/terrain_3d_world_runtime.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h"

#include "eastl_config.h"
#include <stddef.h>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include "data/model/terrain_3d_data.h"
#include "rendering/material/terrain_3d_material.h"

class Terrain3DRenderCoordinator : public Terrain3DRenderCoordinatorAccess, public Terrain3DForwardPassHost, public Terrain3DPublicationHost, public Terrain3DPreviewHost, public Terrain3DMegaVTCookHost {
private:

	CLASS_NAME_STATIC( "Terrain3DRenderCoordinator" );

private:
	// OWNER THREAD ONLY: live Godot/Terrain3D state. Render-thread code must never
	// dereference this pointer; render-visible policy belongs in the immutable
	// frame publication. Pointer lifetime is still coordinated by startup/shutdown.
	Terrain3DOwner *m_pTerrain = NULL;
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
	Terrain3DIncrementalMegaVTCooker m_incrementalMegaVTCooker;
	// RENDER THREAD OWNED: RD renderer/runtime resource creation, mutation, and
	// destruction. Owner-thread lifecycle code may control policy but must not
	// inspect or mutate these resource internals as a synchronization mechanism.
	Terrain3DRdClipmapRenderer m_renderer;
	Terrain3DRdInstanceRenderer m_instanceRenderer;
	Terrain3DRdOceanRenderer m_oceanRenderer;
	Terrain3DSharedVTRuntime m_sharedVtRuntime;
	Terrain3DWorldRuntime m_worldRuntime;
	Terrain3DRdPreviewService m_previewService;
	bool initializeForwardRenderer( uint64_t nFrame ) override { return initializeRendererOnRenderThread( nFrame ); }
	const t3dRdFrameSnapshotSlot_t *acquireForwardSnapshot( void ) override { return acquireSnapshotOnRenderThread(); }
	bool ensureForwardSharedVt( void ) override { return ensureSharedVtRuntimeCold(); }
	bool snapshotForwardSharedVt( t3dSharedVTBindings_t &rBindings ) override { return snapshotSharedVtBindingsHot( rBindings ); }
	void publishForwardWorldReadiness( const Terrain3DWorldPublication &rPublication, bool bReady ) override {
		publishStaticWorldReadinessOnRenderThread( rPublication, bReady );
	}
	t3dPublicationServices_t publicationServices( void ) {
		// The services mutate only through the owner-thread publication entry points.
		return { m_pTerrain, m_terrainMegaVT,
			m_materialStateBuilder,
			m_staticScene,
			{ m_pTerrain, m_virtualTexture, m_terrainMegaVT },
			*this };
	}
	void importPublicationMegaVTPersistence( void ) override { importTerrainMegaVTPersistentStateHot(); }
	void reportPublicationError( const String &rError ) override {
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = rError;
	}
	void failPublicationRenderer( void ) override { m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE ); }
	t3dPreviewServices_t previewServices( void ) {
		return { m_pTerrain, m_virtualTexture, m_sharedVtRuntime, m_shaderLibrary,
			m_publicationBuilder, m_effect, m_nStartupGeneration, m_diagnosticsMutex, *this };
	}
	bool composePreviewTerrainPackageCold( PackedByteArray &rPackage, uint32_t &rGeneration, uint64_t &rFingerprint ) override {
		return m_incrementalMegaVTCooker.composePackageCold( rPackage, rGeneration, rFingerprint );
	}
	void requestMegaVTPackageRecomposeCold( void ) override { m_previewService.requestPackageRecomposeCold(); }
	uint64_t publishedMegaVTGpuFingerprintCold( void ) const override { return m_previewService.publishedGpuFingerprintCold(); }
	void persistMegaVTEditorStateCold( void ) override { persistTerrainMegaVTEditorStateHot(); }
	bool loadPreviewPackage( const void *pBytes, uint64_t nBytes ) override { return loadVirtualTexturePackageCold( pBytes, nBytes ); }
	bool ensurePreviewSharedVt( t3dVTSharedResourceClass_t eClass ) override;
	void shutdownPreviewResources( void ) override { shutdownRendererOnRenderThread( true ); }
	void setPreviewReady( uint32_t nGeneration, uint32_t nBit ) override { setRenderLifecycleReadyBitOnRenderThread( nGeneration, nBit ); }
	void clearPreviewReady( uint32_t nBit ) override { clearRenderLifecycleReadyBitOnRenderThread( nBit ); }
	t3dRenderLifecycleStatus_t previewLifecycleStatus( void ) const override { return snapshotRenderLifecycleStatusOnOwnerThread(); }
	t3dDiagnosticInputs_t diagnosticInputs( void ) const {
		return { m_bCompositorDebugEnabled, m_bFailureLatched, m_bForwardBridgeAttached, m_bInstanceRendererInitialized, m_bMotionDebugEnabled, m_bOceanRendererInitialized, m_bRenderEnabled, m_bRendererInitialized, m_diagnosticsMutex, m_forwardPassPreparer, m_initializationError, m_instanceRenderer, m_materialStateBuilder, m_nCallbackCount, m_nConsecutiveRenderFailures, m_nInstanceInitializationAttempts, m_nInstanceInitializationFailures, m_nInstanceInitializationRecoveries, m_nRendererRecoveryCount, m_oceanRenderer, m_pTerrain, m_previewService, m_publicationBuilder, m_renderer, m_shaderLibrary, m_sharedVtRuntime, m_terrainMegaVT, m_virtualTexture, m_worldRuntime, snapshotRenderLifecycleStatusOnOwnerThread(), getSharedVtBindingsHot(),
			isEditorAuthoringMode(), isForwardBridgeReady() };
	}
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
	mutable Terrain3DMutex m_renderLifecycleMutex;
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
	Terrain3DStaticScenePublication m_staticScene;
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
	mutable Terrain3DMutex m_diagnosticsMutex;
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
	static const char *startupGpuStageName( t3dStartupGpuStage_t eStage );
	bool queueRendererRecoveryCold( uint32_t nStartupGeneration, uint64_t nRecoveryRevision );
	bool completeRendererRecoveryOnRenderThread( uint32_t nStartupGeneration, uint64_t nRecoveryRevision );
	bool isStartupGenerationCurrent( uint32_t nStartupGeneration ) const;
	bool isStartupActivationBaseReady( uint32_t nStartupGeneration ) const;
	void publishRenderLifecycleStatusOnRenderThread( const t3dRenderLifecycleStatus_t &rStatus );
	void publishRenderLifecycleStatusCold( const t3dRenderLifecycleStatus_t &rStatus );
	t3dRenderLifecycleStatus_t snapshotRenderLifecycleStatusOnOwnerThread( void ) const;
	void setRenderLifecycleReadyBitOnRenderThread( uint32_t nStartupGeneration, uint32_t nReadyBit );
	void clearRenderLifecycleReadyBitOnRenderThread( uint32_t nReadyBit );
	void publishRenderSharedResourceReadyOnRenderThread( t3dVTSharedResourceClass_t eResourceClass );
	void publishRenderLifecycleFailureOnRenderThread( uint32_t nStartupGeneration, t3dRenderLifecycleFailure_t eFailure );
	bool queueStartupCancellationAckCold( uint32_t nCancelledGeneration, bool bForceDrain );
	bool ensureSharedVtRuntimeCold( t3dVTSharedResourceClass_t eResourceClass = T3D_VT_SHARED_RESOURCES_PRODUCTION );
	bool snapshotSharedVtBindingsHot( t3dSharedVTBindings_t &rBindings ) const;
	const t3dSharedVTBindings_t &getSharedVtBindingsHot( void ) const;
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

	bool beginStartupCold( Terrain3DOwner *pTerrain, uint32_t nStartupGeneration );
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
	bool cancelTerrainMegaVTIncrementalCook( const t3dTerrainMegaVTCookTicket_t &rTicket );
	void render( RenderData *pRenderData );
	bool renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	Dictionary getForwardLaneDiagnostics( void ) const;
	void shutdownRendererOnRenderThread( void ) override { shutdownRendererOnRenderThread( false ); }
	void shutdownRendererOnRenderThread( bool bPreserveTailStaging );
	void shutdownRendererLifecycleOnRenderThread( uint32_t nShutdownGeneration );
	void recoverRendererLifecycleOnRenderThread( uint32_t nStartupGeneration, uint64_t nRecoveryRevision );
	void renderPreviewOnRenderThread( void );
	void renderPreviewAsyncOnRenderThread( void );
	void servicePreviewVtRepublishOnRenderThread( void );
	void serviceShaderStartupOnRenderThread( uint32_t nStartupGeneration );
	void acknowledgeStartupCancellationOnRenderThread( uint32_t nStartupGeneration );
	void serviceStartupWorldPublicationOnRenderThread( uint32_t nStartupGeneration, uint64_t nSnapshotGeneration );
	void consumePreviewReadbackOnRenderThread( uint32_t nSlot, uint64_t nRequestGeneration, const PackedByteArray &rReadbackBytes );
	bool publishStaticWorldCold( uint64_t nProducerId, const Terrain3DWorldPublication &rWorld );
	void releaseStaticSceneDatabaseCold( uint64_t nProducerId );
	bool isStaticWorldReadyCold( uint64_t nProducerId );
	void publishStaticWorldReadinessOnRenderThread( const Terrain3DWorldPublication &rWorld, bool bReady );
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
