// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/vt/terrain_3d_vt_streaming.h"
#include "core/terrain_3d_sync.h"

#include <string.h>

static const uint16_t s_nInvalidJob16 = 0xffffu;

uint16_t t3d_vt_scheduler_priority_bucket_reference_debug( const uint16_t pPriority ) {
	if ( pPriority >= T3D_VT_PRIORITY_CRITICAL_MIN ) {
		return T3D_VT_PRIORITY_CRITICAL;
	}
	if ( pPriority >= T3D_VT_PRIORITY_NEAR_MIN ) {
		return T3D_VT_PRIORITY_NEAR;
	}
	if ( pPriority >= T3D_VT_PRIORITY_NORMAL_MIN ) {
		return T3D_VT_PRIORITY_NORMAL;
	}
	if ( pPriority >= T3D_VT_PRIORITY_PREFETCH_MIN ) {
		return T3D_VT_PRIORITY_PREFETCH;
	}
	if ( pPriority >= T3D_VT_PRIORITY_TRANSITION_MIN ) {
		return T3D_VT_PRIORITY_TRANSITION;
	}
	return T3D_VT_PRIORITY_BACKGROUND;
}

static uint32_t t3d_vt_stream_find_free_lane( const T3DVTStreamingState &pState ) {
	for ( uint32_t iLane = 0u; iLane < pState.Buffers.nScratchLaneCount; ++iLane ) {
		if ( pState.Buffers.pScratchLaneOwner[iLane] == T3D_RW_INVALID_ID ) {
			return iLane;
		}
	}
	return T3D_RW_INVALID_ID;
}

static bool t3d_vt_stream_queue_insert( T3DVTStreamingState &rState, const uint32_t pJobIndex ) {
	if ( pJobIndex >= rState.Buffers.nJobCapacity ) {
		return false;
	}
	T3DVTStreamJob &Job = rState.Buffers.pJobs[pJobIndex];
	const uint16_t nBucket = t3d_vt_scheduler_priority_bucket_reference_debug( Job.nPriority );
	if ( nBucket >= T3D_VT_PRIORITY_BUCKET_COUNT || rState.nPriorityBucketCount[nBucket] >= rState.Buffers.nJobCapacity ) {
		return false;
	}
	const uint16_t nTail = rState.nPriorityBucketTail[nBucket];
	Job.nPriorityBucket = nBucket;
	rState.Buffers.pQueuedJobs[pJobIndex] = s_nInvalidJob16;
	rState.Buffers.pJobQueuePosition[pJobIndex] = nTail;
	if ( nTail == s_nInvalidJob16 ) {
		rState.nPriorityBucketHead[nBucket] = ( uint16_t )pJobIndex;
	} else {
		rState.Buffers.pQueuedJobs[nTail] = ( uint16_t )pJobIndex;
	}
	rState.nPriorityBucketTail[nBucket] = ( uint16_t )pJobIndex;
	++rState.nPriorityBucketCount[nBucket];
	++rState.nQueuedJobCount;
	if ( rState.nPriorityBucketCount[nBucket] > rState.Stats.nPriorityBucketHighWater[nBucket] ) {
		rState.Stats.nPriorityBucketHighWater[nBucket] = rState.nPriorityBucketCount[nBucket];
	}
	return true;
}

static void t3d_vt_stream_queue_remove( T3DVTStreamingState &rState, const uint32_t pJobIndex ) {
	if ( pJobIndex >= rState.Buffers.nJobCapacity ) {
		return;
	}
	T3DVTStreamJob &Job = rState.Buffers.pJobs[pJobIndex];
	const uint16_t nBucket = Job.nPriorityBucket;
	if ( nBucket >= T3D_VT_PRIORITY_BUCKET_COUNT || rState.nPriorityBucketCount[nBucket] == 0u ) {
		return;
	}
	const uint16_t nNext = rState.Buffers.pQueuedJobs[pJobIndex];
	const uint16_t nPrevious = rState.Buffers.pJobQueuePosition[pJobIndex];
	if ( nPrevious == s_nInvalidJob16 ) {
		if ( rState.nPriorityBucketHead[nBucket] != pJobIndex ) {
			return;
		}
		rState.nPriorityBucketHead[nBucket] = nNext;
	} else {
		rState.Buffers.pQueuedJobs[nPrevious] = nNext;
	}
	if ( nNext == s_nInvalidJob16 ) {
		if ( rState.nPriorityBucketTail[nBucket] != pJobIndex ) {
			return;
		}
		rState.nPriorityBucketTail[nBucket] = nPrevious;
	} else {
		rState.Buffers.pJobQueuePosition[nNext] = nPrevious;
	}
	rState.Buffers.pQueuedJobs[pJobIndex] = s_nInvalidJob16;
	rState.Buffers.pJobQueuePosition[pJobIndex] = s_nInvalidJob16;
	--rState.nPriorityBucketCount[nBucket];
	--rState.nQueuedJobCount;
}

static uint32_t t3d_vt_stream_find_existing( const T3DVTStreamingState &pState, const uint32_t pVirtualPage ) {
	if ( pVirtualPage >= pState.Buffers.nPageToJobCapacity ) {
		return T3D_RW_INVALID_ID;
	}
	const uint16_t nJob = pState.Buffers.pPageToJob[pVirtualPage];
	return nJob == s_nInvalidJob16 ? T3D_RW_INVALID_ID : ( uint32_t )nJob;
}

static uint32_t t3d_vt_stream_pop_free_job( T3DVTStreamingState &rState ) {
	if ( rState.nFreeJobCount == 0u ) {
		return T3D_RW_INVALID_ID;
	}
	return rState.Buffers.pFreeJobs[--rState.nFreeJobCount];
}

