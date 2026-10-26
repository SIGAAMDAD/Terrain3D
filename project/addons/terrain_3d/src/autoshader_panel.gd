# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Editor-first Terrain3D autoshader authoring, preview, bake, and stale-state UI.
@tool
extends PopupPanel


const EditorUI = preload("res://addons/terrain_3d/src/editor_ui_util.gd")
const AutoShaderProfile = preload("res://addons/terrain_3d/src/autoshader_profile.gd")
const AutoShaderRule = preload("res://addons/terrain_3d/src/autoshader_rule.gd")
const AuthoringDependencies = preload("res://addons/terrain_3d/tools/authoring_dependencies.gd")

enum BakeScope {
	SELECTED_REGION,
	RESIDENT_REGIONS,
	ALL_REGIONS,
	DIRTY_REGIONS,
}

var plugin: Terrain3DEditorPluginBase
var _terrain: Terrain3D
var _profile: Terrain3DAutoShaderProfile
var _baker: Terrain3DAutoShaderBaker
var _connected_data: Terrain3DData
var _connected_assets: Terrain3DAssets
var _dirty_regions: Dictionary[Vector2i, bool] = {}
var _suppress_dirty_tracking: bool = false
var _suppress_profile_change_tracking: bool = false
var _display_rule_indices: Array[int] = []
var _preview_snapshot: Dictionary[Vector2i, Image] = {}
var _preview_auto_shader_enabled: bool = false
var _auto_rebake_timer: Timer
var _region_reconcile_scheduled: bool = false

var _profile_picker: EditorResourcePicker
var _mode: OptionButton
var _rule_list: ItemList
var _rule_summary: RichTextLabel
var _scope: OptionButton
var _status: Label
var _stale_status: Label
var _preview_button: Button
var _revert_button: Button
var _bake_button: Button
var _clear_button: Button
var _save_dialog: EditorFileDialog


func _ready() -> void:
	_baker = Terrain3DAutoShaderBaker.new()
	_build_ui()
	_auto_rebake_timer = Timer.new()
	_auto_rebake_timer.one_shot = true
	_auto_rebake_timer.timeout.connect(_on_auto_rebake_timeout)
	add_child(_auto_rebake_timer)
	about_to_popup.connect(_sync_from_terrain)
	popup_hide.connect(_on_popup_hidden)


func open() -> void:
	set_terrain(plugin.terrain if is_instance_valid(plugin) else null)
	if is_instance_valid(_terrain) and not is_instance_valid(_profile):
		_create_default_profile(false)
	_sync_from_terrain()
	if is_instance_valid(_profile) and _profile.mode == Terrain3DAutoShaderProfile.Mode.EDITOR_PREVIEW:
		_refresh_editor_preview.call_deferred()
	var preferred := Vector2i(int(EditorUI.scaled(980.0)), int(EditorUI.scaled(720.0)))
	var minimum := Vector2i(int(EditorUI.scaled(720.0)), int(EditorUI.scaled(480.0)))
	popup_centered(EditorUI.popup_size_for_screen(self, preferred, minimum))


func set_terrain(p_terrain: Terrain3D) -> void:
	if _terrain == p_terrain:
		return
	_cancel_auto_rebake()
	_revert_preview(false)
	_disconnect_sources()
	_terrain = p_terrain
	_profile = null
	_dirty_regions.clear()
	if is_instance_valid(_terrain):
		_set_profile(_terrain.auto_shader_profile as Terrain3DAutoShaderProfile, false, false)
		_connected_data = _terrain.data
		_connected_assets = _terrain.assets
		if is_instance_valid(_connected_data):
			if not _connected_data.maps_edit_committed.is_connected(_on_map_edit_committed):
				_connected_data.maps_edit_committed.connect(_on_map_edit_committed)
			if not _connected_data.region_map_changed.is_connected(_on_region_map_changed):
				_connected_data.region_map_changed.connect(_on_region_map_changed)
		if is_instance_valid(_connected_assets) and not _connected_assets.textures_changed.is_connected(_on_textures_changed):
			_connected_assets.textures_changed.connect(_on_textures_changed)
	_mark_unbaked_regions_dirty()
	_refresh_ui()


func shutdown() -> void:
	_cancel_auto_rebake()
	_revert_preview(false)
	_disconnect_sources()
	_terrain = null
	_profile = null
	_dirty_regions.clear()


func _cancel_auto_rebake() -> void:
	if is_instance_valid(_auto_rebake_timer):
		_auto_rebake_timer.stop()


func _disconnect_sources() -> void:
	if is_instance_valid(_connected_data):
		if _connected_data.maps_edit_committed.is_connected(_on_map_edit_committed):
			_connected_data.maps_edit_committed.disconnect(_on_map_edit_committed)
		if _connected_data.region_map_changed.is_connected(_on_region_map_changed):
			_connected_data.region_map_changed.disconnect(_on_region_map_changed)
	if is_instance_valid(_connected_assets) and _connected_assets.textures_changed.is_connected(_on_textures_changed):
		_connected_assets.textures_changed.disconnect(_on_textures_changed)
	if is_instance_valid(_profile) and _profile.changed.is_connected(_on_profile_changed):
		_profile.changed.disconnect(_on_profile_changed)
	_connected_data = null
	_connected_assets = null


