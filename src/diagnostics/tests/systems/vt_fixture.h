#ifndef TERRAIN3D_TEST_VT_FIXTURE_H
#define TERRAIN3D_TEST_VT_FIXTURE_H

#include "rendering/rd/vt/streaming/terrain_3d_vt_residency.h"
#include "rendering/rd/vt/streaming/terrain_3d_vt_streaming.h"

namespace terrain3d_tests
{
// Tiny normalized residency database: four neutral roots and two demand pages.
// No RenderingDevice or filesystem is required. Sparse V1 packages are tested
// separately with the real cooker and pack reader.
struct ResidencyFixture
{
	t3dVTFileHeader_t		  header{};
	t3dVTAllocationRecordV1_t source_allocation{};
	t3dVTPageFileRecord_t	  pages[6]{};
	t3dVTPayloadRecord_t	  payloads[6]{};
	t3dVTDatabaseView_t		  database{};
	t3dVTAllocation_t		  allocations[1]{};
	t3dVTTailMetadata_t		  tails[1]{};
	t3dVTPageTableEntry_t	  table[6]{};
	t3dVTPhysicalSlot_t		  slots[4]{};
	uint32_t				  free_slots[4]{}, last_evicted[6]{};
	t3dVTRetiredPageSet_t	  retired[4]{};
	t3dVTResidencyBuffers_t	  buffers{};
	t3dVTResidencyState_t	  state{};

	ResidencyFixture() {
		header.nAllocationCount = 1;
		header.nPageCount = 6;
		header.nPayloadCount = 6;
		header.nChannelCount = T3D_VT_CHANNEL_COUNT;
		header.nPayloadDataBytes = 6 * 16;
		source_allocation.nPageCount = 6;
		source_allocation.nMip0WidthPages = 6;
		source_allocation.nMip0HeightPages = 1;
		source_allocation.nMipCount = 1;
		source_allocation.nRootPage = 0;
		for ( uint32_t index = 0; index < 6; ++index ) {
			pages[index].nVirtualPage = index;
			pages[index].nPayloadId = index;
			pages[index].nGeneration = 7;
			pages[index].nParentPage = index < 4 ? T3D_VT_INVALID_PAGE : 0;
			pages[index].nChannel = index < 4 ? static_cast< uint16_t >( index ) : T3D_VT_CHANNEL_ALBEDO;
			pages[index].nFlags = index < 4 ? T3D_VT_PAGE_NEUTRAL | T3D_VT_PAGE_ROOT : 0;
			payloads[index].nOffset = index * 16;
			payloads[index].nCompressedBytes = 16;
			payloads[index].nDecodedBytes = 16;
		}
		database.pHeader = &header;
		database.pAllocations = &source_allocation;
		database.pPages = pages;
		database.pPayloads = payloads;
		buffers.pAllocations = allocations;
		buffers.nAllocationCapacity = 1;
		buffers.pPageTable = table;
		buffers.nPageTableCapacity = 6;
		buffers.pTailMetadata = tails;
		buffers.nTailMetadataCapacity = 1;
		buffers.pPhysicalPageSets = slots;
		buffers.nPhysicalPageSetCapacity = 4;
		buffers.pFreeSlots = free_slots;
		buffers.pRetiredPageSets = retired;
		buffers.pLastEvictedFrame = last_evicted;
	}
	bool initialize() {
		return t3d_vt_initialize_residency_cold( database, buffers, 7, 2, state ) &&
			   t3d_vt_commit_neutral_backing_warm( state, 0 ) && t3d_vt_finalize_neutral_publication_warm( state );
	}
	bool upload(
		uint32_t page,
		uint32_t frame = 10 ) {
		t3dVTUploadReservation_t reservation{};
		return t3d_vt_reserve_upload_hot( state, page, 0, frame, reservation ) &&
			   t3d_vt_commit_upload_hot( state, reservation, T3D_VT_PAGE_SET_CHANNEL_MASK, frame );
	}
};

struct StreamingFixture : ResidencyFixture
{
	t3dVTStreamJob_t			jobs[4]{};
	uint8_t						scratch[128]{};
	uint32_t					lane_owner[2]{};
	Terrain3DAtomic< uint32_t > lane_completion[2];
	uint16_t page_to_job[6]{}, free_jobs[4]{}, queued[4]{}, positions[4]{}, ready[4]{}, merged_mip[4]{},
		merged_priority[4]{};
	t3dVTStreamingBuffers_t stream_buffers{};
	t3dVTStreamingLimits_t	limits{ 64, 2, 2, 2 };
	t3dVTStreamingState_t	streaming{};

	bool initialize() {
		if ( !ResidencyFixture::initialize() ) {
			return false;
		}
		stream_buffers.pJobs = jobs;
		stream_buffers.nJobCapacity = 4;
		stream_buffers.pScratchBytes = scratch;
		stream_buffers.nScratchLaneCount = 2;
		stream_buffers.nScratchLaneBytes = 64;
		stream_buffers.pScratchLaneOwner = lane_owner;
		stream_buffers.pScratchLaneCompletion = lane_completion;
		stream_buffers.pPageToJob = page_to_job;
		stream_buffers.nPageToJobCapacity = 6;
		stream_buffers.pFreeJobs = free_jobs;
		stream_buffers.pQueuedJobs = queued;
		stream_buffers.pJobQueuePosition = positions;
		stream_buffers.pReadyJobs = ready;
		stream_buffers.pMergedWantedMip = merged_mip;
		stream_buffers.pMergedPriority = merged_priority;
		return t3d_vt_streaming_initialize_cold( state, stream_buffers, limits, streaming );
	}
};
} // namespace terrain3d_tests
#endif
