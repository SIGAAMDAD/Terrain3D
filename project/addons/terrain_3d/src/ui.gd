# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# UI for Terrain3D
extends Node


# Includes
const TerrainMenu = preload("res://addons/terrain_3d/menu/terrain_menu.gd")
const EXECUTION_MODE_MENU_PATH: String = "res://addons/terrain_3d/src/execution_mode_menu.gd"
const TerrainToolbar = preload("res://addons/terrain_3d/src/toolbar.gd")
const TerrainToolSettings = preload("res://addons/terrain_3d/src/tool_settings.gd")
const OperationBuilder = preload("res://addons/terrain_3d/src/operation_builder.gd")
const GradientOperationBuilder = preload("res://addons/terrain_3d/src/gradient_operation_builder.gd")
const LandformController = preload("res://addons/terrain_3d/src/landform_controller.gd")

# Decal colors
const COLOR_RAISE := Color(1., 1., 1.) # White
const COLOR_LOWER := Color(0.2, 0.2, 0.2) # Dark gray
const COLOR_SMOOTH := Color(0.5, 0.0, 0.2) # Dark Red
const COLOR_AVERAGE := Color(0.6, 0.1, 0.3) # Neutral purple
const COLOR_LIFT := Color(1.0, 0.6, 0.0) # Bright orange
const COLOR_FLATTEN := Color(0.0, 0.6, 1.0) # Cyan
const COLOR_HEIGHT := Color(0.0, 0.8, 0.8) # Brighter cyan
const COLOR_SLOPE := Color(1.0, 1.0, 0.0) # Bright yellow
const COLOR_PAINT := Color(0.0, 0.5, 0.0) # Dark green
const COLOR_SPRAY := Color(0.4, 0.8, 0.4) # Lighter green
const COLOR_UNSPRAY := Color(0.5, 0.2, 0.5) # Neutral purple
const COLOR_WET := Color(0.4, 0.6, 1.0) # Light blue
const COLOR_DRY := Color(0.6, 0.4, 0.0) # Warm brown
const COLOR_AUTOSHADER := Color(0.36, 0.2, 0.09) # Chocolate
const COLOR_HOLES := Color(0.1, 0.1, 0.1) # Near-black
const COLOR_NAVIGATION := Color(0.5, 0.2, 0.5) # Purple
const COLOR_INSTANCE := Color(0.863, 0.08, 0.235) # Crimson
const COLOR_UNINSTANCE := Color(0.2, 0.9, 0.6) # Cyan-green
const COLOR_PICK := Color.WHITE
const COLOR_LANDFORM := Color(1.0, 0.48, 0.08)

const OP_NONE: int = 0x0
const OP_POSITIVE_ONLY: int = 0x01
const OP_NEGATIVE_ONLY: int = 0x02

@onready var region_texture := ImageTexture.new() :
	set(value):
		var image: Image = Image.create_empty(1, 1, false, Image.FORMAT_R8)
		image.fill(Color.WHITE)
		value.create_from_image(image)
		region_texture = value


var plugin: Terrain3DEditorPluginBase
var asset_dock: Terrain3DAssetDock
var toolbar: TerrainToolbar
var tool_settings: TerrainToolSettings
var terrain_menu: TerrainMenu
var execution_mode_menu: Terrain3DExecutionModeMenuBase
var setting_has_changed: bool = false
var visible: bool = false
var picking: int = Terrain3DEditor.TOOL_MAX
var picking_callback: Callable
var brush_data: Dictionary[StringName, Variant] = {}
var operation_builder: OperationBuilder
var landform_controller: LandformController
var active_tool: int = int(Terrain3DEditor.TOOL_MAX)
var _selected_tool: int = int(Terrain3DEditor.TOOL_MAX)
var active_operation: int = int(Terrain3DEditor.OP_MAX)
var _selected_operation: int = int(Terrain3DEditor.OP_MAX)
var _touchscreen_invert_active: bool = false

# 3 Editor decals: 0 = cursor, 1 = slope point1, 2 = slope point2
var mat_rid: RID
var editor_brush_texture_rid: RID = RID()
var editor_decal_position: Array[Vector2] = [Vector2(), Vector2(), Vector2()]
var editor_decal_rotation: Array[float] = [0., 0., 0.]
var editor_decal_size: Array[float] = [0., 0., 0.]
var editor_decal_color: Array[Color] = [Color(), Color(), Color()]
var editor_decal_visible: Array[bool] = [false, false, false]
var editor_decal_part: Array[bool] = [true, true] # Decal[0] cursor components: brush, reticle
var editor_decal_timer: Timer
var editor_decal_fade: float :
	set(value):
		editor_decal_fade = value
		if editor_decal_color.size() > 0:
			editor_decal_color[0].a = value
			if is_shader_valid():
				RenderingServer.material_set_param(mat_rid, "_editor_decal_color", editor_decal_color)
				if value < 0.001:
					var r_map: PackedInt32Array = plugin.terrain.data.get_region_map()
					RenderingServer.material_set_param(mat_rid, "_region_map", r_map)


func _enter_tree() -> void:
	if plugin.debug:
		print("Terrain3DUI: _enter_tree()")

	toolbar = TerrainToolbar.new()
	toolbar.plugin = plugin
	toolbar.hide()
	toolbar.tool_changed.connect(_on_tool_changed)

	tool_settings = TerrainToolSettings.new()
	tool_settings.setting_changed.connect(_on_setting_changed)
	tool_settings.picking.connect(_on_picking)
	tool_settings.plugin = plugin
	tool_settings.hide()

	terrain_menu = TerrainMenu.new()
	terrain_menu.plugin = plugin
	terrain_menu.hide()

	landform_controller = LandformController.new()
	landform_controller.plugin = plugin
	landform_controller.tool_settings = tool_settings
	add_child(landform_controller)

	plugin.add_control_to_container(EditorPlugin.CONTAINER_SPATIAL_EDITOR_SIDE_LEFT, toolbar)
	plugin.add_control_to_container(EditorPlugin.CONTAINER_SPATIAL_EDITOR_BOTTOM, tool_settings)
	plugin.add_control_to_container(EditorPlugin.CONTAINER_SPATIAL_EDITOR_MENU, terrain_menu)

	_on_tool_changed(Terrain3DEditor.REGION, Terrain3DEditor.ADD)

	editor_decal_timer = Timer.new()
	editor_decal_timer.wait_time = .5
	editor_decal_timer.one_shot = true
	editor_decal_timer.timeout.connect(func() -> void:
		get_tree().create_tween().tween_property(self, "editor_decal_fade", 0.0, 0.15))
	add_child(editor_decal_timer)