func _build_ui() -> void:
	var margin := MarginContainer.new()
	for side: StringName in [&"margin_left", &"margin_top", &"margin_right", &"margin_bottom"]:
		margin.add_theme_constant_override(side, 12)
	add_child(margin)

	var root := VBoxContainer.new()
	root.custom_minimum_size = Vector2(EditorUI.scaled(680.0), 0.0)
	root.add_theme_constant_override("separation", 8)
	margin.add_child(root)

	var title := Label.new()
	title.text = "Terrain Material Rules"
	title.add_theme_font_size_override("font_size", 17)
	root.add_child(title)

	var intro := Label.new()
	intro.text = "Author automatic terrain materials as ordered rules. Preview the expanded rules in the editor, then bake ordinary control maps for a zero-classification-cost runtime path."
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(intro)

	var profile_row := HBoxContainer.new()
	profile_row.add_theme_constant_override("separation", 6)
	root.add_child(profile_row)
	var profile_label := Label.new()
	profile_label.text = "Profile"
	profile_label.custom_minimum_size.x = EditorUI.scaled(80.0)
	profile_row.add_child(profile_label)
	_profile_picker = EditorResourcePicker.new()
	_profile_picker.base_type = "Terrain3DAutoShaderProfile"
	_profile_picker.tooltip_text = "Terrain3DAutoShaderProfile resource that owns the ordered material rules, masks, and automatic classification workflow for this terrain."
	_profile_picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_profile_picker.resource_changed.connect(_on_profile_resource_changed)
	profile_row.add_child(_profile_picker)
	_add_button(profile_row, "New", "Create a new material-rule profile for this terrain.", _on_new_profile)
	_add_button(profile_row, "Save", "Save the current external profile resource.", _save_profile_now)
	_add_button(profile_row, "Save As…", "Save the profile as a reusable .tres resource.", _on_save_profile)
	_add_button(profile_row, "Advanced", "Edit masks, derived metrics, and profile-wide settings in the Inspector.", _inspect_profile)

	var workflow_grid := GridContainer.new()
	EditorUI.configure_form_grid(workflow_grid)
	root.add_child(workflow_grid)
	_add_form_label(workflow_grid, "Workflow", "Live Editor Preview evaluates these rules temporarily. Baked Production commits ordinary control maps. Legacy Runtime retains the old two-texture fragment classifier.")
	_mode = OptionButton.new()
	_mode.add_item("Live Editor Preview", Terrain3DAutoShaderProfile.Mode.EDITOR_PREVIEW)
	_mode.add_item("Baked Production", Terrain3DAutoShaderProfile.Mode.BAKED)
	_mode.add_item("Legacy Runtime (two-texture)", Terrain3DAutoShaderProfile.Mode.LEGACY_RUNTIME)
	_mode.add_item("Manual / Disabled", Terrain3DAutoShaderProfile.Mode.DISABLED)
	_mode.tooltip_text = "Chooses how automatic material rules are evaluated: temporary editor preview, baked production control maps, legacy runtime classification, or disabled/manual painting."
	_mode.item_selected.connect(_on_mode_selected)
	EditorUI.configure_field_control(_mode)
	workflow_grid.add_child(_mode)

	var split := HSplitContainer.new()
	split.size_flags_vertical = Control.SIZE_EXPAND_FILL
	split.split_offset = int(EditorUI.scaled(330.0))
	root.add_child(split)

	var rules_panel := VBoxContainer.new()
	rules_panel.add_theme_constant_override("separation", 6)
	split.add_child(rules_panel)
	var rules_title := Label.new()
	rules_title.text = "Material rules · highest priority first"
	rules_panel.add_child(rules_title)
	_rule_list = ItemList.new()
	_rule_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_rule_list.custom_minimum_size = Vector2(EditorUI.scaled(300.0), EditorUI.scaled(250.0))
	_rule_list.item_selected.connect(func(_index: int) -> void: _update_rule_summary())
	_rule_list.item_activated.connect(func(_index: int) -> void: _inspect_selected_rule())
	rules_panel.add_child(_rule_list)
	var rule_buttons := HFlowContainer.new()
	rules_panel.add_child(rule_buttons)
	var add_menu := MenuButton.new()
	add_menu.text = "Add Rule"
	add_menu.tooltip_text = "Add a blank rule or a useful starting template."
	EditorUI.configure_action_button(add_menu)
	add_menu.get_popup().add_item("Blank rule", 0)
	add_menu.get_popup().add_item("Steep rock", 1)
	add_menu.get_popup().add_item("High elevation", 2)
	add_menu.get_popup().add_item("Depositional channel", 3)
	add_menu.get_popup().add_item("Shoreline", 4)
	add_menu.get_popup().add_item("Directional exposure", 5)
	add_menu.get_popup().id_pressed.connect(_on_rule_template_selected)
	rule_buttons.add_child(add_menu)
	_add_button(rule_buttons, "Enable / Disable", "Toggle the selected rule without deleting it.", _toggle_rule_enabled)
	_add_button(rule_buttons, "Duplicate", "Duplicate the selected rule.", _duplicate_rule)
	_add_button(rule_buttons, "Remove", "Remove the selected rule.", _remove_rule)
	_add_button(rule_buttons, "Up", "Increase evaluation priority by moving the rule earlier.", func() -> void: _move_rule(-1))
	_add_button(rule_buttons, "Down", "Decrease evaluation priority by moving the rule later.", func() -> void: _move_rule(1))
	_add_button(rule_buttons, "Edit Rule", "Open the selected rule in the Inspector.", _inspect_selected_rule)

	_rule_summary = RichTextLabel.new()
	_rule_summary.fit_content = false
	_rule_summary.bbcode_enabled = true
	_rule_summary.custom_minimum_size = Vector2(EditorUI.scaled(320.0), EditorUI.scaled(250.0))
	_rule_summary.text = "Select a rule to see its material output and active conditions. Double-click it to edit the full rule in the Inspector."
	split.add_child(_rule_summary)

	root.add_child(HSeparator.new())
	var bake_grid := GridContainer.new()
	EditorUI.configure_form_grid(bake_grid)
	root.add_child(bake_grid)
	_add_form_label(bake_grid, "Bake scope", "Selected uses the region under the editor cursor. Resident targets current texture-array slots. Dirty targets regions invalidated by edits or profile changes.")
	_scope = OptionButton.new()
	_scope.add_item("Selected region", BakeScope.SELECTED_REGION)
	_scope.add_item("Resident / visible regions", BakeScope.RESIDENT_REGIONS)
	_scope.add_item("All active regions", BakeScope.ALL_REGIONS)
	_scope.add_item("Dirty regions", BakeScope.DIRTY_REGIONS)
	_scope.select(BakeScope.DIRTY_REGIONS)
	_scope.tooltip_text = "Chooses which terrain regions the next autoshader preview, bake, clear, or dirty-state operation targets."
	EditorUI.configure_field_control(_scope)
	bake_grid.add_child(_scope)

	_stale_status = Label.new()
	_stale_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_stale_status)

	var actions := HFlowContainer.new()
	actions.add_theme_constant_override("h_separation", 6)
	root.add_child(actions)
	_preview_button = _add_button(actions, "Preview Selected Scope", "Evaluate rules temporarily. Revert restores the exact previous control maps.", _preview_bake)
	_revert_button = _add_button(actions, "Revert Preview", "Restore control maps captured before the temporary rule preview.", func() -> void: _revert_preview(true))
	_bake_button = _add_button(actions, "Bake Production Maps", "Commit rule output as ordinary control maps with editor undo support.", _commit_bake)
	_clear_button = _add_button(actions, "Clear Baked Auto Pixels", "Reset material fields on auto-marked pixels while preserving unrelated control flags.", _clear_bake)
	_add_button(actions, "Mark All Dirty", "Invalidate all active regions for the next dirty-region bake.", _mark_all_dirty)

	_status = Label.new()
	_status.custom_minimum_size.y = EditorUI.scaled(42.0)
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_status)

	var footer := HBoxContainer.new()
	root.add_child(footer)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	footer.add_child(spacer)
	_add_button(footer, "Close", "Close the autoshader baker.", hide)

	_save_dialog = EditorFileDialog.new()
	_save_dialog.file_mode = EditorFileDialog.FILE_MODE_SAVE_FILE
	_save_dialog.access = EditorFileDialog.ACCESS_RESOURCES
	_save_dialog.add_filter("*.tres", "Godot Resource")
	_save_dialog.title = "Save Terrain3D Material Rule Profile"
	_save_dialog.file_selected.connect(_on_profile_file_selected)
	add_child(_save_dialog)


