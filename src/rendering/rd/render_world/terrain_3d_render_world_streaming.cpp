// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_streaming.h"
#include "core/terrain_3d_sync.h"

#include <string.h>

static const uint16_t s_nInvalidJob16 = 0xffffu;

static uint32_t t3d_rw_stream_find_free_lane( const T3DVTStreamingState &pState ) {
	for ( uint32_t i = 0u; i < pState.Buffers.nScratchLaneCount; ++i ) {
		if ( pState.Buffers.pScratchLaneOwner[i] == T3D_RW_INVALID_ID ) {
			return i;
		}
	}
	return T3D_RW_INVALID_ID;
}

static bool t3d_rw_stream_queue_before( const T3DVTStreamJob &pA, const T3DVTStreamJob &pB ) {
	return pA.nPriority > pB.nPriority || ( pA.nPriority == pB.nPriority && pA.nRequestFrame < pB.nRequestFrame );
}

static void t3d_rw_stream_queue_remove( T3DVTStreamingState &rState, uint32_t pJobIndex ) {
	const uint32_t nPosition = rState.Buffers.pJobQueuePosition[pJobIndex];
	if ( nPosition >= rState.nQueuedJobCount || rState.Buffers.pQueuedJobs[nPosition] != pJobIndex ) {
		return;
	}
	if ( nPosition + 1u < rState.nQueuedJobCount ) {
		memmove( rState.Buffers.pQueuedJobs + nPosition,
		        rState.Buffers.pQueuedJobs + nPosition + 1u,
		        ( rState.nQueuedJobCount - nPosition - 1u ) * sizeof( uint16_t ) );
	}
	--rState.nQueuedJobCount;
	rState.Buffers.pJobQueuePosition[pJobIndex] = s_nInvalidJob16;
	for ( uint32_t i = nPosition; i < rState.nQueuedJobCount; ++i ) {
		rState.Buffers.pJobQueuePosition[rState.Buffers.pQueuedJobs[i]] = ( uint16_t )i;
	}
}

static void t3d_rw_stream_queue_insert( T3DVTStreamingState &rState, uint32_t pJobIndex ) {
	uint32_t nInsert = rState.nQueuedJobCount;
	const T3DVTStreamJob &Job = rState.Buffers.pJobs[pJobIndex];
	for ( uint32_t i = 0u; i < rState.nQueuedJobCount; ++i ) {
		if ( t3d_rw_stream_queue_before( Job, rState.Buffers.pJobs[rState.Buffers.pQueuedJobs[i]] ) ) {
			nInsert = i;
			break;
		}
	}
	if ( nInsert < rState.nQueuedJobCount ) {
		memmove( rState.Buffers.pQueuedJobs + nInsert + 1u,
		        rState.Buffers.pQueuedJobs + nInsert,
		        ( rState.nQueuedJobCount - nInsert ) * sizeof( uint16_t ) );
	}
	rState.Buffers.pQueuedJobs[nInsert] = ( uint16_t )pJobIndex;
	++rState.nQueuedJobCount;
	for ( uint32_t i = nInsert; i < rState.nQueuedJobCount; ++i ) {
		rState.Buffers.pJobQueuePosition[rState.Buffers.pQueuedJobs[i]] = ( uint16_t )i;
	}
}

static uint32_t t3d_rw_stream_find_existing( const T3DVTStreamingState &pState, uint32_t pVirtualPage ) {
	if ( pVirtualPage >= pState.Buffers.nPageToJobCapacity ) {
		return T3D_RW_INVALID_ID;
	}
	const uint16_t nJob = pState.Buffers.pPageToJob[pVirtualPage];
	return nJob == s_nInvalidJob16 ? T3D_RW_INVALID_ID : ( uint32_t )nJob;
}

static uint32_t t3d_rw_stream_pop_free_job( T3DVTStreamingState &rState ) {
	if ( rState.nFreeJobCount == 0u ) {
		return T3D_RW_INVALID_ID;
	}
	return rState.Buffers.pFreeJobs[--rState.nFreeJobCount];
}

static uint32_t t3d_rw_stream_find_replaceable( const T3DVTStreamingState &pState, uint16_t pPriority ) {
	if ( pState.nQueuedJobCount == 0u ) {
		return T3D_RW_INVALID_ID;
	}
	const uint32_t nJob = pState.Buffers.pQueuedJobs[pState.nQueuedJobCount - 1u];
	return pState.Buffers.pJobs[nJob].nPriority < pPriority ? nJob : T3D_RW_INVALID_ID;
}

