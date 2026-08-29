// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_streaming.h"
#include "core/terrain_3d_sync.h"

#include <string.h>

static uint32_t t3d_rw_stream_find_free_lane( const T3DVTStreamingState &pState ) {
	for ( uint32_t i = 0u; i < pState.Buffers.nScratchLaneCount; ++i ) {
		if ( pState.Buffers.pScratchLaneOwner[i] == T3D_RW_INVALID_ID ) {
			return i;
		}
	}
	return T3D_RW_INVALID_ID;
}

static uint32_t t3d_rw_stream_find_existing( const T3DVTStreamingState &pState, uint32_t pVirtualPage ) {
	for ( uint32_t i = 0u; i < pState.Buffers.nJobCapacity; ++i ) {
		const T3DVTStreamJob &Job = pState.Buffers.pJobs[i];
		if ( Job.nState != T3D_VT_STREAM_FREE && Job.nVirtualPage == pVirtualPage ) {
			return i;
		}
	}
	return T3D_RW_INVALID_ID;
}

static uint32_t t3d_rw_stream_find_free_job( const T3DVTStreamingState &pState ) {
	for ( uint32_t i = 0u; i < pState.Buffers.nJobCapacity; ++i ) {
		if ( pState.Buffers.pJobs[i].nState == T3D_VT_STREAM_FREE ) {
			return i;
		}
	}
	return T3D_RW_INVALID_ID;
}

static uint32_t t3d_rw_stream_find_replaceable( const T3DVTStreamingState &pState, uint16_t pPriority ) {
	uint32_t nBest = T3D_RW_INVALID_ID;
	for ( uint32_t i = 0u; i < pState.Buffers.nJobCapacity; ++i ) {
		const T3DVTStreamJob &Job = pState.Buffers.pJobs[i];
		if ( Job.nState != T3D_VT_STREAM_QUEUED || Job.nPriority >= pPriority ) {
			continue;
		}
		if ( nBest == T3D_RW_INVALID_ID || Job.nPriority < pState.Buffers.pJobs[nBest].nPriority
		        || ( Job.nPriority == pState.Buffers.pJobs[nBest].nPriority
		                && Job.nRequestFrame > pState.Buffers.pJobs[nBest].nRequestFrame ) ) {
			nBest = i;
		}
	}
	return nBest;
}

static void t3d_rw_stream_free_job( T3DVTStreamingState &rState, uint32_t pJobIndex ) {
	T3DVTStreamJob &Job = rState.Buffers.pJobs[pJobIndex];
	if ( Job.nScratchLane != T3D_RW_INVALID_ID && Job.nScratchLane < rState.Buffers.nScratchLaneCount ) {
		rState.Buffers.pScratchLaneOwner[Job.nScratchLane] = T3D_RW_INVALID_ID;
	}
	memset( &Job, 0, sizeof( Job ) );
	Job.nScratchLane = T3D_RW_INVALID_ID;
	Job.nState = T3D_VT_STREAM_FREE;
	if ( rState.Stats.nActiveJobCount != 0u ) {
		--rState.Stats.nActiveJobCount;
	}
}

bool t3d_render_world_vt_streaming_initialize_cold( T3DVTResidencyState &rResidency,
        const T3DVTStreamingBuffers &pBuffers,
        uint64_t pUploadBudgetBytesPerFrame,
        T3DVTStreamingState &rState ) {
	memset( &rState, 0, sizeof( rState ) );
	if ( !rResidency.bInitialized || pBuffers.pJobs == NULL || pBuffers.nJobCapacity == 0u || pBuffers.pScratchBytes == NULL
	        || pBuffers.pScratchLaneOwner == NULL || pBuffers.pScratchLaneCompletion == NULL || pBuffers.nScratchLaneCount == 0u
	        || pBuffers.nScratchLaneCount > T3D_RW_MAX_STREAM_SCRATCH_LANES || pBuffers.nScratchLaneBytes == 0u
	        || pUploadBudgetBytesPerFrame == 0u ) {
		return false;
	}
	rState.pResidency = &rResidency;
	rState.Buffers = pBuffers;
	rState.nUploadBudgetBytesPerFrame = pUploadBudgetBytesPerFrame;
	for ( uint32_t i = 0u; i < pBuffers.nJobCapacity; ++i ) {
		memset( &pBuffers.pJobs[i], 0, sizeof( T3DVTStreamJob ) );
		pBuffers.pJobs[i].nScratchLane = T3D_RW_INVALID_ID;
	}
	for ( uint32_t i = 0u; i < pBuffers.nScratchLaneCount; ++i ) {
		pBuffers.pScratchLaneOwner[i] = T3D_RW_INVALID_ID;
		pBuffers.pScratchLaneCompletion[i].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
	}
	rState.bInitialized = true;
	return true;
}