static uint32_t t3d_vt_stream_find_replaceable( const T3DVTStreamingState &pState, const uint16_t pPriority ) {
	if ( pState.nQueuedJobCount == 0u ) {
		return T3D_RW_INVALID_ID;
	}
	const uint16_t nIncomingBucket = t3d_vt_scheduler_priority_bucket_reference_debug( pPriority );
	for ( int32_t nBucket = ( int32_t )T3D_VT_PRIORITY_BACKGROUND; nBucket > ( int32_t )nIncomingBucket; --nBucket ) {
		const uint16_t nJob = pState.nPriorityBucketTail[nBucket];
		if ( nJob != s_nInvalidJob16 ) {
			return nJob;
		}
	}
	return T3D_RW_INVALID_ID;
}

static void t3d_vt_stream_ready_remove( T3DVTStreamingState &rState, const uint32_t pReadyIndex ) {
	if ( pReadyIndex >= rState.nReadyJobCount ) {
		return;
	}
	if ( pReadyIndex + 1u < rState.nReadyJobCount ) {
		memmove( rState.Buffers.pReadyJobs + pReadyIndex,
		        rState.Buffers.pReadyJobs + pReadyIndex + 1u,
		        ( rState.nReadyJobCount - pReadyIndex - 1u ) * sizeof( uint16_t ) );
	}
	--rState.nReadyJobCount;
}

static void t3d_vt_stream_free_job( T3DVTStreamingState &rState, const uint32_t pJobIndex ) {
	if ( pJobIndex >= rState.Buffers.nJobCapacity ) {
		return;
	}
	T3DVTStreamJob &Job = rState.Buffers.pJobs[pJobIndex];
	if ( Job.nState == T3D_VT_STREAM_QUEUED ) {
		t3d_vt_stream_queue_remove( rState, pJobIndex );
	}
	if ( Job.nVirtualPage < rState.Buffers.nPageToJobCapacity && rState.Buffers.pPageToJob[Job.nVirtualPage] == pJobIndex ) {
		rState.Buffers.pPageToJob[Job.nVirtualPage] = s_nInvalidJob16;
	}
	if ( Job.nScratchLane != T3D_RW_INVALID_ID && Job.nScratchLane < rState.Buffers.nScratchLaneCount ) {
		rState.Buffers.pScratchLaneOwner[Job.nScratchLane] = T3D_RW_INVALID_ID;
	}
	memset( &Job, 0, sizeof( Job ) );
	Job.nScratchLane = T3D_RW_INVALID_ID;
	Job.nPriorityBucket = T3D_VT_PRIORITY_BACKGROUND;
	Job.nState = T3D_VT_STREAM_FREE;
	rState.Buffers.pQueuedJobs[pJobIndex] = s_nInvalidJob16;
	rState.Buffers.pJobQueuePosition[pJobIndex] = s_nInvalidJob16;
	rState.Buffers.pMergedWantedMip[pJobIndex] = 0xffffu;
	rState.Buffers.pMergedPriority[pJobIndex] = 0u;
	if ( rState.nFreeJobCount < rState.Buffers.nJobCapacity ) {
		rState.Buffers.pFreeJobs[rState.nFreeJobCount++] = ( uint16_t )pJobIndex;
	}
	if ( rState.Stats.nActiveJobCount != 0u ) {
		--rState.Stats.nActiveJobCount;
	}
}

static bool t3d_vt_stream_has_required_storage( const T3DVTResidencyState &rResidency, const T3DVTStreamingBuffers &pBuffers ) {
	return rResidency.bInitialized && rResidency.pDatabase != NULL && rResidency.pDatabase->pHeader != NULL && pBuffers.pJobs != NULL
	        && pBuffers.nJobCapacity != 0u && pBuffers.nJobCapacity <= 0xffffu && pBuffers.pScratchBytes != NULL
	        && pBuffers.pScratchLaneOwner != NULL && pBuffers.pScratchLaneCompletion != NULL && pBuffers.pPageToJob != NULL
	        && pBuffers.nPageToJobCapacity >= rResidency.pDatabase->pHeader->nPageCount && pBuffers.pFreeJobs != NULL
	        && pBuffers.pQueuedJobs != NULL && pBuffers.pJobQueuePosition != NULL && pBuffers.pReadyJobs != NULL
	        && pBuffers.pMergedWantedMip != NULL && pBuffers.pMergedPriority != NULL && pBuffers.nScratchLaneCount != 0u
	        && pBuffers.nScratchLaneCount <= T3D_RW_MAX_STREAM_SCRATCH_LANES && pBuffers.nScratchLaneBytes != 0u;
}

static bool t3d_vt_stream_has_valid_limits( const T3DVTStreamingBuffers &pBuffers, const T3DVTStreamingLimits &pLimits ) {
	return pLimits.nMaxUploadBytesPerFrame != 0u && pLimits.nMaxPageSetsPerFrame != 0u && pLimits.nMaxInFlightIoJobs != 0u
	        && pLimits.nMaxInFlightIoJobs <= pBuffers.nScratchLaneCount && pLimits.nMaxAdmittedRequestsPerFrame != 0u
	        && pLimits.nMaxAdmittedRequestsPerFrame <= pBuffers.nJobCapacity;
}

