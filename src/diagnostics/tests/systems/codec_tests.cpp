#include "core/utility/terrain_3d_binary_io.h"
#include "streaming/io/terrain_3d_cold_chunk_format.h"
#include "core/utility/terrain_3d_util.h"
#include "rendering/rd/vt/streaming/terrain_3d_vt_residency.h"
#include <godot_cpp/variant/rid.hpp>
#include "data/formats/terrain_3d_half.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "diagnostics/tests/systems/cooking_fixture.h"
#include "rendering/rd/vt/io/terrain_3d_vt_pack_runtime.h"
#include "rendering/rd/vt/io/terrain_3d_vt_file_source.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include "rendering/rd/vt/streaming/terrain_3d_vt_streaming.h"
#include "diagnostics/tests/systems/instance_fixture.h"
#include "diagnostics/tests/systems/test_harness.h"
#include "instances/storage/terrain_3d_instance_page_codec.h"
#include <string.h>
#include <new>

namespace terrain3d_tests
{
namespace
{
PackedByteArray encode_instances(
	const Terrain3DInstanceStore &store ) {
	PackedByteArray page;
	uint32_t		total = 0;
	uint16_t		meshes = 0;
	if ( !Terrain3DInstancePageCodec::encode( store, page, nullptr, total, meshes ) ) {
		return {};
	}
	return page;
}
PackedFloat32Array collision_heights() {
	PackedFloat32Array heights;
	heights.resize( ( T3D_REGION_SIZE + 1 ) * ( T3D_REGION_SIZE + 1 ) );
	for ( int64_t index = 0; index < heights.size(); ++index ) {
		heights.set( index, static_cast< float >( index % 129 ) - 64.5f );
	}
	return heights;
}
void *allocate_test_vt_pack(
	void *,
	size_t bytes,
	size_t alignment ) {
	return ::operator new( bytes, std::align_val_t( alignment ), std::nothrow );
}
void free_test_vt_pack(
	void *,
	void *memory ) {
	::operator delete( memory, std::align_val_t( 64u ) );
}
} // namespace

void register_codec_tests(
	TestRunner &runner ) {
	runner.run( "codec", "io00_indexed_cold_chunk_roundtrip_and_generation", []( TestContext &test ) {
		const uint8_t decoded[8] = { 17u, 17u, 17u, 17u, 21u, 21u, 21u, 21u };
		const uint8_t encoded[4] = { 4u, 17u, 4u, 21u };
		t3dColdChunkRecord_t record;
		record.m_iRegionX = -7;
		record.m_iRegionY = 8;
		record.m_nKind = 1u;
		record.m_nRegionGeneration = 12u;
		record.m_nMaterialGeneration = 18u;
		record.m_nCodec = T3D_COLD_CHUNK_CODEC_RLE;
		record.m_nEncodedBytes = sizeof( encoded );
		record.m_nDecodedBytes = sizeof( decoded );
		record.m_nOffset = 4096u;
		T3D_REQUIRE( test, t3dColdChunkSetHashes( record, encoded, decoded ) );
		uint8_t packed[T3D_COLD_CHUNK_RECORD_BYTES] = {};
		T3D_REQUIRE( test, t3dColdChunkWriteRecord( record, packed, sizeof( packed ) ) );
		t3dColdIndexHeader_t header;
		header.m_nRecordCount = 1u;
		header.m_nIndexOffset = 4100u;
		header.m_nIndexBytes = sizeof( packed );
		header.m_nIndexHash = t3dHashStableBytes( packed, sizeof( packed ) );
		header.m_nGeneration = 22u;
		uint8_t prefix[T3D_COLD_INDEX_HEADER_BYTES] = {};
		T3D_REQUIRE( test, t3dColdIndexWriteHeader( header, prefix, sizeof( prefix ) ) );
		t3dColdIndexHeader_t loadedHeader;
		T3D_REQUIRE( test, t3dColdIndexReadHeader( prefix, sizeof( prefix ), 4180u, loadedHeader ) );
		t3dColdChunkRecord_t loaded;
		T3D_REQUIRE( test, t3dColdIndexReadRecords( loadedHeader, packed, sizeof( packed ), &loaded, 1u ) );
		T3D_CHECK( test, t3dColdIndexFind( &loaded, 1u, -7, 8, 1u, 0u ) == &loaded );
		T3D_CHECK( test, !t3dColdChunkGenerationMatches( loaded, 11u, 18u ) );
		T3D_CHECK( test, t3dColdChunkGenerationMatches( loaded, 12u, 18u ) );
		uint8_t output[8] = {};
		T3D_CHECK( test, t3dColdChunkDecode( loaded, encoded, sizeof( encoded ), output, sizeof( output ) ) );
		T3D_CHECK( test, memcmp( output, decoded, sizeof( decoded ) ) == 0 );
		const uint8_t truncated[3] = { 4u, 17u, 4u };
		T3D_CHECK( test, !t3dColdChunkDecode( loaded, truncated, sizeof( truncated ), output, sizeof( output ) ) );
	} );
	runner.run( "codec", "control_word_bitcast_roundtrips_nan_and_sign_bit", []( TestContext &test ) {
		const uint32_t values[] = { 0u, 0x3f800000u, 0x80000000u, 0x7f800000u, 0xff800000u,
									0x7fc01234u, 0xffffffffu, 0x12345678u };
		for ( uint32_t bits : values ) {
			T3D_CHECK( test, t3dAsUint( t3dAsFloat( bits ) ) == bits );
		}
	} );
	runner.run( "codec", "godot_rid_native_handle_matches_eight_byte_abi", []( TestContext &test ) {
		static_assert( sizeof( godot::RID ) == sizeof( uint64_t ), "Godot RID native ABI size" );
		static_assert( alignof( godot::RID ) >= alignof( uint64_t ), "Godot RID alignment" );
		godot::RID object;
		const uint64_t handle = 0x123456789abcdef0ULL;
		memcpy( object._native_ptr(), &handle, sizeof( handle ) );
		uint64_t roundtrip = 0u;
		memcpy( &roundtrip, object._native_ptr(), sizeof( roundtrip ) );
		T3D_CHECK( test, roundtrip == handle );
	} );
	runner.run( "codec", "legacy_vt_pack_decodes_from_misaligned_bytes", []( TestContext &test ) {
		t3dVTAllocationRecordV1_t allocation{};
		t3dVTPageFileRecord_t page{};
		t3dVTPayloadRecord_t payload{};
		uint8_t payload_bytes[4] = { 1u, 2u, 3u, 4u };
		page.nVirtualPage = 0u;
		page.nChannel = 0u;
		page.nPayloadId = 0u;
		page.nParentPage = T3D_VT_INVALID_PAGE;
		payload.nOffset = 0u;
		payload.nCompressedBytes = 4u;
		t3dVTPackSource_t source{};
		source.pAllocations = &allocation;
		source.nAllocationCount = 1u;
		source.pPages = &page;
		source.nPageCount = 1u;
		source.pPayloads = &payload;
		source.nPayloadCount = 1u;
		source.pPayloadData = payload_bytes;
		source.nPayloadDataBytes = sizeof( payload_bytes );
		const uint64_t nBytes = t3d_vt_compute_pack_bytes( source );
		T3D_REQUIRE( test, nBytes > 0u && nBytes < 1024u );
		uint8_t buffer[1024]{};
		T3D_REQUIRE( test, t3d_vt_write_pack_cold( source, buffer + 1, nBytes ) );
		t3dVTLegacyDecodeStorage_t storage;
		t3dVTDatabaseView_t view{};
		T3D_REQUIRE( test, t3d_vt_open_pack_cold( buffer + 1, nBytes, storage, view ) );
		T3D_CHECK( test, view.pHeader == &storage.Header && view.pPages == storage.Pages.data() );
		T3D_CHECK( test, view.pPages[0].nVirtualPage == 0u && view.pPayloadData[0] == 1u );
		T3D_CHECK( test, reinterpret_cast< uintptr_t >( view.pPages ) % alignof( t3dVTPageFileRecord_t ) == 0u );
		T3D_CHECK( test, !t3d_vt_open_pack_cold( buffer + 1, sizeof( t3dVTFileHeader_t ), storage, view ) );
	} );
	// Explicit endian golden bytes and checked cursors.
	runner.run( "codec", "little_endian_integer_encoding_matches_golden_bytes", []( TestContext &test ) {
		uint8_t		  bytes[8]{};
		const uint8_t expected[8] = { 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01 };
		t3dWriteLittleUnsigned< uint64_t >( bytes, 0x0123456789abcdefull );
		T3D_CHECK( test, memcmp( bytes, expected, 8 ) == 0 );
		T3D_CHECK( test, t3dReadLittleUnsigned< uint64_t >( expected ) == 0x0123456789abcdefull );
	} );
	runner.run( "codec", "float_encoding_matches_ieee754_golden_bytes", []( TestContext &test ) {
		uint8_t		  bytes[4]{};
		const uint8_t expected[4] = { 0, 0, 0x80, 0x3f };
		t3dWriteLittleF32( bytes, 1.0f );
		T3D_CHECK( test, memcmp( bytes, expected, 4 ) == 0 && t3dReadLittleF32( expected ) == 1.0f );
	} );
	runner.run( "codec", "mixed_cursor_records_roundtrip_at_unaligned_offsets", []( TestContext &test ) {
		uint8_t			bytes[19]{};
		t3dByteWriter_t writer( bytes, sizeof( bytes ) );
		T3D_REQUIRE( test,
					 writer.writeU8( 7 ) && writer.writeU16( 0x1234 ) && writer.writeI32( -42 ) &&
						 writer.writeU64( UINT64_MAX ) && writer.writeF32( -0.25f ) );
		T3D_CHECK( test, writer.complete() );
		t3dByteReader_t reader( bytes, sizeof( bytes ) );
		T3D_CHECK( test, reader.readU8() == 7 && reader.readU16() == 0x1234 && reader.readI32() == -42 );
		T3D_CHECK( test, reader.readU64() == UINT64_MAX && reader.readF32() == -0.25f && reader.complete() );
	} );
	runner.run( "codec", "failed_write_latches_failure_and_preserves_guards", []( TestContext &test ) {
		uint8_t bytes[6];
		memset( bytes, 0xa5, sizeof( bytes ) );
		t3dByteWriter_t writer( bytes + 1, 4 );
		T3D_REQUIRE( test, writer.writeU16( 0x1234 ) );
		T3D_CHECK( test, !writer.writeU32( 42 ) && writer.offset() == 2 );
		T3D_CHECK( test, !writer.writeU8( 9 ) && bytes[3] == 0xa5 && bytes[0] == 0xa5 && bytes[5] == 0xa5 );
	} );
	runner.run( "codec", "failed_read_never_advances_and_cannot_resume", []( TestContext &test ) {
		const uint8_t	bytes[3] = { 7, 8, 9 };
		t3dByteReader_t reader( bytes, 3 );
		T3D_REQUIRE( test, reader.readU8() == 7 );
		T3D_CHECK( test, reader.readU32() == 0 && !reader.m_bValid && reader.m_iOffset == 1 );
		T3D_CHECK( test, reader.readU8() == 0 && reader.m_iOffset == 1 && !reader.complete() );
	} );
	runner.run( "codec",
				"null_empty_cursors_are_complete_but_nonempty_null_cursors_are_invalid",
				[]( TestContext &test ) {
					t3dByteReader_t empty_reader( nullptr, 0 ), invalid_reader( nullptr, 1 );
					t3dByteWriter_t empty_writer( nullptr, 0 ), invalid_writer( nullptr, 1 );
					T3D_CHECK( test, empty_reader.complete() && empty_writer.complete() );
					T3D_CHECK( test, !invalid_reader.m_bValid && !invalid_writer.m_bValid );
					T3D_CHECK( test, !invalid_writer.writeU8( 1 ) && invalid_reader.readU8() == 0 );
				} );
	runner.run( "codec", "zero_padding_writes_only_the_requested_span", []( TestContext &test ) {
		uint8_t bytes[8];
		memset( bytes, 0xa5, sizeof( bytes ) );
		t3dByteWriter_t writer( bytes, 8 );
		T3D_REQUIRE( test, writer.writeU8( 9 ) && writer.skipZeroed( 3 ) );
		T3D_CHECK( test, bytes[0] == 9 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0 && bytes[4] == 0xa5 );
	} );
	// Binary16: golden values, rounding and exhaustive representable values.
	runner.run( "codec", "half_encoding_matches_known_values_and_signed_zero", []( TestContext &test ) {
		T3D_CHECK( test, t3dHalfEncode( 0.0f ) == 0 && t3dHalfEncode( -0.0f ) == 0x8000 );
		T3D_CHECK( test, t3dHalfEncode( 1.0f ) == 0x3c00 && t3dHalfEncode( -2.0f ) == 0xc000 );
		T3D_CHECK( test, t3dHalfEncode( 65504.0f ) == 0x7bff );
		T3D_CHECK( test, signbit( t3dHalfDecode( 0x8000 ) ) );
	} );
	runner.run( "codec", "half_rounding_uses_ties_to_even", []( TestContext &test ) {
		T3D_CHECK( test, t3dHalfEncode( 1.0f + 1.0f / 2048.0f ) == 0x3c00 );
		T3D_CHECK( test, t3dHalfEncode( 1.0f + 3.0f / 2048.0f ) == 0x3c02 );
	} );
	runner.run( "codec", "half_special_values_keep_infinity_sign_and_nan_semantics", []( TestContext &test ) {
		T3D_CHECK( test, t3dHalfEncode( INFINITY ) == 0x7c00 && t3dHalfEncode( -INFINITY ) == 0xfc00 );
		T3D_CHECK( test, isinf( t3dHalfDecode( 0x7c00 ) ) && isnan( t3dHalfDecode( t3dHalfEncode( NAN ) ) ) );
		T3D_CHECK( test, t3dHalfEncode( 1e20f ) == 0x7c00 && t3dHalfEncode( -1e20f ) == 0xfc00 );
	} );
	runner.run( "codec", "every_finite_half_value_reencodes_to_the_same_bits", []( TestContext &test ) {
		for ( uint32_t bits = 0; bits <= UINT16_MAX; ++bits ) {
			if ( ( bits & 0x7c00u ) != 0x7c00u ) {
				T3D_CHECK( test, t3dHalfEncode( t3dHalfDecode( static_cast< uint16_t >( bits ) ) ) == bits );
			}
		}
	} );
	runner.run( "codec", "packed_transform_roundtrip_keeps_origin_and_basis_precision", []( TestContext &test ) {
		Transform3D source;
		source.origin = Vector3( -123.25, 456.5, 789.75 );
		source.basis = Basis::from_euler( Vector3( 0.3, 0.7, -0.2 ) ).scaled( Vector3( 1.25, 2.0, 0.75 ) );
		t3dPackedTransform_t packed( source );
		const Transform3D	 decoded = packed.get();
		T3D_CHECK( test, decoded.origin == source.origin );
		for ( int row = 0; row < 3; ++row ) {
			for ( int column = 0; column < 3; ++column ) {
				T3D_CHECK( test, near( decoded.basis[row][column], source.basis[row][column], 0.001 ) );
			}
		}
	} );
	runner.run( "codec", "packed_transform_masks_unknown_behavior_flags", []( TestContext &test ) {
		t3dPackedTransform_t packed;
		packed.setWithBehaviorFlags( Transform3D(), UINT16_MAX );
		T3D_CHECK( test, packed.getBehaviorFlags() == T3D_INSTANCE_BEHAVIOR_MASK );
		packed.set( Transform3D() );
		T3D_CHECK( test, packed.getBehaviorFlags() == T3D_INSTANCE_BEHAVIOR_MASK );
	} );
	// Region height codec: quantization bounds and sanitization.
	runner.run( "codec", "height_endpoints_clamp_to_the_representable_range", []( TestContext &test ) {
		T3D_CHECK( test, t3dRegionCodecEncodeHeightU16( s_fRegionHeightMin - 1 ) == 0 );
		T3D_CHECK( test, t3dRegionCodecEncodeHeightU16( s_fRegionHeightMax + 1 ) == UINT16_MAX );
		T3D_CHECK( test, near( t3dRegionCodecDecodeHeightU16( 0 ), s_fRegionHeightMin ) );
		T3D_CHECK( test, near( t3dRegionCodecDecodeHeightU16( UINT16_MAX ), s_fRegionHeightMax ) );
	} );
	runner.run( "codec", "height_quantization_stays_within_half_a_quantization_step", []( TestContext &test ) {
		const double tolerance = s_fRegionHeightRange / 65535.0 / 2.0 + 0.0002;
		for ( int index = 0; index <= 4096; ++index ) {
			const float height = s_fRegionHeightMin + s_fRegionHeightRange * ( static_cast< float >( index ) / 4096 );
			T3D_CHECK(
				test,
				near( t3dRegionCodecDecodeHeightU16( t3dRegionCodecEncodeHeightU16( height ) ), height, tolerance ) );
		}
	} );
	runner.run( "codec", "height_conversion_counts_clamped_and_nonfinite_samples", []( TestContext &test ) {
		PackedByteArray bytes;
		bytes.resize( 16 );
		const float heights[4] = { s_fRegionHeightMin - 100, s_fRegionHeightMax + 100, NAN, INFINITY };
		memcpy( bytes.ptrw(), heights, sizeof( heights ) );
		const auto source = Image::create_from_data( 2, 2, false, Image::FORMAT_RF, bytes );
		uint32_t   clamped = 0, nonfinite = 0;
		const auto decoded = t3dRegionCodecConvertHeightToR16( source, 2, clamped, nonfinite );
		T3D_REQUIRE( test, decoded.is_valid() );
		T3D_CHECK( test, clamped == 2 && nonfinite == 2 && decoded->get_format() == Image::FORMAT_R16 );
		T3D_CHECK( test, near( t3dRegionCodecReadHeightPixel( *decoded, Vector2i( 0, 1 ) ), 0, 0.02 ) );
	} );
	runner.run( "codec", "height_conversion_reuses_already_valid_r16_storage", []( TestContext &test ) {
		const auto source = t3dRegionCodecConvertHeightToR16( Ref< Image >(), 4 );
		T3D_REQUIRE( test, source.is_valid() );
		T3D_CHECK( test, t3dRegionCodecConvertHeightToR16( source, 4 ) == source );
		T3D_CHECK( test, t3dRegionCodecConvertHeightToR16( source, 0 ).is_null() );
	} );
	runner.run( "codec", "height_byte_reader_supports_unaligned_r16_and_rf_payloads", []( TestContext &test ) {
		uint8_t		   bytes[9]{};
		const uint16_t encoded = t3dRegionCodecEncodeHeightU16( 32 );
		memcpy( bytes + 1, &encoded, sizeof( encoded ) );
		const float height = -17.25f;
		memcpy( bytes + 5, &height, sizeof( height ) );
		T3D_CHECK( test, near( t3dRegionCodecReadHeightBytes( bytes + 1, 0, Image::FORMAT_R16 ), 32, 0.02 ) );
		T3D_CHECK( test, t3dRegionCodecReadHeightBytes( bytes + 5, 0, Image::FORMAT_RF ) == height );
		T3D_CHECK( test, isnan( t3dRegionCodecReadHeightBytes( nullptr, 0, Image::FORMAT_RF ) ) );
	} );
	// Instance pages: roundtrip, inspect and atomic rejection.
	runner.run( "codec",
				"instance_page_roundtrip_preserves_transforms_colors_owners_and_flags",
				[]( TestContext &test ) {
					Terrain3DInstanceStore source, decoded;
					T3D_REQUIRE( test, populate_instance_store( source ) );
					const auto page = encode_instances( source );
					uint32_t   total = 0, counts[T3D_MAX_MESH_ASSETS]{};
					uint16_t   meshes = 0;
					T3D_REQUIRE( test,
								 !page.is_empty() &&
									 Terrain3DInstancePageCodec::decode( page, decoded, counts, total, meshes ) );
					T3D_CHECK( test, total == 12 && meshes == 2 && counts[1] == 6 && counts[3] == 6 );
					T3D_CHECK( test, same_instance_store( source, decoded ) );
				} );
	runner.run( "codec", "instance_page_inspection_matches_decoded_counts", []( TestContext &test ) {
		Terrain3DInstanceStore source;
		T3D_REQUIRE( test, populate_instance_store( source ) );
		const auto page = encode_instances( source );
		uint32_t   total = 0, counts[T3D_MAX_MESH_ASSETS]{};
		uint16_t   meshes = 0;
		T3D_CHECK( test, Terrain3DInstancePageCodec::inspect( page, counts, total, meshes ) );
		T3D_CHECK( test, total == 12 && meshes == 2 && counts[2] == 0 && counts[3] == 6 );
	} );
	runner.run( "codec", "empty_instance_page_replaces_a_populated_destination", []( TestContext &test ) {
		Terrain3DInstanceStore empty, decoded;
		T3D_REQUIRE( test, populate_instance_store( decoded ) );
		const auto page = encode_instances( empty );
		uint32_t   total = 99;
		uint16_t   meshes = 99;
		T3D_CHECK( test, Terrain3DInstancePageCodec::decode( page, decoded, nullptr, total, meshes ) );
		T3D_CHECK( test, total == 0 && meshes == 0 && decoded.empty() && decoded.validateDeep() );
	} );
	runner.run( "codec",
				"nonempty_instance_page_truncations_are_rejected_without_mutating_the_store",
				[]( TestContext &test ) {
					Terrain3DInstanceStore source, destination;
					T3D_REQUIRE( test, populate_instance_store( source ) && populate_instance_store( destination ) );
					const auto page = encode_instances( source );
					uint32_t   total = 0;
					uint16_t   meshes = 0;
					// Zero bytes is the codec's valid empty sentinel, not malformed input.
					for ( int64_t length = 1; length < page.size(); ++length ) {
						T3D_CHECK( test,
								   !Terrain3DInstancePageCodec::decode( page,
																		static_cast< size_t >( length ),
																		destination,
																		nullptr,
																		total,
																		meshes ) );
					}
					T3D_CHECK( test, same_instance_store( source, destination ) );
				} );
	runner.run( "codec", "zero_byte_instance_sentinel_clears_the_store_and_keeps_its_layout", []( TestContext &test ) {
		Terrain3DInstanceStore destination;
		T3D_REQUIRE( test, populate_instance_store( destination ) );
		t3dInstanceLayout_t layout;
		T3D_REQUIRE( test, t3dInstanceLayoutForCellSize( 16, layout ) );
		destination.setLayout( layout );
		PackedByteArray sentinel;
		uint32_t		total = 99;
		uint16_t		meshes = 99;
		T3D_CHECK( test, Terrain3DInstancePageCodec::decode( sentinel, destination, nullptr, total, meshes ) );
		T3D_CHECK( test, destination.empty() && total == 0 && meshes == 0 && destination.validateDeep() );
		T3D_CHECK( test, destination.getLayout().m_nCellSize == 16 );
	} );
	runner.run( "codec", "unsupported_instance_page_version_is_rejected_transactionally", []( TestContext &test ) {
		Terrain3DInstanceStore source, destination;
		T3D_REQUIRE( test, populate_instance_store( source ) && populate_instance_store( destination ) );
		auto page = encode_instances( source );
		t3dWriteLittleUnsigned< uint16_t >( page.ptrw() + 4, UINT16_MAX );
		uint32_t total = 0;
		uint16_t meshes = 0;
		T3D_CHECK( test, !Terrain3DInstancePageCodec::decode( page, destination, nullptr, total, meshes ) );
		T3D_CHECK( test, same_instance_store( source, destination ) );
	} );
	runner.run( "codec", "instance_page_declared_count_must_match_its_payload", []( TestContext &test ) {
		Terrain3DInstanceStore source;
		T3D_REQUIRE( test, populate_instance_store( source ) );
		auto page = encode_instances( source );
		t3dWriteLittleUnsigned< uint32_t >( page.ptrw() + 8, UINT32_MAX );
		uint32_t total = 0;
		uint16_t meshes = 0;
		T3D_CHECK( test, !Terrain3DInstancePageCodec::inspect( page, nullptr, total, meshes ) );
	} );
	// Runtime masks and collision payloads.
	runner.run( "codec", "navigation_mask_excludes_holes_even_when_navigation_is_set", []( TestContext &test ) {
		PackedInt32Array controls;
		controls.resize( 129 * 129 );
		controls.fill( 0 );
		controls.set( 0, 2 );
		controls.set( 1, 4 );
		controls.set( 2, 6 );
		const auto				blob = Terrain3DRuntimeBakeCodec::encodeNavigationMask( controls );
		t3dNavigationMaskView_t view;
		T3D_REQUIRE( test, Terrain3DRuntimeBakeCodec::getNavigationMaskView( blob, view ) );
		T3D_CHECK( test, view.isValidSample( 0 ) && view.isNavigationSample( 0 ) );
		T3D_CHECK( test, !view.isValidSample( 1 ) && !view.isNavigationSample( 1 ) );
		T3D_CHECK( test, !view.isValidSample( 2 ) && !view.isNavigationSample( 2 ) );
		T3D_CHECK( test, view.isValidSample( 3 ) && !view.isNavigationSample( 3 ) );
		T3D_CHECK( test, !view.isValidSample( view.m_nSamples ) );
	} );
	runner.run( "codec", "navigation_mask_rejects_bad_magic_sizes_and_truncated_planes", []( TestContext &test ) {
		const auto valid = Terrain3DRuntimeBakeCodec::encodeNavigationMask( Ref< Image >() );
		T3D_REQUIRE( test, !valid.is_empty() );
		for ( const int offset : { 0, 4, 6, 8, 12 } ) {
			auto damaged = valid;
			damaged.set( offset, damaged[offset] ^ 0x7f );
			t3dNavigationMaskView_t view;
			T3D_CHECK( test, !Terrain3DRuntimeBakeCodec::getNavigationMaskView( damaged, view ) );
		}
		auto truncated = valid;
		truncated.resize( truncated.size() - 1 );
		t3dNavigationMaskView_t view;
		T3D_CHECK( test, !Terrain3DRuntimeBakeCodec::getNavigationMaskView( truncated, view ) );
	} );
	runner.run( "codec", "collision_height_roundtrip_preserves_every_sample", []( TestContext &test ) {
		const auto		   source = collision_heights();
		const auto		   blob = Terrain3DRuntimeBakeCodec::encodeCollisionHeight( source );
		PackedFloat32Array decoded;
		int				   width = 0;
		T3D_REQUIRE( test, Terrain3DRuntimeBakeCodec::decodeCollisionHeight( blob, decoded, width ) );
		T3D_CHECK( test, width == 129 && decoded == source );
	} );
	runner.run( "codec", "collision_decode_into_requires_preallocated_exact_size", []( TestContext &test ) {
		const auto		   source = collision_heights();
		const auto		   blob = Terrain3DRuntimeBakeCodec::encodeCollisionHeight( source );
		PackedFloat32Array destination;
		destination.resize( 1 );
		destination.set( 0, 42 );
		int width = 0;
		T3D_CHECK( test, !Terrain3DRuntimeBakeCodec::decodeCollisionHeightInto( blob, destination, width ) );
		T3D_CHECK( test, destination.size() == 1 && destination[0] == 42 );
		destination.resize( source.size() );
		T3D_CHECK( test,
				   Terrain3DRuntimeBakeCodec::decodeCollisionHeightInto( blob, destination, width ) &&
					   destination == source );
	} );
	runner.run( "codec", "collision_height_rejects_wrong_sample_count_and_truncation", []( TestContext &test ) {
		PackedFloat32Array wrong;
		wrong.resize( 128 * 128 );
		T3D_CHECK( test, Terrain3DRuntimeBakeCodec::encodeCollisionHeight( wrong ).is_empty() );
		auto blob = Terrain3DRuntimeBakeCodec::encodeCollisionHeight( collision_heights() );
		blob.resize( blob.size() - 1 );
		PackedFloat32Array decoded;
		int				   width = 0;
		T3D_CHECK( test, !Terrain3DRuntimeBakeCodec::decodeCollisionHeight( blob, decoded, width ) );
	} );
	runner.run( "codec", "collision_bounds_roundtrip_keeps_signed_height_range", []( TestContext &test ) {
		const auto blob = Terrain3DRuntimeBakeCodec::encodeCollisionBounds( -23.5f, 456.25f );
		float	   low = 0, high = 0;
		int		   width = 0;
		T3D_CHECK( test, Terrain3DRuntimeBakeCodec::decodeCollisionBounds( blob, low, high, width ) );
		T3D_CHECK( test, low == -23.5f && high == 456.25f && width == 129 );
	} );
	// Sparse VT serialization uses real cooked bytes, not a handcrafted mock pack.
	runner.run( "codec", "sparse_vt_pack_roundtrip_preserves_stable_keys_and_payloads", []( TestContext &test ) {
		CookingSource	source;
		PackedByteArray bytes;
		T3D_REQUIRE( test, source.fragment( bytes ) );
		t3dVTPackViewV3_t view{};
		T3D_REQUIRE( test, t3d_vt_pack_open_view_v3( bytes.ptr(), bytes.size(), view ) );
		T3D_CHECK( test, view.m_pHeader->m_nAllocationCount == 1 && view.m_pAllocationKeys[0].m_nStableKey == 101 );
		T3D_CHECK( test, view.m_pHeader->m_nPayloadCount > 0 && view.m_pHeader->m_nTailCount > 0 );
	} );
	runner.run( "codec", "sparse_vt_metadata_only_open_and_file_range_reads", []( TestContext &test ) {
		CookingSource	input;
		PackedByteArray bytes;
		T3D_REQUIRE( test, input.fragment( bytes ) );
		t3dVTPackViewV3_t full{};
		T3D_REQUIRE( test, t3d_vt_pack_open_view_v3( bytes.ptr(), bytes.size(), full ) );
		const uint64_t	  prefix = full.m_pHeader->m_nPayloadDataOffset;
		t3dVTPackViewV3_t metadata{};
		T3D_REQUIRE( test, t3d_vt_pack_open_metadata_view_v3( bytes.ptr(), prefix, bytes.size(), metadata ) );
		T3D_CHECK( test,
				   metadata.m_pPayloadData == nullptr &&
					   metadata.m_pHeader->m_nContentSignature == full.m_pHeader->m_nContentSignature );
		T3D_CHECK( test, !t3d_vt_pack_open_metadata_view_v3( bytes.ptr(), prefix - 1u, bytes.size(), metadata ) );
		T3D_CHECK( test, !t3d_vt_pack_open_metadata_view_v3( bytes.ptr(), prefix, bytes.size() + 1u, metadata ) );

		const String path = "user://terrain3d_vt_file_source_test.t3dvt";
		{
			const Ref< FileAccess > write = FileAccess::open( path, FileAccess::WRITE );
			T3D_REQUIRE( test, write.is_valid() );
			write->store_buffer( bytes );
		}
		t3dVTFileSource_s source{};
		PackedByteArray	  prefixBytes;
		T3D_REQUIRE( test, t3d_vt_file_source_open_cold( path, source, prefixBytes, true ) );
		T3D_REQUIRE(
			test,
			t3d_vt_pack_open_metadata_view_v3( prefixBytes.ptr(), prefixBytes.size(), source.nFileBytes, metadata ) );
		const t3dVTPackPayloadRecordV3_t payload = metadata.m_pPayloads[0u];
		const t3dVTPackClusterRecordV3_t cluster = metadata.m_pClusters[payload.m_nClusterId];
		const uint64_t					 offset = cluster.m_nPayloadDataOffset + payload.m_nClusterOffset;
		uint8_t							 scratch[T3D_VT_PHYSICAL_TILE * T3D_VT_PHYSICAL_TILE]{};
		T3D_REQUIRE( test, payload.m_nDecodedBytes <= sizeof( scratch ) );
		T3D_REQUIRE( test, t3d_vt_file_source_read_payload( source, offset, scratch, payload.m_nDecodedBytes ) );
		T3D_CHECK( test, memcmp( scratch, bytes.ptr() + prefix + offset, payload.m_nDecodedBytes ) == 0 );
		T3D_CHECK( test, !t3d_vt_file_source_read_payload( source, source.nPayloadBytes - 1u, scratch, 2u ) );
		t3d_vt_file_source_close( source );
		T3D_CHECK( test, source.pNativeFile == nullptr && source.m_File.is_null() );
		PackedByteArray corrupted = bytes;
		corrupted.set( corrupted.size() - 1, corrupted[corrupted.size() - 1] ^ 1u );
		{
			const Ref< FileAccess > write = FileAccess::open( path, FileAccess::WRITE );
			T3D_REQUIRE( test, write.is_valid() );
			write->store_buffer( corrupted );
		}
		T3D_CHECK( test, !t3d_vt_file_source_open_cold( path, source, prefixBytes, true ) );
		t3d_vt_file_source_close( source );
		const ProjectSettings *settings = ProjectSettings::get_singleton();
		if ( settings != nullptr ) {
			DirAccess::remove_absolute( settings->globalize_path( path ) );
		}
	} );
	runner.run( "codec", "sparse_vt_file_runtime_retains_metadata_not_payload", []( TestContext &test ) {
		CookingSource	input;
		PackedByteArray bytes;
		T3D_REQUIRE( test, input.fragment( bytes ) );
		const String path = "user://terrain3d_vt_runtime_file_test.t3dvt";
		{
			const Ref< FileAccess > write = FileAccess::open( path, FileAccess::WRITE );
			T3D_REQUIRE( test, write.is_valid() );
			write->store_buffer( bytes );
		}
		const t3dVTPackRuntimeAllocator_t allocator{ nullptr, allocate_test_vt_pack, free_test_vt_pack };
		t3dVTPackRuntimeStorage_t		  resident{}, streamed{};
		t3d_vt_pack_runtime_clear( resident );
		t3d_vt_pack_runtime_clear( streamed );
		const bool residentReady =
			t3d_vt_pack_runtime_open_cold( bytes.ptr(), ( uint64_t ) bytes.size(), allocator, resident );
		const bool streamedReady = t3d_vt_pack_runtime_open_file_cold( path, allocator, streamed );
		T3D_CHECK( test, residentReady && streamedReady );
		if ( residentReady && streamedReady ) {
			T3D_CHECK( test, streamed.pFileSource != nullptr && streamed.Database.pFileSource == streamed.pFileSource );
			T3D_CHECK( test, streamed.Database.pPayloadData == nullptr );
			T3D_CHECK( test, streamed.nPackageBytes == ( uint64_t ) bytes.size() );
			T3D_CHECK( test, streamed.nOwnedBytes < resident.nOwnedBytes );
			T3D_CHECK( test, streamed.Database.pHeader->nPageCount == resident.Database.pHeader->nPageCount );
			T3D_CHECK( test, streamed.Database.nPackageContentSignature == resident.Database.nPackageContentSignature );
		}
		t3d_vt_pack_runtime_release_cold( allocator, resident );
		t3d_vt_pack_runtime_release_cold( allocator, streamed );
		const ProjectSettings *settings = ProjectSettings::get_singleton();
		if ( settings != nullptr ) {
			DirAccess::remove_absolute( settings->globalize_path( path ) );
		}
	} );
	runner.run( "codec",
				"sparse_vt_reader_handles_unaligned_storage_and_rejects_misaligned_directories",
				[]( TestContext &test ) {
					CookingSource	source;
					PackedByteArray bytes;
					T3D_REQUIRE( test, source.fragment( bytes ) );
					PackedByteArray unaligned;
					T3D_REQUIRE( test, unaligned.resize( bytes.size() + 1 ) == OK );
					memcpy( unaligned.ptrw() + 1, bytes.ptr(), bytes.size() );
					t3dVTPackViewV3_t view{};
					T3D_REQUIRE( test, t3d_vt_pack_open_view_v3( unaligned.ptr() + 1, bytes.size(), view ) );
					T3D_CHECK( test,
							   view.m_pHeader->m_nAllocationCount == 1u &&
								   view.m_pAllocationKeys[0].m_nStableKey == 101u );
					t3dVTPackHeaderV3_t header{};
					memcpy( &header, bytes.ptr(), sizeof( header ) );
					++header.m_nPageOffset;
					header.m_nHeaderChecksum = 0u;
					header.m_nHeaderChecksum = t3d_vt_pack_checksum32( &header, sizeof( header ) );
					memcpy( unaligned.ptrw() + 1, &header, sizeof( header ) );
					T3D_CHECK( test, !t3d_vt_pack_open_view_v3( unaligned.ptr() + 1, bytes.size(), view ) );
				} );
	runner.run( "codec", "vt_runtime_package_transfer_clears_source_without_copying_storage", []( TestContext &test ) {
		alignas( 64 ) uint8_t	  block[128]{};
		t3dVTPackRuntimeStorage_t source{}, destination{};
		t3d_vt_pack_runtime_clear( source );
		t3d_vt_pack_runtime_clear( destination );
		source.pOwnedBlock = block;
		source.pPackageBytes = block + 64;
		source.nOwnedBytes = sizeof( block );
		T3D_REQUIRE( test, t3d_vt_pack_runtime_transfer( destination, source ) );
		T3D_CHECK( test, source.pOwnedBlock == nullptr && source.pPackageBytes == nullptr );
		T3D_CHECK( test, destination.pOwnedBlock == block && destination.pPackageBytes == block + 64 );
		T3D_CHECK( test, t3d_vt_pack_runtime_transfer( source, destination ) );
		T3D_CHECK( test, destination.pOwnedBlock == nullptr && source.pOwnedBlock == block );
		t3d_vt_pack_runtime_clear( source );
	} );
	runner.run( "codec", "vt_stream_readback_decoder_accepts_unaligned_requests", []( TestContext &test ) {
		uint8_t			   bytes[1 + sizeof( t3dVTGpuCounters_t ) + 2 * sizeof( t3dVTRuntimeRequest_t )]{};
		t3dVTGpuCounters_t counters{};
		counters.nRequestCount = 2u;
		const t3dVTRuntimeRequest_t expected[2] = { { 7u, 1u, 4u }, { 9u, 2u, 3u } };
		memcpy( bytes + 1, &counters, sizeof( counters ) );
		memcpy( bytes + 1 + sizeof( counters ), expected, sizeof( expected ) );
		uint32_t		   offset = 0u, count = 0u;
		t3dVTGpuCounters_t parsed{};
		T3D_REQUIRE(
			test,
			t3d_vt_decode_readback_reference_debug( bytes + 1, sizeof( bytes ) - 1, 2u, parsed, offset, count ) );
		T3D_CHECK( test, count == 2u && offset == sizeof( counters ) && parsed.nRequestCount == 2u );
		t3dVTRuntimeRequest_t copy{};
		memcpy( &copy, bytes + 1 + offset, sizeof( copy ) );
		T3D_CHECK( test, copy.nVirtualPage == 7u && copy.nWantedMip == 1u && copy.nPriority == 4u );
		T3D_CHECK(
			test,
			!t3d_vt_decode_readback_reference_debug( bytes + 1, sizeof( bytes ) - 2, 2u, parsed, offset, count ) );
	} );
	runner.run( "codec",
				"sparse_vt_pack_rejects_every_truncated_header_and_the_last_missing_byte",
				[]( TestContext &test ) {
					CookingSource	source;
					PackedByteArray bytes;
					T3D_REQUIRE( test, source.fragment( bytes ) );
					for ( uint32_t length = 0; length < T3D_VT_PACK_V3_HEADER_BYTES; ++length ) {
						t3dVTPackViewV3_t view{};
						T3D_CHECK( test, !t3d_vt_pack_open_view_v3( bytes.ptr(), length, view ) );
					}
					t3dVTPackViewV3_t view{};
					T3D_CHECK( test, !t3d_vt_pack_open_view_v3( bytes.ptr(), bytes.size() - 1, view ) );
				} );
	runner.run( "codec", "sparse_vt_pack_rejects_corrupt_magic_and_payload_checksum", []( TestContext &test ) {
		CookingSource	source;
		PackedByteArray valid;
		T3D_REQUIRE( test, source.fragment( valid ) );
		for ( const int64_t offset : { int64_t( 0 ), valid.size() - 1 } ) {
			auto damaged = valid;
			damaged.set( offset, damaged[offset] ^ 1 );
			t3dVTPackViewV3_t view{};
			T3D_CHECK( test, !t3d_vt_pack_open_view_v3( damaged.ptr(), damaged.size(), view ) );
		}
	} );
	runner.run( "codec", "sparse_vt_writer_requires_the_measured_output_capacity", []( TestContext &test ) {
		CookingSource source;
		CookedPackage cooked;
		T3D_REQUIRE( test, cooked.cook( source ) );
		t3dVTPackSourceV3_t pack_source{};
		T3D_REQUIRE( test, t3d_vt_cooker_build_pack_source_v3( cooked.package, pack_source ) );
		const uint64_t expected = t3d_vt_pack_compute_bytes_v3( pack_source );
		T3D_REQUIRE( test, expected > T3D_VT_PACK_V3_HEADER_BYTES );
		PackedByteArray bytes;
		bytes.resize( expected + 1 );
		bytes.fill( 0xa5 );
		uint64_t written = 99;
		T3D_CHECK( test, !t3d_vt_pack_write_v3( pack_source, bytes.ptrw(), expected - 1, written ) );
		T3D_CHECK( test, written == 0 && bytes[expected] == 0xa5 );
		T3D_CHECK( test, t3d_vt_pack_write_v3( pack_source, bytes.ptrw(), expected, written ) && written == expected );
		t3dVTPackViewV3_t view{};
		T3D_CHECK( test, t3d_vt_pack_open_view_v3( bytes.ptr(), written, view ) && bytes[expected] == 0xa5 );
	} );
}
} // namespace terrain3d_tests
