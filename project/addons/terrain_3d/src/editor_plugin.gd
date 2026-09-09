# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Editor Plugin for Terrain3D
@tool
extends "res://addons/terrain_3d/src/editor_plugin_base.gd"


# Includes
const Terrain3DUI = preload("res://addons/terrain_3d/src/ui.gd")
const ASSET_DOCK: PackedScene = preload("res://addons/terrain_3d/src/asset_dock.tscn")
const ErosionInspectorPlugin = preload("res://addons/terrain_3d/menu/erosion_inspector_plugin.gd")
const ShaderPerformancePanel = preload("res://addons/terrain_3d/src/shader_performance_panel.gd")
const AutoShaderPanel = preload("res://addons/terrain_3d/src/autoshader_panel.gd")
const StreamingConverter = preload("res://addons/terrain_3d/tools/streaming_converter.gd")
const RegionLightmapBaker = preload("res://addons/terrain_3d/tools/region_lightmap_baker.gd")
const RegionLightmapPanel = preload("res://addons/terrain_3d/tools/region_lightmap_panel.gd")
const HLODBaker = preload("res://addons/terrain_3d/tools/hlod_baker.gd")
const HLODPanel = preload("res://addons/terrain_3d/tools/hlod_panel.gd")
const TOOL_MENU_OPEN: String = "Terrain3D: Open Tools Menu"
const TOOL_MENU_EROSION: String = "Terrain3D: Show Erosion in Inspector"
const TOOL_MENU_SHADER: String = "Terrain3D: Runtime Shader Quality"
const TOOL_MENU_AUTOSHADER: String = "Terrain3D: Material Rules"
const TOOL_MENU_CONVERT_STREAMING: String = "Terrain3D: Convert Selected Objects to Region Streaming"
const TOOL_MENU_LIGHTMAP: String = "Terrain3D: Bake Region Lightmaps"
const TOOL_MENU_HLOD: String = "Terrain3D: HLOD"
const EXPECTED_NATIVE_API_REVISION: int = 2026090601
const EXPECTED_NATIVE_BUILD_ID: String = "memory-budget-zone-containment-p1-2026-09-09"

# Editor Plugin
var ui: Terrain3DUI
var asset_dock: Terrain3DAssetDock
var erosion_inspector_plugin: ErosionInspectorPlugin
var shader_performance_panel: ShaderPerformancePanel
var autoshader_panel: AutoShaderPanel
var streaming_converter: StreamingConverter
var region_lightmap_baker: RegionLightmapBaker
var region_lightmap_panel: RegionLightmapPanel
var hlod_baker: HLODBaker
var hlod_panel: HLODPanel

class ErosionSettingsCacheEntry extends RefCounted:
	var terrain_ref: WeakRef
	var settings: Terrain3DErosionSettings

	func _init(p_terrain: Terrain3D, p_settings: Terrain3DErosionSettings) -> void:
		terrain_ref = weakref(p_terrain)
		settings = p_settings

var erosion_settings_cache: Dictionary[int, ErosionSettingsCacheEntry] = {}

# Terrain
var _last_terrain: Terrain3D

# Input
var _last_modifiers: int = 0
var _use_meta: bool = false
var _native_contract_error_reported: bool = false


func _init() -> void:
	if debug:
		print("Terrain3DEditorPlugin: _init")
	if OS.get_name() == "macOS":
		_use_meta = true


