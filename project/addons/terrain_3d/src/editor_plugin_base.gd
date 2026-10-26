# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Typed contract shared by Terrain3D editor-side helpers.
@tool
@abstract
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


@abstract func _read_input(_p_event: InputEvent = null) -> AfterGUIInput

@abstract func get_erosion_settings(_p_terrain: Terrain3D) -> Terrain3DErosionSettings

@abstract func set_erosion_settings(_p_terrain: Terrain3D, _p_settings: Terrain3DErosionSettings) -> void

@abstract func open_erosion_simulator() -> void

@abstract func open_shader_performance() -> void

@abstract func open_terrain_settings() -> void

@abstract func open_runtime_dataset_cook_confirmation() -> void

@abstract func open_autoshader_baker() -> void

@abstract func open_region_lightmap_baker() -> void

@abstract func open_hlod_panel() -> void

@abstract func convert_selected_to_region_streaming() -> void

@abstract func convert_selected_to_hlod_visual(_p_min_level: int = 1) -> void

@abstract func get_region_lightmap_baker() -> Node

@abstract func get_terrain() -> Terrain3D

@abstract func is_terrain_valid(_p_terrain: Terrain3D = null) -> bool

@abstract func select_terrain() -> void

@abstract func set_setting(_p_path: String, _p_value: Variant) -> void

@abstract func get_setting(_p_path: String, _p_default: Variant) -> Variant

@abstract func has_setting(_p_path: String) -> bool

@abstract func erase_setting(_p_path: String) -> void

@abstract func change_tool(_p_tool_name: String) -> void

@abstract func notify_ui_setting_changed() -> void

@abstract func set_instance_edit_mode(_p_enabled: bool) -> void
