# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Metadata-driven runtime render quality controls.
@tool
extends PopupPanel

const EditorUI = preload("res://addons/terrain_3d/src/editor_ui_util.gd")
const SettingMetadata = preload("res://addons/terrain_3d/src/setting_metadata.gd")
const SettingControlFactory = preload("res://addons/terrain_3d/src/setting_control_factory.gd")

const TERRAIN_GLOBAL_GROUPS = [
	"Terrain Rendering / Displacement / Tessellation",
]
const OCEAN_GROUPS = ["Ocean"]

var plugin: Terrain3DEditorPluginBase
var _syncing: bool = false
var _controls: Array[Control] = []
var _bindings: Dictionary[String, Control] = {}
var _binding_descriptors: Dictionary[String, Dictionary] = {}
var _status: Label
var _search_box: LineEdit

class SearchRow extends RefCounted:
	var grid: GridContainer
	var property: String = ""
	var label: String = ""
	var tooltip: String = ""
	var label_control: Control
	var control: Control


var _search_rows: Array[SearchRow] = []
var _groups: Array[FoldableContainer] = []


func _ready() -> void:
	_build_ui()
	about_to_popup.connect(sync_from_material)


func open() -> void:
	sync_from_material()
	var preferred := Vector2i(int(EditorUI.scaled(980.0)), int(EditorUI.scaled(780.0)))
	var minimum := Vector2i(int(EditorUI.scaled(680.0)), int(EditorUI.scaled(500.0)))
	popup_centered(EditorUI.popup_size_for_screen(self, preferred, minimum))


func _build_ui() -> void:
	var margin: MarginContainer = MarginContainer.new()
	for side: StringName in [&"margin_left", &"margin_top", &"margin_right", &"margin_bottom"]:
		margin.add_theme_constant_override(side, 12)
	add_child(margin)

	var root: VBoxContainer = VBoxContainer.new()
	root.custom_minimum_size = Vector2(EditorUI.scaled(640.0), 0)
	margin.add_child(root)

	var title: Label = Label.new()
	title.text = "Runtime Render Quality"
	title.add_theme_font_size_override("font_size", 17)
	root.add_child(title)

	var intro: Label = Label.new()
	intro.text = "Controls are generated from the authoritative Terrain3D setting schema. Profile-owned edits remain serialized overrides and immediately update Exact/Modified profile state."
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(intro)

	var search_row := HBoxContainer.new()
	search_row.add_theme_constant_override("separation", 6)
	root.add_child(search_row)
	_search_box = LineEdit.new()
	_search_box.placeholder_text = "Filter render settings"
	_search_box.tooltip_text = "Filter the documented render/performance setting list by name or metadata. This changes the editor view only."
	_search_box.clear_button_enabled = true
	_search_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	EditorUI.configure_compact_control(_search_box)
	_search_box.text_changed.connect(_on_search_changed)
	search_row.add_child(_search_box)
	var reset_filter := Button.new()
	reset_filter.text = "Clear"
	reset_filter.tooltip_text = "Clear the setting filter."
	EditorUI.configure_action_button(reset_filter)
	reset_filter.pressed.connect(func() -> void: _search_box.clear())
	search_row.add_child(reset_filter)

	var tabs: TabContainer = TabContainer.new()
	tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root.add_child(tabs)
	_build_terrain_tab(tabs)
	_build_ocean_tab(tabs)
	_build_reference_tab(tabs)

	root.add_child(HSeparator.new())
	_status = Label.new()
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status.custom_minimum_size.y = EditorUI.scaled(50.0)
	_status.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_status.tooltip_text = "Current profile and direct-RD shader catalog readiness."
	root.add_child(_status)

	var actions: HBoxContainer = HBoxContainer.new()
	actions.add_theme_constant_override("separation", int(EditorUI.scaled(6.0)))
	root.add_child(actions)
	var action_spacer := Control.new()
	action_spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	actions.add_child(action_spacer)
	var close: Button = Button.new()
	close.text = "Close"
	EditorUI.configure_action_button(close)
	close.pressed.connect(hide)
	actions.add_child(close)


func _build_terrain_tab(p_tabs: TabContainer) -> void:
	var body: VBoxContainer = _make_tab_body(p_tabs, "Terrain")
	_add_descriptor_groups(body, "Terrain3DMaterial")
	_add_descriptor_groups(body, "Terrain3D", TERRAIN_GLOBAL_GROUPS)


