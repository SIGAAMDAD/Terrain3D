# Copyright © 2023-2026 Terrain3D Contributors.
# Terrain-only presentation of the existing Terrain3D native settings.
@tool
extends Window

const SettingMetadata = preload("res://addons/terrain_3d/src/setting_metadata.gd")
const SettingControlFactory = preload("res://addons/terrain_3d/src/setting_control_factory.gd")
const TerrainTransaction = preload("res://addons/terrain_3d/src/inspector/terrain_edit_transaction.gd")

const VIEW_MODES = [
	"show_checkered", "show_grey", "show_heightmap", "show_jaggedness",
	"show_autoshader", "show_control_texture", "show_control_blend",
	"show_control_angle", "show_control_scale", "show_colormap",
	"show_roughmap", "show_displacement_buffer", "show_texture_albedo",
	"show_texture_height", "show_texture_normal", "show_texture_rough", "show_texture_ao",
]
const VIEW_OVERLAYS = [
	"show_region_grid", "show_vertex_grid", "show_contours",
]

var plugin: Terrain3DEditorPluginBase
var terrain: Terrain3D
var transaction: Terrain3DEditTransaction
var _tabs: TabContainer
var _pages: Dictionary[String, VBoxContainer] = {}
var _rows: Dictionary[String, Control] = {}
var _descriptors: Dictionary[String, Dictionary] = {}
var _status_labels: Dictionary[String, Label] = {}
var _view_mode: OptionButton
var _dataset_status: Label
var _syncing: bool = false
var _layout_dirty: bool = true


func _ready() -> void:
	title = "Terrain3D Terrain Settings"
	min_size = Vector2i(660, 460)
	size = Vector2i(900, 730)
	visible = false
	close_requested.connect(hide)
	var margin: MarginContainer = MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	for side: StringName in [&"margin_left", &"margin_top", &"margin_right", &"margin_bottom"]:
		margin.add_theme_constant_override(side, 12)
	add_child(margin)
	var root: VBoxContainer = VBoxContainer.new()
	margin.add_child(root)
	var toolbar: HBoxContainer = HBoxContainer.new()
	root.add_child(toolbar)
	var description: Label = Label.new()
	description.text = "Terrain renderer and dataset settings"
	description.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	toolbar.add_child(description)
	var refresh: Button = Button.new()
	refresh.text = "Refresh"
	refresh.pressed.connect(_refresh)
	toolbar.add_child(refresh)
	_tabs = TabContainer.new()
	_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root.add_child(_tabs)
	for tab_name: String in ["Streaming & VT", "Bake & Cook", "Viewport", "Diagnostics"]:
		var scroll: ScrollContainer = ScrollContainer.new()
		scroll.name = tab_name
		scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_tabs.add_child(scroll)
		var body: VBoxContainer = VBoxContainer.new()
		body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		scroll.add_child(body)
		_pages[tab_name] = body
	_tabs.tab_changed.connect(func(_index: int) -> void: _refresh())
	_dataset_status = Label.new()
	_dataset_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_dataset_status)
	transaction = TerrainTransaction.new(plugin.get_undo_redo()) if is_instance_valid(plugin) else null
	var telemetry_timer: Timer = Timer.new()
	telemetry_timer.wait_time = 1.0
	telemetry_timer.timeout.connect(func() -> void:
		if visible:
			_refresh())
	add_child(telemetry_timer)
	telemetry_timer.start()
	if transaction != null and transaction.manager != null and transaction.manager.has_signal(&"version_changed"):
		transaction.manager.connect(&"version_changed", _on_undo_redo_version_changed)
	_rebuild()


func _on_undo_redo_version_changed() -> void:
	if visible:
		_refresh()


func set_terrain(p_terrain: Terrain3D) -> void:
	if terrain == p_terrain:
		return
	terrain = p_terrain
	_layout_dirty = true
	# Selection changes must not rebuild a hidden settings window.
	if is_node_ready() and visible:
		_rebuild()


func open_tab(p_tab: String, p_terrain: Terrain3D) -> void:
	set_terrain(p_terrain)
	if not is_node_ready():
		return
	if _layout_dirty:
		_rebuild()
	for index: int in range(_tabs.get_tab_count()):
		if _tabs.get_tab_title(index) == p_tab:
			_tabs.current_tab = index
			break
	_refresh()
	popup_centered()


