@tool
extends PopupPanel

const PRESET_SCRIPT = preload("res://addons/terrain_3d/src/foliage_preset.gd")
const DEFINITION_SCRIPT = preload("res://addons/terrain_3d/src/foliage_definition.gd")
const RULE_SCRIPT = preload("res://addons/terrain_3d/src/foliage_mesh_rule.gd")
const MANIFEST_SCRIPT = preload("res://addons/terrain_3d/src/foliage_manifest.gd")
const TRANSACTION_SCRIPT = preload("res://addons/terrain_3d/tools/foliage_transaction.gd")
const ASYNC_TRANSACTION = preload("res://addons/terrain_3d/tools/foliage_async_transaction.gd")
const SOURCE_JOB = preload("res://addons/terrain_3d/tools/foliage_source_job.gd")
const EditorUI = preload("res://addons/terrain_3d/src/editor_ui_util.gd")

var plugin: Terrain3DEditorPluginBase
var _terrain: Terrain3D
var _preset: Terrain3DFoliagePreset
var _manifest: Terrain3DFoliageManifest
var _pending_manifest: Terrain3DFoliageManifest
var _manifest_path: String = ""
var _ledger_error: String = ""
var _config_error: String = ""
var _pending_removals: Array[Dictionary] = []
var _pending_seed: int = 0
var _generator: Terrain3DFoliageGenerator
var _active: bool = false
var _pending_mode: String = ""
var _pending_regions: Array[Vector2i] = []
var _pending_keys: Dictionary[String, String] = {}
var _pending_records: Array[Dictionary] = []
var _saving: bool = false
var _snapshot_serial: int = 0
enum JobState { IDLE, VALIDATING, GENERATING, STAGING, COMMITTING, RESTORING, FAILED, DONE }
var _state: JobState = JobState.IDLE
var _target_region: Vector2i = Vector2i(2147483647, 2147483647)
var _region_selector: OptionButton
var _watched_resources: Array[Resource] = []
var _asset_signal_source: Resource
var _source_signatures: Dictionary = {}
var _problems: Array[String] = []
var _refresh_queued: bool = false
var _preparation
var _source_job
var _transaction_job
var _commit_terrain: Terrain3D
var _watched_terrain: Terrain3D
var _frozen_rules_signature: String = ""
var _last_job_metrics: Dictionary = {}
const GENERATION_BUDGET: Dictionary = {"usec": 2000, "candidates": 2048,
	"neighbors": 4096, "samples": 4096, "transitions": 256}
const COMMIT_BUDGET: Dictionary = {"usec": 2000, "pages": 1,
	"decode_bytes": 1048576, "io_bytes": 262144}

var _preset_picker: EditorResourcePicker
var _scope: OptionButton
var _rule_container: VBoxContainer
var _status: Label
var _progress: ProgressBar
var _generate: Button
var _regenerate: Button
var _clear: Button
var _cancel: Button
var _save_dialog: EditorFileDialog

func _ready() -> void:
	_generator = Terrain3DFoliageGenerator.new()
	_build_ui()
	popup_hide.connect(_on_popup_hidden)
	var manager: EditorUndoRedoManager = EditorInterface.get_editor_undo_redo()
	if manager.has_signal("version_changed"):
		manager.connect("version_changed", _on_history_changed)
	set_process(false)

func open() -> void:
	set_terrain(plugin.terrain if is_instance_valid(plugin) else null)
	_sync_preset()
	_populate_region_selector(true)
	_recover_journal()
	popup_centered(EditorUI.popup_size_for_screen(self, Vector2i(940, 700), Vector2i(650, 480)))

func set_terrain(p_terrain: Terrain3D) -> void:
	if _terrain == p_terrain:
		return
	if _active or _saving or _preparation != null:
		_abort()
	if is_instance_valid(_watched_terrain) and _watched_terrain.tree_exiting.is_connected(_on_terrain_exiting):
		_watched_terrain.tree_exiting.disconnect(_on_terrain_exiting)
	_terrain = p_terrain
	_watched_terrain = p_terrain
	if _watched_terrain != null:
		_watched_terrain.tree_exiting.connect(_on_terrain_exiting)
	_load_manifest()
	_sync_preset()
	_populate_region_selector(false)
	_refresh_editor()

func _sync_preset() -> void:
	_preset = _terrain.foliage_profile as Terrain3DFoliagePreset if _terrain != null else null
	if is_instance_valid(_preset_picker):
		_preset_picker.edited_resource = _preset
	_watch_resources()

func _set_preset(p_preset: Terrain3DFoliagePreset) -> void:
	if _terrain == null:
		return
	if _active or _saving or _preparation != null:
		_abort()
	_discard_pending()
	var previous: Terrain3DFoliagePreset = _terrain.foliage_profile as Terrain3DFoliagePreset if _terrain != null else _preset
	# Preserve ownership from the old resource before the picker releases it,
	# including a retry after an earlier migration write failed.
	_preset = previous
	if p_preset != previous and not _import_legacy_manifest():
		if is_instance_valid(_preset_picker):
			_preset_picker.edited_resource = previous
		if is_instance_valid(_status):
			_status.text = _ledger_error
		return
	_preset = p_preset
	if _preset != null:
		_preset.ensure_uid()
	if not _import_legacy_manifest():
		_sync_preset()
		_status.text = _ledger_error
		return
	if previous == p_preset:
		_sync_preset()
		return
	var manager: EditorUndoRedoManager = EditorInterface.get_editor_undo_redo()
	manager.create_action("Assign Terrain3D Foliage Preset", UndoRedo.MERGE_DISABLE, _terrain)
	manager.add_do_property(_terrain, "foliage_profile", p_preset)
	manager.add_undo_property(_terrain, "foliage_profile", previous)
	manager.commit_action()
	_sync_preset()
	_refresh_editor()

