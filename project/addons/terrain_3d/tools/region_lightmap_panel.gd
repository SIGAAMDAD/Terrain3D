# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Persistent editor UI for baking one Terrain3D region at a time from the active scene.
@tool
class_name Terrain3DRegionLightmapPanel
extends Window

var plugin: Terrain3DEditorPluginBase
var baker: Terrain3DRegionLightmapBaker
var terrain: Terrain3D

var _region_selector: OptionButton
var _region_status: Label
var _workflow_status: Label
var _prepare_button: Button
var _select_lightmap_button: Button
var _finalize_button: Button
var _available_locations: Array[Vector2i] = []
var _poll_timer: Timer


func _ready() -> void:
	title = "Terrain3D Regional Lightmap Baking"
	initial_position = Window.WINDOW_INITIAL_POSITION_CENTER_MAIN_WINDOW_SCREEN
	min_size = Vector2i(620, 390)
	size = Vector2i(680, 440)
	unresizable = false
	transient = true
	exclusive = false
	close_requested.connect(hide)
	_build_ui()

	_poll_timer = Timer.new()
	_poll_timer.wait_time = 0.5
	_poll_timer.one_shot = false
	_poll_timer.timeout.connect(_refresh_workflow_state)
	add_child(_poll_timer)


func open(p_terrain: Terrain3D) -> void:
	terrain = p_terrain
	_refresh_regions()
	_refresh_workflow_state()
	popup()
	_poll_timer.start()


func _notification(p_what: int) -> void:
	if p_what == NOTIFICATION_VISIBILITY_CHANGED and is_instance_valid(_poll_timer):
		if visible:
			_poll_timer.start()
		else:
			_poll_timer.stop()


func _build_ui() -> void:
	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 14)
	margin.add_theme_constant_override("margin_right", 14)
	margin.add_theme_constant_override("margin_top", 12)
	margin.add_theme_constant_override("margin_bottom", 12)
	add_child(margin)
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)

	var content := VBoxContainer.new()
	content.add_theme_constant_override("separation", 10)
	margin.add_child(content)

	var heading := Label.new()
	heading.text = "Regional Lightmap Baking"
	heading.add_theme_font_size_override("font_size", 18)
	content.add_child(heading)

	var explanation := Label.new()
	explanation.text = "Bake one authored Terrain3D region at a time from this scene. Neighboring regions are used only as temporary lighting context; finalizing changes only the selected region's lighting package."
	explanation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	content.add_child(explanation)

	var region_row := HBoxContainer.new()
	region_row.add_theme_constant_override("separation", 8)
	content.add_child(region_row)

	var region_label := Label.new()
	region_label.text = "Region"
	region_row.add_child(region_label)

	_region_selector = OptionButton.new()
	_region_selector.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_region_selector.item_selected.connect(_on_region_selected)
	region_row.add_child(_region_selector)

	var cursor_button := Button.new()
	cursor_button.text = "Use Cursor Region"
	cursor_button.tooltip_text = "Select the authored region currently under the Terrain3D editor cursor."
	cursor_button.pressed.connect(_select_cursor_region)
	region_row.add_child(cursor_button)

	var refresh_button := Button.new()
	refresh_button.text = "Refresh"
	refresh_button.pressed.connect(_refresh_regions)
	region_row.add_child(refresh_button)

	_region_status = Label.new()
	_region_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_region_status.custom_minimum_size = Vector2(0, 44)
	content.add_child(_region_status)

	var separator := HSeparator.new()
	content.add_child(separator)

	var workflow_heading := Label.new()
	workflow_heading.text = "Bake workflow"
	workflow_heading.add_theme_font_size_override("font_size", 15)
	content.add_child(workflow_heading)

	var workflow_help := Label.new()
	workflow_help.text = "1. Prepare the selected region.  2. With the generated LightmapGI selected, run Godot's Bake Lightmaps command.  3. Finalize here to publish the bake to that Terrain3D region."
	workflow_help.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	content.add_child(workflow_help)

	var button_row := HBoxContainer.new()
	button_row.add_theme_constant_override("separation", 8)
	content.add_child(button_row)

	_prepare_button = Button.new()
	_prepare_button.text = "Prepare Selected Region"
	_prepare_button.tooltip_text = "Build the temporary per-region LightmapGI hierarchy in this scene."
	_prepare_button.pressed.connect(_prepare_selected_region)
	button_row.add_child(_prepare_button)

	_select_lightmap_button = Button.new()
	_select_lightmap_button.text = "Select Prepared LightmapGI"
	_select_lightmap_button.tooltip_text = "Select the generated LightmapGI so Godot's Bake Lightmaps command targets this regional bake."
	_select_lightmap_button.pressed.connect(_select_prepared_lightmap)
	button_row.add_child(_select_lightmap_button)

	_finalize_button = Button.new()
	_finalize_button.text = "Finalize Baked Region"
	_finalize_button.tooltip_text = "Publish the completed LightmapGIData and attach it only to the prepared Terrain3D region."
	_finalize_button.pressed.connect(_finalize_prepared_region)
	button_row.add_child(_finalize_button)

	_workflow_status = Label.new()
	_workflow_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_workflow_status.custom_minimum_size = Vector2(0, 72)
	content.add_child(_workflow_status)


