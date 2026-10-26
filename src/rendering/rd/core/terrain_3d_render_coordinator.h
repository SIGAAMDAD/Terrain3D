// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_COORDINATOR_H
#define TERRAIN3D_RENDER_COORDINATOR_H

#include "Terrain3DTypes.h"
#if defined( T3D_EDITOR_NATIVE )
#include "authoring/vt/build/terrain_3d_incremental_megavt_cooker.h"
#include "authoring/vt/integration/terrain_3d_composed_surface_edit_journal.h"
#endif

#include "Terrain3DConstants.h"
#if defined( T3D_EDITOR_NATIVE )
#include "rendering/rd/core/terrain_3d_editor_brush_transaction.h"
#include "rendering/rd/core/terrain_3d_editor_brush_gpu.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_lease_table.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#endif
#include "rendering/rd/world/runtime/terrain_3d_static_scene_publication.h"
#include "core/utility/terrain_3d_owner.h"
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
#include "rendering/rd/environment/terrain_3d_rd_environment_field.h"
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

class Terrain3DRenderAuthoringCompanion;
#if defined( T3D_EDITOR_NATIVE )
struct t3dCs14EditorCookRequest_t;
struct t3dCs14EditorCookResult_t;
#endif

class Terrain3DRenderCoordinator : public Terrain3DRenderCoordinatorAccess,
								   public Terrain3DForwardPassHost,
								   public Terrain3DPublicationHost,
								   public Terrain3DPreviewHost
#if defined( T3D_EDITOR_NATIVE )
	,
								   public Terrain3DMegaVTCookHost