func _load_manifest() -> void:
	_manifest = null
	_manifest_path = ""
	_ledger_error = ""
	if _terrain == null:
		return
	if _terrain.data_directory.is_empty():
		_ledger_error = "Assign a terrain data directory before generating foliage."
		return
	var identity: String = _terrain_identity(_terrain)
	_manifest_path = _terrain.data_directory.path_join(MANIFEST_SCRIPT.FILE_NAME)
	if FileAccess.file_exists(_manifest_path):
		_manifest = ResourceLoader.load(_manifest_path, "", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DFoliageManifest
		if _manifest == null or _manifest.terrain_identity != identity:
			_manifest = null
			_ledger_error = "Foliage ledger is unreadable or belongs to another terrain data directory."
			return
	else:
		_manifest = MANIFEST_SCRIPT.new()
		_manifest.terrain_identity = identity

func _terrain_identity(terrain: Terrain3D) -> String:
	var identity: String = ProjectSettings.globalize_path(terrain.data_directory).simplify_path().replace("\\", "/").trim_suffix("/")
	return identity.to_lower() if OS.has_feature("windows") else identity

func _import_legacy_manifest() -> bool:
	if _manifest == null or _preset == null or _preset.manifest == null:
		return true
	# Legacy presets could be shared. Import only owners actually present in this
	# terrain's matching region; never adopt uncommitted reservations or other worlds.
	var owners_by_region: Dictionary = {}
	var migrated: Terrain3DFoliageManifest = _manifest.duplicate(true) as Terrain3DFoliageManifest
	var changed: bool = false
	for record: Dictionary in _preset.manifest.entries:
		var key: String = String(record.get("key", ""))
		var owner: int = int(record.get("owner", 0))
		var region: Vector2i = record.get("region", Vector2i(2147483647, 2147483647))
		if key.is_empty() or owner <= 0 or owner > 2147483647 or String(record.get("source", "")).is_empty():
			continue
		if migrated.owner_for(key) > 0 or not _terrain.data.has_region(region):
			continue
		if not owners_by_region.has(region):
			if not _terrain.data.ensure_instance_region_resident(region):
				_ledger_error = "Cannot read legacy foliage owners in region " + str(region)
				return false
			var owners: Dictionary = {}
			for cells: Dictionary in _terrain.data.get_region(region).get_instances().values():
				for payload: Array in cells.values():
					if payload.size() > 3:
						for value: int in payload[3]:
							owners[value] = true
			owners_by_region[region] = owners
		if not owners_by_region[region].has(owner):
			continue
		for existing: Dictionary in migrated.region_records(region):
			if int(existing["owner"]) == owner and String(existing["key"]) != key:
				_ledger_error = "Ambiguous legacy foliage owner in region " + str(region)
				return false
		migrated.entries.append(record.duplicate(true))
		migrated.next_owner_id = maxi(migrated.next_owner_id, owner + 1)
		changed = true
	if changed:
		# These are already-published instances, not prospective reservations.
		# Persist migration before the preset picker can drop the legacy resource.
		if not _persist_manifest(migrated, _terrain):
			_ledger_error = "Cannot persist legacy foliage ownership; preset assignment was not changed."
			return false
		_manifest = migrated
	_ledger_error = ""
	return true

func _discard_pending() -> void:
	_pending_manifest = null
	_pending_keys.clear()
	_pending_records.clear()
	_pending_removals.clear()
	_source_signatures.clear()

func _label(text: String, parent: Node) -> Label:
	var result := Label.new()
	result.text = text
	parent.add_child(result)
	return result

func _button(text: String, parent: Node, callback: Callable) -> Button:
	var result := Button.new()
	result.text = text
	result.pressed.connect(callback)
	parent.add_child(result)
	return result

func _build_ui() -> void:
	var vertical := VBoxContainer.new()
	vertical.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	vertical.add_theme_constant_override("separation", 8)
	add_child(vertical)
	var heading := HBoxContainer.new()
	vertical.add_child(heading)
	_label("Foliage Generation", heading).size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_button("Save Preset As…", heading, _save_as)
	var config := HBoxContainer.new()
	vertical.add_child(config)
	_label("Preset", config)
	_preset_picker = EditorResourcePicker.new()
	_preset_picker.base_type = "Terrain3DFoliagePreset"
	_preset_picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_preset_picker.resource_changed.connect(func(resource: Resource) -> void: _set_preset(resource as Terrain3DFoliagePreset))
	config.add_child(_preset_picker)
	_button("New", config, func() -> void: _set_preset(PRESET_SCRIPT.new()))
	var section := HBoxContainer.new()
	vertical.add_child(section)
	_label("Material associations and mesh banks", section).size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_button("Add Material", section, _add_definition)
	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	vertical.add_child(scroll)
	_rule_container = VBoxContainer.new()
	_rule_container.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_rule_container)
	var settings := HBoxContainer.new()
	vertical.add_child(settings)
	_label("Scope", settings)
	_scope = OptionButton.new()
	_scope.add_item("Selected Region", 0)
	_scope.add_item("All Regions", 1)
	_scope.item_selected.connect(func(_index: int) -> void: _scope_changed())
	settings.add_child(_scope)
	_region_selector = OptionButton.new()
	_region_selector.item_selected.connect(func(index: int) -> void:
		_target_region = _region_selector.get_item_metadata(index)
		_scope_changed())
	settings.add_child(_region_selector)
	_button("Edit Preset Settings", settings, func() -> void:
		if _preset != null:
			EditorInterface.edit_resource(_preset))
	_button("Refresh Stale Status", settings, _refresh_editor)
	_status = _label("Select a Terrain3D and foliage preset.", vertical)
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_progress = ProgressBar.new()
	_progress.max_value = 1.0
	_progress.show_percentage = true
	vertical.add_child(_progress)
	var actions := HBoxContainer.new()
	vertical.add_child(actions)
	_generate = _button("Generate", actions, func() -> void: _start("generate"))
	_regenerate = _button("Regenerate", actions, func() -> void: _start("regenerate"))
	_clear = _button("Clear Generated", actions, func() -> void: _start("clear"))
	_cancel = _button("Cancel", actions, _abort)
	_cancel.disabled = true
	_save_dialog = EditorFileDialog.new()
	_save_dialog.access = EditorFileDialog.ACCESS_RESOURCES
	_save_dialog.file_mode = EditorFileDialog.FILE_MODE_SAVE_FILE
	_save_dialog.add_filter("*.tres", "Terrain foliage preset")
	_save_dialog.file_selected.connect(_save_preset_to)
	add_child(_save_dialog)

