#include "diagnostics/tests/systems/test_harness.h"
#include "streaming/residency/terrain_3d_residency_plan.h"
#include "rendering/rd/resources/terrain_3d_rd_texture_manager.h"
#include "streaming/gpu/terrain_3d_gpu_page_store.h"
#include <stdint.h>

namespace terrain3d_tests
{
static t3dGpuPageTicket_t make_ticket(
	uint16_t region,
	uint8_t	 channel,
	uint32_t deviceEpoch = 7u,
	uint32_t sourceRevision = 11u ) {
	t3dGpuPageTicket_t ticket{};
	ticket.m_key = { region, channel, 0u };
	ticket.m_nDeviceEpoch = deviceEpoch;
	ticket.m_nSourceGeneration = sourceRevision;
	ticket.m_nEditGeneration = 1u;
	ticket.m_nSlotGeneration = 1u;
	ticket.m_nPublicationGeneration = 1u;
	return ticket;
}
void register_gpu_single_copy_tests(
	TestRunner &runner ) {
	runner.run( "gpu_single_copy", "versioned_identity_rejects_stale_device_and_edit", []( TestContext &test ) {
		Terrain3DResidencyPlan plan;
		plan.reset( 7u );
		plan.setEnabled( true );
		auto ticket = make_ticket( 0, T3D_GPU_PAGE_HEIGHT );
		T3D_REQUIRE( test, plan.request( ticket, T3D_GPU_PAGE_PRIORITY_REQUIRED_HEIGHT, 0u ) );
		t3dGpuPageTicket_t popped;
		T3D_REQUIRE( test, plan.dequeue( popped ) );
		T3D_CHECK( test, t3dGpuPageTicketCurrent( popped, ticket ) );
		auto stale = popped;
		++stale.m_nEditGeneration;
		T3D_CHECK( test, !plan.complete( stale, true, 1u ) );
		T3D_CHECK( test, plan.complete( popped, true, 1u ) );
		plan.reset( 8u );
		T3D_CHECK( test, !plan.current( popped ) );
		T3D_CHECK( test, !plan.request( ticket, T3D_GPU_PAGE_PRIORITY_NEAR, 1u ) );
	} );
	runner.run( "gpu_single_copy", "priority_overflow_and_retry_are_bounded", []( TestContext &test ) {
		Terrain3DResidencyPlan plan;
		plan.reset( 7u );
		plan.setEnabled( true );
		for ( uint32_t i = 0; i < Terrain3DResidencyPlan::QUEUE_CAPACITY; ++i ) {
			T3D_REQUIRE( test,
						 plan.request( make_ticket( ( uint16_t ) i, T3D_GPU_PAGE_HEIGHT ),
									   T3D_GPU_PAGE_PRIORITY_PREFETCH,
									   0u ) );
		}
		T3D_CHECK( test, !plan.request( make_ticket( 600, T3D_GPU_PAGE_HEIGHT ), T3D_GPU_PAGE_PRIORITY_NEAR, 0u ) );
		T3D_CHECK( test, plan.dropped() == 1u && plan.queued() == Terrain3DResidencyPlan::QUEUE_CAPACITY );
		t3dGpuPageTicket_t ticket;
		T3D_REQUIRE( test, plan.dequeue( ticket ) );
		T3D_CHECK( test, plan.complete( ticket, false, 4u ) );
		T3D_CHECK( test, !plan.request( ticket, T3D_GPU_PAGE_PRIORITY_NEAR, 5u ) );
		T3D_CHECK( test, plan.request( ticket, T3D_GPU_PAGE_PRIORITY_NEAR, 64u ) );
	} );
	runner.run( "gpu_single_copy", "channel_priorities_and_logical_bounds", []( TestContext &test ) {
		Terrain3DResidencyPlan plan;
		plan.reset( 7u );
		plan.setEnabled( true );
		const auto color = make_ticket( 1023u, T3D_GPU_PAGE_COLOR );
		const auto height = make_ticket( 0u, T3D_GPU_PAGE_HEIGHT );
		T3D_CHECK( test, plan.request( color, T3D_GPU_PAGE_PRIORITY_PREFETCH, 0u ) );
		T3D_CHECK( test, plan.request( height, T3D_GPU_PAGE_PRIORITY_REQUIRED_HEIGHT, 0u ) );
		t3dGpuPageTicket_t picked;
		T3D_CHECK( test, plan.dequeue( picked ) && picked.m_key.m_nChannel == T3D_GPU_PAGE_HEIGHT );
		T3D_CHECK( test, !plan.request( make_ticket( 1024u, T3D_GPU_PAGE_HEIGHT ), T3D_GPU_PAGE_PRIORITY_NEAR, 0u ) );
		T3D_CHECK( test,
				   !plan.request( make_ticket( 0, T3D_GPU_PAGE_CHANNEL_COUNT ), T3D_GPU_PAGE_PRIORITY_NEAR, 0u ) );
		T3D_CHECK( test, !t3dGpuPageCanRetire( 17u, 16u ) && t3dGpuPageCanRetire( 17u, 17u ) );
	} );
	runner.run( "gpu_single_copy", "raw_layout_bytes_and_usage_without_image", []( TestContext &test ) {
		t3dRdTextureLayout_t height, control, color;
		T3D_REQUIRE( test,
					 Terrain3DRdTextureManager::buildRawPageLayout( RenderingDevice::DATA_FORMAT_R16_UNORM,
																	128,
																	128,
																	1,
																	1,
																	false,
																	height ) );
		T3D_REQUIRE( test,
					 Terrain3DRdTextureManager::buildRawPageLayout( RenderingDevice::DATA_FORMAT_R32_SFLOAT,
																	128,
																	128,
																	2,
																	1,
																	true,
																	control ) );
		T3D_REQUIRE( test,
					 Terrain3DRdTextureManager::buildRawPageLayout( RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
																	128,
																	128,
																	1,
																	8,
																	true,
																	color ) );
		T3D_CHECK( test, height.m_nBytes == 32768u && control.m_nBytes == 131072u );
		T3D_CHECK( test, color.m_nBytes == 87380u );
		T3D_CHECK( test, ( height.m_nUsageBits & RenderingDevice::TEXTURE_USAGE_STORAGE_BIT ) == 0u );
		T3D_CHECK( test, ( control.m_nUsageBits & RenderingDevice::TEXTURE_USAGE_STORAGE_BIT ) != 0u );
		T3D_CHECK( test,
				   !Terrain3DRdTextureManager::buildRawPageLayout( RenderingDevice::DATA_FORMAT_BC7_UNORM_BLOCK,
																   128,
																   128,
																   1,
																   1,
																   true,
																   height ) );
	} );
}
} // namespace terrain3d_tests

