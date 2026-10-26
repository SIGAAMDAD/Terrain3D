#include "diagnostics/tests/systems/test_harness.h"
#include "data/storage/terrain_3d_raster_tile_store.h"
#include "authoring/vt/build/terrain_3d_vt_page_builder.h"
#include <godot_cpp/classes/image.hpp>
#include <stdint.h>

namespace terrain3d_tests
{
void register_localized_correctness_tests(
	TestRunner &runner ) {
	runner.run( "correctness", "mip_builder_rejects_zero_dimensions_in_supplied_layout", []( TestContext &test ) {
		uint8_t pixels[64]{};
		t3dVTMipChainLayout_t layout{};
		T3D_REQUIRE( test, t3d_vt_page_builder_layout_mips( 2u, 2u, 4u, layout ) );
		T3D_REQUIRE( test, layout.m_nMipCount == 2u );
		T3D_CHECK( test, t3d_vt_page_builder_build_logical_channel_mips_u8(
			T3D_VT_LOGICAL_CHANNEL_ALBEDO, true, 128u, pixels, sizeof( pixels ), layout, 2u ) );
		layout.m_Levels[0].m_nWidth = 0u;
		T3D_CHECK( test, !t3d_vt_page_builder_build_logical_channel_mips_u8(
			T3D_VT_LOGICAL_CHANNEL_ALBEDO, true, 128u, pixels, sizeof( pixels ), layout, 2u ) );
		layout.m_Levels[0].m_nWidth = 2u;
		layout.m_Levels[0].m_nHeight = 0u;
		T3D_CHECK( test, !t3d_vt_page_builder_build_logical_channel_mips_u8(
			T3D_VT_LOGICAL_CHANNEL_ALBEDO, true, 128u, pixels, sizeof( pixels ), layout, 2u ) );
	} );
	runner.run( "correctness", "raster_hash_index_tracks_live_revisions", []( TestContext &test ) {
		Terrain3DRasterTileStore store;
		const Vector2i region( 0, 0 );
		const uint16_t id = t3dRasterTileId( region, TYPE_HEIGHT, 0, 0 );
		const uint16_t other = t3dRasterTileId( region, TYPE_HEIGHT, 1, 0 );
		PackedByteArray pixels;
		pixels.resize( 128 * 128 * 2 );
		for ( uint32_t revision = 1u; revision <= 1024u; ++revision ) {
			uint8_t *const bytes = pixels.ptrw();
			bytes[0] = ( uint8_t ) ( revision & 0xffu );
			bytes[1] = ( uint8_t ) ( revision >> 8u );
			const Ref< Image > image = Image::create_from_data( 128, 128, false, Image::FORMAT_R16, pixels );
			T3D_REQUIRE( test, image.is_valid() );
			T3D_REQUIRE( test, store.updateRegion( region, TYPE_HEIGHT, image, Rect2i( 0, 0, 32, 32 ) ) );
			const Dictionary stats = store.getStats();
			T3D_CHECK( test, ( int64_t ) stats["raster_hash_index_entries"] <=
							 ( int64_t ) stats["raster_raw_tiles"] );
		}
		T3D_REQUIRE( test, store.findTile( id ) != NULL );
		const t3dRasterTile_t shared = *store.findTile( id );
		T3D_REQUIRE( test, shared.m_eEncoding == T3D_RASTER_TILE_RAW );
		store.importTile( other, shared );
		T3D_CHECK( test, ( int64_t ) store.getStats()["raster_hash_index_entries"] == 1 );
		// Replacing one indexed tile must retain a representative for the other.
		pixels.ptrw()[0] ^= 0x5au;
		const Ref< Image > next = Image::create_from_data( 128, 128, false, Image::FORMAT_R16, pixels );
		T3D_REQUIRE( test, store.updateRegion( region, TYPE_HEIGHT, next, Rect2i( 0, 0, 32, 32 ) ) );
		T3D_CHECK( test, ( int64_t ) store.getStats()["raster_hash_index_entries"] == 2 );
		store.removeRegion( region );
		T3D_CHECK( test, ( int64_t ) store.getStats()["raster_hash_index_entries"] == 0 );
		T3D_CHECK( test, store.findTile( id ) == NULL );
		T3D_CHECK( test, store.findTile( other ) == NULL );
	} );
	runner.run( "correctness", "raster_import_replacement_prunes_old_hash", []( TestContext &test ) {
		Terrain3DRasterTileStore store;
		const uint16_t id = t3dRasterTileId( Vector2i( 1, 1 ), TYPE_HEIGHT, 0, 0 );
		t3dRasterTile_t tile;
		tile.m_eEncoding = T3D_RASTER_TILE_RAW;
		tile.m_bytes.resize( t3dRasterTileRawBytes( TYPE_HEIGHT ) );
		for ( uint32_t revision = 1u; revision < 2048u; ++revision ) {
			tile.m_nHash = revision;
			tile.m_nRevision = revision;
			store.importTile( id, tile );
			T3D_CHECK( test, ( int64_t ) store.getStats()["raster_hash_index_entries"] == 1 );
		}
		store.clear();
		T3D_CHECK( test, ( int64_t ) store.getStats()["raster_hash_index_entries"] == 0 );
	} );
}
} // namespace terrain3d_tests
