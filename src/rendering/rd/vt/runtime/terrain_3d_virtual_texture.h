// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VIRTUAL_TEXTURE_H
#define TERRAIN3D_VIRTUAL_TEXTURE_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "core/config/terrain_3d_startup_state.h"
#include "memory/arena/terrain_3d_memory_zone.h"
#include "rendering/rd/vt/core/terrain_3d_vt_contract.h"
#include "rendering/rd/vt/gpu/terrain_3d_vt_gpu.h"
#include "rendering/rd/vt/io/terrain_3d_vt_pack_runtime.h"

#include <godot_cpp/variant/dictionary.hpp>

struct t3dRdForwardPassContext_s;

// t3dVirtualTextureStats_s centralized in Terrain3DTypes.h.


// t3dVTRequestProducer_e centralized in Terrain3DTypes.h.


typedef struct t3dVTStagedRequest_s {

	t3dVTRuntimeRequest_t Request;
	uint16_t nProducer;
	uint16_t nReserved;
} t3dVTStagedRequest_t;


typedef struct t3dVTMaterialReadSnapshot_s {
	uint32_t nDatabaseKind;
	uint32_t nDatabaseGeneration;
	uint32_t nPackageGeneration;
	uint32_t nMaterialCount;
	uint64_t nPackageContentSignature;
	uint64_t nPackagePublicationGeneration;
	uint32_t nMaterialAllocation[T3D_RD_MATERIAL_LAYER_COUNT];
} t3dVTMaterialReadSnapshot_t;


// t3dVTFinalTelemetry_s centralized in Terrain3DTypes.h.


