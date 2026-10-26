// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "assets/resources/generated_texture.h"
#include "core/utility/terrain_3d_sync.h"
#include "diagnostics/tests/unit_testing.h"
#include "data/persistence/terrain_3d_file_publication.h"
#include "diagnostics/core/terrain_3d_diagnostics.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_recipe.h"
#include "rendering/rd/world/resources/terrain_3d_world_resources.h"
#include <godot_cpp/classes/rendering_server.hpp>
#include "rendering/rd/forward/terrain_3d_rd_forward_pipeline_base.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"

#include "core/config/terrain_3d_compiler.h"
#include "core/utility/terrain_3d_util.h"
#include "hlod/build/terrain_3d_hlod_builder.h"
#include "rendering/rd/instances/terrain_3d_rd_culling.h"
#include "rendering/rd/instances/terrain_3d_rd_mesh_cache.h"
#include "rendering/rd/resources/terrain_3d_rd_texture_manager.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/geometry/terrain_3d_clipmap_strip_contract.h"

#include <EASTL/numeric_limits.h>
#include <stddef.h>
typedef struct t3dRdUnitTestState_s
{
	uint32_t m_nPassed = 0u;
	uint32_t m_nFailed = 0u;

	void expect(
		const bool		  bCondition,
		const char *const pDescription ) {
		if ( bCondition ) {
			++m_nPassed;
			UtilityFunctions::print( "PASSED: ", pDescription );
		} else {
			++m_nFailed;
			UtilityFunctions::print( "FAILED: ", pDescription );
		}
	}
} t3dRdUnitTestState_t;

static bool t3dRunEastlContainerOwnershipTests(
	t3dRdUnitTestState_t &rState ) {
	eastl::vector< uint32_t, Terrain3DEastlAllocator > values;
	const bool										   resized = t3dVectorResizeCold( values, 2u );
	rState.expect( resized, "EASTL vector cold resize establishes allocator-backed capacity" );
	if ( !resized ) {
		return false;
	}
	values[0] = 7u;
	values[1] = 11u;
	rState.expect( values.size() == 2u && values[0] == 7u && values[1] == 11u,
				   "EASTL vector preserves explicitly-sized contents" );
	if ( !t3dContainerReserveCold( values, 3u ) || !t3dVectorInsertWithinCapacity( values, 1u, 9u ) ) {
		rState.expect( false, "EASTL vector inserts within established capacity" );
		return false;
	}
	rState.expect( values.size() == 3u && values[0] == 7u && values[1] == 9u && values[2] == 11u,
				   "EASTL vector ordered insertion preserves contents" );
	return true;
}

static bool t3dRunRdDescriptorContractTests(
	t3dRdUnitTestState_t &rState ) {
	RenderingServer *const pServer = RenderingServer::get_singleton();
	rState.expect( pServer != NULL, "RD descriptor contract tests have a RID allocator" );
	if ( !pServer ) {
		return false;
	}
	RID handles[6];
	for ( uint32_t i = 0u; i < 6u; ++i ) {
		handles[i] = pServer->canvas_item_create();
	}
	bool valid = true;
	for ( uint32_t i = 0u; i < 6u; ++i ) {
		valid = valid && handles[i].is_valid();
	}
	rState.expect( valid, "RD descriptor recipe fixture creates distinct RID identities" );
	if ( !valid ) {
		for ( uint32_t i = 0u; i < 6u; ++i ) {
			if ( handles[i].is_valid() ) {
				pServer->free_rid( handles[i] );
			}
		}
		return false;
	}
	const t3dRdDescriptorDeviceKey_t device{ T3D_RD_DESCRIPTOR_GLOBAL_DEVICE_ID, 7u, T3D_RD_DESCRIPTOR_DEVICE_GLOBAL };
	const uint32_t					 samplerType = RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	const uint32_t					 storageType = RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
	const RID						 samplerTexture[2] = { handles[2], handles[3] };
	const RID						 samplerTextureReversed[2] = { handles[3], handles[2] };
	const RID						 buffer[1] = { handles[4] };
	t3dRdDescriptorRecipe_t			 original;
	t3dRdDescriptorRecipe_t			 same;
	bool built = t3dRdDescriptorRecipeBeginCold( original, device, handles[0], 1u ) == T3D_RD_DESCRIPTOR_OK;
	built =
		built && t3dRdDescriptorRecipeAddBindingCold( original, 9u, storageType, buffer, 1u ) == T3D_RD_DESCRIPTOR_OK;
	built = built && t3dRdDescriptorRecipeAddBindingCold( original, 2u, samplerType, samplerTexture, 2u ) ==
						 T3D_RD_DESCRIPTOR_OK;
	built = built && t3dRdDescriptorRecipeBeginCold( same, device, handles[0], 1u ) == T3D_RD_DESCRIPTOR_OK;
	built = built &&
			t3dRdDescriptorRecipeAddBindingCold( same, 2u, samplerType, samplerTexture, 2u ) == T3D_RD_DESCRIPTOR_OK;
	built = built && t3dRdDescriptorRecipeAddBindingCold( same, 9u, storageType, buffer, 1u ) == T3D_RD_DESCRIPTOR_OK;
	rState.expect( built && t3dRdDescriptorRecipeValidate( original, T3D_RD_DESCRIPTOR_DEVICE_GLOBAL ) ==
								T3D_RD_DESCRIPTOR_OK,
				   "RD descriptor recipes canonicalize out-of-order binding insertion" );
	const uint64_t fingerprint = t3dRdDescriptorRecipeFingerprint( original );
	rState.expect( fingerprint != 0u && fingerprint == t3dRdDescriptorRecipeFingerprint( same ) &&
					   t3dRdDescriptorRecipesEqual( original, same ) &&
					   t3dRdDescriptorRecipesMatch( original, fingerprint, same, fingerprint ),
				   "RD descriptor identical exact recipes and fingerprints reuse identity" );
	rState.expect( t3dRdDescriptorRecipeAddBindingCold( same, 2u, samplerType, samplerTexture, 2u ) ==
						   T3D_RD_DESCRIPTOR_INVALID_ARGUMENT &&
					   t3dRdDescriptorRecipesEqual( original, same ),
				   "RD duplicate binding is rejected without mutation" );
	t3dRdDescriptorRecipe_t changed = original;
	changed.m_hShader = handles[1];
	rState.expect( !t3dRdDescriptorRecipesEqual( original, changed ), "RD changed shader RID changes descriptor key" );
	changed = original;
	changed.m_nSetIndex = 2u;
	rState.expect( !t3dRdDescriptorRecipesEqual( original, changed ), "RD changed set index changes descriptor key" );
	changed = original;
	changed.m_device.m_nDeviceGeneration++;
	rState.expect( !t3dRdDescriptorRecipesEqual( original, changed ),
				   "RD changed device generation changes descriptor key" );
	changed = original;
	changed.m_device.m_eDomain = T3D_RD_DESCRIPTOR_DEVICE_LOCAL;
	changed.m_device.m_nDeviceIdentity = 91u;
	rState.expect( t3dRdDescriptorRecipeValidate( changed, T3D_RD_DESCRIPTOR_DEVICE_GLOBAL ) ==
						   T3D_RD_DESCRIPTOR_WRONG_DEVICE &&
					   !t3dRdDescriptorRecipesEqual( original, changed ),
				   "RD local-device key cannot pass global validation" );
	t3dRdDescriptorRecipe_t otherLocal = changed;
	otherLocal.m_device.m_nDeviceIdentity = 92u;
	rState.expect( !t3dRdDescriptorRecipesEqual( changed, otherLocal ),
				   "RD separate local devices have separate RID namespaces" );
	changed = original;
	changed.m_szBindings[0].m_nType = RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER;
	rState.expect( !t3dRdDescriptorRecipesEqual( original, changed ),
				   "RD changed uniform type changes descriptor key" );
	changed = original;
	changed.m_szResources[0] = handles[5];
	rState.expect( !t3dRdDescriptorRecipesEqual( original, changed ), "RD changed sampler RID changes descriptor key" );
	changed = original;
	changed.m_szResources[1] = handles[5];
	rState.expect( !t3dRdDescriptorRecipesEqual( original, changed ), "RD changed texture RID changes descriptor key" );
	changed = original;
	changed.m_szResources[2] = handles[5];
	rState.expect( !t3dRdDescriptorRecipesEqual( original, changed ), "RD changed buffer RID changes descriptor key" );
	changed = original;
	changed.m_szResources[0] = samplerTextureReversed[0];
	changed.m_szResources[1] = samplerTextureReversed[1];
	rState.expect( !t3dRdDescriptorRecipesEqual( original, changed ), "RD resource ordering is part of exact key" );
	rState.expect( !t3dRdDescriptorRecipesMatch( original, 123u, changed, 123u ),
				   "RD forced hash collision still compares exact resource IDs" );
	changed = original;
	changed.m_szBindings[1].m_nResourceFirst = 0u;
	rState.expect( t3dRdDescriptorRecipeValidate( changed, T3D_RD_DESCRIPTOR_DEVICE_GLOBAL ) ==
					   T3D_RD_DESCRIPTOR_INVALID_RECIPE,
				   "RD malformed resource offsets cannot enter the cache" );
	changed = original;
	const RID extra[1] = { handles[5] };
	rState.expect( t3dRdDescriptorRecipeAddBindingCold( changed, 10u, storageType, extra, 1u ) ==
						   T3D_RD_DESCRIPTOR_OK &&
					   !t3dRdDescriptorRecipesEqual( original, changed ),
				   "RD binding count changes descriptor identity" );
	rState.expect( t3dRdDescriptorRecipeAddBindingCold( changed, 11u, samplerType, extra, 1u ) ==
					   T3D_RD_DESCRIPTOR_INVALID_ARGUMENT,
				   "RD incomplete sampler/texture pair rejected without mutation" );
	t3dRdDescriptorRecipe_t capacity;
	bool filled = t3dRdDescriptorRecipeBeginCold( capacity, device, handles[0], 0u ) == T3D_RD_DESCRIPTOR_OK;
	for ( uint32_t i = 0u; i < T3D_RD_DESCRIPTOR_MAX_BINDINGS; ++i ) {
		filled = filled &&
				 t3dRdDescriptorRecipeAddBindingCold( capacity, i, storageType, buffer, 1u ) == T3D_RD_DESCRIPTOR_OK;
	}
	rState.expect(
		filled &&
			t3dRdDescriptorRecipeAddBindingCold( capacity, T3D_RD_DESCRIPTOR_MAX_BINDINGS, storageType, buffer, 1u ) ==
				T3D_RD_DESCRIPTOR_RECIPE_CAPACITY_EXCEEDED &&
			capacity.m_nBindingCount == T3D_RD_DESCRIPTOR_MAX_BINDINGS,
		"RD binding capacity fails closed" );
	RID largeResources[T3D_RD_DESCRIPTOR_MAX_RESOURCES];
	for ( uint32_t i = 0u; i < T3D_RD_DESCRIPTOR_MAX_RESOURCES; ++i ) {
		largeResources[i] = handles[4];
	}
	built = t3dRdDescriptorRecipeBeginCold( capacity, device, handles[0], 0u ) == T3D_RD_DESCRIPTOR_OK;
	built = built && t3dRdDescriptorRecipeAddBindingCold( capacity,
														  0u,
														  storageType,
														  largeResources,
														  T3D_RD_DESCRIPTOR_MAX_RESOURCES ) == T3D_RD_DESCRIPTOR_OK;
	rState.expect( built &&
					   t3dRdDescriptorRecipeAddBindingCold( capacity, 1u, storageType, buffer, 1u ) ==
						   T3D_RD_DESCRIPTOR_RECIPE_CAPACITY_EXCEEDED &&
					   capacity.m_nResourceCount == T3D_RD_DESCRIPTOR_MAX_RESOURCES,
				   "RD bound resource capacity fails closed" );
	uint32_t slots = 0u;
	rState.expect( t3dRdDescriptorPlanCapacityCold( { 16u, 20u, 4u }, slots ) == T3D_RD_DESCRIPTOR_OK && slots == 16u,
				   "RD cache cold capacity reserves non-descriptor registry entries" );
	rState.expect( t3dRdDescriptorPlanCapacityCold( { 17u, 20u, 4u }, slots ) ==
						   T3D_RD_DESCRIPTOR_RESOURCE_REGISTRY_EXHAUSTED &&
					   slots == 0u,
				   "RD cache cold capacity rejects untracked overcommit" );
	const t3dRdDescriptorLease_t lease{ 4u, 9u, 7u };
	rState.expect( t3dRdDescriptorLeaseMatches( lease, 4u, 9u, 7u ) &&
					   !t3dRdDescriptorLeaseMatches( lease, 4u, 10u, 7u ) &&
					   !t3dRdDescriptorLeaseMatches( lease, 4u, 9u, 8u ),
				   "RD leases reject recycled slots and device epochs" );
	for ( uint32_t i = 0u; i < 6u; ++i ) {
		pServer->free_rid( handles[i] );
	}
	return built;
}