func _enter_tree() -> void:
	if debug:
		print("Terrain3DEditorPlugin: _enter_tree")
	editor = Terrain3DEditor.new()
	setup_editor_settings()
	ui = Terrain3DUI.new()
	ui.plugin = self
	add_child(ui)

	asset_dock = ASSET_DOCK.instantiate() as Terrain3DAssetDock
	asset_dock.initialize(self)
	ui.set_asset_dock(asset_dock)
	ui.toolbar.tool_changed.connect(asset_dock._on_tool_changed)

	# Erosion is embedded in the Terrain3D node Inspector. Registering the inspector
	# plugin here keeps the normal top toolbar independent from the erosion UI.
	erosion_inspector_plugin = ErosionInspectorPlugin.new()
	erosion_inspector_plugin.plugin = self
	add_inspector_plugin(erosion_inspector_plugin)

	shader_performance_panel = ShaderPerformancePanel.new()
	shader_performance_panel.plugin = self
	add_child(shader_performance_panel)
	autoshader_panel = AutoShaderPanel.new()
	autoshader_panel.plugin = self
	add_child(autoshader_panel)
	streaming_converter = StreamingConverter.new()
	streaming_converter.plugin = self
	add_child(streaming_converter)
	region_lightmap_baker = RegionLightmapBaker.new()
	region_lightmap_baker.plugin = self
	add_child(region_lightmap_baker)
	region_lightmap_panel = RegionLightmapPanel.new()
	region_lightmap_panel.visible = false
	region_lightmap_panel.plugin = self
	region_lightmap_panel.baker = region_lightmap_baker
	add_child(region_lightmap_panel)
	hlod_baker = HLODBaker.new()
	hlod_panel = HLODPanel.new()
	hlod_panel.visible = false
	hlod_panel.plugin = self
	hlod_panel.baker = hlod_baker
	add_child(hlod_panel)

	add_tool_menu_item(TOOL_MENU_OPEN, _open_terrain_tools_menu)
	add_tool_menu_item(TOOL_MENU_EROSION, open_erosion_simulator)
	add_tool_menu_item(TOOL_MENU_SHADER, open_shader_performance)
	add_tool_menu_item(TOOL_MENU_AUTOSHADER, open_autoshader_baker)
	add_tool_menu_item(TOOL_MENU_CONVERT_STREAMING, convert_selected_to_region_streaming)
	add_tool_menu_item(TOOL_MENU_LIGHTMAP, open_region_lightmap_baker)
	add_tool_menu_item(TOOL_MENU_HLOD, open_hlod_panel)
	_connect_editor_signals()


func _exit_tree() -> void:
	if debug:
		print("Terrain3DEditorPlugin: _exit_tree")
	_disconnect_editor_signals()
	_detach_current_terrain()
	_last_terrain = null
	remove_tool_menu_item(TOOL_MENU_OPEN)
	remove_tool_menu_item(TOOL_MENU_EROSION)
	remove_tool_menu_item(TOOL_MENU_SHADER)
	remove_tool_menu_item(TOOL_MENU_AUTOSHADER)
	remove_tool_menu_item(TOOL_MENU_CONVERT_STREAMING)
	remove_tool_menu_item(TOOL_MENU_LIGHTMAP)
	remove_tool_menu_item(TOOL_MENU_HLOD)
	if erosion_inspector_plugin:
		remove_inspector_plugin(erosion_inspector_plugin)
		erosion_inspector_plugin = null
	erosion_settings_cache.clear()
	if is_instance_valid(autoshader_panel):
		autoshader_panel.shutdown()
		autoshader_panel.queue_free()
		autoshader_panel = null
	if is_instance_valid(shader_performance_panel):
		shader_performance_panel.queue_free()
		shader_performance_panel = null
	if is_instance_valid(streaming_converter):
		streaming_converter.queue_free()
		streaming_converter = null
	if is_instance_valid(region_lightmap_panel):
		region_lightmap_panel.queue_free()
		region_lightmap_panel = null
	if is_instance_valid(region_lightmap_baker):
		region_lightmap_baker.queue_free()
		region_lightmap_baker = null
	if is_instance_valid(hlod_panel):
		hlod_panel.queue_free()
		hlod_panel = null
	hlod_baker = null
	if is_instance_valid(ui) and is_instance_valid(ui.toolbar) and is_instance_valid(asset_dock):
		if ui.toolbar.tool_changed.is_connected(asset_dock._on_tool_changed):
			ui.toolbar.tool_changed.disconnect(asset_dock._on_tool_changed)
	if is_instance_valid(asset_dock):
		asset_dock.remove_dock(true)
		asset_dock = null
	if is_instance_valid(ui):
		ui.queue_free()
		ui = null
	if is_instance_valid(editor):
		editor.free()
		editor = null


func _connect_editor_signals() -> void:
	# Connect only after all editor helpers are initialized so scene/inspector callbacks
	# cannot observe a partially constructed plugin. Terrain3D deliberately does not
	# subscribe to editor-window focus changes.
	if not scene_changed.is_connected(_on_scene_changed):
		scene_changed.connect(_on_scene_changed)
	# Retained for AssetDock focus handoff only. No focus-entered callback is
	# connected here; Terrain3D has no work to perform merely because Godot regained focus.
	godot_editor_window = EditorInterface.get_base_control().get_window()
	var inspector := EditorInterface.get_inspector()
	if is_instance_valid(inspector) and not inspector.mouse_entered.is_connected(_on_inspector_mouse_entered):
		inspector.mouse_entered.connect(_on_inspector_mouse_entered)