bool t3d_vt_streaming_initialize_cold( T3DVTResidencyState &rResidency,
        const T3DVTStreamingBuffers &pBuffers,
        const T3DVTStreamingLimits &pLimits,
        T3DVTStreamingState &rState ) {
	memset( &rState, 0, sizeof( rState ) );
	if ( !t3d_vt_stream_has_required_storage( rResidency, pBuffers ) ) {
		return false;
	}
	if ( !t3d_vt_stream_has_valid_limits( pBuffers, pLimits ) ) {
		return false;
	}
	rState.pResidency = &rResidency;
	rState.Buffers = pBuffers;
	rState.Limits = pLimits;
	rState.nAdmissionFrame = T3D_RW_INVALID_ID;
	rState.nUploadFrame = T3D_RW_INVALID_ID;
	for ( uint32_t nBucket = 0u; nBucket < T3D_VT_PRIORITY_BUCKET_COUNT; ++nBucket ) {
		rState.nPriorityBucketHead[nBucket] = s_nInvalidJob16;
		rState.nPriorityBucketTail[nBucket] = s_nInvalidJob16;
	}
	for ( uint32_t iPage = 0u; iPage < pBuffers.nPageToJobCapacity; ++iPage ) {
		pBuffers.pPageToJob[iPage] = s_nInvalidJob16;
	}
	for ( uint32_t iJob = 0u; iJob < pBuffers.nJobCapacity; ++iJob ) {
		memset( &pBuffers.pJobs[iJob], 0, sizeof( T3DVTStreamJob ) );
		pBuffers.pJobs[iJob].nScratchLane = T3D_RW_INVALID_ID;
		pBuffers.pJobs[iJob].nPriorityBucket = T3D_VT_PRIORITY_BACKGROUND;
		pBuffers.pQueuedJobs[iJob] = s_nInvalidJob16;
		pBuffers.pJobQueuePosition[iJob] = s_nInvalidJob16;
		pBuffers.pMergedWantedMip[iJob] = 0xffffu;
		pBuffers.pMergedPriority[iJob] = 0u;
		pBuffers.pFreeJobs[iJob] = ( uint16_t )( pBuffers.nJobCapacity - 1u - iJob );
	}
	rState.nFreeJobCount = pBuffers.nJobCapacity;
	for ( uint32_t iLane = 0u; iLane < pBuffers.nScratchLaneCount; ++iLane ) {
		pBuffers.pScratchLaneOwner[iLane] = T3D_RW_INVALID_ID;
		pBuffers.pScratchLaneCompletion[iLane].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
	}
	rState.bInitialized = true;
	return true;
}

bool t3d_vt_streaming_initialize_cold( T3DVTResidencyState &rResidency,
        const T3DVTStreamingBuffers &pBuffers,
        const uint64_t pUploadBudgetBytesPerFrame,
        T3DVTStreamingState &rState ) {
	T3DVTStreamingLimits Limits = {};
	Limits.nMaxUploadBytesPerFrame = pUploadBudgetBytesPerFrame;
	Limits.nMaxPageSetsPerFrame = pBuffers.nJobCapacity;
	Limits.nMaxInFlightIoJobs = pBuffers.nScratchLaneCount;
	Limits.nMaxAdmittedRequestsPerFrame = pBuffers.nJobCapacity;
	return t3d_vt_streaming_initialize_cold( rResidency, pBuffers, Limits, rState );
}

static void t3d_vt_stream_begin_admission_frame( T3DVTStreamingState &rState, const uint32_t pFrameId ) {
	if ( rState.nAdmissionFrame != pFrameId ) {
		rState.nAdmissionFrame = pFrameId;
		rState.nAdmittedRequestCountThisFrame = 0u;
	}
}

static bool t3d_vt_stream_prepare_job_identity(
        T3DVTStreamingState &rState, const T3DVTRequest &pRequest, const uint32_t pFrameId, T3DVTStreamJob &rJob ) {
	const T3DVTDatabaseView *const pDatabase = rState.pResidency->pDatabase;
	if ( pDatabase == NULL || pDatabase->pHeader == NULL || pDatabase->pPages == NULL || pDatabase->pPayloads == NULL
	        || pRequest.nVirtualPage >= pDatabase->pHeader->nPageCount ) {
		return false;
	}
	const T3DVTPageFileRecord &Page = pDatabase->pPages[pRequest.nVirtualPage];
	if ( Page.nPayloadId >= pDatabase->pHeader->nPayloadCount || Page.nAllocationId >= pDatabase->pHeader->nAllocationCount ) {
		return false;
	}
	const T3DVTPayloadRecord &Payload = pDatabase->pPayloads[Page.nPayloadId];
	if ( Payload.nCompressedBytes == 0u || Payload.nDecodedBytes == 0u || Payload.nCompressedBytes > rState.Buffers.nScratchLaneBytes
	        || Payload.nDecodedBytes > rState.Buffers.nScratchLaneBytes || Payload.nOffset > pDatabase->pHeader->nPayloadDataBytes
	        || Payload.nCompressedBytes > pDatabase->pHeader->nPayloadDataBytes - Payload.nOffset ) {
		++rState.Stats.nOversizedReadCount;
		return false;
	}
	const T3DVTPageTableEntry &Entry = rState.pResidency->Buffers.pPageTable[pRequest.nVirtualPage];
	memset( &rJob, 0, sizeof( rJob ) );
	rJob.nReadOffset = Payload.nOffset;
	rJob.nReadBytes = Payload.nCompressedBytes;
	rJob.nExpectedDecodedBytes = Payload.nDecodedBytes;
	rJob.nVirtualPage = pRequest.nVirtualPage;
	rJob.nAllocationId = Page.nAllocationId;
	rJob.nPayloadId = Page.nPayloadId;
		rJob.nContentId = Page.nPayloadId;
		rJob.nPageGeneration = Entry.nGeneration;
		rJob.nAllocationGeneration = Page.nGeneration;
	rJob.nDatabaseGeneration = rState.pResidency->nDatabaseGeneration;
	rJob.nRequestFrame = pFrameId;
	rJob.nScratchLane = T3D_RW_INVALID_ID;
	rJob.nWantedMip = pRequest.nWantedMip;
	rJob.nPriority = pRequest.nPriority;
	rJob.nPriorityBucket = t3d_vt_scheduler_priority_bucket_reference_debug( pRequest.nPriority );
	rJob.nChannel = Page.nChannel;
	rJob.nState = T3D_VT_STREAM_QUEUED;
	rJob.nResultCode = T3D_VT_STREAM_RESULT_OK;
	return true;
}

static void t3d_vt_stream_record_admission_status( T3DVTStreamingState &rState, const T3DVTAdmissionStatus pStatus ) {
	if ( pStatus < T3D_VT_ADMISSION_STATUS_COUNT ) {
		++rState.Stats.nAdmissionStatusCount[pStatus];
	}
}