// t3dRdPublicationExchangeState_s centralized in Terrain3DTypes.h.

static void t3dRdPublicationPublish(
	t3dRdPublicationExchangeState_t *const pState,
	const uint64_t						   nGeneration ) {
	pState->m_pSlotGenerations[*pState->m_pWriteIndex] = nGeneration;
	const uint64_t nReclaimed =
		pState->m_pExchangeToken->exchange( t3dRdMakeFrameSnapshotToken( *pState->m_pWriteIndex, nGeneration, true ),
											T3D_MEMORY_ORDER_ACQ_REL );
	*pState->m_pWriteIndex = t3dRdFrameSnapshotTokenIndex( nReclaimed );
}

static bool t3dRdPublicationAcquire(
	t3dRdPublicationExchangeState_t *const pState ) {
	uint64_t nToken = pState->m_pExchangeToken->load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( !t3dRdFrameSnapshotTokenIsPublished( nToken ) ) {
		return false;
	}
	const uint32_t nCandidateIndex = t3dRdFrameSnapshotTokenIndex( nToken );
	const uint64_t nCandidateGeneration = t3dRdFrameSnapshotTokenGeneration( nToken );
	const uint64_t nReleased =
		t3dRdMakeFrameSnapshotToken( *pState->m_pRenderIndex, *pState->m_pRenderGeneration, false );
	if ( !pState->m_pExchangeToken->compare_exchange_strong( nToken,
															 nReleased,
															 T3D_MEMORY_ORDER_ACQ_REL,
															 T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return false;
	}
	*pState->m_pRenderIndex = nCandidateIndex;
	*pState->m_pRenderGeneration = nCandidateGeneration;
	return pState->m_pSlotGenerations[*pState->m_pRenderIndex] == *pState->m_pRenderGeneration;
}

// t3dRdPublicationPayload_s centralized in Terrain3DTypes.h.

// t3dRdPublicationThreadContext_s centralized in Terrain3DTypes.h.

static void t3dRdPublicationProducer(
	void *const pContext ) {
	t3dRdPublicationThreadContext_t *const pState = ( t3dRdPublicationThreadContext_t * ) ( pContext );
	uint32_t							   nWriteIndex = 2u;
	for ( uint64_t nGeneration = 1u; nGeneration <= T3D_UNIT_TESTING_PUBLICATION_COUNT; ++nGeneration ) {
		if ( nWriteIndex >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ) {
			pState->m_pValid->store( false, T3D_MEMORY_ORDER_RELEASE );
			break;
		}
		pState->m_pSlots[nWriteIndex].m_nGeneration = nGeneration;
		pState->m_pSlots[nWriteIndex].m_nCheck = nGeneration ^ T3D_UNIT_TESTING_CHECK_MASK;
		const uint64_t nReclaimed =
			pState->m_pToken->exchange( t3dRdMakeFrameSnapshotToken( nWriteIndex, nGeneration, true ),
										T3D_MEMORY_ORDER_ACQ_REL );
		nWriteIndex = t3dRdFrameSnapshotTokenIndex( nReclaimed );
	}
	pState->m_pProducerDone->store( true, T3D_MEMORY_ORDER_RELEASE );
}

static void t3dRdPublicationConsumer(
	void *const pContext ) {
	t3dRdPublicationThreadContext_t *const pState = ( t3dRdPublicationThreadContext_t * ) ( pContext );
	uint32_t							   nRenderIndex = 0u;
	uint64_t							   nGeneration = 0u;
	for ( ;; ) {
		uint64_t nToken = pState->m_pToken->load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( !t3dRdFrameSnapshotTokenIsPublished( nToken ) ) {
			if ( pState->m_pProducerDone->load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
				break;
			}
			t3dThreadYield();
			continue;
		}
		const uint32_t nCandidateIndex = t3dRdFrameSnapshotTokenIndex( nToken );
		const uint64_t nCandidateGeneration = t3dRdFrameSnapshotTokenGeneration( nToken );
		if ( nCandidateIndex >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ||
			 !t3dRdFrameSnapshotGenerationIsNewer( nCandidateGeneration, nGeneration ) ) {
			pState->m_pValid->store( false, T3D_MEMORY_ORDER_RELEASE );
			break;
		}
		const uint64_t nReleased = t3dRdMakeFrameSnapshotToken( nRenderIndex, nGeneration, false );
		if ( !pState->m_pToken->compare_exchange_weak( nToken,
													   nReleased,
													   T3D_MEMORY_ORDER_ACQ_REL,
													   T3D_MEMORY_ORDER_ACQUIRE ) ) {
			continue;
		}
		nRenderIndex = nCandidateIndex;
		nGeneration = nCandidateGeneration;
		const t3dRdPublicationPayload_t payload = pState->m_pSlots[nRenderIndex];
		if ( payload.m_nGeneration != nGeneration ||
			 payload.m_nCheck != ( nGeneration ^ T3D_UNIT_TESTING_CHECK_MASK ) ) {
			pState->m_pValid->store( false, T3D_MEMORY_ORDER_RELEASE );
			break;
		}
		pState->m_pAcquires->fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
}

static bool t3dRunRdAbiAndPublicationTests(
	t3dRdUnitTestState_t &rState ) {
	rState.expect( sizeof( t3dClipmapStripPart_t ) == 12u, "RD procedural strip descriptor ABI is 12 bytes" );
	rState.expect(
		offsetof( t3dClipmapStripPart_t, m_nOriginX ) == 0u && offsetof( t3dClipmapStripPart_t, m_nCellWidth ) == 4u &&
			offsetof( t3dClipmapStripPart_t, m_nPartCode ) == 8u && offsetof( t3dClipmapStripPart_t, m_nFlags ) == 10u,
		"RD procedural strip descriptor offsets match the shader packing contract" );
	rState.expect( sizeof( t3dRdPatch_t ) == 64u, "RD patch descriptor ABI is 64 bytes" );
	rState.expect(
		offsetof( t3dRdPatch_t, m_fOriginX ) == 0u && offsetof( t3dRdPatch_t, m_nFlags ) == 28u &&
			offsetof( t3dRdPatch_t, m_fBoundsMinY ) == 32u && offsetof( t3dRdPatch_t, m_fBoundsMaxY ) == 36u &&
			offsetof( t3dRdPatch_t, m_fBoundsHalfExtent ) == 40u &&
			offsetof( t3dRdPatch_t, m_fPreviousOriginX ) == 44u &&
			offsetof( t3dRdPatch_t, m_fPreviousOriginZ ) == 48u && offsetof( t3dRdPatch_t, m_fPreviousScale ) == 52u &&
			offsetof( t3dRdPatch_t, m_fPadding1 ) == 60u,
		"RD patch bounds and previous-frame offsets match std430" );
	rState.expect( sizeof( t3dRdMaterialLayer_t ) == 80u && offsetof( t3dRdMaterialLayer_t, m_nPhysicalLayer ) == 56u &&
					   offsetof( t3dRdMaterialLayer_t, m_nFlags ) == 60u &&
					   offsetof( t3dRdMaterialLayer_t, m_nMicroVTAllocation ) == 64u &&
					   offsetof( t3dRdMaterialLayer_t, m_nMicroVTGeneration ) == 68u,
				   "RD material layer size and terminal offsets match std430" );
	rState.expect( sizeof( t3dRdIndexedIndirectCommand_t ) == 20u &&
					   offsetof( t3dRdIndexedIndirectCommand_t, m_nIndexCount ) == 0u &&
					   offsetof( t3dRdIndexedIndirectCommand_t, m_nFirstInstance ) == 16u,
				   "RD indexed indirect command matches VkDrawIndexedIndirectCommand" );
	rState.expect( sizeof( t3dRdCullPushConstants_t ) == 32u,
				   "RD culling push constants include arena offsets, HOT selection and copy mode" );
	rState.expect( sizeof( t3dRdDrawPushConstants_t ) == 20u &&
					   offsetof( t3dRdDrawPushConstants_t, m_nDirectMode ) == 4u,
				   "RD direct-draw push constants match raster shader layout" );

	for ( uint32_t slot = 0u; slot < T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT; ++slot ) {
		const uint64_t token = t3dRdMakeFrameSnapshotToken( slot, T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX, true );
		rState.expect( t3dRdFrameSnapshotTokenIndex( token ) == slot &&
						   t3dRdFrameSnapshotTokenGeneration( token ) == T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX &&
						   t3dRdFrameSnapshotTokenIsPublished( token ),
					   "RD snapshot token round-trips slot/generation/publication" );
	}
	const uint64_t unpublished = t3dRdMakeFrameSnapshotToken( 1u, 7u, false );
	rState.expect( !t3dRdFrameSnapshotTokenIsPublished( unpublished ) &&
					   t3dRdFrameSnapshotTokenIndex( unpublished ) == 1u &&
					   t3dRdFrameSnapshotTokenGeneration( unpublished ) == 7u,
				   "RD unpublished snapshot token preserves payload" );
	rState.expect( t3dRdFrameSnapshotGenerationIsNewer( 8u, 7u ) && !t3dRdFrameSnapshotGenerationIsNewer( 7u, 7u ) &&
					   t3dRdFrameSnapshotGenerationIsStale( 7u, 7u ) && t3dRdFrameSnapshotGenerationIsStale( 6u, 7u ) &&
					   !t3dRdFrameSnapshotGenerationIsStale( 0u, 7u ),
				   "RD snapshot generation ordering detects stale publication" );
	rState.expect( t3dRdFrameSnapshotGenerationIsNewer( 1u, T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX ) &&
					   t3dRdFrameSnapshotGenerationIsStale( T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX, 1u ) &&
					   t3dRdFrameSnapshotGenerationDistance( 1u, T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX ) == 1u,
				   "RD snapshot generation ordering survives max-to-one wrap" );

	// Exercise the same ownership-exchange protocol used by the coordinator:
	// one slot is rendered, one is published, and only the exchanged slot may
	// be reclaimed by the single writer.
	uint64_t slot_generations[T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT];
	memset( slot_generations, 0, sizeof( slot_generations ) );
	t3dAtomicU64_t					exchange_token{ t3dRdMakeFrameSnapshotToken( 1u, 0u, false ) };
	uint32_t						write_index = 2u;
	uint32_t						render_index = 0u;
	uint64_t						render_generation = 0u;
	t3dRdPublicationExchangeState_t exchangeState;
	exchangeState.m_pSlotGenerations = slot_generations;
	exchangeState.m_pExchangeToken = &exchange_token;
	exchangeState.m_pWriteIndex = &write_index;
	exchangeState.m_pRenderIndex = &render_index;
	exchangeState.m_pRenderGeneration = &render_generation;
	t3dRdPublicationPublish( &exchangeState, 1u );
	const bool first_acquire = t3dRdPublicationAcquire( &exchangeState );
	const bool first_slots_distinct = write_index != render_index;
	t3dRdPublicationPublish( &exchangeState, 2u );
	t3dRdPublicationPublish( &exchangeState, 3u ); // Deliberately replace an unconsumed publication.
	const bool latest_acquire = t3dRdPublicationAcquire( &exchangeState );
	rState.expect( first_acquire && first_slots_distinct && latest_acquire && render_generation == 3u &&
					   write_index != render_index,
				   "RD three-slot ownership exchange publishes latest complete data without writer/reader overlap" );

	t3dRdPublicationPayload_t concurrent_slots[T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT];
	memset( concurrent_slots, 0, sizeof( concurrent_slots ) );
	t3dAtomicU64_t	concurrent_token{ t3dRdMakeFrameSnapshotToken( 1u, 0u, false ) };
	t3dAtomicBool_t producer_done{ false };
	t3dAtomicBool_t concurrent_valid{ true };
	t3dAtomicU64_t	concurrent_acquires{ 0u };

	t3dRdPublicationThreadContext_t threadContext;
	threadContext.m_pSlots = concurrent_slots;
	threadContext.m_pToken = &concurrent_token;
	threadContext.m_pProducerDone = &producer_done;
	threadContext.m_pValid = &concurrent_valid;
	threadContext.m_pAcquires = &concurrent_acquires;
	Terrain3DThread producer( t3dRdPublicationProducer, &threadContext );
	Terrain3DThread consumer( t3dRdPublicationConsumer, &threadContext );
	producer.join();
	consumer.join();
	rState.expect( concurrent_valid.load( T3D_MEMORY_ORDER_ACQUIRE ) &&
					   concurrent_acquires.load( T3D_MEMORY_ORDER_ACQUIRE ) > 0u,
				   "RD three-slot exchange survives concurrent SPSC overwrite/consume stress without torn payloads" );
	return rState.m_nFailed == 0u;
}

static t3dRdPatch_t t3dRdMakeCullTestPatch(
	const uint32_t nLod,
	const uint32_t nTier,
	const float	   fOriginX,
	const float	   fOriginZ ) {
	t3dRdPatch_t patch;
	patch.m_fOriginX = fOriginX;
	patch.m_fOriginZ = fOriginZ;
	patch.m_fScale = 1.0f;
	patch.m_nLod = nLod;
	patch.m_nTier = nTier;
	patch.m_nStitchX = 0u;
	patch.m_nStitchZ = 2u;
	patch.m_fBoundsMinY = 0.0f;
	patch.m_fBoundsMaxY = 0.5f;
	patch.m_fBoundsHalfExtent = 0.1f;
	patch.m_fPreviousOriginX = fOriginX;
	patch.m_fPreviousOriginZ = fOriginZ;
	patch.m_fPreviousScale = 1.0f;
	return patch;
}

static bool t3dRunRdCullingAndIndirectTests(
	t3dRdUnitTestState_t &rState ) {
	const Projection identity;
	t3dRdPatch_t	 lod0 = t3dRdMakeCullTestPatch( 0u, 2u, 0.0f, 0.0f );
	t3dRdPatch_t	 outer_near = t3dRdMakeCullTestPatch( 1u, 0u, 0.2f, 0.0f );
	t3dRdPatch_t	 outer_middle = t3dRdMakeCullTestPatch( 2u, 1u, -0.2f, 0.0f );
	t3dRdPatch_t	 outer_far = t3dRdMakeCullTestPatch( 3u, 2u, 0.0f, 0.2f );
	t3dRdPatch_t	 outside = t3dRdMakeCullTestPatch( 4u, 2u, 3.0f, 0.0f );
	rState.expect( Terrain3DRdCpuCuller::isPatchValid( lod0 ), "RD patch accepts stitch state zero" );
	rState.expect( Terrain3DRdCpuCuller::isPatchVisible( lod0, identity ) &&
					   !Terrain3DRdCpuCuller::isPatchVisible( outside, identity ),
				   "RD CPU reference culler separates visible and outside bounds" );
	rState.expect( Terrain3DRdCpuCuller::getDrawGroup( lod0 ) == T3D_RD_DRAW_GROUP_LOD0_NEAR &&
					   Terrain3DRdCpuCuller::getDrawGroup( outer_near ) == T3D_RD_DRAW_GROUP_OUTER_NEAR &&
					   Terrain3DRdCpuCuller::getDrawGroup( outer_middle ) == T3D_RD_DRAW_GROUP_OUTER_MIDDLE &&
					   Terrain3DRdCpuCuller::getDrawGroup( outer_far ) == T3D_RD_DRAW_GROUP_OUTER_FAR,
				   "RD CPU reference culler assigns all topology/tier groups" );
	t3dRdPatch_t invalid_stitch = lod0;
	invalid_stitch.m_nStitchX = 3u;
	rState.expect( !Terrain3DRdCpuCuller::isPatchValid( invalid_stitch ), "RD patch rejects stitch states above two" );
	t3dRdPatch_t invalid_bounds = lod0;
	invalid_bounds.m_fBoundsMinY = 2.0f;
	invalid_bounds.m_fBoundsMaxY = 1.0f;
	rState.expect( !Terrain3DRdCpuCuller::isPatchValid( invalid_bounds ), "RD patch rejects inverted vertical bounds" );

	t3dRdFrameSnapshot_t snapshot;
	snapshot.m_nPatchCount = 5u;
	snapshot.m_szPatches[0] = lod0;
	snapshot.m_szPatches[1] = outer_near;
	snapshot.m_szPatches[2] = outer_middle;
	snapshot.m_szPatches[3] = outer_far;
	snapshot.m_szPatches[4] = outside;
	t3dRdCpuCullResult_t result;
	rState.expect( Terrain3DRdCpuCuller::build( snapshot, identity, 12u, 24u, result ),
				   "RD CPU reference culler builds validated visible/indirect output" );
	rState.expect( result.m_nVisibleCount == 4u && result.m_nInvalidPatchCount == 0u,
				   "RD CPU reference culler reports exact visible and invalid counts" );
	bool groups_valid = true;
	for ( uint32_t group = 0u; group < T3D_RD_DRAW_GROUP_COUNT; ++group ) {
		const t3dRdIndexedIndirectCommand_t &command = result.m_szCommands[group];
		groups_valid = groups_valid && command.m_nIndexCount == ( group == 0u ? 12u : 24u ) &&
					   command.m_nInstanceCount == 1u && command.m_nFirstIndex == 0u && command.m_nVertexOffset == 0 &&
					   command.m_nFirstInstance == group * T3D_MAX_CLIPMAP_LEVELS &&
					   result.m_szVisibleIndices[group * T3D_MAX_CLIPMAP_LEVELS] == group;
	}
	rState.expect( groups_valid, "RD CPU culler emits bounded persistent indirect groups" );
	rState.expect( Terrain3DRdCpuCuller::validate( snapshot, identity, 12u, 24u, result ),
				   "RD CPU/GPU comparison validator accepts matching visible output" );
	t3dRdCpuCullResult_t corrupted = result;
	corrupted.m_szCommands[T3D_RD_DRAW_GROUP_OUTER_FAR].m_nFirstInstance = 1u;
	rState.expect( !Terrain3DRdCpuCuller::validate( snapshot, identity, 12u, 24u, corrupted ),
				   "RD indirect validation rejects a corrupt command offset" );

	rState.expect(
		Terrain3DRdCpuCuller::isCommandRangeValid( 3u, T3D_UNIT_TESTING_COMMAND_BUFFER_BYTES ) &&
			!Terrain3DRdCpuCuller::isCommandRangeValid( 3u, T3D_UNIT_TESTING_COMMAND_BUFFER_BYTES - 1u ) &&
			!Terrain3DRdCpuCuller::isCommandRangeValid( 4u, T3D_UNIT_TESTING_COMMAND_BUFFER_BYTES ) &&
			!Terrain3DRdCpuCuller::isCommandRangeValid( 0u, T3D_UNIT_TESTING_COMMAND_BUFFER_BYTES, 19u ) &&
			!Terrain3DRdCpuCuller::isCommandRangeValid( 0u, T3D_UNIT_TESTING_COMMAND_BUFFER_BYTES, 22u ) &&
			!Terrain3DRdCpuCuller::isCommandRangeValid( 3u, T3D_UNIT_TESTING_COMMAND_BUFFER_BYTES, 20u, 2u ) &&
			Terrain3DRdCpuCuller::isCommandRangeValid( 3u, 96u, 24u ),
		"RD indirect validation enforces command offsets and strides" );
	snapshot.m_szPatches[0].m_fScale = 0.0f;
	rState.expect( !Terrain3DRdCpuCuller::build( snapshot, identity, 12u, 24u, result ) &&
					   result.m_nInvalidPatchCount == 1u,
				   "RD CPU reference culler rejects invalid patch descriptors" );
	snapshot.m_nPatchCount = T3D_MAX_CLIPMAP_LEVELS + 1u;
	rState.expect( !Terrain3DRdCpuCuller::build( snapshot, identity, 12u, 24u, result ),
				   "RD CPU reference culler bounds-checks snapshot patch count" );
	return rState.m_nFailed == 0u;
}

static bool t3dRunRdContextDiagnosticTests(
	t3dRdUnitTestState_t &rState ) {
	Terrain3DRdContext context;
	rState.expect( context.getState() == T3D_RD_CONTEXT_STATE_UNINITIALIZED && !context.isReady() &&
					   !context.hasFailure() && context.getDevice() == NULL,
				   "RD context starts detached without hidden device ownership" );
	context.beginFrame( 41u, 9u );
	context.recordBufferUpdate( 128u );
	context.recordBufferUpdate( 64u );
	Dictionary diagnostics = context.getDiagnostics();
	rState.expect( ( int64_t ) ( diagnostics.get( "gpu_frame_id", 0 ) ) == 41 &&
					   ( int64_t ) ( diagnostics.get( "snapshot_generation", 0 ) ) == 9 &&
					   ( int64_t ) ( diagnostics.get( "buffer_updates_frame", 0 ) ) == 2 &&
					   ( int64_t ) ( diagnostics.get( "bytes_uploaded_frame", 0 ) ) == 192,
				   "RD context accounts per-frame buffer update diagnostics" );
	context.recordFailure( T3D_RD_CONTEXT_FAILURE_RESOURCE_CREATION, "sentinel" );
	diagnostics = context.getDiagnostics();
	rState.expect( context.hasFailure() && context.getState() == T3D_RD_CONTEXT_STATE_FAILED &&
					   context.getDevice() == NULL &&
					   String( diagnostics.get( "failure_state", String() ) ) == "resource_creation" &&
					   String( diagnostics.get( "last_failure", String() ) ) == "sentinel",
				   "RD context exposes explicit resource-creation failure state" );

	Terrain3DRdContext untracked_context;
	RID				   external_rid = UtilityFunctions::rid_from_int64( 0x12345678 );
	if ( external_rid.is_valid() ) {
		untracked_context.freeResource( external_rid );
	}
	diagnostics = untracked_context.getDiagnostics();
	rState.expect( external_rid.is_valid() && untracked_context.getState() == T3D_RD_CONTEXT_STATE_FAILED &&
					   untracked_context.getFailureCode() == T3D_RD_CONTEXT_FAILURE_UNTRACKED_RESOURCE &&
					   ( int64_t ) ( diagnostics.get( "untracked_free_attempts", 0 ) ) == 1,
				   "RD context rejects untracked RIDs without destroying external ownership" );
	return rState.m_nFailed == 0u;
}

static bool t3dRunRdMaterialAbiTests(
	t3dRdUnitTestState_t &rState ) {
	rState.expect( sizeof( t3dRdMaterialLayer_t ) == 80u && offsetof( t3dRdMaterialLayer_t, m_fColorR ) == 0u &&
					   offsetof( t3dRdMaterialLayer_t, m_fUvScale ) == 16u &&
					   offsetof( t3dRdMaterialLayer_t, m_fAoStrength ) == 32u &&
					   offsetof( t3dRdMaterialLayer_t, m_fDisplacementOffset ) == 48u &&
					   offsetof( t3dRdMaterialLayer_t, m_nPhysicalLayer ) == 56u &&
					   offsetof( t3dRdMaterialLayer_t, m_nFlags ) == 60u &&
					   offsetof( t3dRdMaterialLayer_t, m_nMicroVTAllocation ) == 64u &&
					   offsetof( t3dRdMaterialLayer_t, m_nMicroVTFlags ) == 72u,
				   "RD material layer ABI pins scalar, legacy texture-index, and micro-VT offsets" );
	rState.expect( sizeof( t3dRdTierMaterialConfig_t ) == 48u &&
					   offsetof( t3dRdTierMaterialConfig_t, m_nTextureSurfaceMode ) == 16u &&
					   offsetof( t3dRdTierMaterialConfig_t, m_fAuthoredColorContribution ) == 32u,
				   "RD tier material ABI pins quality modes and scalar offsets" );
	rState.expect( sizeof( t3dRdMaterialConfig_t ) == 384u &&
					   offsetof( t3dRdMaterialConfig_t, m_fDisplacementSharpness ) == 32u &&
					   offsetof( t3dRdMaterialConfig_t, m_fAutoSlope ) == 64u &&
					   offsetof( t3dRdMaterialConfig_t, m_nDualScaleTexture ) == 80u &&
					   offsetof( t3dRdMaterialConfig_t, m_fMacroVariation1R ) == 120u &&
					   offsetof( t3dRdMaterialConfig_t, m_fNoise1OffsetX ) == 152u &&
					   offsetof( t3dRdMaterialConfig_t, m_fWorldNoiseScale ) == 168u &&
					   offsetof( t3dRdMaterialConfig_t, m_fWorldNoiseOffsetX ) == 192u &&
					   offsetof( t3dRdMaterialConfig_t, m_fSilhouetteR ) == 208u &&
					   offsetof( t3dRdMaterialConfig_t, m_szTier ) == 240u,
				   "RD material config ABI pins the complete fixed GPU table layout" );
	rState.expect( T3D_RD_MATERIAL_LAYER_COUNT == 32u && T3D_RD_MATERIAL_LAYER_PACKED_SURFACE == 1u &&
					   T3D_RD_MATERIAL_LAYER_RESIDENT == 2u,
				   "RD material table keeps stable 32-layer indices and integral layer flags" );
	rState.expect( sizeof( t3dRdEditorDecal_t ) == 48u && offsetof( t3dRdEditorDecal_t, m_fColorR ) == 16u &&
					   offsetof( t3dRdEditorDecal_t, m_nVisible ) == 32u,
				   "RD editor decal ABI pins transform, color, and state offsets" );
	rState.expect( sizeof( t3dRdEditorOverlayConfig_t ) == 192u &&
					   offsetof( t3dRdEditorOverlayConfig_t, m_fHeightmapBlackHeight ) == 144u &&
					   offsetof( t3dRdEditorOverlayConfig_t, m_fContourColorR ) == 160u &&
					   offsetof( t3dRdEditorOverlayConfig_t, m_fInstancerCellSize ) == 176u,
				   "RD editor overlay ABI pins decal, debug-height, contour, and grid offsets" );
	t3dRdFrameSnapshot_t snapshot;
	rState.expect( snapshot.m_nMaterialLayerCount == 1u &&
					   snapshot.m_materialConfig.m_szTier[0].m_nSamplingMode == 3u,
				   "RD snapshot owns one deterministic material-config publication table" );
	return rState.m_nFailed == 0u;
}

static bool t3dRunRdInstanceAbiTests(
	t3dRdUnitTestState_t &rState ) {
	rState.expect( sizeof( t3dRdMeshHandle_t ) == 8u && sizeof( t3dRdSurfaceHandle_t ) == 8u,
				   "RD mesh and surface handles remain compact generation-safe pairs" );
	rState.expect( sizeof( t3dRdMeshVertex_t ) == 48u && offsetof( t3dRdMeshVertex_t, m_nColorRgba8 ) == 12u &&
					   offsetof( t3dRdMeshVertex_t, m_szNormal ) == 16u && offsetof( t3dRdMeshVertex_t, m_szUv ) == 32u,
				   "RD instance mesh vertex ABI pins packed attributes and stride" );
	rState.expect( sizeof( t3dRdInstanceRecord_t ) == 32u &&
					   offsetof( t3dRdInstanceRecord_t, m_nQuaternion01 ) == 12u &&
					   offsetof( t3dRdInstanceRecord_t, m_nColorRgba8 ) == 28u,
				   "RD regular-instance record remains 32 bytes with fixed packed offsets" );
	rState.expect( sizeof( t3dRdHlodCullDescriptor_t ) == 48u &&
					   offsetof( t3dRdHlodCullDescriptor_t, m_nVisibilityLayers ) == 32u &&
					   offsetof( t3dRdHlodCullDescriptor_t, m_nCommandIndex ) == 36u &&
					   offsetof( t3dRdHlodCullDescriptor_t, m_nFirstVisible ) == 40u,
				   "RD HLOD cull descriptor pins bounds, visibility, and indirect command offsets" );
	const uint64_t regular_key_a = t3dRdRegularMeshSourceKey( 4u, 1 );
	const uint64_t regular_key_b = t3dRdRegularMeshSourceKey( 4u, 2 );
	const uint64_t hlod_key_a = t3dRdHlodMeshSourceKey( 0u );
	const uint64_t hlod_key_b = t3dRdHlodMeshSourceKey( 1u );
	rState.expect( regular_key_a != regular_key_b && hlod_key_a != hlod_key_b && regular_key_a != hlod_key_a &&
					   ( regular_key_a & 0xF000000000000000ull ) == 0x1000000000000000ull &&
					   ( hlod_key_a & 0xF000000000000000ull ) == 0x2000000000000000ull &&
					   ( T3D_RD_INSTANCE_HLOD & T3D_RD_INSTANCE_FADE_MASK ) == 0u,
				   "RD HLOD source and instance namespaces remain disjoint from regular LOD/fade state" );
	rState.expect( t3dRdPackGodotRgba32ForGpu( 0x11223344u ) == 0x44332211u &&
					   t3dRdPackGodotRgba32ForGpu( 0xFFFFFFFFu ) == 0xFFFFFFFFu,
				   "RD RGBA8 packing converts Godot numeric channel order to GPU byte order" );
	rState.expect( sizeof( t3dRdInstanceMaterial_t ) == 64u &&
					   offsetof( t3dRdInstanceMaterial_t, m_fNormalScale ) == 48u,
				   "RD regular-instance material table remains std430-compatible with scalar/raster state; texture "
				   "identity resides in VT "
				   "material records" );
	t3dRdInstanceMaterial_t material;
	material.m_nFlags = T3D_RD_INSTANCE_MATERIAL_VERTEX_COLOR | T3D_RD_INSTANCE_MATERIAL_NORMAL_MAPPING |
						T3D_RD_INSTANCE_MATERIAL_TEXTURE_REPEAT | T3D_RD_INSTANCE_MATERIAL_CULL_FRONT |
						T3D_RD_INSTANCE_MATERIAL_ALPHA_BLEND | T3D_RD_INSTANCE_MATERIAL_ALPHA_DEPTH_PREPASS |
						T3D_RD_INSTANCE_MATERIAL_ROUGHNESS_TEXTURE;
	material.m_fNormalScale = 0.4f;
	rState.expect(
		( material.m_nFlags & T3D_RD_INSTANCE_MATERIAL_VERTEX_COLOR ) != 0u &&
			( material.m_nFlags & T3D_RD_INSTANCE_MATERIAL_NORMAL_MAPPING ) != 0u &&
			( material.m_nFlags & T3D_RD_INSTANCE_MATERIAL_TEXTURE_REPEAT ) != 0u &&
			( material.m_nFlags & T3D_RD_INSTANCE_MATERIAL_CULL_FRONT ) != 0u &&
			( material.m_nFlags & T3D_RD_INSTANCE_MATERIAL_ALPHA_BLEND ) != 0u &&
			( material.m_nFlags & T3D_RD_INSTANCE_MATERIAL_ALPHA_DEPTH_PREPASS ) != 0u &&
			( material.m_nFlags & T3D_RD_INSTANCE_MATERIAL_ROUGHNESS_TEXTURE ) != 0u &&
			Math::is_equal_approx( material.m_fNormalScale, 0.4f ),
		"RD regular-instance material ABI preserves filtering/culling/vertex-color/normal/alpha/roughness state" );
	const t3dRdMeshHandle_t valid_handle{ 7u, 3u };
	const t3dRdMeshHandle_t stale_handle{ 7u, 2u };
	rState.expect( valid_handle.valid() && stale_handle.valid() && !( valid_handle == stale_handle ),
				   "RD mesh handles distinguish stale generations at identical slots" );
	rState.expect( Terrain3DRdMeshCache::nextArenaCapacity( 1u, 4u * 1024u * 1024u ) == 4u * 1024u * 1024u &&
					   Terrain3DRdMeshCache::nextArenaCapacity( 4u * 1024u * 1024u + 1u, 4u * 1024u * 1024u ) ==
						   8u * 1024u * 1024u,
				   "RD mesh arenas use deterministic power-of-two growth above the static minimum" );
	t3dRdInstanceCell_t cell;
	cell.m_nFirstInstance = 17u;
	cell.m_nInstanceCount = 23u;
	cell.m_nFlags = T3D_RD_INSTANCE_CAST_SHADOWS | T3D_RD_INSTANCE_SHADOW_DOUBLE_SIDED;
	rState.expect( cell.m_nFirstInstance == 17u && cell.m_nInstanceCount == 23u &&
					   ( cell.m_nFlags & T3D_RD_INSTANCE_CAST_SHADOWS ) != 0u &&
					   ( cell.m_nFlags & T3D_RD_INSTANCE_SHADOW_DOUBLE_SIDED ) != 0u &&
					   ( cell.m_nFlags & T3D_RD_INSTANCE_DOUBLE_SIDED ) == 0u,
				   "RD instance cell publication keeps shadow double-sided state separate from color-pass culling" );
	return rState.m_nFailed == 0u;
}

static bool t3dRunRdTextureLayoutTests(
	t3dRdUnitTestState_t &rState ) {
	RenderingDevice::DataFormat format = RenderingDevice::DATA_FORMAT_MAX;
	rState.expect(
		Terrain3DRdTextureManager::imageFormatToDataFormat( Image::FORMAT_R16, T3D_RD_TEXTURE_LINEAR_DATA, format ) &&
			format == RenderingDevice::DATA_FORMAT_R16_UNORM,
		"RD texture layout maps R16 height data explicitly" );
	rState.expect(
		Terrain3DRdTextureManager::imageFormatToDataFormat( Image::FORMAT_RGBA8, T3D_RD_TEXTURE_LINEAR_DATA, format ) &&
			format == RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
		"RD texture layout keeps RGBA8 data textures linear" );
	rState.expect( Terrain3DRdTextureManager::imageFormatToDataFormat( Image::FORMAT_RGBA8,
																	   T3D_RD_TEXTURE_SOURCE_COLOR,
																	   format ) &&
					   format == RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB,
				   "RD texture layout maps RGBA8 source-color textures to sRGB sampling" );
	rState.expect(
		Terrain3DRdTextureManager::imageFormatToDataFormat( Image::FORMAT_RF, T3D_RD_TEXTURE_LINEAR_DATA, format ) &&
			format == RenderingDevice::DATA_FORMAT_R32_SFLOAT,
		"RD texture layout maps RF authoring data explicitly" );
	rState.expect(
		!Terrain3DRdTextureManager::imageFormatToDataFormat( Image::FORMAT_R16I, T3D_RD_TEXTURE_LINEAR_DATA, format ) &&
			format == RenderingDevice::DATA_FORMAT_MAX,
		"RD texture layout rejects unsupported integer image formats" );

	const Ref< Image >	 image = Image::create_empty( 8, 4, false, Image::FORMAT_R16 );
	t3dRdTextureLayout_t layout;
	rState.expect(
		image.is_valid() &&
			Terrain3DRdTextureManager::buildLayout( image, 3u, true, 192u, T3D_RD_TEXTURE_LINEAR_DATA, layout ),
		"RD texture layout accepts a complete generated array" );
	rState.expect( layout.m_type == RenderingDevice::TEXTURE_TYPE_2D_ARRAY && layout.m_nWidth == 8u &&
					   layout.m_nHeight == 4u && layout.m_nLayers == 3u && layout.m_nMipmaps == 1u &&
					   layout.m_nBytes == 192u,
				   "RD texture layout preserves dimensions, layer count, mip count, and bytes" );
	const uint64_t required_usage = ( uint64_t ) ( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT ) |
									( uint64_t ) ( RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT ) |
									( uint64_t ) ( RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT ) |
									( uint64_t ) ( RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );
	rState.expect( ( layout.m_nUsageBits & required_usage ) == required_usage,
				   "RD generated textures expose sampling, dirty update, and copy usage" );
	rState.expect(
		!Terrain3DRdTextureManager::buildLayout( image, 0u, true, 192u, T3D_RD_TEXTURE_LINEAR_DATA, layout ) &&
			!Terrain3DRdTextureManager::buildLayout( image, 3u, true, 0u, T3D_RD_TEXTURE_LINEAR_DATA, layout ),
		"RD texture layout rejects zero-layer and zero-byte resources" );
	Terrain3DGeneratedTexture generated_a;
	Terrain3DGeneratedTexture generated_b;
	rState.expect( !generated_a.getRid().is_valid() && generated_a.size() == 0 && generated_a.payloadBytes() == 0u &&
					   generated_a.maxMipmapLevel() == 0 && generated_a.getImage().is_null(),
				   "GeneratedTexture default state contains no duplicate physical texture authority" );
	generated_a.swap( generated_b );
	rState.expect( !generated_a.getRid().is_valid() && !generated_b.getRid().is_valid(),
				   "GeneratedTexture swap preserves canonical empty texture ownership" );
	return rState.m_nFailed == 0u;
}
template< typename TLeft, typename TRight >
static EA_FORCE_INLINE void t3dUnitTestingLogDiffers(
	const TLeft	 &rA,
	const TRight &rB,
	const String &rDescription,
	const bool	  bExpected ) {
	const bool	 bDiffersResult = t3dDiffers( rA, rB );
	const String result = bDiffersResult == bExpected ? "PASSED" : "FAILED";
	UtilityFunctions::print( rDescription, ": differs(", rA, ", ", rB, ") = ", bDiffersResult, " - ", result );
}

void t3dTestDiffers(
	void ) {
	UtilityFunctions::print( "=== Testing differs function ===" );

	// 1. Scalars: int, float (should use ==, differs if values differ)
	{
		int i1 = 42;
		int i2 = 42; // Same
		int i3 = 43; // Diff
		t3dUnitTestingLogDiffers( i1, i2, "int same", false );
		t3dUnitTestingLogDiffers( i1, i3, "int diff", true );

		double fF1 = 3.14;
		double fF2 = 3.14;	  // Same
		double fF3 = 3.14159; // Diff
		t3dUnitTestingLogDiffers( fF1, fF2, "float same", false );
		t3dUnitTestingLogDiffers( fF1, fF3, "float diff", true );
	}

	// 2. Vectors: Vector2, Vector2i, Vector3, Vector3i (should use ==)
	{
		Vector2 v21( 1.0, 2.0 );
		Vector2 v22( 1.0, 2.0 ); // Same
		Vector2 v23( 1.0, 3.0 ); // Diff
		t3dUnitTestingLogDiffers( v21, v22, "Vector2 same", false );
		t3dUnitTestingLogDiffers( v21, v23, "Vector2 diff", true );

		Vector2i v2i1( 1, 2 );
		Vector2i v2i2( 1, 2 ); // Same
		Vector2i v2i3( 1, 3 ); // Diff
		t3dUnitTestingLogDiffers( v2i1, v2i2, "Vector2i same", false );
		t3dUnitTestingLogDiffers( v2i1, v2i3, "Vector2i diff", true );

		Vector3 v31( 1.0, 2.0, 3.0 );
		Vector3 v32( 1.0, 2.0, 3.0 ); // Same
		Vector3 v33( 1.0, 2.0, 4.0 ); // Diff
		t3dUnitTestingLogDiffers( v31, v32, "Vector3 same", false );
		t3dUnitTestingLogDiffers( v31, v33, "Vector3 diff", true );

		Vector3i v3i1( 1, 2, 3 );
		Vector3i v3i2( 1, 2, 3 ); // Same
		Vector3i v3i3( 1, 2, 4 ); // Diff
		t3dUnitTestingLogDiffers( v3i1, v3i2, "Vector3i same", false );
		t3dUnitTestingLogDiffers( v3i1, v3i3, "Vector3i diff", true );
	}

	// 3. String: Share (COW), same value diff ptr, diff value
	{
		String s1 = "test";
		String s2 = s1;		 // Shared (COW)
		String s3( "test" ); // Separate alloc, same value
		String s4 = "diff";
		t3dUnitTestingLogDiffers( String(), String(), "String() vs String()", false );
		t3dUnitTestingLogDiffers( String(), String( "a" ), "String() vs String('a')", true );
		t3dUnitTestingLogDiffers( s1, s2, "String shared", false );
		t3dUnitTestingLogDiffers( s1, s3, "String same value diff ptr", false ); // Length match + == true
		t3dUnitTestingLogDiffers( s1, s4, "String diff value", true );			 // Length may match, but == false
	}

	// 4. StringName: Similar to String (uses ==, but test sharing if applicable)
	{
		StringName sn1 = "test";
		StringName sn2 = sn1;	  // Shared
		StringName sn3( "test" ); // Separate, same value
		StringName sn4 = "diff";
		t3dUnitTestingLogDiffers( sn1, sn2, "StringName shared", false );
		t3dUnitTestingLogDiffers( sn1, sn3, "StringName same value diff ptr", false ); // == handles
		t3dUnitTestingLogDiffers( sn1, sn4, "StringName diff value", true );
	}

	// 5. Array: Share vs mutate
	{
		Array arr1;
		arr1.push_back( 42 );
		Array arr2 = arr1;	  // Shared
		Array arr3;			  // Empty diff
		arr3.push_back( 42 ); // Same content, but separate (differs=true, conservative)
		Array empty_arr;	  // Explicit empty for size diff
		t3dUnitTestingLogDiffers( arr1, arr2, "Array shared", false ); // Same self ptr
		t3dUnitTestingLogDiffers( arr1,
								  arr3,
								  "Array same content diff ptr",
								  true );									  // Diff self ptr (no fallback for Array)
		t3dUnitTestingLogDiffers( arr1, empty_arr, "Array size diff", true ); // Size mismatch
	}

	// 6. TypedArray: e.g., TypedArray<int>
	{
		TypedArray< int > ta1;
		ta1.push_back( 42 );
		TypedArray< int > ta2 = ta1;													// Shared
		TypedArray< int > ta3;															// Empty
		ta3.push_back( 42 );															// Separate
		TypedArray< int > empty_ta;														// Explicit empty
		t3dUnitTestingLogDiffers( ta1, ta2, "TypedArray shared", false );				// Via Array base
		t3dUnitTestingLogDiffers( ta1, ta3, "TypedArray same content diff ptr", true ); // Diff self
		t3dUnitTestingLogDiffers( ta1, empty_ta, "TypedArray size diff", true );		// Size mismatch
	}

	// 7. Dictionary: Share vs mutate
	{
		Dictionary dict1;
		dict1["key"] = 42;
		Dictionary dict2 = dict1; // Shared
		Dictionary dict3;		  // Empty
		dict3["key"] = 42;		  // Separate
		Dictionary empty_dict;	  // Explicit empty
		t3dUnitTestingLogDiffers( dict1, dict2, "Dictionary shared", false );
		t3dUnitTestingLogDiffers( dict1, dict3, "Dictionary same content diff ptr", true ); // Diff self
		t3dUnitTestingLogDiffers( dict1, empty_dict, "Dictionary size diff", true );		// Size mismatch
	}

	// 8. Variant types: Wrap above (falls to ==)
	{
		Variant v_int1 = 42;
		Variant v_int2 = 42; // Same
		Variant v_int3 = 43; // Diff
		t3dUnitTestingLogDiffers( v_int1, v_int2, "Variant int same", false );
		t3dUnitTestingLogDiffers( v_int1, v_int3, "Variant int diff", true );

		Variant v_float1 = 3.14;
		Variant v_float2 = 3.14;
		Variant v_float3 = 3.14159;
		t3dUnitTestingLogDiffers( v_float1, v_float2, "Variant float same", false );
		t3dUnitTestingLogDiffers( v_float1, v_float3, "Variant float diff", true );

		Variant v_str1 = String( "test" );
		Variant v_str2 = String( "test" );
		Variant v_str3 = String( "diff" );
		t3dUnitTestingLogDiffers( v_str1, v_str2, "Variant String same", false );
		t3dUnitTestingLogDiffers( v_str1, v_str3, "Variant String diff", true );

		// Variant Object (use RefCounted for ref support)
		Ref< RefCounted > pRc1 = memnew( RefCounted );
		Ref< RefCounted > pRc2 = pRc1;				   // Same ref
		Ref< RefCounted > pRc3 = memnew( RefCounted ); // Diff
		Variant			  v_rc1 = pRc1;
		Variant			  v_rc2 = pRc2;
		Variant			  v_rc3 = pRc3;
		t3dUnitTestingLogDiffers( v_rc1, v_rc2, "Variant RefCounted same ref", false ); // Ref == true
		t3dUnitTestingLogDiffers( v_rc1, v_rc3, "Variant RefCounted diff ref", true );

		// Variant Array
		Array arr_var1;
		arr_var1.push_back( 42 );
		Variant v_arr1 = arr_var1;
		Array	arr_var2 = arr_var1;
		Variant v_arr2 = arr_var2;
		t3dUnitTestingLogDiffers( v_arr1, v_arr2, "Variant Array shared", false ); // Variant == checks inner

		// Variant Dictionary
		Dictionary dict_var1;
		dict_var1["key"] = 42;
		Variant	   v_dict1 = dict_var1;
		Dictionary dict_var2 = dict_var1;
		Variant	   v_dict2 = dict_var2;
		t3dUnitTestingLogDiffers( v_dict1, v_dict2, "Variant Dictionary shared", false );
	}

	UtilityFunctions::print( "=== End differs tests ===" );
}

bool t3dTestRdTopology(
	void ) {
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD topology contracts ===" );
	UtilityFunctions::print( "=== RD topology tests: ", state.m_nPassed, " passed, ", state.m_nFailed, " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dTestRdAbiAndPublication(
	void ) {
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD ABI/publication contracts ===" );
	t3dRunRdAbiAndPublicationTests( state );
	t3dRunRdDescriptorContractTests( state );
	UtilityFunctions::print( "=== RD ABI/publication tests: ",
							 state.m_nPassed,
							 " passed, ",
							 state.m_nFailed,
							 " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dTestRdCullingAndIndirect(
	void ) {
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD culling/indirect contracts ===" );
	t3dRunRdCullingAndIndirectTests( state );
	UtilityFunctions::print( "=== RD culling/indirect tests: ",
							 state.m_nPassed,
							 " passed, ",
							 state.m_nFailed,
							 " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dTestRdContextDiagnostics(
	void ) {
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD context diagnostics ===" );
	t3dRunRdContextDiagnosticTests( state );
	UtilityFunctions::print( "=== RD context tests: ", state.m_nPassed, " passed, ", state.m_nFailed, " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dTestRdTextureLayouts(
	void ) {
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD texture layout contracts ===" );
	t3dRunRdTextureLayoutTests( state );
	UtilityFunctions::print( "=== RD texture layout tests: ",
							 state.m_nPassed,
							 " passed, ",
							 state.m_nFailed,
							 " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dTestRdMaterialAbi(
	void ) {
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD material ABI contracts ===" );
	t3dRunRdMaterialAbiTests( state );
	UtilityFunctions::print( "=== RD material ABI tests: ",
							 state.m_nPassed,
							 " passed, ",
							 state.m_nFailed,
							 " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dTestRdInstanceAbi(
	void ) {
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD regular-instance ABI contracts ===" );
	t3dRunRdInstanceAbiTests( state );
	UtilityFunctions::print( "=== RD regular-instance ABI tests: ",
							 state.m_nPassed,
							 " passed, ",
							 state.m_nFailed,
							 " failed ===" );
	return state.m_nFailed == 0u;
}

static bool t3dRunHlodArrayMeshSurfaceTests(
	t3dRdUnitTestState_t &rState ) {
	Ref< ArrayMesh > mesh;
	mesh.instantiate();
	Array arrays;
	arrays.resize( Mesh::ARRAY_MAX );
	PackedVector3Array vertices;
	vertices.resize( 3 );
	vertices.set( 0, Vector3( 0.0f, 0.0f, 0.0f ) );
	vertices.set( 1, Vector3( 1.0f, 0.0f, 0.0f ) );
	vertices.set( 2, Vector3( 0.0f, 0.0f, 1.0f ) );
	PackedInt32Array indices;
	indices.resize( 3 );
	indices.set( 0, 0 );
	indices.set( 1, 1 );
	indices.set( 2, 2 );
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->add_surface_from_arrays( Mesh::PRIMITIVE_TRIANGLES, arrays );

	Ref< Terrain3DHLODBuilder > builder;
	builder.instantiate();
	const Dictionary result = builder->simplifyMesh( mesh, 1.0f );
	const bool		 valid_surface =
		mesh->get_surface_count() == 1 && mesh->surface_get_primitive_type( 0 ) == Mesh::PRIMITIVE_TRIANGLES;
	const bool nonzero_triangles =
		( bool ) result.get( "ok", false ) && ( int64_t ) result.get( "source_triangles", 0 ) > 0;
	rState.expect( valid_surface && nonzero_triangles,
				   "HLOD ArrayMesh surface inspection reports non-zero triangle geometry" );
	return valid_surface && nonzero_triangles;
}

static void t3dRunReductionBudgetTests(
	t3dRdUnitTestState_t &rState ) {
	t3dWorldResourceLimits_t low{};
	low.m_nMaxSourceVertices = 1048576u;
	low.m_nMaxClusters = 65536u;
	low.m_nMaxClusterVertexIndices = 65536u * T3D_WORLD_CLUSTER_MAX_VERTICES;
	low.m_nMaxTriangleSlots = 65536u * T3D_WORLD_CLUSTER_MAX_TRIANGLES;
	low.m_nMaxTerrainClusters = 32768u;
	low.m_nMaxTransforms = 32768u;
	low.m_nMaxVisibleItems = 65536u;
	low.m_nMaxMaterials = 4096u;
	const uint32_t			 lowBytes = Terrain3DWorldResources::estimateSharedColdArenaBytes( low );
	t3dWorldResourceLimits_t high = low;
	high.m_nMaxSourceVertices *= 2u;
	high.m_nMaxClusters *= 2u;
	high.m_nMaxClusterVertexIndices *= 2u;
	high.m_nMaxTriangleSlots *= 2u;
	const uint32_t highBytes = Terrain3DWorldResources::estimateSharedColdArenaBytes( high );
	rState.expect( lowBytes >= 96u * 1024u * 1024u && lowBytes < 128u * 1024u * 1024u && highBytes > lowBytes &&
					   highBytes <= Terrain3DRdContext::COLD_ARENA_BYTES,
				   "Profile-sized COLD arena preserves configurable world geometry budgets" );
	rState.expect( Terrain3DRdContext::HOT_RECORD_CAPACITY == 256u &&
					   Terrain3DRdContext::HOT_FRAME_BYTES == 256u * sizeof( Terrain3DRdHotRecord ),
				   "HOT staging halves per-slot capacity without changing record ABI" );
	rState.expect( sizeof( t3dWorldTerrainCluster_t ) == 32u && offsetof( t3dWorldTerrainCluster_t, m_nFlags ) == 16u,
				   "Procedural strip clusters retain the 32-byte GPU topology contract" );
	Terrain3DWorldPublication reuseWorld;
	reuseWorld.m_vertices.resize( 2u );
	reuseWorld.m_clusterVertexIndices.resize( 4u );
	reuseWorld.m_materials.resize( 1u );
	reuseWorld.m_nMeshPayloadSignature = 42u;
	reuseWorld.clearForBuild( true );
	rState.expect( reuseWorld.m_vertices.size() == 2u && reuseWorld.m_clusterVertexIndices.size() == 4u &&
					   reuseWorld.m_materials.empty() && reuseWorld.m_nMeshPayloadSignature == 42u,
				   "Reused publication bank retains mesh topology but clears mutable tables" );
	reuseWorld.clearForBuild();
	rState.expect( reuseWorld.m_vertices.empty() && reuseWorld.m_clusterVertexIndices.empty() &&
					   reuseWorld.m_nMeshPayloadSignature == 0u,
				   "Cold publication reset invalidates the bank-local mesh payload identity" );
	Terrain3DWorldPublication emptyWorld;
	Terrain3DWorldResources	  emptyResources;
	uint32_t				  changedClusters = UINT32_MAX;
	const uint64_t			  uninitializedBudget =
		emptyResources.estimateChangedPublicationUploadBytes( emptyWorld, false, changedClusters );
	rState.expect( uninitializedBudget == Terrain3DWorldResources::estimatePublicationUploadBytes( emptyWorld ) &&
					   changedClusters == 0u,
				   "Publication preflight uses gross bytes only before GPU buffers exist" );
	rState.expect( sizeof( t3dRdDescriptorRecipe_t ) <= 808u,
				   "Descriptor recipe compacts bounded binding/resource metadata" );
	rState.expect( sizeof( Terrain3DCollision ) < 64u * 1024u,
				   "Collision owner does not contain the 4096-record shape pool" );
	rState.expect( sizeof( t3dRuntimeProfileHistory_t ) <= 56u * 1024u,
				   "Runtime profile history uses the reduced fixed window" );
}

bool t3dRunRdCpuTests(
	void ) {
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Running Terrain3D RD CPU tests ===" );
	t3dRunEastlContainerOwnershipTests( state );
	t3dRunReductionBudgetTests( state );
	t3dRunRdAbiAndPublicationTests( state );
	t3dRunRdCullingAndIndirectTests( state );
	t3dRunRdContextDiagnosticTests( state );
	t3dRunRdTextureLayoutTests( state );
	t3dRunRdMaterialAbiTests( state );
	t3dRunRdInstanceAbiTests( state );
	t3dRunHlodArrayMeshSurfaceTests( state );
	state.expect( t3dRunMegaVTCacheTests(), "MegaVT region cache startup regression suite" );
	state.expect( t3dRunVTWarmRepublishTests(), "VT warm republish regression suite" );
	UtilityFunctions::print( "=== Terrain3D RD CPU tests: ",
							 state.m_nPassed,
							 " passed, ",
							 state.m_nFailed,
							 " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dRunCentralizationTests(
	void ) {
	t3dRdUnitTestState_t state;
	uint32_t			 startupShaders = 0u;
	bool				 startupDomainsValid = true;
	bool				 rasterVariantsDeferred = true;
	for ( uint32_t value = 0u; value < T3D_RD_SHADER_COUNT; ++value ) {
		const t3dRdShaderId_t id = ( t3dRdShaderId_t ) value;
		const bool			  required = t3dRdShaderNeedsStartupPhysical( id );
		if ( required ) {
			++startupShaders;
			startupDomainsValid =
				startupDomainsValid && t3dRdShaderCatalogGet( id ).m_domain == T3D_RD_SHADER_GLOBAL_DEVICE;
		}
		if ( value < T3D_RD_SHADER_NEAR_ALIAS ||
			 ( value >= T3D_RD_SHADER_INSTANCE_FORWARD_BASE && value < T3D_RD_SHADER_INSTANCE_ALIAS ) ||
			 ( value >= T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE && value < T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW ) ) {
			rasterVariantsDeferred = rasterVariantsDeferred && !required;
		}
	}
	state.expect( startupShaders == 14u && startupDomainsValid,
				  "Startup creates 14 global-device infrastructure and unified shaders" );
	state.expect( rasterVariantsDeferred,
				  "Terrain, one-sun, instance and world pass variants are deferred until PREPARE" );
	state.expect( t3dRdShaderNeedsStartupPhysical( T3D_RD_SHADER_UCMP_ALIAS ) &&
					  t3dRdShaderNeedsStartupPhysical( T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW ) &&
					  t3dRdShaderNeedsStartupPhysical( T3D_RD_SHADER_WORLD_RASTER_MESH_SHADOW ) &&
					  t3dRdShaderNeedsStartupPhysical( T3D_RD_SHADER_DISPLACEMENT_COMPUTE ),
				  "Raster aliases, fixed shadows and displacement remain available at startup" );
	state.expect( !t3dRdShaderNeedsStartupPhysical( T3D_RD_SHADER_BRUSH_COMPUTE ) &&
					  !t3dRdShaderNeedsStartupPhysical( T3D_RD_SHADER_DERIVED_COMPUTE ) &&
					  !t3dRdShaderNeedsStartupPhysical( T3D_RD_SHADER_COUNT ),
				  "Local-device programs and invalid IDs are excluded from global prewarm" );
	if ( Terrain3DRdContext::isOnRenderThread() && Terrain3DRdShaderLibrary::physicalCacheReady() ) {
		Terrain3DRdContext	 &context = Terrain3DRdContext::getShared();
		const t3dRdShaderId_t requested[] = {
			t3dRdTerrainForwardId( T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL, 0u, 0u ),
			t3dRdTerrainForwardId( T3D_TERRAIN_SHADER_PATH_EDITOR_DEBUG_MIXED, 0u, 0u ),
			t3dRdTerrainOneSunId( T3D_TERRAIN_SHADER_PATH_EDITOR_DEBUG_MIXED, 0u, 0u ),
			T3D_RD_SHADER_INSTANCE_FORWARD_BASE,
			T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE,
			T3D_RD_SHADER_WORLD_RASTER_MESH_BASE };
		for ( const t3dRdShaderId_t id : requested ) {
			Terrain3DRdShader first, second;
			const bool		  created =
				first.createTracked( context, T3D_RD_CONTEXT_OWNER_TERRAIN, "T3D.Test.Lazy.First", id );
			const bool reused =
				second.createTracked( context, T3D_RD_CONTEXT_OWNER_TERRAIN, "T3D.Test.Lazy.Second", id );
			const RID physical = first.rid();
			state.expect( created && reused && physical.is_valid() && physical == second.rid(),
						  "Deferred raster variant reuses one physical shader across borrowers" );
			first.releaseTracked( context );
			second.releaseTracked( context );
			state.expect( Terrain3DRdShaderLibrary::borrowPhysical( id ) == physical,
						  "Borrower release retains the physical shader until the final cache user exits" );
		}
		state.expect(
			!Terrain3DRdShaderLibrary::acquirePhysicalCold( context, T3D_RD_SHADER_BRUSH_COMPUTE ).is_valid() &&
				!Terrain3DRdShaderLibrary::acquirePhysicalCold( context, T3D_RD_SHADER_COUNT ).is_valid() &&
				Terrain3DRdShaderLibrary::physicalCacheReady(),
			"Local and invalid requests cannot create global shaders or invalidate the ready cache" );
	}
	for ( uint32_t mode = 0u; mode < 3u; ++mode ) {
		for ( uint32_t cull = 0u; cull < 3u; ++cull ) {
			t3dRdForwardPipelineBaseKey_t key;
			key.m_samples = RenderingDevice::TEXTURE_SAMPLES_4;
			t3dRdForwardPipelineStateOptions_t options;
			options.m_cullMode = ( RenderingDevice::PolygonCullMode ) cull;
			options.m_frontFace = RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE;
			options.m_depthCompare = RenderingDevice::COMPARE_OP_LESS_OR_EQUAL;
			options.m_nBlendAttachmentCount = 1u;
			options.m_bBlendFirstAttachment = mode == 1u;
			options.m_bEnableDepthWrite = mode != 1u;
			options.m_bColorWrite = mode != 2u;
			t3dRdForwardPipelineStates_t values;
			const bool					 created = t3dRdBuildForwardPipelineStates( key, options, values );
			state.expect( created, "Pipeline states exist for every opacity/cull combination" );
			if ( !created ) {
				continue;
			}
			const Ref< RDPipelineColorBlendStateAttachment > blend = values.m_blend->get_attachments()[0];
			state.expect( values.m_multisample->get_sample_count() == RenderingDevice::TEXTURE_SAMPLES_4 &&
							  values.m_raster->get_cull_mode() == options.m_cullMode &&
							  values.m_raster->get_front_face() == RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE &&
							  values.m_depth->get_enable_depth_test() &&
							  values.m_depth->get_enable_depth_write() == ( mode != 1u ) &&
							  values.m_depth->get_depth_compare_operator() == RenderingDevice::COMPARE_OP_LESS_OR_EQUAL,
						  "Pipeline sample, cull, winding and depth policy is preserved" );
			state.expect( blend->get_enable_blend() == ( mode == 1u ) && blend->get_write_r() == ( mode != 2u ) &&
							  blend->get_write_g() == ( mode != 2u ) && blend->get_write_b() == ( mode != 2u ) &&
							  blend->get_write_a() == ( mode != 2u ),
						  "Transparent blending and depth-only RGBA masks are preserved" );
			if ( mode == 1u ) {
				state.expect(
					blend->get_src_color_blend_factor() == RenderingDevice::BLEND_FACTOR_SRC_ALPHA &&
						blend->get_dst_color_blend_factor() == RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA &&
						blend->get_src_alpha_blend_factor() == RenderingDevice::BLEND_FACTOR_ONE &&
						blend->get_dst_alpha_blend_factor() == RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
					"Transparent color and alpha use the original blend factors" );
			}
		}
	}

	t3dCpuRegionCacheStats_t counters;
	counters.m_nLoadBytesLastTick = UINT64_MAX;
	counters.m_nServiceUsecLast = ( uint64_t ) INT64_MAX;
	Dictionary stats;
	t3dAppendRegionLoadCacheStats( stats, counters, 16, 7 );
	state.expect( ( int64_t ) stats["load_bytes_last_tick"] == -1 &&
					  ( int64_t ) stats["service_cpu_usec_last"] == INT64_MAX &&
					  ( int64_t ) stats["demand_loads"] == 7 && stats.size() == 11,
				  "Cache counters preserve signed 64-bit Variant conversion and common schema" );

	const String path =
		ProjectSettings::get_singleton()->globalize_path( "res://../validation/centralization_writer_contract.bin" );
	const String staged = path + String( ".staged" ), previous = path + String( ".previous" );
	const bool	 fresh =
		!FileAccess::file_exists( path ) && !FileAccess::file_exists( staged ) && !FileAccess::file_exists( previous );
	state.expect( fresh, "Publication fixture paths are unused" );
	if ( !fresh ) {
		return false;
	}
	PackedByteArray original, replacement;
	original.append( 0x13u );
	replacement.append( 0x42u );
	replacement.append( 0x7fu );
	state.expect( t3dPublishPackedFile( path, original, NULL ) == OK,
				  "Packed publication writes the exact initial bytes" );
	Terrain3DFilePublication transaction( path, staged, previous );
	state.expect( transaction.stagePacked( replacement ) == OK && FileAccess::get_file_as_bytes( path ) == original &&
					  transaction.publish() == OK && FileAccess::get_file_as_bytes( path ) == replacement,
				  "Staging preserves the old artifact until publication" );
	state.expect( transaction.rollback() == OK && FileAccess::get_file_as_bytes( path ) == original,
				  "Rejected publication restores the original artifact" );
	Terrain3DFilePublication shortWrite( path, staged, previous );
	Ref< FileAccess >		 file;
	state.expect( shortWrite.openStaged( file ) == OK, "Scalar serializers share staged stream opening" );
	if ( file.is_valid() ) {
		file->store_32( 0x12345678u );
		state.expect( shortWrite.finishStaged( file, 8u ) == ERR_FILE_CANT_WRITE && file.is_null() &&
						  !FileAccess::file_exists( staged ) && FileAccess::get_file_as_bytes( path ) == original,
					  "Short write is rejected, closed and cleaned without replacing the original" );
	}
	Terrain3DFilePublication accepted( path, staged, previous );
	state.expect( accepted.stagePacked( replacement ) == OK && accepted.publish() == OK && accepted.commit() == OK &&
					  FileAccess::get_file_as_bytes( path ) == replacement && !FileAccess::file_exists( previous ),
				  "Accepted publication retains exact bytes and releases the backup" );
	DirAccess::remove_absolute( staged );
	DirAccess::remove_absolute( previous );
	DirAccess::remove_absolute( path );
	UtilityFunctions::print( "=== Centralization tests: ",
							 state.m_nPassed,
							 " passed, ",
							 state.m_nFailed,
							 " failed ===" );
	return state.m_nFailed == 0u;
}
