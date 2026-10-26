// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_STREAMING_H
#define TERRAIN3D_VT_STREAMING_H

#include "rendering/rd/vt/terrain_3d_vt_residency.h"
#include "core/terrain_3d_sync.h"

#include <stdint.h>

static const uint32_t T3D_RW_MAX_STREAM_SCRATCH_LANES = 8u;
static const uint32_t T3D_VT_PRIORITY_BUCKET_COUNT = 6u;

enum T3DVTPriorityBucket : uint16_t {
	T3D_VT_PRIORITY_CRITICAL = 0u,
	T3D_VT_PRIORITY_NEAR = 1u,
	T3D_VT_PRIORITY_NORMAL = 2u,
	T3D_VT_PRIORITY_PREFETCH = 3u,
	T3D_VT_PRIORITY_TRANSITION = 4u,
	T3D_VT_PRIORITY_BACKGROUND = 5u
};

// WG-E owns the raw 16-bit request score. WG-D freezes only the CPU scheduler
// mapping: higher raw score is more urgent, while lower bucket index is better.
static const uint16_t T3D_VT_PRIORITY_CRITICAL_MIN = 8192u;
static const uint16_t T3D_VT_PRIORITY_NEAR_MIN = 4096u;
static const uint16_t T3D_VT_PRIORITY_NORMAL_MIN = 2048u;
static const uint16_t T3D_VT_PRIORITY_PREFETCH_MIN = 1024u;
static const uint16_t T3D_VT_PRIORITY_TRANSITION_MIN = 512u;

enum T3DVTAdmissionStatus : uint32_t {
	T3D_VT_ADMISSION_ACCEPTED_NEW = 0u,
	T3D_VT_ADMISSION_MERGED_EXISTING = 1u,
	T3D_VT_ADMISSION_ALREADY_RESIDENT = 2u,
	T3D_VT_ADMISSION_DEFERRED_FRAME_BUDGET = 3u,
	T3D_VT_ADMISSION_DROPPED_QUEUE_FULL = 4u,
	T3D_VT_ADMISSION_INVALID_PAGE = 5u,
	T3D_VT_ADMISSION_INVALID_PAYLOAD = 6u,
	T3D_VT_ADMISSION_STATUS_COUNT = 7u
};

enum T3DVTReadbackConsumeStatus : uint32_t {
	T3D_VT_READBACK_CONSUMED = 0u,
	T3D_VT_READBACK_STALE_DATABASE = 1u,
	T3D_VT_READBACK_INVALID = 2u
};

struct T3DVTReadbackSlotHeader {
	uint32_t nFrameId;
	uint32_t nDatabaseGeneration;
	uint32_t nRequestCapacity;
	uint32_t nReadbackBytes;
};

enum T3DVTStreamJobState : uint16_t {
	T3D_VT_STREAM_FREE = 0,
	T3D_VT_STREAM_QUEUED,
	T3D_VT_STREAM_WORKING,
	T3D_VT_STREAM_READY,
	T3D_VT_STREAM_FAILED,
	T3D_VT_STREAM_STAGED
};

enum T3DVTStreamResultCode : uint32_t {
	T3D_VT_STREAM_RESULT_OK = 0u,
	T3D_VT_STREAM_RESULT_READ_FAILED = 1u,
	T3D_VT_STREAM_RESULT_INVALID_PAYLOAD = 2u,
	T3D_VT_STREAM_RESULT_STALE_DATABASE = 3u,
	T3D_VT_STREAM_RESULT_STALE_PAGE = 4u,
	T3D_VT_STREAM_RESULT_STALE_ALLOCATION = 5u,
	T3D_VT_STREAM_RESULT_OUTPUT_SIZE_MISMATCH = 6u
};

struct T3DVTStreamJob {
	uint64_t nReadOffset;
	uint32_t nReadBytes;
	uint32_t nExpectedDecodedBytes;
	uint32_t nVirtualPage;
	uint32_t nAllocationId;
	uint32_t nPayloadId;
	uint32_t nContentId;
	uint32_t nPageGeneration;
	uint32_t nAllocationGeneration;
	uint32_t nDatabaseGeneration;
	uint32_t nRequestFrame;
	uint32_t nDecodedBytes;
	uint32_t nScratchLane;
	uint16_t nWantedMip;
	uint16_t nPriority;
	uint16_t nPriorityBucket;
	uint16_t nChannel;
	uint16_t nState;
	uint16_t nReserved;
	uint32_t nResultCode;
};

