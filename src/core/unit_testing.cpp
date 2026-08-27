// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/unit_testing.h"

#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_util.h"
#include "rendering/rd/terrain_3d_rd_culling.h"
#include "rendering/rd/terrain_3d_rd_mesh_cache.h"
#include "rendering/rd/terrain_3d_rd_texture_manager.h"
#include "rendering/rd/terrain_3d_rd_topology.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <atomic>
#include <cstddef>
#include <limits>
#include <thread>

typedef struct t3dRdUnitTestState_s
{
	uint32_t m_nPassed = 0u;
	uint32_t m_nFailed = 0u;

	void expect( const bool bCondition, const char *const pDescription )
	{
		if ( bCondition ) {
			++m_nPassed;
			UtilityFunctions::print( "PASSED: ", pDescription );
		} else {
			++m_nFailed;
			UtilityFunctions::print( "FAILED: ", pDescription );
		}
	}
} t3dRdUnitTestState_t;

static t3dClipmapGeometry_t t3dRdMakePackingTestGeometry( void )
{
	t3dClipmapGeometry_t geometry;
	geometry.m_vertices.push_back( Vector3( -32768.0, 0.0, 32767.0 ) );
	geometry.m_vertices.push_back( Vector3( 32767.0, 0.0, -32768.0 ) );
	geometry.m_vertices.push_back( Vector3( 0.0, 0.0, 0.0 ) );
	geometry.m_metadata.push_back( Vector2( 4.0, 9.0 ) );
	geometry.m_metadata.push_back( Vector2( 0.0, 65535.0 ) );
	geometry.m_metadata.push_back( Vector2( 1.0, 0.0 ) );
	geometry.m_indices.push_back( 0 );
	geometry.m_indices.push_back( 1 );
	geometry.m_indices.push_back( 2 );
	return geometry;
}

