# Execute with the custom editor-native Godot binary:
# godot --headless --path project --script res://addons/terrain_3d/tests/hlod_runtime_hierarchy_test.gd
# Tests the real compiled manifest codec with an omitted parent and corrupt links.
extends SceneTree


func _initialize() -> void:
	var child_entry: Dictionary = _entry(1, Vector2i(0, 0), "res://hlod_child.res")
	var parent_entry: Dictionary = _entry(2, Vector2i(0, 0), "res://hlod_parent.res")
	var hierarchy := Terrain3DHLODManifest.new()
	hierarchy.entries = [child_entry, parent_entry]
	if not hierarchy.compile_runtime_blob() or not hierarchy.validate_runtime_blob():
		_fail("Valid parent and child were rejected")
		return

	# Entry records are 104-byte fixed headers followed by UTF-8 path bytes.
	# The first entry's parent is at byte offset 80 within its header.
	var valid: PackedByteArray = hierarchy.runtime_blob
	var wrong_parent: PackedByteArray = valid.duplicate()
	wrong_parent.encode_s32(24 + 80, -1)
	hierarchy.runtime_blob = wrong_parent
	if hierarchy.validate_runtime_blob():
		_fail("Root count did not reject a corrupted child parent index")
		return

	var parent_start: int = 24 + 104 + String(child_entry["package_path"]).to_utf8_buffer().size()
	var missing_child_link: PackedByteArray = valid.duplicate()
	missing_child_link.encode_s32(parent_start + 84, -1)
	hierarchy.runtime_blob = missing_child_link
	if hierarchy.validate_runtime_blob():
		_fail("Parent without reciprocal child link was accepted")
		return

	# Omitting the intermediate L2 parent creates two valid independent L1 roots.
	var split := Terrain3DHLODManifest.new()
	split.entries = [_entry(1, Vector2i(0, 0), "res://a.res"),
		_entry(1, Vector2i(1, 0), "res://b.res")]
	if not split.compile_runtime_blob() or not split.validate_runtime_blob():
		_fail("Independently resident split children were rejected")
		return
	var split_blob: PackedByteArray = split.runtime_blob
	var root_start: int = 24 + 2 * 104 + String("res://a.res").to_utf8_buffer().size() + \
		String("res://b.res").to_utf8_buffer().size()
	var duplicate_root: PackedByteArray = split_blob.duplicate()
	duplicate_root.encode_u32(root_start + 4, 0)
	split.runtime_blob = duplicate_root
	if split.validate_runtime_blob():
		_fail("Duplicate root index was accepted")
		return

	var duplicates := Terrain3DHLODManifest.new()
	duplicates.entries = [_entry(1, Vector2i(0, 0), "res://a.res"),
		_entry(1, Vector2i(0, 0), "res://b.res")]
	if duplicates.compile_runtime_blob():
		_fail("Duplicate same-level cell identity was accepted")
		return

	print("HLOD packed runtime hierarchy regression passed")
	quit(0)


func _entry(level: int, cluster: Vector2i, path: String) -> Dictionary:
	var span: int = 1 << level
	var region_min: Vector2i = cluster * span - Vector2i(16, 16)
	return {"level": level, "cluster": cluster, "region_min": region_min,
		"region_max": region_min + Vector2i(span - 1, span - 1),
		"package_path": path, "local_bounds": AABB(Vector3.ZERO, Vector3.ONE),
		"geometric_error": 1.0, "source_signature": 12, "build_signature": 34,
		"estimated_gpu_bytes": 256}


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
