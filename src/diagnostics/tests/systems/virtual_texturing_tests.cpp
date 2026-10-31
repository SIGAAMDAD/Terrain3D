#include "diagnostics/tests/systems/test_harness.h"
#include "diagnostics/tests/systems/vt_fixture.h"
#include "assets/resources/terrain_3d_texture_asset.h"
#include "assets/resources/terrain_3d_packed_pbr_texture.h"
#include "rendering/rd/vt/core/terrain_3d_vt_neutral_payload.h"
#include "rendering/rd/vt/gpu/terrain_3d_vt_feedback_epoch.h"
#include "rendering/rd/vt/planning/terrain_3d_vt_resource_plan.h"
#include <string.h>

namespace terrain3d_tests
{
void register_virtual_texturing_tests(
	TestRunner &runner ) {
	runner.run( "virtual_texturing", "feedback_rejection_preserves_unreset_pair", []( TestContext &test ) {
		for ( int mask = 0; mask < 4; ++mask ) {
			t3dVTFeedbackEpochState_t epoch;
			const bool hash = ( mask & 1 ) != 0;
			const bool counters = ( mask & 2 ) != 0;
			T3D_CHECK( test, epoch.acceptSubmissions( hash, counters ) == ( hash && counters ) );
			T3D_CHECK( test, epoch.canSubmit() == ( !hash && !counters ) );
			T3D_CHECK( test, !epoch.canConsume() );
			if ( hash ) {
				T3D_CHECK( test, epoch.completeHash( 1u ) == ( hash && counters ) );
			}
			if ( counters ) {
				T3D_CHECK( test, epoch.completeCounters( 1u ) == ( hash && counters ) );
			}
			T3D_CHECK( test, epoch.canSubmit() );
			T3D_CHECK( test, epoch.canConsume() == ( hash && counters ) );
			// Successful pair is consumed once. Rejected pair starts a new pair.
			T3D_CHECK( test, !epoch.completeHash( 1u ) && !epoch.completeCounters( 1u ) );
		}
	} );
	runner.run( "virtual_texturing", "feedback_epoch_rejects_stale_callbacks", []( TestContext &test ) {
		t3dVTFeedbackEpochState_t epoch;
		T3D_REQUIRE( test, epoch.acceptSubmissions( true, true ) );
		T3D_CHECK( test, epoch.completeHash( 1u ) );
		T3D_CHECK( test, !epoch.canConsume() );
		epoch.invalidate( 2u );
		T3D_CHECK( test, !epoch.completeCounters( 1u ) );
		T3D_CHECK( test, epoch.canSubmit() && !epoch.canConsume() );
		T3D_REQUIRE( test, epoch.acceptSubmissions( true, true ) );
		T3D_CHECK( test, !epoch.completeHash( 1u ) );
		T3D_CHECK( test, epoch.completeCounters( 2u ) );
		T3D_CHECK( test, epoch.completeHash( 2u ) );
		T3D_CHECK( test, epoch.canConsume() );
	} );

	// P09: runtime-only material identity must survive dropping authoring resources.
	runner.run( "virtual_texturing",
				"runtime_material_view_preserves_identity_and_scalar_settings",
				[]( TestContext &test ) {
					Ref< Terrain3DTextureAsset > authored;
					authored.instantiate();
					authored->setId( 13 );
					authored->setName( "Basalt_Plate" );
					authored->setUvScale( 0.23f );
					authored->setRoughness( 0.41f );
					authored->setErosionEnabled( false );
					authored->setErosionExposedTextureId( 8 );
					Ref< Terrain3DPackedPBRTexture > authoring_pbr;
					authoring_pbr.instantiate();
					authored->setPackedPbrResource( authoring_pbr );
					const Ref< Terrain3DTextureAsset > view = authored->createRuntimeMaterialViewCold();
					T3D_REQUIRE( test, view.is_valid() && view != authored );
					T3D_CHECK( test, view->getId() == 13 && view->getName() == "Basalt_Plate" );
					T3D_CHECK( test, near( view->getUvScale(), 0.23f ) && near( view->getRoughness(), 0.41f ) );
					T3D_CHECK( test, !view->getErosionEnabled() && view->getErosionExposedTextureId() == 8 );
					T3D_CHECK( test, authored->getPackedPbrResource() == authoring_pbr );
					T3D_CHECK( test,
							   view->getPackedPbrResource().is_null() && view->getAlbedoTexture().is_null() &&
								   view->getNormalTexture().is_null() && view->getSurfaceTexture().is_null() &&
								   view->getDisplacementTexture().is_null() );
				} );
	// GPU caps change physical residency without truncating virtual addressing.
	runner.run(
		"virtual_texturing",
		"manual_gpu_cap_selects_the_largest_fitting_physical_cache",
		[]( TestContext &test ) {
			t3dWorldLimits_t limits{};
			limits.nMaxMaterials = 16;
			limits.nMaxVtVirtualPages = 1024;
			limits.nMaxVtPhysicalPageSets = 32;
			const auto			   original = limits;
			const auto			   profile = t3d_vt_make_development_gpu_profile();
			t3dVTGpuResourcePlan_t expected{}, fitted{};
			limits.nMaxVtPhysicalPageSets = 11;
			T3D_REQUIRE( test, t3d_vt_compute_resource_plan( limits, profile, expected, T3D_VT_TAIL_TILE ) );
			limits = original;
			T3D_REQUIRE(
				test,
				t3d_vt_fit_resource_plan_to_budget( limits, profile, expected.nTotalBytes, fitted, T3D_VT_TAIL_TILE ) );
			T3D_CHECK( test, limits.nMaxVtPhysicalPageSets == 11 && fitted.nTotalBytes == expected.nTotalBytes );
			T3D_CHECK( test,
					   limits.nMaxVtVirtualPages == original.nMaxVtVirtualPages &&
						   limits.nMaxMaterials == original.nMaxMaterials );
			T3D_CHECK( test, memcmp( fitted.nBufferBytes, expected.nBufferBytes, sizeof( fitted.nBufferBytes ) ) == 0 );
			T3D_CHECK( test, fitted.nVtTailAtlasHeight == expected.nVtTailAtlasHeight );
		} );
	runner.run( "virtual_texturing",
				"gpu_cap_below_mandatory_storage_leaves_plan_and_limits_untouched",
				[]( TestContext &test ) {
					t3dWorldLimits_t limits{};
					limits.nMaxMaterials = 8;
					limits.nMaxVtVirtualPages = 128;
					limits.nMaxVtPhysicalPageSets = 3;
					const auto			   profile = t3d_vt_make_development_gpu_profile();
					t3dVTGpuResourcePlan_t minimum{}, unchanged{};
					T3D_REQUIRE( test, t3d_vt_compute_resource_plan( limits, profile, minimum, T3D_VT_TAIL_TILE ) );
					limits.nMaxVtPhysicalPageSets = 16;
					unchanged.nTotalBytes = 123;
					T3D_CHECK( test,
							   !t3d_vt_fit_resource_plan_to_budget( limits,
																	profile,
																	minimum.nTotalBytes - 1,
																	unchanged,
																	T3D_VT_TAIL_TILE ) );
					T3D_CHECK( test, limits.nMaxVtPhysicalPageSets == 16 && unchanged.nTotalBytes == 123 );
					T3D_CHECK( test,
							   t3d_vt_fit_resource_plan_to_budget( limits,
																   profile,
																   minimum.nTotalBytes,
																   unchanged,
																   T3D_VT_TAIL_TILE ) );
					T3D_CHECK( test,
							   limits.nMaxVtPhysicalPageSets == 3 && unchanged.nTotalBytes == minimum.nTotalBytes );
				} );
	runner.run( "virtual_texturing", "large_gpu_cap_preserves_the_existing_physical_cache", []( TestContext &test ) {
		t3dWorldLimits_t limits{};
		limits.nMaxMaterials = 8;
		limits.nMaxVtVirtualPages = 128;
		limits.nMaxVtPhysicalPageSets = 16;
		t3dVTGpuResourcePlan_t original{}, fitted{};
		const auto			   profile = t3d_vt_make_development_gpu_profile();
		T3D_REQUIRE( test, t3d_vt_compute_resource_plan( limits, profile, original ) );
		T3D_REQUIRE( test, t3d_vt_fit_resource_plan_to_budget( limits, profile, original.nTotalBytes, fitted ) );
		T3D_CHECK( test, limits.nMaxVtPhysicalPageSets == 16 && fitted.nTotalBytes == original.nTotalBytes );
	} );
	// Addressing and capacity planning.
	runner.run( "virtual_texturing", "physical_address_roundtrips_every_format_family", []( TestContext &test ) {
		for ( uint32_t family = 0; family < T3D_VT_PHYSICAL_FORMAT_COUNT; ++family ) {
			for ( const uint32_t slot : { 0u, 1u, 1023u, 65535u } ) {
				const uint32_t address =
					t3d_vt_pack_physical_address( static_cast< t3dVTPhysicalFormatFamily_t >( family ), slot );
				T3D_CHECK( test, static_cast< uint32_t >( t3d_vt_physical_address_family( address ) ) == family );
				T3D_CHECK( test, t3d_vt_physical_address_slot( address ) == slot );
			}
		}
	} );
	runner.run( "virtual_texturing", "invalid_physical_addresses_are_rejected", []( TestContext &test ) {
		T3D_CHECK(
			test,
			t3d_vt_pack_physical_address( static_cast< t3dVTPhysicalFormatFamily_t >( T3D_VT_PHYSICAL_FORMAT_COUNT ),
											 0 ) == T3D_VT_INVALID_SLOT );
		T3D_CHECK( test,
				   t3d_vt_pack_physical_address( T3D_VT_PHYSICAL_FORMAT_BC1_UNORM,
													T3D_VT_PHYSICAL_SLOT_MASK + 1 ) == T3D_VT_INVALID_SLOT );
	} );
	runner.run( "virtual_texturing",
				"single_bank_accepts_exact_capacity_and_rejects_overflow",
				[]( TestContext &test ) {
					t3dVTBankLayout_t layout{};
					T3D_CHECK( test,
							   t3d_vt_compute_bank_layout( 256, 256, layout ) && layout.nBankCount == 1 &&
								   layout.nLayers[0] == 256 );
					T3D_CHECK( test, !t3d_vt_compute_bank_layout( 257, 256, layout ) );
					T3D_CHECK( test,
							   !t3d_vt_compute_bank_layout( 0, 256, layout ) &&
								   !t3d_vt_compute_bank_layout( 1, 0, layout ) );
				} );
	runner.run( "virtual_texturing", "gpu_profile_rejects_incompatible_table_record_sizes", []( TestContext &test ) {
		auto profile = t3d_vt_make_development_gpu_profile();
		T3D_CHECK( test, t3d_vt_validate_gpu_profile( profile ) );
		profile.nVtPageTableEntryBytes++;
		T3D_CHECK( test, !t3d_vt_validate_gpu_profile( profile ) );
	} );
	runner.run( "virtual_texturing", "active_tail_plan_preserves_tables_and_physical_cache", []( TestContext &test ) {
		t3dWorldLimits_t limits{};
		limits.nMaxMaterials = 16;
		limits.nMaxVtVirtualPages = 1024;
		limits.nMaxVtPhysicalPageSets = 32;
		t3dVTGpuResourcePlan_t full{}, active{};
		const auto			   profile = t3d_vt_make_development_gpu_profile();
		T3D_REQUIRE( test, t3d_vt_compute_resource_plan( limits, profile, full ) );
		T3D_REQUIRE( test, t3d_vt_compute_resource_plan( limits, profile, active, T3D_VT_TAIL_TILE ) );
		T3D_CHECK( test, memcmp( full.nBufferBytes, active.nBufferBytes, sizeof( full.nBufferBytes ) ) == 0 );
		T3D_CHECK( test,
				   memcmp( full.nVtPhysicalBytes, active.nVtPhysicalBytes, sizeof( full.nVtPhysicalBytes ) ) == 0 );
		T3D_CHECK( test,
				   active.nTotalBytes < full.nTotalBytes &&
					   active.nTotalBytes == active.nTotalBufferBytes + active.nTotalTextureBytes );
	} );
	runner.run( "virtual_texturing", "disabled_ancestor_hints_do_not_block_startup_resource_plan", []( TestContext &test ) {
		t3dWorldLimits_t limits = t3d_world_make_prototype_limits();
		t3dVTGpuResourcePlan_t plan{};
		T3D_REQUIRE( test, t3d_vt_compute_resource_plan( limits, t3d_vt_make_development_gpu_profile(), plan, 64u ) );
		T3D_CHECK( test, plan.nVtTailAtlasHeight == 64u );
		uint64_t nBufferBytes = 0u;
		for ( uint32_t iBuffer = 0u; iBuffer < T3D_VT_GPU_BUFFER_COUNT; ++iBuffer ) {
#if !defined( T3D_ENABLE_VT_RESIDENT_ANCESTOR_HINTS ) || !T3D_ENABLE_VT_RESIDENT_ANCESTOR_HINTS
			if ( iBuffer == T3D_VT_GPU_BUFFER_ANCESTOR_HINTS ) {
				T3D_CHECK( test, plan.nBufferBytes[iBuffer] == 0u );
				continue;
			}
#endif
			T3D_CHECK( test, plan.nBufferBytes[iBuffer] != 0u );
			nBufferBytes += plan.nBufferBytes[iBuffer];
		}
		T3D_CHECK( test, nBufferBytes == plan.nTotalBufferBytes );
		T3D_CHECK( test, plan.nTotalBytes == plan.nTotalBufferBytes + plan.nTotalTextureBytes );
	} );
	runner.run( "virtual_texturing", "zero_resource_limits_are_rejected", []( TestContext &test ) {
		t3dWorldLimits_t	   limits{};
		t3dVTGpuResourcePlan_t plan{};
		T3D_CHECK( test, !t3d_vt_compute_resource_plan( limits, t3d_vt_make_development_gpu_profile(), plan ) );
	} );
	runner.run( "virtual_texturing", "tail_high_water_includes_the_entire_last_slot", []( TestContext &test ) {
		t3dVTTailMetadata_t tails[3]{};
		tails[1].m_nAtlasY = T3D_VT_PACK_TAIL_SLOT_HEIGHT * 2;
		uint32_t height = 0;
		T3D_CHECK( test, t3d_vt_compute_active_tail_height_cold( tails, 3, height ) );
		T3D_CHECK( test, height == T3D_VT_PACK_TAIL_SLOT_HEIGHT * 3 );
	} );
	runner.run( "virtual_texturing", "invalid_tail_metadata_cannot_overflow_the_atlas", []( TestContext &test ) {
		t3dVTTailMetadata_t tail{};
		uint32_t			height = 999;
		T3D_CHECK( test, !t3d_vt_compute_active_tail_height_cold( nullptr, 1, height ) && height == 0 );
		T3D_CHECK( test, !t3d_vt_compute_active_tail_height_cold( &tail, 0, height ) );
		tail.m_nAtlasY = 1;
		T3D_CHECK( test, !t3d_vt_compute_active_tail_height_cold( &tail, 1, height ) );
		tail.m_nAtlasY = UINT32_MAX - T3D_VT_PACK_TAIL_SLOT_HEIGHT + 1;
		T3D_CHECK( test, !t3d_vt_compute_active_tail_height_cold( &tail, 1, height ) );
	} );
	runner.run( "virtual_texturing",
				"neutral_payload_repetition_preserves_partial_blocks_and_guards",
				[]( TestContext &test ) {
					const uint8_t pattern[4] = { 7, 11, 19, 23 };
					uint8_t		  buffer[21];
					memset( buffer, 0xa5, sizeof( buffer ) );
					t3d_vt_repeat_neutral_payload_cold( buffer + 1, 19, pattern, 4 );
					T3D_CHECK( test, buffer[0] == 0xa5 && buffer[20] == 0xa5 );
					for ( size_t index = 0; index < 19; ++index ) {
						T3D_CHECK( test, buffer[index + 1] == pattern[index % 4] );
					}
				} );
	// Residency publication, failed uploads and delayed retirement.
	runner.run( "virtual_texturing", "neutral_roots_share_one_pinned_physical_slot", []( TestContext &test ) {
		ResidencyFixture fixture;
		T3D_REQUIRE( test, fixture.initialize() );
		for ( uint32_t channel = 0; channel < 4; ++channel ) {
			T3D_CHECK( test,
					   fixture.table[channel].nPhysicalSlot == 0 &&
						   ( fixture.table[channel].nFlags & T3D_VT_PAGE_PINNED ) != 0 );
		}
		T3D_CHECK( test, fixture.state.Stats.nResidentCount == 1 && fixture.state.Stats.nFreeSlotCount == 3 );
		T3D_CHECK( test, t3d_vt_validate_residency_reference_debug( fixture.state ) );
	} );
	runner.run( "virtual_texturing", "missing_physical_storage_rejects_initialization", []( TestContext &test ) {
		ResidencyFixture fixture;
		fixture.buffers.pPhysicalPageSets = nullptr;
		T3D_CHECK( test, !t3d_vt_initialize_residency_cold( fixture.database, fixture.buffers, 7, 2, fixture.state ) );
		T3D_CHECK( test, !fixture.state.bInitialized );
	} );
	runner.run( "virtual_texturing", "self_parent_page_is_rejected", []( TestContext &test ) {
		ResidencyFixture fixture;
		fixture.pages[4].nParentPage = 4;
		T3D_CHECK( test, !fixture.initialize() && !fixture.state.bInitialized );
	} );
	runner.run( "virtual_texturing", "upload_becomes_visible_only_after_all_channels_commit", []( TestContext &test ) {
		ResidencyFixture		 fixture;
		t3dVTUploadReservation_t reservation{};
		T3D_REQUIRE( test, fixture.initialize() && t3d_vt_reserve_upload_hot( fixture.state, 4, 0, 10, reservation ) );
		T3D_CHECK( test, ( fixture.table[4].nFlags & T3D_VT_PAGE_RESIDENT ) == 0 );
		T3D_CHECK( test, !t3d_vt_commit_upload_hot( fixture.state, reservation, 1, 10 ) );
		T3D_CHECK( test, t3d_vt_commit_upload_hot( fixture.state, reservation, T3D_VT_PAGE_SET_CHANNEL_MASK, 10 ) );
		T3D_CHECK( test,
				   fixture.table[4].nPhysicalSlot == reservation.nPhysicalSlot &&
					   fixture.state.Stats.nPendingSlotCount == 0 );
		T3D_CHECK( test, t3d_vt_validate_residency_reference_debug( fixture.state ) );
	} );
	runner.run( "virtual_texturing", "aborted_upload_returns_its_slot_without_publishing", []( TestContext &test ) {
		ResidencyFixture		 fixture;
		t3dVTUploadReservation_t reservation{};
		T3D_REQUIRE( test, fixture.initialize() && t3d_vt_reserve_upload_hot( fixture.state, 4, 0, 10, reservation ) );
		t3d_vt_abort_upload_hot( fixture.state, reservation );
		T3D_CHECK( test, fixture.state.Stats.nFreeSlotCount == 3 && fixture.state.Stats.nPendingSlotCount == 0 );
		T3D_CHECK( test, ( fixture.table[4].nFlags & T3D_VT_PAGE_RESIDENT ) == 0 );
		T3D_CHECK( test, t3d_vt_validate_residency_reference_debug( fixture.state ) );
	} );
	runner.run( "virtual_texturing", "stale_database_generation_cannot_commit_an_upload", []( TestContext &test ) {
		ResidencyFixture		 fixture;
		t3dVTUploadReservation_t reservation{};
		T3D_REQUIRE( test, fixture.initialize() && t3d_vt_reserve_upload_hot( fixture.state, 4, 0, 10, reservation ) );
		auto stale = reservation;
		stale.nDatabaseGeneration++;
		T3D_CHECK( test, !t3d_vt_can_commit_upload_hot( fixture.state, stale ) );
		T3D_CHECK( test, !t3d_vt_commit_upload_hot( fixture.state, stale, T3D_VT_PAGE_SET_CHANNEL_MASK, 10 ) );
		t3d_vt_abort_upload_hot( fixture.state, reservation );
		T3D_CHECK( test, fixture.state.Stats.nFreeSlotCount == 3 );
	} );
	runner.run( "virtual_texturing", "resident_and_out_of_range_pages_cannot_reserve_again", []( TestContext &test ) {
		ResidencyFixture		 fixture;
		t3dVTUploadReservation_t reservation{};
		T3D_REQUIRE( test, fixture.initialize() && fixture.upload( 4 ) );
		T3D_CHECK( test, !t3d_vt_reserve_upload_hot( fixture.state, 4, 0, 11, reservation ) );
		T3D_CHECK( test, !t3d_vt_reserve_upload_hot( fixture.state, 6, 0, 11, reservation ) );
	} );
	runner.run( "virtual_texturing", "missing_pages_resolve_to_the_neutral_ancestor", []( TestContext &test ) {
		ResidencyFixture fixture;
		uint16_t		 mip = 99;
		T3D_REQUIRE( test, fixture.initialize() );
		T3D_CHECK( test, t3d_vt_resolve_page_hot( fixture.state, 4, mip ) == 0 );
		T3D_CHECK( test, t3d_vt_resolve_page_hot( fixture.state, 6, mip ) == T3D_VT_INVALID_PAGE );
	} );
	runner.run( "virtual_texturing",
				"touch_updates_the_visible_frame_only_for_resident_pages",
				[]( TestContext &test ) {
					ResidencyFixture fixture;
					T3D_REQUIRE( test, fixture.initialize() && fixture.upload( 4 ) );
					T3D_CHECK( test, t3d_vt_touch_page_hot( fixture.state, 4, 42 ) );
					T3D_CHECK( test, fixture.slots[fixture.table[4].nPhysicalSlot].m_nLastVisibleFrame == 42 );
					T3D_CHECK( test, !t3d_vt_touch_page_hot( fixture.state, 5, 42 ) );
				} );
	runner.run( "virtual_texturing", "retired_slots_wait_for_the_frame_delay_before_reuse", []( TestContext &test ) {
		ResidencyFixture fixture;
		T3D_REQUIRE( test, fixture.initialize() && fixture.upload( 4 ) );
		const uint32_t slot = fixture.table[4].nPhysicalSlot;
		T3D_REQUIRE( test, t3d_vt_begin_eviction_hot( fixture.state, slot, 4, 20 ) );
		T3D_CHECK( test, fixture.state.Stats.nRetiredSlotCount == 1 );
		T3D_CHECK( test, t3d_vt_service_retired_slots_hot( fixture.state, 21 ) );
		T3D_CHECK( test, fixture.state.Stats.nRetiredSlotCount == 1 && fixture.state.Stats.nFreeSlotCount == 2 );
		T3D_CHECK( test, t3d_vt_service_retired_slots_hot( fixture.state, 22 ) );
		T3D_CHECK( test, fixture.state.Stats.nRetiredSlotCount == 0 && fixture.state.Stats.nFreeSlotCount == 3 );
		T3D_CHECK( test, t3d_vt_validate_residency_reference_debug( fixture.state ) );
	} );
	runner.run( "virtual_texturing", "neutral_roots_cannot_be_evicted", []( TestContext &test ) {
		ResidencyFixture fixture;
		T3D_REQUIRE( test, fixture.initialize() );
		T3D_CHECK( test, !t3d_vt_begin_eviction_hot( fixture.state, 0, 0, 1000 ) );
		T3D_CHECK( test, fixture.state.Stats.nResidentCount == 1 );
	} );
	runner.run( "virtual_texturing", "residency_policy_keeps_a_shared_capacity_pool", []( TestContext &test ) {
		t3dVTResidencyPolicy_t policy{};
		t3d_vt_default_residency_policy( policy );
		T3D_CHECK( test, t3d_vt_validate_residency_policy( policy ) );
		memset( policy.nProtectedResidentPercent, 0, sizeof( policy.nProtectedResidentPercent ) );
		policy.nProtectedResidentPercent[0] = 100;
		T3D_CHECK( test, !t3d_vt_validate_residency_policy( policy ) );
	} );
	// Streaming demand: deduplication, priorities, budgets and cancellation.
	runner.run( "virtual_texturing", "duplicate_demand_merges_without_allocating_another_job", []( TestContext &test ) {
		StreamingFixture fixture;
		T3D_REQUIRE( test, fixture.initialize() );
		T3D_CHECK( test,
				   t3d_vt_streaming_admit_request_hot( fixture.streaming, { 4, 0, 1 }, 10 ) ==
					   T3D_VT_ADMISSION_ACCEPTED_NEW );
		T3D_CHECK( test,
				   t3d_vt_streaming_admit_request_hot( fixture.streaming, { 4, 0, 65535 }, 10 ) ==
					   T3D_VT_ADMISSION_MERGED_EXISTING );
		T3D_CHECK( test, fixture.streaming.nQueuedJobCount == 1 && fixture.streaming.Stats.nDuplicateMergeCount == 1 );
		T3D_CHECK( test, fixture.jobs[fixture.page_to_job[4]].nPriority == 65535 );
	} );
	runner.run( "virtual_texturing", "scheduler_claims_the_more_urgent_page_first", []( TestContext &test ) {
		StreamingFixture fixture;
		uint32_t		 job = UINT32_MAX;
		T3D_REQUIRE( test, fixture.initialize() );
		T3D_REQUIRE( test,
					 t3d_vt_streaming_admit_request_hot( fixture.streaming, { 4, 0, 1 }, 10 ) ==
						 T3D_VT_ADMISSION_ACCEPTED_NEW );
		T3D_REQUIRE( test,
					 t3d_vt_streaming_admit_request_hot( fixture.streaming, { 5, 0, 65535 }, 10 ) ==
						 T3D_VT_ADMISSION_ACCEPTED_NEW );
		T3D_REQUIRE( test, t3d_vt_streaming_claim_job_owner_hot( fixture.streaming, job ) );
		T3D_CHECK( test, fixture.jobs[job].nVirtualPage == 5 );
		T3D_CHECK( test, t3d_vt_streaming_cancel_claim_owner_hot( fixture.streaming, job ) );
	} );
	runner.run( "virtual_texturing", "resident_requests_do_not_start_io", []( TestContext &test ) {
		StreamingFixture fixture;
		T3D_REQUIRE( test, fixture.initialize() && fixture.upload( 4 ) );
		T3D_CHECK( test,
				   t3d_vt_streaming_admit_request_hot( fixture.streaming, { 4, 0, 65535 }, 11 ) ==
					   T3D_VT_ADMISSION_ALREADY_RESIDENT );
		T3D_CHECK( test, fixture.streaming.nQueuedJobCount == 0 );
	} );
	runner.run( "virtual_texturing", "invalid_demand_does_not_consume_jobs", []( TestContext &test ) {
		StreamingFixture fixture;
		T3D_REQUIRE( test, fixture.initialize() );
		T3D_CHECK( test,
				   t3d_vt_streaming_admit_request_hot( fixture.streaming, { 6, 0, 1 }, 10 ) ==
					   T3D_VT_ADMISSION_INVALID_PAGE );
		T3D_CHECK( test, fixture.streaming.nFreeJobCount == 4 && fixture.streaming.nQueuedJobCount == 0 );
	} );
	runner.run( "virtual_texturing", "admission_budget_defers_demand_until_the_next_frame", []( TestContext &test ) {
		StreamingFixture fixture;
		fixture.limits.nMaxAdmittedRequestsPerFrame = 1;
		T3D_REQUIRE( test, fixture.initialize() );
		T3D_REQUIRE( test,
					 t3d_vt_streaming_admit_request_hot( fixture.streaming, { 4, 0, 1 }, 10 ) ==
						 T3D_VT_ADMISSION_ACCEPTED_NEW );
		T3D_CHECK( test,
				   t3d_vt_streaming_admit_request_hot( fixture.streaming, { 5, 0, 1 }, 10 ) ==
					   T3D_VT_ADMISSION_DEFERRED_FRAME_BUDGET );
		T3D_CHECK( test,
				   t3d_vt_streaming_admit_request_hot( fixture.streaming, { 5, 0, 1 }, 11 ) ==
					   T3D_VT_ADMISSION_ACCEPTED_NEW );
	} );
	runner.run(
		"virtual_texturing",
		"cancelling_a_claim_releases_scratch_and_requeues_demand",
		[]( TestContext &test ) {
			StreamingFixture fixture;
			uint32_t		 job = UINT32_MAX;
			T3D_REQUIRE( test, fixture.initialize() );
			T3D_REQUIRE( test,
						 t3d_vt_streaming_admit_request_hot( fixture.streaming, { 4, 0, 1 }, 10 ) ==
							 T3D_VT_ADMISSION_ACCEPTED_NEW );
			T3D_REQUIRE( test, t3d_vt_streaming_claim_job_owner_hot( fixture.streaming, job ) );
			const uint32_t lane = fixture.jobs[job].nScratchLane;
			T3D_CHECK( test, t3d_vt_streaming_cancel_claim_owner_hot( fixture.streaming, job ) );
			T3D_CHECK( test,
					   fixture.lane_owner[lane] == T3D_VT_INVALID_ID && fixture.streaming.nInFlightIoJobCount == 0 );
			T3D_CHECK( test, fixture.streaming.nFreeJobCount == 3 && fixture.streaming.nQueuedJobCount == 1 );
			T3D_CHECK( test, fixture.jobs[job].nState == T3D_VT_STREAM_QUEUED && fixture.page_to_job[4] == job );
			uint32_t reclaimed_job = UINT32_MAX;
			T3D_REQUIRE( test, t3d_vt_streaming_claim_job_owner_hot( fixture.streaming, reclaimed_job ) );
			T3D_CHECK( test, reclaimed_job == job );
			T3D_CHECK( test, t3d_vt_streaming_cancel_claim_owner_hot( fixture.streaming, reclaimed_job ) );
		} );
	runner.run( "virtual_texturing",
				"quiescing_invalidates_queued_demand_and_resume_reopens_admission",
				[]( TestContext &test ) {
					StreamingFixture fixture;
					T3D_REQUIRE( test, fixture.initialize() );
					T3D_REQUIRE( test,
								 t3d_vt_streaming_admit_request_hot( fixture.streaming, { 4, 0, 1 }, 10 ) ==
									 T3D_VT_ADMISSION_ACCEPTED_NEW );
					T3D_CHECK( test, t3d_vt_streaming_quiesce_package_warm( fixture.streaming ) );
					T3D_CHECK( test,
							   fixture.streaming.bQuiescing && fixture.streaming.nQueuedJobCount == 0 &&
								   fixture.streaming.nFreeJobCount == 4 );
					t3d_vt_streaming_resume_package_warm( fixture.streaming );
					T3D_CHECK( test, !fixture.streaming.bQuiescing );
					T3D_CHECK( test,
							   t3d_vt_streaming_admit_request_hot( fixture.streaming, { 5, 0, 1 }, 11 ) ==
								   T3D_VT_ADMISSION_ACCEPTED_NEW );
				} );
}
} // namespace terrain3d_tests