class Terrain3DVirtualTexture {
public:
	Terrain3DVirtualTexture();
	~Terrain3DVirtualTexture();

	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone );
	bool configure_streaming_limits_cold( const t3dVTStreamingLimits_t &pLimits );
	void set_frame_budgets_hot( uint32_t nRequestBudget, uint32_t nAdmissionBudget );
	const t3dVTStreamingLimits_t &get_streaming_limits() const;
	bool configure_residency_policy_cold( const t3dVTResidencyPolicy_t &pPolicy );
	const t3dVTResidencyPolicy_t &get_residency_policy() const;
	bool prepare_cpu_cold( const t3dWorldLimits_t &pLimits, const t3dVTGpuResourcePlan_t &pPlan, uint32_t nStartupGeneration );
	bool prepare_gpu_storage_cold( Terrain3DVTResourceImage *pResources );
	bool bind_gpu_executable_cold();
	bool service_gpu_executable_cold( uint32_t nMaxWorkUnits );
	bool publish_initial_cold();
	bool service_initial_publication_cold( uint64_t nMaxBytes, uint32_t nMaxWorkUnits );
	void shutdown_cold( bool bPreserveTailStaging = false );
	void seal_hot_runtime();

	bool is_initialized() const;
	bool is_cpu_ready() const;
	bool is_gpu_storage_ready() const;
	bool is_gpu_ready() const;
	bool is_initial_published() const;
	bool is_hot_sealed() const;
	bool is_readback_enabled() const;
	bool validate_hot_plateau_reference_debug();
	uint64_t get_cpu_image_bytes() const;
	uint64_t get_gpu_reserved_bytes() const;
	uint32_t get_page_table_generation() const;
	uint32_t getMaterialVTAllocationId( uint32_t nMaterialId ) const;
	bool loadPackageCold( const void *pPackageBytes, uint64_t nPackageBytes );
	bool stagePackageWarm( const void *pPackageBytes, uint64_t nPackageBytes );
	// Quiescing may span calls. Once GPU mutation starts, all upload slices and
	// any rollback complete in this render-thread call; consumers see one commit.
	bool servicePackageRepublishWarm( uint64_t nMaxBytes, uint32_t nMaxWorkUnits );
	void abortPackageRepublishWarm();
	bool isPackageRepublishPendingWarm() const { return m_bWarmPackagePending; }
	bool isPackageResourceRebuildRequiredWarm() const { return m_bWarmResourceRebuildRequired; }
	bool isPackageRollbackReadyWarm() const { return m_bWarmRollbackReady; }
	uint64_t getPackagePublicationGeneration() const { return m_nPackagePublicationGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ); }
	bool clearPackageCold( void );
	void set_startup_generation( uint32_t nGeneration );
	void cancel_startup_cold( uint32_t nGeneration );
	EA_FORCE_INLINE t3dStartupDomainSnapshot_t get_startup_snapshot() const {
		return t3dStartupDomainSnapshot( m_startupState );
	}
	void mark_startup_failed( uint32_t nGeneration );
	godot::Dictionary get_startup_diagnostics() const;
	bool findMaterialAllocationCold( uint64_t nStableKey, uint32_t &rAllocationId ) const;
	bool snapshotMaterialReadStateCold( t3dVTMaterialReadSnapshot_t &rSnapshot ) const;
	bool snapshotTerrainMegaVTReadStateCold( uint32_t *pAllocationIds, uint32_t *pCookedGenerations, uint8_t *pCookedValid, uint32_t nCount ) const;
	uint32_t getActiveAllocationCount() const;
	uint32_t getActiveVirtualPageCount() const;
	uint32_t getLoadedPackageAllocationCountCold() const;
	uint32_t getLoadedPackageVirtualPageCountCold() const;
	uint32_t getLoadedPackageTailAtlasHeightCold() const;
	uint32_t getDatabaseGeneration() const;
	uint32_t getPackageGeneration() const;
	uint64_t getPackageContentSignature() const;
	uint32_t getDatabaseKind() const;
	bool supportsSparseV3() const;
	uint32_t getTerrainMegaVTAllocationBase() const;
	uint32_t getTerrainMegaVTAllocationId( uint32_t nRegionMapIndex ) const;
	bool getTerrainMegaVTCookedGenerationCold( uint32_t nRegionMapIndex, uint32_t &rGeneration ) const;
	const t3dVTDatabaseView_t *get_database() const;
	const Terrain3DVTResourceImage *get_resource_image() const;
	bool set_quality_profile( uint32_t nProfile );
	uint32_t get_requested_quality_profile() const;
	uint32_t get_package_fully_supported_profile_mask() const;
	uint32_t get_quality_fallback_allocation_count() const;
	uint32_t get_quality_generation() const;
	uint32_t get_allocations_by_highest_profile( uint32_t nProfile ) const;
	uint32_t get_allocations_by_resolved_profile( uint32_t nProfile ) const;
	uint32_t getPhysicalFormatFamilyCountV3() const;
	bool getPhysicalFormatFamilyBindingV3( uint32_t nFamily, godot::RID &rRid, uint32_t &rLayerCount ) const;
	t3dVTResidencyState_t &get_residency();
	const t3dVTResidencyState_t &get_residency() const;
	t3dVTStreamingState_t &get_streaming();
	const t3dVTStreamingState_t &get_streaming() const;
	Terrain3DVTGpu &get_gpu();
	const Terrain3DVTGpu &get_gpu() const;
	const t3dVirtualTextureStats_t &get_stats() const;
	t3dVTFinalTelemetry_t get_final_telemetry_reference_debug() const;
	void service_fragment_feedback_hot( uint32_t nFrameId );
	void service_streaming_hot( uint32_t nFrameId );
	bool shouldRefreshMaterialDemandHot( uint32_t nLane, const t3dRdForwardPassContext_s &rContext, uint64_t nSourceSignature );
	uint32_t stage_material_footprint_hot( uint32_t nAllocationId,
	        uint32_t nFrameId,
	        const float pUvMin[2],
	        const float pUvMax[2],
	        uint16_t nWantedMip,
	        uint32_t nMaxRequests,
	        uint16_t nPriority,
	        t3dVTRequestProducer_t eProducer,
	        uint32_t nFootprintWindow = UINT32_MAX );
	bool flush_staged_material_requests_hot(
	        uint32_t nFrameId, uint32_t &rTerrainAccepted, uint32_t &rInstanceAccepted );

	bool claim_stream_job_owner_hot( uint32_t &rJobIndex );
	bool cancel_stream_job_owner_hot( uint32_t pJobIndex );
	bool execute_stream_job_async( uint32_t pJobIndex, t3dVTStreamReadDecodeFn_t pReadDecode, void *pUser );
	uint64_t service_uploads_hot( uint32_t pFrameId );
	bool service_retired_slots_hot( uint32_t pFrameId );