func _disconnect_editor_signals() -> void:
	# Disconnect before teardown so queued editor callbacks cannot access objects
	# that are being removed below.
	if scene_changed.is_connected(_on_scene_changed):
		scene_changed.disconnect(_on_scene_changed)
	var inspector := EditorInterface.get_inspector()
	if is_instance_valid(inspector) and inspector.mouse_entered.is_connected(_on_inspector_mouse_entered):
		inspector.mouse_entered.disconnect(_on_inspector_mouse_entered)
	godot_editor_window = null


func get_erosion_settings(p_terrain: Terrain3D) -> Terrain3DErosionSettings:
	if not is_instance_valid(p_terrain):
		return Terrain3DErosionSettings.new()
	var terrain_id := p_terrain.get_instance_id()
	if erosion_settings_cache.has(terrain_id):
		var entry: ErosionSettingsCacheEntry = erosion_settings_cache[terrain_id]
		var cached_terrain := entry.terrain_ref.get_ref() as Terrain3D if entry.terrain_ref else null
		if cached_terrain == p_terrain and is_instance_valid(entry.settings):
			return entry.settings
	var settings := Terrain3DErosionSettings.new()
	erosion_settings_cache[terrain_id] = ErosionSettingsCacheEntry.new(p_terrain, settings)
	return settings


func set_erosion_settings(p_terrain: Terrain3D, p_settings: Terrain3DErosionSettings) -> void:
	if not is_instance_valid(p_terrain) or not is_instance_valid(p_settings):
		return
	erosion_settings_cache[p_terrain.get_instance_id()] = ErosionSettingsCacheEntry.new(p_terrain, p_settings)


func open_erosion_simulator() -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before opening erosion tools")
		return
	# The erosion workflow is an EditorInspectorPlugin section on Terrain3D.
	# Inspecting the node is the canonical way to reveal it in Godot 4.7.
	EditorInterface.inspect_object(terrain)


func open_shader_performance() -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before opening shader quality controls")
		return
	if is_instance_valid(shader_performance_panel):
		shader_performance_panel.open()


func open_autoshader_baker() -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before opening the autoshader baker")
		return
	if is_instance_valid(autoshader_panel):
		autoshader_panel.set_terrain(terrain)
		autoshader_panel.open()


func convert_selected_to_region_streaming() -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before converting streamed objects")
		return
	if is_instance_valid(streaming_converter):
		streaming_converter.convert_selected()


func convert_selected_to_hlod_visual(min_level: int = 1) -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before converting HLOD visual scenes")
		return
	if is_instance_valid(streaming_converter):
		streaming_converter.convert_selected_as_hlod_visual(min_level)


func open_region_lightmap_baker() -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before baking region lightmaps")
		return
	if is_instance_valid(region_lightmap_panel):
		region_lightmap_panel.open(terrain)


func open_hlod_panel() -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before opening HLOD tools")
		return
	if is_instance_valid(hlod_panel):
		hlod_panel.open(terrain)


func get_region_lightmap_baker() -> Node:
	return region_lightmap_baker



func change_tool(p_tool_name: String) -> void:
	if is_instance_valid(ui) and is_instance_valid(ui.toolbar):
		ui.toolbar.change_tool(p_tool_name)


func notify_ui_setting_changed() -> void:
	if is_instance_valid(ui):
		ui._on_setting_changed()


func _open_terrain_tools_menu() -> void:
	if not is_instance_valid(ui) or not is_instance_valid(ui.terrain_menu):
		return
	# Switch to the 3D editor before opening the spatial toolbar popup. This provides
	# a reliable fallback when a crowded editor layout clips the toolbar button.
	EditorInterface.set_main_screen_editor("3D")
	ui.terrain_menu.show()
	ui.terrain_menu.open_menu.call_deferred()



func _on_inspector_mouse_entered() -> void:
	mouse_in_main = false