T3DVTAdmissionStatus t3d_vt_streaming_admit_request_hot(
        T3DVTStreamingState &rState, const T3DVTRequest &pRequest, const uint32_t pFrameId ) {
	if ( !rState.bInitialized || rState.pResidency == NULL || rState.pResidency->pDatabase == NULL
	        || rState.pResidency->pDatabase->pHeader == NULL
	        || pRequest.nVirtualPage >= rState.pResidency->pDatabase->pHeader->nPageCount ) {
		++rState.Stats.nDroppedRequestCount;
		t3d_vt_stream_record_admission_status( rState, T3D_VT_ADMISSION_INVALID_PAGE );
		return T3D_VT_ADMISSION_INVALID_PAGE;
	}
	t3d_vt_stream_begin_admission_frame( rState, pFrameId );
	const T3DVTPageTableEntry &Entry = rState.pResidency->Buffers.pPageTable[pRequest.nVirtualPage];
	if ( ( Entry.nFlags & T3D_VT_PAGE_RESIDENT ) != 0u ) {
		t3d_vt_touch_page_hot( *rState.pResidency, pRequest.nVirtualPage, pFrameId );
		++rState.Stats.nPageHitCount;
		++rState.Stats.nResidentRequestCount;
		t3d_vt_stream_record_admission_status( rState, T3D_VT_ADMISSION_ALREADY_RESIDENT );
		return T3D_VT_ADMISSION_ALREADY_RESIDENT;
	}
	++rState.Stats.nPageMissCount;

	uint32_t nJob = t3d_vt_stream_find_existing( rState, pRequest.nVirtualPage );
	if ( nJob != T3D_RW_INVALID_ID ) {
		T3DVTStreamJob &Job = rState.Buffers.pJobs[nJob];
		const uint16_t nOldPriority = rState.Buffers.pMergedPriority[nJob];
		const uint16_t nOldWantedMip = rState.Buffers.pMergedWantedMip[nJob];
		const uint16_t nMergedPriority = pRequest.nPriority > nOldPriority ? pRequest.nPriority : nOldPriority;
		const uint16_t nMergedWantedMip = pRequest.nWantedMip < nOldWantedMip ? pRequest.nWantedMip : nOldWantedMip;
		rState.Buffers.pMergedPriority[nJob] = nMergedPriority;
		rState.Buffers.pMergedWantedMip[nJob] = nMergedWantedMip;
		if ( Job.nState == T3D_VT_STREAM_QUEUED && ( nMergedPriority != Job.nPriority || nMergedWantedMip != Job.nWantedMip ) ) {
			const uint16_t nOldBucket = Job.nPriorityBucket;
			const uint16_t nNewBucket = t3d_vt_scheduler_priority_bucket_reference_debug( nMergedPriority );
			if ( nNewBucket != nOldBucket ) {
				t3d_vt_stream_queue_remove( rState, nJob );
			}
			Job.nPriority = nMergedPriority;
			Job.nWantedMip = nMergedWantedMip;
			if ( nNewBucket != nOldBucket && !t3d_vt_stream_queue_insert( rState, nJob ) ) {
				t3d_vt_stream_free_job( rState, nJob );
				++rState.Stats.nDroppedRequestCount;
				t3d_vt_stream_record_admission_status( rState, T3D_VT_ADMISSION_DROPPED_QUEUE_FULL );
				return T3D_VT_ADMISSION_DROPPED_QUEUE_FULL;
			}
			if ( nNewBucket < nOldBucket ) {
				++rState.Stats.nPriorityPromotionCount;
			}
		}
		++rState.Stats.nDuplicateMergeCount;
		t3d_vt_stream_record_admission_status( rState, T3D_VT_ADMISSION_MERGED_EXISTING );
		return T3D_VT_ADMISSION_MERGED_EXISTING;
	}

	if ( rState.nAdmittedRequestCountThisFrame >= rState.Limits.nMaxAdmittedRequestsPerFrame ) {
		++rState.Stats.nAdmissionBudgetDeferredCount;
		++rState.Stats.nBudgetDeferredCount;
		t3d_vt_stream_record_admission_status( rState, T3D_VT_ADMISSION_DEFERRED_FRAME_BUDGET );
		return T3D_VT_ADMISSION_DEFERRED_FRAME_BUDGET;
	}
	nJob = t3d_vt_stream_pop_free_job( rState );
	if ( nJob == T3D_RW_INVALID_ID ) {
		nJob = t3d_vt_stream_find_replaceable( rState, pRequest.nPriority );
		if ( nJob == T3D_RW_INVALID_ID ) {
			++rState.Stats.nDroppedRequestCount;
			t3d_vt_stream_record_admission_status( rState, T3D_VT_ADMISSION_DROPPED_QUEUE_FULL );
			return T3D_VT_ADMISSION_DROPPED_QUEUE_FULL;
		}
		t3d_vt_stream_free_job( rState, nJob );
		nJob = t3d_vt_stream_pop_free_job( rState );
		++rState.Stats.nReplacedQueuedJobCount;
	}
	T3DVTStreamJob &Job = rState.Buffers.pJobs[nJob];
	if ( !t3d_vt_stream_prepare_job_identity( rState, pRequest, pFrameId, Job ) ) {
		memset( &Job, 0, sizeof( Job ) );
		Job.nScratchLane = T3D_RW_INVALID_ID;
		Job.nPriorityBucket = T3D_VT_PRIORITY_BACKGROUND;
		Job.nState = T3D_VT_STREAM_FREE;
		rState.Buffers.pFreeJobs[rState.nFreeJobCount++] = ( uint16_t )nJob;
		++rState.Stats.nDroppedRequestCount;
		t3d_vt_stream_record_admission_status( rState, T3D_VT_ADMISSION_INVALID_PAYLOAD );
		return T3D_VT_ADMISSION_INVALID_PAYLOAD;
	}
	rState.Buffers.pMergedWantedMip[nJob] = pRequest.nWantedMip;
	rState.Buffers.pMergedPriority[nJob] = pRequest.nPriority;
	++rState.Stats.nActiveJobCount;
	if ( rState.Stats.nActiveJobCount > rState.Stats.nQueueHighWater ) {
		rState.Stats.nQueueHighWater = rState.Stats.nActiveJobCount;
	}
	rState.Buffers.pPageToJob[pRequest.nVirtualPage] = ( uint16_t )nJob;
	if ( !t3d_vt_stream_queue_insert( rState, nJob ) ) {
		t3d_vt_stream_free_job( rState, nJob );
		++rState.Stats.nDroppedRequestCount;
		t3d_vt_stream_record_admission_status( rState, T3D_VT_ADMISSION_DROPPED_QUEUE_FULL );
		return T3D_VT_ADMISSION_DROPPED_QUEUE_FULL;
	}
	++rState.nAdmittedRequestCountThisFrame;
	++rState.Stats.nQueuedCount;
	t3d_vt_stream_record_admission_status( rState, T3D_VT_ADMISSION_ACCEPTED_NEW );
	return T3D_VT_ADMISSION_ACCEPTED_NEW;
}