func _refresh_regions() -> void:
	var previous: Variant = _selected_location()
	_available_locations.clear()
	_region_selector.clear()
	if not _is_terrain_ready():
		_region_status.text = "Select a Terrain3D node with loaded authored data."
		_prepare_button.disabled = true
		return
	_available_locations.assign(terrain.data.get_region_locations())
	_available_locations.sort_custom(_region_location_less)
	for location: Vector2i in _available_locations:
		var region: Terrain3DRegion = terrain.data.get_region(location)
		var suffix: String = ""
		if is_instance_valid(region) and is_instance_valid(region.get_lighting()):
			suffix = "  • baked"
		_region_selector.add_item("%d, %d%s" % [location.x, location.y, suffix])

	if _available_locations.is_empty():
		_region_status.text = "This Terrain3D data set has no authored regions."
		_prepare_button.disabled = true
		return

	var select_index: int = 0
	if previous is Vector2i:
		var previous_index: int = _available_locations.find(previous)
		if previous_index >= 0:
			select_index = previous_index
	else:
		var cursor_location: Variant = _cursor_region_location()
		if cursor_location is Vector2i:
			var cursor_index: int = _available_locations.find(cursor_location)
			if cursor_index >= 0:
				select_index = cursor_index
	_region_selector.select(select_index)
	_prepare_button.disabled = false
	_refresh_region_status()


func _on_region_selected(_p_index: int) -> void:
	_refresh_region_status()


func _refresh_region_status() -> void:
	var location: Variant = _selected_location()
	if not (location is Vector2i) or not _is_terrain_ready():
		_region_status.text = "Choose an authored terrain region."
		return
	var region: Terrain3DRegion = terrain.data.get_region(location)
	if is_instance_valid(region) and is_instance_valid(region.get_lighting()):
		var lighting: Resource = region.get_lighting()
		var data_path: String = String(lighting.get("light_data_path"))
		_region_status.text = "Region %s has baked lighting. Re-preparing and finalizing replaces this region's package.\n%s" % [location, data_path]
	else:
		_region_status.text = "Region %s has no baked lighting yet." % location


func _refresh_workflow_state() -> void:
	_sync_active_terrain()
	if not is_instance_valid(baker):
		_select_lightmap_button.disabled = true
		_finalize_button.disabled = true
		_workflow_status.text = "Regional lightmap baker is unavailable."
		return
	var prepared: Variant = baker.get_prepared_region()
	if not (prepared is Vector2i):
		_select_lightmap_button.disabled = true
		_finalize_button.disabled = true
		_workflow_status.text = "No regional bake is prepared in the active scene."
		return
	_select_lightmap_button.disabled = false
	var baked: bool = baker.is_prepared_bake_complete()
	_finalize_button.disabled = not baked
	if baked:
		_workflow_status.text = "Region %s is baked and ready to finalize. Finalize publishes only this region's lighting package and removes the temporary bake subtree." % prepared
	else:
		_workflow_status.text = "Region %s is prepared. Run Godot's Bake Lightmaps command for the selected Terrain3DRegionalLightmapGI, then return here to finalize." % prepared


func _prepare_selected_region() -> void:
	_sync_active_terrain()
	var location: Variant = _selected_location()
	if not (location is Vector2i) or not is_instance_valid(baker):
		return
	if baker.prepare_region_bake(location):
		baker.select_prepared_lightmap()
	_refresh_workflow_state()


func _select_prepared_lightmap() -> void:
	if is_instance_valid(baker):
		baker.select_prepared_lightmap()
	_refresh_workflow_state()


func _finalize_prepared_region() -> void:
	if not is_instance_valid(baker):
		return
	if baker.finalize_prepared_bake():
		_refresh_regions()
	_refresh_workflow_state()


func _select_cursor_region() -> void:
	var location: Variant = _cursor_region_location()
	if not (location is Vector2i):
		return
	var index: int = _available_locations.find(location)
	if index < 0:
		_region_status.text = "The editor cursor is not over an authored Terrain3D region."
		return
	_region_selector.select(index)
	_refresh_region_status()


func _selected_location() -> Variant:
	if not is_instance_valid(_region_selector):
		return null
	var index: int = _region_selector.selected
	if index < 0 or index >= _available_locations.size():
		return null
	return _available_locations[index]


func _cursor_region_location() -> Variant:
	if not _is_terrain_ready() or not is_instance_valid(plugin):
		return null
	var mouse_position: Variant = plugin.get("mouse_global_position")
	if not (mouse_position is Vector3):
		return null
	var location: Vector2i = terrain.data.get_region_location(mouse_position)
	return location if is_instance_valid(terrain.data.get_region(location)) else null


func _sync_active_terrain() -> void:
	if not is_instance_valid(plugin):
		return
	var active: Terrain3D = plugin.get_terrain()
	# Selecting the generated LightmapGI can hide the Terrain3D editor plugin without
	# invalidating the source terrain. Keep the current source in that case so the
	# persistent bake window remains usable through the Godot bake step.
	if not is_instance_valid(active) or active == terrain:
		return
	terrain = active
	_refresh_regions()


func _is_terrain_ready() -> bool:
	return is_instance_valid(terrain) and terrain.data != null and not terrain.data_directory.is_empty()


func _region_location_less(a: Vector2i, b: Vector2i) -> bool:
	return a.y < b.y or (a.y == b.y and a.x < b.x)