## EditorPlugin selection function call chain isn't consistent. Here's the map of calls:
## Assume we handle Terrain3D and NavigationRegion3D
# Click Terrain3D: 					_handles(Terrain3D), _edit(Terrain3D), _make_visible(true)
# Deselect:							_edit(null), _make_visible(false)
# Click other node:					_handles(OtherNode)
# Click NavRegion3D:				_handles(NavReg3D), _edit(NavReg3D), _make_visible(true)
# Click NavRegion3D, Terrain3D:		_handles(Terrain3D), _make_visible(true), _edit(Terrain3D)
# Click Terrain3D, NavRegion3D:		_handles(NavReg3D), _make_visible(true), _edit(NavReg3D)
func _handles(p_object: Object) -> bool:
	if p_object is Terrain3D:
		return true
	elif p_object is NavigationRegion3D and is_instance_valid(_last_terrain):
		return true

	# Terrain3DObjects requires access to EditorUndoRedoManager. The only way to make sure it
	# always has it, is to pass it in here. _edit is NOT called if the node is cut and pasted.
	elif p_object is Terrain3DObjects:
		var objects := p_object as Terrain3DObjects
		objects.editor_setup(self)
	elif p_object is Node3D:
		var node := p_object as Node3D
		var parent := node.get_parent()
		if parent is Terrain3DObjects:
			(parent as Terrain3DObjects).editor_setup(self)

	return false


func _edit(p_object: Object) -> void:
	if !p_object:
		_clear()
		return

	if p_object is Terrain3D:
		_bind_terrain(p_object as Terrain3D)
	else:
		_clear()

	if is_terrain_valid(_last_terrain):
		if p_object is NavigationRegion3D:
			ui.set_visible(true, true)
			nav_region = p_object
		else:
			nav_region = null


func _validate_native_contract(p_terrain: Terrain3D) -> bool:
	if not is_instance_valid(p_terrain):
		return false
	var revision: int = p_terrain.get_native_api_revision()
	var build_id: String = p_terrain.get_native_build_id()
	if revision != EXPECTED_NATIVE_API_REVISION or build_id != EXPECTED_NATIVE_BUILD_ID:
		if not _native_contract_error_reported:
			push_error("Terrain3D native/script revision mismatch. Expected API %d (%s), loaded API %d (%s). Refusing to bind the editor UI to mixed native/script revisions." % [EXPECTED_NATIVE_API_REVISION, EXPECTED_NATIVE_BUILD_ID, revision, build_id])
			_native_contract_error_reported = true
		return false
	_native_contract_error_reported = false
	return true


func _bind_terrain(p_terrain: Terrain3D) -> void:
	if not _validate_native_contract(p_terrain):
		_detach_current_terrain()
		return
	if p_terrain == terrain:
		return
	_detach_current_terrain()
	terrain = p_terrain
	_last_terrain = terrain
	terrain.set_plugin(self)
	terrain.set_editor(editor)
	# Bind the editor camera as soon as the terrain becomes the active edited
	# object. Waiting for _forward_3d_gui_input() means the native renderer can
	# keep the startup origin-centered clipmap until the user first interacts with
	# the viewport.
	var main_viewport := EditorInterface.get_editor_viewport_3d(0)
	if is_instance_valid(main_viewport):
		var main_camera := main_viewport.get_camera_3d()
		if is_instance_valid(main_camera):
			terrain.set_camera(main_camera)
	debug = terrain.debug_level
	editor.set_terrain(terrain)
	terrain.set_meta("_edit_lock_", true)
	ui.set_visible(true)

	# Get alerted when a new asset list is loaded.
	if is_instance_valid(asset_dock):
		if not terrain.assets_changed.is_connected(asset_dock.update_assets):
			terrain.assets_changed.connect(asset_dock.update_assets)
		asset_dock.update_assets()
	if is_instance_valid(autoshader_panel):
		autoshader_panel.set_terrain(terrain)


func _make_visible(p_visible: bool) -> void:
	if debug:
		print("Terrain3DEditorPlugin: _make_visible(%s)" % p_visible)
	if not is_instance_valid(ui):
		return
	if not p_visible:
		if is_instance_valid(editor) and editor.is_operating():
			editor.stop_operation()
		ui.cancel_landform_stroke()
		ui.clear_picking()
	if p_visible and is_selected():
		ui.set_visible(true)
		if is_instance_valid(asset_dock):
			asset_dock.update_dock()
	else:
		ui.set_visible(false)