uint32_t t3d_vt_streaming_ingest_requests_hot(
        T3DVTStreamingState &rState, const T3DVTRequest *pRequests, const uint32_t pRequestCount, const uint32_t pFrameId ) {
	if ( !rState.bInitialized || pRequests == NULL ) {
		return 0u;
	}
	t3d_vt_stream_begin_admission_frame( rState, pFrameId );
	uint32_t nAccepted = 0u;
	for ( uint32_t iRequest = 0u; iRequest < pRequestCount; ++iRequest ) {
		if ( t3d_vt_streaming_admit_request_hot( rState, pRequests[iRequest], pFrameId ) == T3D_VT_ADMISSION_ACCEPTED_NEW ) {
			++nAccepted;
		}
	}
	return nAccepted;
}

T3DVTReadbackConsumeStatus t3d_vt_streaming_ingest_readback_owner_hot( T3DVTStreamingState &rState,
        const T3DVTReadbackSlotHeader &pHeader,
        const uint8_t *pReadbackBytes,
        const uint32_t pReadbackByteCount,
        T3DVTGpuCounters &rCounters,
        uint32_t &rAcceptedRequestCount ) {
	memset( &rCounters, 0, sizeof( rCounters ) );
	rAcceptedRequestCount = 0u;
	if ( !rState.bInitialized || rState.pResidency == NULL || pReadbackBytes == NULL || pHeader.nRequestCapacity == 0u
	        || pHeader.nReadbackBytes == 0u || pHeader.nReadbackBytes != pReadbackByteCount ) {
		++rState.Stats.nInvalidReadbackCount;
		return T3D_VT_READBACK_INVALID;
	}
	if ( pHeader.nDatabaseGeneration != rState.pResidency->nDatabaseGeneration ) {
		++rState.Stats.nStaleReadbackCount;
		return T3D_VT_READBACK_STALE_DATABASE;
	}
	const T3DVTRequest *pRequests = NULL;
	uint32_t nRequestCount = 0u;
	if ( !t3d_vt_decode_readback_reference_debug(
	             pReadbackBytes, pReadbackByteCount, pHeader.nRequestCapacity, rCounters, pRequests, nRequestCount ) ) {
		++rState.Stats.nInvalidReadbackCount;
		return T3D_VT_READBACK_INVALID;
	}
	rAcceptedRequestCount = t3d_vt_streaming_ingest_requests_hot( rState, pRequests, nRequestCount, pHeader.nFrameId );
	return T3D_VT_READBACK_CONSUMED;
}

uint32_t t3d_vt_streaming_ingest_readback_owner_hot( T3DVTStreamingState &rState,
        const uint8_t *pReadbackBytes,
        const uint32_t pReadbackByteCount,
        const uint32_t pRequestCapacity,
        const uint32_t pFrameId,
        T3DVTGpuCounters &rCounters ) {
	T3DVTReadbackSlotHeader Header = {};
	Header.nFrameId = pFrameId;
	Header.nDatabaseGeneration = rState.pResidency != NULL ? rState.pResidency->nDatabaseGeneration : 0u;
	Header.nRequestCapacity = pRequestCapacity;
	Header.nReadbackBytes = pReadbackByteCount;
	uint32_t nAcceptedRequestCount = 0u;
	return t3d_vt_streaming_ingest_readback_owner_hot(
	               rState, Header, pReadbackBytes, pReadbackByteCount, rCounters, nAcceptedRequestCount )
	                == T3D_VT_READBACK_CONSUMED
	        ? nAcceptedRequestCount
	        : 0u;
}

uint32_t t3d_vt_streaming_invalidate_queued_demand_hot( T3DVTStreamingState &rState ) {
	if ( !rState.bInitialized ) return 0u;
	uint32_t nCancelled = 0u;
	for ( uint32_t iJob = 0u; iJob < rState.Buffers.nJobCapacity; ++iJob ) {
		if ( rState.Buffers.pJobs[iJob].nState != T3D_VT_STREAM_QUEUED ) continue;
		t3d_vt_stream_free_job( rState, iJob );
		++nCancelled;
	}
	rState.nAdmissionFrame = T3D_RW_INVALID_ID;
	rState.nAdmittedRequestCountThisFrame = 0u;
	return nCancelled;
}