namespace terrain3d_tests
{
void register_gpu_page_reduction_tests(
	TestRunner &runner ) {
	runner.run( "gpu_single_copy", "independent_channel_pool_budgets", []( TestContext &test ) {
		Terrain3DGpuPageStore::PoolConfig config;
		const RenderingDevice::DataFormat formats[T3D_GPU_PAGE_CHANNEL_COUNT] = {
			RenderingDevice::DATA_FORMAT_R16_UINT,
			RenderingDevice::DATA_FORMAT_R32_UINT,
			RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
			RenderingDevice::DATA_FORMAT_R8G8_UNORM,
			RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
			RenderingDevice::DATA_FORMAT_R8_UNORM };
		const uint32_t layers[T3D_GPU_PAGE_CHANNEL_COUNT] = { 20u, 14u, 14u, 7u, 5u, 3u };
		const uint32_t mipmaps[T3D_GPU_PAGE_CHANNEL_COUNT] = { 1u, 1u, 8u, 1u, 8u, 8u };
		uint64_t	   total = 0u;
		for ( uint32_t c = 0u; c < T3D_GPU_PAGE_CHANNEL_COUNT; ++c ) {
			auto &pool = config.channels[c];
			pool.format = formats[c];
			pool.width = 128u;
			pool.height = 128u;
			pool.layers = layers[c];
			pool.mipmaps = mipmaps[c];
			t3dRdTextureLayout_t layout;
			T3D_REQUIRE( test,
						 Terrain3DRdTextureManager::buildRawPageLayout( pool.format,
																		pool.width,
																		pool.height,
																		pool.layers,
																		pool.mipmaps,
																		true,
																		layout ) );
			pool.maximumBytes = layout.m_nBytes;
			total += layout.m_nBytes;
		}
		static Terrain3DGpuPageStore store;
		T3D_CHECK( test, !store.configure( config, total - 1u ) );
		T3D_REQUIRE( test, store.configure( config, total ) );
		T3D_CHECK( test, store.physicalBytes() == total );
		T3D_CHECK( test,
				   store.pool( T3D_GPU_PAGE_HEIGHT ).allocatedBytes() !=
					   store.pool( T3D_GPU_PAGE_CONTROL ).allocatedBytes() );
		T3D_CHECK( test, !store.active() );
	} );
	runner.run( "gpu_single_copy", "channel_directory_binary_layout", []( TestContext &test ) {
		T3D_CHECK( test, sizeof( t3dGpuChannelDirectoryEntry_t ) == 16u );
		T3D_CHECK( test,
				   Terrain3DGpuPageStore::DIRECTORY_BYTES == T3D_REGION_SLOTS * T3D_GPU_PAGE_CHANNEL_COUNT * 16u );
		t3dGpuChannelDirectoryEntry_t absent;
		T3D_CHECK( test, absent.physicalLayer == UINT32_MAX );
		T3D_CHECK( test, absent.slotGeneration == 0u );
	} );
}
} // namespace terrain3d_tests

