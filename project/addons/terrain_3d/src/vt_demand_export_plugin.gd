# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends EditorExportPlugin

# Include explicitly addressed baked data. Godot does not import these byte
# streams as Resources, while native runtime readers expect the original paths.
# HLOD manifests/packages also use dynamic paths in runtime code.
func _get_name() -> String:
	return "Terrain3D Cooked Runtime Artifacts"


func _export_begin(_features: PackedStringArray, _is_debug: bool, _path: String, _flags: int) -> void:
	var pending: Array[String] = ["res://"]
	while not pending.is_empty():
		var directory: String = pending.pop_back()
		for subdir: String in DirAccess.get_directories_at(directory):
			if subdir.begins_with("."):
				continue
			pending.push_back(directory.path_join(subdir))
		for filename: String in DirAccess.get_files_at(directory):
			# The runtime uses getDataDirectory()/terrain3d_instances.t3dvt.
			# Export only that V1 package, never the .vt_cache source fragments.
			var include_artifact: bool = filename == "terrain3d_instances.t3dvt"
			include_artifact = include_artifact or filename.ends_with(".t3sp") or filename.ends_with(".t3pm")
			if filename.ends_with(".vt_demand.t3vd"):
				var stem: String = filename.trim_suffix(".vt_demand.t3vd")
				include_artifact = FileAccess.file_exists(directory.path_join(stem + ".tres")) or FileAccess.file_exists(directory.path_join(stem + ".res"))
			# HLOD resources are loaded by manifest/package path at runtime,
			# not by an external Resource reference in a packed scene.
			if filename == "terrain3d_hlod_manifest.tres" or filename == "terrain3d_hlod_manifest.res":
				include_artifact = true
			if (directory.ends_with("/_hlod") or directory.contains("/_hlod/")) and (filename.ends_with(".tres") or filename.ends_with(".res")):
				include_artifact = true
			if not include_artifact:
				continue
			var artifact: String = directory.path_join(filename)
			var bytes: PackedByteArray = FileAccess.get_file_as_bytes(artifact)
			if not bytes.is_empty():
				add_file(artifact, bytes, false)


# Retired native filenames are never dependencies of an exported runtime.
# Keep this exact-name list aligned with tools/dual_dll/retire_legacy.py.
const LEGACY_NATIVE_STEMS = [
    "libterrain.windows.x86_64",
    "libterrain.windows.editor_debug.x86_64",
    "libterrain.windows.runtime.x86_64",
    "terrain3d.windows.x86_64",
    "libterrain.windows.debug.x86_32",
    "libterrain.windows.release.x86_32",
]


# The editor superset library must never be staged as an ordinary export file.
# Godot's GDExtension exporter separately selects the matching template binary.
func _export_file(path: String, _type: String, _features: PackedStringArray) -> void:
	var filename: String = path.get_file()
	if filename == "libterrain.windows.editor.x86_64.dll" or filename == "libterrain.windows.editor.x86_64.pdb":
		skip()
		return
	if filename.get_extension().to_lower() in ["dll", "pdb", "lib", "exp"] and filename.get_basename() in LEGACY_NATIVE_STEMS:
		skip()