func _refresh_editor() -> void:
	if not is_instance_valid(_rule_container):
		return
	for child in _rule_container.get_children():
		child.queue_free()
	if _preset == null or _terrain == null:
		if is_instance_valid(_status) and _terrain != null:
			_status.text = "Assign a preset to generate. Clear Generated uses this terrain's saved owners."
		return
	for definition: Terrain3DFoliageDefinition in _preset.definitions:
		if definition == null:
			continue
		var box := VBoxContainer.new()
		box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_rule_container.add_child(box)
		var header := HBoxContainer.new()
		box.add_child(header)
		var enabled := CheckBox.new()
		enabled.button_pressed = definition.enabled
		enabled.toggled.connect(func(value: bool) -> void: _edit_property(definition, "enabled", value, "Enable Foliage Material"))
		header.add_child(enabled)
		var resource_picker := EditorResourcePicker.new()
		resource_picker.base_type = "Terrain3DTextureAsset"
		resource_picker.edited_resource = definition.terrain_material
		resource_picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		resource_picker.resource_changed.connect(func(resource: Resource) -> void:
			_edit_property(definition, "terrain_material", resource as Terrain3DTextureAsset, "Assign Foliage Material"))
		header.add_child(resource_picker)
		_button("Add Mesh", header, func() -> void: _add_mesh(definition))
		_button("Remove", header, func() -> void: _remove_definition(definition))
		for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
			if rule == null:
				continue
			var line := HBoxContainer.new()
			box.add_child(line)
			var rule_enabled := CheckBox.new()
			rule_enabled.button_pressed = rule.enabled
			rule_enabled.toggled.connect(func(value: bool) -> void: _edit_property(rule, "enabled", value, "Enable Foliage Rule"))
			line.add_child(rule_enabled)
			var mesh_picker := EditorResourcePicker.new()
			mesh_picker.base_type = "Terrain3DMeshAsset"
			mesh_picker.edited_resource = rule.mesh_asset
			mesh_picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			mesh_picker.resource_changed.connect(func(resource: Resource) -> void:
				_edit_property(rule, "mesh_asset", resource as Terrain3DMeshAsset, "Assign Foliage Mesh"))
			line.add_child(mesh_picker)
			_button("Rules…", line, func() -> void: EditorInterface.edit_resource(rule))
			_button("×", line, func() -> void: _remove_rule(definition, rule))
			var fields := HFlowContainer.new()
			box.add_child(fields)
			_rule_field(fields, rule, "density_per_square_meter", "Density / m²", 0.0, 100000.0, 0.001)
			_rule_field(fields, rule, "minimum_spacing", "Spacing (m)", 0.0, 50.0, 0.05)
			_rule_field(fields, rule, "min_slope", "Min slope", 0.0, 90.0, 0.1)
			_rule_field(fields, rule, "max_slope", "Max slope", 0.0, 90.0, 0.1)
			_rule_field(fields, rule, "min_elevation", "Min height", -100000.0, 100000.0, 1.0)
			_rule_field(fields, rule, "max_elevation", "Max height", -100000.0, 100000.0, 1.0)
		var divider := HSeparator.new()
		box.add_child(divider)
	_watch_resources()
	_refresh_status()

func _add_definition() -> void:
	if _preset == null:
		return
	var definition: Terrain3DFoliageDefinition = DEFINITION_SCRIPT.new()
	definition.ensure_uid()
	var rule: Terrain3DFoliageMeshRule = RULE_SCRIPT.new()
	rule.ensure_uid()
	definition.mesh_bank.append(rule)
	var definitions: Array[Terrain3DFoliageDefinition] = _preset.definitions.duplicate()
	definitions.append(definition)
	_edit_property(_preset, "definitions", definitions, "Add Foliage Material")

func _selected_regions() -> Array[Vector2i]:
	var locations: Array[Vector2i] = []
	if _terrain == null or _terrain.data == null:
		return locations
	if is_instance_valid(_scope) and _scope.selected == 0:
		if _terrain.data.has_region(_target_region):
			locations.append(_target_region)
	else:
		locations.assign(_terrain.data.get_region_locations())
	locations.sort_custom(func(a: Vector2i, b: Vector2i) -> bool: return a.x < b.x or (a.x == b.x and a.y < b.y))
	return locations

func _resource_key(resource: Resource) -> String:
	if resource == null:
		return ""
	return "%s/%s/%s" % [resource.resource_path, resource.resource_scene_unique_id, resource.resource_name]

func _populate_region_selector(preselect_hover: bool) -> void:
	if not is_instance_valid(_region_selector):
		return
	_region_selector.clear()
	if _terrain == null or _terrain.data == null:
		return
	if preselect_hover and is_instance_valid(plugin):
		_target_region = _terrain.data.get_region_location(plugin.mouse_global_position)
	var locations: Array[Vector2i] = []
	locations.assign(_terrain.data.get_region_locations())
	locations.sort_custom(func(a: Vector2i, b: Vector2i) -> bool: return a.x < b.x or (a.x == b.x and a.y < b.y))
	var selected: int = -1
	for location: Vector2i in locations:
		var index: int = _region_selector.item_count
		_region_selector.add_item("Region (%d, %d)" % [location.x, location.y])
		_region_selector.set_item_metadata(index, location)
		if location == _target_region:
			selected = index
	_region_selector.select(selected)
	if selected < 0:
		_region_selector.text = "Choose a region"