func _add_button(parent: Control, text: String, tooltip: String, callback: Callable) -> Button:
	var button := Button.new()
	button.text = text
	button.tooltip_text = tooltip
	EditorUI.configure_action_button(button)
	button.pressed.connect(callback)
	parent.add_child(button)
	return button


func _add_form_label(parent: Control, text: String, tooltip: String) -> void:
	var label := Label.new()
	label.text = text
	label.tooltip_text = tooltip
	EditorUI.configure_form_label(label)
	parent.add_child(label)


func _sync_from_terrain() -> void:
	if is_instance_valid(plugin) and plugin.terrain != _terrain:
		set_terrain(plugin.terrain)
	if is_instance_valid(_terrain):
		var terrain_profile := _terrain.auto_shader_profile as Terrain3DAutoShaderProfile
		if terrain_profile != _profile:
			_set_profile(terrain_profile, false, false)
		_mark_unbaked_regions_dirty()
	_refresh_ui()


func _refresh_ui() -> void:
	var has_terrain := is_instance_valid(_terrain) and is_instance_valid(_terrain.data)
	if is_instance_valid(_profile_picker):
		_profile_picker.edited_resource = _profile
	var has_profile := has_terrain and is_instance_valid(_profile)
	if is_instance_valid(_mode):
		_mode.disabled = not has_profile
		if has_profile:
			var mode_index := _mode.get_item_index(int(_profile.mode))
			if mode_index >= 0:
				_mode.select(mode_index)
	if is_instance_valid(_rule_list):
		var previous_rule_index := _selected_rule_index()
		_rule_list.clear()
		_display_rule_indices.clear()
		if has_profile:
			_display_rule_indices = _get_ordered_rule_indices()
			for rule_index: int in _display_rule_indices:
				var rule := _profile.rules[rule_index]
				if not is_instance_valid(rule):
					continue
				var state := "" if rule.enabled else " · disabled"
				var text := "%s · %s · %s%s" % [
					rule.get_display_name(),
					_rule_output_summary(rule),
					rule.get_condition_summary(),
					state,
				]
				_rule_list.add_item(text)
				_rule_list.set_item_tooltip(_rule_list.item_count - 1,
					"Priority %d\nOutput: %s\nConditions: %s" % [
						rule.priority, _rule_output_summary(rule), rule.get_condition_summary()
					])
			if _rule_list.item_count > 0:
				var display_index := _display_rule_indices.find(previous_rule_index)
				_rule_list.select(display_index if display_index >= 0 else 0)
	_update_rule_summary()
	for button: Button in [_preview_button, _bake_button, _clear_button]:
		if is_instance_valid(button):
			button.disabled = not has_profile
	if is_instance_valid(_preview_button):
		_preview_button.text = "Refresh Live Preview" if has_profile and \
			_profile.mode == Terrain3DAutoShaderProfile.Mode.EDITOR_PREVIEW else "Preview Selected Scope"
	if is_instance_valid(_revert_button):
		_revert_button.disabled = _preview_snapshot.is_empty()
	_update_stale_status()
	if not has_terrain:
		_set_status("Select a Terrain3D node before editing material rules.")
	elif not has_profile:
		_set_status("Create or assign a material-rule profile.")


func _set_profile(
		p_profile: Terrain3DAutoShaderProfile,
		p_assign_to_terrain: bool = true,
		p_mark_dirty: bool = true,
) -> void:
	if _profile == p_profile:
		return
	_cancel_auto_rebake()
	if is_instance_valid(_profile) and _profile.changed.is_connected(_on_profile_changed):
		_profile.changed.disconnect(_on_profile_changed)
	_profile = p_profile
	if is_instance_valid(_profile):
		_profile.sync_dependency_connections()
		if not _profile.changed.is_connected(_on_profile_changed):
			_profile.changed.connect(_on_profile_changed)
	if p_assign_to_terrain and is_instance_valid(_terrain):
		_terrain.auto_shader_profile = _profile
		_terrain.notify_property_list_changed()
		EditorInterface.mark_scene_as_unsaved()
	if p_mark_dirty:
		_mark_all_dirty(false)
	_refresh_ui()


func _create_default_profile(report: bool = true) -> void:
	if not is_instance_valid(_terrain):
		if report:
			_set_status("Select a Terrain3D node first.")
		return
	var profile := AutoShaderProfile.new() as Terrain3DAutoShaderProfile
	profile.ensure_default_rules()
	_configure_default_rule_outputs(profile)
	_set_profile(profile)
	if report:
		_set_status("Created steep-rock, deposition, and fallback rules. Set their texture IDs, preview, then bake.")


