# Run from Godot's Script editor with File > Run (EditorScript).
# This script deliberately has no Terrain3D native class dependency.
@tool
extends EditorScript

const PLUGIN_PATH: String = "res://addons/terrain_3d/src/editor_plugin.gd"
const TOOLBAR_PATH: String = "res://addons/terrain_3d/src/toolbar.gd"

func _run() -> void:
	print("=== Terrain3D sidebar probe ===")
	print("native Terrain3D: ", ClassDB.class_exists("Terrain3D"))
	print("native Terrain3DUtil: ", ClassDB.class_exists("Terrain3DUtil"))
	print("native Terrain3DEditor: ", ClassDB.class_exists("Terrain3DEditor"))
	var plugin_resource: Script = load(PLUGIN_PATH) as Script
	print("editor plugin script loaded: ", plugin_resource != null)
	if plugin_resource != null:
		print("editor plugin can instantiate: ", plugin_resource.can_instantiate())
	var selection: EditorSelection = EditorInterface.get_selection()
	if is_instance_valid(selection):
		for node: Node in selection.get_selected_nodes():
			print("selected: ", node.get_path(), " / ", node.get_class())
			if node.has_method("get_native_build_flavor"):
				print("selected native flavor: ", node.call("get_native_build_flavor"))
			if node.has_method("get_native_api_revision"):
				print("selected native revision: ", node.call("get_native_api_revision"))
			if node.has_method("get_native_build_id"):
				print("selected native build ID: ", node.call("get_native_build_id"))
			if node.has_method("get_data"):
				print("selected terrain data: ", node.call("get_data"))
	var sidebar_found: bool = false
	var plugin_found: bool = false
	var root: Window = EditorInterface.get_base_control().get_window()
	for node: Node in root.find_children("*", "", true, false):
		var script: Script = node.get_script() as Script
		if script == null:
			continue
		if script.resource_path == PLUGIN_PATH:
			plugin_found = true
			print("editor plugin node exists: ", node.get_path())
			print("native editor initialized: ", node.get("_native_editor_initialized"))
			print("plugin terrain bound: ", node.get("terrain"))
			var ui: Object = node.get("ui")
			print("plugin UI constructed: ", is_instance_valid(ui))
			if is_instance_valid(ui):
				print("plugin toolbar constructed: ", is_instance_valid(ui.get("toolbar")))
		if script.resource_path == TOOLBAR_PATH:
			sidebar_found = true
			var control: Control = node as Control
			print("sidebar path: ", node.get_path())
			print("sidebar visible: ", control.visible)
			print("sidebar visible in tree: ", control.is_visible_in_tree())
			print("sidebar children: ", control.get_child_count())
			print("sidebar parent: ", control.get_parent().get_path())
	print("editor plugin node found: ", plugin_found)
	print("toolbar node found: ", sidebar_found)
	if not plugin_found:
		print("ACTION: check first GDScript parse error and native-editor-build mismatch in Output.")
	elif not sidebar_found:
		print("ACTION: editor plugin exists but toolbar construction/registration failed.")
	else:
		print("ACTION: check visible flags and selected terrain state above.")
	print("=== end sidebar probe ===")
