# Godot 4.7 custom GDExtension: --headless --path project --script res://addons/terrain_3d/tests/hlod_subregion_64m_test.gd
extends SceneTree


func _initialize() -> void:
	var baker := Terrain3DHLODBaker.new()
	baker.layer_settings.subregion_leaves = true
	baker.layer_settings.spatial_partitioning = true
	var inputs: Array[Terrain3DHLODBaker.Source] = []
	var first_origin := Vector3(-32.0 * 128.0, 0.0, -32.0 * 128.0)
	for slot: int in range(4):
		var source := Terrain3DHLODBaker.Source.new()
		source.owner_region = Vector2i(-32, -32)
		source.min_level = 1
		source.source_key = "fixture:%d" % slot
		source.source_triangles = 120
		source.bounds = AABB(first_origin + Vector3(float((slot & 1) * 64 + 10), 0.0,
			float(((slot >> 1) & 1) * 64 + 10)), Vector3(1, 1, 1))
		inputs.append(source)
	var leaf_map: Dictionary[Vector2i, Array] = baker._collect_level_plans(-1, {}, inputs)
	if leaf_map.size() != 4:
		_fail("64m source partition did not produce four independent cells")
		return
	for slot: int in range(4):
		var cell := Vector2i(slot & 1, (slot >> 1) & 1)
		if not leaf_map.has(cell) or leaf_map[cell].size() != 1:
			_fail("64m source owner was dropped or duplicated")
			return
	# Wide authored geometry belongs to the 128m parent, never one 64m leaf.
	var wide := Terrain3DHLODBaker.Source.new()
	wide.owner_region = Vector2i(-32, -32)
	wide.min_level = 1
	wide.source_key = "fixture:wide"
	wide.bounds = AABB(first_origin + Vector3(50, 0, 50), Vector3(30, 2, 30))
	var mixed: Array[Terrain3DHLODBaker.Source] = inputs.duplicate()
	mixed.append(wide)
	if baker._collect_level_plans(-1, {}, mixed).size() != 4 or \
			baker._collect_level_plans(0, {}, mixed)[Vector2i.ZERO].size() != 1:
		_fail("Cross-quadrant geometry was not kept at its 128m parent")
		return
	var leaf_children: Dictionary[Vector2i, Terrain3DHLODBaker.BuiltCluster] = {}
	var jobs: Array[Dictionary] = []
	var manifest_entries: Array = []
	for slot: int in range(4):
		var key := Vector2i(slot & 1, (slot >> 1) & 1)
		var child := Terrain3DHLODBaker.BuiltCluster.new()
		child.level = -1
		child.cluster = key
		child.region_min = Vector2i(-32, -32)
		child.region_max = child.region_min
		leaf_children[key] = child
		jobs.append({"level": -1, "id": Terrain3DHLODBuildSchedule.job_id(-1, key),
			"cluster": key, "dependencies": PackedStringArray(), "action": "build"})
		manifest_entries.append(_entry(-1, key, "res://sub_%d.res" % slot))
	var parents: Dictionary[Vector2i, Array] = baker._collect_level_plans(0, leaf_children, inputs)
	if parents.size() != 1 or parents[Vector2i.ZERO].size() != 4:
		_fail("128m parent did not inherit all four unique 64m children")
		return
	var dependencies := PackedStringArray()
	for job: Dictionary in jobs:
		dependencies.append(job["id"])
	jobs.append({"level": 0, "id": "0:0:0", "cluster": Vector2i.ZERO,
		"dependencies": dependencies, "action": "build"})
	if not bool(Terrain3DHLODBuildSchedule.validate_jobs(jobs).get("ok", false)):
		_fail("64m child-first dependency schedule rejected")
		return
	manifest_entries.append(_entry(0, Vector2i.ZERO, "res://parent.res"))
	var manifest := Terrain3DHLODManifest.new()
	manifest.entries = manifest_entries
	if not manifest.compile_runtime_blob() or not manifest.validate_runtime_blob():
		_fail("64m native packed hierarchy encode/decode failed")
		return
	var blob: PackedByteArray = manifest.runtime_blob
	if blob.decode_u16(4) != 5 or blob[28] != 255:
		_fail("Version 5 did not encode the signed 64m level")
		return
	var older_blob: PackedByteArray = _legacy_wire(blob, 3)
	manifest.runtime_blob = older_blob
	if manifest.validate_runtime_blob():
		_fail("Version 3 incorrectly accepted a 64m subregion entry")
		return
	var old_manifest := Terrain3DHLODManifest.new()
	old_manifest.entries = [_entry(0, Vector2i.ZERO, "res://legacy.res")]
	if not old_manifest.compile_runtime_blob():
		_fail("Legacy package fixture did not encode")
		return
	var old_data: PackedByteArray = old_manifest.runtime_blob
	old_manifest.runtime_blob = _legacy_wire(old_data, 3)
	if not old_manifest.validate_runtime_blob():
		_fail("Version 3 package failed backward compatibility")
		return
	var package := Terrain3DHLODPackage.new()
	package.level = -1
	if package.level != -1:
		_fail("Subregion package level was clamped")
		return
	var hash_before: int = baker.layer_settings.get_recipe_hash()
	baker.layer_settings.subregion_leaves = false
	if baker.layer_settings.get_recipe_hash() == hash_before:
		_fail("Subregion policy was not included in dirty cook signature")
		return
	print("HLOD 64m independent packages: spatial identities, hierarchy, V3 compatibility and recipe hash passed")
	quit(0)


func _legacy_wire(wire: PackedByteArray, version: int) -> PackedByteArray:
    var legacy: PackedByteArray = wire.slice(0, 24)
    legacy.append_array(wire.slice(28))
    legacy.encode_u16(4, version)
    return legacy


func _entry(level: int, cell: Vector2i, path: String) -> Dictionary:
	var first := Vector2i(-32, -32)
	if level < 0:
		first += Vector2i(cell.x >> 1, cell.y >> 1)
	else:
		first += cell * (1 << level)
	var span: int = 1 << maxi(level, 0)
	return {"level": level, "cluster": cell, "region_min": first,
		"region_max": first + Vector2i(span - 1, span - 1),
		"local_bounds": AABB(Vector3.ZERO, Vector3.ONE),
		"package_path": path, "geometric_error": 1.0,
		"source_signature": 23, "build_signature": 45,
		"estimated_gpu_bytes": 256}


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
