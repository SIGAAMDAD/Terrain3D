#ifndef TERRAIN3D_TEST_COOKING_FIXTURE_H
#define TERRAIN3D_TEST_COOKING_FIXTURE_H

#include "authoring/vt/build/terrain_3d_vt_reference_encoder.h"
#include "authoring/vt/io/terrain_3d_vt_godot_cooker.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "memory/core/terrain_3d_memory.h"
#include <limits>
#include <string.h>

namespace terrain3d_tests
{
struct CookingSource
{
	PackedByteArray			  albedo, pbr;
	t3dVTCookSparseSource_t source{};
	CookingSource(
		uint64_t key = 101,
		uint8_t	 color = 170 ) {
		albedo.resize( 256 * 256 * 4 );
		albedo.fill( color );
		pbr.resize( 128 * 128 * 4 );
		pbr.fill( 128 );
		source.m_nStableKey = key;
		source.m_nGenerationSeed = 7;
		source.m_nConsumer = T3D_VT_CONSUMER_CLIPMAP;
		source.m_nWrapMode = T3D_VT_COOK_WRAP_CLAMP;
		source.m_nAllocationFlags =
			T3D_VT_ALLOCATION_MEGATEXTURE | T3D_VT_ALLOCATION_CLAMP | T3D_VT_ALLOCATION_SHIPPING_IMMUTABLE;
		source.m_nAuthoredChannelMask = T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_ALBEDO ) |
										T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_PBR );
		source.m_Channels[T3D_VT_LOGICAL_CHANNEL_ALBEDO] = { albedo.ptr(), 256, 256, 4, 1024 };
		source.m_Channels[T3D_VT_LOGICAL_CHANNEL_PBR] = { pbr.ptr(), 128, 128, 4, 512 };
		source.m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_ALBEDO] =
			t3dHashPublicationBytes( albedo.ptr(), albedo.size() );
		source.m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_PBR] = t3dHashPublicationBytes( pbr.ptr(), pbr.size() );
	}
	bool fragment(
		PackedByteArray &bytes,
		uint64_t		 byte_limit = 32ull * 1024 * 1024 ) const {
		return t3d_vt_godot_cook_source_fragment( source, bytes, byte_limit );
	}
};

// Allocation failure is injected through the cooker's public allocator seam.
// Count outstanding blocks so success AND early-exit cleanup are observable.
struct CookAllocator
{
	size_t		 calls = 0, live_blocks = 0;
	size_t		 fail_after = std::numeric_limits< size_t >::max();
	static void *allocate(
		void	*user,
		uint64_t bytes,
		uint32_t alignment ) {
		auto &self = *static_cast< CookAllocator * >( user );
		if ( self.calls++ >= self.fail_after || alignment == 0 || ( alignment & ( alignment - 1 ) ) != 0 ||
			 bytes > std::numeric_limits< size_t >::max() - alignment - sizeof( void * ) ) {
			return nullptr;
		}
		void *raw = t3dMemoryAllocate( static_cast< size_t >( bytes ) + alignment + sizeof( void * ) );
		if ( !raw ) {
			return nullptr;
		}
		const uintptr_t start = reinterpret_cast< uintptr_t >( raw ) + sizeof( void * );
		void		   *aligned =
			reinterpret_cast< void * >( ( start + alignment - 1 ) & ~static_cast< uintptr_t >( alignment - 1 ) );
		memcpy( static_cast< uint8_t * >( aligned ) - sizeof( void * ), &raw, sizeof( raw ) );
		++self.live_blocks;
		return aligned;
	}
	static void free(
		void *user,
		void *memory ) {
		if ( !memory ) {
			return;
		}
		auto &self = *static_cast< CookAllocator * >( user );
		void *raw = nullptr;
		memcpy( &raw, static_cast< uint8_t * >( memory ) - sizeof( void * ), sizeof( raw ) );
		t3dMemoryRelease( raw );
		--self.live_blocks;
	}
	t3dVTCookAllocator_t interface() {
		return { this, allocate, free };
	}
};

struct CookedPackage
{
	CookAllocator				 allocator;
	t3dVTCookedSparsePackage_t package{};
	~CookedPackage() {
		t3d_vt_cooker_release_sparse_package( package );
	}
	bool cook(
		const CookingSource &source,
		t3dVTEncodeTileFn	 encoder_override = nullptr ) {
		t3dVTCookerSettings_t settings{};
		t3d_vt_cooker_default_settings( settings );
		t3dVTEncoderInterface_t encoder{};
		t3d_vt_reference_encoder_make_interface( encoder );
		if ( encoder_override ) {
			encoder.m_pEncodeTile = encoder_override;
		}
		return t3d_vt_cooker_cook_sparse_pages( settings,
												   &source.source,
												   1,
												   encoder,
												   allocator.interface(),
												   nullptr,
												   package );
	}
};
} // namespace terrain3d_tests
#endif