func _clear() -> void:
	if is_instance_valid(ui):
		ui.cancel_landform_stroke()
	_detach_current_terrain()


func _detach_current_terrain() -> void:
	nav_region = null
	viewport = null
	_input_mode = 0
	if is_instance_valid(ui):
		ui.clear_picking()
	if is_instance_valid(editor):
		if editor.is_operating():
			editor.stop_operation()
		editor.set_tool(int(Terrain3DEditor.TOOL_MAX))
		editor.set_operation(int(Terrain3DEditor.OP_MAX))
		editor.set_terrain(null)

	var previous := terrain
	terrain = null
	if is_instance_valid(autoshader_panel):
		autoshader_panel.set_terrain(null)
	if not is_instance_valid(previous):
		return

	if is_instance_valid(asset_dock) and previous.assets_changed.is_connected(asset_dock.update_assets):
		previous.assets_changed.disconnect(asset_dock.update_assets)
	previous.set_editor(null)
	previous.set_plugin(null)


func _forward_3d_gui_input(p_viewport_camera: Camera3D, p_event: InputEvent) -> AfterGUIInput:
	mouse_in_main = true
	if not is_terrain_valid():
		return AFTER_GUI_INPUT_PASS

	# Camera/navigation input can return PASS from _read_input(). Hand the active
	# viewport camera to Terrain3D before that early return so a terrain whose
	# physics snapping was disabled during startup can recover without a brush edit.
	terrain.set_camera(p_viewport_camera)

	var continue_input: AfterGUIInput = _read_input(p_event)
	if continue_input != AFTER_GUI_INPUT_CUSTOM:
		return continue_input
	## Setup active camera & viewport
	# Always update this for all inputs, as the mouse position can move without
	# necessarily being a InputEventMouseMotion object. get_intersection() also
	# returns the last frame position, and should be updated more frequently.

	# Detect if viewport is set to half_resolution
	# Structure is: Node3DEditorViewportContainer/Node3DEditorViewport(4)/SubViewportContainer/SubViewport/Camera3D
	viewport = p_viewport_camera.get_viewport() as SubViewport
	var viewport_container := viewport.get_parent() as SubViewportContainer
	var full_resolution: bool = not is_instance_valid(viewport_container) or viewport_container.stretch_shrink != 2

	## Get mouse location on terrain
	# Project 2D mouse position to 3D position and direction
	var vp_mouse_pos: Vector2 = viewport.get_mouse_position()
	var mouse_pos: Vector2 = vp_mouse_pos if full_resolution else vp_mouse_pos / 2
	var camera_pos: Vector3 = p_viewport_camera.project_ray_origin(mouse_pos)
	var camera_dir: Vector3 = p_viewport_camera.project_ray_normal(mouse_pos)

	ui.update_decal()

	# If region tool, grab mouse position without considering height
	if editor.get_tool() == Terrain3DEditor.REGION:
		var t: float = -Vector3(0, 1, 0).dot(camera_pos) / Vector3(0, 1, 0).dot(camera_dir)
		mouse_global_position = (camera_pos + t * camera_dir)
	else:
	#Else look for intersection with terrain
		var intersection_point: Vector3 = terrain.get_intersection(camera_pos, camera_dir, true)
		if intersection_point.z > 3.4e38 or is_nan(intersection_point.y): # max double or nan
			return AFTER_GUI_INPUT_PASS
		mouse_global_position = intersection_point

	## Handle mouse movement
	if p_event is InputEventMouseMotion:
		var mouse_motion := p_event as InputEventMouseMotion

		if _input_mode != -1: # Not cam rotation
			## Update region highlight
			var region_position: Vector2 = ( Vector2(mouse_global_position.x, mouse_global_position.z) \
				/ (terrain.get_region_size() * terrain.get_vertex_spacing()) ).floor()

			if _input_mode > 0 and editor.get_tool() == Terrain3DEditor.LANDFORM:
				ui.append_landform_stroke(mouse_global_position)
				return AFTER_GUI_INPUT_STOP

			if _input_mode > 0 and editor.is_operating():
				# Inject pressure - Relies on C++ set_brush_data() using same dictionary instance
				ui.brush_data["mouse_pressure"] = mouse_motion.pressure

				editor.operate(mouse_global_position, p_viewport_camera.rotation.y)
				return AFTER_GUI_INPUT_STOP

		return AFTER_GUI_INPUT_PASS

	if p_event is InputEventMouseButton and _input_mode > 0:
		if p_event.is_pressed():
			if editor.get_tool() == Terrain3DEditor.LANDFORM:
				ui.begin_landform_stroke(mouse_global_position)
				return AFTER_GUI_INPUT_STOP

			# If picking
			if ui.is_picking():
				ui.pick(mouse_global_position)
				if not ui.operation_builder or not ui.operation_builder.is_ready():
					return AFTER_GUI_INPUT_STOP

			if modifier_ctrl and editor.get_tool() == Terrain3DEditor.HEIGHT:
				var height: float = terrain.data.get_height(mouse_global_position)
				ui.brush_data["height"] = height
				ui.tool_settings.set_setting("height", height)

			# If adjusting regions
			if editor.get_tool() == Terrain3DEditor.REGION:
				# Skip regions that already exist or don't
				var has_region: bool = terrain.data.has_regionp(mouse_global_position)
				var op: int = editor.get_operation()
				if	( has_region and op == Terrain3DEditor.ADD) or \
					( not has_region and op == Terrain3DEditor.SUBTRACT ):
					return AFTER_GUI_INPUT_STOP

			# If an automatic operation is ready to go (e.g. gradient)
			if ui.operation_builder and ui.operation_builder.is_ready():
				ui.operation_builder.apply_operation(editor, mouse_global_position, p_viewport_camera.rotation.y)
				return AFTER_GUI_INPUT_STOP

			# Mouse clicked, start editing
			editor.start_operation(mouse_global_position)
			editor.operate(mouse_global_position, p_viewport_camera.rotation.y)
			return AFTER_GUI_INPUT_STOP

		# _input_apply released, save undo data
		elif editor.get_tool() == Terrain3DEditor.LANDFORM:
			ui.finish_landform_stroke(mouse_global_position)
			return AFTER_GUI_INPUT_STOP
		elif editor.is_operating():
			editor.stop_operation()
			return AFTER_GUI_INPUT_STOP

	return AFTER_GUI_INPUT_PASS