func set_asset_dock(p_asset_dock: Terrain3DAssetDock) -> void:
	asset_dock = p_asset_dock
	if is_instance_valid(tool_settings):
		tool_settings.asset_dock = p_asset_dock
	if is_instance_valid(landform_controller):
		landform_controller.asset_dock = p_asset_dock


func _exit_tree() -> void:
	if plugin.debug:
		print("Terrain3DUI: _exit_tree()")
	plugin.remove_control_from_container(EditorPlugin.CONTAINER_SPATIAL_EDITOR_SIDE_LEFT, toolbar)
	plugin.remove_control_from_container(EditorPlugin.CONTAINER_SPATIAL_EDITOR_BOTTOM, tool_settings)
	plugin.remove_control_from_container(EditorPlugin.CONTAINER_SPATIAL_EDITOR_MENU, terrain_menu)
	if is_instance_valid(execution_mode_menu):
		plugin.remove_control_from_container(EditorPlugin.CONTAINER_SPATIAL_EDITOR_MENU, execution_mode_menu)
	if landform_controller:
		landform_controller.cancel_stroke()
		landform_controller.queue_free()
	toolbar.queue_free()
	tool_settings.queue_free()
	terrain_menu.queue_free()
	if is_instance_valid(execution_mode_menu):
		execution_mode_menu.queue_free()
		execution_mode_menu = null
	editor_decal_timer.queue_free()


func _ensure_execution_mode_menu() -> Terrain3DExecutionModeMenuBase:
	if is_instance_valid(execution_mode_menu):
		return execution_mode_menu
	var menu_script: GDScript = load(EXECUTION_MODE_MENU_PATH) as GDScript
	if menu_script == null:
		push_error("Unable to load Terrain3D execution-mode menu")
		return null
	execution_mode_menu = menu_script.new() as Terrain3DExecutionModeMenuBase
	if not is_instance_valid(execution_mode_menu):
		push_error("Unable to instantiate Terrain3D execution-mode menu")
		return null
	execution_mode_menu.plugin = plugin
	execution_mode_menu.hide()
	plugin.add_control_to_container(EditorPlugin.CONTAINER_SPATIAL_EDITOR_MENU, execution_mode_menu)
	return execution_mode_menu


func set_visible(p_visible: bool, p_menu_only: bool = false) -> void:
	if plugin.debug:
		print("Terrain3DUI: set_visible(%s, %s)" % [ p_visible, p_menu_only ])

	terrain_menu.set_visible(p_visible)
	if p_visible and not p_menu_only:
		var mode_menu: Terrain3DExecutionModeMenuBase = _ensure_execution_mode_menu()
		if is_instance_valid(mode_menu):
			mode_menu.set_visible(true)
			mode_menu.set_terrain(plugin.terrain)
	elif is_instance_valid(execution_mode_menu):
		execution_mode_menu.set_visible(false)
		execution_mode_menu.set_terrain(null)

	if p_menu_only:
		toolbar.set_visible(false)
		tool_settings.set_visible(false)
	else:
		visible = p_visible
		toolbar.set_visible(p_visible)
		tool_settings.set_visible(p_visible)

	if is_instance_valid(plugin.editor) and is_instance_valid(plugin.terrain) and p_visible:
		var terrain_at_request: Terrain3D = plugin.terrain
		await get_tree().process_frame # Editor selection may change while this is suspended.
		if not is_instance_valid(plugin) or not is_instance_valid(plugin.editor):
			return
		if not is_instance_valid(plugin.terrain) or plugin.terrain != terrain_at_request:
			return
		if plugin.debug:
			print("Terrain3DUI: set_visible: calling _on_tool_changed()")
		_on_tool_changed(_selected_tool, _selected_operation)
		if _selected_tool in [ Terrain3DEditor.REGION, Terrain3DEditor.NAVIGATION ] and \
				is_instance_valid(terrain_at_request.material) and \
				is_instance_valid(terrain_at_request.data) and \
				terrain_at_request.is_initialized() and terrain_at_request.data.is_runtime_ready():
			terrain_at_request.material.update_editor_overlay()


func set_menu_visibility(p_list: Control, p_visible: bool) -> void:
	if is_instance_valid(tool_settings):
		tool_settings.set_submenu_visible(p_list, p_visible)