#endif
{
private:
	CLASS_NAME_STATIC( "Terrain3DRenderCoordinator" );

private:
	// OWNER THREAD ONLY: live Godot/Terrain3D state. Render-thread code must never
	// dereference this pointer; render-visible policy belongs in the immutable
	// frame publication. Pointer lifetime is still coordinated by startup/shutdown.
	Terrain3DOwner					  *m_pTerrain = NULL;
	Terrain3DMemoryZone				  *m_pRuntimeAllocatorZone = NULL;
	t3dAllocatorOverflowPolicy_t	   m_eRuntimeAllocatorOverflowPolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT;
	bool							   m_bRuntimeAllocatorBound = false;
	Ref< Terrain3DRdCompositorEffect > m_effect;
	Ref< Compositor >				   m_attachedCompositor;
	uint64_t						   m_nAttachedCameraId = 0u;
	bool							   m_bCreatedCompositor = false;

	// OWNER THREAD MUTATION: publication/material builders. The render thread may
	// consume only acquired immutable snapshots and immutable banks selected by a
	// published bank index; it must not inspect producer-side mutable counters.
	Terrain3DRenderPublicationBuilder m_publicationBuilder;
	// Prepared on the owner before startup CPU publication; immutable until the
	// GPU startup stage acknowledges its resources and the owner commits them.
	Terrain3DStPublicationImage m_stStartupCandidate;
    // Render-side upload token published only after all six device resources and
    // the native/WORLD bindings are installed. It is cleared before teardown.
    t3dAtomicU64_t m_nStGpuReadySignature{ 0u };
    // The owner marks only successfully committed candidates. The render side
    // may discard a prepared, uncommitted GPU candidate without racing reads.
    t3dAtomicU64_t m_nStCommittedSignature{ 0u };
    // Two-thread cold activation: 0 idle, 1 ready, 2 owner commit in
    // progress, 3 committed, 4 render-side discard in progress.
    t3dAtomicU32_t m_nStActivationState{ 0u };
	Terrain3DMaterialStateBuilder	  m_materialStateBuilder;
	Terrain3DForwardPassPreparer	  m_forwardPassPreparer;
	Terrain3DDiagnosticsPublisher	  m_diagnosticsPublisher;
	Terrain3DRdShaderLibrary		  m_shaderLibrary;
	// The shared VT owner must outlive all borrowed VT clients and renderers.
	// Its GPU mutation remains render-thread-only.
	Terrain3DSharedVTRuntime		   m_sharedVtRuntime;
	Terrain3DTerrainMegaVT			   m_terrainMegaVT;
#if defined( T3D_EDITOR_NATIVE )
    // A render-owned transient stroke. Only metadata and immutable byte payloads
    // cross the editor/render-thread boundary under m_editorBrushMutex.
    // The editor never obtains or frees m_editorBrushWorking RID.
    mutable Terrain3DMutex m_editorBrushMutex;
    Terrain3DEditorBrushTransaction m_editorBrushTransaction;
    Terrain3DRdGpuBuffer m_editorBrushWorking;
    Terrain3DRdGpuBuffer m_editorBrushTileTable;
    Terrain3DRdGpuBuffer m_editorBrushMask;
    Terrain3DRdShader m_editorBrushShader;
    Terrain3DRdPipeline m_editorBrushPipeline;
    struct EditorBrushSetKey {
        RID shader, working, tiles, mask;
        bool operator==( const EditorBrushSetKey &r ) const {
            return shader == r.shader && working == r.working && tiles == r.tiles && mask == r.mask;
        }
    };
    Terrain3DRdDescriptorLeaseTable< EditorBrushSetKey, 1u > m_editorBrushSets{
        Terrain3DRdContext::getShared(), T3D_RD_CONTEXT_OWNER_TERRAIN };
    RID m_hEditorBrushComputeSet;
    PackedByteArray m_editorBrushMaskUpload;
    uint32_t m_nEditorBrushMaskWidth = 0u, m_nEditorBrushMaskHeight = 0u;
    PackedByteArray m_editorBrushPushBytes;
    PackedByteArray m_editorBrushInitial;
    PackedByteArray m_editorBrushCompleted;
    bool m_bEditorBrushGpuPending = false;
    // Set only by successful shared-RD brush dispatch, never by source upload.
    // Readback must not masquerade as a completed brush without GPU output.
    bool m_bEditorBrushOutputProduced = false;
    bool m_bEditorBrushReadbackInFlight = false;
    uint64_t m_nEditorBrushRejectedCompletions = 0u;
    uint64_t m_nLastEditorBrushStrokeId = 0u;
    void initializeEditorBrushOnRenderThread( uint64_t nStrokeId ) override;
    void sealEditorBrushOnRenderThread( uint64_t nStrokeId ) override;
    void uploadEditorBrushTileOnRenderThread( uint64_t nStrokeId, uint32_t nTile, const PackedByteArray &rSource ) override;
    void executeEditorBrushCommandOnRenderThread( uint64_t nStrokeId, uint32_t nCommand ) override;
    bool prepareEditorBrushComputeOnRenderThread();
    void discardEditorBrushOnRenderThread( uint64_t nStrokeId ) override;
    void releaseEditorBrushOnRenderThread( void );
#endif
#if defined( T3D_EDITOR_NATIVE )
    Terrain3DComposedSurfaceEditJournal m_composedSurfaceEdits;
    uint64_t m_nComposedSourceGeneration = 1u;
    uint64_t m_nComposedTopologyGeneration = 0u;
#endif
	Terrain3DRenderAuthoringCompanion *m_pAuthoring = NULL;
	void							   serviceForceRebakeCold( void );
#if defined( T3D_EDITOR_NATIVE )
    void serviceComposedReferenceBakeCold( void );
#endif
#if defined( T3D_EDITOR_NATIVE )
	void serviceAuthoringUpdateCold( void );
#endif
public:
	EA_FORCE_INLINE uint64_t getPreparedHlodSourceGenerationCold( void ) const {
		return m_nPreparedHlodSourceGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	}

    void clearEnvironmentRegionClimateOwner() {
        m_publicationBuilder.clearEnvironmentRegionClimateOwner();
    }
    bool setEnvironmentRegionClimateOwner( const Vector2i &rRegion, const Vector3 &rClimate ) {
        return m_publicationBuilder.setEnvironmentRegionClimateOwner( rRegion, rClimate );
    }
    Vector3 getEnvironmentRegionClimateOwner( const Vector2i &rRegion ) const {
        return m_publicationBuilder.getEnvironmentRegionClimateOwner( rRegion );
    }
    PackedByteArray exportEnvironmentRegionClimateOwner() const {
        return m_publicationBuilder.exportEnvironmentRegionClimateOwner();
    }
    bool importEnvironmentRegionClimateOwner( const PackedByteArray &rData ) {
        return m_publicationBuilder.importEnvironmentRegionClimateOwner( rData );
    }
    uint64_t getEnvironmentRegionClimateEpochOwner() const {
        return m_publicationBuilder.getEnvironmentRegionClimateEpochOwner();
    }
    void setEnvironmentForcingOwner( const t3dEnvironmentForcing_t &rForcing ) {
        m_publicationBuilder.setEnvironmentForcingOwner( rForcing );
    }
    uint32_t submitEnvironmentImpulsesOwner( const t3dEnvironmentImpulse_t *pEvents, uint32_t nCount ) {
        return m_publicationBuilder.submitEnvironmentImpulsesOwner( pEvents, nCount );
    }
    uint64_t getEnvironmentDroppedCountOwner() const {
        return m_publicationBuilder.getEnvironmentDroppedCountOwner();
    }
#if defined( T3D_EDITOR_NATIVE )
    // Explicit editor-owner cold call; no automatic per-frame cook.
    bool cookComposedSurfaceSidecarEditorCold( const t3dCs14EditorCookRequest_t &request,
                                               t3dCs14EditorCookResult_t &result );
#endif
#if defined( T3D_EDITOR_NATIVE )
    // BRP-04/06: backend readiness and explicit seal/readback. The GPU brush
    // producer and clipmap overlay are integrated in subsequent operations;
    // these entry points do not mutate Terrain3DRegion data.
    bool beginEditorBrushStrokeCold( const Terrain3DEditorBrushTransaction &rTransaction,
                                     const PackedByteArray &rSource, const PackedByteArray &rMaskBits,
                                     uint32_t nMaskWidth, uint32_t nMaskHeight );
    bool appendEditorBrushTileCold( uint64_t nStrokeId, const t3dEditorBrushTile_t &rTile,
                                    const PackedByteArray &rSource );
    bool enqueueEditorBrushCommandCold( uint64_t nStrokeId, const t3dEditorBrushCommand_t &rCommand );
    bool sealEditorBrushStrokeCold( uint64_t nStrokeId );
    t3dEditorBrushStrokeState_t getEditorBrushStrokeStateCold( uint64_t nStrokeId ) const;
    void cancelEditorBrushStrokeCold( uint64_t nStrokeId );
    bool takeEditorBrushReadbackCold( uint64_t nStrokeId,
                                      Terrain3DEditorBrushTransaction &rTransaction,
                                      PackedByteArray &rBytes );
    bool acknowledgeEditorBrushCommitCold( uint64_t nStrokeId );
    void consumeEditorBrushReadbackOnRenderThread( const PackedByteArray &rBytes,
                                                    uint64_t nStrokeId, uint32_t nOwnerGeneration, uint64_t nDeviceGeneration ) override;
#endif
	uint32_t getMegaVTTexelsPerMeterCold() const {
		return m_terrainMegaVT.getTexelsPerMeter();
	}

private:
	// RENDER THREAD OWNED: RD renderer/runtime resource creation, mutation, and
	// destruction. Owner-thread lifecycle code may control policy but must not
	// inspect or mutate these resource internals as a synchronization mechanism.
	Terrain3DRdClipmapRenderer	m_renderer;
	Terrain3DRdInstanceRenderer m_instanceRenderer;
	Terrain3DRdOceanRenderer	m_oceanRenderer;
    Terrain3DRdEnvironmentField m_environmentField;
	uint32_t					m_nLastSharedVtLogStatus = UINT32_MAX;
	uint32_t					m_nLastSharedVtLogGeneration = UINT32_MAX;
	t3dVTSharedResourceClass_t	m_eLastSharedVtLogClass = T3D_VT_SHARED_RESOURCES_NONE;
	Terrain3DWorldRuntime		m_worldRuntime;
	Terrain3DRdPreviewService	m_previewService;
	bool						isForwardShaderReady(
							   void ) const override {
		return m_shaderLibrary.isReady();
	}
	bool isForwardRendererReady(
		void ) const override {
		return m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	const t3dRdFrameSnapshotSlot_t *acquireForwardSnapshot(
		void ) override {
		return acquireSnapshotOnRenderThread();
	}
	t3dVTAcquireStatus_t acquireForwardSharedVt(
		t3dSharedVTBindings_t &rBindings ) const override {
		return m_sharedVtRuntime.acquirePublishedBindingsHot( T3D_VT_SHARED_RESOURCES_PRODUCTION, rBindings );
	}
	void publishForwardWorldReadiness(
		const Terrain3DWorldPublication &rPublication,
		bool							 bReady ) override {
		publishStaticWorldReadinessOnRenderThread( rPublication, bReady );
	}
	t3dPublicationServices_t publicationServices(
		void ) {
		// The services mutate only through the owner-thread publication entry points.
		return { m_pTerrain,
				 m_terrainMegaVT,
                 m_sharedVtRuntime.virtualTexture(),
				 m_materialStateBuilder,
				 m_staticScene,
				 { m_pTerrain, m_sharedVtRuntime.virtualTexture(), m_terrainMegaVT },
				 *this };
	}
	void importPublicationMegaVTPersistence(
		void ) override {
		importTerrainMegaVTPersistentStateHot();
	}
	void reportPublicationError(
		const String &rError ) override {
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = rError;
	}
	void failPublicationRenderer(
		void ) override {
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
	}
	t3dPreviewServices_t previewServices(
		void ) {
		return { m_pTerrain,
				 m_sharedVtRuntime.virtualTexture(),
				 m_sharedVtRuntime,
				 m_shaderLibrary,
				 m_publicationBuilder,
				 m_effect,
				 m_nStartupGeneration,
				 m_diagnosticsMutex,
				 *this };
	}
	bool   composePreviewTerrainPackageCold( PackedByteArray				  &rPackage,
											 uint32_t						  &rGeneration,
											 uint64_t						  &rFingerprint,
											 Terrain3DSharedMemoryReservation &rPackageMemory ) override;
	String previewTerrainCompositionErrorCold() const override;
#if defined( T3D_EDITOR_NATIVE )
	void requestMegaVTPackageRecomposeCold(
		void ) override {
		m_previewService.requestPackageRecomposeCold();
	}
	uint64_t publishedMegaVTGpuFingerprintCold(
		void ) const override {
		return m_previewService.publishedGpuFingerprintCold();
	}
	void persistMegaVTEditorStateCold(
		void ) override {
		persistTerrainMegaVTEditorStateHot();
	}
#endif
	bool loadPreviewPackage(
		const void *pBytes,
		uint64_t	nBytes ) override {
		return loadVirtualTexturePackageCold( pBytes, nBytes );
	}
	bool stagePreviewPackageWarm(
		const void *pBytes,
		uint64_t	nBytes ) override {
		return m_sharedVtRuntime.stagePackageWarm( pBytes, nBytes );
	}
	bool servicePreviewPackageWarm(
		uint64_t nMaxBytes,
		uint32_t nMaxWorkUnits ) override {
		return m_sharedVtRuntime.servicePackageRepublishWarm( nMaxBytes, nMaxWorkUnits );
	}
	bool rebuildPreviewPackageResourcesCold( const void				   *pBytes,
											 uint64_t					nBytes,
											 bool						bNeutral,
											 t3dVTSharedResourceClass_t eClass ) override;
	bool ensurePreviewSharedVt( t3dVTSharedResourceClass_t eClass ) override;
	void setPreviewReady(
		uint32_t nGeneration,
		uint32_t nBit ) override {
		setRenderLifecycleReadyBitOnRenderThread( nGeneration, nBit );
	}
	void clearPreviewReady(
		uint32_t nBit ) override {
		clearRenderLifecycleReadyBitOnRenderThread( nBit );
	}
	t3dRenderLifecycleStatus_t previewLifecycleStatus(
		void ) const override {
		return snapshotRenderLifecycleStatusOnOwnerThread();
	}
	t3dDiagnosticInputs_t diagnosticInputs(
		void ) const {
		t3dSharedVTBindings_t bindings{};
		( void ) m_sharedVtRuntime.acquirePublishedBindingsHot( m_sharedVtRuntime.get_shared_resource_class(),
																bindings );
		return { m_bCompositorDebugEnabled,
				 m_bFailureLatched,
				 m_bForwardBridgeAttached,
				 m_bInstanceRendererInitialized,
				 m_bMotionDebugEnabled,
				 m_bOceanRendererInitialized,
				 m_bRenderEnabled,
				 m_bRendererInitialized,
				 m_diagnosticsMutex,
				 m_forwardPassPreparer,
				 m_initializationError,
				 m_materialStateBuilder,
				 m_nCallbackCount,
				 m_nConsecutiveRenderFailures,
				 m_nInstanceInitializationAttempts,
				 m_nInstanceInitializationFailures,
				 m_nInstanceInitializationRecoveries,
				 m_nRendererRecoveryCount,
				 m_oceanRenderer,
				 m_pTerrain,
				 m_previewService,
				 m_publicationBuilder,
				 m_renderer,
				 m_shaderLibrary,
				 m_sharedVtRuntime,
				 m_terrainMegaVT,
				 m_sharedVtRuntime.virtualTexture(),
				 m_worldRuntime,
				 snapshotRenderLifecycleStatusOnOwnerThread(),
				 bindings,
				 isEditorAuthoringMode(),
				 isForwardBridgeReady() };
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
	// Once per 16 render frames, the render thread publishes a bounded POD
	// telemetry snapshot. Owner diagnostics never walk render-owned VT arrays.
	struct t3dPublishedDiagnostics_t
	{
		t3dVTFinalTelemetry_t vt{};
		uint64_t			  nRenderFrame = 0u;
		bool				  bVtPackageFileBacked = false;
		uint64_t			  nVtPackageMetadataBytes = 0u;
		uint64_t			  nVtPackageFileBytes = 0u;
		uint64_t			  nFeedbackAttempts = 0u;
		uint64_t			  nFragmentFaults = 0u;
		uint64_t			  nFeedbackAccepted = 0u;
		uint64_t			  nPredictionIncompleteFallbacks = 0u;
		uint64_t			  nMaskParamUploadsSaved = 0u;
		bool				  bPredictionCurrentCellComplete = false;
		uint64_t			  nActiveJobs = 0u;
		uint64_t			  nResidencyHotScans = 0u;
		uint64_t			  nResidencyWarmScans = 0u;
		uint64_t			  nResidencyColdScans = 0u;
		uint64_t			  nResidencyReservationFailures = 0u;
		uint64_t			  nResidencyRetireDeferred = 0u;
		uint64_t			  nResidencyThrashWindowFrames = 0u;
		uint64_t			  nShaderPhysicalReady = 0u;
		uint64_t			  nShaderPhysicalExpected = 0u;
		uint64_t              nShaderPhysicalMisses = 0u;
        // Render-owned, bounded POD telemetry. No cross-thread RD access in getDiagnostics().
        uint64_t nEnvironmentGeneration = 0u;
        uint64_t nEnvironmentFramesIntegrated = 0u;
        uint64_t nEnvironmentImpulsesAccepted = 0u;
        uint64_t nEnvironmentIntegrationFailures = 0u;
        uint64_t nEnvironmentTeleportsReset = 0u;
        uint64_t nEnvironmentTimestampPairs = 0u;
        uint64_t nEnvironmentTextureBytes = 0u;
        uint64_t nEnvironmentClimateUploads = 0u;
        uint64_t nEnvironmentWorldSetMisses = 0u;
        uint64_t nEnvironmentOceanSetMisses = 0u;
        bool bEnvironmentReady = false;
        bool bEnvironmentFaulted = false;
        bool bEnvironmentEnabled = false;
        t3dRdGlobalDescriptorDiagnostics_t descriptorCache{};
	};
	mutable Terrain3DMutex	   m_publishedDiagnosticsMutex;
	t3dPublishedDiagnostics_t  m_publishedDiagnostics{};
	uint64_t				   m_nLastDiagnosticsFrameOnRenderThread = UINT64_MAX;
	bool					   publishDiagnosticsOnRenderThread( uint64_t nFrame );
	mutable Terrain3DMutex	   m_vtDemandIndexMutex;
	String					   m_vtDemandIndexPath;
	String					   m_vtDemandIndexStatus;
	mutable Terrain3DMutex	   m_renderLifecycleMutex;
	t3dRenderLifecycleStatus_t m_renderLifecycleStatus{};

	t3dAtomicBool_t					m_bRendererFailed{ false };
	bool							m_bFailureLatched = false;
	uint64_t						m_nFailureLatchTerrainTextureGeneration = 0u;
	uint64_t						m_nFailureLatchMaterialTextureGeneration = 0u;
	uint64_t						m_nRendererRecoveryCount = 0u;
	uint64_t						m_nRecoveryRequestRevision = 0u;
	uint64_t						m_nRecoveryQueuedRevision = 0u;
	t3dAtomicU64_t					m_nRecoveryActiveRevision{ 0u };
	// Render-thread exact WORLD upload acknowledgement, owner-thread read only.
	t3dAtomicU64_t                 m_nPreparedHlodSourceGeneration{ 0u };
	t3dAtomicU64_t m_nGpuSafeMeshPublicationGeneration{ 0u };
	// Render-thread-only pending async fence target; never interpreted as completion.
	uint64_t m_nPendingMeshRetirementPublication = 0u;
	uint64_t m_nPendingMeshRetirementEpoch = 0u;
	t3dAtomicBool_t					m_bRecoveryInProgress{ false };
	bool							m_bForwardBridgeAttached = false;
	bool							m_bCompositorDebugEnabled = false;
	bool							m_bMotionDebugEnabled = false;
	t3dAtomicU64_t					m_nCallbackCount{ 0u };
	uint64_t						m_nInstanceInitializationAttempts = 0u;
	uint64_t						m_nInstanceInitializationFailures = 0u;
	uint64_t						m_nInstanceInitializationRecoveries = 0u;
	uint64_t						m_nLastInstanceInitializationRenderFrame = UINT64_MAX;
	uint64_t						m_nLastOceanInitializationRenderFrame = UINT64_MAX;
	Terrain3DStaticScenePublication m_staticScene;
	t3dAtomicU32_t					m_nConsecutiveRenderFailures{ 0u };
	t3dAtomicU32_t					m_nWorldUploadBudgetKbPolicy{ 4096u };
	t3dAtomicU32_t					m_nWorldClusterBudgetPolicy{ 8192u };
	t3dAtomicU32_t					m_nVtRequestBudgetPolicy{ 2048u };
	t3dAtomicU32_t					m_nVtAdmissionBudgetPolicy{ 64u };
	t3dAtomicBool_t					m_bWorldBoundaryDeferredPolicy{ false };
	t3dAtomicU32_t					m_nStartupGeneration{ 0u };
	uint64_t						m_nStartupPublicationPrepareCount = 0u;
	uint64_t						m_nStartupPublicationPrepareUsecLast = 0u;
	uint64_t						m_nStartupRendererInitializeAttemptCount = 0u;
	uint64_t						m_nStartupRendererInitializeSuccessCount = 0u;
	uint64_t						m_nStartupRendererInitializeUsecLast = 0u;
	uint64_t						m_nStartupRendererInitializeUsecTotal = 0u;
	uint64_t						m_nStartupGpuServiceCount = 0u;
	uint64_t						m_nStartupGpuServiceUsecLast = 0u;
	uint64_t						m_nStartupOwnerServiceCount = 0u;
	uint64_t						m_nStartupOwnerServiceLastLogUsec = 0u;
	uint64_t						m_nStartupOwnerServiceSignature = UINT64_MAX;
	t3dAtomicU64_t					m_nStartupStaleCompletionRejectCount{ 0u };
	uint64_t						m_nStartupCancellationCount = 0u;
	uint64_t						m_nStartupRenderDrainCount = 0u;
	uint64_t						m_nStartupActivationBarrierCount = 0u;
	uint64_t						m_nStartupActivationBarrierUsecLast = 0u;
	uint64_t						m_nStartupActivationSnapshotGeneration = 0u;
	uint64_t						m_nStartupFinalSyncSuccessCount = 0u;
	uint64_t						m_nStartupFinalSyncFailureCount = 0u;
	uint64_t						m_nStartupFinalSyncBeginUsec = 0u;
	uint64_t						m_nStartupFinalSyncElapsedUsecLast = 0u;
	t3dAtomicU32_t					m_nStartupGpuStage{ T3D_STARTUP_GPU_VT_STORAGE };
	bool							m_bStartupTerrainRendererPrepared = false;
	bool							m_bStartupWorldRuntimePrepared = false;
	t3dAtomicBool_t					m_bStartupWorldPublicationQueued{ false };
	t3dAtomicBool_t					m_bStartupWorldPublicationPrepared{ false };
	t3dAtomicU64_t					m_nStartupWorldPublicationGeneration{ 0u };
	t3dAtomicU64_t					m_nStartupWorldPublicationPrepareUsecLast{ 0u };
	t3dAtomicBool_t					m_bStartupGpuServiceComplete{ false };
	bool							m_bStartupActivationPrepared = false;
	bool							m_bStartupActivationCommitted = false;
	t3dAtomicBool_t					m_bStartupColdBegun{ false };
	t3dAtomicBool_t					m_bStartupColdPrepared{ false };
	t3dAtomicBool_t					m_bStartupColdFailed{ false };
	mutable Terrain3DMutex			m_diagnosticsMutex;
	String							m_initializationError;

	uint64_t						hashBytes( const void *pData, size_t nBytes ) const;
	uint64_t						advanceGeneration( uint64_t nGeneration ) const;
	void							publishSnapshot( void );
	const t3dRdFrameSnapshotSlot_t *acquireSnapshotOnRenderThread( void );
	void							resetSnapshotPublication( void );
	void							attachEffectToActiveCamera( void );
#if defined( T3D_EDITOR_NATIVE )
	Camera3D *editorViewportCameraCold( void ) const;
#endif
	void			   detachEffect( void );
	bool			   initializeRendererOnRenderThread( uint64_t nRenderFrame = UINT64_MAX );
	void			   serviceShaderStartupCold( void );
	bool			   serviceStartupGpuOnRenderThreadBudgeted( uint32_t nStartupGeneration );
	bool			   queueStartupGpuServiceOnRenderThread( uint32_t nStartupGeneration );
	static const char *startupGpuStageName( t3dStartupGpuStage_t eStage );
	bool			   queueRendererRecoveryCold( uint32_t nStartupGeneration, uint64_t nRecoveryRevision );
	bool completeRendererRecoveryOnRenderThread( uint32_t nStartupGeneration, uint64_t nRecoveryRevision );
	bool isStartupGenerationCurrent( uint32_t nStartupGeneration ) const;
	bool isStartupActivationBaseReady( uint32_t nStartupGeneration ) const;
	void publishRenderLifecycleStatusOnRenderThread( const t3dRenderLifecycleStatus_t &rStatus );
	void publishRenderLifecycleStatusCold( const t3dRenderLifecycleStatus_t &rStatus );
	t3dRenderLifecycleStatus_t snapshotRenderLifecycleStatusOnOwnerThread( void ) const;
	void setRenderLifecycleReadyBitOnRenderThread( uint32_t nStartupGeneration, uint32_t nReadyBit );
	void clearRenderLifecycleReadyBitOnRenderThread( uint32_t nReadyBit );
	void publishRenderSharedResourceReadyOnRenderThread( t3dVTSharedResourceClass_t eResourceClass );
	void publishRenderLifecycleFailureOnRenderThread( uint32_t					  nStartupGeneration,
													  t3dRenderLifecycleFailure_t eFailure );
	bool queueStartupCancellationAckCold( uint32_t nCancelledGeneration, bool bForceDrain );
	void logSharedVtEnsureStatusOnRenderThread( uint32_t nStatus, t3dVTSharedResourceClass_t eRequestedClass );
	bool ensureSharedVtRuntimeCold( t3dVTSharedResourceClass_t eResourceClass = T3D_VT_SHARED_RESOURCES_PRODUCTION );
	bool reloadPublishedPreviewVtPackageCold( void );
	bool preparePreviewAuthoringPackageCold( const Ref< Terrain3DAssets > &rAssets, String *pError );
	bool loadRuntimeVirtualTexturePackageFileCold( String *pError );
	void stageAssetVtDemandIndexCold( void );
	bool attachForwardBridge( void );
	void detachForwardBridge( void );
	bool isForwardBridgeReady( void ) const;
	void logStartupActivationFailureState( const char *pReason, uint32_t nStartupGeneration ) const;
	void updateRenderRoute( void );
	void resetForwardLifecycle( void );
	void importTerrainMegaVTPersistentStateHot( void );
	void persistTerrainMegaVTEditorStateHot( void );

public:
    // Strict cold two-phase activation. The owner prepares ST03/ST04 sidecars
    // and a parity-approved COMPOSED allocation before calling these methods.
    // Upload runs on the render thread; commit runs on the owner thread only
    // after the matching GPU token is published. Live replacement is deferred
    // until the ST06 generation-safe double-bank migration.
    // Load a published ST06 surface package, validating every region against
    // the currently loaded V1 COMPOSED allocation before GPU submission.
    bool prepareSuperTerrainPackageOwnerCold( const PackedByteArray &rPackage,
                                               Terrain3DStPublicationImage &rCandidate,
                                               uint32_t nPinnedMaskWords,
                                               uint32_t nFinePageBudget );
    bool uploadSuperTerrainCandidateOnRenderThread( const Terrain3DStPublicationImage &rCandidate,
                                                     uint32_t nFinePageSlots );
    bool discardSuperTerrainUncommittedOnRenderThread( uint64_t nExpectedSignature );
    bool commitSuperTerrainCandidateOwnerCold( Terrain3DStPublicationImage &rCandidate,
                                                uint64_t nRegionTopology );
	bool	   bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	bool	   loadVirtualTexturePackageCold( const void *pPackageBytes, uint64_t nPackageBytes );
	bool	   setVirtualTextureQualityProfile( uint32_t nProfile );
	bool	   resolveVirtualTextureMaterialAllocationCold( uint64_t nStableKey, uint32_t &rAllocationId ) const;
	bool	   bakePreviewVirtualTexturesCold( godot::String *pError = NULL );
	Dictionary forceRebakeVirtualTexturesCold( void );
	Dictionary bakeChangedVirtualTexturesCold( void );
	Dictionary getForceRebakeVirtualTexturesStatusCold( void ) const;
	void	   serviceColdCapacity( void );
	Terrain3DRenderCoordinator( void ) = default;
	~Terrain3DRenderCoordinator( void );

	bool				 beginStartupCold( Terrain3DOwner *pTerrain, uint32_t nStartupGeneration );
	void				 cancelStartupCold( uint32_t nStartupGeneration );
	void				 serviceStartupCold( uint32_t nStartupGeneration );
	void				 logStartupFailureDiagnosticsCold( const char *pszService, uint32_t nStartupGeneration ) const;
	bool				 isStartupActivationReady( uint32_t nStartupGeneration ) const;
	bool				 prepareStartupActivationCold( uint32_t nStartupGeneration );
	EA_FORCE_INLINE bool isStartupActivationPrepared(
		const uint32_t nStartupGeneration ) const {
		return m_bStartupActivationPrepared && !m_bStartupColdFailed && m_nStartupGeneration == nStartupGeneration;
	}
	EA_FORCE_INLINE bool hasStartupActivationSnapshot(
		const uint32_t nStartupGeneration ) const {
		return m_nStartupGeneration == nStartupGeneration && m_nStartupActivationSnapshotGeneration != 0u;
	}
	bool				 commitStartupActivationCold( uint32_t nStartupGeneration );
	EA_FORCE_INLINE bool isStartupPrepared(
		const uint32_t nStartupGeneration ) const {
		return m_bStartupColdPrepared && !m_bStartupColdFailed && m_nStartupGeneration == nStartupGeneration;
	}
	EA_FORCE_INLINE bool isStartupFailed(
		const uint32_t nStartupGeneration ) const {
		return m_bStartupColdFailed && m_nStartupGeneration == nStartupGeneration;
	}
	bool	   prepareStartupCold( uint32_t nStartupGeneration );
	void	   initializeOcean( void );
	void	   shutdown( void );
	void	   update( void );
	void	   onTerrainMapsEdited( const AABB &rEditedArea );
	bool	   claimTerrainMegaVTIncrementalCook( uint32_t									nRegionMapIndex,
												  t3dTerrainMegaVTIncrementalCookRequest_t &rRequest,
												  t3dTerrainMegaVTCookTicket_t			   &rTicket );
	bool	   validateTerrainMegaVTIncrementalCookCompletion( const t3dTerrainMegaVTCookTicket_t &rTicket );
	bool	   acceptTerrainMegaVTIncrementalCookCompletion( const t3dTerrainMegaVTCookTicket_t &rTicket );
	bool	   cancelTerrainMegaVTIncrementalCook( const t3dTerrainMegaVTCookTicket_t &rTicket );
	void	   render( RenderData *pRenderData );
	bool	   renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	Dictionary getForwardLaneDiagnostics( void ) const;
	void	   shutdownRendererOnRenderThread(
			  void ) override {
		shutdownRendererOnRenderThread( false );
	}
	void shutdownRendererOnRenderThread( bool bPreserveTailStaging );
	void shutdownRendererLifecycleOnRenderThread( uint32_t nShutdownGeneration );
	void recoverRendererLifecycleOnRenderThread( uint32_t nStartupGeneration, uint64_t nRecoveryRevision );
	void renderPreviewOnRenderThread( void );
	void renderPreviewAsyncOnRenderThread( void );
	void servicePreviewVtRepublishOnRenderThread( void );
	void serviceShaderStartupOnRenderThread( uint32_t nStartupGeneration );
	void acknowledgeStartupCancellationOnRenderThread( uint32_t nStartupGeneration );
	void serviceStartupWorldPublicationOnRenderThread( uint32_t nStartupGeneration, uint64_t nSnapshotGeneration );
	void consumeFragmentFeedbackOnRenderThread( const PackedByteArray &rBytes ) override;
	void consumeDescriptorFenceOnRenderThread( const PackedByteArray &rBytes,
		uint64_t nEpoch, uint64_t nDeviceGeneration, uint64_t nTicket ) override;
	void consumePreviewReadbackOnRenderThread( uint32_t				  nSlot,
											   uint64_t				  nRequestGeneration,
											   const PackedByteArray &rReadbackBytes );
	bool publishStaticWorldCold( uint64_t nProducerId, const Terrain3DWorldPublication &rWorld );
	void releaseStaticSceneDatabaseCold( uint64_t nProducerId );
	bool isStaticWorldReadyCold( uint64_t nProducerId );
	void publishStaticWorldReadinessOnRenderThread( const Terrain3DWorldPublication &rWorld, bool bReady );
	Ref< Image > createMeshPreview( uint16_t nAssetId, const Vector2i &rSize, t3dRdPreviewFailure_t *pFailure = NULL );
	bool		 requestMeshPreviewAsync( uint16_t				 nAssetId,
										  const Vector2i		&rSize,
										  uint64_t				 nCompletionOwnerId,
										  uint64_t				 nCompletionGeneration,
										  t3dRdPreviewFailure_t *pFailure = NULL );
	Dictionary	 getDiagnostics( void ) const;				   // Polled, published counters only; never syncs.
	Dictionary	 getDeepDiagnostics( void ) const;			   // Explicit cold inspection; may synchronize.
	Dictionary	 getPublishedStartupDiagnostics( void ) const; // Polled atomic startup snapshots only.
	Dictionary	 getStartupDiagnostics( void ) const;
	EA_FORCE_INLINE t3dStartupDomainSnapshot_t getShaderStartupState(
		void ) const {
		return m_shaderLibrary.getStartupSnapshot();
	}
	EA_FORCE_INLINE t3dStartupDomainSnapshot_t getVirtualTextureStartupState(
		void ) const {
		return m_sharedVtRuntime.virtualTexture().get_startup_snapshot();
	}
	size_t	 getRetainedCpuBytes( void ) const;
	void	 setUnifiedWorldFramePolicy( uint32_t nWorldUploadKb,
										 uint32_t nWorldClusters,
										 uint32_t nVtRequests,
										 uint32_t nVtAdmissions,
										 bool	  bBoundaryDeferred );
	uint64_t getWorldGpuBufferBytes( void ) const;
	uint64_t getWorldHiZGpuBytes( void ) const;
	size_t	 getWorldRetainedCpuBytes( void ) const;
	uint64_t getWorldBudgetDeferralCount( void ) const;
	uint64_t getVirtualTextureBudgetDeferralCount( void ) const;
	uint64_t getVirtualTextureCpuBytes( void ) const;
	uint64_t getVirtualTextureGpuBytes() const;
	bool	 isEditorAuthoringMode( void ) const;
};

#endif // TERRAIN3D_RENDER_COORDINATOR_H