func _edit_property(resource: Resource, property: String, value: Variant, label: String) -> void:
	if resource.get(property) == value or _saving:
		return
	if _active:
		_abort()
	var manager: EditorUndoRedoManager = EditorInterface.get_editor_undo_redo()
	manager.create_action(label, UndoRedo.MERGE_ENDS, _terrain)
	manager.add_do_property(resource, property, value)
	manager.add_undo_property(resource, property, resource.get(property))
	manager.add_do_method(resource, "emit_changed")
	manager.add_undo_method(resource, "emit_changed")
	manager.commit_action()
	_refresh_editor()

func _add_mesh(definition: Terrain3DFoliageDefinition) -> void:
	var bank: Array[Terrain3DFoliageMeshRule] = definition.mesh_bank.duplicate()
	var rule: Terrain3DFoliageMeshRule = RULE_SCRIPT.new()
	rule.ensure_uid()
	bank.append(rule)
	_edit_property(definition, "mesh_bank", bank, "Add Foliage Mesh")

func _remove_definition(definition: Terrain3DFoliageDefinition) -> void:
	var definitions: Array[Terrain3DFoliageDefinition] = _preset.definitions.duplicate()
	definitions.erase(definition)
	_edit_property(_preset, "definitions", definitions, "Remove Foliage Material")

func _remove_rule(definition: Terrain3DFoliageDefinition, rule: Terrain3DFoliageMeshRule) -> void:
	var bank: Array[Terrain3DFoliageMeshRule] = definition.mesh_bank.duplicate()
	bank.erase(rule)
	_edit_property(definition, "mesh_bank", bank, "Remove Foliage Mesh")

func _rule_field(parent: Control, rule: Terrain3DFoliageMeshRule, property: String,
		label: String, minimum: float, maximum: float, increment: float) -> void:
	var field := VBoxContainer.new()
	parent.add_child(field)
	_label(label, field)
	var value := SpinBox.new()
	value.min_value = minimum
	value.max_value = maximum
	value.step = increment
	value.value = float(rule.get(property))
	value.custom_minimum_size.x = 115.0
	value.value_changed.connect(func(number: float) -> void: _edit_property(rule, property, number, "Change Foliage " + label))
	field.add_child(value)

func _watch_resources() -> void:
	if is_instance_valid(_asset_signal_source):
		for signal_name: String in ["textures_changed", "meshes_changed"]:
			if _asset_signal_source.is_connected(signal_name, _on_resource_changed):
				_asset_signal_source.disconnect(signal_name, _on_resource_changed)
	_asset_signal_source = null
	for resource: Resource in _watched_resources:
		if is_instance_valid(resource) and resource.changed.is_connected(_on_resource_changed):
			resource.changed.disconnect(_on_resource_changed)
	_watched_resources.clear()
	_watch(_preset)
	if _terrain != null:
		_watch(_terrain.assets)
		_watch(_terrain.material)
		_asset_signal_source = _terrain.assets
		for signal_name: String in ["textures_changed", "meshes_changed"]:
			if _asset_signal_source != null and _asset_signal_source.has_signal(signal_name):
				_asset_signal_source.connect(signal_name, _on_resource_changed)
	if _preset == null:
		return
	for definition: Terrain3DFoliageDefinition in _preset.definitions:
		if definition == null:
			continue
		_watch(definition)
		_watch(definition.terrain_material)
		for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
			if rule != null:
				_watch(rule)
				_watch(rule.mesh_asset)

func _watch(resource: Resource) -> void:
	if resource != null and not _watched_resources.has(resource):
		_watched_resources.append(resource)
		resource.changed.connect(_on_resource_changed)

func _on_resource_changed() -> void:
	if _active or _saving or _preparation != null:
		_abort()
	if not _saving:
		_set_state(JobState.IDLE)
	if not _refresh_queued:
		_refresh_queued = true
		call_deferred("_refresh_changed_resources")

func _scope_changed() -> void:
	if _active or _saving or _preparation != null:
		_abort()
	_set_state(JobState.IDLE)
	_refresh_status()

func _refresh_changed_resources() -> void:
	_refresh_queued = false
	_refresh_editor()

func _asset_slot(resource: Resource, mesh: bool) -> int:
	if resource == null or _terrain == null or _terrain.assets == null:
		return -1
	var count: int = _terrain.assets.get_mesh_count() if mesh else _terrain.assets.get_texture_count()
	for index: int in range(count):
		var asset: Resource = _terrain.assets.get_mesh_asset(index) if mesh else _terrain.assets.get_texture_asset(index)
		if asset == resource:
			return index
	return -1

