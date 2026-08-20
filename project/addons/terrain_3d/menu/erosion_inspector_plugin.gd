# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Adds the Terrain3D erosion workflow directly to Terrain3D's node Inspector.
@tool
extends EditorInspectorPlugin

const EROSION_PANEL_PATH: String = "res://addons/terrain_3d/menu/erosion_simulator.gd"

var plugin: T3DEditorPluginBase


func _can_handle(p_object: Object) -> bool:
	return p_object is T3DTerrain


func _parse_begin(p_object: Object) -> void:
	if not (p_object is T3DTerrain):
		return

	# Keep the primary Terrain3D editor independent from the optional erosion UI.
	# A bad panel script should produce an Inspector error, not disable the plugin.
	var panel_script := load(EROSION_PANEL_PATH) as Script
	if not is_instance_valid(panel_script):
		_add_error_control("Terrain erosion UI could not be loaded.")
		return

	var panel: Control = panel_script.new() as Control
	if not is_instance_valid(panel):
		_add_error_control("Terrain erosion UI could not be instantiated.")
		return
	panel.set("plugin", plugin)
	panel.set("terrain", p_object)
	add_custom_control(panel)


func _add_error_control(p_message: String) -> void:
	var error_label: Label = Label.new()
	error_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	error_label.text = p_message
	error_label.tooltip_text = "Check the editor Output panel for Terrain3D erosion script errors."
	error_label.add_theme_color_override("font_color", Color(1.0, 0.35, 0.35))
	add_custom_control(error_label)
	push_error(p_message)