enum T3DVTStreamCompletionState : uint32_t {
	T3D_VT_STREAM_COMPLETION_IDLE = 0u,
	T3D_VT_STREAM_COMPLETION_READY = 1u,
	T3D_VT_STREAM_COMPLETION_FAILED = 2u
};

struct T3DVTStreamingBuffers {
	T3DVTStreamJob *pJobs;
	uint32_t nJobCapacity;
	uint8_t *pScratchBytes;
	uint32_t nScratchLaneCount;
	uint32_t nScratchLaneBytes;
	uint32_t *pScratchLaneOwner;
	t3dAtomic_t<uint32_t> *pScratchLaneCompletion;
	uint16_t *pPageToJob;
	uint32_t nPageToJobCapacity;
	uint16_t *pFreeJobs;
	// P10 names are retained for ABI/source stability. Under P23 these arrays
	// are intrusive next/previous links for the six fixed scheduler buckets.
	uint16_t *pQueuedJobs;
	uint16_t *pJobQueuePosition;
	uint16_t *pReadyJobs;
	uint16_t *pMergedWantedMip;
	uint16_t *pMergedPriority;
};

struct T3DVTStreamingLimits {
	uint64_t nMaxUploadBytesPerFrame;
	uint32_t nMaxPageSetsPerFrame;
	uint32_t nMaxInFlightIoJobs;
	uint32_t nMaxAdmittedRequestsPerFrame;
};

struct T3DVTStreamingStats {
	uint64_t nQueuedCount;
	uint64_t nClaimedCount;
	uint64_t nCompletedCount;
	uint64_t nUploadedPageCount;
	uint64_t nUploadedBytes;
	uint64_t nStaleCompletionCount;
	uint64_t nStaleReadbackCount;
	uint64_t nInvalidReadbackCount;
	uint64_t nDroppedRequestCount;
	uint64_t nDuplicateMergeCount;
	uint64_t nResidentRequestCount;
	uint64_t nPriorityPromotionCount;
	uint64_t nReplacedQueuedJobCount;
	uint64_t nBudgetDeferredCount;
	uint64_t nAdmissionBudgetDeferredCount;
	uint64_t nIoBudgetDeferredCount;
	uint64_t nPageSetBudgetDeferredCount;
	uint64_t nUploadByteBudgetDeferredCount;
	uint64_t nOversizedReadCount;
	uint64_t nDecodeFailureCount;
	uint64_t nPageHitCount;
	uint64_t nPageMissCount;
	uint64_t nIoBytesTotal;
	uint64_t nAdmissionStatusCount[T3D_VT_ADMISSION_STATUS_COUNT];
	uint64_t nPriorityBucketClaimCount[T3D_VT_PRIORITY_BUCKET_COUNT];
	uint32_t nPriorityBucketHighWater[T3D_VT_PRIORITY_BUCKET_COUNT];
	uint32_t nQueueHighWater;
	uint32_t nActiveJobCount;
	uint32_t nInFlightIoHighWater;
};

struct T3DVTStreamingState {
	T3DVTResidencyState *pResidency;
	T3DVTStreamingBuffers Buffers;
	T3DVTStreamingLimits Limits;
	T3DVTStreamingStats Stats;
	uint16_t nPriorityBucketHead[T3D_VT_PRIORITY_BUCKET_COUNT];
	uint16_t nPriorityBucketTail[T3D_VT_PRIORITY_BUCKET_COUNT];
	uint32_t nPriorityBucketCount[T3D_VT_PRIORITY_BUCKET_COUNT];
	uint32_t nFreeJobCount;
	uint32_t nQueuedJobCount;
	uint32_t nReadyJobCount;
	uint32_t nInFlightIoJobCount;
	uint32_t nAdmissionFrame;
	uint32_t nAdmittedRequestCountThisFrame;
	uint32_t nUploadFrame;
	uint32_t nUploadedPageSetCountThisFrame;
	uint64_t nUploadedBytesThisFrame;
	bool bInitialized;
};