func _on_tool_changed(p_tool: int, p_operation: int) -> void:
	if plugin.debug:
		print("Terrain3DUI: _on_tool_changed: ", p_tool, ", ", p_operation)
	if _selected_tool == p_tool and _selected_operation == p_operation:
		# Terrain/scene detachment resets native state while the UI intentionally
		# retains its selection. Refresh brush data as well so the selected asset and
		# terrain-specific settings cannot leak from the previously edited terrain.
		_on_setting_changed()
		return
	# A native stroke owns one map/instance undo transaction. Changing tools while
	# the mouse is still down can otherwise move the same transaction to a second
	# map type without capturing a second original snapshot.
	if plugin.editor and plugin.editor.is_operating() and \
			(_selected_tool != p_tool or _selected_operation != p_operation):
		plugin.editor.stop_operation()
	if landform_controller and _selected_tool == Terrain3DEditor.LANDFORM and p_tool != Terrain3DEditor.LANDFORM:
		landform_controller.cancel_stroke()
	_selected_tool = p_tool
	_selected_operation = p_operation
	var context: PackedStringArray = _get_tool_context(p_tool, p_operation)
	tool_settings.set_context(context[0], context[1])
	tool_settings.set_context_action()
	clear_picking()
	set_menu_visibility(tool_settings.advanced_list, true)
	set_menu_visibility(tool_settings.scale_list, false)
	set_menu_visibility(tool_settings.rotation_list, false)
	set_menu_visibility(tool_settings.height_list, false)
	set_menu_visibility(tool_settings.color_list, false)
	set_menu_visibility(tool_settings.collision_list, false)
	set_menu_visibility(tool_settings.landform_shape_list, false)
	set_menu_visibility(tool_settings.landform_surface_list, false)
	set_menu_visibility(tool_settings.landform_mesh_list, false)

	# Select which settings to show. Options in tool_settings.gd:_ready
	var to_show: PackedStringArray = []

	match _selected_tool:
		Terrain3DEditor.REGION:
			to_show.push_back("instructions")
			to_show.push_back("invert")
			set_menu_visibility(tool_settings.advanced_list, false)

		Terrain3DEditor.SCULPT:
			to_show.push_back("brush")
			to_show.push_back("size")
			to_show.push_back("strength")
			if _selected_operation in [Terrain3DEditor.ADD, Terrain3DEditor.SUBTRACT]:
				to_show.push_back("invert")
			elif _selected_operation == Terrain3DEditor.GRADIENT:
				to_show.push_back("gradient_points")
				to_show.push_back("drawable")

		Terrain3DEditor.HEIGHT:
			to_show.push_back("brush")
			to_show.push_back("size")
			to_show.push_back("strength")
			to_show.push_back("height")
			to_show.push_back("height_picker")
			to_show.push_back("invert")

		Terrain3DEditor.TEXTURE:
			to_show.push_back("brush")
			to_show.push_back("size")
			to_show.push_back("enable_texture")
			to_show.push_back("texture_picker")
			if _selected_operation == Terrain3DEditor.ADD:
				to_show.push_back("strength")
				to_show.push_back("invert")
			to_show.push_back("slope")
			to_show.push_back("enable_angle")
			to_show.push_back("angle")
			to_show.push_back("angle_picker")
			to_show.push_back("dynamic_angle")
			to_show.push_back("enable_scale")
			to_show.push_back("scale")
			to_show.push_back("scale_picker")

		Terrain3DEditor.COLOR:
			to_show.push_back("brush")
			to_show.push_back("size")
			to_show.push_back("strength")
			to_show.push_back("color")
			to_show.push_back("color_picker")
			to_show.push_back("slope")
			to_show.push_back("texture_filter")
			to_show.push_back("margin")
			to_show.push_back("invert")

		Terrain3DEditor.ROUGHNESS:
			to_show.push_back("brush")
			to_show.push_back("size")
			to_show.push_back("strength")
			to_show.push_back("roughness")
			to_show.push_back("roughness_picker")
			to_show.push_back("slope")
			to_show.push_back("texture_filter")
			to_show.push_back("margin")
			to_show.push_back("invert")

		Terrain3DEditor.AUTOSHADER:
			to_show.push_back("brush")
			to_show.push_back("size")
			to_show.push_back("invert")
			tool_settings.set_context_action(
				"Material Rules…",
				"Configure expanded editor rules, preview them, and bake production control maps.",
				plugin.open_autoshader_baker,
			)

		Terrain3DEditor.HOLES, Terrain3DEditor.NAVIGATION:
			to_show.push_back("brush")
			to_show.push_back("size")
			to_show.push_back("invert")

		Terrain3DEditor.LANDFORM:
			tool_settings.set_context_action(
				"Rebuild Selected Geometry",
				"Regenerate a selected landform from its stored stroke using the current cliff-geometry settings.",
				_regenerate_selected_landform,
			)
			to_show.push_back("landform_mode")
			to_show.push_back("landform_width")
			to_show.push_back("landform_amplitude")
			to_show.push_back("landform_strength")
			set_menu_visibility(tool_settings.landform_shape_list, true)
			set_menu_visibility(tool_settings.landform_surface_list, true)
			set_menu_visibility(tool_settings.landform_mesh_list, true)
			for setting_name: String in [
				"landform_falloff", "landform_crest", "landform_asymmetry", "landform_cliff_side",
				"landform_terrace_step",
				"landform_road_crown", "landform_water_level", "landform_shore_slope",
				"landform_dune_spacing", "landform_repose", "landform_iterations",
				"landform_morphology", "landform_path_spacing", "landform_noise",
				"landform_noise_scale", "landform_seed", "landform_paint_material",
				"landform_overlay_texture", "landform_material_blend", "landform_generate_mesh",
				"landform_geometry_mode", "landform_mesh_depth", "landform_mesh_irregularity",
				"landform_mesh_spacing", "landform_mesh_lip", "landform_mesh_toe",
				"landform_module_length", "landform_module_corner_angle",
				"landform_module_min_scale", "landform_module_max_scale",
				"landform_collision_spacing", "landform_generate_collision"
			]:
				to_show.push_back(setting_name)
			for prefix: String in ["landform_module_mesh_", "landform_corner_mesh_", "landform_end_mesh_"]:
				var count: int = 4 if prefix == "landform_module_mesh_" else 2
				for index: int in range(count):
					to_show.push_back("%s%d" % [prefix, index])
			set_menu_visibility(tool_settings.advanced_list, true)

		Terrain3DEditor.INSTANCER:
			to_show.push_back("size")
			to_show.push_back("strength")
			to_show.push_back("slope")
			to_show.push_back("mesh_picker")
			set_menu_visibility(tool_settings.height_list, true)
			to_show.push_back("height_offset")
			to_show.push_back("random_height")
			set_menu_visibility(tool_settings.scale_list, true)
			to_show.push_back("fixed_scale")
			to_show.push_back("random_scale")
			set_menu_visibility(tool_settings.rotation_list, true)
			to_show.push_back("fixed_spin")
			to_show.push_back("random_spin")
			to_show.push_back("fixed_tilt")
			to_show.push_back("random_tilt")
			to_show.push_back("align_to_normal")
			set_menu_visibility(tool_settings.color_list, true)
			to_show.push_back("vertex_color")
			to_show.push_back("random_darken")
			to_show.push_back("random_hue")
			set_menu_visibility(tool_settings.collision_list, true)
			to_show.push_back("on_collision")
			to_show.push_back("raycast_height")
			to_show.push_back("invert")

		_:
			pass

	# Advanced menu settings
	to_show.push_back("auto_regions")
	if _selected_tool != Terrain3DEditor.LANDFORM:
		to_show.push_back("align_to_view")
		to_show.push_back("show_brush_texture")
		to_show.push_back("gamma")
		to_show.push_back("brush_spin_speed")
	tool_settings.show_settings(to_show)
	_refresh_landform_mode_visibility()

	if plugin.debug:
		print("Terrain3DUI: _on_tool_changed: calling _on_setting_changed()")
	_on_setting_changed()


func _on_setting_changed() -> void:
	if plugin.debug:
		print("Terrain3DUI: _on_setting_changed")
	if not asset_dock: # Skip function if not _ready()
		return
	var previous_touchscreen_invert_active: bool = _touchscreen_invert_active
	brush_data = tool_settings.get_settings()
	_refresh_landform_mode_visibility()
	match _selected_tool:
		Terrain3DEditor.TEXTURE, Terrain3DEditor.COLOR, Terrain3DEditor.ROUGHNESS:
			brush_data["asset_id"] = asset_dock.texture_list.get_selected_asset_id()
		Terrain3DEditor.INSTANCER:
			brush_data["asset_id"] = asset_dock.mesh_list.get_selected_asset_id()
		_:
			brush_data["asset_id"] = -1
	if plugin.debug:
		print("Terrain3DUI: _on_setting_changed: selected resource ID: ", brush_data["asset_id"])
	if plugin.editor:
		plugin.editor.set_brush_data(brush_data)
	_touchscreen_invert_active = bool(brush_data.get("invert", false))
	if _touchscreen_invert_active != previous_touchscreen_invert_active:
		plugin._read_input() # Revalidate keyboard input for modifier_ctrl.
	set_active_operation()
	update_decal()