func _validation(mode: String) -> Dictionary:
	var problems: Array[String] = []
	var regions: Array[Vector2i] = _selected_regions()
	if _terrain == null or _terrain.data == null:
		return {"kind": "error", "problems": ["Select a Terrain3D node."], "regions": regions, "estimate": 0}
	if regions.is_empty():
		return {"kind": "error", "problems": ["Choose a valid, loaded region or All Regions."], "regions": regions, "estimate": 0}
	if not _ledger_error.is_empty():
		return {"kind": "error", "problems": [_ledger_error], "regions": regions, "estimate": 0}
	if mode == "clear":
		return {"kind": "ready", "problems": [], "regions": regions, "estimate": 0}
	if _preset == null:
		return {"kind": "error", "problems": ["Use New or assign a foliage preset."], "regions": regions, "estimate": 0}
	var estimate: float = 0.0
	for definition: Terrain3DFoliageDefinition in _preset.definitions:
		if definition == null or not definition.enabled:
			continue
		if _asset_slot(definition.terrain_material, false) < 0:
			problems.append("Material association has no registered terrain texture.")
			continue
		var weight: float = 0.0
		for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
			if rule != null and rule.enabled and rule.density_per_square_meter > 0.0:
				weight += maxf(rule.relative_weight, 0.0)
		for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
			if rule == null or not rule.enabled or rule.density_per_square_meter <= 0.0:
				continue
			var problem: String = rule.validate()
			if not problem.is_empty():
				problems.append(problem)
			elif _asset_slot(rule.mesh_asset, true) < 0:
				problems.append("Foliage mesh is not registered in this terrain.")
			else:
				var density: float = rule.density_per_square_meter * _preset.global_density_multiplier * rule.relative_weight / maxf(weight, 0.001)
				var spacing_problem: String = rule.validate_spacing(density)
				if not spacing_problem.is_empty():
					problems.append(spacing_problem)
				estimate += density * 16384.0 * regions.size()
	var owned: bool = false
	if _manifest != null:
		for region: Vector2i in regions:
			owned = owned or not _manifest.region_records(region).is_empty()
	var kind: String = "ready"
	if not problems.is_empty():
		kind = "warning" if mode == "regenerate" and owned else "error"
	elif estimate <= 0.0 and not owned:
		kind = "empty"
	return {"kind": kind, "problems": problems, "regions": regions, "estimate": ceili(estimate)}

func _refresh_status() -> void:
	if not is_instance_valid(_status) or _active or _saving or _state == JobState.FAILED:
		return
	var result: Dictionary = _validation("regenerate")
	_status.text = "%d regions; estimated %d placements before terrain filters. Regenerate reconciles earlier presets." % [result["regions"].size(), result["estimate"]]
	if not result["problems"].is_empty():
		_status.text += "\n" + "\n".join(result["problems"])

func _source_signature(region: Vector2i, rules_signature: String = "") -> String:
	if _terrain == null:
		return ""
	var targets: Array[Vector2i] = [region]
	var revisions: Dictionary = _terrain.data.capture_foliage_sources(targets, 4)
	var hasher := HashingContext.new()
	hasher.start(HashingContext.HASH_SHA256)
	hasher.update(var_to_bytes(revisions["regions"]))
	hasher.update((_rules_signature() if rules_signature.is_empty() else rules_signature).to_utf8_buffer())
	return hasher.finish().hex_encode()

func _rules_signature() -> String:
	if _preset == null:
		return "clear"
	var signature_records: Array[String] = []
	for definition: Terrain3DFoliageDefinition in _preset.definitions:
		if definition == null:
			continue
		var rules: Array[String] = []
		for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
			if rule == null:
				continue
			rules.append(JSON.stringify({"uid": rule.stable_uid, "enabled": rule.enabled,
				"mesh": _resource_key(rule.mesh_asset), "mesh_slot": _asset_slot(rule.mesh_asset, true), "density": rule.density_per_square_meter,
				"weight": rule.relative_weight, "min_scale": rule.min_scale, "max_scale": rule.max_scale,
				"yaw": rule.random_yaw, "slope": [rule.min_slope, rule.max_slope],
				"height": [rule.min_elevation, rule.max_elevation], "alignment": rule.normal_alignment,
				"spacing": rule.minimum_spacing, "cluster": rule.clustering_strength,
				"cluster_scale": rule.clustering_scale, "exclusions": Array(rule.excluded_material_ids)}))
		rules.sort()
		signature_records.append(JSON.stringify({"uid": definition.stable_uid, "enabled": definition.enabled,
			"material": _resource_key(definition.terrain_material), "material_slot": _asset_slot(definition.terrain_material, false), "rules": rules}))
	signature_records.sort()
	var shader_state: Array = []
	if _terrain.material != null:
		for parameter: String in ["auto_slope", "auto_height_reduction", "auto_base_texture", "auto_overlay_texture"]:
			shader_state.append(_terrain.material.get_shader_param(parameter))
	var details: String = JSON.stringify({"seed": _preset.random_seed, "density": _preset.global_density_multiplier,
		"region_size": int(_terrain.region_size), "limit": _preset.region_instance_limit,
		"material_identity": _terrain.material.get_instance_id() if _terrain.material != null else 0,
		"auto_enabled": _terrain.material.get("auto_shader_enabled") if _terrain.material != null else false,
		"shader_auto": shader_state, "rules": signature_records})
	return details.sha256_text()

func _is_region_stale(region: Vector2i) -> bool:
	if _preset == null or _manifest == null:
		return true
	var source: String = _source_signature(region)
	var found: bool = false
	for record: Dictionary in _manifest.region_records(region):
		found = true
		if String(record.get("source", "")) != source:
			return true
	return not found

