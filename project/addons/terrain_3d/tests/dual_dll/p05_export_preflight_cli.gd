# Run in the editor host before exporting a runtime template:
# godot --headless --editor --path project --script res://addons/terrain_3d/tests/dual_dll/p05_export_preflight_cli.gd
extends SceneTree

const ExportPreflight = preload("res://addons/terrain_3d/src/runtime_export_preflight.gd")


func _initialize() -> void:
	var failures: Array[String] = ExportPreflight.validate_project()
	if not failures.is_empty():
		for failure: String in failures:
			push_error("[Terrain3D][P05] " + failure)
		quit(1)
		return
	print("[Terrain3D][P05] PASS: exported scene classes and cooked HLOD references")
	quit(0)