func _on_new_profile() -> void:
	_create_default_profile(true)


func _on_profile_resource_changed(resource: Resource) -> void:
	if resource == null:
		_set_profile(null)
		return
	if not resource is Terrain3DAutoShaderProfile:
		_set_status("The selected resource is not a Terrain3DAutoShaderProfile.")
		_profile_picker.edited_resource = _profile
		return
	_set_profile(resource as Terrain3DAutoShaderProfile)


func _on_save_profile() -> void:
	if not is_instance_valid(_profile):
		return
	_save_dialog.current_file = "terrain_autoshader_profile.tres"
	_save_dialog.popup_centered_ratio(0.7)


func _save_profile_now() -> void:
	if not is_instance_valid(_profile):
		return
	if _profile.resource_path.is_empty() or not _profile.resource_path.begins_with("res://"):
		_on_save_profile()
		return
	var error := ResourceSaver.save(_profile)
	_set_status("Saved %s." % _profile.resource_path if error == OK else "Failed to save profile: error %d." % error)


func _on_profile_file_selected(path: String) -> void:
	if not is_instance_valid(_profile):
		return
	var error := ResourceSaver.save(_profile, path)
	if error == OK and _profile.resource_path != path:
		_profile.take_over_path(path)
	_set_status("Saved profile to %s." % path if error == OK else "Failed to save profile: error %d." % error)


func _inspect_profile() -> void:
	if is_instance_valid(_profile):
		EditorInterface.inspect_object(_profile)


func _get_ordered_rule_indices() -> Array[int]:
	var indices: Array[int] = []
	if not is_instance_valid(_profile):
		return indices
	for index: int in _profile.rules.size():
		if is_instance_valid(_profile.rules[index]):
			indices.append(index)
	indices.sort_custom(func(a: int, b: int) -> bool:
		var rule_a: Terrain3DAutoShaderRule = _profile.rules[a]
		var rule_b: Terrain3DAutoShaderRule = _profile.rules[b]
		return rule_a.priority > rule_b.priority if rule_a.priority != rule_b.priority else a < b
	)
	return indices


func _selected_rule_index() -> int:
	if not is_instance_valid(_rule_list):
		return -1
	var selected := _rule_list.get_selected_items()
	if selected.is_empty():
		return -1
	var display_index: int = selected[0]
	return _display_rule_indices[display_index] if display_index >= 0 and display_index < _display_rule_indices.size() else -1


func _normalize_rule_priorities() -> void:
	var ordered := _get_ordered_rule_indices()
	for display_index: int in ordered.size():
		_profile.rules[ordered[display_index]].priority = (ordered.size() - display_index) * 100


func _on_rule_template_selected(template_id: int) -> void:
	if not is_instance_valid(_profile):
		return
	var rule := _create_rule_template(template_id)
	_profile.begin_change_batch()
	_profile.add_rule(rule)
	_normalize_rule_priorities()
	_profile.end_change_batch()
	EditorInterface.mark_scene_as_unsaved()
	_refresh_ui()
	var actual_index := _profile.rules.find(rule)
	var display_index := _display_rule_indices.find(actual_index)
	if display_index >= 0:
		_rule_list.select(display_index)
	_update_rule_summary()


func _toggle_rule_enabled() -> void:
	var index := _selected_rule_index()
	if not is_instance_valid(_profile) or index < 0 or index >= _profile.rules.size():
		return
	var rule: Terrain3DAutoShaderRule = _profile.rules[index]
	rule.enabled = not rule.enabled
	EditorInterface.mark_scene_as_unsaved()
	_refresh_ui()


func _create_rule_template(template_id: int) -> Terrain3DAutoShaderRule:
	var rule := AutoShaderRule.new() as Terrain3DAutoShaderRule
	rule.rule_name = "Material rule %d" % (_profile.rules.size() + 1)
	match template_id:
		1:
			rule.rule_name = "Steep rock"
			rule.use_slope = true
			rule.slope_min = 42.0
			rule.slope_falloff = 6.0
			rule.blend = 1.0
		2:
			rule.rule_name = "High elevation"
			rule.use_height = true
			rule.height_min = 900.0
			rule.height_falloff = 120.0
			rule.blend = 1.0
		3:
			rule.rule_name = "Depositional channel"
			rule.use_deposition = true
			rule.deposition_min = 0.25
			rule.deposition_falloff = 0.1
			rule.blend = 0.75
		4:
			rule.rule_name = "Shoreline"
			rule.use_water_distance = true
			rule.water_distance_max = 12.0
			rule.water_distance_falloff = 8.0
			rule.blend = 1.0
		5:
			rule.rule_name = "Directional exposure"
			rule.use_aspect = true
			rule.aspect_center = 180.0
			rule.aspect_width = 120.0
			rule.aspect_falloff = 25.0
			rule.blend = 0.65
	return rule


func _duplicate_rule() -> void:
	var index := _selected_rule_index()
	if not is_instance_valid(_profile) or index < 0 or index >= _profile.rules.size():
		return
	var copy := _profile.rules[index].duplicate(true) as Terrain3DAutoShaderRule
	copy.rule_name += " copy"
	copy.priority = _profile.rules[index].priority - 1
	_profile.add_rule(copy)
	EditorInterface.mark_scene_as_unsaved()
	_refresh_ui()
	var actual_index := _profile.rules.find(copy)
	var display_index := _display_rule_indices.find(actual_index)
	if display_index >= 0:
		_rule_list.select(display_index)
	_update_rule_summary()


func _remove_rule() -> void:
	var index := _selected_rule_index()
	if not is_instance_valid(_profile) or index < 0 or index >= _profile.rules.size():
		return
	_profile.begin_change_batch()
	_profile.remove_rule_at(index)
	_normalize_rule_priorities()
	_profile.end_change_batch()
	EditorInterface.mark_scene_as_unsaved()
	_refresh_ui()


