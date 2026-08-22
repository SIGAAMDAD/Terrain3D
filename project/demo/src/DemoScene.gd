@tool
extends Node

@onready var terrain: Terrain3D = find_child("Terrain3D")


func _ready() -> void:
	if not Engine.is_editor_hint() and has_node("UI"):
		$UI.player = $Player

	# EditorInterface is editor-only, so keep this lookup runtime-safe for exports.
	const sky3d_path := "res://addons/sky_3d/src/Sky3D.gd"
	if not _is_editor_plugin_enabled(&"sky_3d") or not has_node("Environment") or \
			 not ResourceLoader.exists(sky3d_path, "Script"):
		return
	var sky3d_script := load(sky3d_path) as Script
	if sky3d_script == null:
		return
	$Environment.queue_free()
	var sky3d: Node = sky3d_script.new() as Node
	if sky3d == null:
		return
	sky3d.name = "Sky3D"
	add_child(sky3d, true)
	move_child(sky3d, 1)
	sky3d.owner = self
	sky3d.set("current_time", 10)
	sky3d.set("enable_editor_time", false)


func _is_editor_plugin_enabled(plugin_name: StringName) -> bool:
	if not Engine.is_editor_hint() or not Engine.has_singleton(&"EditorInterface"):
		return false
	var editor_interface: Object = Engine.get_singleton(&"EditorInterface")
	return editor_interface != null and bool(editor_interface.call("is_plugin_enabled", plugin_name))
