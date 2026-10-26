# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends Window

# This window deliberately uses the account snapshot instead of the full
# renderer profiler. Sampling it never requests a GPU synchronization.
const EDITOR_PROFILE_KEYS: Dictionary = {
	"total_mb": "terrain3d/editor_authoring/total_memory_mb",
	"transient_mb": "terrain3d/editor_authoring/transient_reserve_mb",
	"cooking_mb": "terrain3d/editor_authoring/cooking_memory_mb",
}
const EDITOR_PROFILE_DEFAULTS: Dictionary = {"total_mb": 8192, "transient_mb": 256, "cooking_mb": 0}
const RESOURCE_CAPS: Array[Array] = [
	["Virtual texturing GPU", "virtual_texture_memory_budget_mb"],
	["Instance transforms / cells GPU", "instance_render_memory_budget_mb"],
	["World / shared mesh GPU", "world_render_memory_budget_mb"],
	["Terrain rendering GPU", "terrain_render_memory_budget_mb"],
	["HLOD rendering GPU", "hlod_render_memory_budget_mb"],
	["Ocean rendering GPU", "ocean_render_memory_budget_mb"],
	["Forward lighting GPU", "forward_lighting_memory_budget_mb"],
	["VT cooking working set", "cooking_memory_budget_mb"],
]
const CACHE_BUDGETS: Array[Array] = [
	["GPU terrain pages", "gpu_region_cache_budget_mb", "gpu_region_cache_budget_auto", 512],
	["CPU terrain pages", "cpu_region_cache_budget_mb", "cpu_region_cache_budget_auto", 128],
	["Instance pages CPU", "instance_region_cache_budget_mb", "instance_region_cache_budget_auto", 128],
	["Baked lighting residency", "lightmap_residency_budget_mb", "lightmap_residency_budget_auto", 1024],
	["HLOD residency", "hlod_memory_budget_mb", "hlod_memory_budget_auto", 4096],
]

var terrain: Terrain3D
var _summary: Label
var _usage: ProgressBar
var _error: Label
var _tree: Tree
var _scope: Label
var _pause: CheckButton
var _controls: Dictionary = {}
var _editor_inputs: Dictionary = {}
var _editor_settings: EditorSettings
var _editor_profile_error: String = ""
var _profile_label: Label
var _elapsed: float = 0.0