func _move_rule(direction: int) -> void:
	var actual_index := _selected_rule_index()
	if not is_instance_valid(_profile) or actual_index < 0:
		return
	var display_index := _display_rule_indices.find(actual_index)
	var destination_display := display_index + direction
	if display_index < 0 or destination_display < 0 or destination_display >= _display_rule_indices.size():
		return
	_profile.begin_change_batch()
	_normalize_rule_priorities()
	var neighbor_index := _display_rule_indices[destination_display]
	var priority := _profile.rules[actual_index].priority
	_profile.rules[actual_index].priority = _profile.rules[neighbor_index].priority
	_profile.rules[neighbor_index].priority = priority
	_profile.end_change_batch()
	EditorInterface.mark_scene_as_unsaved()
	_refresh_ui()
	var new_display_index := _display_rule_indices.find(actual_index)
	if new_display_index >= 0:
		_rule_list.select(new_display_index)
	_update_rule_summary()


func _update_rule_summary() -> void:
	if not is_instance_valid(_rule_summary):
		return
	var index := _selected_rule_index()
	if not is_instance_valid(_profile) or index < 0 or index >= _profile.rules.size():
		_rule_summary.text = "Select a rule to see its material output and active conditions."
		return
	var rule: Terrain3DAutoShaderRule = _profile.rules[index]
	_rule_summary.text = (
		"[b]%s[/b]\n\n"
		+ "[b]Output[/b]  %s\n"
		+ "[b]Conditions[/b]  %s\n"
		+ "[b]Priority[/b]  %d\n"
		+ "[b]Manual paint[/b]  %s\n\n"
		+ "The first enabled matching rule wins. Double-click the rule or press Edit Rule for all thresholds, falloffs, curves, and masks."
	) % [
		rule.get_display_name(),
		_rule_output_summary(rule),
		rule.get_condition_summary(),
		rule.priority,
		"may be overwritten" if rule.overwrite_manual else "preserved",
	]


func _texture_label(texture_id: int) -> String:
	if not is_instance_valid(_terrain) or not is_instance_valid(_terrain.assets):
		return "Texture %d" % texture_id
	if texture_id < 0 or texture_id >= _terrain.assets.get_texture_count():
		return "Missing texture %d" % texture_id
	var asset: Terrain3DTextureAsset = _terrain.assets.get_texture_asset(texture_id)
	if not is_instance_valid(asset):
		return "Missing texture %d" % texture_id
	var asset_name := asset.name.strip_edges()
	return "%s [%d]" % [asset_name if not asset_name.is_empty() else "Texture", texture_id]


func _rule_output_summary(rule: Terrain3DAutoShaderRule) -> String:
	if not is_instance_valid(rule):
		return "No output"
	var base := _texture_label(rule.base_texture)
	if rule.base_texture == rule.overlay_texture or rule.blend <= 0.001:
		return base
	return "%s → %s at %d%%" % [
		base,
		_texture_label(rule.overlay_texture),
		roundi(rule.blend * 100.0),
	]


func _configure_default_rule_outputs(profile: Terrain3DAutoShaderProfile) -> void:
	if not is_instance_valid(profile) or not is_instance_valid(_terrain) or not is_instance_valid(_terrain.assets):
		return
	var valid_texture_ids: Array[int] = []
	for texture_id: int in range(_terrain.assets.get_texture_count()):
		if is_instance_valid(_terrain.assets.get_texture_asset(texture_id)):
			valid_texture_ids.append(texture_id)
	if valid_texture_ids.is_empty():
		return
	var ground_id := valid_texture_ids[0]
	var rock_id := valid_texture_ids[mini(1, valid_texture_ids.size() - 1)]
	var deposit_id := valid_texture_ids[mini(2, valid_texture_ids.size() - 1)]
	profile.begin_change_batch()
	for rule: Terrain3DAutoShaderRule in profile.rules:
		if not is_instance_valid(rule):
			continue
		match rule.rule_name:
			"Steep exposed rock":
				rule.base_texture = ground_id
				rule.overlay_texture = rock_id
			"Depositional channels":
				rule.base_texture = ground_id
				rule.overlay_texture = deposit_id
			"Ground fallback":
				rule.base_texture = ground_id
				rule.overlay_texture = ground_id
	profile.default_base_texture = ground_id
	profile.default_overlay_texture = ground_id
	profile.end_change_batch()


func _inspect_selected_rule() -> void:
	var index := _selected_rule_index()
	if is_instance_valid(_profile) and index >= 0 and index < _profile.rules.size():
		EditorInterface.inspect_object(_profile.rules[index])


func _on_mode_selected(index: int) -> void:
	if not is_instance_valid(_profile):
		return
	_revert_preview(false)
	_suppress_profile_change_tracking = true
	_profile.mode = _mode.get_item_id(index)
	_suppress_profile_change_tracking = false
	EditorInterface.mark_scene_as_unsaved()
	match _profile.mode:
		Terrain3DAutoShaderProfile.Mode.EDITOR_PREVIEW:
			_set_status("Live editor preview evaluates the expanded rules temporarily; closing this panel restores the original control maps.")
			_refresh_editor_preview.call_deferred()
		Terrain3DAutoShaderProfile.Mode.BAKED:
			_set_status("Baked production mode uses explicit control maps and no runtime classifier.")
		Terrain3DAutoShaderProfile.Mode.LEGACY_RUNTIME:
			_set_status("Legacy runtime mode uses the original two-texture slope/height classifier every frame.")
		Terrain3DAutoShaderProfile.Mode.DISABLED:
			_set_status("Automatic classification is disabled; existing painted or baked control values remain active.")
	_refresh_ui()


func _scope_locations() -> Array[Vector2i]:
	var locations: Array[Vector2i] = []
	if not is_instance_valid(_terrain) or not is_instance_valid(_terrain.data):
		return locations
	match _scope.get_selected_id():
		BakeScope.SELECTED_REGION:
			var location: Vector2i = _terrain.data.get_region_location(plugin.mouse_global_position)
			if _terrain.data.has_region(location):
				locations.append(location)
		BakeScope.RESIDENT_REGIONS:
			for location: Vector2i in _terrain.data.get_render_region_slots():
				if _terrain.data.has_region(location) and not locations.has(location):
					locations.append(location)
		BakeScope.ALL_REGIONS:
			locations.assign(_terrain.data.get_region_locations())
		BakeScope.DIRTY_REGIONS:
			return _dirty_scope_locations()
	return locations