typedef bool ( *T3DVTStreamReadDecodeFn )( const T3DVTDatabaseView &pDatabase,
        const T3DVTStreamJob &pJob,
        uint8_t *pScratch,
        uint32_t pScratchCapacity,
        uint32_t &rDecodedBytes,
        void *pUser );

typedef bool ( *T3DVTUploadCommitFn )( T3DVTResidencyState &rResidency,
        const T3DVTUploadReservation &pReservation,
        const uint8_t *pDecodedBytes,
        uint32_t pDecodedByteCount,
        uint32_t pFrameId,
        void *pUser );

typedef bool ( *T3DVTUploadBatchBeginFn )( void *pUser );
typedef bool ( *T3DVTUploadBatchEndFn )( T3DVTResidencyState &rResidency, uint32_t pFrameId, void *pUser );

bool t3d_vt_streaming_initialize_cold( T3DVTResidencyState &rResidency,
        const T3DVTStreamingBuffers &pBuffers,
        const T3DVTStreamingLimits &pLimits,
        T3DVTStreamingState &rState );
// P16 compatibility adapter. New runtime owners should provide all budgets explicitly.
bool t3d_vt_streaming_initialize_cold( T3DVTResidencyState &rResidency,
        const T3DVTStreamingBuffers &pBuffers,
        uint64_t pUploadBudgetBytesPerFrame,
        T3DVTStreamingState &rState );
uint16_t t3d_vt_scheduler_priority_bucket_reference_debug( uint16_t pPriority );
T3DVTAdmissionStatus t3d_vt_streaming_admit_request_hot( T3DVTStreamingState &rState, const T3DVTRequest &pRequest, uint32_t pFrameId );
uint32_t t3d_vt_streaming_ingest_requests_hot(
        T3DVTStreamingState &rState, const T3DVTRequest *pRequests, uint32_t pRequestCount, uint32_t pFrameId );
T3DVTReadbackConsumeStatus t3d_vt_streaming_ingest_readback_owner_hot( T3DVTStreamingState &rState,
        const T3DVTReadbackSlotHeader &pHeader,
        const uint8_t *pReadbackBytes,
        uint32_t pReadbackByteCount,
        T3DVTGpuCounters &rCounters,
        uint32_t &rAcceptedRequestCount );
// P20 compatibility adapter. P23 owners should use the explicit slot header overload.
uint32_t t3d_vt_streaming_ingest_readback_owner_hot( T3DVTStreamingState &rState,
        const uint8_t *pReadbackBytes,
        uint32_t pReadbackByteCount,
        uint32_t pRequestCapacity,
        uint32_t pFrameId,
        T3DVTGpuCounters &rCounters );
uint32_t t3d_vt_streaming_invalidate_queued_demand_hot( T3DVTStreamingState &rState );
bool t3d_vt_streaming_claim_job_owner_hot( T3DVTStreamingState &rState, uint32_t &rJobIndex );
bool t3d_vt_streaming_cancel_claim_owner_hot( T3DVTStreamingState &rState, uint32_t pJobIndex );
bool t3d_vt_streaming_execute_job_async(
        T3DVTStreamingState &rState, uint32_t pJobIndex, T3DVTStreamReadDecodeFn pReadDecode, void *pUser );
uint64_t t3d_vt_streaming_service_uploads_hot(
        T3DVTStreamingState &rState, uint32_t pFrameId, T3DVTUploadCommitFn pUploadCommit, void *pUser );
uint64_t t3d_vt_streaming_service_uploads_batched_hot( T3DVTStreamingState &rState,
        uint32_t pFrameId,
        T3DVTUploadBatchBeginFn pBatchBegin,
        T3DVTUploadCommitFn pUploadStage,
        T3DVTUploadBatchEndFn pBatchEnd,
        void *pUser );
bool t3d_vt_decode_readback_reference_debug( const uint8_t *pReadbackBytes,
        uint32_t pReadbackByteCount,
        uint32_t pRequestCapacity,
        T3DVTGpuCounters &rCounters,
        const T3DVTRequest *&rRequests,
        uint32_t &rRequestCount );

#endif // TERRAIN3D_VT_STREAMING_H