func _rebuild() -> void:
	if not is_node_ready():
		return
	_layout_dirty = false
	for body: VBoxContainer in _pages.values():
		for child: Node in body.get_children():
			body.remove_child(child)
			child.queue_free()
	_rows.clear()
	_descriptors.clear()
	_status_labels.clear()
	_view_mode = null
	if not is_instance_valid(terrain):
		_dataset_status.text = "Select a Terrain3D node to edit terrain settings."
		return
	# The current repository owns its Inspector schema in setting_metadata.json.
	# Use the selected node's effective property list so hidden or retired
	# properties cannot become editable through a stale hand-maintained manifest.
	var sections: Dictionary[String, GridContainer] = {}
	var available_properties: Dictionary = {}
	var native_list: Array = terrain.get_property_list()
	for info: Dictionary in native_list:
		if int(info.get("usage", 0)) & PROPERTY_USAGE_EDITOR:
			available_properties[str(info.get("name", ""))] = true
	for native_info: Dictionary in native_list:
		var usage: int = int(native_info.get("usage", 0))
		if not (usage & PROPERTY_USAGE_EDITOR):
			continue
		var key: String = str(native_info.get("name", ""))
		if key.is_empty() or key in VIEW_MODES or key in VIEW_OVERLAYS or key == "editor_execution_mode":
			continue
		var record: Dictionary = SettingMetadata.get_record("Terrain3D", StringName(key))
		if record.is_empty() or str(record.get("visibility", "hidden")) == "hidden":
			continue
		var state: String = str(record.get("state", ""))
		if state != "editable" and state != "generated_read_only":
			continue
		var group_path: String = str(record.get("group_path", ""))
		var tab_name: String = _tab_for_group(group_path)
		if tab_name.is_empty():
			continue
		var descriptor: Dictionary = record.duplicate(true)
		descriptor["property_name"] = key
		descriptor["variant_type"] = int(native_info.get("type", TYPE_NIL))
		descriptor["hint"] = int(native_info.get("hint", PROPERTY_HINT_NONE))
		descriptor["hint_string"] = str(native_info.get("hint_string", ""))
		var read_only: bool = state == "generated_read_only" or bool(usage & PROPERTY_USAGE_READ_ONLY)
		if not read_only and not SettingControlFactory.is_supported(descriptor):
			# Unsupported hints and resource types remain in the native Inspector.
			continue
		var group: GridContainer = _get_section(sections, tab_name, group_path)
		if read_only:
			_add_read_only(group, key, record)
		else:
			_add_editable(group, key, descriptor)

	var viewport: VBoxContainer = _pages["Viewport"]
	var view_section: GridContainer = _get_section(sections, "Viewport", "Terrain Shading & Overlays")
	_add_view_selector(view_section)
	for overlay: String in VIEW_OVERLAYS:
		if not available_properties.has(overlay):
			continue
		var descriptor: Dictionary = {"variant_type": TYPE_BOOL, "hint": PROPERTY_HINT_NONE,
			"tooltip": SettingMetadata.tooltip("Terrain3D", StringName(overlay))}
		_add_editable(view_section, overlay, descriptor)
	var mode_note: Label = Label.new()
	mode_note.text = "Execution/streaming preview is controlled by the Authoring / Runtime Profile menu in the 3D viewport."
	mode_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	viewport.add_child(mode_note)
	var bake_actions: HFlowContainer = HFlowContainer.new()
	_pages["Bake & Cook"].add_child(bake_actions)
	for action_name: String in ["Inspect Cooked Package", "Cook Runtime Dataset...", "Region Lightmaps..."]:
		var button: Button = Button.new()
		button.text = action_name
		button.pressed.connect(_bake_action.bind(action_name))
		bake_actions.add_child(button)
	var status_section: GridContainer = _get_section(sections, "Diagnostics", "Dataset")
	_add_read_only(status_section, "source_region_count", {"display_name": "Source Regions"})
	_add_read_only(status_section, "cooked_package_status", {"display_name": "Cooked Dataset"})
	_refresh()