#include "streaming/gpu/terrain_3d_gpu_content_directory.h"
#include "streaming/gpu/terrain_3d_gpu_readback_service.h"
namespace terrain3d_tests
{
void register_gpu_page_upload_tests(
	TestRunner &runner ) {
	runner.run( "gpu_single_copy", "fingerprint_uses_complete_mips_and_recipe", []( TestContext &test ) {
		Terrain3DGpuPagePool::Configuration config;
		config.format = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
		config.width = 4u;
		config.height = 4u;
		config.mipmaps = 3u;
		PackedByteArray a, b;
		a.resize( 84 );
		b.resize( 84 );
		memset( a.ptrw(), 3, 84 );
		memset( b.ptrw(), 3, 84 );
		b.ptrw()[83] ^= 1u; // last mip: not visible in base level
		const auto keyA = Terrain3DGpuContentDirectory::fingerprint( T3D_GPU_PAGE_COLOR, config, a );
		const auto keyB = Terrain3DGpuContentDirectory::fingerprint( T3D_GPU_PAGE_COLOR, config, b );
		T3D_CHECK( test, keyA.m_nContentHash != keyB.m_nContentHash );
		T3D_CHECK( test, !Terrain3DGpuContentDirectory::equalBytes( a, b, 84u ) );
		config.mipmaps = 2u;
		const auto keyC = Terrain3DGpuContentDirectory::fingerprint( T3D_GPU_PAGE_COLOR, config, a );
		T3D_CHECK( test, keyA.m_nFormatRecipe != keyC.m_nFormatRecipe );
	} );
	runner.run( "gpu_single_copy", "forced_collision_must_compare_complete_payload", []( TestContext &test ) {
		PackedByteArray a, b;
		a.resize( 32768 );
		b.resize( 32768 );
		memset( a.ptrw(), 0xab, 32768 );
		memset( b.ptrw(), 0xab, 32768 );
		b.ptrw()[32767] ^= 1u;
		// Simulated deliberately identical 64-bit hashes must still reject alias.
		t3dGpuPageContentKey_t candidate{ 0xdeadbeefULL, 12u, 32768u };
		t3dGpuPageContentKey_t original{ 0xdeadbeefULL, 12u, 32768u };
		T3D_CHECK( test, candidate.m_nContentHash == original.m_nContentHash );
		T3D_CHECK( test, !Terrain3DGpuContentDirectory::equalBytes( a, b, 32768u ) );
		b.ptrw()[32767] ^= 1u;
		T3D_CHECK( test, Terrain3DGpuContentDirectory::equalBytes( a, b, 32768u ) );
		T3D_CHECK( test, !Terrain3DGpuContentDirectory::equalBytes( a, b, 32767u ) );
	} );
	runner.run( "gpu_single_copy", "readback_bounds_and_invalid_snapshots", []( TestContext &test ) {
		static Terrain3DGpuReadbackService readback;
		static Terrain3DGpuPageStore	   store;
		T3D_CHECK( test, readback.begin( store, 0u, 1u ) == 0u );
		T3D_CHECK( test, readback.begin( store, 1024u, 1u ) == 0u );
		T3D_CHECK( test, readback.pendingCount() == 0u );
		T3D_CHECK( test, readback.stagingBytes() == 0u );
		readback.cancel( 1234u );
	} );
}
} // namespace terrain3d_tests