static bool t3dRunRdTopologyTests( t3dRdUnitTestState_t& rState )
{
	Terrain3DRdTopology topology;
	t3dClipmapGeometry_t geometry = t3dRdMakePackingTestGeometry();
	rState.expect( topology.buildCpu( geometry, "CpuPackingTest" ), "RD topology accepts canonical triangle" );
	rState.expect( topology.isCpuReady(), "RD topology publishes complete CPU state" );
	rState.expect( topology.getVertexCount() == 3u && topology.getIndexCount() == 3u,
			"RD topology preserves vertex/index counts" );
	rState.expect( topology.getIndexFormat() == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16,
			"RD topology selects U16 for a representable maximum index" );
	rState.expect( topology.getGridMinimum() == Vector2i( -32768, -32768 ) &&
					topology.getGridMaximum() == Vector2i( 32767, 32767 ),
			"RD topology records validated inclusive grid bounds" );

	t3dRdClipmapVertex_t packed;
	rState.expect( topology.getPackedVertex( 0u, packed ) && packed.m_nGridX == INT16_MIN &&
					packed.m_nGridZ == INT16_MAX && packed.m_nPartCode == 4u && packed.m_nFlags == 9u,
			"RD topology packs X/Z/part/flags into the 8-byte vertex ABI" );
	rState.expect( topology.getPackedVertex( 1u, packed ) && packed.m_nGridX == INT16_MAX &&
					packed.m_nGridZ == INT16_MIN && packed.m_nPartCode == 0u && packed.m_nFlags == UINT16_MAX,
			"RD topology preserves signed and unsigned packing boundaries" );
	rState.expect( !topology.getPackedVertex( 3u, packed ), "RD topology rejects an out-of-range vertex read" );
	uint32_t unpacked_index = UINT32_MAX;
	rState.expect( topology.getIndex( 1u, unpacked_index ) && unpacked_index == 1u,
			"RD topology reads packed U16 indices without widening errors" );
	rState.expect( !topology.getIndex( 3u, unpacked_index ), "RD topology rejects an out-of-range index read" );
	rState.expect( topology.getVertexChecksum() == 0x813f4c0c3d69ad4bull &&
					topology.getIndexChecksum() == 0x37f11cf1d36d7afeull,
			"RD topology packing checksums pin the little-endian Windows ABI" );

	Terrain3DRdTopology same_topology;
	rState.expect( same_topology.buildCpu( geometry, "SamePackingTest" ) &&
					same_topology.getVertexChecksum() == topology.getVertexChecksum() &&
					same_topology.getIndexChecksum() == topology.getIndexChecksum(),
			"RD topology checksums are deterministic" );
	geometry.m_metadata.set( 0, Vector2( 4.0, 10.0 ) );
	Terrain3DRdTopology changed_topology;
	rState.expect( changed_topology.buildCpu( geometry, "ChangedPackingTest" ) &&
					changed_topology.getVertexChecksum() != topology.getVertexChecksum(),
			"RD topology checksum covers packed metadata" );

	const uint64_t stable_vertex_checksum = topology.getVertexChecksum();
	geometry = t3dRdMakePackingTestGeometry();
	geometry.m_vertices.set( 0, Vector3( 32768.0, 0.0, 0.0 ) );
	rState.expect( !topology.buildCpu( geometry, "OutOfRangeGrid", false ) &&
					topology.getVertexChecksum() == stable_vertex_checksum && topology.isCpuReady(),
			"RD topology rejects out-of-range coordinates transactionally" );
	geometry = t3dRdMakePackingTestGeometry();
	geometry.m_vertices.set( 0, Vector3( 0.25, 0.0, 0.0 ) );
	rState.expect( !topology.buildCpu( geometry, "FractionalGrid", false ),
			"RD topology rejects fractional grid coordinates" );
	geometry = t3dRdMakePackingTestGeometry();
	geometry.m_vertices.set( 0, Vector3( 0.0, 1.0, 0.0 ) );
	rState.expect( !topology.buildCpu( geometry, "ExplicitY", false ), "RD topology enforces implicit terrain Y" );
	geometry = t3dRdMakePackingTestGeometry();
	geometry.m_vertices.set( 0, Vector3( std::numeric_limits<real_t>::quiet_NaN(), 0.0, 0.0 ) );
	rState.expect(
			!topology.buildCpu( geometry, "NonFiniteGrid", false ), "RD topology rejects non-finite coordinates" );
	geometry = t3dRdMakePackingTestGeometry();
	geometry.m_metadata.set( 0, Vector2( 5.0, 0.0 ) );
	rState.expect(
			!topology.buildCpu( geometry, "InvalidPart", false ), "RD topology rejects non-canonical part metadata" );
	geometry = t3dRdMakePackingTestGeometry();
	geometry.m_metadata.set( 0, Vector2( 0.0, 1.5 ) );
	rState.expect(
			!topology.buildCpu( geometry, "FractionalFlags", false ), "RD topology rejects fractional flag metadata" );
	geometry = t3dRdMakePackingTestGeometry();
	geometry.m_indices.set( 0, -1 );
	rState.expect( !topology.buildCpu( geometry, "NegativeIndex", false ), "RD topology rejects negative indices" );
	geometry = t3dRdMakePackingTestGeometry();
	geometry.m_indices.set( 2, 3 );
	rState.expect( !topology.buildCpu( geometry, "LargeIndex", false ), "RD topology rejects indices at vertex count" );
	geometry = t3dRdMakePackingTestGeometry();
	geometry.m_indices.resize( 2 );
	rState.expect( !topology.buildCpu( geometry, "IncompleteTriangle", false ),
			"RD topology rejects incomplete triangle lists" );
	geometry = t3dRdMakePackingTestGeometry();
	geometry.m_metadata.resize( 2 );
	rState.expect( !topology.buildCpu( geometry, "MetadataMismatch", false ),
			"RD topology rejects vertex/metadata count mismatch" );

	t3dClipmapGeometry_t boundary_geometry;
	boundary_geometry.m_vertices.resize( 65537 );
	boundary_geometry.m_metadata.resize( 65537 );
	boundary_geometry.m_indices.resize( 3 );
	boundary_geometry.m_indices.set( 0, 0 );
	boundary_geometry.m_indices.set( 1, 65535 );
	boundary_geometry.m_indices.set( 2, 1 );
	Terrain3DRdTopology boundary_topology;
	rState.expect( boundary_topology.buildCpu( boundary_geometry, "U16Boundary" ) &&
					boundary_topology.getIndexFormat() == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16,
			"RD topology keeps index 65535 in U16 format" );
	boundary_geometry.m_indices.set( 1, 65536 );
	rState.expect( boundary_topology.buildCpu( boundary_geometry, "U32Boundary" ) &&
					boundary_topology.getIndexFormat() == RenderingDevice::INDEX_BUFFER_FORMAT_UINT32 &&
					boundary_topology.getIndex( 1u, unpacked_index ) && unpacked_index == 65536u,
			"RD topology falls back to U32 at index 65536" );

	boundary_topology.clearCpu();
	rState.expect( !boundary_topology.isCpuReady() && boundary_topology.getVertexCount() == 65537u &&
					boundary_topology.getVertexChecksum() != 0u,
			"RD topology can release staging bytes without losing immutable draw metadata" );
	return rState.m_nFailed == 0u;
}