func _dirty_scope_locations() -> Array[Vector2i]:
	var locations: Array[Vector2i] = []
	if not is_instance_valid(_terrain) or not is_instance_valid(_terrain.data):
		return locations
	for location: Vector2i in _dirty_regions:
		if _terrain.data.has_region(location):
			locations.append(location)
	return locations


func _capture_snapshot(locations: Array[Vector2i]) -> Dictionary[Vector2i, Image]:
	var snapshot: Dictionary[Vector2i, Image] = {}
	for location: Vector2i in locations:
		var region: Terrain3DRegion = _terrain.data.get_region(location)
		if not is_instance_valid(region):
			continue
		var image: Image = region.control_map
		snapshot[location] = image.duplicate() if is_instance_valid(image) else null
	return snapshot


func _control_maps_equal(left: Image, right: Image) -> bool:
	if not is_instance_valid(left) or not is_instance_valid(right):
		return left == right
	return left.get_size() == right.get_size() and left.get_format() == right.get_format() and \
		left.has_mipmaps() == right.has_mipmaps() and left.get_data() == right.get_data()


func _request_viewport_refresh() -> void:
	if is_instance_valid(plugin):
		plugin.update_overlays()


func _apply_snapshot(p_terrain: Terrain3D, snapshot: Dictionary[Vector2i, Image]) -> void:
	if not is_instance_valid(p_terrain) or not is_instance_valid(p_terrain.data):
		return
	var changed: bool = false
	_suppress_dirty_tracking = true
	for region_location: Vector2i in snapshot:
		var region: Terrain3DRegion = p_terrain.data.get_region(region_location)
		if not is_instance_valid(region):
			continue
		var image: Image = snapshot[region_location]
		if _control_maps_equal(region.control_map, image):
			continue
		region.set_control_map(image.duplicate() if is_instance_valid(image) else null)
		# Material-only changes must not resnap foliage or query editor physics.
		p_terrain.data.commit_map_edit(int(Terrain3DRegion.TYPE_CONTROL), region_location, Rect2i(), false)
		changed = true
	_suppress_dirty_tracking = false
	if changed:
		_request_viewport_refresh()


func _copy_signatures(source: Dictionary[String, int]) -> Dictionary[String, int]:
	var copy: Dictionary[String, int] = {}
	for key: String in source:
		copy[key] = source[key]
	return copy


func _coerce_signatures(source: Dictionary) -> Dictionary[String, int]:
	var typed: Dictionary[String, int] = {}
	for raw_key: Variant in source.keys():
		var key: String = str(raw_key)
		typed[key] = int(source[raw_key])
	return typed


func _merge_signatures(base: Dictionary[String, int], updates: Dictionary[String, int]) -> Dictionary[String, int]:
	var merged: Dictionary[String, int] = _copy_signatures(base)
	for key: String in updates:
		merged[key] = updates[key]
	return merged


func _remove_signatures(base: Dictionary[String, int], locations: Array[Vector2i]) -> Dictionary[String, int]:
	var remaining: Dictionary[String, int] = _copy_signatures(base)
	for location: Vector2i in locations:
		remaining.erase(Terrain3DUtil.location_to_filename(location).trim_prefix("terrain3d").trim_suffix(".res"))
	return remaining


func _apply_bake_transaction(
		p_terrain: Terrain3D,
		snapshot: Dictionary[Vector2i, Image],
		p_profile: Terrain3DAutoShaderProfile,
		mode: int,
		auto_shader_enabled: bool,
		signatures: Dictionary[String, int],
) -> void:
	_apply_snapshot(p_terrain, snapshot)
	if is_instance_valid(p_profile):
		_suppress_profile_change_tracking = true
		p_profile.mode = mode
		p_profile.baked_region_signatures = signatures.duplicate(true)
		p_profile.baked_version = Terrain3DAutoShaderProfile.BAKE_VERSION
		p_profile.emit_changed()
		_suppress_profile_change_tracking = false
	if is_instance_valid(p_terrain) and is_instance_valid(p_terrain.material):
		p_terrain.material.auto_shader_enabled = auto_shader_enabled
	if p_terrain == _terrain and p_profile == _profile:
		for location: Vector2i in snapshot:
			_dirty_regions.erase(location)
		_mark_unbaked_regions_dirty()
		_refresh_ui()
		EditorInterface.mark_scene_as_unsaved()
		_request_viewport_refresh()


func _texture_resource_signature(texture: Texture2D) -> int:
	if not is_instance_valid(texture):
		return 0
	return hash([texture.resource_path, Vector2i(texture.get_width(), texture.get_height())])


func _asset_signature() -> int:
	if not is_instance_valid(_terrain) or not is_instance_valid(_terrain.assets):
		return 0
	var signature: int = 17
	for index: int in _terrain.assets.get_texture_count():
		var asset: Terrain3DTextureAsset = _terrain.assets.get_texture_asset(index)
		if not is_instance_valid(asset):
			continue
		signature = hash([
			signature,
			asset.id,
			asset.name,
			_texture_resource_signature(asset.albedo_texture),
			_texture_resource_signature(asset.normal_texture),
			_texture_resource_signature(asset.surface_texture),
			_texture_resource_signature(asset.displacement_texture),
			_texture_resource_signature(asset.pbr_texture),
		])
	return signature


func _bake_dictionary() -> Dictionary[String, Variant]:
	var bake_profile: Dictionary[String, Variant] = _profile.to_bake_dictionary()
	bake_profile["profile_signature"] = hash([
		bake_profile.get("profile_signature", 0),
		_asset_signature(),
	])
	return bake_profile


func _run_bake(locations: Array[Vector2i], record_signatures: bool = true) -> Terrain3DAutoShaderBakeResult:
	if locations.is_empty():
		return Terrain3DAutoShaderBakeResult.failure("No regions match the selected scope.")
	_suppress_dirty_tracking = true
	var result: Terrain3DAutoShaderBakeResult = Terrain3DAutoShaderBakeResult.from_value(
		_baker.bake_regions(_terrain, _bake_dictionary(), locations))
	_suppress_dirty_tracking = false
	if result.ok and record_signatures:
		_suppress_profile_change_tracking = true
		_profile.set_bake_signatures(result.region_signatures)
		_suppress_profile_change_tracking = false
		for location: Vector2i in locations:
			_dirty_regions.erase(location)
	return result