func is_input_inverted() -> bool:
	return _touchscreen_invert_active


# Change tool/operation based on modifiers. Called from:
# * editor_plugin.gd:_read_input() - when a modifier key is pressed
# * _on_tool_changed() via:
# * _on_setting_changed() eg. Touchscreen Invert
func set_active_operation() -> void:
	var inverted: bool = plugin.modifier_ctrl || _touchscreen_invert_active
	active_tool = _selected_tool
	active_operation = _selected_operation

	# Toggle toolbar buttons
	toolbar.show_add_buttons(not inverted)

	# Shift provides smoothing only for tools that actually implement AVERAGE.
	# For all other tools it must leave the selected tool/operation unchanged.
	if plugin.modifier_shift and not inverted:
		match _selected_tool:
			Terrain3DEditor.SCULPT, Terrain3DEditor.HEIGHT:
				active_tool = Terrain3DEditor.SCULPT
				active_operation = Terrain3DEditor.AVERAGE
			Terrain3DEditor.TEXTURE:
				active_tool = Terrain3DEditor.TEXTURE
				active_operation = Terrain3DEditor.AVERAGE
			Terrain3DEditor.COLOR:
				active_tool = Terrain3DEditor.COLOR
				active_operation = Terrain3DEditor.AVERAGE
			Terrain3DEditor.ROUGHNESS:
				active_tool = Terrain3DEditor.ROUGHNESS
				active_operation = Terrain3DEditor.AVERAGE
			_:
				pass

	# Else if Ctrl/Invert checked, opposite
	elif _selected_operation == Terrain3DEditor.ADD and inverted:
		active_operation = Terrain3DEditor.SUBTRACT
	elif _selected_operation == Terrain3DEditor.SUBTRACT and not inverted:
		active_operation = Terrain3DEditor.ADD

	# Preserve a partially picked multipoint operation across unrelated setting or
	# modifier refreshes. Rebuild it only when entering/leaving the operation.
	if active_operation == Terrain3DEditor.GRADIENT:
		if not (operation_builder is GradientOperationBuilder):
			operation_builder = GradientOperationBuilder.new()
			operation_builder.tool_settings = tool_settings
	else:
		operation_builder = null

	if plugin.editor:
		plugin.editor.set_tool(active_tool)
		plugin.editor.set_operation(active_operation)