bool t3d_vt_streaming_claim_job_owner_hot( T3DVTStreamingState &rState, uint32_t &rJobIndex ) {
	rJobIndex = T3D_RW_INVALID_ID;
	if ( !rState.bInitialized || rState.nQueuedJobCount == 0u ) {
		return false;
	}
	if ( rState.nInFlightIoJobCount >= rState.Limits.nMaxInFlightIoJobs ) {
		++rState.Stats.nIoBudgetDeferredCount;
		++rState.Stats.nBudgetDeferredCount;
		return false;
	}
	const uint32_t nLane = t3d_vt_stream_find_free_lane( rState );
	if ( nLane == T3D_RW_INVALID_ID ) {
		return false;
	}
	uint32_t nJob = T3D_RW_INVALID_ID;
	uint16_t nBucket = T3D_VT_PRIORITY_BACKGROUND;
	for ( uint16_t iBucket = 0u; iBucket < T3D_VT_PRIORITY_BUCKET_COUNT; ++iBucket ) {
		if ( rState.nPriorityBucketHead[iBucket] != s_nInvalidJob16 ) {
			nJob = rState.nPriorityBucketHead[iBucket];
			nBucket = iBucket;
			break;
		}
	}
	if ( nJob == T3D_RW_INVALID_ID ) {
		return false;
	}
	t3d_vt_stream_queue_remove( rState, nJob );
	T3DVTStreamJob &Job = rState.Buffers.pJobs[nJob];
	Job.nState = T3D_VT_STREAM_WORKING;
	Job.nScratchLane = nLane;
	rState.Buffers.pScratchLaneOwner[nLane] = nJob;
	rState.Buffers.pScratchLaneCompletion[nLane].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
	++rState.nInFlightIoJobCount;
	if ( rState.nInFlightIoJobCount > rState.Stats.nInFlightIoHighWater ) {
		rState.Stats.nInFlightIoHighWater = rState.nInFlightIoJobCount;
	}
	++rState.Stats.nPriorityBucketClaimCount[nBucket];
	++rState.Stats.nClaimedCount;
	rJobIndex = nJob;
	return true;
}

bool t3d_vt_streaming_cancel_claim_owner_hot( T3DVTStreamingState &rState, const uint32_t pJobIndex ) {
	if ( !rState.bInitialized || pJobIndex >= rState.Buffers.nJobCapacity ) {
		return false;
	}
	T3DVTStreamJob &Job = rState.Buffers.pJobs[pJobIndex];
	if ( Job.nState != T3D_VT_STREAM_WORKING || Job.nScratchLane >= rState.Buffers.nScratchLaneCount
	        || rState.Buffers.pScratchLaneOwner[Job.nScratchLane] != pJobIndex ) {
		return false;
	}
	rState.Buffers.pScratchLaneOwner[Job.nScratchLane] = T3D_RW_INVALID_ID;
	rState.Buffers.pScratchLaneCompletion[Job.nScratchLane].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
	if ( rState.nInFlightIoJobCount != 0u ) {
		--rState.nInFlightIoJobCount;
	}
	Job.nScratchLane = T3D_RW_INVALID_ID;
	Job.nPriority = rState.Buffers.pMergedPriority[pJobIndex];
	Job.nWantedMip = rState.Buffers.pMergedWantedMip[pJobIndex];
	Job.nState = T3D_VT_STREAM_QUEUED;
	return t3d_vt_stream_queue_insert( rState, pJobIndex );
}