func _ready() -> void:
	title = "Terrain3D Memory Tracker & Budgets"
	size = Vector2i(1120, 720)
	min_size = Vector2i(800, 480)
	visible = false
	close_requested.connect(hide)
	visibility_changed.connect(func() -> void: set_process(visible))
	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	for side: String in ["left", "top", "right", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 12)
	add_child(margin)
	var root := VBoxContainer.new()
	margin.add_child(root)
	var toolbar := HBoxContainer.new()
	root.add_child(toolbar)
	_summary = Label.new()
	_summary.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	toolbar.add_child(_summary)
	_pause = CheckButton.new()
	_pause.text = "Pause display"
	toolbar.add_child(_pause)
	var refresh := Button.new()
	refresh.text = "Refresh"
	refresh.pressed.connect(refresh_now)
	toolbar.add_child(refresh)
	_usage = ProgressBar.new()
	_usage.show_percentage = true
	root.add_child(_usage)
	_error = Label.new()
	_error.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_error.modulate = Color(1.0, 0.65, 0.3)
	root.add_child(_error)
	var split := HSplitContainer.new()
	split.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root.add_child(split)
	_tree = Tree.new()
	_tree.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_tree.columns = 5
	_tree.column_titles_visible = true
	_tree.hide_root = true
	for column: int in range(5):
		_tree.set_column_title(column, ["Allocation / cache", "Current", "Peak", "Limit / grant", "Denied / scope"][column])
	_tree.set_column_custom_minimum_width(0, 190)
	split.add_child(_tree)
	var scroll := ScrollContainer.new()
	scroll.custom_minimum_size.x = 390
	split.add_child(scroll)
	var budgets := VBoxContainer.new()
	budgets.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(budgets)
	_profile_label = Label.new()
	_profile_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	budgets.add_child(_profile_label)
	if Engine.is_editor_hint():
		_editor_settings = EditorInterface.get_editor_settings()
		_add_note(budgets, "EDITOR AUTHORING — shared EditorSettings, not saved into the terrain scene. Cooking 0 = automatic 2 GiB allowance.")
		_add_editor_profile_field(budgets, "Authoring account ceiling", "total_mb", 512, 8192)
		_add_editor_profile_field(budgets, "Authoring transient reserve", "transient_mb", 32, 1024)
		_add_editor_profile_field(budgets, "Authoring cooking limit (0 = auto)", "cooking_mb", 0, 2048)
		var apply_editor := Button.new()
		apply_editor.text = "Apply editor authoring limits"
		apply_editor.pressed.connect(_apply_editor_profile)
		budgets.add_child(apply_editor)
		_sync_editor_profile_settings()
	_add_note(budgets, "SHIPPING / RUNTIME PROFILE — serialized scene budgets. Changes below affect exported scenes, not Editor Authoring limits.")
	_add_note(budgets, "Apply runtime changes explicitly. Live reductions below resident resource usage keep the previous cap.")
	_add_budget(budgets, "Total ceiling", "total_memory_budget_mb", 32, 4096)
	_add_budget(budgets, "Transient reserve", "memory_transient_reserve_mb", 4, 1024)
	_add_note(budgets, "Resource caps: 0 inherits the total ceiling. Shared mesh geometry is charged to World. These caps do not reserve separate pools.")
	for entry: Array in RESOURCE_CAPS:
		_add_budget(budgets, entry[0], entry[1], 0, 4096)
	_add_note(budgets, "Cache grants: Auto shares the managed pool. Disable Auto to request a manual grant. The effective grant remains constrained by the total budget.")
	for entry: Array in CACHE_BUDGETS:
		_add_budget(budgets, entry[0], entry[1], 1, entry[3], entry[2])
	_scope = Label.new()
	_scope.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_scope)
	set_process(false)
	refresh_now()


func _add_editor_profile_field(parent: VBoxContainer, label_text: String, key: String, minimum: int, maximum: int) -> void:
	var label := Label.new()
	label.text = label_text
	parent.add_child(label)
	var input := SpinBox.new()
	input.min_value = minimum
	input.max_value = maximum
	input.step = 1
	input.suffix = "MiB"
	input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(input)
	_editor_inputs[key] = input


func _sync_editor_profile_settings() -> void:
	if not is_instance_valid(_editor_settings):
		return
	for key: String in EDITOR_PROFILE_KEYS:
		var setting: String = EDITOR_PROFILE_KEYS[key]
		if not _editor_settings.has_setting(setting):
			_editor_settings.set_setting(setting, EDITOR_PROFILE_DEFAULTS[key])
		(_editor_inputs[key] as SpinBox).set_value_no_signal(int(_editor_settings.get_setting(setting)))


func _editor_profile_values() -> Dictionary:
	var values: Dictionary = {}
	for key: String in EDITOR_PROFILE_KEYS:
		values[key] = int(_editor_settings.get_setting(EDITOR_PROFILE_KEYS[key]))
	return values


func _apply_saved_editor_profile() -> void:
	if not is_instance_valid(terrain) or not is_instance_valid(_editor_settings):
		return
	var values: Dictionary = _editor_profile_values()
	if not terrain.set_editor_authoring_memory_profile_mb(values["total_mb"], values["transient_mb"], values["cooking_mb"]):
		_editor_profile_error = "Saved editor limits rejected by the native account; current effective limits were preserved."
	else:
		_editor_profile_error = ""