uint32_t t3d_render_world_vt_streaming_ingest_requests_hot(
        T3DVTStreamingState &rState, const T3DVTRequest *pRequests, uint32_t pRequestCount, uint32_t pFrameId ) {
	if ( !rState.bInitialized || pRequests == NULL ) {
		return 0u;
	}
	uint32_t nAccepted = 0u;
	for ( uint32_t i = 0u; i < pRequestCount; ++i ) {
		const T3DVTRequest &Request = pRequests[i];
		if ( Request.nVirtualPage >= rState.pResidency->pDatabase->pHeader->nPageCount ) {
			++rState.Stats.nDroppedRequestCount;
			continue;
		}
		const T3DVTPageTableEntry &Entry = rState.pResidency->Buffers.pPageTable[Request.nVirtualPage];
		if ( ( Entry.nFlags & T3D_VT_PAGE_RESIDENT ) != 0u ) {
			continue;
		}
		uint32_t nJob = t3d_rw_stream_find_existing( rState, Request.nVirtualPage );
		if ( nJob != T3D_RW_INVALID_ID ) {
			T3DVTStreamJob &Job = rState.Buffers.pJobs[nJob];
			if ( Request.nPriority > Job.nPriority ) {
				Job.nPriority = Request.nPriority;
			}
			if ( Request.nWantedMip < Job.nWantedMip ) {
				Job.nWantedMip = Request.nWantedMip;
			}
			continue;
		}
		nJob = t3d_rw_stream_find_free_job( rState );
		if ( nJob == T3D_RW_INVALID_ID ) {
			nJob = t3d_rw_stream_find_replaceable( rState, Request.nPriority );
			if ( nJob == T3D_RW_INVALID_ID ) {
				++rState.Stats.nDroppedRequestCount;
				continue;
			}
			++rState.Stats.nReplacedQueuedJobCount;
		} else {
			++rState.Stats.nActiveJobCount;
			if ( rState.Stats.nActiveJobCount > rState.Stats.nQueueHighWater ) {
				rState.Stats.nQueueHighWater = rState.Stats.nActiveJobCount;
			}
		}
		const T3DVTPageFileRecord &Page = rState.pResidency->pDatabase->pPages[Request.nVirtualPage];
		T3DVTStreamJob &Job = rState.Buffers.pJobs[nJob];
		memset( &Job, 0, sizeof( Job ) );
		Job.nVirtualPage = Request.nVirtualPage;
		Job.nPayloadId = Page.nPayloadId;
		Job.nPageGeneration = Entry.nGeneration;
		Job.nDatabaseGeneration = rState.pResidency->nDatabaseGeneration;
		Job.nRequestFrame = pFrameId;
		Job.nScratchLane = T3D_RW_INVALID_ID;
		Job.nWantedMip = Request.nWantedMip;
		Job.nPriority = Request.nPriority;
		Job.nChannel = Page.nChannel;
		Job.nState = T3D_VT_STREAM_QUEUED;
		++rState.Stats.nQueuedCount;
		++nAccepted;
	}
	return nAccepted;
}