func _build_config(regions: Array[Vector2i], mode: String) -> Dictionary:
	var rules: Array[Dictionary] = []
	var requests: Array[Dictionary] = []
	_discard_pending()
	_config_error = ""
	if _manifest == null or not _ledger_error.is_empty():
		_config_error = _ledger_error if not _ledger_error.is_empty() else "No terrain foliage ledger."
		return {}
	_preset.ensure_uid()
	_pending_seed = _preset.random_seed
	# Stable identities are initialized only by an explicit generation action.
	# Status display and opening the popup never edit preset/rule resources.
	for definition: Terrain3DFoliageDefinition in _preset.definitions:
		if definition != null:
			definition.ensure_uid()
			for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
				if rule != null:
					rule.ensure_uid()
	_frozen_rules_signature = _rules_signature()
	var committed_by_key: Dictionary = {}
	for entry: Dictionary in _manifest.entries:
		committed_by_key[String(entry.get("key", ""))] = entry
	for region: Vector2i in regions:
		var source: String = _source_signature(region, _frozen_rules_signature)
		_source_signatures[str(region)] = source
		for definition: Terrain3DFoliageDefinition in _preset.definitions:
			if definition == null or not definition.enabled or definition.terrain_material == null:
				continue
			definition.ensure_uid()
			var material_id: int = -1
			for i: int in range(_terrain.assets.get_texture_count()):
				if _terrain.assets.get_texture_asset(i) == definition.terrain_material:
					material_id = i
					break
			if material_id < 0:
				# An invalidated association must retire its previously baked owners.
				continue
			var total_weight: float = 0.0
			for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
				if rule != null and rule.enabled and rule.density_per_square_meter > 0.0:
					total_weight += maxf(rule.relative_weight, 0.0)
			for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
				if rule == null or not rule.enabled or rule.density_per_square_meter <= 0.0:
					continue
				var error: String = rule.validate()
				if error != "":
					continue
				var mesh_id: int = -1
				for mesh_index: int in range(_terrain.assets.get_mesh_count()):
					if _terrain.assets.get_mesh_asset(mesh_index) == rule.mesh_asset:
						mesh_id = mesh_index
						break
				if mesh_id < 0:
					continue
				if _preset.global_density_multiplier <= 0.0 or rule.relative_weight <= 0.0:
					continue
				if not rule.validate_spacing(rule.density_per_square_meter * _preset.global_density_multiplier
						* rule.relative_weight / maxf(total_weight, 0.001)).is_empty():
					continue
				var key: String = "%s/%s/%s/%d,%d" % [_preset.stable_uid, definition.stable_uid, rule.stable_uid, region.x, region.y]
				if mode == "generate" and committed_by_key.has(key) and not String(committed_by_key[key].get("source", "")).is_empty():
					continue
				_pending_keys[key] = source
				requests.append({"key": key, "region": region, "mesh_id": mesh_id})
				rules.append({"uid": definition.stable_uid + "/" + rule.stable_uid, "material_id": material_id, "mesh_id": mesh_id,
					"key": key, "region": region,
					"density": rule.density_per_square_meter * _preset.global_density_multiplier,
					"weight": rule.relative_weight / maxf(total_weight, 0.001),
					"min_scale": rule.min_scale, "max_scale": rule.max_scale,
					"random_yaw": rule.random_yaw,
					"min_height": rule.min_elevation, "max_height": rule.max_elevation,
					"min_slope": rule.min_slope, "max_slope": rule.max_slope,
					"normal_align": rule.normal_alignment, "spacing": rule.minimum_spacing,
					"cluster": rule.clustering_strength, "cluster_scale": rule.clustering_scale,
					"excluded_materials": rule.excluded_material_ids})
	requests.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return String(a["key"]) < String(b["key"]))
	var changes: Dictionary = _manifest.prepare_owner_changes(requests, regions, mode,
		Callable(_source_job, "owner_is_used") if _source_job != null else Callable(_terrain.instancer, "is_owner_id_used"))
	if not bool(changes.get("ok", false)):
		_config_error = String(changes.get("error", "Cannot stage foliage ownership"))
		_discard_pending()
		return {}
	_pending_manifest = changes["manifest"]
	for request: Dictionary in requests:
		var record: Dictionary = request.duplicate(true)
		record["owner"] = changes["owners"][String(record["key"])]
		_pending_records.append(record)
	for record: Dictionary in changes["removals"]:
		_pending_removals.append({"region": record["region"], "owner": record["owner"],
			"mesh_id": -1, "transforms": []})
		var retired: Dictionary = record.duplicate(true)
		retired["retired"] = true
		_pending_records.append(retired)
	for rule: Dictionary in rules:
		rule["owner"] = changes["owners"][String(rule["key"])]
	rules.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		var ar: Vector2i = a["region"]
		var br: Vector2i = b["region"]
		if ar.x != br.x:
			return ar.x < br.x
		if ar.y != br.y:
			return ar.y < br.y
		return String(a["uid"]) < String(b["uid"]))
	return {"rules": rules, "seed": _preset.random_seed,
		"region_limit": _preset.region_instance_limit, "job_limit": _preset.job_instance_limit,
		"scratch_mb": _preset.scratch_budget_mb}

func _start(mode: String) -> void:
	if _active or _saving or _preparation != null or _terrain == null or (mode != "clear" and _preset == null):
		return
	_set_state(JobState.VALIDATING)
	if not _recover_journal():
		return
	if _manifest_path != _terrain.data_directory.path_join(MANIFEST_SCRIPT.FILE_NAME):
		_load_manifest()
		_import_legacy_manifest()
	if _manifest == null or not _ledger_error.is_empty():
		_fail(_ledger_error)
		return
	if not _import_legacy_manifest():
		_fail(_ledger_error)
		return
	var validation: Dictionary = _validation(mode)
	_problems.assign(validation["problems"])
	if validation["kind"] == "error":
		_fail("\n".join(_problems))
		return
	var regions: Array[Vector2i] = []
	regions.assign(validation["regions"])
	_pending_mode = mode
	_pending_regions = regions
	_source_job = SOURCE_JOB.new()
	if not _source_job.begin(_terrain, regions, mode != "clear"):
		_fail("Cannot start foliage source preparation.")
		_source_job = null
		return
	_preparation = _source_job
	_active = true
	_set_busy(true)
	set_process(true)
	_status.text = "Preparing source revisions and instance ownership…"

