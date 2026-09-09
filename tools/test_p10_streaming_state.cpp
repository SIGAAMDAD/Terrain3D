#include "rendering/rd/render_world/terrain_3d_render_world_streaming.h"
#include <assert.h>
#include <string.h>

static bool decode_stub( const T3DVTDatabaseView &, const T3DVTStreamJob &, uint8_t *pScratch, uint32_t pCapacity, uint32_t &rBytes, void * ) {
	if ( pCapacity < 4u ) {
		return false;
	}
	pScratch[0] = 1u;
	pScratch[1] = 2u;
	pScratch[2] = 3u;
	pScratch[3] = 4u;
	rBytes = 4u;
	return true;
}

static bool commit_stub( T3DVTResidencyState &rResidency,
        const T3DVTUploadReservation &rReservation,
        const uint8_t *pDecoded,
        uint32_t pDecodedBytes,
        uint32_t pFrame,
        void * ) {
	return pDecoded != NULL && pDecodedBytes == 4u && t3d_render_world_vt_commit_upload_hot( rResidency, rReservation, pFrame );
}

int main() {
	T3DVTFileHeader header = {};
	header.nPageCount = 2u;
	header.nPayloadCount = 2u;
	T3DVTPageFileRecord pages[2] = {};
	pages[0].nVirtualPage = 0u;
	pages[0].nChannel = 0u;
	pages[0].nGeneration = 1u;
	pages[1].nVirtualPage = 1u;
	pages[1].nChannel = 0u;
	pages[1].nPayloadId = 1u;
	pages[1].nGeneration = 7u;
	T3DVTDatabaseView database = {};
	database.pHeader = &header;
	database.pPages = pages;

	T3DVTPageTableEntry pageTable[2] = {};
	pageTable[0].nPhysicalSlot = 0u;
	pageTable[0].nGeneration = 1u;
	pageTable[0].nFlags = T3D_VT_PAGE_VALID | T3D_VT_PAGE_RESIDENT | T3D_VT_PAGE_PINNED | T3D_VT_PAGE_NEUTRAL;
	pageTable[1].nPhysicalSlot = T3D_RW_VT_INVALID_SLOT;
	pageTable[1].nGeneration = 7u;
	pageTable[1].nFlags = T3D_VT_PAGE_VALID;

	T3DVTPhysicalSlot slots0[3] = {};
	for ( uint32_t i = 0u; i < 3u; ++i ) {
		slots0[i].nVirtualPage = T3D_RW_VT_INVALID_PAGE;
		slots0[i].nPendingVictimPage = T3D_RW_VT_INVALID_PAGE;
		slots0[i].nChannel = 0u;
	}
	slots0[0].nVirtualPage = 0u;
	slots0[0].nGeneration = 1u;
	slots0[0].nFlags = T3D_VT_SLOT_PINNED | T3D_VT_SLOT_REFERENCED | T3D_VT_SLOT_HOT;

	T3DVTResidencyState residency = {};
	residency.pDatabase = &database;
	residency.Buffers.pPageTable = pageTable;
	residency.Buffers.nPageTableCapacity = 2u;
	residency.Buffers.pPhysicalSlots[0u] = slots0;
	residency.Buffers.nPhysicalSlotCapacity[0u] = 3u;
	residency.Stats.nFreeSlotCount[0u] = 2u;
	residency.Stats.nResidentCount[0u] = 1u;
	residency.nDatabaseGeneration = 3u;
	residency.bInitialized = true;
	residency.bNeutralPublished = true;

	T3DVTStreamJob jobs[4] = {};
	uint8_t scratch[2][16] = {};
	uint32_t laneOwner[2] = {};
	t3dAtomic_t<uint32_t> completion[2];
	uint16_t pageToJob[2] = {};
	uint16_t freeJobs[4] = {};
	uint16_t queuedJobs[4] = {};
	uint16_t queuePos[4] = {};
	uint16_t readyJobs[4] = {};
	T3DVTStreamingBuffers buffers = {};
	buffers.pJobs = jobs;
	buffers.nJobCapacity = 4u;
	buffers.pScratchBytes = &scratch[0][0];
	buffers.nScratchLaneCount = 2u;
	buffers.nScratchLaneBytes = 16u;
	buffers.pScratchLaneOwner = laneOwner;
	buffers.pScratchLaneCompletion = completion;
	buffers.pPageToJob = pageToJob;
	buffers.nPageToJobCapacity = 2u;
	buffers.pFreeJobs = freeJobs;
	buffers.pQueuedJobs = queuedJobs;
	buffers.pJobQueuePosition = queuePos;
	buffers.pReadyJobs = readyJobs;

	T3DVTStreamingState streaming = {};
	assert( t3d_render_world_vt_streaming_initialize_cold( residency, buffers, 64u, streaming ) );
	assert( streaming.nFreeJobCount == 4u );

	T3DVTRequest request = { 1u, 0u, 5u };
	assert( t3d_render_world_vt_streaming_ingest_requests_hot( streaming, &request, 1u, 10u ) == 1u );
	assert( streaming.nQueuedJobCount == 1u );
	assert( streaming.Stats.nActiveJobCount == 1u );
	const uint16_t mappedJob = streaming.Buffers.pPageToJob[1];
	assert( mappedJob != 0xffffu );

	request.nPriority = 9u;
	assert( t3d_render_world_vt_streaming_ingest_requests_hot( streaming, &request, 1u, 11u ) == 0u );
	assert( streaming.Buffers.pJobs[mappedJob].nPriority == 9u );

	uint32_t claimed = T3D_RW_INVALID_ID;
	assert( t3d_render_world_vt_streaming_claim_job_owner_hot( streaming, claimed ) );
	assert( claimed == mappedJob );
	assert( streaming.nQueuedJobCount == 0u );
	assert( t3d_render_world_vt_streaming_cancel_claim_owner_hot( streaming, claimed ) );
	assert( streaming.nQueuedJobCount == 1u );
	assert( t3d_render_world_vt_streaming_claim_job_owner_hot( streaming, claimed ) );
	assert( t3d_render_world_vt_streaming_execute_job_async( streaming, claimed, decode_stub, NULL ) );
	assert( t3d_render_world_vt_streaming_service_uploads_hot( streaming, 12u, commit_stub, NULL ) == 4u );
	assert( streaming.Stats.nActiveJobCount == 0u );
	assert( streaming.nFreeJobCount == 4u );
	assert( streaming.nReadyJobCount == 0u );
	assert( streaming.Buffers.pPageToJob[1] == 0xffffu );
	assert( ( pageTable[1].nFlags & T3D_VT_PAGE_RESIDENT ) != 0u );
	return 0;
}
