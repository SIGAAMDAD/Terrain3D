# Run with the matching custom Godot 4.7.2 host to capture native ClassDB.
# godot --headless --path project -s res://addons/terrain_3d/tests/dual_dll/classdb_snapshot.gd -- --output=<absolute JSON path>
extends SceneTree

func _initialize() -> void:
	var output_path: String = ""
	for arg: String in OS.get_cmdline_user_args():
		if arg.begins_with("--output="):
			output_path = arg.trim_prefix("--output=")
	if output_path.is_empty():
		push_error("ClassDB snapshot requires -- --output=<absolute JSON path>")
		quit(1)
		return
	var native_classes: Array[String] = []
	for cls: String in ClassDB.get_class_list():
		if cls.begins_with("Terrain3D"):
			native_classes.append(cls)
	native_classes.sort()
	var snapshot: Dictionary = {}
	for cls: String in native_classes:
		snapshot[cls] = {
			"parent": ClassDB.get_parent_class(cls),
			"properties": ClassDB.class_get_property_list(cls, true),
			"methods": ClassDB.class_get_method_list(cls, true),
			"signals": ClassDB.class_get_signal_list(cls, true),
		}
	var file: FileAccess = FileAccess.open(output_path, FileAccess.WRITE)
	if file == null:
		push_error("Could not write ClassDB snapshot: " + output_path)
		quit(1)
		return
	file.store_string(JSON.stringify(snapshot, "\t", true))
	file.close()
	print("[Terrain3D][ClassDB] recorded ", native_classes.size(), " classes to ", output_path)
	quit(0)