#include "streaming/gpu/terrain_3d_gpu_edit_journal.h"
#include "streaming/gpu/terrain_3d_gpu_dirty_ledger.h"
#include "streaming/gpu/terrain_3d_gpu_query_bridge.h"
#include "streaming/gpu/terrain_3d_gpu_alias_verifier.h"
namespace terrain3d_tests
{
void register_gpu_page_grouped_cull_tests(
	TestRunner &runner ) {
	runner.run( "gpu_single_copy", "gpu_alias_result_requires_full_equality", []( TestContext &test ) {
		Terrain3DGpuAliasVerifier verifier;
		T3D_CHECK( test, verifier.state() == Terrain3DGpuAliasVerifier::IDLE );
		T3D_CHECK( test, verifier.nextRequestToken() == 0u );
		verifier.abandonDevice();
		T3D_CHECK( test, verifier.state() == Terrain3DGpuAliasVerifier::IDLE );
		PackedByteArray result;
		T3D_CHECK( test, !Terrain3DPageCompute::comparisonEqual( result ) );
		result.resize( 8 );
		memset( result.ptrw(), 0, 8 );
		T3D_CHECK( test, Terrain3DGpuAliasVerifier::validResult( result ) );
		T3D_CHECK( test, Terrain3DPageCompute::comparisonEqual( result ) );
		result.ptrw()[7] = 1u;
		T3D_CHECK( test, !Terrain3DPageCompute::comparisonEqual( result ) );
		result.ptrw()[7] = 0u;
		result.ptrw()[0] = 255u; // hash field cannot override equality
		T3D_CHECK( test, Terrain3DPageCompute::comparisonEqual( result ) );
		result.resize( 7 );
		T3D_CHECK( test, !Terrain3DGpuAliasVerifier::validResult( result ) );
		T3D_CHECK( test, !Terrain3DPageCompute::comparisonEqual( result ) );
	} );

	runner.run( "gpu_single_copy", "bounded_edit_rect_and_channel_invalidation", []( TestContext &test ) {
		T3D_CHECK( test, Terrain3DGpuEditJournal::validRect( 128, 128, 0, 0, 128, 128 ) );
		T3D_CHECK( test, Terrain3DGpuEditJournal::validRect( 128, 128, 127, 127, 1, 1 ) );
		T3D_CHECK( test, !Terrain3DGpuEditJournal::validRect( 128, 128, 127, 127, 2, 1 ) );
		T3D_CHECK( test, !Terrain3DGpuEditJournal::validRect( 256, 256, 0, 0, 1, 1 ) );
		T3D_CHECK( test, !Terrain3DGpuEditJournal::validRect( 128, 128, 0, 0, 0, 1 ) );
		const uint8_t heightMask = Terrain3DGpuDirtyLedger::maskFor( T3D_GPU_PAGE_HEIGHT );
		T3D_CHECK( test, ( heightMask & Terrain3DGpuDirtyLedger::CPU_COLLISION ) != 0u );
		T3D_CHECK( test, ( heightMask & Terrain3DGpuDirtyLedger::MIDDLE_HEIGHT ) != 0u );
		T3D_CHECK(
			test,
			( Terrain3DGpuDirtyLedger::maskFor( T3D_GPU_PAGE_COLOR ) & Terrain3DGpuDirtyLedger::CPU_COLLISION ) == 0u );
	} );
	runner.run( "gpu_single_copy", "dirty_ledger_rejects_stale_completion", []( TestContext &test ) {
		static Terrain3DGpuDirtyLedger ledger;
		ledger.reset();
		auto source = make_ticket( 13, T3D_GPU_PAGE_HEIGHT );
		T3D_REQUIRE( test, ledger.invalidate( source ) );
		T3D_CHECK( test, ledger.pending( 13 ) != 0u );
		uint16_t		   region = UINT16_MAX;
		t3dGpuPageTicket_t captured;
		T3D_REQUIRE( test, ledger.next( Terrain3DGpuDirtyLedger::DERIVED, region, captured ) );
		T3D_CHECK( test, region == 13 && t3dGpuPageTicketCurrent( source, captured ) );
		auto latest = source;
		latest.m_nEditGeneration++;
		T3D_REQUIRE( test, ledger.invalidate( latest ) );
		T3D_CHECK( test, !ledger.acknowledge( Terrain3DGpuDirtyLedger::DERIVED, source ) );
		T3D_CHECK( test, ledger.acknowledge( Terrain3DGpuDirtyLedger::DERIVED, latest ) );
		T3D_CHECK( test, ( ledger.pending( 13 ) & Terrain3DGpuDirtyLedger::DERIVED ) == 0u );
		T3D_CHECK( test, ( ledger.pending( 13 ) & Terrain3DGpuDirtyLedger::CPU_COLLISION ) != 0u );
	} );
	runner.run( "gpu_single_copy", "query_bridge_requires_complete_r16", []( TestContext &test ) {
		PackedByteArray shortBytes;
		shortBytes.resize( 8 );
		T3D_CHECK( test, !Terrain3DGpuQueryBridge::validPayload( shortBytes ) );
		PackedByteArray full;
		full.resize( 128 * 128 * 2 );
		T3D_CHECK( test, Terrain3DGpuQueryBridge::validPayload( full ) );
		Terrain3DGpuQueryBridge bridge( nullptr );
		T3D_CHECK( test, !bridge.acceptClean( make_ticket( 0, T3D_GPU_PAGE_HEIGHT ), Vector2i(), full ) );
	} );
}
} // namespace terrain3d_tests

