#include "data/cooking/terrain_3d_region_image_cooker.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "diagnostics/tests/systems/cooking_fixture.h"
#include "diagnostics/tests/systems/test_harness.h"
#include "rendering/rd/preview/terrain_3d_rd_preview_service.h"
#include "authoring/vt/build/terrain_3d_incremental_megavt_cooker.h"
#include "assets/resources/terrain_3d_mesh_asset.h"
#include "diagnostics/tests/unit_testing.h"
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <string.h>

namespace terrain3d_tests {
namespace {
bool reject_encoding(void *, const t3dVTEncodeTileRequest_t &, uint32_t &written) {
	written = 0;
	return false;
}
bool report_short_encoding(void *, const t3dVTEncodeTileRequest_t &, uint32_t &written) {
	written = 1;
	return true;
}

// Publication is observed at the preview host boundary; cooking and budget
// admission remain production code. No GPU is needed to reproduce a denied
// instance package becoming permanently quarantined.
struct PreviewCookFixture : Terrain3DPreviewHost {
	Terrain3DRdPreviewService preview;
	Terrain3DVirtualTexture virtual_texture;
	Terrain3DSharedVTRuntime shared_vt;
	Terrain3DRdShaderLibrary shaders;
	Terrain3DRenderPublicationBuilder publications;
	Ref<Terrain3DRdCompositorEffect> effect;
	t3dAtomicU32_t generation{1u};
	Terrain3DMutex diagnostics;
	int published = 0;
	bool live = false;
	bool change_during_composition = false;
	t3dPreviewServices_t services() {
		return {nullptr, virtual_texture, shared_vt, shaders, publications, effect, generation, diagnostics, *this};
	}
	bool composePreviewTerrainPackageCold(PackedByteArray &, uint32_t &, uint64_t &, Terrain3DSharedMemoryReservation &) override {
		if (change_during_composition) { preview.requestPackageRecomposeCold(); }
		return true;
	}
	bool loadPreviewPackage(const void *, uint64_t) override { ++published; return true; }
	bool stagePreviewPackageWarm(const void *, uint64_t) override { return false; }
	bool servicePreviewPackageWarm(uint64_t, uint32_t) override { return false; }
	bool rebuildPreviewPackageResourcesCold(const void *, uint64_t, bool, t3dVTSharedResourceClass_t) override { return false; }
	bool ensurePreviewSharedVt(t3dVTSharedResourceClass_t) override { return false; }
	void setPreviewReady(uint32_t, uint32_t) override {}
	void clearPreviewReady(uint32_t) override {}
	t3dRenderLifecycleStatus_t previewLifecycleStatus() const override {
		t3dRenderLifecycleStatus_t status{};
		if (live) { status.m_nSharedResourceClass = T3D_VT_SHARED_RESOURCES_PRODUCTION; }
		return status;
	}
};
struct ExportRegionFixture : Terrain3DRegionSource {
	Ref<Terrain3DRegion> region;
	ExportRegionFixture() {
		region.instantiate();
		const auto color = Image::create(8, 8, false, Image::FORMAT_RGBA8);
		color->fill(Color(0.2, 0.4, 0.1, 0.5)); region->setColorMap(color);
	}
	int getRegionCount() const override { return 1; }
	bool hasRegion(const Vector2i &location) const override { return location == Vector2i(); }
	TypedArray<Vector2i> getRegionLocations() const override {
		TypedArray<Vector2i> locations; locations.push_back(Vector2i()); return locations;
	}
	Ref<Terrain3DRegion> getRegion(const Vector2i &location) const override { return hasRegion(location) ? region : Ref<Terrain3DRegion>(); }
	bool ensureCpuRegionResident(const Vector2i &location) override { return hasRegion(location); }
};
} // namespace

void register_cooking_tests(TestRunner &runner) {
	const String asset_fixture = OS::get_singleton()->get_environment("TERRAIN3D_VT_ASSET_FIXTURE");
	if (!asset_fixture.is_empty()) {
		runner.run("cooking", "captured_assets_shipping_v3_merge_and_validate", [asset_fixture](TestContext &test) {
			const Ref<Terrain3DAssets> assets = ResourceLoader::get_singleton()->load(asset_fixture);
			T3D_REQUIRE(test, assets.is_valid());
			Terrain3DMemoryAccountOwner owner(256ull * 1024 * 1024);
			Terrain3DMemoryAccountScope scope(owner.get());
			Terrain3DMemoryReservation pressure;
			const uint64_t pressure_mb = OS::get_singleton()->get_environment("TERRAIN3D_VT_TEST_PRESSURE_MB").to_int();
			if (pressure_mb) { T3D_REQUIRE(test, pressure.resize(pressure_mb * 1024 * 1024)); }
			PackedByteArray package; uint32_t generation = 0, materials = 0; String error;
			const bool success = t3d_vt_godot_cook_runtime_assets_package(assets, package, generation, materials, &error);
			UtilityFunctions::print("SHIPPING_CAPTURED_ASSETS success=", success, " materials=", materials,
			        " output_bytes=", package.size(), " peak_bytes=", owner.get()->stats().peak, " error=", error);
			T3D_REQUIRE(test, success);
			t3dVTPackViewV3_t view{};
			T3D_REQUIRE(test, t3d_vt_pack_open_view_v3(package.ptr(), package.size(), view));
			T3D_CHECK(test, generation != 0u && materials == view.m_pHeader->m_nAllocationCount);
			T3D_CHECK(test, owner.get()->stats().peak <= owner.get()->stats().limit && owner.get()->stats().denied == 0);
			const String output_path = OS::get_singleton()->get_environment("TERRAIN3D_VT_TEST_PACKAGE_OUTPUT");
			if (!output_path.is_empty()) {
				const auto output = FileAccess::open(output_path, FileAccess::WRITE);
				T3D_REQUIRE(test, output.is_valid()); output->store_buffer(package);
			}
		});
	}
	runner.run("cooking", "shipping_v3_mixed_instance_channels_merge_and_validate", [](TestContext &test) {
		Ref<Terrain3DAssets> assets; assets.instantiate();
		for (int i = 0; i < 6; ++i) {
			Ref<Terrain3DMeshAsset> mesh; mesh.instantiate();
			Ref<Terrain3DRenderingMaterial> material; material.instantiate();
			material->setVtStableKey(1000 + i);
			const auto image = Image::create(i % 2 == 0 ? 256 : 512, i % 2 == 0 ? 256 : 512, false, Image::FORMAT_RGBA8);
			image->fill(Color(0.1 * (i + 1), 0.2, 0.3, i % 2 == 0 ? 0.3 : 1.0));
			material->setAlbedoTexture(ImageTexture::create_from_image(image));
			material->setAlphaScissor(i % 2 == 0);
			if (i >= 2) {
				const auto normal = Image::create(256, 256, false, Image::FORMAT_RGBA8);
				normal->fill(Color(0.5, 0.5, 1.0, 1.0));
				material->setNormalTexture(ImageTexture::create_from_image(normal));
			}
			if (i >= 4) {
				const auto surface = Image::create(256, 256, false, Image::FORMAT_RGBA8);
				surface->fill(Color(1.0, 0.4, 0.1, 1.0));
				material->setSurfaceTexture(ImageTexture::create_from_image(surface));
			}
			mesh->setRenderingMaterialOverride(material); assets->setMeshAsset(i, mesh);
		}
		Terrain3DMemoryAccountOwner owner(64ull * 1024 * 1024);
		Terrain3DMemoryAccountScope scope(owner.get());
		PackedByteArray package; uint32_t generation = 0, materials = 0; String error;
		const bool success = t3d_vt_godot_cook_runtime_assets_package(assets, package, generation, materials, &error);
		UtilityFunctions::print("SHIPPING_V3_FIXTURE success=", success, " materials=", materials, " bytes=", package.size(), " error=", error);
		T3D_REQUIRE(test, success);
		t3dVTPackViewV3_t view{};
		T3D_REQUIRE(test, t3d_vt_pack_open_view_v3(package.ptr(), package.size(), view));
		T3D_CHECK(test, materials == 6u && generation != 0u && view.m_pHeader->m_nAllocationCount == 6u);
		for (uint32_t i = 0; i < materials; ++i) {
			T3D_CHECK(test, (view.m_pAllocations[i].m_nFlags & T3D_VT_ALLOCATION_SHIPPING_IMMUTABLE) != 0u);
			T3D_CHECK(test, (view.m_pAllocations[i].m_nFlags & T3D_VT_ALLOCATION_EDITOR_DYNAMIC) == 0u);
		}
	});
	// Optional source manifests reproduce project-specific staging using raw
	// images, without opening the project or importing/writing its resources.
	const String input_list = OS::get_singleton()->get_environment("TERRAIN3D_VT_MATERIAL_SOURCE_LIST");
	if (!input_list.is_empty()) {
		runner.run("cooking", "captured_material_sources_stage_without_a_budget_dependency", [input_list](TestContext &test) {
			const auto paths = FileAccess::get_file_as_string(input_list).split("\n", false);
			T3D_REQUIRE(test, !paths.is_empty());
			Ref<Terrain3DAssets> assets; assets.instantiate();
			eastl::vector<uint64_t, Terrain3DEastlAllocator> keys;
			T3D_REQUIRE(test, t3dContainerReserveCold(keys, paths.size()));
			for (int64_t i = 0; i < paths.size(); ++i) {
				const auto fields = paths[i].split("\t", true);
				T3D_REQUIRE(test, fields.size() == 4);
				Ref<Terrain3DRenderingMaterial> material; material.instantiate();
				material->setVtStableKeyHex(fields[0]);
				material->setAlphaScissor(true);
				material->setSurfaceTextureLayout(fields[3].to_int());
				for (int channel = 1; channel <= 2; ++channel) {
					if (fields[channel].is_empty()) { continue; }
					Ref<Image> image;
					if (fields[channel].get_extension().to_lower() == "dds") {
						image.instantiate();
						T3D_REQUIRE(test, image->load_dds_from_buffer(FileAccess::get_file_as_bytes(fields[channel])) == OK);
					} else { image = Image::load_from_file(fields[channel]); }
					T3D_REQUIRE(test, image.is_valid() && !image->is_empty());
					if (image->is_compressed()) { T3D_REQUIRE(test, image->decompress() == OK); }
					const auto texture = ImageTexture::create_from_image(image);
					if (channel == 1) { material->setAlbedoTexture(texture); }
					else { material->setSurfaceTexture(texture); }
				}
				Ref<Terrain3DMeshAsset> mesh; mesh.instantiate();
				mesh->setRenderingMaterialOverride(material); assets->setMeshAsset(i, mesh);
				keys.push_back(material->getVtStableKey());
			}
			Terrain3DMemoryAccountOwner owner(512ull * 1024 * 1024);
			Terrain3DMemoryAccountScope scope(owner.get());
			t3dVTGodotAssetMaterials_t materials;
			String error;
			T3D_REQUIRE(test, t3d_vt_godot_collect_asset_materials(assets, materials, &error));
			T3D_REQUIRE(test, !materials.m_stableKeys.empty());
			t3dVTEditorCookCacheV3_t cache;
			PackedByteArray merged; Terrain3DSharedMemoryReservation memory;
			uint64_t fingerprint = 0; uint32_t generation = 0;
			t3dVTEditorCookTelemetryV3_t telemetry{};
			const bool success = t3d_vt_godot_cook_editor_package_incremental_v3(assets, materials.m_stableKeys.data(),
			        materials.m_records.data(), materials.m_stableKeys.size(),
			        cache, merged, fingerprint, generation, telemetry, &error, &memory);
			UtilityFunctions::print("VT_CAPTURED_SOURCES materials=", materials.m_stableKeys.size(), " output_bytes=", merged.size(),
			        " peak_bytes=", owner.get()->stats().peak, " error=", error);
			T3D_REQUIRE(test, success);
			t3dVTPackViewV3_t view{};
			T3D_CHECK(test, t3d_vt_pack_open_view_v3(merged.ptr(), merged.size(), view));
			T3D_CHECK(test, view.m_pHeader->m_nAllocationCount == uint32_t(materials.m_stableKeys.size()));
			T3D_CHECK(test, owner.get()->stats().peak <= owner.get()->stats().limit && owner.get()->stats().denied == 0);
		});
	}
	// Validate public cooking settings before doing expensive work.
	runner.run("cooking", "default_settings_obey_the_frozen_tile_contract", [](TestContext &test) {
		t3dVTCookerSettings_t settings{};
		t3d_vt_cooker_default_settings(settings);
		T3D_CHECK(test, t3d_vt_cooker_validate_settings(settings));
		T3D_CHECK(test, settings.m_nPageInterior == 128 && settings.m_nPageGutter == 4);
	});
	runner.run("cooking", "settings_reject_incompatible_geometry_and_unbounded_quality", [](TestContext &test) {
		t3dVTCookerSettings_t original{};
		t3d_vt_cooker_default_settings(original);
		for (int field = 0; field < 7; ++field) {
			auto settings = original;
			switch (field) {
			case 0:
				settings.m_nPageInterior++;
				break;
			case 1:
				settings.m_nPageGutter++;
				break;
			case 2:
				settings.m_nClusterTargetPages = 0;
				break;
			case 3:
				settings.m_nClusterTargetBytes = 0;
				break;
			case 4:
				settings.m_nOuterCompression = UINT32_MAX;
				break;
			case 5:
				settings.m_nMipQuality = 4;
				break;
			case 6:
				settings.m_nCompressionQuality = 4;
				break;
			}
			T3D_CHECK(test, !t3d_vt_cooker_validate_settings(settings));
		}
	});
	// Mip layout and pixel-level output, including odd and rectangular inputs.
	runner.run("cooking", "mip_layout_for_a_rectangular_image_has_exact_offsets", [](TestContext &test) {
		t3dVTMipChainLayout_t layout{};
		T3D_REQUIRE(test, t3d_vt_page_builder_layout_mips(8, 4, 1, layout));
		T3D_CHECK(test, layout.m_nMipCount == 4 && layout.m_nTotalBytes == 43);
		T3D_CHECK(test, layout.m_Levels[0].m_nOffset == 0 && layout.m_Levels[1].m_nOffset == 32);
		T3D_CHECK(test, layout.m_Levels[2].m_nOffset == 40 && layout.m_Levels[3].m_nOffset == 42);
	});
	runner.run("cooking", "mip_layout_rejects_zero_dimensions_and_invalid_components", [](TestContext &test) {
		t3dVTMipChainLayout_t layout{};
		T3D_CHECK(test, !t3d_vt_page_builder_layout_mips(0, 4, 1, layout));
		T3D_CHECK(test, !t3d_vt_page_builder_layout_mips(4, 0, 1, layout));
		T3D_CHECK(test, !t3d_vt_page_builder_layout_mips(4, 4, 0, layout));
		T3D_CHECK(test, !t3d_vt_page_builder_layout_mips(4, 4, 5, layout));
	});
	runner.run("cooking", "odd_mip_dimensions_eventually_reach_one_texel", [](TestContext &test) {
		t3dVTMipChainLayout_t layout{};
		T3D_REQUIRE(test, t3d_vt_page_builder_layout_mips(7, 3, 4, layout));
		T3D_CHECK(test,
		          layout.m_nMipCount == 3 && layout.m_Levels[1].m_nWidth == 3 && layout.m_Levels[1].m_nHeight == 1);
		T3D_CHECK(test, layout.m_Levels[2].m_nWidth == 1 && layout.m_Levels[2].m_nHeight == 1);
	});
	runner.run("cooking", "height_mip_averages_known_pixel_values", [](TestContext &test) {
		t3dVTMipChainLayout_t layout{};
		T3D_REQUIRE(test, t3d_vt_page_builder_layout_mips(2, 2, 1, layout));
		uint8_t pixels[5] = {0, 64, 128, 192, 0};
		T3D_REQUIRE(test, t3d_vt_page_builder_build_logical_channel_mips_u8(T3D_VT_LOGICAL_CHANNEL_HEIGHT, false, 0,
		                                                                    pixels, 5, layout));
		T3D_CHECK(test, pixels[4] == 96);
	});
	runner.run("cooking", "albedo_mips_filter_in_linear_light", [](TestContext &test) {
		t3dVTMipChainLayout_t layout{};
		T3D_REQUIRE(test, t3d_vt_page_builder_layout_mips(2, 2, 4, layout));
		uint8_t pixels[20] = {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255};
		T3D_REQUIRE(test, t3d_vt_page_builder_build_logical_channel_mips_u8(T3D_VT_LOGICAL_CHANNEL_ALBEDO, false, 128,
		                                                                    pixels, sizeof(pixels), layout));
		T3D_CHECK(test, pixels[16] >= 187 && pixels[16] <= 189 && pixels[17] == pixels[16] && pixels[19] == 255);
	});
	runner.run("cooking", "mip_build_rejects_short_storage_without_writing_guards", [](TestContext &test) {
		t3dVTMipChainLayout_t layout{};
		T3D_REQUIRE(test, t3d_vt_page_builder_layout_mips(2, 2, 1, layout));
		uint8_t pixels[6];
		memset(pixels, 0xa5, sizeof(pixels));
		T3D_CHECK(test, !t3d_vt_page_builder_build_mips_u8(pixels + 1, 4, layout));
		for (const uint8_t value : pixels) {
			T3D_CHECK(test, value == 0xa5);
		}
	});
	runner.run("cooking", "resampling_preserves_constant_pixels_and_padded_source_rows", [](TestContext &test) {
		const uint8_t pixels[8] = {42, 42, 0xa5, 0xa5, 42, 42, 0xa5, 0xa5};
		t3dVTCookImageView_t source{pixels, 2, 2, 1, 4};
		uint8_t output[15]{};
		T3D_REQUIRE(test, t3d_vt_page_builder_resample_u8(source, 5, 3, output, sizeof(output)));
		for (const uint8_t value : output) {
			T3D_CHECK(test, value == 42);
		}
	});
	runner.run("cooking", "short_source_stride_is_rejected", [](TestContext &test) {
		uint8_t pixels[16]{}, output[16]{};
		t3dVTCookImageView_t source{pixels, 2, 2, 4, 7};
		T3D_CHECK(test, !t3d_vt_page_builder_resample_u8(source, 2, 2, output, sizeof(output)));
	});
	runner.run("cooking", "roughness_channel_normalization_keeps_ao_and_metallic_neutral", [](TestContext &test) {
		const uint8_t source_pixel[4] = {11, 22, 33, 44};
		uint8_t output[4]{};
		t3dVTCookImageView_t source{source_pixel, 1, 1, 4, 4};
		T3D_REQUIRE(test, t3d_vt_page_builder_resample_logical_channel_u8(T3D_VT_LOGICAL_CHANNEL_ORM,
		                                                                  T3D_VT_SURFACE_SOURCE_ROUGHNESS_B, false, 0,
		                                                                  source, 1, 1, output, 4));
		T3D_CHECK(test, output[0] == 255 && output[1] == 33 && output[2] == 255 && output[3] == 255);
	});
	for (uint32_t channel = 0; channel < T3D_VT_LOGICAL_CHANNEL_COUNT; ++channel) {
		const CharString name = (String("neutral_pixels_for_logical_channel_") + String::num_int64(channel)).utf8();
		runner.run("cooking", name.get_data(), [channel](TestContext &test) {
			uint8_t output[17];
			memset(output, 0xa5, sizeof(output));
			const auto logical = static_cast<t3dVTLogicalChannel_t>(channel);
			const uint32_t components = t3d_vt_page_builder_logical_channel_components(logical);
			const uint8_t expected[T3D_VT_LOGICAL_CHANNEL_COUNT][4] = {
			    {255, 255, 255, 255}, {128, 128, 255, 255}, {128, 0, 0, 0}, {128, 128, 0, 0}, {255, 255, 0, 255}};
			T3D_REQUIRE(test, t3d_vt_page_builder_fill_logical_neutral(logical, 2, 2, output, components * 4));
			T3D_CHECK(test, output[components * 4] == 0xa5);
			for (uint32_t index = 0; index < components * 4; ++index) {
				T3D_CHECK(test, output[index] == expected[channel][index % components]);
			}
		});
	}
	// Tile gutters: compare to independent expected source coordinates.
	for (const auto wrap : {T3D_VT_COOK_WRAP_CLAMP, T3D_VT_COOK_WRAP_REPEAT}) {
		runner.run("cooking",
		           wrap == T3D_VT_COOK_WRAP_CLAMP ? "tile_gutters_clamp_to_border_pixels"
		                                          : "tile_gutters_repeat_across_source_edges",
		           [wrap](TestContext &test) {
			           const uint8_t source[4] = {10, 20, 30, 40};
			           PackedByteArray output;
			           output.resize(T3D_VT_PHYSICAL_TILE * T3D_VT_PHYSICAL_TILE);
			           T3D_REQUIRE(test, t3d_vt_page_builder_build_tile_u8(source, 2, 2, 1, 0, 0, wrap, output.ptrw(),
			                                                               output.size()));
			           for (int y = 0; y < T3D_VT_PHYSICAL_TILE; ++y) {
				           for (int x = 0; x < T3D_VT_PHYSICAL_TILE; ++x) {
					           const int sx = wrap == T3D_VT_COOK_WRAP_CLAMP ? (x < T3D_VT_PAGE_GUTTER + 1 ? 0 : 1)
					                                                         : ((x - T3D_VT_PAGE_GUTTER) % 2 + 2) % 2;
					           const int sy = wrap == T3D_VT_COOK_WRAP_CLAMP ? (y < T3D_VT_PAGE_GUTTER + 1 ? 0 : 1)
					                                                         : ((y - T3D_VT_PAGE_GUTTER) % 2 + 2) % 2;
					           T3D_CHECK(test, output[y * T3D_VT_PHYSICAL_TILE + x] == source[sy * 2 + sx]);
				           }
			           }
		           });
	}
	// Real encoders plus boundary validation before entering their unchecked core.
	for (const auto channel : {T3D_VT_LOGICAL_CHANNEL_ALBEDO, T3D_VT_LOGICAL_CHANNEL_PBR, T3D_VT_LOGICAL_CHANNEL_NORMAL,
	                           T3D_VT_LOGICAL_CHANNEL_HEIGHT}) {
		const CharString name = (String("reference_tile_encoding_for_channel_") + String::num_int64(channel)).utf8();
		runner.run("cooking", name.get_data(), [channel](TestContext &test) {
			t3dVTEncoderInterface_t encoder{};
			t3d_vt_reference_encoder_make_interface(encoder);
			const uint32_t components = t3d_vt_page_builder_logical_channel_components(channel);
			const auto family = t3d_vt_logical_channel_physical_family(channel);
			const uint32_t expected = t3d_vt_pack_expected_physical_tile_bytes_v3(family);
			PackedByteArray source, output;
			source.resize(T3D_VT_PHYSICAL_TILE * T3D_VT_PHYSICAL_TILE * components);
			source.fill(128);
			output.resize(expected + 1);
			output.fill(0xa5);
			t3dVTEncodeTileRequest_t request{};
			request.m_nChannel = channel;
			request.m_nFormat = family;
			request.m_nWidth = T3D_VT_PHYSICAL_TILE;
			request.m_nHeight = T3D_VT_PHYSICAL_TILE;
			request.m_nComponents = components;
			request.m_nSourceBytes = source.size();
			request.m_pSourcePixels = source.ptr();
			request.m_nContractVersion = T3D_VT_FILE_VERSION_V3;
			request.m_pDestination = output.ptrw();
			request.m_nDestinationCapacity = expected;
			uint32_t written = 999;
			T3D_CHECK(test, t3d_vt_cooker_encode_tile_v3(encoder, request, written) && written == expected &&
			                    output[expected] == 0xa5);
			request.m_nDestinationCapacity = expected - 1;
			T3D_CHECK(test, !t3d_vt_cooker_encode_tile_v3(encoder, request, written) && written == 0);
		});
	}
	runner.run("cooking", "sparse_cook_builds_inspectable_allocations_pages_and_tails", [](TestContext &test) {
		CookingSource source;
		CookedPackage cooked;
		T3D_REQUIRE(test, cooked.cook(source));
		T3D_CHECK(test, cooked.package.m_nAllocationCount == 1 && cooked.package.m_nPageCount > 0 &&
		                    cooked.package.m_nTailCount > 0);
		T3D_CHECK(test, cooked.package.m_pAllocationKeys[0].m_nStableKey == source.source.m_nStableKey);
		t3dVTPackSourceV3_t pack_source{};
		T3D_CHECK(test, t3d_vt_cooker_build_pack_source_v3(cooked.package, pack_source) &&
		                    t3d_vt_pack_validate_source_v3(pack_source));
	});
	runner.run("cooking", "sparse_cook_releases_all_owned_blocks", [](TestContext &test) {
		CookingSource source;
		CookedPackage cooked;
		T3D_REQUIRE(test, cooked.cook(source));
		T3D_CHECK(test, cooked.allocator.live_blocks > 0);
		t3d_vt_cooker_release_sparse_package_v3(cooked.package);
		T3D_CHECK(test, cooked.allocator.live_blocks == 0 && cooked.package.m_pPayloadData == nullptr);
		t3d_vt_cooker_release_sparse_package_v3(cooked.package);
		T3D_CHECK(test, cooked.allocator.live_blocks == 0);
	});
	runner.run("cooking", "identical_tiles_share_payload_storage", [](TestContext &test) {
		CookingSource source;
		CookedPackage cooked;
		T3D_REQUIRE(test, cooked.cook(source));
		T3D_CHECK(test, cooked.package.m_Stats.m_nRawDedupHits > 0);
		T3D_CHECK(test, cooked.package.m_nPayloadCount < cooked.package.m_nPageCount + cooked.package.m_nTailCount);
	});
	runner.run("cooking", "sparse_cook_allocation_failures_release_partial_results", [](TestContext &test) {
		CookingSource source;
		for (const size_t failure : {size_t(0), size_t(1), size_t(3), size_t(8)}) {
			CookedPackage cooked;
			cooked.allocator.fail_after = failure;
			T3D_CHECK(test, !cooked.cook(source));
			t3d_vt_cooker_release_sparse_package_v3(cooked.package);
			T3D_CHECK(test, cooked.allocator.live_blocks == 0);
		}
	});
	runner.run("cooking", "failed_encoder_cannot_publish_a_partial_package", [](TestContext &test) {
		CookingSource source;
		CookedPackage cooked;
		T3D_CHECK(test, !cooked.cook(source, reject_encoding));
		t3d_vt_cooker_release_sparse_package_v3(cooked.package);
		T3D_CHECK(test, cooked.allocator.live_blocks == 0 && cooked.package.m_nAllocationCount == 0);
	});
	runner.run("cooking", "encoder_byte_count_must_match_the_format_contract", [](TestContext &test) {
		CookingSource source;
		CookedPackage cooked;
		T3D_CHECK(test, !cooked.cook(source, report_short_encoding));
		t3d_vt_cooker_release_sparse_package_v3(cooked.package);
		T3D_CHECK(test, cooked.allocator.live_blocks == 0);
	});
	runner.run("cooking", "identical_sources_produce_identical_fragment_bytes", [](TestContext &test) {
		CookingSource source;
		PackedByteArray first, second;
		T3D_REQUIRE(test, source.fragment(first) && source.fragment(second));
		T3D_CHECK(test, first == second);
	});
	runner.run("cooking", "working_memory_limit_rejects_before_publishing_output", [](TestContext &test) {
		CookingSource source;
		PackedByteArray bytes;
		T3D_CHECK(test, !source.fragment(bytes, 1));
		T3D_CHECK(test, bytes.is_empty());
	});
	runner.run("cooking", "package_merge_keeps_both_stable_allocation_keys", [](TestContext &test) {
		CookingSource first(101), second(202, 90);
		PackedByteArray inputs[2], merged;
		T3D_REQUIRE(test, first.fragment(inputs[0]) && second.fragment(inputs[1]));
		T3D_REQUIRE(test, t3d_vt_godot_merge_packages_v3(inputs, 2, 11, merged));
		t3dVTPackViewV3_t view{};
		T3D_REQUIRE(test, t3d_vt_pack_open_view_v3(merged.ptr(), merged.size(), view));
		T3D_CHECK(test, view.m_pHeader->m_nAllocationCount == 2);
		T3D_CHECK(test, view.m_pAllocationKeys[0].m_nStableKey == 101 && view.m_pAllocationKeys[1].m_nStableKey == 202);
	});
	runner.run("cooking", "package_merge_rejects_malformed_input_and_tight_budgets", [](TestContext &test) {
		CookingSource source;
		PackedByteArray valid, merged, invalid;
		T3D_REQUIRE(test, source.fragment(valid));
		T3D_CHECK(test, !t3d_vt_godot_merge_packages_v3(&valid, 1, 1, merged, 1));
		invalid.resize(16);
		invalid.fill(0);
		T3D_CHECK(test, !t3d_vt_godot_merge_packages_v3(&invalid, 1, 1, merged));
		T3D_CHECK(test, merged.is_empty());
	});
	runner.run("cooking", "deduplicated_merge_charges_actual_buffers_and_transfers_output_ownership", [](TestContext &test) {
		PackedByteArray inputs[8], merged;
		for (uint32_t i = 0; i < 8; ++i) {
			CookingSource source(101 + i);
			T3D_REQUIRE(test, source.fragment(inputs[i]));
		}
		Terrain3DMemoryAccountOwner owner(1024 * 1024);
		Terrain3DMemoryAccountScope scope(owner.get());
		Terrain3DSharedMemoryReservation output_memory;
		t3dVTMergeStatistics_t statistics{};
		T3D_REQUIRE(test, t3d_vt_godot_merge_packages_v3(inputs, 8, 11, merged,
		    1024 * 1024, &statistics, &output_memory));
		t3dVTPackViewV3_t view{};
		T3D_REQUIRE(test, t3d_vt_pack_open_view_v3(merged.ptr(), merged.size(), view));
		T3D_CHECK(test, view.m_pHeader->m_nAllocationCount == 8);
		const auto stats = owner.get()->stats();
		uint64_t input_bytes = 0;
		for (const auto &input : inputs) { input_bytes += input.size(); }
		T3D_CHECK(test, 3 * input_bytes > stats.limit); // The former preflight rejects this merge.
		T3D_CHECK(test, statistics.m_nWorkingBytes <= stats.limit && !statistics.m_bWorkingLimitExceeded);
		T3D_CHECK(test, stats.peak <= stats.limit && stats.denied == 0);
		T3D_CHECK(test, stats.committed == uint64_t(merged.size()) + 64);
		auto published_memory = output_memory;
		output_memory.reset();
		T3D_CHECK(test, owner.get()->stats().committed == uint64_t(merged.size()) + 64);
		merged.clear(); published_memory.reset();
		T3D_CHECK(test, owner.get()->stats().committed == 0);
	});
	runner.run("cooking", "true_working_limit_rejection_preserves_output_and_releases_scratch", [](TestContext &test) {
		CookingSource source;
		PackedByteArray input, output;
		T3D_REQUIRE(test, source.fragment(input));
		Terrain3DMemoryAccountOwner owner(8ull * 1024 * 1024);
		Terrain3DMemoryAccountScope scope(owner.get());
		Terrain3DSharedMemoryReservation output_memory;
		t3dVTMergeStatistics_t baseline{};
		T3D_REQUIRE(test, t3d_vt_godot_merge_packages_v3(&input, 1, 11, output,
		    1024 * 1024, &baseline, &output_memory));
		auto published = output;
		auto published_memory = output_memory;
		const uint64_t committed = owner.get()->stats().committed;
		t3dVTMergeStatistics_t denied{};
		T3D_CHECK(test, !t3d_vt_godot_merge_packages_v3(&input, 1, 12, output,
		    baseline.m_nWorkingBytes - 1, &denied, &output_memory));
		T3D_CHECK(test, denied.m_bWorkingLimitExceeded && denied.m_nWorkingBytes > baseline.m_nWorkingBytes - 1);
		T3D_CHECK(test, output == published && owner.get()->stats().committed == committed);
		T3D_REQUIRE(test, t3d_vt_godot_merge_packages_v3(&input, 1, 11, output,
		    baseline.m_nWorkingBytes, nullptr, &output_memory));
		T3D_CHECK(test, output == published && owner.get()->stats().committed == 2 * committed);
		published.clear(); published_memory.reset();
		T3D_CHECK(test, owner.get()->stats().committed == committed);
	});
	runner.run("cooking", "borrowed_payloads_remain_valid_when_output_aliases_the_input", [](TestContext &test) {
		CookingSource source;
		PackedByteArray input, expected;
		T3D_REQUIRE(test, source.fragment(input));
		T3D_REQUIRE(test, t3d_vt_godot_merge_packages_v3(&input, 1, 11, expected, 1024 * 1024));
		T3D_REQUIRE(test, t3d_vt_godot_merge_packages_v3(&input, 1, 11, input, 1024 * 1024));
		T3D_CHECK(test, input == expected);
		t3dVTPackViewV3_t view{};
		T3D_CHECK(test, t3d_vt_pack_open_view_v3(input.ptr(), input.size(), view));
	});
	runner.run("cooking", "package_merge_fits_output_and_metadata_without_a_second_payload_buffer", [](TestContext &test) {
		CookingSource source;
		PackedByteArray input, expected, merged;
		T3D_REQUIRE(test, source.fragment(input));
		T3D_REQUIRE(test, t3d_vt_godot_merge_packages_v3(&input, 1, 11, expected));
		t3dVTPackViewV3_t view{};
		T3D_REQUIRE(test, t3d_vt_pack_open_view_v3(expected.ptr(), expected.size(), view));
		T3D_REQUIRE(test, view.m_pHeader->m_nPayloadDataBytes > 16 * 1024);
		const uint64_t budget = uint64_t(expected.size()) + 64 + 16 * 1024;
		Terrain3DMemoryAccountOwner owner(budget);
		Terrain3DMemoryAccountScope scope(owner.get());
		Terrain3DSharedMemoryReservation memory;
		T3D_REQUIRE(test, t3d_vt_godot_merge_packages_v3(&input, 1, 11, merged, budget, nullptr, &memory));
		T3D_CHECK(test, merged == expected);
		T3D_CHECK(test, owner.get()->stats().denied == 0 && owner.get()->stats().peak <= budget);
	});
	runner.run("cooking", "v3_writer_preserves_payloads_already_in_the_destination", [](TestContext &test) {
		CookingSource source; CookedPackage cooked;
		T3D_REQUIRE(test, cooked.cook(source));
		t3dVTPackSourceV3_t pack{};
		T3D_REQUIRE(test, t3d_vt_cooker_build_pack_source_v3(cooked.package, pack));
		const uint64_t bytes = t3d_vt_pack_compute_bytes_v3(pack);
		PackedByteArray external, in_place;
		T3D_REQUIRE(test, bytes > 0 && external.resize(bytes) == OK && in_place.resize(bytes) == OK);
		uint64_t written = 0;
		T3D_REQUIRE(test, t3d_vt_pack_write_v3(pack, external.ptrw(), bytes, written) && written == bytes);
		const uint64_t metadata = t3d_vt_pack_compute_metadata_bytes_v3(pack);
		T3D_CHECK(test, metadata + pack.m_nPayloadDataBytes == bytes);
		memcpy(in_place.ptrw() + metadata, pack.m_pPayloadData, pack.m_nPayloadDataBytes);
		pack.m_pPayloadData = in_place.ptr() + metadata;
		T3D_REQUIRE(test, t3d_vt_pack_write_v3(pack, in_place.ptrw(), bytes, written) && written == bytes);
		T3D_CHECK(test, external == in_place);
		t3dVTPackViewV3_t view{};
		T3D_CHECK(test, t3d_vt_pack_open_view_v3(in_place.ptr(), in_place.size(), view));
	});
	runner.run("cooking", "runtime_region_export_charges_actual_buffers_under_live_budget_pressure", [](TestContext &test) {
		ExportRegionFixture source;
		Terrain3DMemoryAccountOwner owner(40ull * 1024 * 1024);
		Terrain3DMemoryAccountScope scope(owner.get());
		Terrain3DMemoryReservation pressure;
		T3D_REQUIRE(test, pressure.resize(32ull * 1024 * 1024));
		t3dMegaVTAuthoringInputs_t inputs{source, String(), 64ull * 1024 * 1024};
		PackedByteArray package; uint32_t generation = 0; String error;
		T3D_REQUIRE(test, Terrain3DIncrementalMegaVTCooker::appendRuntimeRegionsCold(inputs, package, generation, error));
		t3dVTPackViewV3_t view{};
		T3D_REQUIRE(test, t3d_vt_pack_open_view_v3(package.ptr(), package.size(), view));
		T3D_CHECK(test, generation != 0u && view.m_pHeader->m_nAllocationCount == 1u);
		T3D_CHECK(test, owner.get()->stats().denied == 0 && owner.get()->stats().committed == pressure.bytes());
	});
	for (const bool during_cook : {false, true}) {
		runner.run("cooking", during_cook ? "forced_preview_source_change_during_cook_fails_explicitly"
		                                 : "forced_preview_source_change_before_publication_fails_explicitly", [during_cook](TestContext &test) {
			Ref<Terrain3DAssets> assets; assets.instantiate();
			Ref<Terrain3DMeshAsset> mesh; mesh.instantiate();
			Ref<Terrain3DRenderingMaterial> material; material.instantiate(); material->setVtStableKey(101);
			const auto image = Image::create(256, 256, false, Image::FORMAT_RGBA8); image->fill(Color(0.8, 0.2, 0.1, 1));
			material->setAlbedoTexture(ImageTexture::create_from_image(image));
			mesh->setRenderingMaterialOverride(material); assets->setMeshAsset(0, mesh);
			PreviewCookFixture fixture; String error;
			T3D_REQUIRE(test, fixture.preview.preparePreviewAuthoringPackageCold(fixture.services(), assets, &error, true));
			const auto previous = fixture.preview.getDiagnosticsView().m_previewVtPublishedPackageBytes;
			fixture.live = true; fixture.change_during_composition = during_cook;
			const bool staged = fixture.preview.preparePreviewAuthoringPackageCold(fixture.services(), assets, &error, true, true, 1);
			if (during_cook) { T3D_CHECK(test, !staged && !error.is_empty()); }
			else {
				T3D_REQUIRE(test, staged);
				fixture.preview.requestPackageRecomposeCold();
				fixture.preview.servicePreviewVtRepublishOnRenderThread(fixture.services());
				const auto status = fixture.preview.getForceRebakeStatusCold();
				T3D_CHECK(test, !(bool)status["pending"] && !String(status["error"]).is_empty());
				T3D_CHECK(test, uint64_t(status["published_serial"]) == 0u);
			}
			T3D_CHECK(test, fixture.preview.getDiagnosticsView().m_previewVtPublishedPackageBytes == previous);
		});
	}
	runner.run("cooking", "denied_merge_releases_scratch_and_can_retry_the_same_inputs", [](TestContext &test) {
		CookingSource source;
		PackedByteArray input, merged;
		T3D_REQUIRE(test, source.fragment(input));
		Terrain3DMemoryAccountOwner owner(1024 * 1024);
		Terrain3DMemoryAccountScope scope(owner.get());
		Terrain3DMemoryReservation pressure;
		T3D_REQUIRE(test, pressure.resize(1024 * 1024 - 2048));
		Terrain3DSharedMemoryReservation output_memory;
		T3D_CHECK(test, !t3d_vt_godot_merge_packages_v3(&input, 1, 11, merged,
		    T3D_VT_COOK_WORKING_BYTE_LIMIT, nullptr, &output_memory));
		T3D_CHECK(test, merged.is_empty() && owner.get()->stats().denied > 0);
		T3D_CHECK(test, owner.get()->stats().committed == pressure.bytes());
		pressure.reset();
		T3D_REQUIRE(test, t3d_vt_godot_merge_packages_v3(&input, 1, 11, merged,
		    T3D_VT_COOK_WORKING_BYTE_LIMIT, nullptr, &output_memory));
		T3D_CHECK(test, !merged.is_empty() && owner.get()->stats().peak <= owner.get()->stats().limit);
	});
	runner.run("cooking", "failed_warm_preview_is_quiet_until_explicit_retry_or_changed_content", [](TestContext &test) {
		Ref<Terrain3DAssets> assets; assets.instantiate();
		Ref<Terrain3DMeshAsset> mesh; mesh.instantiate();
		Ref<Terrain3DRenderingMaterial> material; material.instantiate();
		material->setVtStableKey(101);
		const auto image = Image::create(256, 256, false, Image::FORMAT_RGBA8);
		image->fill(Color(0.8, 0.2, 0.1, 1));
		material->setAlbedoTexture(ImageTexture::create_from_image(image));
		mesh->setRenderingMaterialOverride(material); assets->setMeshAsset(0, mesh);
		PreviewCookFixture fixture;
		String error;
		T3D_REQUIRE(test, fixture.preview.preparePreviewAuthoringPackageCold(fixture.services(), assets, &error, true));
		const auto previous = fixture.preview.getDiagnosticsView().m_previewVtPublishedPackageBytes;
		fixture.live = true;
		T3D_REQUIRE(test, fixture.preview.preparePreviewAuthoringPackageCold(fixture.services(), assets, &error, true, true, 1));
		fixture.preview.servicePreviewVtRepublishOnRenderThread(fixture.services());
		T3D_CHECK(test, fixture.preview.getDiagnosticsView().m_nPreviewVtRepublishFailureCount == 1);
		const auto failed_status = fixture.preview.getForceRebakeStatusCold();
		T3D_CHECK(test, !String(failed_status["error"]).is_empty() && !(bool)failed_status["pending"]);
		for (int i = 0; i < 5; ++i) {
			error = String();
			T3D_CHECK(test, fixture.preview.preparePreviewAuthoringPackageCold(fixture.services(), assets, &error));
			T3D_CHECK(test, error.is_empty());
			fixture.preview.servicePreviewVtRepublishOnRenderThread(fixture.services());
		}
		fixture.preview.requestPackageRecomposeCold();
		T3D_REQUIRE(test, fixture.preview.preparePreviewAuthoringPackageCold(fixture.services(), assets, &error, true));
		T3D_CHECK(test, !fixture.preview.getDiagnosticsView().m_bPreviewVtRepublishPending);
		T3D_CHECK(test, fixture.preview.getDiagnosticsView().m_nPreviewVtRepublishFailureCount == 1);
		T3D_CHECK(test, fixture.preview.getDiagnosticsView().m_previewVtPublishedPackageBytes == previous);
		// A new force action clears quarantine even for deterministic same bytes.
		fixture.preview.requestPackageRecomposeCold();
		T3D_REQUIRE(test, fixture.preview.preparePreviewAuthoringPackageCold(fixture.services(), assets, &error, true, true, 2));
		fixture.preview.servicePreviewVtRepublishOnRenderThread(fixture.services());
		T3D_CHECK(test, fixture.preview.getDiagnosticsView().m_nPreviewVtRepublishFailureCount == 2);
		const auto changed_image = Image::create(256, 256, false, Image::FORMAT_RGBA8);
		changed_image->fill(Color(0.1, 0.7, 0.2, 1));
		material->setAlbedoTexture(ImageTexture::create_from_image(changed_image));
		fixture.preview.requestPackageRecomposeCold();
		T3D_REQUIRE(test, fixture.preview.preparePreviewAuthoringPackageCold(fixture.services(), assets, &error, true));
		fixture.preview.servicePreviewVtRepublishOnRenderThread(fixture.services());
		T3D_CHECK(test, fixture.preview.getDiagnosticsView().m_nPreviewVtRepublishFailureCount == 3);
	});
	runner.run("cooking", "preview_budget_denial_does_not_quarantine_unchanged_instance_materials", [](TestContext &test) {
		Ref<Terrain3DAssets> assets; assets.instantiate();
		Ref<Terrain3DMeshAsset> mesh; mesh.instantiate();
		Ref<Terrain3DRenderingMaterial> material; material.instantiate();
		material->setVtStableKey(101);
		const auto image = Image::create(256, 256, false, Image::FORMAT_RGBA8);
		image->fill(Color(0.8, 0.2, 0.1, 1));
		material->setAlbedoTexture(ImageTexture::create_from_image(image));
		mesh->setRenderingMaterialOverride(material);
		assets->setMeshAsset(0, mesh);
		const uint64_t source_generation = assets->getInstanceVtMaterialSourceGeneration();
		Terrain3DMemoryAccountOwner owner(32ull * 1024 * 1024);
		Terrain3DMemoryAccountScope scope(owner.get());
		PreviewCookFixture fixture;
		Terrain3DMemoryReservation pressure;
		T3D_REQUIRE(test, pressure.resize(owner.get()->stats().limit - owner.get()->stats().committed));
		String error;
		T3D_CHECK(test, !fixture.preview.preparePreviewAuthoringPackageCold(fixture.services(), assets, &error, true));
		T3D_CHECK(test, owner.get()->stats().denied > 0 && fixture.published == 0);
		T3D_CHECK(test, fixture.preview.getDiagnosticsView().m_nPreviewVtFailedMaterialSourceGeneration == 0);
		pressure.reset();
		T3D_REQUIRE(test, fixture.preview.preparePreviewAuthoringPackageCold(fixture.services(), assets, &error, true));
		T3D_CHECK(test, assets->getInstanceVtMaterialSourceGeneration() == source_generation);
		T3D_CHECK(test, fixture.published == 1 && fixture.preview.getDiagnosticsView().m_bPreviewVtPackageValid);
		const auto published_bytes = fixture.preview.getDiagnosticsView().m_previewVtPublishedPackageBytes;
		fixture.preview.requestPackageRecomposeCold();
		T3D_REQUIRE(test, pressure.resize(owner.get()->stats().limit - owner.get()->stats().committed));
		T3D_CHECK(test, !fixture.preview.preparePreviewAuthoringPackageCold(fixture.services(), assets, &error, true));
		T3D_CHECK(test, fixture.published == 1 && fixture.preview.getDiagnosticsView().m_previewVtPublishedPackageBytes == published_bytes);
		pressure.reset();
		T3D_REQUIRE(test, fixture.preview.preparePreviewAuthoringPackageCold(fixture.services(), assets, &error, true));
		T3D_CHECK(test, fixture.preview.getDiagnosticsView().m_previewVtPublishedPackageBytes == published_bytes);
	});
	runner.run("cooking", "megavt_startup_cache_restores_under_a_live_owner_budget", [](TestContext &test) {
		T3D_CHECK(test, t3dRunMegaVTCacheTests());
	});
	runner.run("cooking", "editor_staging_releases_disposable_pyramids_and_reuses_encoded_fragments", [](TestContext &test) {
		Ref<Terrain3DAssets> assets; assets.instantiate();
		Ref<Terrain3DMeshAsset> mesh; mesh.instantiate();
		Ref<Terrain3DRenderingMaterial> material; material.instantiate();
		material->setVtStableKey(101);
		const auto image = Image::create(256, 256, false, Image::FORMAT_RGBA8);
		image->fill(Color(0.8, 0.2, 0.1, 1));
		material->setAlbedoTexture(ImageTexture::create_from_image(image));
		mesh->setRenderingMaterialOverride(material); assets->setMeshAsset(0, mesh);
		Terrain3DMemoryAccountOwner owner(32ull * 1024 * 1024);
		Terrain3DMemoryAccountScope scope(owner.get());
		t3dVTEditorCookCacheV3_t cache;
		PackedByteArray package; Terrain3DSharedMemoryReservation package_memory;
		uint64_t key = 101, fingerprint = 0; uint32_t generation = 0;
		t3dVTEditorCookTelemetryV3_t telemetry{}; String error;
		T3D_REQUIRE(test, t3d_vt_godot_cook_editor_package_incremental_v3(assets, &key, nullptr, 1,
		        cache, package, fingerprint, generation, telemetry, &error, &package_memory));
		T3D_CHECK(test, cache.m_ChannelEntries.empty() && cache.m_AllocationEntries.size() == 1);
		const auto expected = package;
		auto expected_memory = package_memory;
		t3dVTEditorAllocationCacheEntryV3_t removed{};
		removed.m_nStableKey = 999; removed.m_nKey = 999;
		T3D_REQUIRE(test, removed.m_memory.reserve(4096));
		removed.m_PackageBytes.resize(4096);
		T3D_REQUIRE(test, t3dContainerReserveCold(cache.m_AllocationEntries, 2));
		cache.m_AllocationEntries.push_back(removed);
		removed.m_PackageBytes.clear(); removed.m_memory.reset();
		// Reproduce a live cache left over from earlier material edits. Without
		// eviction, 2 KiB of headroom cannot admit source extraction or merging.
		t3dVTEditorChannelCacheEntryV3_t stale{};
		T3D_REQUIRE(test, stale.m_memory.reserve(16ull * 1024 * 1024));
		stale.m_Bytes.resize(16ull * 1024 * 1024);
		T3D_REQUIRE(test, t3dContainerReserveCold(cache.m_ChannelEntries, 1));
		cache.m_ChannelEntries.push_back(stale); stale.m_Bytes.clear(); stale.m_memory.reset();
		Terrain3DMemoryReservation pressure;
		T3D_REQUIRE(test, pressure.resize(owner.get()->stats().limit - owner.get()->stats().committed - 2048));
		T3D_REQUIRE(test, t3d_vt_godot_cook_editor_package_incremental_v3(assets, &key, nullptr, 1,
		        cache, package, fingerprint, generation, telemetry, &error, &package_memory));
		T3D_CHECK(test, package == expected && cache.m_ChannelEntries.empty());
		T3D_CHECK(test, cache.m_AllocationEntries.size() == 1 && cache.m_AllocationEntries[0].m_nStableKey == key);
		T3D_CHECK(test, telemetry.m_nAllocationCacheHits == 1 && telemetry.m_nDirtyAllocations == 0);
		T3D_CHECK(test, owner.get()->stats().peak <= owner.get()->stats().limit && owner.get()->stats().denied == 0);
	});
	runner.run("cooking", "editor_rejects_duplicate_allocation_keys_before_cooking", [](TestContext &test) {
		Ref<Terrain3DAssets> assets; assets.instantiate();
		t3dVTEditorCookCacheV3_t cache; PackedByteArray package;
		const uint64_t keys[2] = {101, 101}; uint64_t fingerprint = 0; uint32_t generation = 0;
		t3dVTEditorCookTelemetryV3_t telemetry{}; String error;
		T3D_CHECK(test, !t3d_vt_godot_cook_editor_package_incremental_v3(assets, keys, nullptr, 2,
		        cache, package, fingerprint, generation, telemetry, &error));
		T3D_CHECK(test, error.contains("duplicate stable keys") && cache.m_AllocationEntries.empty());
	});
	// Region cooking exercises actual image reduction and hierarchy contents.
	runner.run("cooking", "gpu_color_reduction_keeps_alpha_and_source_storage_intact", [](TestContext &test) {
		const auto source = Image::create(8, 8, false, Image::FORMAT_RGBA8);
		source->fill(Color(0.25, 0.5, 0.75, 0.25));
		const auto original = source->get_data();
		const auto reduced = t3dRegionImageCookGpuColor(source, Ref<Image>(), 2);
		T3D_REQUIRE(test, reduced.is_valid());
		T3D_CHECK(test, reduced->get_width() == 4 && reduced->get_height() == 4 && reduced->has_mipmaps());
		T3D_CHECK(test, near(reduced->get_pixel(1, 1).a, 0.25, 1.0 / 255));
		T3D_CHECK(test, source->get_data() == original && source->get_width() == 8 && !source->has_mipmaps());
	});
	runner.run("cooking", "minmax_hierarchy_root_encloses_every_height_sample", [](TestContext &test) {
		const auto height = t3dRegionCodecConvertHeightToR16(Ref<Image>(), T3D_REGION_SIZE);
		T3D_REQUIRE(test, height.is_valid());
		t3dRegionCodecWriteHeightPixel(*height, Vector2i(0, 0), -100);
		t3dRegionCodecWriteHeightPixel(*height, Vector2i(127, 127), 200);
		const auto blob = Terrain3DRuntimeBakeCodec::encodeMinMaxHierarchy(height);
		t3dMinMaxLevelView_t view{};
		T3D_REQUIRE(test, Terrain3DRuntimeBakeCodec::getMinMaxLevelView(blob, 1, view));
		const auto range = Terrain3DRuntimeBakeCodec::getMinMaxRange(view, 0);
		T3D_CHECK(test, near(range.x, -100, 0.02) && near(range.y, 200, 0.02));
	});
}
} // namespace terrain3d_tests