func _build_ocean_tab(p_tabs: TabContainer) -> void:
	var body: VBoxContainer = _make_tab_body(p_tabs, "Ocean")
	_add_descriptor_groups(body, "Terrain3D", OCEAN_GROUPS)


func _build_reference_tab(p_tabs: TabContainer) -> void:
	var reference: RichTextLabel = RichTextLabel.new()
	reference.name = "Reference"
	reference.fit_content = false
	reference.bbcode_enabled = true
	reference.scroll_active = true
	var lines: PackedStringArray = PackedStringArray([
		"[b]Authoritative operational setting reference[/b]",
		"The text below is generated from the same metadata used by the Inspector and Runtime Profiler.",
	])
	var descriptors: Array[Dictionary] = SettingMetadata.descriptors_for_class("Terrain3DMaterial")
	descriptors.append_array(SettingMetadata.descriptors_for_class("Terrain3D", TERRAIN_GLOBAL_GROUPS))
	descriptors.append_array(SettingMetadata.descriptors_for_class("Terrain3D", OCEAN_GROUPS))
	for descriptor: Dictionary in descriptors:
		if not SettingControlFactory.is_supported(descriptor):
			continue
		lines.append("\n[b]%s[/b]\n%s" % [str(descriptor.get("display_name", descriptor.get("property_name", ""))), str(descriptor.get("tooltip", ""))])
	reference.text = "\n".join(lines)
	p_tabs.add_child(reference)


func _make_tab_body(p_tabs: TabContainer, p_name: String) -> VBoxContainer:
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.name = p_name
	EditorUI.configure_scroll_container(scroll)
	p_tabs.add_child(scroll)
	var body: VBoxContainer = VBoxContainer.new()
	body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	body.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	scroll.add_child(body)
	return body


func _add_descriptor_groups(p_root: VBoxContainer, p_class_name: String, p_prefixes: Array = []) -> void:
	for group: Dictionary in SettingControlFactory.supported_groups(p_class_name, p_prefixes):
		var group_path: String = str(group.get("group_path", "General"))
		var grid: GridContainer = _add_group(p_root, group_path)
		for descriptor: Dictionary in group.get("settings", []):
			_add_descriptor_row(grid, descriptor)


func _add_group(p_root: VBoxContainer, p_title: String) -> GridContainer:
	var foldable := FoldableContainer.new()
	foldable.title = p_title
	foldable.folded = p_title.contains("Advanced") or p_title in ["PBR Output / Debug", "Terrain Rendering / Displacement / Tessellation"]
	foldable.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	foldable.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	p_root.add_child(foldable)
	_groups.append(foldable)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", int(EditorUI.scaled(8.0)))
	margin.add_theme_constant_override("margin_top", int(EditorUI.scaled(4.0)))
	margin.add_theme_constant_override("margin_right", int(EditorUI.scaled(8.0)))
	margin.add_theme_constant_override("margin_bottom", int(EditorUI.scaled(8.0)))
	foldable.add_child(margin)

	var grid := GridContainer.new()
	EditorUI.configure_form_grid(grid)
	grid.set_meta("terrain3d_foldable", foldable)
	margin.add_child(grid)
	return grid


func _add_descriptor_row(p_grid: GridContainer, p_descriptor: Dictionary) -> void:
	var property_name: String = str(p_descriptor.get("property_name", ""))
	var key: String = str(p_descriptor.get("key", ""))
	var label_text: String = _row_label(p_descriptor)
	var tooltip: String = SettingControlFactory.tooltip_with_mode(p_descriptor)
	var label: Label = Label.new()
	label.text = label_text
	label.tooltip_text = tooltip
	EditorUI.configure_form_label(label)
	p_grid.add_child(label)
	var control: Control = SettingControlFactory.create_control(p_descriptor, func(p_value: Variant) -> void: _set_descriptor_value(p_descriptor, p_value))
	if control == null:
		label.queue_free()
		return
	control.tooltip_text = tooltip
	control.set_meta("terrain3d_setting_key", key)
	control.set_meta("terrain3d_property", property_name)
	control.set_meta("terrain3d_tier", str(p_descriptor.get("tier", "global")))
	control.set_meta("terrain3d_mutation_mode", str(p_descriptor.get("mutation_mode", "serialized_configuration")))
	EditorUI.configure_field_control(control, 250.0)
	p_grid.add_child(control)
	_bindings[key] = control
	_binding_descriptors[key] = p_descriptor
	_controls.append(control)
	_register_search_row(p_grid, property_name, label_text, label, control, tooltip)


