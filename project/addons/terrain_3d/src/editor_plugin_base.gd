# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Typed contract shared by Terrain3D editor-side helpers.
@tool
class_name Terrain3DEditorPluginBase
extends EditorPlugin

var debug: int = 0
var editor: Terrain3DEditor
var editor_settings: EditorSettings
var godot_editor_window: Window
var viewport: SubViewport
var mouse_global_position: Vector3 = Vector3.ZERO
var mouse_in_main: bool = false

var terrain: Terrain3D
var nav_region: NavigationRegion3D

var modifier_ctrl: bool = false
var modifier_alt: bool = false
var modifier_shift: bool = false
var _input_mode: int = 0
var rmb_release_time: int = 0


func _read_input(_p_event: InputEvent = null) -> AfterGUIInput:
	return AFTER_GUI_INPUT_PASS


func get_erosion_settings(_p_terrain: Terrain3D) -> Terrain3DErosionSettings:
	return Terrain3DErosionSettings.new()


func set_erosion_settings(_p_terrain: Terrain3D, _p_settings: Terrain3DErosionSettings) -> void:
	pass


func open_erosion_simulator() -> void:
	pass


func open_shader_performance() -> void:
	pass


func open_autoshader_baker() -> void:
	pass



func prepare_region_lightmap_bake() -> void:
	pass


func finalize_region_lightmap_bake() -> void:
	pass


func get_terrain() -> Terrain3D:
	return terrain


func is_terrain_valid(_p_terrain: Terrain3D = null) -> bool:
	return false


func select_terrain() -> void:
	pass


func set_setting(_p_path: String, _p_value: Variant) -> void:
	pass


func get_setting(_p_path: String, p_default: Variant) -> Variant:
	return p_default


func has_setting(_p_path: String) -> bool:
	return false


func erase_setting(_p_path: String) -> void:
	pass

func change_tool(_p_tool_name: String) -> void:
	pass


func notify_ui_setting_changed() -> void:
	pass
