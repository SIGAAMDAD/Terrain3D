// Package retirement and content invalidation regression tests; no scene required.
#include "diagnostics/tests/unit_testing.h"
#include "assets/resources/terrain_3d_mesh_asset.h"
#include "rendering/rd/vt/streaming/terrain_3d_vt_streaming.h"
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <string.h>

namespace
{
struct StreamingFixture
{
	t3dVTFileHeader_t			header{};
	t3dVTPageFileRecord_t		pages[4]{};
	t3dVTPayloadRecord_t		payloads[4]{};
	t3dVTPageTableEntry_t		table[4]{};
	t3dVTDatabaseView_t			database{};
	t3dVTResidencyState_t		residency{};
	t3dVTStreamJob_t			jobs[4]{};
	uint16_t					pageToJob[4], freeJobs[4], queued[4], positions[4], ready[4], wanted[4], priority[4];
	uint8_t						scratch[1024]{};
	uint32_t					laneOwner[1]{};
	Terrain3DAtomic< uint32_t > completion[1];
	t3dVTStreamingState_t		streaming{};
	Terrain3DAtomic< bool >		entered{ false }, finish{ false };
	bool						initialize() {
		   header.nPageCount = 4;
		   header.nPayloadCount = 4;
		   header.nAllocationCount = 1;
		   header.nPayloadDataBytes = 1024;
		   for ( uint32_t i = 0; i < 4; ++i ) {
			   pages[i].nVirtualPage = i;
			   pages[i].nPayloadId = i;
			   pages[i].nGeneration = 1;
			   payloads[i].nOffset = i * 256;
			   payloads[i].nCompressedBytes = payloads[i].nDecodedBytes = 256;
			   table[i].nGeneration = 1;
			   table[i].nFlags = T3D_VT_PAGE_VALID;
			   table[i].nPhysicalSlot = T3D_VT_INVALID_SLOT;
		   }
		   database.pHeader = &header;
		   database.pPages = pages;
		   database.pPayloads = payloads;
		   residency.pDatabase = &database;
		   residency.bInitialized = true;
		   residency.nDatabaseGeneration = 1;
		   residency.Buffers.pPageTable = table;
		   residency.Buffers.nPageTableCapacity = 4;
		   t3dVTStreamingBuffers_t b{};
		   b.pJobs = jobs;
		   b.nJobCapacity = 4;
		   b.pScratchBytes = scratch;
		   b.nScratchLaneCount = 1;
		   b.nScratchLaneBytes = 1024;
		   b.pScratchLaneOwner = laneOwner;
		   b.pScratchLaneCompletion = completion;
		   b.pPageToJob = pageToJob;
		   b.nPageToJobCapacity = 4;
		   b.pFreeJobs = freeJobs;
		   b.pQueuedJobs = queued;
		   b.pJobQueuePosition = positions;
		   b.pReadyJobs = ready;
		   b.pMergedWantedMip = wanted;
		   b.pMergedPriority = priority;
		   t3dVTStreamingLimits_t limits{};
		   limits.nMaxUploadBytesPerFrame = 1024;
		   limits.nMaxPageSetsPerFrame = 4;
		   limits.nMaxInFlightIoJobs = 1;
		   limits.nMaxAdmittedRequestsPerFrame = 4;
		   return t3d_vt_streaming_initialize_cold( residency, b, limits, streaming );
	}
	static bool decode(
		const t3dVTDatabaseView_t &database,
		const t3dVTStreamJob_t	  &job,
		uint8_t					  *bytes,
		uint32_t,
		uint32_t &decoded,
		void	 *user ) {
		StreamingFixture &f = *( StreamingFixture * ) user;
		f.entered.store( true, T3D_MEMORY_ORDER_RELEASE );
		while ( !f.finish.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
			t3dThreadYield();
		}
		// Read the old package after the owner requested retirement.
		decoded = database.pPayloads[job.nPayloadId].nDecodedBytes;
		memset( bytes, 37, decoded );
		return true;
	}
};
}