func _row_label(p_descriptor: Dictionary) -> String:
	var label: String = str(p_descriptor.get("display_name", p_descriptor.get("property_name", "")))
	var tier: String = str(p_descriptor.get("tier", "global"))
	if tier in ["near", "middle", "far"]:
		var prefix: String = tier.capitalize() + " "
		if label.begins_with(prefix):
			return label.trim_prefix(prefix)
	return label


func _register_search_row(p_grid: GridContainer, p_property: String, p_label: String, p_label_control: Control, p_control: Control, p_tooltip: String) -> void:
	var row := SearchRow.new()
	row.grid = p_grid
	row.property = p_property
	row.label = p_label
	row.tooltip = p_tooltip
	row.label_control = p_label_control
	row.control = p_control
	_search_rows.append(row)


func _on_search_changed(p_query: String) -> void:
	var query: String = p_query.strip_edges().to_lower()
	for row: SearchRow in _search_rows:
		var searchable: String = "%s %s %s" % [row.label, row.property, row.tooltip]
		var row_visible: bool = query.is_empty() or searchable.to_lower().contains(query)
		if is_instance_valid(row.label_control):
			row.label_control.visible = row_visible
		if is_instance_valid(row.control):
			row.control.visible = row_visible
	for group: FoldableContainer in _groups:
		if query.is_empty():
			group.visible = true
			continue
		var group_has_match: bool = false
		for row: SearchRow in _search_rows:
			if row.grid.get_meta("terrain3d_foldable", null) == group and is_instance_valid(row.control) and row.control.visible:
				group_has_match = true
				break
		group.visible = group_has_match
		if group_has_match:
			group.folded = false


func _get_terrain() -> Terrain3D:
	if not is_instance_valid(plugin):
		return null
	return plugin.terrain


func _get_material() -> Terrain3DMaterial:
	var terrain_node: Terrain3D = _get_terrain()
	return terrain_node.material if is_instance_valid(terrain_node) else null


func sync_from_material() -> void:
	var terrain_node: Terrain3D = _get_terrain()
	var material: Terrain3DMaterial = _get_material()
	_syncing = true
	if not is_instance_valid(terrain_node) or not is_instance_valid(material):
		_status.text = "Select a Terrain3D node to edit runtime shader quality."
		_set_controls_enabled(false)
		_syncing = false
		return
	_set_controls_enabled(true)
	for key: String in _bindings:
		var descriptor: Dictionary = _binding_descriptors[key]
		var target: Object = material if str(descriptor.get("class_name", "")) == "Terrain3DMaterial" else terrain_node
		SettingControlFactory.sync_control(_bindings[key], descriptor, target.get(str(descriptor.get("property_name", ""))))
	var diagnostics: Dictionary = terrain_node.get_runtime_diagnostics(false)
	var shader_diagnostics: Dictionary = diagnostics.get("shaders", {})
	_status.text = (
		"Profile: %s — %s. RD shaders: %d/%d physical ready, %d/%d SPIR-V sources ready.\n"
		+ "Startup stage: %s. Post-ready cache misses: %d."
	) % [
		terrain_node.get_performance_profile_manifest().get("display_name", "Custom"),
		terrain_node.get_performance_profile_status_name(),
		int(shader_diagnostics.get("physical_shader_ready", 0)),
		int(shader_diagnostics.get("physical_shader_expected", 0)),
		int(shader_diagnostics.get("shader_source_ready", 0)),
		int(shader_diagnostics.get("shader_source_count", 0)),
		str(shader_diagnostics.get("startup_stage_name", "UNINITIALIZED")),
		int(shader_diagnostics.get("physical_shader_misses_after_ready", 0)),
	]
	_syncing = false


func _set_controls_enabled(p_enabled: bool) -> void:
	for control: Control in _controls:
		control.set("disabled", not p_enabled)


func _set_descriptor_value(p_descriptor: Dictionary, p_value: Variant) -> void:
	if _syncing:
		return
	var terrain_node: Terrain3D = _get_terrain()
	var material: Terrain3DMaterial = _get_material()
	if not is_instance_valid(terrain_node) or not is_instance_valid(material):
		return
	var target: Object = material if str(p_descriptor.get("class_name", "")) == "Terrain3DMaterial" else terrain_node
	target.set(str(p_descriptor.get("property_name", "")), p_value)
	if target == material:
		material.emit_changed()
	terrain_node.notify_property_list_changed()
	EditorInterface.mark_scene_as_unsaved()
	sync_from_material.call_deferred()
