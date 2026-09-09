// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_STREAMING_H
#define TERRAIN3D_RENDER_WORLD_STREAMING_H

#include "rendering/rd/render_world/terrain_3d_render_world_vt.h"
#include "core/terrain_3d_sync.h"

#include <stdint.h>

static const uint32_t T3D_RW_MAX_STREAM_SCRATCH_LANES = 8u;

enum T3DVTStreamJobState : uint16_t {
	T3D_VT_STREAM_FREE = 0,
	T3D_VT_STREAM_QUEUED,
	T3D_VT_STREAM_WORKING,
	T3D_VT_STREAM_READY,
	T3D_VT_STREAM_FAILED
};

struct T3DVTStreamJob {
	uint32_t nVirtualPage;
	uint32_t nPayloadId;
	uint32_t nPageGeneration;
	uint32_t nDatabaseGeneration;
	uint32_t nRequestFrame;
	uint32_t nDecodedBytes;
	uint32_t nScratchLane;
	uint16_t nWantedMip;
	uint16_t nPriority;
	uint16_t nChannel;
	uint16_t nState;
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
	uint16_t *pQueuedJobs;
	uint16_t *pJobQueuePosition;
	uint16_t *pReadyJobs;
};

struct T3DVTStreamingStats {
	uint64_t nQueuedCount;
	uint64_t nClaimedCount;
	uint64_t nCompletedCount;
	uint64_t nUploadedPageCount;
	uint64_t nUploadedBytes;
	uint64_t nStaleCompletionCount;
	uint64_t nDroppedRequestCount;
	uint64_t nReplacedQueuedJobCount;
	uint64_t nBudgetDeferredCount;
	uint64_t nDecodeFailureCount;
	uint32_t nQueueHighWater;
	uint32_t nActiveJobCount;
};

struct T3DVTStreamingState {
	T3DVTResidencyState *pResidency;
	T3DVTStreamingBuffers Buffers;
	T3DVTStreamingStats Stats;
	uint64_t nUploadBudgetBytesPerFrame;
	uint32_t nFreeJobCount;
	uint32_t nQueuedJobCount;
	uint32_t nReadyJobCount;
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

bool t3d_render_world_vt_streaming_initialize_cold( T3DVTResidencyState &rResidency,
        const T3DVTStreamingBuffers &pBuffers,
        uint64_t pUploadBudgetBytesPerFrame,
        T3DVTStreamingState &rState );
uint32_t t3d_render_world_vt_streaming_ingest_requests_hot(
        T3DVTStreamingState &rState, const T3DVTRequest *pRequests, uint32_t pRequestCount, uint32_t pFrameId );
uint32_t t3d_render_world_vt_streaming_ingest_readback_owner_hot( T3DVTStreamingState &rState,
        const uint8_t *pReadbackBytes,
        uint32_t pReadbackByteCount,
        uint32_t pRequestCapacity,
        uint32_t pFrameId,
        T3DVTGpuCounters &rCounters );
bool t3d_render_world_vt_streaming_claim_job_owner_hot( T3DVTStreamingState &rState, uint32_t &rJobIndex );
bool t3d_render_world_vt_streaming_cancel_claim_owner_hot( T3DVTStreamingState &rState, uint32_t pJobIndex );
bool t3d_render_world_vt_streaming_execute_job_async(
        T3DVTStreamingState &rState, uint32_t pJobIndex, T3DVTStreamReadDecodeFn pReadDecode, void *pUser );
uint64_t t3d_render_world_vt_streaming_service_uploads_hot(
        T3DVTStreamingState &rState, uint32_t pFrameId, T3DVTUploadCommitFn pUploadCommit, void *pUser );
bool t3d_render_world_vt_decode_readback_reference_debug( const uint8_t *pReadbackBytes,
        uint32_t pReadbackByteCount,
        uint32_t pRequestCapacity,
        T3DVTGpuCounters &rCounters,
        const T3DVTRequest *&rRequests,
        uint32_t &rRequestCount );

#endif // TERRAIN3D_RENDER_WORLD_STREAMING_H