static void t3d_rw_stream_ready_remove( T3DVTStreamingState &rState, uint32_t pReadyIndex ) {
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

static void t3d_rw_stream_free_job( T3DVTStreamingState &rState, uint32_t pJobIndex ) {
	T3DVTStreamJob &Job = rState.Buffers.pJobs[pJobIndex];
	if ( Job.nState == T3D_VT_STREAM_QUEUED ) {
		t3d_rw_stream_queue_remove( rState, pJobIndex );
	}
	if ( Job.nVirtualPage < rState.Buffers.nPageToJobCapacity && rState.Buffers.pPageToJob[Job.nVirtualPage] == pJobIndex ) {
		rState.Buffers.pPageToJob[Job.nVirtualPage] = s_nInvalidJob16;
	}
	if ( Job.nScratchLane != T3D_RW_INVALID_ID && Job.nScratchLane < rState.Buffers.nScratchLaneCount ) {
		rState.Buffers.pScratchLaneOwner[Job.nScratchLane] = T3D_RW_INVALID_ID;
	}
	memset( &Job, 0, sizeof( Job ) );
	Job.nScratchLane = T3D_RW_INVALID_ID;
	Job.nState = T3D_VT_STREAM_FREE;
	if ( rState.nFreeJobCount < rState.Buffers.nJobCapacity ) {
		rState.Buffers.pFreeJobs[rState.nFreeJobCount++] = ( uint16_t )pJobIndex;
	}
	if ( rState.Stats.nActiveJobCount != 0u ) {
		--rState.Stats.nActiveJobCount;
	}
}

bool t3d_render_world_vt_streaming_initialize_cold( T3DVTResidencyState &rResidency,
        const T3DVTStreamingBuffers &pBuffers,
        uint64_t pUploadBudgetBytesPerFrame,
        T3DVTStreamingState &rState ) {
	memset( &rState, 0, sizeof( rState ) );
	if ( !rResidency.bInitialized || pBuffers.pJobs == NULL || pBuffers.nJobCapacity == 0u || pBuffers.nJobCapacity > 0xffffu
	        || pBuffers.pScratchBytes == NULL || pBuffers.pScratchLaneOwner == NULL || pBuffers.pScratchLaneCompletion == NULL
	        || pBuffers.pPageToJob == NULL || pBuffers.nPageToJobCapacity < rResidency.pDatabase->pHeader->nPageCount
	        || pBuffers.pFreeJobs == NULL || pBuffers.pQueuedJobs == NULL || pBuffers.pJobQueuePosition == NULL
	        || pBuffers.pReadyJobs == NULL || pBuffers.nScratchLaneCount == 0u
	        || pBuffers.nScratchLaneCount > T3D_RW_MAX_STREAM_SCRATCH_LANES || pBuffers.nScratchLaneBytes == 0u
	        || pUploadBudgetBytesPerFrame == 0u ) {
		return false;
	}
	rState.pResidency = &rResidency;
	rState.Buffers = pBuffers;
	rState.nUploadBudgetBytesPerFrame = pUploadBudgetBytesPerFrame;
	for ( uint32_t i = 0u; i < pBuffers.nPageToJobCapacity; ++i ) {
		pBuffers.pPageToJob[i] = s_nInvalidJob16;
	}
	for ( uint32_t i = 0u; i < pBuffers.nJobCapacity; ++i ) {
		memset( &pBuffers.pJobs[i], 0, sizeof( T3DVTStreamJob ) );
		pBuffers.pJobs[i].nScratchLane = T3D_RW_INVALID_ID;
		pBuffers.pJobQueuePosition[i] = s_nInvalidJob16;
		pBuffers.pFreeJobs[i] = ( uint16_t )( pBuffers.nJobCapacity - 1u - i );
	}
	rState.nFreeJobCount = pBuffers.nJobCapacity;
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
			// A WORKING job is immutable worker input until its scratch-lane
			// completion is owner-consumed. Only queued jobs may be reprioritized.
			if ( Job.nState == T3D_VT_STREAM_QUEUED ) {
				t3d_rw_stream_queue_remove( rState, nJob );
				if ( Request.nPriority > Job.nPriority ) {
					Job.nPriority = Request.nPriority;
				}
				if ( Request.nWantedMip < Job.nWantedMip ) {
					Job.nWantedMip = Request.nWantedMip;
				}
				t3d_rw_stream_queue_insert( rState, nJob );
			}
			continue;
		}
		nJob = t3d_rw_stream_pop_free_job( rState );
		if ( nJob == T3D_RW_INVALID_ID ) {
			nJob = t3d_rw_stream_find_replaceable( rState, Request.nPriority );
			if ( nJob == T3D_RW_INVALID_ID ) {
				++rState.Stats.nDroppedRequestCount;
				continue;
			}
			t3d_rw_stream_free_job( rState, nJob );
			nJob = t3d_rw_stream_pop_free_job( rState );
			++rState.Stats.nReplacedQueuedJobCount;
		}
		++rState.Stats.nActiveJobCount;
		if ( rState.Stats.nActiveJobCount > rState.Stats.nQueueHighWater ) {
			rState.Stats.nQueueHighWater = rState.Stats.nActiveJobCount;
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
		rState.Buffers.pPageToJob[Request.nVirtualPage] = ( uint16_t )nJob;
		t3d_rw_stream_queue_insert( rState, nJob );
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
	if ( !rState.bInitialized || rState.nQueuedJobCount == 0u ) {
		return false;
	}
	const uint32_t nLane = t3d_rw_stream_find_free_lane( rState );
	if ( nLane == T3D_RW_INVALID_ID ) {
		return false;
	}
	const uint32_t nJob = rState.Buffers.pQueuedJobs[0];
	t3d_rw_stream_queue_remove( rState, nJob );
	T3DVTStreamJob &Job = rState.Buffers.pJobs[nJob];
	Job.nState = T3D_VT_STREAM_WORKING;
	Job.nScratchLane = nLane;
	rState.Buffers.pScratchLaneOwner[nLane] = nJob;
	rState.Buffers.pScratchLaneCompletion[nLane].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
	++rState.Stats.nClaimedCount;
	rJobIndex = nJob;
	return true;
}

bool t3d_render_world_vt_streaming_cancel_claim_owner_hot( T3DVTStreamingState &rState, uint32_t pJobIndex ) {
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
	Job.nScratchLane = T3D_RW_INVALID_ID;
	Job.nState = T3D_VT_STREAM_QUEUED;
	t3d_rw_stream_queue_insert( rState, pJobIndex );
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
	for ( uint32_t lane = 0u; lane < rState.Buffers.nScratchLaneCount; ++lane ) {
		const uint32_t nJob = rState.Buffers.pScratchLaneOwner[lane];
		if ( nJob == T3D_RW_INVALID_ID || nJob >= rState.Buffers.nJobCapacity ) {
			continue;
		}
		T3DVTStreamJob &Job = rState.Buffers.pJobs[nJob];
		if ( Job.nState != T3D_VT_STREAM_WORKING || Job.nScratchLane != lane ) {
			continue;
		}
		const uint32_t nCompletion = rState.Buffers.pScratchLaneCompletion[lane].load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( nCompletion == T3D_VT_STREAM_COMPLETION_IDLE ) {
			continue;
		}
		rState.Buffers.pScratchLaneCompletion[lane].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
		Job.nState = nCompletion == T3D_VT_STREAM_COMPLETION_READY ? T3D_VT_STREAM_READY : T3D_VT_STREAM_FAILED;
		if ( Job.nState == T3D_VT_STREAM_READY ) {
			++rState.Stats.nCompletedCount;
		} else {
			++rState.Stats.nDecodeFailureCount;
		}
		if ( rState.nReadyJobCount < rState.Buffers.nJobCapacity ) {
			rState.Buffers.pReadyJobs[rState.nReadyJobCount++] = ( uint16_t )nJob;
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
	uint32_t i = 0u;
	while ( i < rState.nReadyJobCount ) {
		const uint32_t nJob = rState.Buffers.pReadyJobs[i];
		T3DVTStreamJob &Job = rState.Buffers.pJobs[nJob];
		if ( Job.nState == T3D_VT_STREAM_FAILED ) {
			t3d_rw_stream_ready_remove( rState, i );
			t3d_rw_stream_free_job( rState, nJob );
			continue;
		}
		if ( Job.nState != T3D_VT_STREAM_READY ) {
			t3d_rw_stream_ready_remove( rState, i );
			continue;
		}
		if ( Job.nDatabaseGeneration != rState.pResidency->nDatabaseGeneration
		        || Job.nVirtualPage >= rState.pResidency->pDatabase->pHeader->nPageCount
		        || Job.nPageGeneration != rState.pResidency->Buffers.pPageTable[Job.nVirtualPage].nGeneration
		        || ( rState.pResidency->Buffers.pPageTable[Job.nVirtualPage].nFlags & T3D_VT_PAGE_RESIDENT ) != 0u ) {
			++rState.Stats.nStaleCompletionCount;
			t3d_rw_stream_ready_remove( rState, i );
			t3d_rw_stream_free_job( rState, nJob );
			continue;
		}
		if ( Job.nDecodedBytes > rState.nUploadBudgetBytesPerFrame - nUploaded ) {
			++rState.Stats.nBudgetDeferredCount;
			++i;
			continue;
		}
		T3DVTUploadReservation Reservation = {};
		if ( !t3d_render_world_vt_reserve_upload_hot( *rState.pResidency, Job.nVirtualPage, Job.nWantedMip, pFrameId, Reservation ) ) {
			++i;
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
		t3d_rw_stream_ready_remove( rState, i );
		t3d_rw_stream_free_job( rState, nJob );
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