func _preview_bake() -> void:
	_preview_bake_locations(_scope_locations(), "Preview")


func _preview_bake_locations(locations: Array[Vector2i], action: String) -> void:
	if not _preview_snapshot.is_empty():
		_revert_preview(false)
	if locations.is_empty():
		_set_status("No regions match the selected scope.")
		return
	_preview_snapshot = _capture_snapshot(locations)
	_preview_auto_shader_enabled = bool(_terrain.material.auto_shader_enabled) if is_instance_valid(_terrain.material) else false
	if is_instance_valid(_terrain.material):
		_terrain.material.set_auto_shader_enabled_transient(false)
	var result := _run_bake(locations, false)
	if not result.ok:
		_revert_preview(false)
	elif result.changed_regions > 0:
		_request_viewport_refresh()
	_set_status(_format_result(action, result))
	_refresh_ui()


func _refresh_editor_preview() -> void:
	if not visible or not is_instance_valid(_profile) or \
			_profile.mode != Terrain3DAutoShaderProfile.Mode.EDITOR_PREVIEW:
		return
	var locations: Array[Vector2i] = []
	if is_instance_valid(_terrain) and is_instance_valid(_terrain.data):
		for location: Vector2i in _terrain.data.get_render_region_slots():
			if _terrain.data.has_region(location) and not locations.has(location):
				locations.append(location)
		if locations.is_empty():
			locations.assign(_terrain.data.get_region_locations())
	_preview_bake_locations(locations, "Live preview")


func _revert_preview(report: bool) -> void:
	if _preview_snapshot.is_empty():
		return
	_apply_snapshot(_terrain, _preview_snapshot)
	_preview_snapshot.clear()
	if is_instance_valid(_terrain) and is_instance_valid(_terrain.material):
		_terrain.material.set_auto_shader_enabled_transient(_preview_auto_shader_enabled)
	if report:
		_set_status("Preview reverted; original control maps restored.")
	_refresh_ui()


func _commit_bake() -> void:
	if not is_instance_valid(_profile):
		return
	_revert_preview(false)
	var locations := _scope_locations()
	if locations.is_empty():
		_set_status("No regions match the selected scope.")
		return
	var before := _capture_snapshot(locations)
	var before_mode: int = _profile.mode
	var before_auto_shader := bool(_terrain.material.auto_shader_enabled) if is_instance_valid(_terrain.material) else false
	var before_signatures: Dictionary[String, int] = _copy_signatures(_profile.baked_region_signatures)
	var result := _run_bake(locations, false)
	if not result.ok:
		_apply_snapshot(_terrain, before)
		_set_status(_format_result("Bake", result) + " No partial result was retained.")
		return
	var after := _capture_snapshot(locations)
	var after_signatures := _merge_signatures(before_signatures, result.region_signatures)
	_apply_bake_transaction(
		_terrain,
		after,
		_profile,
		Terrain3DAutoShaderProfile.Mode.BAKED,
		false,
		after_signatures,
	)
	var undo := plugin.get_undo_redo()
	undo.create_action("Bake Terrain3D Autoshader")
	undo.add_do_method(
		self,
		"_apply_bake_transaction",
		_terrain,
		after,
		_profile,
		Terrain3DAutoShaderProfile.Mode.BAKED,
		false,
		after_signatures,
	)
	undo.add_undo_method(
		self,
		"_apply_bake_transaction",
		_terrain,
		before,
		_profile,
		before_mode,
		before_auto_shader,
		before_signatures,
	)
	undo.commit_action(false)
	_set_status(_format_result("Bake", result))
	_refresh_ui()


func _clear_bake() -> void:
	_revert_preview(false)
	if not _profile.preserve_auto_mask:
		_set_status("Clear is unavailable for bakes that strip the material-rule mask. Re-enable Preserve Auto Mask and rebake first.")
		return
	var locations := _scope_locations()
	if locations.is_empty():
		_set_status("No regions match the selected scope.")
		return
	var before := _capture_snapshot(locations)
	var mode: int = _profile.mode
	var auto_shader_enabled := bool(_terrain.material.auto_shader_enabled) if is_instance_valid(_terrain.material) else false
	var before_signatures: Dictionary[String, int] = _copy_signatures(_profile.baked_region_signatures)
	_suppress_dirty_tracking = true
	var result: Terrain3DAutoShaderBakeResult = Terrain3DAutoShaderBakeResult.from_value(
		_baker.clear_baked_regions(_terrain, locations, _profile.preserve_auto_mask))
	_suppress_dirty_tracking = false
	if not result.ok:
		_apply_snapshot(_terrain, before)
		_set_status(_format_result("Clear", result) + " No partial result was retained.")
		return
	var after := _capture_snapshot(locations)
	var after_signatures := _remove_signatures(before_signatures, locations)
	_apply_bake_transaction(_terrain, after, _profile, mode, auto_shader_enabled, after_signatures)
	var undo := plugin.get_undo_redo()
	undo.create_action("Clear Terrain3D Autoshader Bake")
	undo.add_do_method(
		self,
		"_apply_bake_transaction",
		_terrain,
		after,
		_profile,
		mode,
		auto_shader_enabled,
		after_signatures,
	)
	undo.add_undo_method(
		self,
		"_apply_bake_transaction",
		_terrain,
		before,
		_profile,
		mode,
		auto_shader_enabled,
		before_signatures,
	)
	undo.commit_action(false)
	_set_status(_format_result("Clear", result))
	_refresh_ui()


func _format_result(action: String, result: Terrain3DAutoShaderBakeResult) -> String:
	if not result.ok:
		var message: String = result.error
		if message.is_empty() and not result.errors.is_empty():
			message = result.errors[0]
		return "%s failed: %s" % [action, message if not message.is_empty() else "unknown error"]
	var text := "%s: %d regions processed, %d regions changed, %d pixels changed." % [
		action,
		result.processed_regions,
		result.changed_regions,
		result.changed_pixels,
	]
	if not result.errors.is_empty():
		text += " %d region errors." % result.errors.size()
	return text