static bool t3dRunRdAbiAndPublicationTests( t3dRdUnitTestState_t& rState )
{
	rState.expect( sizeof( t3dRdClipmapVertex_t ) == 8u, "RD packed clipmap vertex ABI is 8 bytes" );
	rState.expect( offsetof( t3dRdClipmapVertex_t, m_nGridX ) == 0u &&
					offsetof( t3dRdClipmapVertex_t, m_nGridZ ) == 2u &&
					offsetof( t3dRdClipmapVertex_t, m_nPartCode ) == 4u &&
					offsetof( t3dRdClipmapVertex_t, m_nFlags ) == 6u,
			"RD packed clipmap vertex member offsets match the vertex format" );
	rState.expect( sizeof( t3dRdPatch_t ) == 64u, "RD patch descriptor ABI is 64 bytes" );
	rState.expect( offsetof( t3dRdPatch_t, m_fOriginX ) == 0u && offsetof( t3dRdPatch_t, m_nFlags ) == 28u &&
					offsetof( t3dRdPatch_t, m_fBoundsMinY ) == 32u && offsetof( t3dRdPatch_t, m_fBoundsMaxY ) == 36u &&
					offsetof( t3dRdPatch_t, m_fBoundsHalfExtent ) == 40u &&
					offsetof( t3dRdPatch_t, m_fPreviousOriginX ) == 44u &&
					offsetof( t3dRdPatch_t, m_fPreviousOriginZ ) == 48u &&
					offsetof( t3dRdPatch_t, m_fPreviousScale ) == 52u && offsetof( t3dRdPatch_t, m_fPadding1 ) == 60u,
			"RD patch bounds and previous-frame offsets match std430" );
	rState.expect( sizeof( t3dRdMaterialLayer_t ) == 64u && offsetof( t3dRdMaterialLayer_t, m_nPhysicalLayer ) == 56u &&
					offsetof( t3dRdMaterialLayer_t, m_nFlags ) == 60u,
			"RD material layer size and terminal offsets match std430" );
	rState.expect( sizeof( t3dRdIndexedIndirectCommand_t ) == 20u &&
					offsetof( t3dRdIndexedIndirectCommand_t, m_nIndexCount ) == 0u &&
					offsetof( t3dRdIndexedIndirectCommand_t, m_nFirstInstance ) == 16u,
			"RD indexed indirect command matches VkDrawIndexedIndirectCommand" );
	rState.expect( sizeof( t3dRdCullPushConstants_t ) == 16u, "RD culling push constants remain 16 bytes" );
	rState.expect(
			sizeof( t3dRdDrawPushConstants_t ) == 16u && offsetof( t3dRdDrawPushConstants_t, m_nDirectMode ) == 4u,
			"RD direct-draw push constants match raster shader layout" );

	for ( uint32_t slot = 0u; slot < s_nRdFrameSnapshotSlotCount; ++slot ) {
		const uint64_t token = t3dRdMakeFrameSnapshotToken( slot, s_nRdFrameSnapshotGenerationMax, true );
		rState.expect( t3dRdFrameSnapshotTokenIndex( token ) == slot &&
						t3dRdFrameSnapshotTokenGeneration( token ) == s_nRdFrameSnapshotGenerationMax &&
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
	rState.expect( t3dRdFrameSnapshotGenerationIsNewer( 1u, s_nRdFrameSnapshotGenerationMax ) &&
					t3dRdFrameSnapshotGenerationIsStale( s_nRdFrameSnapshotGenerationMax, 1u ) &&
					t3dRdFrameSnapshotGenerationDistance( 1u, s_nRdFrameSnapshotGenerationMax ) == 1u,
			"RD snapshot generation ordering survives max-to-one wrap" );

	// Exercise the same ownership-exchange protocol used by the coordinator:
	// one slot is rendered, one is published, and only the exchanged slot may
	// be reclaimed by the single writer.
	uint64_t slot_generations[s_nRdFrameSnapshotSlotCount]{};
	std::atomic_uint64_t exchange_token{ t3dRdMakeFrameSnapshotToken( 1u, 0u, false ) };
	uint32_t write_index = 2u;
	uint32_t render_index = 0u;
	uint64_t render_generation = 0u;
	auto publish = [&]( const uint64_t generation ) {
		slot_generations[write_index] = generation;
		const uint64_t reclaimed = exchange_token.exchange(
				t3dRdMakeFrameSnapshotToken( write_index, generation, true ), std::memory_order_acq_rel );
		write_index = t3dRdFrameSnapshotTokenIndex( reclaimed );
	};
	auto acquire = [&]() {
		uint64_t token = exchange_token.load( std::memory_order_acquire );
		if ( !t3dRdFrameSnapshotTokenIsPublished( token ) ) {
			return false;
		}
		const uint32_t candidate_index = t3dRdFrameSnapshotTokenIndex( token );
		const uint64_t candidate_generation = t3dRdFrameSnapshotTokenGeneration( token );
		const uint64_t released = t3dRdMakeFrameSnapshotToken( render_index, render_generation, false );
		if ( !exchange_token.compare_exchange_strong(
					 token, released, std::memory_order_acq_rel, std::memory_order_acquire ) ) {
			return false;
		}
		render_index = candidate_index;
		render_generation = candidate_generation;
		return slot_generations[render_index] == render_generation;
	};
	publish( 1u );
	const bool first_acquire = acquire();
	const bool first_slots_distinct = write_index != render_index;
	publish( 2u );
	publish( 3u ); // Deliberately replace an unconsumed publication.
	const bool latest_acquire = acquire();
	rState.expect( first_acquire && first_slots_distinct && latest_acquire && render_generation == 3u &&
					write_index != render_index,
			"RD three-slot ownership exchange publishes latest complete data without writer/reader overlap" );

	typedef struct t3dRdPublicationPayload_s
	{
		uint64_t m_nGeneration = 0u;
		uint64_t m_nCheck = 0u;
	} t3dRdPublicationPayload_t;
	t3dRdPublicationPayload_t concurrent_slots[s_nRdFrameSnapshotSlotCount]{};
	std::atomic_uint64_t concurrent_token{ t3dRdMakeFrameSnapshotToken( 1u, 0u, false ) };
	std::atomic_bool producer_done{ false };
	std::atomic_bool concurrent_valid{ true };
	std::atomic_uint64_t concurrent_acquires{ 0u };
	constexpr uint64_t publication_count = 10000u;
	constexpr uint64_t check_mask = 0x9e3779b97f4a7c15ULL;
	std::thread producer( [&]() {
		uint32_t producer_write_index = 2u;
		for ( uint64_t generation = 1u; generation <= publication_count; ++generation ) {
			if ( producer_write_index >= s_nRdFrameSnapshotSlotCount ) {
				concurrent_valid.store( false, std::memory_order_release );
				break;
			}
			concurrent_slots[producer_write_index].m_nGeneration = generation;
			concurrent_slots[producer_write_index].m_nCheck = generation ^ check_mask;
			const uint64_t reclaimed = concurrent_token.exchange(
					t3dRdMakeFrameSnapshotToken( producer_write_index, generation, true ), std::memory_order_acq_rel );
			producer_write_index = t3dRdFrameSnapshotTokenIndex( reclaimed );
		}
		producer_done.store( true, std::memory_order_release );
	} );
	std::thread consumer( [&]() {
		uint32_t consumer_render_index = 0u;
		uint64_t consumer_generation = 0u;
		for ( ;; ) {
			uint64_t token = concurrent_token.load( std::memory_order_acquire );
			if ( !t3dRdFrameSnapshotTokenIsPublished( token ) ) {
				if ( producer_done.load( std::memory_order_acquire ) ) {
					break;
				}
				std::this_thread::yield();
				continue;
			}
			const uint32_t candidate_index = t3dRdFrameSnapshotTokenIndex( token );
			const uint64_t candidate_generation = t3dRdFrameSnapshotTokenGeneration( token );
			if ( candidate_index >= s_nRdFrameSnapshotSlotCount ||
					!t3dRdFrameSnapshotGenerationIsNewer( candidate_generation, consumer_generation ) ) {
				concurrent_valid.store( false, std::memory_order_release );
				break;
			}
			const uint64_t released = t3dRdMakeFrameSnapshotToken( consumer_render_index, consumer_generation, false );
			if ( !concurrent_token.compare_exchange_weak(
						 token, released, std::memory_order_acq_rel, std::memory_order_acquire ) ) {
				continue;
			}
			consumer_render_index = candidate_index;
			consumer_generation = candidate_generation;
			const t3dRdPublicationPayload_t payload = concurrent_slots[consumer_render_index];
			if ( payload.m_nGeneration != consumer_generation ||
					payload.m_nCheck != ( consumer_generation ^ check_mask ) ) {
				concurrent_valid.store( false, std::memory_order_release );
				break;
			}
			concurrent_acquires.fetch_add( 1u, std::memory_order_relaxed );
		}
	} );
	producer.join();
	consumer.join();
	rState.expect( concurrent_valid.load( std::memory_order_acquire ) &&
					concurrent_acquires.load( std::memory_order_acquire ) > 0u,
			"RD three-slot exchange survives concurrent SPSC overwrite/consume stress without torn payloads" );
	return rState.m_nFailed == 0u;
}

static t3dRdPatch_t
t3dRdMakeCullTestPatch( const uint32_t nLod, const uint32_t nTier, const float fOriginX, const float fOriginZ )
{
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

static bool t3dRunRdCullingAndIndirectTests( t3dRdUnitTestState_t& rState )
{
	const Projection identity;
	t3dRdPatch_t lod0 = t3dRdMakeCullTestPatch( 0u, 2u, 0.0f, 0.0f );
	t3dRdPatch_t outer_near = t3dRdMakeCullTestPatch( 1u, 0u, 0.2f, 0.0f );
	t3dRdPatch_t outer_middle = t3dRdMakeCullTestPatch( 2u, 1u, -0.2f, 0.0f );
	t3dRdPatch_t outer_far = t3dRdMakeCullTestPatch( 3u, 2u, 0.0f, 0.2f );
	t3dRdPatch_t outside = t3dRdMakeCullTestPatch( 4u, 2u, 3.0f, 0.0f );
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
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
		const t3dRdIndexedIndirectCommand_t& command = result.m_szCommands[group];
		groups_valid = groups_valid && command.m_nIndexCount == ( group == 0u ? 12u : 24u ) &&
				command.m_nInstanceCount == 1u && command.m_nFirstIndex == 0u && command.m_nVertexOffset == 0 &&
				command.m_nFirstInstance == group * s_nMaxClipmapLevels &&
				result.m_szVisibleIndices[group * s_nMaxClipmapLevels] == group;
	}
	rState.expect( groups_valid, "RD CPU culler emits bounded persistent indirect groups" );
	rState.expect( Terrain3DRdCpuCuller::validate( snapshot, identity, 12u, 24u, result ),
			"RD CPU/GPU comparison validator accepts matching visible output" );
	t3dRdCpuCullResult_t corrupted = result;
	corrupted.m_szCommands[T3D_RD_DRAW_GROUP_OUTER_FAR].m_nFirstInstance = 1u;
	rState.expect( !Terrain3DRdCpuCuller::validate( snapshot, identity, 12u, 24u, corrupted ),
			"RD indirect validation rejects a corrupt command offset" );
	constexpr uint32_t command_buffer_bytes = sizeof( t3dRdIndexedIndirectCommand_t ) * s_nRdDrawGroupCount;
	rState.expect( Terrain3DRdCpuCuller::isCommandRangeValid( 3u, command_buffer_bytes ) &&
					!Terrain3DRdCpuCuller::isCommandRangeValid( 3u, command_buffer_bytes - 1u ) &&
					!Terrain3DRdCpuCuller::isCommandRangeValid( 4u, command_buffer_bytes ) &&
					!Terrain3DRdCpuCuller::isCommandRangeValid( 0u, command_buffer_bytes, 19u ) &&
					!Terrain3DRdCpuCuller::isCommandRangeValid( 0u, command_buffer_bytes, 22u ) &&
					!Terrain3DRdCpuCuller::isCommandRangeValid( 3u, command_buffer_bytes, 20u, 2u ) &&
					Terrain3DRdCpuCuller::isCommandRangeValid( 3u, 96u, 24u ),
			"RD indirect validation enforces command offsets and strides" );
	snapshot.m_szPatches[0].m_fScale = 0.0f;
	rState.expect(
			!Terrain3DRdCpuCuller::build( snapshot, identity, 12u, 24u, result ) && result.m_nInvalidPatchCount == 1u,
			"RD CPU reference culler rejects invalid patch descriptors" );
	snapshot.m_nPatchCount = s_nMaxClipmapLevels + 1u;
	rState.expect( !Terrain3DRdCpuCuller::build( snapshot, identity, 12u, 24u, result ),
			"RD CPU reference culler bounds-checks snapshot patch count" );
	return rState.m_nFailed == 0u;
}

static bool t3dRunRdContextDiagnosticTests( t3dRdUnitTestState_t& rState )
{
	Terrain3DRdContext context;
	rState.expect( context.getState() == Terrain3DRdContext::STATE_UNINITIALIZED && !context.isReady() &&
					!context.hasFailure() && context.getDevice() == nullptr,
			"RD context starts detached without hidden device ownership" );
	context.beginFrame( 41u, 9u );
	context.recordBufferUpdate( 128u );
	context.recordBufferUpdate( 64u );
	Dictionary diagnostics = context.getDiagnostics();
	rState.expect( (int64_t)( diagnostics.get( "gpu_frame_id", 0 ) ) == 41 &&
					(int64_t)( diagnostics.get( "snapshot_generation", 0 ) ) == 9 &&
					(int64_t)( diagnostics.get( "buffer_updates_frame", 0 ) ) == 2 &&
					(int64_t)( diagnostics.get( "bytes_uploaded_frame", 0 ) ) == 192,
			"RD context accounts per-frame buffer update diagnostics" );
	context.recordFailure( Terrain3DRdContext::FAILURE_RESOURCE_CREATION, "sentinel" );
	diagnostics = context.getDiagnostics();
	rState.expect( context.hasFailure() && context.getState() == Terrain3DRdContext::STATE_FAILED &&
					context.getDevice() == nullptr &&
					String( diagnostics.get( "failure_state", String() ) ) == "resource_creation" &&
					String( diagnostics.get( "last_failure", String() ) ) == "sentinel",
			"RD context exposes explicit resource-creation failure state" );

	Terrain3DRdContext untracked_context;
	RID external_rid = UtilityFunctions::rid_from_int64( 0x12345678 );
	if ( external_rid.is_valid() ) {
		untracked_context.freeResource( external_rid );
	}
	diagnostics = untracked_context.getDiagnostics();
	rState.expect( external_rid.is_valid() && untracked_context.getState() == Terrain3DRdContext::STATE_FAILED &&
					untracked_context.getFailureCode() == Terrain3DRdContext::FAILURE_UNTRACKED_RESOURCE &&
					(int64_t)( diagnostics.get( "untracked_free_attempts", 0 ) ) == 1,
			"RD context rejects untracked RIDs without destroying external ownership" );
	return rState.m_nFailed == 0u;
}

static bool t3dRunRdMaterialAbiTests( t3dRdUnitTestState_t& rState )
{
	rState.expect( sizeof( t3dRdMaterialLayer_t ) == 64u && offsetof( t3dRdMaterialLayer_t, m_fColorR ) == 0u &&
					offsetof( t3dRdMaterialLayer_t, m_fUvScale ) == 16u &&
					offsetof( t3dRdMaterialLayer_t, m_fAoStrength ) == 32u &&
					offsetof( t3dRdMaterialLayer_t, m_fDisplacementOffset ) == 48u &&
					offsetof( t3dRdMaterialLayer_t, m_nPhysicalLayer ) == 56u &&
					offsetof( t3dRdMaterialLayer_t, m_nFlags ) == 60u,
			"RD material layer ABI pins scalar, texture-index, and flag offsets" );
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
	rState.expect( s_nRdMaterialLayerCount == 32u && T3D_RD_MATERIAL_LAYER_PACKED_SURFACE == 1u &&
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
					snapshot.m_materialConfig.m_szTier[0].m_nSamplingMode == 3u &&
					snapshot.m_materialConfig.m_szTier[1].m_nSamplingMode == 3u &&
					snapshot.m_materialConfig.m_szTier[2].m_nSamplingMode == 3u,
			"RD snapshot owns one deterministic material-config publication table" );
	return rState.m_nFailed == 0u;
}

static bool t3dRunRdInstanceAbiTests( t3dRdUnitTestState_t& rState )
{
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
	rState.expect( sizeof( t3dRdInstanceMaterial_t ) == 80u &&
					offsetof( t3dRdInstanceMaterial_t, m_fNormalScale ) == 48u &&
					offsetof( t3dRdInstanceMaterial_t, m_nAlbedoTextureIdLow ) == 56u,
			"RD regular-instance material table remains std430-compatible with normal scale and texture keys" );
	t3dRdInstanceMaterial_t material;
	material.m_nFlags = T3D_RD_INSTANCE_MATERIAL_VERTEX_COLOR | T3D_RD_INSTANCE_MATERIAL_NORMAL_MAPPING |
			T3D_RD_INSTANCE_MATERIAL_TEXTURE_REPEAT | T3D_RD_INSTANCE_MATERIAL_CULL_FRONT |
			T3D_RD_INSTANCE_MATERIAL_ALPHA_BLEND | T3D_RD_INSTANCE_MATERIAL_ALPHA_DEPTH_PREPASS |
			T3D_RD_INSTANCE_MATERIAL_ROUGHNESS_TEXTURE;
	material.m_fNormalScale = 0.4f;
	rState.expect( ( material.m_nFlags & T3D_RD_INSTANCE_MATERIAL_VERTEX_COLOR ) != 0u &&
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

static bool t3dRunRdTextureLayoutTests( t3dRdUnitTestState_t& rState )
{
	RenderingDevice::DataFormat format = RenderingDevice::DATA_FORMAT_MAX;
	rState.expect( Terrain3DRdTextureManager::imageFormatToDataFormat( Image::FORMAT_R16, format ) &&
					format == RenderingDevice::DATA_FORMAT_R16_UNORM,
			"RD texture layout maps R16 height data explicitly" );
	rState.expect( Terrain3DRdTextureManager::imageFormatToDataFormat( Image::FORMAT_RGBA8, format ) &&
					format == RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
			"RD texture layout maps RGBA8 color data explicitly" );
	rState.expect( Terrain3DRdTextureManager::imageFormatToDataFormat( Image::FORMAT_RF, format ) &&
					format == RenderingDevice::DATA_FORMAT_R32_SFLOAT,
			"RD texture layout maps RF authoring data explicitly" );
	rState.expect( !Terrain3DRdTextureManager::imageFormatToDataFormat( Image::FORMAT_R16I, format ) &&
					format == RenderingDevice::DATA_FORMAT_MAX,
			"RD texture layout rejects unsupported integer image formats" );

	const Ref<Image> image = Image::create_empty( 8, 4, false, Image::FORMAT_R16 );
	t3dRdTextureLayout_t layout;
	rState.expect( image.is_valid() && Terrain3DRdTextureManager::buildLayout( image, 3u, true, 192u, layout ),
			"RD texture layout accepts a complete generated array" );
	rState.expect( layout.m_type == RenderingDevice::TEXTURE_TYPE_2D_ARRAY && layout.m_nWidth == 8u &&
					layout.m_nHeight == 4u && layout.m_nLayers == 3u && layout.m_nMipmaps == 1u &&
					layout.m_nBytes == 192u,
			"RD texture layout preserves dimensions, layer count, mip count, and bytes" );
	const uint64_t required_usage = (uint64_t)( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT ) |
			(uint64_t)( RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT ) |
			(uint64_t)( RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT ) |
			(uint64_t)( RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );
	rState.expect( ( layout.m_nUsageBits & required_usage ) == required_usage,
			"RD generated textures expose sampling, dirty update, and copy usage" );
	rState.expect( !Terrain3DRdTextureManager::buildLayout( image, 0u, true, 192u, layout ) &&
					!Terrain3DRdTextureManager::buildLayout( image, 3u, true, 0u, layout ),
			"RD texture layout rejects zero-layer and zero-byte resources" );
	return rState.m_nFailed == 0u;
}
template <typename TLeft, typename TRight>
static EA_FORCE_INLINE void
t3dUnitTestingLogDiffers( const TLeft& rA, const TRight& rB, const String& rDescription, const bool bExpected )
{
	const bool bDiffersResult = t3dDiffers( rA, rB );
	const String result = bDiffersResult == bExpected ? "PASSED" : "FAILED";
	UtilityFunctions::print( rDescription, ": differs(", rA, ", ", rB, ") = ", bDiffersResult, " - ", result );
}

void t3dTestDiffers( void )
{
	UtilityFunctions::print( "=== Testing differs function ===" );

	// 1. Scalars: int, float (should use ==, differs if values differ)
	{
		int i1 = 42;
		int i2 = 42; // Same
		int i3 = 43; // Diff
		t3dUnitTestingLogDiffers( i1, i2, "int same", false );
		t3dUnitTestingLogDiffers( i1, i3, "int diff", true );

		double fF1 = 3.14;
		double fF2 = 3.14; // Same
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
		String s2 = s1; // Shared (COW)
		String s3( "test" ); // Separate alloc, same value
		String s4 = "diff";
		t3dUnitTestingLogDiffers( String(), String(), "String() vs String()", false );
		t3dUnitTestingLogDiffers( String(), String( "a" ), "String() vs String('a')", true );
		t3dUnitTestingLogDiffers( s1, s2, "String shared", false );
		t3dUnitTestingLogDiffers( s1, s3, "String same value diff ptr", false ); // Length match + == true
		t3dUnitTestingLogDiffers( s1, s4, "String diff value", true ); // Length may match, but == false
	}

	// 4. StringName: Similar to String (uses ==, but test sharing if applicable)
	{
		StringName sn1 = "test";
		StringName sn2 = sn1; // Shared
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
		Array arr2 = arr1; // Shared
		Array arr3; // Empty diff
		arr3.push_back( 42 ); // Same content, but separate (differs=true, conservative)
		Array empty_arr; // Explicit empty for size diff
		t3dUnitTestingLogDiffers( arr1, arr2, "Array shared", false ); // Same self ptr
		t3dUnitTestingLogDiffers(
				arr1, arr3, "Array same content diff ptr", true ); // Diff self ptr (no fallback for Array)
		t3dUnitTestingLogDiffers( arr1, empty_arr, "Array size diff", true ); // Size mismatch
	}

	// 6. TypedArray: e.g., TypedArray<int>
	{
		TypedArray<int> ta1;
		ta1.push_back( 42 );
		TypedArray<int> ta2 = ta1; // Shared
		TypedArray<int> ta3; // Empty
		ta3.push_back( 42 ); // Separate
		TypedArray<int> empty_ta; // Explicit empty
		t3dUnitTestingLogDiffers( ta1, ta2, "TypedArray shared", false ); // Via Array base
		t3dUnitTestingLogDiffers( ta1, ta3, "TypedArray same content diff ptr", true ); // Diff self
		t3dUnitTestingLogDiffers( ta1, empty_ta, "TypedArray size diff", true ); // Size mismatch
	}

	// 7. Dictionary: Share vs mutate
	{
		Dictionary dict1;
		dict1["key"] = 42;
		Dictionary dict2 = dict1; // Shared
		Dictionary dict3; // Empty
		dict3["key"] = 42; // Separate
		Dictionary empty_dict; // Explicit empty
		t3dUnitTestingLogDiffers( dict1, dict2, "Dictionary shared", false );
		t3dUnitTestingLogDiffers( dict1, dict3, "Dictionary same content diff ptr", true ); // Diff self
		t3dUnitTestingLogDiffers( dict1, empty_dict, "Dictionary size diff", true ); // Size mismatch
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
		Ref<RefCounted> pRc1 = memnew( RefCounted );
		Ref<RefCounted> pRc2 = pRc1; // Same ref
		Ref<RefCounted> pRc3 = memnew( RefCounted ); // Diff
		Variant v_rc1 = pRc1;
		Variant v_rc2 = pRc2;
		Variant v_rc3 = pRc3;
		t3dUnitTestingLogDiffers( v_rc1, v_rc2, "Variant RefCounted same ref", false ); // Ref == true
		t3dUnitTestingLogDiffers( v_rc1, v_rc3, "Variant RefCounted diff ref", true );

		// Variant Array
		Array arr_var1;
		arr_var1.push_back( 42 );
		Variant v_arr1 = arr_var1;
		Array arr_var2 = arr_var1;
		Variant v_arr2 = arr_var2;
		t3dUnitTestingLogDiffers( v_arr1, v_arr2, "Variant Array shared", false ); // Variant == checks inner

		// Variant Dictionary
		Dictionary dict_var1;
		dict_var1["key"] = 42;
		Variant v_dict1 = dict_var1;
		Dictionary dict_var2 = dict_var1;
		Variant v_dict2 = dict_var2;
		t3dUnitTestingLogDiffers( v_dict1, v_dict2, "Variant Dictionary shared", false );
	}

	UtilityFunctions::print( "=== End differs tests ===" );
}

bool t3dTestRdTopology( void )
{
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD topology contracts ===" );
	t3dRunRdTopologyTests( state );
	UtilityFunctions::print( "=== RD topology tests: ", state.m_nPassed, " passed, ", state.m_nFailed, " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dTestRdAbiAndPublication( void )
{
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD ABI/publication contracts ===" );
	t3dRunRdAbiAndPublicationTests( state );
	UtilityFunctions::print(
			"=== RD ABI/publication tests: ", state.m_nPassed, " passed, ", state.m_nFailed, " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dTestRdCullingAndIndirect( void )
{
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD culling/indirect contracts ===" );
	t3dRunRdCullingAndIndirectTests( state );
	UtilityFunctions::print(
			"=== RD culling/indirect tests: ", state.m_nPassed, " passed, ", state.m_nFailed, " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dTestRdContextDiagnostics( void )
{
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD context diagnostics ===" );
	t3dRunRdContextDiagnosticTests( state );
	UtilityFunctions::print( "=== RD context tests: ", state.m_nPassed, " passed, ", state.m_nFailed, " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dTestRdTextureLayouts( void )
{
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD texture layout contracts ===" );
	t3dRunRdTextureLayoutTests( state );
	UtilityFunctions::print(
			"=== RD texture layout tests: ", state.m_nPassed, " passed, ", state.m_nFailed, " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dTestRdMaterialAbi( void )
{
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD material ABI contracts ===" );
	t3dRunRdMaterialAbiTests( state );
	UtilityFunctions::print(
			"=== RD material ABI tests: ", state.m_nPassed, " passed, ", state.m_nFailed, " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dTestRdInstanceAbi( void )
{
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Testing RD regular-instance ABI contracts ===" );
	t3dRunRdInstanceAbiTests( state );
	UtilityFunctions::print(
			"=== RD regular-instance ABI tests: ", state.m_nPassed, " passed, ", state.m_nFailed, " failed ===" );
	return state.m_nFailed == 0u;
}

bool t3dRunRdCpuTests( void )
{
	t3dRdUnitTestState_t state;
	UtilityFunctions::print( "=== Running Terrain3D RD CPU tests ===" );
	t3dRunRdTopologyTests( state );
	t3dRunRdAbiAndPublicationTests( state );
	t3dRunRdCullingAndIndirectTests( state );
	t3dRunRdContextDiagnosticTests( state );
	t3dRunRdTextureLayoutTests( state );
	t3dRunRdMaterialAbiTests( state );
	t3dRunRdInstanceAbiTests( state );
	UtilityFunctions::print(
			"=== Terrain3D RD CPU tests: ", state.m_nPassed, " passed, ", state.m_nFailed, " failed ===" );
	return state.m_nFailed == 0u;
}
