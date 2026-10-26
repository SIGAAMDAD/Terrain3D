# Run only with the custom Terrain3D editor-native Godot build.
# godot --headless --path project --script res://addons/terrain_3d/tests/hlod_region_leaf_test.gd
extends SceneTree


func _initialize() -> void:
	var baker := Terrain3DHLODBaker.new()
	baker.layer_settings.region_leaves = true
	baker.layer_settings.spatial_partitioning = true
	var source := Terrain3DHLODBaker.Source.new()
	source.owner_region = Vector2i(-32, -32)
	source.min_level = 1
	source.source_key = "leaf-fixture"
	var source_array: Array[Terrain3DHLODBaker.Source] = [source]
	var empty_children: Dictionary[Vector2i, Terrain3DHLODBaker.BuiltCluster] = {}
	var leaves: Dictionary[Vector2i, Array] = baker._collect_level_plans(0, empty_children, source_array)
	if leaves.size() != 1 or not leaves.has(Vector2i.ZERO) or leaves[Vector2i.ZERO].size() != 1:
		_fail("Minimum-level-1 source was not assigned to a 128m leaf")
		return
	var child := Terrain3DHLODBaker.BuiltCluster.new()
	child.level = 0
	child.cluster = Vector2i.ZERO
	child.planned_triangles = 300000
	child.planned_sections = 64
	var children: Dictionary[Vector2i, Terrain3DHLODBaker.BuiltCluster] = {}
	children[Vector2i.ZERO] = child
	var parents: Dictionary[Vector2i, Array] = baker._collect_level_plans(1, children, source_array)
	if parents.size() != 1 or parents[Vector2i.ZERO].size() != 1 or not parents[Vector2i.ZERO][0] is Terrain3DHLODBaker.BuiltCluster:
		_fail("The parent duplicated a source already represented by its leaf")
		return
	var neighboring := Terrain3DHLODBaker.BuiltCluster.new()
	neighboring.level = 0
	neighboring.cluster = Vector2i(1, 0)
	neighboring.planned_triangles = 300000
	neighboring.planned_sections = 64
	var partition_cost: Dictionary = {"triangles": 600000, "sections": 128}
	if baker._partition_reason(1, Vector2i.ZERO, [child, neighboring], partition_cost, {}) != "triangle_budget":
		_fail("Oversized 256m parent was not split into independent region leaves")
		return
	var jobs: Array[Dictionary] = [
		{"id": "0:0:0", "level": 0, "cluster": Vector2i.ZERO,
			"dependencies": PackedStringArray(), "action": "build"},
		{"id": "1:0:0", "level": 1, "cluster": Vector2i.ZERO,
			"dependencies": PackedStringArray(["0:0:0"]), "action": "build"},
	]
	if not bool(Terrain3DHLODBuildSchedule.validate_jobs(jobs).get("ok", false)):
		_fail("The child-first scheduler rejected the 128m to 256m hierarchy")
		return
	var manifest := Terrain3DHLODManifest.new()
	manifest.entries = [_entry(0, Vector2i.ZERO, "res://leaf.res"),
		_entry(1, Vector2i.ZERO, "res://parent.res")]
	if not manifest.compile_runtime_blob() or not manifest.validate_runtime_blob():
		_fail("The native runtime codec rejected the level-zero leaf")
		return
	# Without an intermediate parent, independently streamed 128m leaves
	# must form separate roots in the packed runtime hierarchy.
	var split := Terrain3DHLODManifest.new()
	split.entries = [_entry(0, Vector2i.ZERO, "res://leaf-a.res"),
		_entry(0, Vector2i(1, 0), "res://leaf-b.res")]
	if not split.compile_runtime_blob() or not split.validate_runtime_blob():
		_fail("Runtime codec rejected separate 128m HLOD roots")
		return
	var wire: PackedByteArray = manifest.runtime_blob
	if wire.decode_u16(4) != 5:
		_fail("The editor did not emit the new spatial hierarchy format")
		return
	# The old decoder version must continue to reject previously impossible level-zero entries.
	var old_wire: PackedByteArray = _legacy_wire(wire, 2)
	manifest.runtime_blob = old_wire
	if manifest.validate_runtime_blob():
		_fail("Legacy format accepted a level-zero entry")
		return
	var corrupted: PackedByteArray = wire.duplicate()
	# V5's first entry begins at byte 28, so region minimum is byte 40.
	corrupted.encode_s32(40, 255)
	manifest.runtime_blob = corrupted
	if manifest.validate_runtime_blob():
		_fail("Packed hierarchy accepted an invalid spatial footprint")
		return
	var legacy := Terrain3DHLODManifest.new()
	legacy.entries = [_entry(1, Vector2i.ZERO, "res://old.res")]
	if not legacy.compile_runtime_blob():
		_fail("Legacy compatibility fixture failed to encode")
		return
	var legacy_wire: PackedByteArray = legacy.runtime_blob
	legacy.runtime_blob = _legacy_wire(legacy_wire, 2)
	if not legacy.validate_runtime_blob():
		_fail("Existing version-two hierarchy is no longer readable")
		return
	var settings := Terrain3DHLODLayerSettings.new()
	var old_hash: int = settings.get_recipe_hash()
	settings.region_leaves = true
	if settings.get_recipe_hash() == old_hash:
		_fail("Enabling 128m leaves did not invalidate existing cook signatures")
		return
	var package := Terrain3DHLODPackage.new()
	package.level = 0
	if package.level != 0:
		_fail("Level-zero package storage clamped the level")
		return
	print("HLOD 128m leaf topology and legacy decode fixtures passed")
	quit(0)


func _legacy_wire(wire: PackedByteArray, version: int) -> PackedByteArray:
    var legacy: PackedByteArray = wire.slice(0, 24)
    legacy.append_array(wire.slice(28))
    legacy.encode_u16(4, version)
    return legacy


func _entry(level: int, cluster: Vector2i, path: String) -> Dictionary:
	var span: int = 1 << level
	var first: Vector2i = Vector2i(-32, -32) + cluster * span
	return {"level": level, "cluster": cluster, "region_min": first,
		"region_max": first + Vector2i(span - 1, span - 1),
		"local_bounds": AABB(Vector3.ZERO, Vector3.ONE), "package_path": path,
		"geometric_error": 1.0, "source_signature": 12,
		"build_signature": 34, "estimated_gpu_bytes": 256}


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