func _continue_start() -> void:
	var mode: String = _pending_mode
	var regions: Array[Vector2i] = _pending_regions.duplicate()
	_active = false
	_set_busy(false)
	set_process(false)
	_frozen_rules_signature = _rules_signature()
	if mode == "clear":
		_discard_pending()
		var changes: Dictionary = _manifest.prepare_owner_changes([], regions, "clear")
		_pending_manifest = changes["manifest"]
		var batches: Array[Dictionary] = []
		for location: Vector2i in regions:
			for record: Dictionary in _manifest.region_records(location):
				batches.append({"region": location, "owner": int(record["owner"]),
					"mesh_id": -1,
					"transforms": []})
		if batches.size() > 0:
			_commit(batches)
		else:
			_discard_pending()
			_set_state(JobState.DONE)
			_status.text = "No generated owners in scope."
		return
	var config: Dictionary = _build_config(regions, mode)
	if config.is_empty():
		_fail(_config_error)
		_discard_pending()
		return
	if (config["rules"] as Array).is_empty() and not _pending_removals.is_empty():
		_commit(_pending_removals.duplicate(true))
		return
	if (config["rules"] as Array).is_empty():
		_set_state(JobState.DONE)
		_status.text = "No active rules or generated owners in scope." if _problems.is_empty() else "\n".join(_problems)
		_discard_pending()
		return
	_active = _generator.start(_terrain, config, regions)
	if not _active:
		_fail(String(_generator.status().get("error", "Generation could not start")))
		_discard_pending()
		return
	_set_busy(true)
	_set_state(JobState.GENERATING)
	set_process(true)

func _process(_delta: float) -> void:
	if _saving and _transaction_job != null:
		_poll_commit()
		return
	if _preparation != null:
		if not visible:
			_preparation.cancel()
		var preparation: Dictionary = _preparation.poll()
		_status.text = "Preparing source ownership %d/%d…" % [int(preparation.get("index", 0)), int(preparation.get("count", 0))]
		if bool(preparation.get("running", false)):
			return
		_preparation = null
		if not bool(preparation.get("ok", false)):
			_active = false
			_set_busy(false)
			set_process(false)
			if bool(preparation.get("cancelled", false)):
				_status.text = "Foliage cancelled before publication."
			else:
				_fail(String(preparation.get("error", "Foliage source preparation failed")))
			_source_job = null
			return
		_continue_start()
		return
	if not _active:
		return
	if not visible:
		_abort()
		return
	var result: Dictionary = _generator.step_budgeted(GENERATION_BUDGET)
	_last_job_metrics = result.duplicate()
	_progress.value = 0.7 * float(result.get("progress", 0.0))
	_status.text = "Generating region %d/%d; rule %s; %d instances" % [int(result.get("region_index", 0)) + 1,
		int(result.get("region_count", 0)), String(result.get("rule_uid", "")), int(result.get("instances", 0))]
	if bool(result.get("running", false)):
		return
	_active = false
	set_process(false)
	_set_busy(false)
	if not bool(result.get("ready", false)):
		_fail("Generation failed: " + String(result.get("error", "Unknown error")))
		_discard_pending()
		return
	var batches: Array = _generator.take_batches()
	batches.append_array(_pending_removals)
	_commit(batches)

func _abort() -> void:
	if _saving and _transaction_job != null:
		_transaction_job.cancel_async()
		_status.text = "Cancelling foliage staging; waiting for worker acknowledgment…"
		set_process(true)
		return
	if _preparation != null:
		_preparation.cancel()
		_status.text = "Cancelling source preparation…"
		set_process(true)
		return
	if _generator != null:
		_generator.cancel()
	_active = false
	_set_state(JobState.IDLE)
	_discard_pending()
	set_process(false)
	_set_busy(false)
	_status.text = "Cancelled. Previously committed instances preserved."

func _set_busy(value: bool) -> void:
	for control: Button in [_generate, _regenerate, _clear]:
		if control != null:
			control.disabled = value
	if _cancel != null:
		_cancel.disabled = not value

# Older journals have no ledger snapshot. Preserve them rather than guessing
# ownership or discarding the only recovery record during automatic migration.
const JOURNAL_PATH: String = "user://terrain3d_foliage_pending.dat"

func _recover_journal() -> bool:
	if _terrain == null:
		return true
	if FileAccess.file_exists(JOURNAL_PATH):
		_fail("Legacy foliage recovery journal needs manual migration; preserved at " + JOURNAL_PATH)
		return false
	var transaction = TRANSACTION_SCRIPT.new()
	transaction.progress = _transaction_phase
	var result: Dictionary = transaction.recover(_terrain)
	if not bool(result.get("ok", false)):
		_fail(String(result.get("error", "Foliage recovery failed")))
		return false
	if bool(result.get("recovered", false)):
		_manifest = result["manifest"]
		_status.text = "Recovered the complete foliage transaction, including ownership."
	return true

func _commit(batches: Array) -> void:
	if _saving or _terrain == null or batches.is_empty():
		return
	if not visible:
		_abort()
		return
	if _pending_manifest == null or _pending_manifest.terrain_identity != _terrain_identity(_terrain):
		_fail("Terrain ownership or data directory changed; restart generation.")
		_discard_pending()
		return
	if not _commit_guard():
		_fail("Terrain, rules, asset slots or instance ownership changed; restart generation.")
		_discard_pending()
		return
	var after: Terrain3DFoliageManifest = _pending_manifest.duplicate(true) as Terrain3DFoliageManifest
	after.publication_generation = _manifest.publication_generation + 1
	var entry_indices: Dictionary = {}
	for index: int in range(after.entries.size()):
		entry_indices[String(after.entries[index]["key"])] = index
	if _pending_mode != "clear":
		for record: Dictionary in _pending_records:
			if bool(record.get("retired", false)):
				continue
			var key: String = record["key"]
			var index: int = int(entry_indices[key])
			after.entries[index]["source"] = _pending_keys.get(key, "")
			after.entries[index]["seed"] = _pending_seed
			after.entries[index]["revision"] = int(after.entries[index].get("revision", 0)) + 1
			after.entries[index]["mesh_id"] = int(record["mesh_id"])
	_saving = true
	_set_busy(true)
	_set_state(JobState.STAGING)
	_transaction_job = _new_transaction()
	_commit_terrain = _terrain
	_transaction_job.progress = _transaction_phase
	var begun: Dictionary = _transaction_job.begin_async(_terrain, batches, _manifest, after, _source_signatures,
		_commit_guard, _preset.scratch_budget_mb if _preset != null else 64)
	if not bool(begun.get("ok", false)):
		_saving = false
		_set_busy(false)
		_fail(String(begun.get("error", "Cannot stage foliage transaction")))
		_transaction_job = null
		return
	set_process(true)

