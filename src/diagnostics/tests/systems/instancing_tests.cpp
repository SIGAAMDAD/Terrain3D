#include "diagnostics/tests/systems/instance_fixture.h"
#include "diagnostics/tests/systems/test_harness.h"
#include "instances/contracts/terrain_3d_instance_layout.h"
#include "memory/arena/terrain_3d_memory_zone.h"
#include <EASTL/utility.h>

namespace terrain3d_tests {
void register_instancing_tests(TestRunner &runner) {
	// Layout and lookup contracts.
	runner.run("instancing", "default_layout_covers_the_region", [](TestContext &test) {
		t3dInstanceLayout_t layout;
		T3D_CHECK(test, layout.isValid());
		T3D_CHECK(test, layout.m_nCellSize * layout.m_nCellsPerAxis == T3D_REGION_SIZE);
		T3D_CHECK(test, layout.maximumCellCount() == 16);
	});
	for (const int size : {8, 16, 32, 64, 128}) {
		const CharString name = (String("supported_cell_size_") + String::num_int64(size)).utf8();
		runner.run("instancing", name.get_data(), [size](TestContext &test) {
			t3dInstanceLayout_t layout;
			T3D_REQUIRE(test, t3dInstanceLayoutForCellSize(size, layout));
			T3D_CHECK(test, layout.isValid());
			T3D_CHECK(test, layout.contains(0, 0));
			T3D_CHECK(test, layout.contains(layout.m_nCellsPerAxis - 1, layout.m_nCellsPerAxis - 1));
			T3D_CHECK(test, !layout.contains(-1, 0));
			T3D_CHECK(test, !layout.contains(0, layout.m_nCellsPerAxis));
		});
	}
	runner.run("instancing", "unsupported_cell_sizes_preserve_the_existing_layout", [](TestContext &test) {
		for (const int size : {-1, 0, 7, 24, 256}) {
			t3dInstanceLayout_t layout;
			T3D_CHECK(test, !t3dInstanceLayoutForCellSize(size, layout));
			T3D_CHECK(test, layout.isValid() && layout.m_nCellSize == 32);
		}
	});
	runner.run("instancing", "empty_store_has_no_meshes_or_instances", [](TestContext &test) {
		Terrain3DInstanceStore store;
		T3D_CHECK(test, store.empty() && store.meshCount() == 0 && store.instanceCount() == 0);
		T3D_CHECK(test, store.findMesh(3) == nullptr && store.findCell(3, Vector2i()) == nullptr);
		T3D_CHECK(test, store.validateDeep());
	});
	runner.run("instancing", "invalid_mesh_ids_are_rejected", [](TestContext &test) {
		Terrain3DInstanceStore store;
		T3D_CHECK(test, store.tryGetOrCreateMesh(-1) == nullptr);
		T3D_CHECK(test, store.tryGetOrCreateMesh(T3D_MAX_MESH_ASSETS) == nullptr);
		T3D_CHECK(test, store.empty() && store.validateDeep());
	});
	runner.run("instancing", "dictionary_import_discards_out_of_region_cells", [](TestContext &test) {
		Terrain3DInstanceStore source, decoded;
		T3D_REQUIRE(test, populate_instance_store(source));
		const Dictionary exported = source.exportDictionary();
		const Dictionary valid_mesh = exported[3];
		Dictionary cells;
		cells[Vector2i()] = valid_mesh[Vector2i(1, 0)];
		for (const auto location : {Vector2i(-1, 0), Vector2i(4, 0), Vector2i(0, 4)}) {
			T3D_CHECK(test, !decoded.isValidCell(location));
			cells[location] = valid_mesh[Vector2i(1, 0)];
		}
		Dictionary input;
		input[3] = cells;
		decoded.importDictionary(input);
		for (const auto location : {Vector2i(-1, 0), Vector2i(4, 0), Vector2i(0, 4)}) {
			T3D_CHECK(test, decoded.findCell(3, location) == nullptr);
		}
		T3D_CHECK(test, decoded.instanceCount() == 3 && decoded.validateDeep());
	});
	runner.run("instancing", "creating_the_same_cell_returns_existing_data", [](TestContext &test) {
		Terrain3DInstanceStore store;
		auto *cell = store.tryGetOrCreateCell(3, Vector2i(1, 1), 1);
		T3D_REQUIRE(test, cell != nullptr && cell->pushTransform(Transform3D()));
		T3D_CHECK(test, store.tryGetOrCreateCell(3, Vector2i(1, 1)) == cell);
		T3D_CHECK(test, store.instanceCount() == 1 && store.meshCount() == 1);
	});
	runner.run("instancing", "mesh_iteration_is_sorted_and_counts_are_per_mesh", [](TestContext &test) {
		Terrain3DInstanceStore store;
		T3D_REQUIRE(test, populate_instance_store(store));
		T3D_CHECK(test, store.meshes()[0].m_nId == 1 && store.meshes()[1].m_nId == 3);
		T3D_CHECK(test, store.instanceCount() == 12 && store.instanceCount(1) == 6 && store.instanceCount(3) == 6);
		T3D_CHECK(test, store.instanceCount(2) == 0);
	});
	// Sparse sidecars and per-instance edits.
	runner.run("instancing", "uncolored_instances_keep_the_color_sidecar_sparse", [](TestContext &test) {
		Terrain3DInstanceStore store;
		auto *cell = store.tryGetOrCreateCell(0, Vector2i(), 2);
		T3D_REQUIRE(test, cell && cell->pushTransform(Transform3D()));
		T3D_CHECK(test, !cell->usesColors() && !cell->usesOwners());
		T3D_CHECK(test, store.validateDeep());
	});
	runner.run("instancing", "adding_sidecars_backfills_white_colors_and_zero_owners", [](TestContext &test) {
		Terrain3DInstanceStore store;
		auto *cell = store.tryGetOrCreateCell(0, Vector2i(), 3);
		T3D_REQUIRE(test, cell && cell->pushTransform(Transform3D()) && cell->pushTransform(Transform3D()));
		T3D_REQUIRE(test, cell->prepareAppend(1, true, true));
		T3D_CHECK(test, cell->m_colors.size() == 2 && cell->m_owners.size() == 2);
		T3D_CHECK(test, cell->m_colors[0] == T3D_INSTANCE_WHITE_RGBA8 && cell->m_colors[1] == T3D_INSTANCE_WHITE_RGBA8);
		T3D_CHECK(test, cell->m_owners[0] == 0 && cell->m_owners[1] == 0);
	});
	runner.run("instancing", "zero_owner_sidecars_can_be_compacted", [](TestContext &test) {
		Terrain3DInstanceStore store;
		auto *cell = store.tryGetOrCreateCell(0, Vector2i(), 1, false, true);
		T3D_REQUIRE(test, cell && cell->pushTransform(Transform3D()) && cell->m_owners.pushBack(0));
		cell->compactOwnerSidecar();
		T3D_CHECK(test, !cell->usesOwners() && store.validateDeep());
	});
	runner.run("instancing", "nonzero_owner_sidecars_survive_compaction", [](TestContext &test) {
		Terrain3DInstanceStore store;
		auto *cell = store.tryGetOrCreateCell(0, Vector2i(), 1, false, true);
		T3D_REQUIRE(test, cell && cell->pushTransform(Transform3D()) && cell->m_owners.pushBack(42));
		cell->compactOwnerSidecar();
		T3D_CHECK(test, cell->usesOwners() && cell->m_owners[0] == 42);
	});
	runner.run("instancing", "transform_updates_preserve_behavior_flags", [](TestContext &test) {
		Terrain3DInstanceStore store;
		T3D_REQUIRE(test, populate_instance_store(store));
		auto *cell = store.findCell(1, Vector2i(1, 0));
		Transform3D transform;
		transform.origin = Vector3(7, 8, 9);
		cell->setTransform(0, transform);
		T3D_CHECK(test, cell->getTransformOrigin(0) == transform.origin);
		T3D_CHECK(test, cell->getTransformBehaviorFlags(0) == (FOLLOW_TERRAIN_HEIGHT | FOLLOW_TERRAIN_NORMAL));
	});
	runner.run("instancing", "terrain_spacing_rescales_horizontal_position_only", [](TestContext &test) {
		Terrain3DInstanceStore store;
		auto *cell = store.tryGetOrCreateCell(0, Vector2i(), 1);
		Transform3D transform;
		transform.origin = Vector3(2, 3, 4);
		T3D_REQUIRE(test, cell && cell->pushTransform(transform));
		cell->rescaleTransformOriginXz(0, 1, 2);
		T3D_CHECK(test, cell->getTransformOrigin(0) == Vector3(4, 3, 8));
		cell->rescaleTransformOriginXz(0, 0, 4);
		T3D_CHECK(test, cell->getTransformOrigin(0) == Vector3(4, 3, 8));
	});
	runner.run("instancing", "touch_invalidates_bounds_and_skips_revision_zero", [](TestContext &test) {
		t3dInstanceCell_t cell;
		cell.m_bModified = false;
		cell.m_bCachedAabbValid = true;
		cell.m_nRevision = UINT32_MAX;
		cell.touch();
		T3D_CHECK(test, cell.m_bModified && !cell.m_bCachedAabbValid && cell.m_nRevision == 1);
	});
	// Ownership, deletion and serialization.
	runner.run("instancing", "copy_constructor_preserves_all_instance_data", [](TestContext &test) {
		Terrain3DInstanceStore source;
		T3D_REQUIRE(test, populate_instance_store(source));
		Terrain3DInstanceStore copy(source);
		T3D_REQUIRE(test, same_instance_store(source, copy));
		copy.findCell(1, Vector2i(1, 0))->setTransformOriginY(0, 99);
		T3D_CHECK(test, source.findCell(1, Vector2i(1, 0))->getTransformOrigin(0).y == 32);
	});
	runner.run("instancing", "copy_assignment_replaces_existing_destination_data", [](TestContext &test) {
		Terrain3DInstanceStore source, destination;
		T3D_REQUIRE(test, populate_instance_store(source));
		T3D_REQUIRE(test, destination.tryGetOrCreateCell(7, Vector2i()) != nullptr);
		destination = source;
		T3D_CHECK(test, same_instance_store(source, destination) && !destination.hasMesh(7));
		destination = destination;
		T3D_CHECK(test, same_instance_store(source, destination));
	});
	runner.run("instancing", "move_transfers_storage_without_losing_sidecars", [](TestContext &test) {
		Terrain3DInstanceStore source;
		T3D_REQUIRE(test, populate_instance_store(source));
		Terrain3DInstanceStore expected(source);
		Terrain3DInstanceStore moved(eastl::move(source));
		T3D_CHECK(test, same_instance_store(expected, moved));
		T3D_CHECK(test, source.empty() && source.validateDeep());
		Terrain3DInstanceStore destination;
		destination = eastl::move(moved);
		T3D_CHECK(test, same_instance_store(expected, destination));
		T3D_CHECK(test, moved.empty() && moved.validateDeep());
	});
	runner.run("instancing", "erase_cell_preserves_other_cells_and_meshes", [](TestContext &test) {
		Terrain3DInstanceStore store;
		T3D_REQUIRE(test, populate_instance_store(store));
		T3D_CHECK(test, store.eraseCell(1, Vector2i(1, 0)));
		T3D_CHECK(test, store.findCell(1, Vector2i(1, 0)) == nullptr && store.instanceCount() == 9);
		T3D_CHECK(test, store.findCell(1, Vector2i(0, 1)) != nullptr && store.instanceCount(3) == 6);
		T3D_CHECK(test, !store.eraseCell(1, Vector2i(1, 0)) && store.validateDeep());
	});
	runner.run("instancing", "erase_mesh_removes_only_its_instances", [](TestContext &test) {
		Terrain3DInstanceStore store;
		T3D_REQUIRE(test, populate_instance_store(store));
		T3D_CHECK(test, store.eraseMesh(1));
		T3D_CHECK(test, store.meshCount() == 1 && store.instanceCount() == 6 && store.hasMesh(3));
		T3D_CHECK(test, !store.eraseMesh(1) && store.validateDeep());
	});
	runner.run("instancing", "swapping_mesh_ids_preserves_instances", [](TestContext &test) {
		Terrain3DInstanceStore store;
		T3D_REQUIRE(test, populate_instance_store(store));
		const Vector3 origin = store.findCell(1, Vector2i(1, 0))->getTransformOrigin(0);
		store.swapMeshIds(1, 3);
		T3D_CHECK(test, store.findCell(3, Vector2i(1, 0))->getTransformOrigin(0) == origin);
		T3D_CHECK(test, store.instanceCount() == 12 && store.validateDeep());
	});
	runner.run("instancing", "dictionary_export_can_be_imported_without_loss", [](TestContext &test) {
		Terrain3DInstanceStore source, decoded;
		T3D_REQUIRE(test, populate_instance_store(source));
		const Dictionary dictionary = source.exportDictionary();
		decoded.importDictionary(dictionary);
		T3D_CHECK(test, decoded.equalsDictionary(dictionary));
		T3D_CHECK(test, same_instance_store(source, decoded));
		T3D_CHECK(test, decoded.instanceCount() == source.instanceCount() && decoded.validateDeep());
	});
	runner.run("instancing", "reset_allows_repopulation_and_shutdown_releases_storage", [](TestContext &test) {
		Terrain3DInstanceStore store;
		const size_t inline_bytes = store.residentBytes();
		T3D_REQUIRE(test, populate_instance_store(store));
		T3D_CHECK(test, store.residentBytes() > inline_bytes);
		store.reset();
		T3D_CHECK(test, store.empty() && store.validateDeep());
		T3D_REQUIRE(test, populate_instance_store(store));
		store.shutdown();
		T3D_CHECK(test, store.empty() && store.instanceCount() == 0 && store.residentBytes() == inline_bytes);
	});
	runner.run("instancing", "runtime_store_requires_an_allocator_before_growth", [](TestContext &test) {
		Terrain3DInstanceStore store(TERRAIN3D_INSTANCE_STORE_RUNTIME_PREPARED);
		T3D_CHECK(test, !store.runtimeAllocatorReady());
		T3D_CHECK(test, store.tryGetOrCreateCell(0, Vector2i(), 1) == nullptr);
		T3D_CHECK(test, store.empty());
	});
}
} // namespace terrain3d_tests