func _apply_editor_profile() -> void:
	if not is_instance_valid(terrain) or not is_instance_valid(_editor_settings):
		return
	var total_mb: int = int((_editor_inputs["total_mb"] as SpinBox).value)
	var transient_mb: int = int((_editor_inputs["transient_mb"] as SpinBox).value)
	var cooking_mb: int = int((_editor_inputs["cooking_mb"] as SpinBox).value)
	if not terrain.set_editor_authoring_memory_profile_mb(total_mb, transient_mb, cooking_mb):
		_editor_profile_error = "Editor limit reduction rejected; no EditorSettings values were changed."
		refresh_now()
		return
	for key: String in EDITOR_PROFILE_KEYS:
		var value: int = {"total_mb": total_mb, "transient_mb": transient_mb, "cooking_mb": cooking_mb}[key]
		_editor_settings.set_setting(EDITOR_PROFILE_KEYS[key], value)
	_editor_profile_error = ""
	refresh_now()


func _add_note(parent: VBoxContainer, message: String) -> void:
	var note := Label.new()
	note.text = message
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	parent.add_child(note)


func _add_budget(parent: VBoxContainer, label_text: String, property: String, minimum: int, maximum: int, auto_property: String = "") -> void:
	var label := Label.new()
	label.text = label_text
	parent.add_child(label)
	var row := HBoxContainer.new()
	parent.add_child(row)
	var input := SpinBox.new()
	input.min_value = minimum
	input.max_value = maximum
	input.step = 1
	input.suffix = "MiB"
	input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(input)
	var automatic: CheckBox
	if not auto_property.is_empty():
		automatic = CheckBox.new()
		automatic.text = "Auto"
		row.add_child(automatic)
		automatic.toggled.connect(func(enabled: bool) -> void: input.editable = not enabled)
	var apply := Button.new()
	apply.text = "Apply"
	apply.pressed.connect(func() -> void: _apply_budget(property, input, auto_property, automatic))
	row.add_child(apply)
	_controls[property] = {"input": input, "auto": automatic, "auto_property": auto_property, "apply": apply}


func _apply_budget(property: String, input: SpinBox, auto_property: String, automatic: CheckBox) -> void:
	if not is_instance_valid(terrain):
		return
	# Set the manual request before changing Auto so only the final request is
	# applied when transitioning from an automatically allocated grant.
	terrain.set(property, int(input.value))
	if is_instance_valid(automatic):
		terrain.set(auto_property, automatic.button_pressed)
	terrain.force_memory_budget_rebalance()
	if Engine.is_editor_hint():
		EditorInterface.mark_scene_as_unsaved()
	_sync_controls()
	refresh_now()


func set_terrain(selected: Terrain3D) -> void:
	terrain = selected
	if is_node_ready():
		_apply_saved_editor_profile()
		_sync_controls()
		refresh_now()


func open_for_terrain(selected: Terrain3D) -> void:
	set_terrain(selected)
	popup_centered()
	refresh_now()


func _sync_controls() -> void:
	for property: String in _controls:
		var entry: Dictionary = _controls[property]
		var input: SpinBox = entry["input"]
		var automatic: CheckBox = entry["auto"]
		var valid: bool = is_instance_valid(terrain)
		(entry["apply"] as Button).disabled = not valid
		if valid:
			input.set_value_no_signal(float(terrain.get(property)))
			if is_instance_valid(automatic):
				automatic.set_pressed_no_signal(bool(terrain.get(entry["auto_property"])))
		input.editable = valid and (not is_instance_valid(automatic) or not automatic.button_pressed)


func _process(delta: float) -> void:
	_elapsed += delta
	if _elapsed >= 0.5 and not _pause.button_pressed:
		_elapsed = 0.0
		refresh_now()


func _bytes(value: int) -> String:
	return String.humanize_size(value)


func _row(root: TreeItem, label: String, current: int, peak: int, limit: String, status: String) -> void:
	var item := _tree.create_item(root)
	item.set_text(0, label)
	item.set_text(1, _bytes(current))
	item.set_text(2, _bytes(peak) if peak >= 0 else "—")
	item.set_text(3, limit)
	item.set_text(4, status)