func _refresh_landform_mode_visibility() -> void:
	if _selected_tool != Terrain3DEditor.LANDFORM or not is_instance_valid(tool_settings):
		return

	var mode: int = int(tool_settings.get_setting("landform_mode"))
	var morphology: int = int(tool_settings.get_setting("landform_morphology"))
	var paint_material: bool = bool(tool_settings.get_setting("landform_paint_material"))
	var generate_mesh: bool = bool(tool_settings.get_setting("landform_generate_mesh"))
	var geometry_mode: int = int(tool_settings.get_setting("landform_geometry_mode"))
	var procedural_geometry: bool = geometry_mode == Terrain3DLandformBaker.GEOMETRY_PROCEDURAL_UNIQUE
	var modular_geometry: bool = geometry_mode == Terrain3DLandformBaker.GEOMETRY_MODULAR_INSTANCED
	var geometry_has_mesh: bool = geometry_mode != Terrain3DLandformBaker.GEOMETRY_TERRAIN_ONLY
	var supports_cliff_mesh: bool = mode in [Terrain3DLandformBaker.SCARP, Terrain3DLandformBaker.OCCLUSION]
	var uses_noise: bool = mode in [
		Terrain3DLandformBaker.RIDGE, Terrain3DLandformBaker.VALLEY, Terrain3DLandformBaker.SCARP,
		Terrain3DLandformBaker.TALUS, Terrain3DLandformBaker.HYDRAULIC_CHANNEL,
		Terrain3DLandformBaker.DUNE, Terrain3DLandformBaker.OCCLUSION,
	]
	var uses_amplitude: bool = mode in [
		Terrain3DLandformBaker.RIDGE, Terrain3DLandformBaker.VALLEY, Terrain3DLandformBaker.SCARP,
		Terrain3DLandformBaker.BASIN, Terrain3DLandformBaker.PLATEAU, Terrain3DLandformBaker.PASS,
		Terrain3DLandformBaker.TALUS, Terrain3DLandformBaker.HYDRAULIC_CHANNEL,
		Terrain3DLandformBaker.DUNE, Terrain3DLandformBaker.OCCLUSION,
	]
	var uses_profile_falloff: bool = mode != Terrain3DLandformBaker.PLATEAU
	var uses_crest: bool = mode != Terrain3DLandformBaker.BASIN

	var amplitude_label: String = "Height / Depth"
	var falloff_label: String = "Edge Falloff"
	var crest_label: String = "Flat Core / Crest"
	var title: String = "Landform"
	var help: String = "Drag a stroke over the terrain, then release to commit."
	match mode:
		Terrain3DLandformBaker.RIDGE:
			title = "Landform · Ridge"
			amplitude_label = "Ridge Height"
			help = "Raises terrain only. Existing terrain is never lowered."
		Terrain3DLandformBaker.VALLEY:
			title = "Landform · Valley"
			amplitude_label = "Valley Depth"
			help = "Lowers terrain only. Existing terrain is never raised."
		Terrain3DLandformBaker.SCARP:
			title = "Landform · Scarp / Cliff"
			amplitude_label = "Cliff Height"
			help = "Raises the selected side only. Preview ticks point toward the raised side; the generated mesh follows the finished terrain and fills the cap, face, and toe."
		Terrain3DLandformBaker.BASIN:
			title = "Landform · Basin"
			amplitude_label = "Basin Depth"
			falloff_label = "Rim Softness"
			help = "The stroke defines the basin spine and Width defines its full cross-section. The floor follows one fitted longitudinal grade, cuts downward only, and preserves terrain already below the target."
		Terrain3DLandformBaker.PLATEAU:
			title = "Landform · Plateau / Mesa"
			amplitude_label = "Plateau Height"
			help = "Raises terrain toward the plateau target without cutting existing high ground."
		Terrain3DLandformBaker.TERRACE:
			title = "Landform · Terrace"
			help = "Quantizes the existing slope into benches. This mode intentionally cuts and fills."
		Terrain3DLandformBaker.ROAD:
			title = "Landform · Road / Trail"
			help = "Grades the stroke to its drawn elevation with cut-and-fill and an optional crown."
		Terrain3DLandformBaker.PASS:
			title = "Landform · Mountain Pass"
			amplitude_label = "Pass Cut Depth"
			help = "Cuts a navigable pass downward. It does not raise surrounding terrain."
		Terrain3DLandformBaker.TALUS:
			title = "Landform · Talus / Debris"
			amplitude_label = "Deposit Height"
			help = "Deposits material only. Profile Bias chooses the preferred side of the stroke."
		Terrain3DLandformBaker.HYDRAULIC_CHANNEL:
			title = "Landform · Hydraulic Channel"
			amplitude_label = "Channel Depth"
			help = "Cuts the channel and raises small banks. This mode intentionally changes height in both directions."
		Terrain3DLandformBaker.THERMAL_RELAXATION:
			title = "Landform · Thermal Relaxation"
			help = "Moves material toward the selected angle of repose. This mode intentionally lowers and fills."
		Terrain3DLandformBaker.DUNE:
			title = "Landform · Dune"
			amplitude_label = "Dune Height"
			help = "Raises directional dune forms only. Existing terrain is never lowered."
		Terrain3DLandformBaker.SHORELINE:
			title = "Landform · Shoreline"
			help = "Grades terrain toward the water level. This mode intentionally cuts and fills."
		Terrain3DLandformBaker.MORPHOLOGY:
			title = "Landform · Morphology"
			match morphology:
				Terrain3DLandformBaker.MORPH_EXPAND_PEAKS, Terrain3DLandformBaker.MORPH_FILL_PITS, Terrain3DLandformBaker.MORPH_RIDGE_SHARPEN:
					help = "This morphology raises terrain only."
				Terrain3DLandformBaker.MORPH_VALLEY_SHARPEN:
					help = "This morphology lowers terrain only."
				_:
					help = "Plateau cleanup may raise or lower small deviations around a local plane."
		Terrain3DLandformBaker.OCCLUSION:
			title = "Landform · Occlusion"
			amplitude_label = "Occluder Height"
			help = "Raises an occluding formation only. Its optional cliff mesh fills the terrain-to-mesh transition."

	tool_settings.set_context(title, help)
	tool_settings.set_setting_label(&"landform_amplitude", amplitude_label)
	tool_settings.set_setting_label(&"landform_falloff", falloff_label)
	tool_settings.set_setting_label(&"landform_crest", crest_label)
	tool_settings.set_setting_tooltip(&"landform_amplitude", help)
	tool_settings.set_setting_label(
		&"landform_cliff_side",
		"Raised Side" if mode == Terrain3DLandformBaker.SCARP else "Cliff Face Side"
	)
	tool_settings.set_setting_tooltip(
		&"landform_cliff_side",
		"Chooses which side is raised; preview ticks point toward it."
		if mode == Terrain3DLandformBaker.SCARP
		else "Chooses which side of the occlusion stroke receives the generated cliff face."
	)

	var mode_rows: Dictionary[StringName, bool] = {
		"landform_amplitude": uses_amplitude,
		"landform_falloff": uses_profile_falloff,
		"landform_crest": uses_crest,
		"landform_asymmetry": mode in [Terrain3DLandformBaker.SCARP, Terrain3DLandformBaker.TALUS],
		"landform_cliff_side": mode in [Terrain3DLandformBaker.SCARP, Terrain3DLandformBaker.OCCLUSION],
		"landform_terrace_step": mode == Terrain3DLandformBaker.TERRACE,
		"landform_road_crown": mode == Terrain3DLandformBaker.ROAD,
		"landform_water_level": mode == Terrain3DLandformBaker.SHORELINE,
		"landform_shore_slope": mode == Terrain3DLandformBaker.SHORELINE,
		"landform_dune_spacing": mode == Terrain3DLandformBaker.DUNE,
		"landform_repose": mode == Terrain3DLandformBaker.THERMAL_RELAXATION,
		"landform_iterations": mode == Terrain3DLandformBaker.THERMAL_RELAXATION,
		"landform_morphology": mode == Terrain3DLandformBaker.MORPHOLOGY,
		"landform_basin_floor_width": mode == Terrain3DLandformBaker.BASIN,
		"landform_basin_create_regions": mode == Terrain3DLandformBaker.BASIN,
		"landform_noise": uses_noise,
		"landform_noise_scale": uses_noise,
		"landform_seed": uses_noise,
		"landform_overlay_texture": paint_material,
		"landform_material_blend": paint_material,
		"landform_generate_mesh": supports_cliff_mesh,
		"landform_geometry_mode": supports_cliff_mesh and generate_mesh,
		"landform_mesh_depth": supports_cliff_mesh and generate_mesh and geometry_has_mesh,
		"landform_mesh_irregularity": supports_cliff_mesh and generate_mesh and procedural_geometry,
		"landform_mesh_spacing": supports_cliff_mesh and generate_mesh and procedural_geometry,
		"landform_mesh_lip": supports_cliff_mesh and generate_mesh and geometry_has_mesh,
		"landform_mesh_toe": supports_cliff_mesh and generate_mesh and geometry_has_mesh,
		"landform_module_length": supports_cliff_mesh and generate_mesh and modular_geometry,
		"landform_module_corner_angle": supports_cliff_mesh and generate_mesh and modular_geometry,
		"landform_module_min_scale": supports_cliff_mesh and generate_mesh and modular_geometry,
		"landform_module_max_scale": supports_cliff_mesh and generate_mesh and modular_geometry,
		"landform_collision_spacing": supports_cliff_mesh and generate_mesh and geometry_has_mesh,
		"landform_generate_collision": supports_cliff_mesh and generate_mesh and geometry_has_mesh,
	}
	for prefix: String in ["landform_module_mesh_", "landform_corner_mesh_", "landform_end_mesh_"]:
		var count: int = 4 if prefix == "landform_module_mesh_" else 2
		for index: int in range(count):
			mode_rows["%s%d" % [prefix, index]] = supports_cliff_mesh and generate_mesh and modular_geometry
	for setting_name: StringName in mode_rows:
		tool_settings.set_setting_row_visible(setting_name, bool(mode_rows[setting_name]))
	set_menu_visibility(tool_settings.landform_mesh_list, supports_cliff_mesh)


func _regenerate_selected_landform() -> void:
	if not landform_controller:
		return
	var selected: Array[Node] = EditorInterface.get_selection().get_selected_nodes()
	for selected_node: Node in selected:
		var candidate: Node = selected_node
		while candidate and candidate != plugin.terrain:
			if bool(candidate.get_meta(&"terrain3d_generated_landform", false)):
				var replacement: Node3D = landform_controller.regenerate_landform(candidate as Node3D, true)
				if replacement:
					var editor_selection: EditorSelection = EditorInterface.get_selection()
					editor_selection.clear()
					editor_selection.add_node(replacement)
				return
			candidate = candidate.get_parent()
	push_warning("Terrain3D landform: select a generated landform root or one of its children.")