func _tab_for_group(p_group: String) -> String:
	if p_group.begins_with("Streaming & Residency") or p_group.begins_with("Memory") \
			or p_group.begins_with("Frame Scheduling") or p_group.begins_with("Runtime Scheduling") \
			or p_group.begins_with("Instance Texture Sampling"):
		return "Streaming & VT"
	if p_group.begins_with("General / Data & Authoring / Regional Lightmap Baking"):
		return "Bake & Cook"
	if p_group.begins_with("Terrain Rendering") or p_group.begins_with("Editor Visualization") \
			or p_group == "Quality & Effective Profile" or p_group == "World Rendering" \
			or p_group == "Ocean" or p_group == "Semantic Lighting":
		return "Viewport"
	if p_group.begins_with("Diagnostics & Profiling") or p_group.begins_with("Performance Profile"):
		return "Diagnostics"
	return ""


func _get_section(p_sections: Dictionary[String, GridContainer], p_tab: String, p_title: String) -> GridContainer:
	var id: String = "%s/%s" % [p_tab, p_title]
	if p_sections.has(id):
		return p_sections[id]
	var foldable: FoldableContainer = FoldableContainer.new()
	foldable.title = p_title.replace("Terrain Streaming & VT / ", "").replace("Terrain lightmap / ", "Lightmap / ")
	foldable.folded = p_title.contains("Advanced") or p_title.contains("Memory ceilings")
	_pages[p_tab].add_child(foldable)
	var margin: MarginContainer = MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 10)
	margin.add_theme_constant_override("margin_right", 10)
	margin.add_theme_constant_override("margin_bottom", 10)
	foldable.add_child(margin)
	var grid: GridContainer = GridContainer.new()
	grid.columns = 2
	grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.add_child(grid)
	p_sections[id] = grid
	return grid


func _add_read_only(p_grid: GridContainer, p_key: String, p_record: Dictionary) -> void:
	var label: Label = Label.new()
	label.text = str(p_record.get("display_name", p_key.capitalize()))
	label.tooltip_text = str(p_record.get("tooltip", ""))
	p_grid.add_child(label)
	var value: Label = Label.new()
	value.text = "—"
	value.tooltip_text = label.tooltip_text
	value.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	value.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	p_grid.add_child(value)
	_status_labels[p_key] = value


func _add_editable(p_grid: GridContainer, p_key: String, p_descriptor: Dictionary) -> void:
	var label: Label = Label.new()
	label.text = str(p_descriptor.get("display_name", p_key.capitalize()))
	label.tooltip_text = str(p_descriptor.get("tooltip", ""))
	p_grid.add_child(label)
	var control: Control = SettingControlFactory.create_control(p_descriptor,
		func(p_value: Variant) -> void: _edit(p_key, p_value))
	if control == null:
		label.queue_free()
		return
	control.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	control.tooltip_text = label.tooltip_text
	p_grid.add_child(control)
	_rows[p_key] = control
	_descriptors[p_key] = p_descriptor


func _add_view_selector(p_grid: GridContainer) -> void:
	var label: Label = Label.new()
	label.text = "Terrain Shading Mode"
	label.tooltip_text = "Choose one exclusive terrain shader-debug view. Overlays below may be enabled independently."
	p_grid.add_child(label)
	_view_mode = OptionButton.new()
	_view_mode.tooltip_text = label.tooltip_text
	_view_mode.add_item("Normal / Lit", VIEW_MODES.size())
	for index: int in range(VIEW_MODES.size()):
		_view_mode.add_item(VIEW_MODES[index].trim_prefix("show_").replace("_", " ").capitalize(), index)
	_view_mode.item_selected.connect(_on_view_mode_changed)
	_view_mode.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	p_grid.add_child(_view_mode)


func _on_view_mode_changed(p_index: int) -> void:
	if _syncing or not is_instance_valid(terrain) or transaction == null \
			or not is_instance_valid(plugin) or plugin.get_terrain() != terrain:
		return
	var chosen: int = _view_mode.get_item_id(p_index)
	var changes: Array[Dictionary] = []
	var available: Dictionary = {}
	for info: Dictionary in terrain.get_property_list():
		if int(info.get("usage", 0)) & PROPERTY_USAGE_EDITOR:
			available[str(info.get("name", ""))] = true
	for index: int in range(VIEW_MODES.size()):
		if available.has(VIEW_MODES[index]):
			changes.append({"target": terrain, "property": VIEW_MODES[index], "value": index == chosen})
	if transaction.apply_batch(changes, "Change Terrain Debug Shading"):
		_refresh.call_deferred()