private:
	void streamWorkerMain( void );
	void stopStreamWorkerCold( void );
	bool dispatchStreamJobHot( uint32_t nJob );
	bool open_package_candidate( const void *pBytes, uint64_t nBytes, t3dVTPackRuntimeStorage_t &rCandidate );
	bool validate_package_limits( const t3dVTPackRuntimeStorage_t &rCandidate ) const;
	bool publish_package_state_warm( const t3dVTDatabaseView_t &rDatabase, bool bNeutral,
	        uint64_t nMaxBytes, uint32_t nMaxWorkUnits, bool *pMutationStarted = NULL );
	bool allocate_cpu_image_cold();
	void release_cpu_image_cold();
	bool prepare_neutral_legacy_cpu_cold();
	bool initialize_runtime_state_cold( const t3dVTDatabaseView_t &rDatabase, uint32_t nDatabaseGeneration );
	bool prepare_runtime_package_cpu_cold();
	bool publish_neutral_legacy_warm();
	bool publish_runtime_package_warm();
	void release_runtime_pack_cold();
	uint32_t count_physical_banks() const;
	bool apply_quality_profile_state_v3( t3dVTAllocation_t *pAllocations, uint32_t nAllocationCount );

	Terrain3DVTResourceImage *m_pResources;
	Terrain3DMemoryZone *m_pRuntimeAllocatorZone;
	Terrain3DVTGpu m_Gpu;
	uint32_t m_nStreamingServiceFrame = UINT32_MAX;
	uint64_t m_szDemandSignature[3]{}; // Terrain, visible instances, world candidates.
	uint32_t m_szDemandRefreshFrame[3]{ UINT32_MAX, UINT32_MAX, UINT32_MAX };
	Terrain3DThread m_streamWorker;
	Terrain3DMutex m_streamWorkerMutex;
	Terrain3DCondition m_streamWorkerCondition;
	uint32_t m_szStreamWorkerJobs[T3D_VT_MAX_STREAM_SCRATCH_LANES]{};
	uint32_t m_nStreamWorkerHead = 0u;
	uint32_t m_nStreamWorkerCount = 0u;
	bool m_bStreamWorkerStop = false;
	t3dWorldLimits_t m_Limits;
	t3dVTResidencyState_t m_Residency;
	t3dVTStreamingState_t m_Streaming;
	t3dVTStreamingLimits_t m_StreamingLimits;
	t3dVTResidencyPolicy_t m_ResidencyPolicy;
	t3dVTDatabaseView_t m_NeutralDatabase;
	t3dVTPackRuntimeStorage_t m_RuntimePack;
	mutable Terrain3DMutex m_packageMutex;
	t3dVTPackRuntimeStorage_t m_WarmCandidate{};
	t3dAtomicU64_t m_nPackagePublicationGeneration{ 0u };
	uint64_t m_nWarmPublicationAttemptCount = 0u;
	uint64_t m_nWarmPublicationSuccessCount = 0u;
	uint64_t m_nWarmPublicationFailureCount = 0u;
	uint64_t m_nWarmPublicationUsecLast = 0u;
	bool m_bWarmPackagePending = false;
	bool m_bWarmCandidateNeutral = false;
	bool m_bWarmResourceRebuildRequired = false;
	bool m_bWarmRollbackReady = true;
	bool m_bInitialPublicationStarted = false;
	t3dVTFileHeader_t m_NeutralHeader;
	t3dVTAllocationRecordV1_t m_NeutralAllocations[T3D_VT_CHANNEL_COUNT];
	t3dVTPageFileRecord_t m_NeutralPages[T3D_VT_CHANNEL_COUNT];
	t3dVTPayloadRecord_t m_NeutralPayloads[T3D_VT_CHANNEL_COUNT];
	uint8_t m_NeutralPayloadBytes[T3D_VT_CHANNEL_COUNT];
	void *m_pCpuImage;
	uint64_t m_nCpuImageBytes;
	t3dVTAllocation_t *m_pAllocations;
	t3dVTTailMetadata_t *m_pTailMetadata;
	t3dVTPageTableEntry_t *m_pPageTable;
	t3dVTPhysicalSlot_t *m_pPhysicalPageSets;
	t3dVTStreamJob_t *m_pStreamJobs;
	uint16_t *m_pStreamPageToJob;
	uint16_t *m_pStreamFreeJobs;
	uint16_t *m_pStreamQueuedJobs;
	uint16_t *m_pStreamJobQueuePosition;
	uint16_t *m_pStreamReadyJobs;
	uint16_t *m_pStreamMergedWantedMip;
	uint16_t *m_pStreamMergedPriority;
	uint32_t *m_pFreePhysicalSlots;
	t3dVTRetiredPageSet_t *m_pRetiredPageSets;
	uint32_t *m_pLastEvictedFrame;
	t3dVTStagedRequest_t *m_pStagedRequests;
	uint32_t *m_pStagedByPage;
	uint32_t m_nStagedRequestCount;
	uint32_t m_nStagedRequestFrame;
	uint32_t m_nFrameRequestBudget = 1u;
	uint32_t m_nFragmentRequestFrame = UINT32_MAX;
	uint32_t m_nFragmentRequestsUsed = 0u;
	uint8_t *m_pStreamScratch;
	uint32_t *m_pScratchLaneOwner;
	Terrain3DAtomic<uint32_t> m_ScratchLaneCompletion[T3D_VT_MAX_STREAM_SCRATCH_LANES];
	t3dVirtualTextureStats_t m_Stats;
	t3dVTQualityRuntimeStats_t m_QualityStats;
	uint32_t m_nRequestedQualityProfile;
	uint32_t m_nQualityGeneration;
	uint64_t m_nGpuHelperCreateEpoch;
	uint64_t m_nTelemetryStartUsec;
	uint64_t m_nTelemetryLastUploadedPageSets;
	uint64_t m_nTelemetryLastEvictions;
	uint64_t m_nTelemetryUploadPageSetsFrame;
	uint64_t m_nTelemetryUploadBytesFrame;
	uint64_t m_nTelemetryEvictionsFrame;
	t3dAtomicU32_t m_nStartupGeneration{ 0u };
	t3dStartupDomainState_t m_startupState;
	uint64_t m_nStartupPackagePrepareCount;
	uint64_t m_nStartupPackagePrepareUsecLast;
	uint64_t m_nStartupCpuImagePrepareCount;
	uint64_t m_nStartupCpuImagePrepareUsecLast;
	uint64_t m_nStartupGpuInitializeCount;
	uint64_t m_nStartupGpuInitializeUsecLast;
	uint64_t m_nStartupGpuStoragePrepareCount;
	uint64_t m_nStartupGpuStoragePrepareUsecLast;
	uint64_t m_nStartupGpuBindCount;
	uint64_t m_nStartupGpuBindUsecLast;
	uint64_t m_nStartupInitialPublicationCount;
	uint64_t m_nStartupInitialPublicationUsecLast;
	uint64_t m_nResolverDepth0Samples;
	uint64_t m_nResolverDepth1Samples;
	uint64_t m_nResolverDepth2To3Samples;
	uint64_t m_nResolverDepth4PlusSamples;
	uint64_t m_nResolverTailSamples;
	uint32_t m_nResolverTelemetrySampleFrame;
	uint32_t m_nResolverTelemetrySamplesThisFrame;
	uint32_t m_nUploadCadenceStableFrames;
	uint64_t m_nUploadCadenceLastPageMisses;
	uint64_t m_nUploadCadenceLastEvictions;
	uint64_t m_nUploadCadenceLastThrash;
	bool m_bStreamingLimitsConfigured;
	bool m_bResidencyPolicyConfigured;
	bool m_bRuntimePackLoaded;
	bool m_bCpuPrepared;
	bool m_bGpuStoragePrepared;
	bool m_bExecutableBound;
	bool m_bInitialPublished;
	bool m_bInitialized;
	bool m_bHotSealed;
};

#endif // TERRAIN3D_VIRTUAL_TEXTURE_H