func begin_landform_stroke(p_position: Vector3) -> void:
	if landform_controller:
		landform_controller.begin_stroke(p_position)


func append_landform_stroke(p_position: Vector3) -> void:
	if landform_controller:
		landform_controller.append_stroke(p_position)


func finish_landform_stroke(p_position: Vector3) -> bool:
	return landform_controller.finish_stroke(p_position) if landform_controller else false


func cancel_landform_stroke() -> void:
	if landform_controller:
		landform_controller.cancel_stroke()


func update_decal() -> void:
	if not plugin.terrain or not plugin.viewport or brush_data.size() <= 3:
		return

	# If not a state that should show the decal, hide everything and return
	mat_rid = plugin.terrain.material.get_material_rid() # Used in hide_decal() and below
	# After moving camera, wait for mouse cursor to update before revealing.
	# See https://github.com/godotengine/godot/issues/70098.
	if (
		not visible
		or plugin._input_mode == -1
		or Time.get_ticks_msec() - plugin.rmb_release_time <= 100
	):
		hide_decal()
		return

	# Only show decal if in viewport or toolbars
	var main: Control = EditorInterface.get_editor_main_screen()
	var main_rect := Rect2(main.position, main.size)
	main_rect.size.y += tool_settings.size.y
	if not ( main_rect.has_point(plugin.viewport.get_mouse_position()) && plugin.mouse_in_main ):
		return

	reset_decal_arrays()
	editor_decal_position[0] = Vector2(plugin.mouse_global_position.x, plugin.mouse_global_position.z)
	editor_decal_visible = [true, false, false] # Show cursor by default
	editor_decal_part = [true, true] # Show brush and reticle by default
	editor_decal_timer.start()

	## Region Operations
	var r_map: PackedInt32Array = plugin.terrain.data.get_region_map()
	if plugin.editor.get_tool() == Terrain3DEditor.REGION:
		var r_size: float = float(plugin.terrain.get_region_size()) * plugin.terrain.get_vertex_spacing()
		var map_size: int = plugin.terrain.data.get_region_map_size()
		var half_r_size: float = r_size * 0.5
		var pos: Vector2 = (Vector2(plugin.mouse_global_position.x, plugin.mouse_global_position.z) +
			Vector2(half_r_size, half_r_size)).snappedf(r_size) - Vector2(half_r_size, half_r_size)
		editor_brush_texture_rid = region_texture.get_rid()
		editor_decal_position[0] = pos
		editor_decal_size[0] = r_size
		editor_decal_rotation[0] = 0.0
		editor_decal_part[1] = false # Disable reticle

		var loc: Vector2i = plugin.terrain.data.get_region_location(plugin.mouse_global_position)
		loc += Vector2i(map_size / 2, map_size / 2)
		if !(loc.x < 0 or loc.x > map_size - 1 or loc.y < 0 or loc.y > map_size - 1):
			var index: int = clampi(loc.y * map_size + loc.x, 0, map_size * map_size - 1)
			if plugin.terrain.material.get_world_background() == Terrain3DMaterial.NONE:
				if r_map[index] == 0 and active_operation == Terrain3DEditor.ADD:
					r_map[index] = -index - 1
				else:
					r_map[index] = r_map[index]

			match active_operation:
				Terrain3DEditor.ADD:
					if r_map[index] <= 0:
						editor_decal_color[0] = Color.WHITE
						editor_decal_color[0].a = 0.25
					else:
						hide_decal()

				Terrain3DEditor.SUBTRACT:
					if r_map[index] > 0:
						editor_decal_color[0] = Color.WHITE * .15
						editor_decal_color[0].a = 0.75
					else:
						hide_decal()
		else:
			hide_decal()

	## Picking
	elif picking != Terrain3DEditor.TOOL_MAX:
		editor_decal_part[0] = false # Hide brush
		editor_decal_size[0] = plugin.terrain.get_vertex_spacing()
		editor_decal_color[0] = COLOR_PICK
		editor_decal_color[0].a = 1.0

	## Brushing Operations
	else:
		editor_brush_texture_rid = brush_data["brush"][1].get_rid()
		var decal_size: float = float(brush_data["size"])
		if plugin.editor.get_tool() == Terrain3DEditor.LANDFORM:
			decal_size = float(brush_data.get("landform_width", decal_size))
		editor_decal_size[0] = maxf(decal_size, 0.5)
		if brush_data["align_to_view"]:
			var cam: Camera3D = plugin.terrain.get_camera();
			if (cam):
				editor_decal_rotation[0] = cam.rotation.y
			else:
				editor_decal_rotation[0] = 0.
		match plugin.editor.get_tool():
			Terrain3DEditor.SCULPT:
				match active_operation:
					Terrain3DEditor.ADD:
						if plugin.modifier_alt:
							editor_decal_color[0] = COLOR_LIFT
							editor_decal_color[0].a = clampf(float(brush_data["strength"]), .2, .5)
						else:
							editor_decal_color[0] = COLOR_RAISE
							editor_decal_color[0].a = clampf(float(brush_data["strength"]), .25, .5)
					Terrain3DEditor.SUBTRACT:
						if plugin.modifier_alt:
							editor_decal_color[0] = COLOR_FLATTEN
							editor_decal_color[0].a = clampf(float(brush_data["strength"]), .25, .5) + .1
						else:
							editor_decal_color[0] = COLOR_LOWER
							editor_decal_color[0].a = clampf(float(brush_data["strength"]), .2, .5) + .25
					Terrain3DEditor.AVERAGE:
						editor_decal_color[0] = COLOR_SMOOTH
						editor_decal_color[0].a = clampf(float(brush_data["strength"]), .2, .5) + .25
					Terrain3DEditor.GRADIENT:
						editor_decal_color[0] = COLOR_SLOPE
						editor_decal_color[0].a = clampf(float(brush_data["strength"]), .2, .4)
			Terrain3DEditor.HEIGHT:
				editor_decal_color[0] = COLOR_HEIGHT
				editor_decal_color[0].a = clampf(float(brush_data["strength"]), .2, .5) + .25
			Terrain3DEditor.TEXTURE:
				if plugin._input_mode == 1:
					editor_decal_part[0] = false # Hide brush
				if plugin.modifier_shift:
					editor_decal_color[0] = COLOR_AVERAGE
					editor_decal_color[0].a = clampf(float(brush_data["strength"]), .2, .5) + .25
				else:
					match active_operation:
						Terrain3DEditor.REPLACE:
							editor_decal_color[0] = COLOR_PAINT
							editor_decal_color[0].a = .6
						Terrain3DEditor.SUBTRACT:
							editor_decal_color[0] = COLOR_UNSPRAY
							editor_decal_color[0].a = clampf(float(brush_data["strength"]), .2, .5) + .1
						Terrain3DEditor.ADD:
							editor_decal_color[0] = COLOR_SPRAY
							editor_decal_color[0].a = clampf(float(brush_data["strength"]), .15, .4)
			Terrain3DEditor.COLOR:
				if active_operation == Terrain3DEditor.AVERAGE:
					editor_decal_color[0] = COLOR_AVERAGE
					editor_decal_color[0].a = clampf(float(brush_data["strength"]), .2, .5) + .25
				elif active_operation == Terrain3DEditor.SUBTRACT:
					editor_decal_color[0] = Color.WHITE
					editor_decal_color[0].a = clampf(float(brush_data["strength"]), .2, .5)
				else:
					editor_decal_color[0] = brush_data["color"].srgb_to_linear()
					editor_decal_color[0].a *= clampf(float(brush_data["strength"]), .3, .5)
			Terrain3DEditor.ROUGHNESS:
				if plugin._input_mode == 1:
					editor_decal_part[0] = false # Hide brush
				if active_operation == Terrain3DEditor.AVERAGE:
					editor_decal_color[0] = COLOR_AVERAGE
					editor_decal_color[0].a = clampf(float(brush_data["strength"]), .2, .5) + .25
				elif active_operation == Terrain3DEditor.SUBTRACT:
					editor_decal_color[0] = COLOR_DRY
					editor_decal_color[0].a = clampf(float(brush_data["strength"]), .2, .5) + .1
				else:
					editor_decal_color[0] = COLOR_WET
					editor_decal_color[0].a = clampf(float(brush_data["strength"]), .2, .5) + .1
			Terrain3DEditor.AUTOSHADER:
				editor_decal_color[0] = COLOR_AUTOSHADER
				editor_decal_color[0].a = .6
			Terrain3DEditor.HOLES:
				editor_decal_color[0] = COLOR_HOLES
				editor_decal_color[0].a = .75
			Terrain3DEditor.NAVIGATION:
				editor_decal_color[0] = COLOR_NAVIGATION
				editor_decal_color[0].a = .80
			Terrain3DEditor.LANDFORM:
				editor_decal_color[0] = COLOR_LANDFORM
				editor_decal_color[0].a = 0.48
			Terrain3DEditor.INSTANCER:
				editor_decal_part[0] = false # Hide brush
				if active_operation == Terrain3DEditor.SUBTRACT:
					editor_decal_color[0] = COLOR_UNINSTANCE
					editor_decal_color[0].a = .75
				else:
					editor_decal_color[0] = COLOR_INSTANCE
					editor_decal_color[0].a = .75

	if plugin.editor.get_tool() != Terrain3DEditor.REGION and not brush_data["show_brush_texture"]:
		editor_decal_part[0] = false # Hide brush

	if active_operation == Terrain3DEditor.GRADIENT:
		var point1: Vector3 = brush_data["gradient_points"][0]
		if point1 != Vector3.ZERO:
			editor_decal_color[1] = COLOR_SLOPE
			editor_decal_size[1] = 0.25
			editor_decal_visible[1] = true
			editor_decal_position[1] = Vector2(point1.x, point1.z)
		var point2: Vector3 = brush_data["gradient_points"][1]
		if point2 != Vector3.ZERO:
			editor_decal_color[2] = COLOR_SLOPE
			editor_decal_size[2] = 0.25
			editor_decal_visible[2] = true
			editor_decal_position[2] = Vector2(point2.x, point2.z)

	if RenderingServer.get_current_rendering_method().contains("gl_compatibility"):
		for i: int in editor_decal_color.size():
			editor_decal_color[i].a = maxf(0.1, editor_decal_color[i].a - .25)

	editor_decal_fade = editor_decal_color[0].a
	# Update Shader params
	if is_shader_valid():
		RenderingServer.material_set_param(mat_rid, "_editor_brush_texture", editor_brush_texture_rid)
		RenderingServer.material_set_param(mat_rid, "_editor_decal_position", editor_decal_position)
		RenderingServer.material_set_param(mat_rid, "_editor_decal_rotation", editor_decal_rotation)
		RenderingServer.material_set_param(mat_rid, "_editor_decal_size", editor_decal_size)
		RenderingServer.material_set_param(mat_rid, "_editor_decal_color", editor_decal_color)
		RenderingServer.material_set_param(mat_rid, "_editor_decal_visible", editor_decal_visible)
		RenderingServer.material_set_param(mat_rid, "_editor_decal_part", editor_decal_part)
		RenderingServer.material_set_param(mat_rid, "_region_map", r_map)


