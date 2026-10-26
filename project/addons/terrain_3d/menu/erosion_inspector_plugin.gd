# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Lightweight Terrain3D Inspector launcher. Authoring workflows use editor windows.
@tool
extends EditorInspectorPlugin

var plugin: Terrain3DEditorPluginBase


func _can_handle(p_object: Object) -> bool:
	return p_object is Terrain3D or p_object is Terrain3DMaterial


func _parse_begin(p_object: Object) -> void:
	if is_instance_valid(plugin):
		_add_tools(p_object)


func _add_tools(p_object: Object) -> void:
	var terrain_node: Terrain3D = p_object as Terrain3D
	if not is_instance_valid(terrain_node):
		terrain_node = plugin.get_terrain()
		if not is_instance_valid(terrain_node) or terrain_node.material != p_object:
			return
	var target: WeakRef = weakref(terrain_node)
	var section: VBoxContainer = VBoxContainer.new()
	section.add_theme_constant_override("separation", 5)
	var header: Label = Label.new()
	header.text = "Terrain3D Tools"
	header.add_theme_font_size_override("font_size", 14)
	section.add_child(header)

	var actions: HFlowContainer = HFlowContainer.new()
	actions.add_theme_constant_override("h_separation", 5)
	actions.add_theme_constant_override("v_separation", 5)
	section.add_child(actions)
	for action: String in ["Paint", "Erosion...", "All Tools...", "Render Quality..."]:
		var button: Button = Button.new()
		button.text = action
		button.tooltip_text = "Open %s for the selected terrain." % action.trim_suffix("...")
		button.pressed.connect(_open_tools.bind(target, action))
		actions.add_child(button)
	add_custom_control(section)


func _open_tools(p_target: WeakRef, p_action: String) -> void:
	var terrain_node := p_target.get_ref() as Terrain3D
	if not is_instance_valid(plugin) or not is_instance_valid(terrain_node):
		return
	EditorInterface.set_main_screen_editor("3D")
	var selection := EditorInterface.get_selection()
	selection.clear()
	selection.add_node(terrain_node)
	EditorInterface.inspect_object(terrain_node)
	# Let the selection bind the terrain before opening a dependent popup.
	if p_action == "All Tools...":
		plugin.call_deferred("_open_terrain_tools_menu")
	elif p_action == "Erosion...":
		plugin.call_deferred("open_erosion_simulator")
	elif p_action == "Render Quality...":
		plugin.call_deferred("open_shader_performance")


func _parse_property(_object: Object, _type: Variant.Type, p_name: String,
		_hint_type: PropertyHint, _hint_string: String, p_usage_flags: int, _wide: bool) -> bool:
	# Native editors retain undo, resources, ranges and instance validation.
	# Preserve native read-only diagnostics; suppress only obsolete aliases.
	return p_name in [
		"save_16_bit", "show_grid", "gi_mode", "ocean_gi_mode", "packed_pbr_single_fetch",
	]