func _edit(p_property: String, p_value: Variant) -> void:
	if _syncing or transaction == null or not is_instance_valid(terrain) \
			or not is_instance_valid(plugin) or plugin.get_terrain() != terrain:
		return
	if transaction.apply(terrain, p_property, p_value, "Change Terrain %s" % p_property.capitalize(), true):
		_refresh.call_deferred()


func _refresh() -> void:
	if not is_node_ready() or not is_instance_valid(_tabs):
		return
	_syncing = true
	var valid: bool = is_instance_valid(terrain) and is_instance_valid(plugin) and plugin.get_terrain() == terrain
	var native_properties: Dictionary[String, Dictionary] = {}
	if valid:
		for info: Dictionary in terrain.get_property_list():
			native_properties[str(info.get("name", ""))] = info
	for key: String in _rows:
		var control: Control = _rows[key]
		SettingControlFactory.set_control_enabled(control, valid)
		if not valid:
			continue
		var info: Dictionary = native_properties.get(key, {})
		if info.is_empty():
			SettingControlFactory.set_control_enabled(control, false)
			continue
		var read_only: bool = bool(int(info.get("usage", 0)) & PROPERTY_USAGE_READ_ONLY)
		var condition: String = _inactive_reason(key, native_properties)
		SettingControlFactory.set_control_enabled(control, not (read_only or not condition.is_empty()))
		control.tooltip_text = str(_descriptors[key].get("tooltip", "")) + ("\n" + condition if not condition.is_empty() else "")
		SettingControlFactory.sync_control(control, _descriptors[key], terrain.get(key))
	if valid:
		for key: String in _status_labels:
			if key == "source_region_count":
				_status_labels[key].text = str(terrain.data.get_region_count()) if is_instance_valid(terrain.data) else "Unavailable"
			elif key == "cooked_package_status":
				var path: String = terrain.data_directory.path_join("terrain_stream.t3sp")
				_status_labels[key].text = "Pack exists (inspect to validate)" if FileAccess.file_exists(path) else "Not cooked"
			else:
				_status_labels[key].text = str(terrain.get(key)) if native_properties.has(key) else "Unavailable"
		if is_instance_valid(_view_mode):
			var selected: int = -1
			for index: int in range(VIEW_MODES.size()):
				if native_properties.has(VIEW_MODES[index]) and bool(terrain.get(VIEW_MODES[index])):
					selected = index
					break
			var selected_index: int = _view_mode.get_item_index(selected if selected >= 0 else VIEW_MODES.size())
			if selected_index >= 0:
				_view_mode.select(selected_index)
		_dataset_status.text = "Source: %s" % terrain.data_directory
	else:
		_dataset_status.text = "Terrain selection is no longer active."
	if is_instance_valid(_view_mode):
		SettingControlFactory.set_control_enabled(_view_mode, valid)
	_syncing = false


func _inactive_reason(p_key: String, p_native_properties: Dictionary[String, Dictionary]) -> String:
	if p_key in ["gpu_region_cache_budget_mb", "cpu_region_cache_budget_mb", "lightmap_residency_budget_mb"]:
		var auto_key: String = p_key.trim_suffix("_mb") + "_auto"
		if p_native_properties.has(auto_key) and bool(terrain.get(auto_key)):
			return "Auto budget is active; change the Auto setting to edit the manual request."
	if p_key.begins_with("streaming_prediction_") and p_key != "streaming_prediction_enabled" \
			and p_native_properties.has("streaming_prediction_enabled") and not terrain.streaming_prediction_enabled:
		return "Enable Streaming Prediction to use this setting."
	return ""


func _bake_action(p_name: String) -> void:
	if not is_instance_valid(plugin) or not is_instance_valid(terrain) or plugin.get_terrain() != terrain:
		return
	if p_name == "Inspect Cooked Package":
		if not is_instance_valid(terrain.data):
			_dataset_status.text = "No terrain dataset loaded."
			return
		var inspection: Dictionary = terrain.data.inspect_gpu_region_pages()
		_dataset_status.text = "%s  |  valid regions %d/%d  |  missing %d  |  package %s" % [
			str(inspection.get("package_path", "")), int(inspection.get("current", 0)),
			int(inspection.get("discovered", 0)), int(inspection.get("missing", 0)),
			"ready" if bool(inspection.get("package_ready", false)) else "missing / invalid"]
	elif p_name == "Cook Runtime Dataset...":
		plugin.open_runtime_dataset_cook_confirmation()
	elif p_name == "Region Lightmaps...":
		plugin.open_region_lightmap_baker()
