#include "diagnostics/tests/systems/test_harness.h"
#include "memory/arena/terrain_3d_linear_arena.h"
#include "memory/arena/terrain_3d_memory_arena.h"
#include "memory/arena/terrain_3d_memory_zone.h"
#include <EASTL/utility.h>
#include <limits>
#include <type_traits>
#include <string.h>

namespace terrain3d_tests
{
namespace
{
struct ArenaConstructionProbe
{
	static uint32_t s_nConstructed;
	uint32_t        m_nSentinel;
	ArenaConstructionProbe() : m_nSentinel( 0x1234abcdu ) { ++s_nConstructed; }
};
uint32_t ArenaConstructionProbe::s_nConstructed = 0u;
static_assert( std::is_trivially_copyable< ArenaConstructionProbe >::value, "Probe must be trivially copyable" );

// The zone must be drained before its backing arena is destroyed.
struct ZoneFixture
{
	Terrain3DLinearArena backing;
	Terrain3DMemoryZone	 zone;
	void				*outstanding[8]{};
	void				*allocate(
					   size_t bytes,
					   size_t alignment = alignof( max_align_t ) ) {
		for ( auto &entry : outstanding ) {
			if ( !entry ) {
				entry = zone.allocate( bytes, alignment );
				return entry;
			}
		}
		return nullptr;
	}
	void deallocate(
		void *memory ) {
		zone.deallocate( memory );
		for ( auto &entry : outstanding ) {
			if ( entry == memory ) {
				entry = nullptr;
			}
		}
	}
	bool initialize() {
		return backing.configure( 256 * 1024 ) && zone.initialize( backing.allocate( 256 * 1024, 64 ), 256 * 1024 );
	}
	~ZoneFixture() {
		for ( void *entry : outstanding ) {
			if ( entry ) {
				zone.deallocate( entry );
			}
		}
		if ( zone.state() == T3D_MEMORY_ZONE_ACTIVE ) {
			zone.beginDraining();
		}
		if ( zone.state() == T3D_MEMORY_ZONE_DRAINING ) {
			zone.release();
		}
	}
};
} // namespace

void register_memory_arena_tests(
	TestRunner &runner ) {
	// Fixed image layout: alignment, bounds and lifecycle.
	runner.run( "memory_arena", "layout_reserves_nonoverlapping_aligned_slices", []( TestContext &test ) {
		Terrain3DMemoryArenaLayout layout( 1024 );
		t3dMemoryArenaSlice_t	   first, second;
		T3D_REQUIRE( test, layout.reserveLow( 3, 1, first ) && layout.reserveLow( 128, 64, second ) );
		T3D_CHECK( test, first.m_iOffset == 0 && second.m_iOffset == 64 );
		T3D_CHECK( test, first.m_iOffset + first.m_nSize <= second.m_iOffset );
		T3D_CHECK( test, layout.lowUsed() == 192 && layout.remaining() == 832 );
	} );
	runner.run( "memory_arena", "layout_accepts_an_exact_capacity_reservation", []( TestContext &test ) {
		Terrain3DMemoryArenaLayout layout( 64 );
		t3dMemoryArenaSlice_t	   slice;
		T3D_CHECK( test, layout.reserveLow( 64, 64, slice ) );
		T3D_CHECK( test, layout.remaining() == 0 && layout.seal() );
	} );
	runner.run( "memory_arena", "layout_overflow_latches_failure_without_publishing_a_slice", []( TestContext &test ) {
		Terrain3DMemoryArenaLayout layout( 32 );
		t3dMemoryArenaSlice_t	   slice;
		T3D_CHECK( test, !layout.reserveLow( 33, 1, slice ) );
		T3D_CHECK( test, !slice.valid() && layout.lowUsed() == 0 && !layout.valid() );
		T3D_CHECK( test, !layout.seal() && !layout.reserveLow( 1, 1, slice ) );
	} );
	runner.run( "memory_arena", "layout_rejects_zero_and_non_power_of_two_alignment", []( TestContext &test ) {
		for ( const size_t alignment : { size_t( 0 ), size_t( 3 ), size_t( 6 ) } ) {
			Terrain3DMemoryArenaLayout layout( 64 );
			t3dMemoryArenaSlice_t	   slice;
			T3D_CHECK( test, !layout.reserveLow( 1, alignment, slice ) );
			T3D_CHECK( test, !layout.valid() && !slice.valid() );
		}
	} );
	runner.run( "memory_arena", "sealed_layout_rejects_later_reservations", []( TestContext &test ) {
		Terrain3DMemoryArenaLayout layout( 64 );
		t3dMemoryArenaSlice_t	   slice;
		T3D_REQUIRE( test, layout.seal() );
		T3D_CHECK( test, !layout.reserveLow( 1, 1, slice ) && layout.lowUsed() == 0 );
	} );
	runner.run( "memory_arena", "reconfigure_recovers_an_invalid_layout", []( TestContext &test ) {
		Terrain3DMemoryArenaLayout layout( 8 );
		t3dMemoryArenaSlice_t	   slice;
		T3D_REQUIRE( test, !layout.reserveLow( 9, 1, slice ) );
		T3D_CHECK( test, layout.configure( 64 ) && layout.reserveLow( 32, 16, slice ) && layout.seal() );
	} );
	runner.run( "memory_arena", "arena_requires_a_valid_sealed_layout", []( TestContext &test ) {
		Terrain3DMemoryArenaLayout layout( 64 );
		Terrain3DMemoryArena	   arena;
		T3D_CHECK( test, !arena.initialize( layout ) );
		T3D_CHECK( test, !arena.initialized() && !arena.commit() && !arena.seal() );
	} );
	runner.run( "memory_arena", "commit_zeroes_every_byte_and_seal_preserves_written_data", []( TestContext &test ) {
		Terrain3DMemoryArenaLayout layout( 128 );
		t3dMemoryArenaSlice_t	   slice;
		T3D_REQUIRE( test, layout.reserveLow( 128, 64, slice ) && layout.seal() );
		Terrain3DMemoryArena arena;
		T3D_REQUIRE( test, arena.initialize( layout ) );
		T3D_CHECK( test, !arena.seal() );
		T3D_REQUIRE( test, arena.commit() );
		auto span = arena.resolve( slice );
		T3D_REQUIRE( test, span.m_pData != nullptr && span.m_nSize == 128 );
		for ( size_t index = 0; index < span.m_nSize; ++index ) {
			T3D_CHECK( test, span.m_pData[index] == 0 );
		}
		memset( span.m_pData, 0x5a, span.m_nSize );
		T3D_CHECK( test, arena.seal() && !arena.commit() );
		T3D_CHECK( test, arena.resolve( slice ).m_pData[127] == 0x5a );
	} );
	runner.run( "memory_arena", "arena_honors_the_largest_requested_alignment", []( TestContext &test ) {
		Terrain3DMemoryArenaLayout layout( 512 );
		t3dMemoryArenaSlice_t	   slice;
		T3D_REQUIRE( test, layout.reserveLow( 16, 256, slice ) && layout.seal() );
		Terrain3DMemoryArena arena;
		T3D_REQUIRE( test, arena.initialize( layout ) );
		T3D_CHECK( test, reinterpret_cast< uintptr_t >( arena.resolve( slice ).m_pData ) % 256 == 0 );
	} );
	runner.run( "memory_arena", "forged_out_of_bounds_or_misaligned_slices_are_rejected", []( TestContext &test ) {
		Terrain3DMemoryArenaLayout layout( 64 );
		Terrain3DMemoryArena	   arena;
		T3D_REQUIRE( test, layout.seal() && arena.initialize( layout ) );
		for ( const t3dMemoryArenaSlice_t slice : { t3dMemoryArenaSlice_t{ 65, 0, 1, true },
													t3dMemoryArenaSlice_t{ 32, 33, 1, true },
													t3dMemoryArenaSlice_t{ 1, 4, 4, true },
													t3dMemoryArenaSlice_t{ 0, 4, 0, true },
													t3dMemoryArenaSlice_t{ 0, 4, 1, false } } ) {
			T3D_CHECK( test, arena.resolve( slice ).m_pData == nullptr );
		}
	} );
	runner.run( "memory_arena", "empty_arena_has_a_complete_zero_capacity_lifecycle", []( TestContext &test ) {
		Terrain3DMemoryArenaLayout layout( 0 );
		Terrain3DMemoryArena	   arena;
		T3D_REQUIRE( test, layout.seal() );
		T3D_CHECK( test, arena.initialize( layout ) && arena.commit() && arena.seal() );
		T3D_CHECK( test, arena.capacity() == 0 );
		arena.release();
		arena.release();
		T3D_CHECK( test, !arena.initialized() && !arena.committed() && !arena.sealed() );
	} );
	runner.run( "memory_arena", "move_transfers_backing_and_resets_the_source", []( TestContext &test ) {
		Terrain3DMemoryArenaLayout layout( 64 );
		t3dMemoryArenaSlice_t	   slice;
		T3D_REQUIRE( test, layout.reserveLow( 64, 64, slice ) && layout.seal() );
		Terrain3DMemoryArena source;
		T3D_REQUIRE( test, source.initialize( layout ) && source.commit() );
		auto *address = source.resolve( slice ).m_pData;
		address[0] = 19;
		Terrain3DMemoryArena destination( eastl::move( source ) );
		T3D_CHECK( test, !source.initialized() && destination.resolve( slice ).m_pData == address );
		T3D_CHECK( test, destination.resolve( slice ).m_pData[0] == 19 );
		Terrain3DMemoryArena assigned;
		assigned = eastl::move( destination );
		T3D_CHECK( test, !destination.initialized() && assigned.resolve( slice ).m_pData == address );
	} );
	// Linear allocation: failure must not consume capacity.
	runner.run( "memory_arena", "linear_allocations_include_alignment_padding", []( TestContext &test ) {
		Terrain3DLinearArena arena;
		T3D_REQUIRE( test, arena.configure( 256, 128 ) );
		T3D_REQUIRE( test, arena.allocate( 3, 1 ) != nullptr );
		auto *aligned = arena.allocate( 16, 64 );
		T3D_CHECK( test, aligned != nullptr && reinterpret_cast< uintptr_t >( aligned ) % 64 == 0 );
		T3D_CHECK( test, arena.used() == 80 && arena.remaining() == 176 );
	} );
	runner.run( "memory_arena", "linear_overflow_preserves_cursor_and_live_data", []( TestContext &test ) {
		Terrain3DLinearArena arena;
		T3D_REQUIRE( test, arena.configure( 64 ) );
		auto *data = static_cast< uint8_t * >( arena.allocate( 32, 1 ) );
		T3D_REQUIRE( test, data != nullptr );
		data[0] = 42;
		T3D_CHECK( test, arena.allocate( 33, 1 ) == nullptr );
		T3D_CHECK( test, arena.allocate( std::numeric_limits< size_t >::max(), 1 ) == nullptr );
		T3D_CHECK( test, arena.used() == 32 && data[0] == 42 );
		T3D_CHECK( test, arena.allocate( 32, 1 ) != nullptr && arena.remaining() == 0 );
	} );
	runner.run( "memory_arena", "linear_zero_size_and_invalid_alignment_do_not_advance", []( TestContext &test ) {
		Terrain3DLinearArena arena;
		T3D_REQUIRE( test, arena.configure( 64 ) );
		T3D_CHECK( test, arena.allocate( 0, 1 ) == nullptr );
		T3D_CHECK( test, arena.allocate( 1, 0 ) == nullptr && arena.allocate( 1, 3 ) == nullptr );
		T3D_CHECK( test, arena.used() == 0 );
	} );
	runner.run( "memory_arena", "linear_reset_reuses_the_same_backing_address", []( TestContext &test ) {
		Terrain3DLinearArena arena;
		T3D_REQUIRE( test, arena.configure( 128 ) );
		auto *first = arena.allocate( 64, 64 );
		arena.reset();
		T3D_CHECK( test, arena.used() == 0 && arena.allocate( 64, 64 ) == first );
	} );
	runner.run( "memory_arena", "sealed_linear_capacity_allows_reuse_but_rejects_growth", []( TestContext &test ) {
		Terrain3DLinearArena arena;
		T3D_REQUIRE( test, arena.configure( 128 ) && arena.sealCapacity() );
		T3D_CHECK( test, arena.ensureCapacity( 128 ) && arena.allocate( 32, 1 ) != nullptr );
		T3D_CHECK( test, !arena.ensureCapacity( 129 ) && !arena.configure( 128 ) );
		T3D_CHECK( test, arena.capacity() == 128 && arena.used() == 32 );
		arena.unsealCapacityCold();
		T3D_CHECK( test, arena.ensureCapacity( 256 ) && arena.capacity() == 256 );
	} );
	runner.run( "memory_arena", "typed_arrays_reject_count_multiplication_overflow", []( TestContext &test ) {
		Terrain3DLinearArena arena;
		T3D_REQUIRE( test, arena.configure( 128 ) );
		T3D_CHECK( test, arena.allocateArray< uint64_t >( std::numeric_limits< size_t >::max() ) == nullptr );
		T3D_CHECK( test, arena.used() == 0 );
		Terrain3DLinearArenaLayout layout;
		T3D_CHECK( test, !layout.addArray< uint64_t >( std::numeric_limits< size_t >::max() ) && !layout.valid() );
	} );
	runner.run( "memory_arena", "linear_layout_rejects_alignment_and_size_overflow", []( TestContext &test ) {
		Terrain3DLinearArenaLayout invalid_alignment, invalid_size;
		T3D_CHECK( test, !invalid_alignment.add( 1, 3 ) && !invalid_alignment.valid() );
		T3D_REQUIRE( test, invalid_size.add( 1, 1 ) );
		T3D_CHECK( test, !invalid_size.add( std::numeric_limits< size_t >::max(), 1 ) && !invalid_size.valid() );
	} );
	runner.run( "memory_arena", "typed_buffers_copy_values_into_independent_storage", []( TestContext &test ) {
		Terrain3DLinearArena arena;
		T3D_REQUIRE( test, arena.configure( 256 ) );
		Terrain3DArenaBuffer< uint32_t > source, destination;
		T3D_REQUIRE( test, source.assign( arena, 8, 42 ) && destination.assign( arena, 8, 0 ) );
		T3D_CHECK( test, destination.copyFrom( source ) && destination[7] == 42 );
		destination[0] = 9;
		T3D_CHECK( test, source[0] == 42 && source.data() != destination.data() );
	} );
	runner.run( "memory_arena", "typed_buffer_size_mismatch_preserves_the_destination", []( TestContext &test ) {
		Terrain3DLinearArena arena;
		T3D_REQUIRE( test, arena.configure( 256 ) );
		Terrain3DArenaBuffer< uint32_t > source, destination;
		T3D_REQUIRE( test, source.assign( arena, 4, 42 ) && destination.assign( arena, 3, 9 ) );
		T3D_CHECK( test, !destination.copyFrom( source ) && destination.size() == 3 && destination[0] == 9 );
	} );
	runner.run( "memory_arena", "clearing_a_typed_buffer_does_not_rewind_the_arena", []( TestContext &test ) {
		Terrain3DLinearArena arena;
		T3D_REQUIRE( test, arena.configure( 128 ) );
		Terrain3DArenaBuffer< uint32_t > buffer;
		T3D_REQUIRE( test, buffer.assign( arena, 8, 1 ) );
		const size_t used = arena.used();
		buffer.clear();
		T3D_CHECK( test, buffer.empty() && buffer.data() == nullptr && arena.used() == used );
	} );
	runner.run( "memory_arena", "typed_scratch_starts_each_object_lifetime", []( TestContext &test ) {
		Terrain3DLinearArena arena;
		T3D_REQUIRE( test, arena.configure( 1024, 64 ) );
		ArenaConstructionProbe::s_nConstructed = 0u;
		auto *first = arena.allocateArray< ArenaConstructionProbe >( 3 );
		T3D_REQUIRE( test, first != nullptr );
		T3D_CHECK( test, ArenaConstructionProbe::s_nConstructed == 3 && first[2].m_nSentinel == 0x1234abcdu );
		arena.reset();
		auto *second = arena.allocateArray< ArenaConstructionProbe >( 3 );
		T3D_CHECK( test, second == first && ArenaConstructionProbe::s_nConstructed == 6 );
		Terrain3DArenaBuffer< godot::Vector3 > vectors;
		T3D_CHECK( test, vectors.allocateUninitialized( arena, 3 ) && vectors.data() != nullptr );
	} );
	runner.run( "memory_arena", "linear_alignment_upgrade_reallocates_only_before_seal", []( TestContext &test ) {
		Terrain3DLinearArena arena;
		T3D_REQUIRE( test, arena.configure( 512, 64 ) );
		T3D_CHECK( test, arena.allocate( 8, 128 ) == nullptr && arena.used() == 0u );
		T3D_REQUIRE( test, arena.ensureCapacity( 512, 128 ) );
		auto *aligned = arena.allocate( 8, 128 );
		T3D_CHECK( test, aligned && ( reinterpret_cast< uintptr_t >( aligned ) % 128u ) == 0u );
		T3D_REQUIRE( test, arena.sealCapacity() );
		T3D_CHECK( test, !arena.ensureCapacity( 512, 256 ) && arena.allocate( 8, 256 ) == nullptr );
		T3D_CHECK( test, arena.ensureCapacity( 512, 128 ) );
		T3D_CHECK( test, ( reinterpret_cast< uintptr_t >( arena.allocate( 8, 128 ) ) % 128u ) == 0u );
		arena.unsealCapacityCold();
		T3D_REQUIRE( test, arena.ensureCapacity( 512, 256 ) );
		T3D_CHECK( test, ( reinterpret_cast< uintptr_t >( arena.allocate( 8, 256 ) ) % 256u ) == 0u );
	} );
	// Bounded zones: accounting, reclamation and shutdown admission.
	runner.run( "memory_arena", "zone_allocations_are_owned_aligned_and_accounted", []( TestContext &test ) {
		ZoneFixture fixture;
		T3D_REQUIRE( test, fixture.initialize() );
		auto *first = fixture.allocate( 17, 16 );
		auto *second = fixture.allocate( 5000, 64 );
		T3D_REQUIRE( test, first != nullptr && second != nullptr );
		T3D_CHECK( test, fixture.zone.owns( first ) && fixture.zone.owns( second ) );
		T3D_CHECK( test, reinterpret_cast< uintptr_t >( second ) % 64 == 0 );
		T3D_CHECK( test, fixture.zone.liveAllocations() == 2 && fixture.zone.liveBytes() == 5017 );
		fixture.deallocate( second );
		fixture.deallocate( first );
		T3D_CHECK( test, fixture.zone.liveAllocations() == 0 && fixture.zone.liveBytes() == 0 );
	} );
	runner.run( "memory_arena",
				"zone_draining_blocks_new_allocations_but_allows_deallocation",
				[]( TestContext &test ) {
					ZoneFixture fixture;
					T3D_REQUIRE( test, fixture.initialize() );
					auto *allocation = fixture.allocate( 128 );
					T3D_REQUIRE( test, allocation != nullptr && fixture.zone.beginDraining() );
					T3D_CHECK( test, fixture.allocate( 8 ) == nullptr && !fixture.zone.canAllocate( 8 ) );
					T3D_CHECK( test, !fixture.zone.release() );
					fixture.deallocate( allocation );
					T3D_CHECK( test, fixture.zone.release() );
				} );
	runner.run( "memory_arena", "zone_exhaustion_does_not_corrupt_existing_allocations", []( TestContext &test ) {
		ZoneFixture fixture;
		T3D_REQUIRE( test, fixture.initialize() );
		auto *allocation = static_cast< uint8_t * >( fixture.allocate( 64 ) );
		T3D_REQUIRE( test, allocation != nullptr );
		memset( allocation, 0x3c, 64 );
		T3D_CHECK( test, fixture.allocate( fixture.zone.capacity() + 1 ) == nullptr );
		T3D_CHECK( test, fixture.zone.liveAllocations() == 1 && allocation[0] == 0x3c && allocation[63] == 0x3c );
		fixture.deallocate( allocation );
	} );
	runner.run( "memory_arena", "zone_free_extents_coalesce_for_a_larger_replacement", []( TestContext &test ) {
		ZoneFixture fixture;
		T3D_REQUIRE( test, fixture.initialize() );
		void *first = fixture.allocate( 32 * 1024 ), *second = fixture.allocate( 32 * 1024 );
		T3D_REQUIRE( test, first && second );
		fixture.deallocate( first );
		fixture.deallocate( second );
		void *replacement = fixture.allocate( 128 * 1024 );
		T3D_CHECK( test, replacement != nullptr );
		fixture.deallocate( replacement );
	} );
}
} // namespace terrain3d_tests