func _on_map_edit_committed(map_type: int, changed_regions: Array[Vector2i], area: AABB) -> void:
	if _suppress_dirty_tracking or not is_instance_valid(_terrain):
		return
	var products: int = AuthoringDependencies.products_for_sources(AuthoringDependencies.source_for_map_type(map_type))
	if (products & AuthoringDependencies.AUTOSHADER) == 0:
		return
	var locations: Array[Vector2i] = AuthoringDependencies.regions_for_committed_edit(_terrain, changed_regions, area)
	if locations.is_empty():
		return
	for location: Vector2i in locations:
		_dirty_regions[location] = true
	_update_stale_status()
	_schedule_auto_rebake()


func _on_textures_changed() -> void:
	_cancel_auto_rebake()
	_mark_all_dirty()
	if is_instance_valid(_profile) and _profile.mode == Terrain3DAutoShaderProfile.Mode.EDITOR_PREVIEW:
		_schedule_auto_rebake()


func _on_profile_changed() -> void:
	if _suppress_profile_change_tracking:
		return
	_cancel_auto_rebake()
	EditorInterface.mark_scene_as_unsaved()
	_mark_all_dirty()
	_refresh_ui()
	if is_instance_valid(_profile) and _profile.mode == Terrain3DAutoShaderProfile.Mode.EDITOR_PREVIEW:
		_schedule_auto_rebake()


func _on_region_map_changed() -> void:
	if _region_reconcile_scheduled:
		return
	_region_reconcile_scheduled = true
	_reconcile_region_layout.call_deferred()


func _reconcile_region_layout() -> void:
	_region_reconcile_scheduled = false
	if not is_inside_tree() or not is_instance_valid(_terrain):
		return
	_mark_unbaked_regions_dirty()
	_refresh_ui()
	if is_instance_valid(_profile) and _profile.mode == Terrain3DAutoShaderProfile.Mode.EDITOR_PREVIEW:
		_schedule_auto_rebake()


func _mark_unbaked_regions_dirty() -> void:
	if not is_instance_valid(_terrain) or not is_instance_valid(_terrain.data):
		_dirty_regions.clear()
		return
	for location: Vector2i in _dirty_regions.keys():
		if not _terrain.data.has_region(location):
			_dirty_regions.erase(location)
	if not is_instance_valid(_profile) or not is_instance_valid(_baker):
		return
	var locations: Array[Vector2i] = []
	locations.assign(_terrain.data.get_region_locations())
	if _profile.baked_version != Terrain3DAutoShaderProfile.BAKE_VERSION:
		for location: Vector2i in locations:
			_dirty_regions[location] = true
		return
	var current: Dictionary[String, int] = _coerce_signatures(_baker.compute_region_signatures(
		_terrain,
		_bake_dictionary(),
		locations,
	))
	for location: Vector2i in locations:
		var key := Terrain3DUtil.location_to_filename(location).trim_prefix("terrain3d").trim_suffix(".res")
		if not _profile.baked_region_signatures.has(key) or _profile.baked_region_signatures[key] != current.get(key):
			_dirty_regions[location] = true


func _mark_all_dirty(refresh: bool = true) -> void:
	if is_instance_valid(_terrain) and is_instance_valid(_terrain.data):
		for location: Vector2i in _terrain.data.get_region_locations():
			_dirty_regions[location] = true
	if refresh:
		_update_stale_status()


func _update_stale_status() -> void:
	if not is_instance_valid(_stale_status):
		return
	var active_count := _terrain.data.get_region_count() if is_instance_valid(_terrain) and is_instance_valid(_terrain.data) else 0
	if not is_instance_valid(_profile):
		_stale_status.text = "No material-rule profile assigned."
		return
	var enabled_rules := 0
	var invalid_outputs := 0
	var assets: Terrain3DAssets = _terrain.assets if is_instance_valid(_terrain) else null
	var texture_count := assets.get_texture_count() if is_instance_valid(assets) else 0
	var fallback_invalid := not is_instance_valid(assets) or \
		_profile.default_base_texture < 0 or _profile.default_base_texture >= texture_count or \
		_profile.default_overlay_texture < 0 or _profile.default_overlay_texture >= texture_count or \
		not is_instance_valid(assets.get_texture_asset(_profile.default_base_texture)) or \
		not is_instance_valid(assets.get_texture_asset(_profile.default_overlay_texture))
	for rule: Terrain3DAutoShaderRule in _profile.rules:
		if not is_instance_valid(rule):
			continue
		if rule.enabled:
			enabled_rules += 1
		if rule.base_texture < 0 or rule.base_texture >= texture_count or \
			rule.overlay_texture < 0 or rule.overlay_texture >= texture_count or \
			not is_instance_valid(assets.get_texture_asset(rule.base_texture)) or \
			not is_instance_valid(assets.get_texture_asset(rule.overlay_texture)):
			invalid_outputs += 1
	_stale_status.text = "%s · %d/%d rules enabled · %d/%d regions dirty or unbaked." % [
		_profile.get_mode_name(),
		enabled_rules,
		_profile.rules.size(),
		_dirty_regions.size(),
		active_count,
	]
	if fallback_invalid:
		_stale_status.text += "  Profile fallback references a missing terrain texture."
	if invalid_outputs > 0:
		_stale_status.text += "  %d rules reference missing terrain textures." % invalid_outputs


func _schedule_auto_rebake() -> void:
	if not is_instance_valid(_profile):
		return
	if _profile.mode == Terrain3DAutoShaderProfile.Mode.EDITOR_PREVIEW:
		_auto_rebake_timer.start(_profile.auto_rebake_delay)
		return


func _on_auto_rebake_timeout() -> void:
	if not is_instance_valid(_profile) or not is_instance_valid(_terrain):
		return
	if _profile.mode == Terrain3DAutoShaderProfile.Mode.EDITOR_PREVIEW:
		_refresh_editor_preview()


func _set_status(text: String) -> void:
	if is_instance_valid(_status):
		_status.text = text


func _on_popup_hidden() -> void:
	# Preview is intentionally transactional. Closing the panel cannot leave a
	# temporary bake behind without an undo record.
	_revert_preview(false)
