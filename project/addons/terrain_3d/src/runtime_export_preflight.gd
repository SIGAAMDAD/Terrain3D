# Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# P05: read-only checks before building template exports. Does not cook assets.
@tool
extends RefCounted

const REJECTED_NATIVE_NODE_CLASSES: PackedStringArray = [
	"Terrain3DWorldController",
	"Terrain3DEditor",
	"Terrain3DErosionJob",
	"Terrain3DAutoShaderBaker",
	"Terrain3DLandformBaker",
	"Terrain3DFoliageGenerator",
	"Terrain3DHLODBuilder",
]
const HLOD_MANIFEST_NAME: String = "terrain3d_hlod_manifest.tres"


static func validate_project() -> Array[String]:
	var failures: Array[String] = []
	if not ClassDB.class_exists("Terrain3DWorldController"):
		failures.append("P05 preflight requires the editor Terrain3D native binary")
		return failures

	var pending: Array[String] = ["res://"]
	while not pending.is_empty():
		var directory: String = pending.pop_back()
		for subdir: String in DirAccess.get_directories_at(directory):
			if not subdir.begins_with("."):
				pending.push_back(directory.path_join(subdir))
		for file_name: String in DirAccess.get_files_at(directory):
			var path: String = directory.path_join(file_name)
			if file_name.ends_with(".tscn") or file_name.ends_with(".scn"):
				_validate_scene(path, failures)
			elif file_name.ends_with(".tres") or file_name.ends_with(".res"):
				_validate_resource(path, failures)
	return failures


static func _validate_resource(path: String, failures: Array[String]) -> void:
	var resource: Resource = ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
	if resource == null:
		failures.append("%s: exported Resource could not be loaded" % path)
		return
	if resource.get_class() in REJECTED_NATIVE_NODE_CLASSES:
		failures.append("%s: editor-only native Resource must not be present in a runtime export (%s)" % [path, resource.get_class()])


static func _validate_scene(path: String, failures: Array[String]) -> void:
	var resource: Resource = ResourceLoader.load(path, "PackedScene", ResourceLoader.CACHE_MODE_IGNORE)
	var scene: PackedScene = resource as PackedScene
	if scene == null:
		failures.append("%s: PackedScene could not be loaded" % path)
		return

	# Instance while detached: no _ready(), editor rebuild, or runtime publishing.
	var root: Node = scene.instantiate(PackedScene.GEN_EDIT_STATE_DISABLED)
	if root == null:
		failures.append("%s: PackedScene could not be instantiated" % path)
		return
	var pending: Array[Node] = [root]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		var native_class_name: String = node.get_class()
		if native_class_name in REJECTED_NATIVE_NODE_CLASSES:
			failures.append("%s: node '%s' contains uncooked authoring class %s; bake World data and remove the authoring node before export" % [path, str(root.get_path_to(node)), native_class_name])
		if node is Terrain3D:
			_validate_terrain(path, node as Terrain3D, failures)
		for child: Node in node.get_children():
			pending.append(child)
	root.free()


static func _validate_terrain(scene_path: String, terrain: Terrain3D, failures: Array[String]) -> void:
	if not terrain.hlod_enabled:
		return
	var manifest_path: String = terrain.hlod_manifest_path
	if manifest_path.is_empty() and not terrain.data_directory.is_empty():
		manifest_path = terrain.data_directory.trim_suffix("/").path_join(HLOD_MANIFEST_NAME)
	if manifest_path.is_empty() or not FileAccess.file_exists(manifest_path):
		failures.append("%s: HLOD is enabled but a cooked HLOD manifest is missing (%s)" % [scene_path, manifest_path])
		return
	var manifest: Terrain3DHLODManifest = ResourceLoader.load(manifest_path, "Terrain3DHLODManifest", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DHLODManifest
	if manifest == null or manifest.format != 3 or not manifest.validate_runtime_blob():
		failures.append("%s: HLOD manifest is invalid or missing its cooked runtime blob: %s" % [scene_path, manifest_path])
		return
	if manifest.max_level != terrain.hlod_max_level:
		failures.append("%s: HLOD manifest level does not match Terrain3D settings: %s" % [scene_path, manifest_path])
	for value: Variant in manifest.entries:
		if not value is Dictionary:
			failures.append("%s: HLOD manifest has a malformed entry: %s" % [scene_path, manifest_path])
			continue
		var entry: Dictionary = value
		var package_path: String = str(entry.get("package_path", ""))
		if package_path.is_empty() or not FileAccess.file_exists(package_path):
			failures.append("%s: cooked HLOD package is missing: %s" % [scene_path, package_path])
			continue
		var package: Terrain3DHLODPackage = ResourceLoader.load(package_path, "Terrain3DHLODPackage", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DHLODPackage
		if package == null or package.mesh == null or package.build_signature != int(entry.get("build_signature", -1)) or package.source_signature != int(entry.get("source_signature", -1)):
			failures.append("%s: cooked HLOD package is missing or stale: %s" % [scene_path, package_path])