func refresh_now() -> void:
	if not is_node_ready():
		return
	_tree.clear()
	var root := _tree.create_item()
	if not is_instance_valid(terrain):
		_summary.text = "Select a Terrain3D node"
		_usage.value = 0
		_error.text = _editor_profile_error
		_profile_label.text = "No Terrain3D selected"
		_scope.text = ""
		return
	var snapshot: Dictionary = terrain.get_memory_tracker_stats()
	var current: int = snapshot.get("current_bytes", 0)
	var limit: int = snapshot.get("limit_bytes", 0)
	var authoring: bool = snapshot.get("profile", "runtime") == "editor_authoring"
	_profile_label.text = "Active: %s | account %d MiB | transient %d MiB | runtime scene %d MiB" % [
		"Editor Authoring" if authoring else "Runtime Profile",
		int(snapshot.get("effective_total_budget_mb", 0)),
		int(snapshot.get("effective_transient_reserve_mb", 0)),
		int(snapshot.get("configured_runtime_budget_mb", 0))]
	_summary.text = "%s / %s  |  peak %s  |  free %s  |  denied %d" % [
		_bytes(current), _bytes(limit), _bytes(snapshot.get("peak_bytes", 0)),
		_bytes(snapshot.get("headroom_bytes", 0)), snapshot.get("denied_allocations", 0)]
	_usage.value = 100.0 * current / limit if limit > 0 else 0.0
	_error.text = snapshot.get("last_budget_change_error", "")
	if not _editor_profile_error.is_empty():
		_error.text += " " + _editor_profile_error
	_scope.text = snapshot.get("scope", "") + " EditorSettings limits are editor-local and do not change shipping scene budgets."
	_row(root, "Total accounted", current, snapshot.get("peak_bytes", 0), _bytes(limit), str(snapshot.get("denied_allocations", 0)))
	_row(root, "CPU backing / retained", snapshot.get("cpu_bytes", 0), snapshot.get("cpu_peak_bytes", 0), "shared total", "includes arena")
	_row(root, "GPU buffers / textures", snapshot.get("gpu_bytes", 0), snapshot.get("gpu_peak_bytes", 0), "shared total", "capacity")
	_row(root, "External payload reservations", snapshot.get("external_payload_bytes", 0), snapshot.get("external_payload_peak_bytes", 0), "shared total", "accounted payloads")
	for resource: Dictionary in snapshot.get("resources", []):
		var cap: String = _bytes(resource.get("effective_limit_bytes", 0))
		if resource.get("inherits_total", false):
			cap += " (shared)"
		_row(root, "  " + str(resource.get("name", "")).capitalize(), resource.get("current_bytes", 0), resource.get("peak_bytes", 0), cap, str(resource.get("denied_allocations", 0)))
	_row(root, "Arena backing (included in CPU)", snapshot.get("arena_reserved_bytes", 0), -1, "included above", "do not sum")
	_row(root, "Zone used (inside arena)", snapshot.get("zone_used_bytes", 0), -1, _bytes(snapshot.get("zone_reserved_bytes", 0)), "do not sum")
	_row(root, "VT cooking working limit", 0, -1, _bytes(snapshot.get("cooking_working_limit_bytes", 0)),
		"auto editor" if snapshot.get("cooking_auto_editor_policy", false) else "manual/runtime")
	_row(root, "Runtime cooking requested", 0, -1, _bytes(snapshot.get("configured_runtime_cooking_limit_bytes", 0)), "0 = auto")
	_row(root, "GPU region effective grant", 0, -1, _bytes(snapshot.get("gpu_region_effective_budget_bytes", 0)), "grant; not allocated")
	_row(root, "Editor requested GPU pages", 0, -1, str(snapshot.get("editor_gpu_region_requested_pages", 0)), "capacity; not bytes")
	_row(root, "VT package composition limit", 0, -1, _bytes(snapshot.get("composition_working_limit_bytes", 0)), "also needs account headroom")
	for cache: Dictionary in snapshot.get("residency", []):
		_row(root, str(cache.get("name", "")).capitalize(), cache.get("current_bytes", 0), -1, _bytes(cache.get("effective_budget_bytes", 0)), str(cache.get("accuracy", "")) + "; overlaps")