bool t3d_vt_streaming_execute_job_async(
        T3DVTStreamingState &rState, const uint32_t pJobIndex, T3DVTStreamReadDecodeFn pReadDecode, void *pUser ) {
	if ( !rState.bInitialized || pReadDecode == NULL || pJobIndex >= rState.Buffers.nJobCapacity ) {
		return false;
	}
	T3DVTStreamJob &Job = rState.Buffers.pJobs[pJobIndex];
	if ( Job.nState != T3D_VT_STREAM_WORKING || Job.nScratchLane >= rState.Buffers.nScratchLaneCount || Job.nReadBytes == 0u
	        || Job.nExpectedDecodedBytes == 0u || Job.nReadBytes > rState.Buffers.nScratchLaneBytes
	        || Job.nExpectedDecodedBytes > rState.Buffers.nScratchLaneBytes ) {
		return false;
	}
	uint8_t *pScratch = rState.Buffers.pScratchBytes + ( uint64_t )Job.nScratchLane * rState.Buffers.nScratchLaneBytes;
	uint32_t nDecodedBytes = 0u;
	const bool bOk = pReadDecode( *rState.pResidency->pDatabase, Job, pScratch, rState.Buffers.nScratchLaneBytes, nDecodedBytes, pUser );
	if ( !bOk || nDecodedBytes == 0u || nDecodedBytes > rState.Buffers.nScratchLaneBytes ) {
		Job.nDecodedBytes = 0u;
		Job.nResultCode = T3D_VT_STREAM_RESULT_READ_FAILED;
		rState.Buffers.pScratchLaneCompletion[Job.nScratchLane].store( T3D_VT_STREAM_COMPLETION_FAILED, T3D_MEMORY_ORDER_RELEASE );
		return false;
	}
	if ( nDecodedBytes != Job.nExpectedDecodedBytes ) {
		Job.nDecodedBytes = 0u;
		Job.nResultCode = T3D_VT_STREAM_RESULT_OUTPUT_SIZE_MISMATCH;
		rState.Buffers.pScratchLaneCompletion[Job.nScratchLane].store( T3D_VT_STREAM_COMPLETION_FAILED, T3D_MEMORY_ORDER_RELEASE );
		return false;
	}
	Job.nDecodedBytes = nDecodedBytes;
	Job.nResultCode = T3D_VT_STREAM_RESULT_OK;
	rState.Buffers.pScratchLaneCompletion[Job.nScratchLane].store( T3D_VT_STREAM_COMPLETION_READY, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

static void t3d_vt_stream_collect_completions_owner_hot( T3DVTStreamingState &rState ) {
	for ( uint32_t iLane = 0u; iLane < rState.Buffers.nScratchLaneCount; ++iLane ) {
		const uint32_t nJob = rState.Buffers.pScratchLaneOwner[iLane];
		if ( nJob == T3D_RW_INVALID_ID || nJob >= rState.Buffers.nJobCapacity ) {
			continue;
		}
		T3DVTStreamJob &Job = rState.Buffers.pJobs[nJob];
		if ( Job.nState != T3D_VT_STREAM_WORKING || Job.nScratchLane != iLane ) {
			continue;
		}
		const uint32_t nCompletion = rState.Buffers.pScratchLaneCompletion[iLane].load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( nCompletion == T3D_VT_STREAM_COMPLETION_IDLE ) {
			continue;
		}
		rState.Buffers.pScratchLaneCompletion[iLane].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
		if ( rState.nInFlightIoJobCount != 0u ) {
			--rState.nInFlightIoJobCount;
		}
		Job.nState = nCompletion == T3D_VT_STREAM_COMPLETION_READY ? T3D_VT_STREAM_READY : T3D_VT_STREAM_FAILED;
		if ( Job.nState == T3D_VT_STREAM_READY ) {
			++rState.Stats.nCompletedCount;
			const T3DVTDatabaseView *const pDatabase = rState.pResidency != NULL ? rState.pResidency->pDatabase : NULL;
			if ( pDatabase != NULL && pDatabase->pHeader != NULL && pDatabase->pPayloads != NULL
			        && Job.nPayloadId < pDatabase->pHeader->nPayloadCount ) {
				rState.Stats.nIoBytesTotal += pDatabase->pPayloads[Job.nPayloadId].nCompressedBytes;
			}
		} else {
			++rState.Stats.nDecodeFailureCount;
		}
		if ( rState.nReadyJobCount < rState.Buffers.nJobCapacity ) {
			rState.Buffers.pReadyJobs[rState.nReadyJobCount++] = ( uint16_t )nJob;
		}
	}
}

static void t3d_vt_stream_begin_upload_frame( T3DVTStreamingState &rState, const uint32_t pFrameId ) {
	if ( rState.nUploadFrame != pFrameId ) {
		rState.nUploadFrame = pFrameId;
		rState.nUploadedPageSetCountThisFrame = 0u;
		rState.nUploadedBytesThisFrame = 0u;
	}
}

static uint32_t t3d_vt_stream_validate_completion_identity( const T3DVTStreamingState &pState, const T3DVTStreamJob &pJob ) {
	if ( pState.pResidency == NULL || pState.pResidency->pDatabase == NULL || pState.pResidency->pDatabase->pHeader == NULL
	        || pJob.nDatabaseGeneration != pState.pResidency->nDatabaseGeneration ) {
		return T3D_VT_STREAM_RESULT_STALE_DATABASE;
	}
	const T3DVTDatabaseView &Database = *pState.pResidency->pDatabase;
	if ( pJob.nVirtualPage >= Database.pHeader->nPageCount || pJob.nVirtualPage >= pState.pResidency->Buffers.nPageTableCapacity
	        || pJob.nPageGeneration != pState.pResidency->Buffers.pPageTable[pJob.nVirtualPage].nGeneration ) {
		return T3D_VT_STREAM_RESULT_STALE_PAGE;
	}
	const T3DVTPageFileRecord &Page = Database.pPages[pJob.nVirtualPage];
	if ( Page.nAllocationId != pJob.nAllocationId || Page.nPayloadId != pJob.nPayloadId
	        || Page.nGeneration != pJob.nAllocationGeneration ) {
		return T3D_VT_STREAM_RESULT_STALE_ALLOCATION;
	}
	return T3D_VT_STREAM_RESULT_OK;
}

static bool t3d_vt_stream_ready_before( const T3DVTStreamingState &pState, const uint32_t pJobA, const uint32_t pJobB ) {
	const uint16_t nPriorityA = pState.Buffers.pMergedPriority[pJobA];
	const uint16_t nPriorityB = pState.Buffers.pMergedPriority[pJobB];
	const uint16_t nBucketA = t3d_vt_scheduler_priority_bucket_reference_debug( nPriorityA );
	const uint16_t nBucketB = t3d_vt_scheduler_priority_bucket_reference_debug( nPriorityB );
	if ( nBucketA != nBucketB ) {
		return nBucketA < nBucketB;
	}
	if ( nPriorityA != nPriorityB ) {
		return nPriorityA > nPriorityB;
	}
	return pState.Buffers.pJobs[pJobA].nRequestFrame < pState.Buffers.pJobs[pJobB].nRequestFrame;
}

static uint32_t t3d_vt_stream_best_ready_index( const T3DVTStreamingState &pState, const uint32_t pStartIndex ) {
	uint32_t nBestIndex = pStartIndex;
	for ( uint32_t iReady = pStartIndex + 1u; iReady < pState.nReadyJobCount; ++iReady ) {
		if ( t3d_vt_stream_ready_before( pState, pState.Buffers.pReadyJobs[iReady], pState.Buffers.pReadyJobs[nBestIndex] ) ) {
			nBestIndex = iReady;
		}
	}
	return nBestIndex;
}

uint64_t t3d_vt_streaming_service_uploads_batched_hot( T3DVTStreamingState &rState,
        const uint32_t pFrameId,
        T3DVTUploadBatchBeginFn pBatchBegin,
        T3DVTUploadCommitFn pUploadStage,
        T3DVTUploadBatchEndFn pBatchEnd,
        void *pUser ) {
	if ( !rState.bInitialized || pUploadStage == NULL || ( pBatchBegin == NULL ) != ( pBatchEnd == NULL ) ) {
		return 0u;
	}
	t3d_vt_stream_collect_completions_owner_hot( rState );
	t3d_vt_stream_begin_upload_frame( rState, pFrameId );
	const uint64_t nUploadedBefore = rState.nUploadedBytesThisFrame;
	const bool bBatched = pBatchBegin != NULL;
	if ( bBatched && !pBatchBegin( pUser ) ) {
		return 0u;
	}
	uint32_t iReady = 0u;
	uint32_t nStagedPageSets = 0u;
	uint64_t nStagedBytes = 0u;
	while ( iReady < rState.nReadyJobCount ) {
		const uint32_t nBestIndex = t3d_vt_stream_best_ready_index( rState, iReady );
		if ( nBestIndex != iReady ) {
			const uint16_t nSwap = rState.Buffers.pReadyJobs[iReady];
			rState.Buffers.pReadyJobs[iReady] = rState.Buffers.pReadyJobs[nBestIndex];
			rState.Buffers.pReadyJobs[nBestIndex] = nSwap;
		}
		const uint32_t nJob = rState.Buffers.pReadyJobs[iReady];
		T3DVTStreamJob &Job = rState.Buffers.pJobs[nJob];
		if ( Job.nState == T3D_VT_STREAM_FAILED ) {
			t3d_vt_stream_ready_remove( rState, iReady );
			t3d_vt_stream_free_job( rState, nJob );
			continue;
		}
		if ( Job.nState != T3D_VT_STREAM_READY ) {
			t3d_vt_stream_ready_remove( rState, iReady );
			continue;
		}
		const uint32_t nIdentityResult = t3d_vt_stream_validate_completion_identity( rState, Job );
		if ( nIdentityResult != T3D_VT_STREAM_RESULT_OK
		        || ( rState.pResidency->Buffers.pPageTable[Job.nVirtualPage].nFlags & T3D_VT_PAGE_RESIDENT ) != 0u ) {
			Job.nResultCode = nIdentityResult;
			++rState.Stats.nStaleCompletionCount;
			t3d_vt_stream_ready_remove( rState, iReady );
			t3d_vt_stream_free_job( rState, nJob );
			continue;
		}
		if ( rState.nUploadedPageSetCountThisFrame + nStagedPageSets >= rState.Limits.nMaxPageSetsPerFrame ) {
			++rState.Stats.nPageSetBudgetDeferredCount;
			++rState.Stats.nBudgetDeferredCount;
			++iReady;
			continue;
		}
		const uint64_t nFrameBytes = rState.nUploadedBytesThisFrame + nStagedBytes;
		if ( nFrameBytes >= rState.Limits.nMaxUploadBytesPerFrame
		        || Job.nDecodedBytes > rState.Limits.nMaxUploadBytesPerFrame - nFrameBytes ) {
			++rState.Stats.nUploadByteBudgetDeferredCount;
			++rState.Stats.nBudgetDeferredCount;
			++iReady;
			continue;
		}
		T3DVTUploadReservation Reservation = {};
		const uint16_t nMergedWantedMip = rState.Buffers.pMergedWantedMip[nJob];
		if ( !t3d_vt_reserve_upload_hot( *rState.pResidency, Job.nVirtualPage, nMergedWantedMip, pFrameId, Reservation ) ) {
			++iReady;
			continue;
		}
		const uint8_t *pScratch = rState.Buffers.pScratchBytes + ( uint64_t )Job.nScratchLane * rState.Buffers.nScratchLaneBytes;
		if ( !pUploadStage( *rState.pResidency, Reservation, pScratch, Job.nDecodedBytes, pFrameId, pUser ) ) {
			t3d_vt_abort_upload_hot( *rState.pResidency, Reservation );
			break;
		}
		if ( bBatched ) {
			Job.nState = T3D_VT_STREAM_STAGED;
			++nStagedPageSets;
			nStagedBytes += Job.nDecodedBytes;
			++iReady;
			continue;
		}
		rState.nUploadedBytesThisFrame += Job.nDecodedBytes;
		++rState.nUploadedPageSetCountThisFrame;
		++rState.Stats.nUploadedPageCount;
		rState.Stats.nUploadedBytes += Job.nDecodedBytes;
		t3d_vt_stream_ready_remove( rState, iReady );
		t3d_vt_stream_free_job( rState, nJob );
	}
	if ( bBatched ) {
		if ( !pBatchEnd( *rState.pResidency, pFrameId, pUser ) ) {
			for ( uint32_t i = 0u; i < rState.nReadyJobCount; ++i ) {
				T3DVTStreamJob &Job = rState.Buffers.pJobs[rState.Buffers.pReadyJobs[i]];
				if ( Job.nState == T3D_VT_STREAM_STAGED ) {
					Job.nState = T3D_VT_STREAM_READY;
				}
			}
			return rState.nUploadedBytesThisFrame - nUploadedBefore;
		}
		rState.nUploadedBytesThisFrame += nStagedBytes;
		rState.nUploadedPageSetCountThisFrame += nStagedPageSets;
		uint32_t i = 0u;
		while ( i < rState.nReadyJobCount ) {
			const uint32_t nJob = rState.Buffers.pReadyJobs[i];
			T3DVTStreamJob &Job = rState.Buffers.pJobs[nJob];
			if ( Job.nState != T3D_VT_STREAM_STAGED ) {
				++i;
				continue;
			}
			++rState.Stats.nUploadedPageCount;
			rState.Stats.nUploadedBytes += Job.nDecodedBytes;
			t3d_vt_stream_ready_remove( rState, i );
			t3d_vt_stream_free_job( rState, nJob );
		}
	}
	return rState.nUploadedBytesThisFrame - nUploadedBefore;
}

uint64_t t3d_vt_streaming_service_uploads_hot(
        T3DVTStreamingState &rState, const uint32_t pFrameId, T3DVTUploadCommitFn pUploadCommit, void *pUser ) {
	return t3d_vt_streaming_service_uploads_batched_hot( rState, pFrameId, NULL, pUploadCommit, NULL, pUser );
}

bool t3d_vt_decode_readback_reference_debug( const uint8_t *pReadbackBytes,
        const uint32_t pReadbackByteCount,
        const uint32_t pRequestCapacity,
        T3DVTGpuCounters &rCounters,
        const T3DVTRequest *&rRequests,
        uint32_t &rRequestCount ) {
	memset( &rCounters, 0, sizeof( rCounters ) );
	rRequests = NULL;
	rRequestCount = 0u;
	if ( pReadbackBytes == NULL || pReadbackByteCount < sizeof( T3DVTGpuCounters ) ) {
		return false;
	}
	memcpy( &rCounters, pReadbackBytes, sizeof( rCounters ) );
	rRequestCount = rCounters.nRequestCount < pRequestCapacity ? rCounters.nRequestCount : pRequestCapacity;
	const uint64_t nRequired = sizeof( T3DVTGpuCounters ) + ( uint64_t )rRequestCount * sizeof( T3DVTRequest );
	if ( nRequired > pReadbackByteCount ) {
		return false;
	}
	rRequests = ( const T3DVTRequest * )( pReadbackBytes + sizeof( T3DVTGpuCounters ) );
	return true;
}
