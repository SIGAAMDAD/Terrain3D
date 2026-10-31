# Run with matching native editor (never against a template):
# godot --headless --editor --path project --script res://addons/terrain_3d/tests/dual_dll/world_export_smoke.gd
extends SceneTree

const Preflight = preload("res://addons/terrain_3d/src/runtime_export_preflight.gd")
const SCENE_PATH: String = "res://addons/terrain_3d/tests/dual_dll/_p05_world_controller_fixture.tscn"


func _initialize() -> void:
	if not ClassDB.class_exists("Terrain3DWorldController"):
		push_error("P05 World export smoke requires editor native registration")
		quit(1)
		return
	var root: Node3D = Node3D.new()
	root.name = "Root"
	var controller: Terrain3DWorldController = Terrain3DWorldController.new()
	controller.name = "WorldAuthoring"
	root.add_child(controller)
	controller.owner = root
	var scene: PackedScene = PackedScene.new()
	var packed: Error = scene.pack(root)
	var saved: Error = ResourceSaver.save(scene, SCENE_PATH) if packed == OK else packed
	root.free()
	if saved != OK:
		push_error("P05 fixture could not be written: %s" % error_string(saved))
		quit(1)
		return
	var rejected: Array[String] = Preflight.validate_project()
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SCENE_PATH))
	var found: bool = false
	for failure: String in rejected:
		if failure.contains("_p05_world_controller_fixture.tscn") and failure.contains("Terrain3DWorldController"):
			found = true
	if not found:
		push_error("P05 did not reject a saved World authoring node")
		quit(1)
		return
	print("[Terrain3D][P05] PASS: saved World controller rejected for runtime export")
	quit(0)