func _commit_guard() -> bool:
	if not is_instance_valid(_terrain) or _rules_signature() != _frozen_rules_signature:
		return false
	if _source_job != null:
		return _source_job.valid_sources() and _source_job.valid_instances()
	for region: Vector2i in _pending_regions:
		if _source_signatures.has(str(region)) and _source_signature(region) != _source_signatures[str(region)]:
			return false
	return true

func _poll_commit() -> void:
	if not visible:
		_transaction_job.cancel_async()
	var result: Dictionary = _transaction_job.poll_async(COMMIT_BUDGET)
	if bool(result.get("running", false)):
		_status.text = "Foliage %s: region %d/%d; I/O %d KiB" % [String(result.get("phase", "staging")),
			int(result.get("regions_prepared", 0)), int(result.get("region_count", 0)),
			int(_transaction_job.metrics.get("io_bytes", 0)) / 1024]
		return
	_saving = false
	_set_busy(false)
	set_process(false)
	_last_job_metrics["commit"] = result.get("metrics", {})
	var transaction = _transaction_job
	_transaction_job = null
	_source_job = null
	_discard_pending()
	if not bool(result.get("ok", false)):
		if bool(result.get("cancelled", false)):
			_set_state(JobState.IDLE)
			_status.text = "Foliage cancelled before publication."
		else:
			_fail(String(result.get("error", "Foliage publication failed")))
		return
	if _terrain == _commit_terrain:
		_manifest = result["manifest"]
	var manager: EditorUndoRedoManager = EditorInterface.get_editor_undo_redo()
	if not is_instance_valid(_commit_terrain):
		_status.text = "Foliage saved to the original terrain data directory."
		return
	manager.create_action("Generate Terrain3D Foliage", UndoRedo.MERGE_DISABLE, _commit_terrain)
	manager.add_do_method(transaction, "restore_history", _commit_terrain, "after")
	manager.add_undo_method(transaction, "restore_history", _commit_terrain, "before")
	# Retain the transaction and its immutable snapshots until this undo action
	# is evicted. A scene switch never changes its terrain/ledger target.
	manager.add_do_reference(transaction)
	manager.commit_action(false)
	_set_state(JobState.DONE)
	_status.text = String(result.get("warning", "Saved %d regions with matching foliage ownership." % int(result.get("regions", 0))))
	if not _problems.is_empty():
		_status.text += "\nSkipped invalid rules:\n" + "\n".join(_problems)
	_progress.value = 1.0

func _new_transaction():
	return ASYNC_TRANSACTION.new()

func _persist_manifest(ledger: Terrain3DFoliageManifest, terrain: Terrain3D) -> bool:
	# Importing a legacy ledger records already-published owners; generation and
	# undo publication use foliage_transaction.gd instead.
	if terrain.data_directory.is_empty() or ledger.terrain_identity != _terrain_identity(terrain):
		return false
	var destination: String = terrain.data_directory.path_join(MANIFEST_SCRIPT.FILE_NAME)
	var staged_path: String = destination.get_basename() + "." + Resource.generate_scene_unique_id() + ".tres"
	if ResourceSaver.save(ledger, staged_path) != OK:
		return false
	return DirAccess.rename_absolute(ProjectSettings.globalize_path(staged_path), ProjectSettings.globalize_path(destination)) == OK

func _transaction_phase(phase: String) -> void:
	if phase == "RESTORING":
		_set_state(JobState.RESTORING)
	else:
		_set_state(JobState.COMMITTING)
	var messages: Dictionary = {"PREPARED": "Saving instance files…", "REGIONS_APPLIED": "Saving foliage ownership…",
		"LEDGER_APPLIED": "Finishing foliage commit…", "COMMITTED": "Foliage saved.", "RESTORING": "Restoring previous foliage…"}
	_status.text = String(messages.get(phase, "Foliage: " + phase.to_lower().replace("_", " ")))

func _set_state(state: JobState) -> void:
	_state = state

func _fail(message: String) -> void:
	_set_state(JobState.FAILED)
	_status.text = message

func _on_popup_hidden() -> void:
	if _active or _saving or _preparation != null:
		_abort()

func _on_terrain_exiting() -> void:
	_generator.cancel()
	if _transaction_job != null:
		_transaction_job.cancel_async()
	if _preparation != null:
		_preparation.cancel()
	_active = false

func _exit_tree() -> void:
	if _transaction_job != null:
		_transaction_job.shutdown_async()
	if _source_job != null:
		_source_job.shutdown()
	_generator.cancel()
	for resource: Resource in _watched_resources:
		if is_instance_valid(resource) and resource.changed.is_connected(_on_resource_changed):
			resource.changed.disconnect(_on_resource_changed)

func _on_history_changed() -> void:
	call_deferred("_sync_after_history")

func _sync_after_history() -> void:
	if not is_instance_valid(_terrain):
		return
	if _active or _saving or _preparation != null:
		_abort()
	_load_manifest()
	_sync_preset()
	_refresh_editor()

func _save_as() -> void:
	if _preset != null:
		_save_dialog.popup_centered_ratio(0.6)

func _save_preset_to(path: String) -> void:
	if _preset == null:
		return
	_preset.ensure_uid()
	var err: Error = ResourceSaver.save(_preset, path)
	_status.text = "Preset saved." if err == OK else "Preset save failed: %s" % str(err)