bool t3dRunVTWarmRepublishTests() {
	uint32_t   passed = 0, failed = 0;
	const auto expect = [&]( bool value, const char *name ) {
		value ? ++passed : ++failed;
		godot::UtilityFunctions::print( value ? "PASSED: " : "FAILED: ", name );
	};
	StreamingFixture fixture;
	expect( fixture.initialize(), "warm retirement fixture initializes production scheduler" );
	t3dVTRuntimeRequest_t requests[2]{};
	requests[0].nVirtualPage = 0;
	requests[1].nVirtualPage = 1;
	expect( t3d_vt_streaming_ingest_requests_hot( fixture.streaming, requests, 2, 1 ) == 2,
			"old package admits queued work" );
	uint32_t   job = 0;
	const bool claimed = t3d_vt_streaming_claim_job_owner_hot( fixture.streaming, job );
	expect( claimed, "worker claims old package scratch lane" );
	if ( claimed ) {
		Terrain3DThread worker( [&]() {
			t3d_vt_streaming_execute_job_async( fixture.streaming, job, StreamingFixture::decode, &fixture );
		} );
		while ( !fixture.entered.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
			t3dThreadYield();
		}
		expect( !t3d_vt_streaming_quiesce_package_warm( fixture.streaming ),
				"barrier waits for worker still reading old storage" );
		expect( fixture.streaming.nQueuedJobCount == 0 && fixture.streaming.nInFlightIoJobCount == 1,
				"barrier cancels queued demand while retaining active worker" );
		expect( fixture.laneOwner[0] == job && fixture.jobs[job].nState == T3D_VT_STREAM_WORKING,
				"barrier preserves worker identity and scratch ownership" );
		expect( t3d_vt_streaming_admit_request_hot( fixture.streaming, requests[1], 2 ) ==
					T3D_VT_ADMISSION_DEFERRED_FRAME_BUDGET,
				"retiring package cannot admit more work" );
		uint32_t other = 0;
		expect( !t3d_vt_streaming_claim_job_owner_hot( fixture.streaming, other ),
				"retiring package cannot claim another worker" );
		fixture.finish.store( true, T3D_MEMORY_ORDER_RELEASE );
		worker.join();
		expect( t3d_vt_streaming_quiesce_package_warm( fixture.streaming ),
				"completed worker makes package retirement safe" );
		expect( fixture.streaming.nFreeJobCount == 4 && fixture.streaming.nReadyJobCount == 0 &&
					fixture.laneOwner[0] == T3D_VT_INVALID_ID &&
					fixture.completion[0].load() == T3D_VT_STREAM_COMPLETION_IDLE,
				"retirement releases all jobs, mappings and completion lanes" );
		expect( t3d_vt_streaming_quiesce_package_warm( fixture.streaming ) && fixture.streaming.nFreeJobCount == 4,
				"repeated barrier polling does not double-free jobs" );
		t3d_vt_streaming_resume_package_warm( fixture.streaming );
		expect( t3d_vt_streaming_admit_request_hot( fixture.streaming, requests[1], 3 ) ==
					T3D_VT_ADMISSION_ACCEPTED_NEW,
				"aborting retirement resumes last-good package admission" );
	}
	Ref< Terrain3DRenderingMaterial > material;
	material.instantiate();
	uint64_t generation = material->getVtContentGeneration();
	material->setBaseColor( Color( .4, .5, .6 ) );
	material->setRoughness( .3 );
	material->setEmissionEnergy( 2 );
	material->setUvScale( Vector2( 2, 3 ) );
	material->setTextureFilter( 1 );
	material->setTwoSided( true );
	expect( material->getVtContentGeneration() == generation,
			"scalar, UV and pipeline edits do not invalidate VT source bytes" );
	material->setVtStableKey( 171 );
	expect( material->getVtContentGeneration() == ++generation, "VT identity changes invalidate package source" );
	material->setSurfaceTextureLayout( SURFACE_TEXTURE_ROUGHNESS_R );
	expect( material->getVtContentGeneration() == ++generation, "channel packing changes invalidate package source" );
	material->setSamplerClass( T3D_VT_SAMPLER_LINEAR_CLAMP );
	expect( material->getVtContentGeneration() == ++generation, "sampler metadata changes invalidate package source" );
	Ref< Image >			  image = Image::create( 16, 16, false, Image::FORMAT_RGBA8 );
	const Ref< ImageTexture > texture = ImageTexture::create_from_image( image );
	material->setAlbedoTexture( texture );
	expect( material->getVtContentGeneration() == ++generation, "texture replacement invalidates VT source" );
	material->setAlbedoTexture( texture );
	expect( material->getVtContentGeneration() == generation,
			"unchanged texture setter does not invalidate VT source" );
	texture->emit_signal( "changed" );
	expect( material->getVtContentGeneration() == ++generation, "in-place texture edits invalidate VT source" );
	material->setAlphaScissor( true );
	expect( material->getVtContentGeneration() == ++generation,
			"alpha-preserving mip cooking changes invalidate VT source" );
	material->setBaseColor( Color( .4, .5, .6, .5 ) );
	expect( material->getVtContentGeneration() == ++generation,
			"opaque-to-alpha material changes invalidate package flags" );
	godot::UtilityFunctions::print( "=== VT warm republish tests: ", passed, " passed, ", failed, " failed ===" );
	return failed == 0;
}