uint32_t t3d_render_world_vt_streaming_ingest_readback_owner_hot( T3DVTStreamingState &rState,
        const uint8_t *pReadbackBytes,
        uint32_t pReadbackByteCount,
        uint32_t pRequestCapacity,
        uint32_t pFrameId,
        T3DVTGpuCounters &rCounters ) {
	const T3DVTRequest *pRequests = NULL;
	uint32_t nRequestCount = 0u;
	if ( !t3d_render_world_vt_decode_readback_reference_debug(
	             pReadbackBytes, pReadbackByteCount, pRequestCapacity, rCounters, pRequests, nRequestCount ) ) {
		return 0u;
	}
	return t3d_render_world_vt_streaming_ingest_requests_hot( rState, pRequests, nRequestCount, pFrameId );
}

bool t3d_render_world_vt_streaming_claim_job_owner_hot( T3DVTStreamingState &rState, uint32_t &rJobIndex ) {
	rJobIndex = T3D_RW_INVALID_ID;
	if ( !rState.bInitialized ) {
		return false;
	}
	const uint32_t nLane = t3d_rw_stream_find_free_lane( rState );
	if ( nLane == T3D_RW_INVALID_ID ) {
		return false;
	}
	uint32_t nBest = T3D_RW_INVALID_ID;
	for ( uint32_t i = 0u; i < rState.Buffers.nJobCapacity; ++i ) {
		const T3DVTStreamJob &Job = rState.Buffers.pJobs[i];
		if ( Job.nState != T3D_VT_STREAM_QUEUED ) {
			continue;
		}
		if ( nBest == T3D_RW_INVALID_ID || Job.nPriority > rState.Buffers.pJobs[nBest].nPriority
		        || ( Job.nPriority == rState.Buffers.pJobs[nBest].nPriority
		                && Job.nRequestFrame < rState.Buffers.pJobs[nBest].nRequestFrame ) ) {
			nBest = i;
		}
	}
	if ( nBest == T3D_RW_INVALID_ID ) {
		return false;
	}
	T3DVTStreamJob &Job = rState.Buffers.pJobs[nBest];
	Job.nState = T3D_VT_STREAM_WORKING;
	Job.nScratchLane = nLane;
	rState.Buffers.pScratchLaneOwner[nLane] = nBest;
	rState.Buffers.pScratchLaneCompletion[nLane].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
	++rState.Stats.nClaimedCount;
	rJobIndex = nBest;
	return true;
}