func _read_input(p_event: InputEvent = null) -> AfterGUIInput:
	## Determine if user is moving camera or applying.
	var mouse_button: InputEventMouseButton = p_event as InputEventMouseButton
	var released_button: int = MOUSE_BUTTON_NONE
	if mouse_button and not mouse_button.pressed:
		released_button = mouse_button.button_index

	if (
		Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
		or released_button == MOUSE_BUTTON_LEFT
	):
		_input_mode = 1
	else:
		_input_mode = 0

	match get_setting("editors/3d/navigation/navigation_scheme", 0):
		2, 1: # Modo, Maya
			if (
				Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)
				or (
					Input.is_key_pressed(KEY_ALT)
					and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
				)
			):
				_input_mode = -1
			if (
				released_button == MOUSE_BUTTON_RIGHT
				or (
					Input.is_key_pressed(KEY_ALT)
					and released_button == MOUSE_BUTTON_LEFT
				)
			):
				rmb_release_time = Time.get_ticks_msec()
		0, _: # Godot
			if (
				Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)
				or Input.is_mouse_button_pressed(MOUSE_BUTTON_MIDDLE)
			):
				_input_mode = -1
			if released_button in [MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE]:
				rmb_release_time = Time.get_ticks_msec()
	if _input_mode < 0:
		return AFTER_GUI_INPUT_PASS

	# The left-button release can occur outside the 3D viewport. Recover the
	# transaction as soon as input/focus returns so edited flags and undo state do
	# not leak into the next stroke. A real left release keeps _input_mode at 1 and
	# is finalized by _forward_3d_gui_input() below.
	if _input_mode == 0 and is_instance_valid(editor):
		if editor.is_operating() and is_terrain_valid():
			editor.stop_operation()
		if editor.get_tool() == Terrain3DEditor.LANDFORM and is_instance_valid(ui):
			ui.finish_landform_stroke(mouse_global_position)

	## Determine modifiers pressed.
	modifier_shift = Input.is_key_pressed(KEY_SHIFT)

	# Editor responds to modifier_ctrl so we must register touchscreen Invert.
	var ui_inverted: bool = is_instance_valid(ui) and ui.is_input_inverted()
	if _use_meta:
		modifier_ctrl = Input.is_key_pressed(KEY_META) or ui_inverted
	else:
		modifier_ctrl = Input.is_key_pressed(KEY_CTRL) or ui_inverted

	# Keybind enum: Alt, Space, Meta, Caps Lock.
	var alt_key: int
	match get_setting("terrain3d/config/alt_key_bind", 0):
		3: alt_key = KEY_CAPSLOCK
		2: alt_key = KEY_META
		1: alt_key = KEY_SPACE
		0, _: alt_key = KEY_ALT
	modifier_alt = Input.is_key_pressed(alt_key)
	var current_mods: int = (
		int(modifier_shift)
		| int(modifier_ctrl) << 1
		| int(modifier_alt) << 2
	)

	## Process hotkeys.
	var key_event: InputEventKey = p_event as InputEventKey
	if (
		key_event
		and current_mods == 0
		and key_event.pressed
		and consume_hotkey(key_event)
	):
		EditorInterface.get_editor_viewport_3d(0).set_input_as_handled()
		return AFTER_GUI_INPUT_STOP

	# Brush data is cleared on set_tool, or clicking textures in the asset dock.
	# Update modifiers if changed or missing.
	if _last_modifiers != current_mods or not ui.brush_data.has("modifier_shift"):
		_last_modifiers = current_mods
		ui.brush_data["modifier_shift"] = modifier_shift
		ui.brush_data["modifier_ctrl"] = modifier_ctrl
		ui.brush_data["modifier_alt"] = modifier_alt
		ui.set_active_operation()

	return AFTER_GUI_INPUT_CUSTOM