#include "streaming/gpu/terrain_3d_gpu_derived_generator.h"
namespace terrain3d_tests
{
void register_gpu_page_scan_tests(
	TestRunner &runner ) {
	runner.run( "gpu_single_copy", "multichannel_dirty_tickets_do_not_overwrite", []( TestContext &test ) {
		static Terrain3DGpuDirtyLedger ledger;
		ledger.reset();
		auto height = make_ticket( 17u, T3D_GPU_PAGE_HEIGHT );
		auto color = make_ticket( 17u, T3D_GPU_PAGE_COLOR );
		auto control = make_ticket( 17u, T3D_GPU_PAGE_CONTROL );
		T3D_REQUIRE( test, ledger.invalidate( height ) );
		T3D_REQUIRE( test, ledger.invalidate( color ) );
		T3D_REQUIRE( test, ledger.invalidate( control ) );
		T3D_CHECK( test, ( ledger.pendingFor( 17u, T3D_GPU_PAGE_HEIGHT ) & Terrain3DGpuDirtyLedger::DERIVED ) != 0u );
		T3D_CHECK( test,
				   ( ledger.pendingFor( 17u, T3D_GPU_PAGE_CONTROL ) & Terrain3DGpuDirtyLedger::MATERIAL_VT ) != 0u );
		uint16_t		   region = UINT16_MAX;
		t3dGpuPageTicket_t source;
		T3D_REQUIRE( test, ledger.next( Terrain3DGpuDirtyLedger::COMPOSITE, region, source ) );
		T3D_CHECK( test, t3dGpuPageTicketCurrent( source, height ) );
		T3D_REQUIRE( test, ledger.acknowledge( Terrain3DGpuDirtyLedger::COMPOSITE, height ) );
		T3D_CHECK( test, ( ledger.pendingFor( 17u, T3D_GPU_PAGE_COLOR ) & Terrain3DGpuDirtyLedger::COMPOSITE ) != 0u );
		T3D_REQUIRE( test, ledger.next( Terrain3DGpuDirtyLedger::COMPOSITE, region, source ) );
		T3D_CHECK( test, t3dGpuPageTicketCurrent( source, control ) );
		T3D_CHECK( test, !ledger.acknowledge( Terrain3DGpuDirtyLedger::COMPOSITE, height ) );
		T3D_REQUIRE( test, ledger.acknowledge( Terrain3DGpuDirtyLedger::COMPOSITE, control ) );
		T3D_REQUIRE( test, ledger.next( Terrain3DGpuDirtyLedger::COMPOSITE, region, source ) );
		T3D_CHECK( test, t3dGpuPageTicketCurrent( source, color ) );
		T3D_REQUIRE( test, ledger.acknowledge( Terrain3DGpuDirtyLedger::COMPOSITE, color ) );
		T3D_CHECK( test, !ledger.next( Terrain3DGpuDirtyLedger::COMPOSITE, region, source ) );
		T3D_CHECK( test, ledger.pendingFor( 17u, T3D_GPU_PAGE_HEIGHT ) != 0u );
	} );
	runner.run( "gpu_single_copy", "dirty_ledger_next_from_skips_pending_lower_regions", []( TestContext &test ) {
		static Terrain3DGpuDirtyLedger ledger;
		ledger.reset();
		auto lower = make_ticket( 4u, T3D_GPU_PAGE_HEIGHT );
		auto upper = make_ticket( 7u, T3D_GPU_PAGE_HEIGHT );
		T3D_REQUIRE( test, ledger.invalidate( lower ) );
		T3D_REQUIRE( test, ledger.invalidate( upper ) );
		uint16_t		   region = UINT16_MAX;
		t3dGpuPageTicket_t found{};
		T3D_REQUIRE( test, ledger.nextFrom( Terrain3DGpuDirtyLedger::DERIVED, 5u, region, found ) );
		T3D_CHECK( test, region == 7u );
		T3D_CHECK( test, t3dGpuPageTicketCurrent( found, upper ) );
		T3D_CHECK( test, !ledger.nextFrom( Terrain3DGpuDirtyLedger::DERIVED, T3D_REGION_SLOTS, region, found ) );
	} );
	runner.run( "gpu_single_copy", "dirty_ledger_rejects_older_source_revision", []( TestContext &test ) {
		static Terrain3DGpuDirtyLedger ledger;
		ledger.reset();
		auto newSource = make_ticket( 19u, T3D_GPU_PAGE_HEIGHT );
		newSource.m_nSourceGeneration = 31u;
		auto oldSource = newSource;
		oldSource.m_nSourceGeneration = 30u;
		T3D_REQUIRE( test, ledger.invalidate( newSource ) );
		T3D_CHECK( test, !ledger.invalidate( oldSource ) );
		T3D_CHECK( test, !ledger.invalidateOnly( Terrain3DGpuDirtyLedger::DERIVED, oldSource ) );
		T3D_CHECK( test, ledger.acknowledge( Terrain3DGpuDirtyLedger::DERIVED, newSource ) );
	} );
	runner.run( "gpu_single_copy", "derived_gpu_requires_exact_page_dimensions", []( TestContext &test ) {
		T3D_CHECK( test, Terrain3DGpuDerivedGenerator::validDimensions( 128u, 128u ) );
		T3D_CHECK( test, !Terrain3DGpuDerivedGenerator::validDimensions( 64u, 64u ) );
		T3D_CHECK( test, !Terrain3DGpuDerivedGenerator::validDimensions( 128u, 127u ) );
	} );
	runner.run( "gpu_single_copy", "dirty_ledger_rejects_old_same_channel_generation", []( TestContext &test ) {
		static Terrain3DGpuDirtyLedger ledger;
		ledger.reset();
		auto old = make_ticket( 19u, T3D_GPU_PAGE_HEIGHT );
		auto newer = old;
		++newer.m_nEditGeneration;
		T3D_REQUIRE( test, ledger.invalidate( newer ) );
		T3D_CHECK( test, !ledger.invalidate( old ) );
		T3D_CHECK( test, !ledger.acknowledge( Terrain3DGpuDirtyLedger::DERIVED, old ) );
		T3D_CHECK( test, ledger.acknowledge( Terrain3DGpuDirtyLedger::DERIVED, newer ) );
	} );
}
} // namespace terrain3d_tests