bool t3d_render_world_vt_streaming_execute_job_async(
        T3DVTStreamingState &rState, uint32_t pJobIndex, T3DVTStreamReadDecodeFn pReadDecode, void *pUser ) {
	if ( !rState.bInitialized || pReadDecode == NULL || pJobIndex >= rState.Buffers.nJobCapacity ) {
		return false;
	}
	T3DVTStreamJob &Job = rState.Buffers.pJobs[pJobIndex];
	if ( Job.nState != T3D_VT_STREAM_WORKING || Job.nScratchLane >= rState.Buffers.nScratchLaneCount ) {
		return false;
	}
	uint8_t *pScratch = rState.Buffers.pScratchBytes + ( uint64_t )Job.nScratchLane * rState.Buffers.nScratchLaneBytes;
	uint32_t nDecodedBytes = 0u;
	const bool bOk = pReadDecode( *rState.pResidency->pDatabase, Job, pScratch, rState.Buffers.nScratchLaneBytes, nDecodedBytes, pUser );
	if ( !bOk || nDecodedBytes == 0u || nDecodedBytes > rState.Buffers.nScratchLaneBytes ) {
		Job.nDecodedBytes = 0u;
		Job.nResultCode = 1u;
		rState.Buffers.pScratchLaneCompletion[Job.nScratchLane].store( T3D_VT_STREAM_COMPLETION_FAILED, T3D_MEMORY_ORDER_RELEASE );
		return false;
	}
	Job.nDecodedBytes = nDecodedBytes;
	Job.nResultCode = 0u;
	rState.Buffers.pScratchLaneCompletion[Job.nScratchLane].store( T3D_VT_STREAM_COMPLETION_READY, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

static void t3d_rw_stream_collect_completions_owner_hot( T3DVTStreamingState &rState ) {
	for ( uint32_t i = 0u; i < rState.Buffers.nJobCapacity; ++i ) {
		T3DVTStreamJob &Job = rState.Buffers.pJobs[i];
		if ( Job.nState != T3D_VT_STREAM_WORKING || Job.nScratchLane >= rState.Buffers.nScratchLaneCount ) {
			continue;
		}
		const uint32_t nCompletion = rState.Buffers.pScratchLaneCompletion[Job.nScratchLane].load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( nCompletion == T3D_VT_STREAM_COMPLETION_IDLE ) {
			continue;
		}
		rState.Buffers.pScratchLaneCompletion[Job.nScratchLane].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
		if ( nCompletion == T3D_VT_STREAM_COMPLETION_READY ) {
			Job.nState = T3D_VT_STREAM_READY;
			++rState.Stats.nCompletedCount;
		} else {
			Job.nState = T3D_VT_STREAM_FAILED;
			++rState.Stats.nDecodeFailureCount;
		}
	}
}

uint64_t t3d_render_world_vt_streaming_service_uploads_hot(
        T3DVTStreamingState &rState, uint32_t pFrameId, T3DVTUploadCommitFn pUploadCommit, void *pUser ) {
	if ( !rState.bInitialized || pUploadCommit == NULL ) {
		return 0u;
	}
	t3d_rw_stream_collect_completions_owner_hot( rState );
	uint64_t nUploaded = 0u;
	for ( uint32_t i = 0u; i < rState.Buffers.nJobCapacity; ++i ) {
		T3DVTStreamJob &Job = rState.Buffers.pJobs[i];
		if ( Job.nState == T3D_VT_STREAM_FAILED ) {
			t3d_rw_stream_free_job( rState, i );
			continue;
		}
		if ( Job.nState != T3D_VT_STREAM_READY ) {
			continue;
		}
		if ( Job.nDatabaseGeneration != rState.pResidency->nDatabaseGeneration
		        || Job.nVirtualPage >= rState.pResidency->pDatabase->pHeader->nPageCount
		        || Job.nPageGeneration != rState.pResidency->Buffers.pPageTable[Job.nVirtualPage].nGeneration
		        || ( rState.pResidency->Buffers.pPageTable[Job.nVirtualPage].nFlags & T3D_VT_PAGE_RESIDENT ) != 0u ) {
			++rState.Stats.nStaleCompletionCount;
			t3d_rw_stream_free_job( rState, i );
			continue;
		}
		if ( Job.nDecodedBytes > rState.nUploadBudgetBytesPerFrame - nUploaded ) {
			++rState.Stats.nBudgetDeferredCount;
			continue;
		}
		T3DVTUploadReservation Reservation = {};
		if ( !t3d_render_world_vt_reserve_upload_hot( *rState.pResidency, Job.nVirtualPage, Job.nWantedMip, pFrameId, Reservation ) ) {
			continue;
		}
		const uint8_t *pScratch = rState.Buffers.pScratchBytes + ( uint64_t )Job.nScratchLane * rState.Buffers.nScratchLaneBytes;
		if ( !pUploadCommit( *rState.pResidency, Reservation, pScratch, Job.nDecodedBytes, pFrameId, pUser ) ) {
			t3d_render_world_vt_abort_upload_hot( *rState.pResidency, Reservation );
			break;
		}
		nUploaded += Job.nDecodedBytes;
		++rState.Stats.nUploadedPageCount;
		rState.Stats.nUploadedBytes += Job.nDecodedBytes;
		t3d_rw_stream_free_job( rState, i );
	}
	return nUploaded;
}

bool t3d_render_world_vt_decode_readback_reference_debug( const uint8_t *pReadbackBytes,
        uint32_t pReadbackByteCount,
        uint32_t pRequestCapacity,
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