# Returns true if hotkey matches and operation triggered
func consume_hotkey(p_event: InputEventKey) -> bool:
	# Handle repeatable keys
	match p_event.keycode:
		KEY_BRACKETLEFT:
			ui.tool_settings.set_setting("size", ui.tool_settings.get_setting("size") - 1)
			return true
		KEY_BRACKETRIGHT:
			ui.tool_settings.set_setting("size", ui.tool_settings.get_setting("size") + 1)
			return true
		KEY_MINUS:
			ui.tool_settings.set_setting("strength", ui.tool_settings.get_setting("strength") - 1)
			return true
		KEY_EQUAL:
			ui.tool_settings.set_setting("strength", ui.tool_settings.get_setting("strength") + 1)
			return true

	if p_event.is_echo():
		return false

	# Handle non-repeatable keys
	match p_event.keycode:
		KEY_ESCAPE:
			if editor.get_tool() == Terrain3DEditor.LANDFORM:
				ui.cancel_landform_stroke()
				return true
			return false
		KEY_1, KEY_KP_1:
			terrain.material.set_show_region_grid(!terrain.material.get_show_region_grid())
		KEY_2, KEY_KP_2:
			terrain.label_distance = 4096.0 if is_zero_approx(terrain.label_distance) else 0.0
		KEY_3, KEY_KP_3:
			terrain.material.set_show_contours(!terrain.material.get_show_contours())
		KEY_4, KEY_KP_4:
			terrain.material.set_show_instancer_grid(!terrain.material.get_show_instancer_grid())
		KEY_5, KEY_KP_5:
			terrain.material.set_show_vertex_grid(!terrain.material.get_show_vertex_grid())
		KEY_E:
			ui.toolbar.change_tool("AddRegion")
		KEY_R:
			ui.toolbar.change_tool("Raise")
		KEY_H:
			ui.toolbar.change_tool("Height")
		KEY_S:
			ui.toolbar.change_tool("Slope")
		KEY_L:
			ui.toolbar.change_tool("LandformStroke")
		KEY_C:
			ui.toolbar.change_tool("PaintColor")
		KEY_N:
			ui.toolbar.change_tool("PaintNavigableArea")
		KEY_I:
			ui.toolbar.change_tool("InstanceMeshes")
		KEY_X:
			ui.toolbar.change_tool("AddHoles")
		KEY_W:
			ui.toolbar.change_tool("PaintWetness")
		KEY_B:
			ui.toolbar.change_tool("PaintTexture")
		KEY_V:
			ui.toolbar.change_tool("SprayTexture")
		KEY_A:
			ui.toolbar.change_tool("EnableMaterialRules")
		KEY_T:
			ui.tool_settings.inverse_slope_range()
		_:
			return false
	return true