func is_shader_valid() -> bool:
	# As long as the compiled shader contains at least 1 uniform, we can use it to check
	# if the shader compilation has failed, as this will then return an empty dictionary.
	if not is_instance_valid(plugin) or not is_instance_valid(plugin.terrain):
		return false
	if not is_instance_valid(plugin.terrain.material) or not mat_rid.is_valid():
		return false
	var shader_rid: RID = plugin.terrain.material.get_shader_rid()
	if not shader_rid.is_valid():
		return false
	return not RenderingServer.get_shader_parameter_list(shader_rid).is_empty()


func hide_decal() -> void:
	editor_decal_visible = [false, false, false]
	if is_shader_valid():
		var r_map: PackedInt32Array = plugin.terrain.data.get_region_map()
		RenderingServer.material_set_param(mat_rid, "_editor_decal_visible", editor_decal_visible)
		RenderingServer.material_set_param(mat_rid, "_region_map", r_map)


# These array sizes are reset to 0 when closing scenes for some unknown reason, so check and reset
func reset_decal_arrays() -> void:
	if editor_decal_color.size() < 3:
		editor_brush_texture_rid = RID()
		editor_decal_position = [Vector2(), Vector2(), Vector2()]
		editor_decal_rotation = [0., 0., 0.]
		editor_decal_size = [0., 0., 0.]
		editor_decal_color = [Color(), Color(), Color()]
		editor_decal_visible = [false, false, false]
		editor_decal_part = [true, true]


func set_decal_rotation(p_rot: float) -> void:
	editor_decal_rotation[0] = p_rot
	if is_shader_valid():
		RenderingServer.material_set_param(mat_rid, "_editor_decal_rotation", editor_decal_rotation)


func _on_picking(p_type: int, p_callback: Callable) -> void:
	picking = p_type
	picking_callback = p_callback
	if picking == Terrain3DEditor.INSTANCER:
		if not get_tree().process_frame.is_connected(_update_picker_highlight):
			get_tree().process_frame.connect(_update_picker_highlight)
	else:
		if get_tree().process_frame.is_connected(_update_picker_highlight):
			get_tree().process_frame.disconnect(_update_picker_highlight)