func _on_scene_changed(scene_root: Node) -> void:
	if debug:
		print("Terrain3DEditorPlugin: _on_scene_changed: ", scene_root)
	if not scene_root:
		_clear()
		_last_terrain = null
		if is_instance_valid(asset_dock):
			asset_dock.update_assets()
		return

	# Scene tabs retain resources and sometimes nodes longer than the visible
	# viewport lifetime. Never carry the active editor binding into another
	# scene root; otherwise both Terrain3D nodes retain the same native editor
	# object and signal destinations.
	if is_instance_valid(terrain) and terrain != scene_root and not scene_root.is_ancestor_of(terrain):
		_clear()
	if is_instance_valid(_last_terrain) and _last_terrain != scene_root and \
			not scene_root.is_ancestor_of(_last_terrain):
		_last_terrain = null

	for node: Node in scene_root.find_children("", "Terrain3DObjects"):
		var objects := node as Terrain3DObjects
		if is_instance_valid(objects):
			objects.editor_setup(self)

	if is_instance_valid(asset_dock):
		asset_dock.update_assets()


func get_terrain() -> Terrain3D:
	if is_terrain_valid():
		return terrain
	elif is_instance_valid(_last_terrain) and is_terrain_valid(_last_terrain):
		return _last_terrain
	else:
		return null


func is_terrain_valid(p_terrain: Terrain3D = null) -> bool:
	var t: Terrain3D
	if p_terrain:
		t = p_terrain
	else:
		t = terrain
	var scene_root := EditorInterface.get_edited_scene_root()
	var belongs_to_active_scene := is_instance_valid(t) and is_instance_valid(scene_root) and \
		(t == scene_root or scene_root.is_ancestor_of(t))
	if belongs_to_active_scene and t.is_inside_tree() and t.data:
		return true
	return false


func is_selected() -> bool:
	var selected: Array[Node] = EditorInterface.get_selection().get_selected_nodes()
	for node: Node in selected:
		if ( is_instance_valid(_last_terrain) and node.get_instance_id() == _last_terrain.get_instance_id() ) or \
			node is Terrain3D:
			return true
	return false


func select_terrain() -> void:
	if debug and is_selected():
		print("Terrain3DEditorPlugin: Terrain is selected, skipping")
	if is_instance_valid(_last_terrain) and is_terrain_valid(_last_terrain) and not is_selected():
		var es: EditorSelection = EditorInterface.get_selection()
		if debug:
			print("Terrain3DEditorPlugin: Clearing and reselecting terrain")
		es.clear()
		es.add_node(_last_terrain)


## Editor Settings


func setup_editor_settings() -> void:
	editor_settings = EditorInterface.get_editor_settings()
	if not editor_settings.has_setting("terrain3d/config/alt_key_bind"):
		editor_settings.set_setting("terrain3d/config/alt_key_bind", 0)
	var property_info: Dictionary[String, Variant] = {
		"name": "terrain3d/config/alt_key_bind",
		"type": TYPE_INT,
		"hint": PROPERTY_HINT_ENUM,
		"hint_string": "Alt,Space,Meta,Capslock"
	}
	editor_settings.add_property_info(property_info)


func set_setting(p_str: String, p_value: Variant) -> void:
	editor_settings.set_setting(p_str, p_value)


func get_setting(p_str: String, p_default: Variant) -> Variant:
	if editor_settings.has_setting(p_str):
		return editor_settings.get_setting(p_str)
	else:
		return p_default


func has_setting(p_str: String) -> bool:
	return editor_settings.has_setting(p_str)


func erase_setting(p_str: String) -> void:
	editor_settings.erase(p_str)


## Undo / Redo Functions


func create_undo_action(p_action_name: String) -> void:
	get_undo_redo().create_action(p_action_name, UndoRedo.MERGE_DISABLE, terrain)


func add_undo_method(p_method: Callable) -> void:
	var args: Array[Variant] = [ p_method.get_object(), p_method.get_method() ]
	args.append_array(p_method.get_bound_arguments())
	get_undo_redo().add_undo_method.callv(args)


func add_do_method(p_method: Callable) -> void:
	var args: Array[Variant] = [ p_method.get_object(), p_method.get_method() ]
	args.append_array(p_method.get_bound_arguments())
	get_undo_redo().add_do_method.callv(args)


func commit_action(p_execute: bool) -> void:
	get_undo_redo().commit_action(p_execute)