func _update_picker_highlight() -> void:
	if not is_instance_valid(plugin) or not is_instance_valid(plugin.terrain) or not plugin.terrain.assets:
		clear_picking()
		return
	var mesh_asset_id: int = -1
	if plugin.terrain.data.has_regionp(plugin.mouse_global_position):
		mesh_asset_id = plugin.terrain.instancer.get_closest_mesh_id(plugin.mouse_global_position)
	for i: int in plugin.terrain.assets.get_mesh_count():
		var ma: Terrain3DMeshAsset = plugin.terrain.assets.get_mesh_asset(i)
		if ma:
			ma.set_highlighted(i == mesh_asset_id)


func clear_picking() -> void:
	picking = Terrain3DEditor.TOOL_MAX
	picking_callback = Callable()
	if is_inside_tree():
		var tree: SceneTree = get_tree()
		if tree.process_frame.is_connected(_update_picker_highlight):
			tree.process_frame.disconnect(_update_picker_highlight)
	if not is_instance_valid(plugin) or not is_instance_valid(plugin.terrain) or not plugin.terrain.assets:
		return
	for i: int in range(0, plugin.terrain.assets.get_mesh_count()):
		var ma: Terrain3DMeshAsset = plugin.terrain.assets.get_mesh_asset(i)
		if ma:
			ma.set_highlighted(false)
	asset_dock.update_dock()


func is_picking() -> bool:
	if picking != Terrain3DEditor.TOOL_MAX:
		return true

	if operation_builder and operation_builder.is_picking():
		return true

	return false


func pick(p_global_position: Vector3) -> void:
	if picking != Terrain3DEditor.TOOL_MAX:
		var color: Color
		match picking:
			Terrain3DEditor.HEIGHT, Terrain3DEditor.SCULPT:
				color = Color(plugin.terrain.data.get_height(p_global_position), 0., 0., 1.)
			Terrain3DEditor.ROUGHNESS:
				color = plugin.terrain.data.get_pixel(int(Terrain3DRegion.TYPE_COLOR), p_global_position)
			Terrain3DEditor.COLOR:
				color = plugin.terrain.data.get_color(p_global_position)
			Terrain3DEditor.ANGLE:
				color = Color(plugin.terrain.data.get_control_angle(p_global_position), 0., 0., 1.)
			Terrain3DEditor.SCALE:
				color = Color(plugin.terrain.data.get_control_scale(p_global_position), 0., 0., 1.)
			Terrain3DEditor.INSTANCER:
				var mesh_asset_id: int = plugin.terrain.instancer.get_closest_mesh_id(p_global_position)
				color = Color(mesh_asset_id, 0., 0., 1.)
			Terrain3DEditor.TEXTURE:
				var texture_blend_data: Vector3 = plugin.terrain.data.get_texture_id(p_global_position)
				if not texture_blend_data.is_finite():
					return
				if texture_blend_data.z < 0.65:
					color = Color(texture_blend_data.x, 0., 0., 1.)
				else:
					color = Color(texture_blend_data.y, 0., 0., 1.)
			_:
				push_error("Unsupported picking type: ", picking)
				return
		if picking_callback.is_valid():
			picking_callback.call(picking, color, p_global_position)
			picking_callback = Callable()
		clear_picking()

	elif operation_builder and operation_builder.is_picking():
		operation_builder.pick(p_global_position, plugin.terrain)


func _get_autoshader_context() -> PackedStringArray:
	if not is_instance_valid(plugin) or not is_instance_valid(plugin.terrain):
		return PackedStringArray(["Material Rule Mask", "Paint where automatic material rules are allowed to write."])
	var profile := plugin.terrain.auto_shader_profile as Terrain3DAutoShaderProfile
	if not is_instance_valid(profile):
		return PackedStringArray(["Material Rule Mask", "No rule profile is assigned. Open Material Rules to create one, then paint the automatic-material mask here."])
	var enabled_rules: int = 0
	for rule: Terrain3DAutoShaderRule in profile.rules:
		if is_instance_valid(rule) and rule.enabled:
			enabled_rules += 1
	return PackedStringArray([
		"Material Rule Mask · %s" % profile.get_mode_name(),
		"Paint where %d enabled material rules may write. The inverse operation protects an area from automatic classification." % enabled_rules,
	])


func _get_tool_context(
	p_tool: int,
	p_operation: int
) -> PackedStringArray:
	match p_tool:
		Terrain3DEditor.REGION:
			return PackedStringArray(["Regions", "Click terrain to add a 128 m region. Use the inverse operation to remove one."])
		Terrain3DEditor.SCULPT:
			match p_operation:
				Terrain3DEditor.AVERAGE:
					return PackedStringArray(["Smooth", "Blend local height differences without changing the selected material."])
				Terrain3DEditor.GRADIENT:
					return PackedStringArray(["Slope", "Pick slope points, then draw or apply the fitted gradient."])
				Terrain3DEditor.SUBTRACT:
					return PackedStringArray(["Lower", "Lower terrain using the selected brush, size, and strength."])
				_:
					return PackedStringArray(["Raise", "Raise terrain using the selected brush, size, and strength."])
		Terrain3DEditor.HEIGHT:
			return PackedStringArray(["Set Height", "Paint an explicit world height; use the picker to sample terrain."])
		Terrain3DEditor.TEXTURE:
			return PackedStringArray(["Texture", "Paint the selected texture asset. Angle, scale, and slope filters are optional."])
		Terrain3DEditor.COLOR:
			return PackedStringArray(["Color", "Tint terrain color data without changing the material assignment."])
		Terrain3DEditor.ROUGHNESS:
			return PackedStringArray(["Wetness", "Paint the terrain wetness/roughness control channel."])
		Terrain3DEditor.AUTOSHADER:
			return _get_autoshader_context()
		Terrain3DEditor.HOLES:
			return PackedStringArray(["Holes", "Mark terrain fragments as holes or restore them."])
		Terrain3DEditor.NAVIGATION:
			return PackedStringArray(["Navigation", "Paint the Terrain3D navigation mask used by navigation baking."])
		Terrain3DEditor.LANDFORM:
			return PackedStringArray(["Landform Stroke", "Drag across the terrain to author a ridge, valley, cliff, road, basin, dune, shoreline, or cleanup operation. Scarp and occlusion modes can generate the missing vertical cliff mesh automatically."])
		Terrain3DEditor.INSTANCER:
			return PackedStringArray(["Mesh Instances", "Place or remove the selected mesh asset with controlled variation."])
	return PackedStringArray(["Terrain Tool", "Select a Terrain3D editing tool."])


func set_button_editor_icon(p_button: Button, p_icon_name: String) -> void:
	p_button.icon = EditorInterface.get_base_control().get_theme_icon(p_icon_name, "EditorIcons")
