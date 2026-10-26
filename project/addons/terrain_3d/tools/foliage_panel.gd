@tool
extends PopupPanel

const PRESET_SCRIPT = preload("res://addons/terrain_3d/src/foliage_preset.gd")
const DEFINITION_SCRIPT = preload("res://addons/terrain_3d/src/foliage_definition.gd")
const RULE_SCRIPT = preload("res://addons/terrain_3d/src/foliage_mesh_rule.gd")
const EditorUI = preload("res://addons/terrain_3d/src/editor_ui_util.gd")

var plugin: Terrain3DEditorPluginBase
var _terrain: Terrain3D
var _preset: Terrain3DFoliagePreset
var _generator: Terrain3DFoliageGenerator
var _active: bool = false
var _pending_mode: String = ""
var _pending_regions: Array[Vector2i] = []
var _pending_keys: Dictionary[String, String] = {}
var _pending_records: Array[Dictionary] = []
var _saving: bool = false
var _snapshot_serial: int = 0

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
	set_process(false)

func open() -> void:
	set_terrain(plugin.terrain if is_instance_valid(plugin) else null)
	if _preset == null and _terrain != null:
		_set_preset(PRESET_SCRIPT.new())
	_recover_journal()
	popup_centered(EditorUI.popup_size_for_screen(self, Vector2i(940, 700), Vector2i(650, 480)))

func set_terrain(p_terrain: Terrain3D) -> void:
	if _terrain == p_terrain:
		return
	if _active:
		_abort()
	_terrain = p_terrain
	_set_preset(_terrain.foliage_profile as Terrain3DFoliagePreset if _terrain != null else null)

func _set_preset(p_preset: Terrain3DFoliagePreset) -> void:
	_preset = p_preset
	if _preset != null:
		_preset.ensure_uid()
	if _terrain != null:
		_terrain.foliage_profile = _preset
	if is_instance_valid(_preset_picker):
		_preset_picker.edited_resource = _preset
	_refresh_editor()

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
	settings.add_child(_scope)
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
		return
	var stale: int = 0
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
		enabled.toggled.connect(func(value: bool) -> void: definition.enabled = value)
		header.add_child(enabled)
		var resource_picker := EditorResourcePicker.new()
		resource_picker.base_type = "Terrain3DTextureAsset"
		resource_picker.edited_resource = definition.terrain_material
		resource_picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		resource_picker.resource_changed.connect(func(resource: Resource) -> void: definition.terrain_material = resource as Terrain3DTextureAsset)
		header.add_child(resource_picker)
		_button("Add Mesh", header, func() -> void:
			definition.mesh_bank.append(RULE_SCRIPT.new())
			_refresh_editor())
		_button("Remove", header, func() -> void:
			_preset.definitions.erase(definition)
			_refresh_editor())
		for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
			if rule == null:
				continue
			var line := HBoxContainer.new()
			box.add_child(line)
			var mesh_picker := EditorResourcePicker.new()
			mesh_picker.base_type = "Terrain3DMeshAsset"
			mesh_picker.edited_resource = rule.mesh_asset
			mesh_picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			mesh_picker.resource_changed.connect(func(resource: Resource) -> void: rule.mesh_asset = resource as Terrain3DMeshAsset)
			line.add_child(mesh_picker)
			_button("Rules…", line, func() -> void: EditorInterface.edit_resource(rule))
			_button("×", line, func() -> void:
				definition.mesh_bank.erase(rule)
				_refresh_editor())
		var divider := HSeparator.new()
		box.add_child(divider)
	for region: Vector2i in _selected_regions():
		if _is_region_stale(region):
			stale += 1
	if is_instance_valid(_status) and not _active:
		_status.text = "%d definitions · %d stale/ungenerated regions in scope" % [_preset.definitions.size(), stale]

func _add_definition() -> void:
	if _preset == null:
		return
	var definition: Terrain3DFoliageDefinition = DEFINITION_SCRIPT.new()
	definition.mesh_bank.append(RULE_SCRIPT.new())
	_preset.definitions.append(definition)
	_refresh_editor()

func _selected_regions() -> Array[Vector2i]:
	var locations: Array[Vector2i] = []
	if _terrain == null or _terrain.data == null:
		return locations
	if is_instance_valid(_scope) and _scope.selected == 0:
		var location: Vector2i = _terrain.data.get_region_location(plugin.mouse_global_position)
		if _terrain.data.has_region(location):
			locations.append(location)
	else:
		locations.assign(_terrain.data.get_region_locations())
	locations.sort_custom(func(a: Vector2i, b: Vector2i) -> bool: return a.x < b.x or (a.x == b.x and a.y < b.y))
	return locations

func _resource_key(resource: Resource) -> String:
	if resource == null:
		return ""
	return "%s/%s/%s" % [resource.resource_path, resource.resource_scene_unique_id, resource.resource_name]

func _source_signature(region: Vector2i) -> String:
	if _terrain == null:
		return ""
	var hasher := HashingContext.new()
	hasher.start(HashingContext.HASH_SHA256)
	var size: int = int(_terrain.region_size)
	# Include neighbor maps in the hash because spacing and sampling depend on border context.
	for dz: int in range(-1, 2):
		for dx: int in range(-1, 2):
			var neighbor: Terrain3DRegion = _terrain.data.get_region(region + Vector2i(dx, dz))
			if neighbor == null:
				continue
			var height: Image = neighbor.get_height_map()
			var control: Image = neighbor.get_control_map()
			if height != null:
				hasher.update(height.get_data())
			if control != null:
				hasher.update(control.get_data())
	var signature_records: Array[String] = []
	for definition: Terrain3DFoliageDefinition in _preset.definitions:
		if definition == null:
			continue
		var rules: Array[String] = []
		for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
			if rule == null:
				continue
			rules.append(JSON.stringify({"uid": rule.stable_uid, "enabled": rule.enabled,
				"mesh": _resource_key(rule.mesh_asset), "density": rule.density_per_square_meter,
				"weight": rule.relative_weight, "min_scale": rule.min_scale, "max_scale": rule.max_scale,
				"yaw": rule.random_yaw, "slope": [rule.min_slope, rule.max_slope],
				"height": [rule.min_elevation, rule.max_elevation], "alignment": rule.normal_alignment,
				"spacing": rule.minimum_spacing, "cluster": rule.clustering_strength,
				"cluster_scale": rule.clustering_scale, "exclusions": Array(rule.excluded_material_ids)}))
		rules.sort()
		signature_records.append(JSON.stringify({"uid": definition.stable_uid, "enabled": definition.enabled,
			"material": _resource_key(definition.terrain_material), "rules": rules}))
	signature_records.sort()
	var shader_state: Array = []
	if _terrain.material != null:
		for parameter: String in ["auto_slope", "auto_height_reduction", "auto_base_texture", "auto_overlay_texture"]:
			shader_state.append(_terrain.material.get_shader_param(parameter))
	var details: String = JSON.stringify({"seed": _preset.random_seed, "density": _preset.global_density_multiplier,
		"region_size": size, "limit": _preset.region_instance_limit,
		"shader_auto": shader_state, "rules": signature_records})
	hasher.update(details.to_utf8_buffer())
	return hasher.finish().hex_encode()

func _is_region_stale(region: Vector2i) -> bool:
	if _preset == null or _preset.manifest == null:
		return true
	var source: String = _source_signature(region)
	var found: bool = false
	for record: Dictionary in _preset.manifest.region_records(region):
		found = true
		if String(record.get("source", "")) != source:
			return true
	return not found

func _build_config(regions: Array[Vector2i], mode: String) -> Dictionary:
	var rules: Array[Dictionary] = []
	_pending_keys.clear()
	_pending_records.clear()
	_preset.ensure_uid()
	for region: Vector2i in regions:
		var source: String = _source_signature(region)
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
				push_error("Foliage definition references an unregistered terrain texture")
				return {}
			var total_weight: float = 0.0
			for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
				if rule != null and rule.enabled:
					total_weight += maxf(rule.relative_weight, 0.0)
			for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
				if rule == null or not rule.enabled or rule.density_per_square_meter <= 0.0:
					continue
				var error: String = rule.validate()
				if error != "":
					push_error(error)
					return {}
				var mesh_id: int = -1
				for mesh_index: int in range(_terrain.assets.get_mesh_count()):
					if _terrain.assets.get_mesh_asset(mesh_index) == rule.mesh_asset:
						mesh_id = mesh_index
						break
				if mesh_id < 0:
					push_error("Foliage rule references an unregistered mesh")
					return {}
				var key: String = "%s/%s/%s/%d,%d" % [_preset.stable_uid, definition.stable_uid, rule.stable_uid, region.x, region.y]
				var existing: int = _preset.manifest.owner_for(key)
				if mode == "generate" and existing > 0:
					var committed: bool = false
					for entry: Dictionary in _preset.manifest.entries:
						if String(entry.get("key", "")) == key and String(entry.get("source", "")) != "":
							committed = true
					if committed:
						continue
				var owner: int = _preset.manifest.assign_owner(key, region, Callable(_terrain.instancer, "is_owner_id_used"))
				if owner <= 0:
					return {}
				_pending_keys[key] = source
				_pending_records.append({"key": key, "region": region, "mesh_id": mesh_id, "owner": owner})
				rules.append({"uid": definition.stable_uid + "/" + rule.stable_uid, "material_id": material_id, "mesh_id": mesh_id,
					"owner": owner, "region": region,
					"density": rule.density_per_square_meter * _preset.global_density_multiplier,
					"weight": rule.relative_weight / maxf(total_weight, 0.001),
					"min_scale": rule.min_scale, "max_scale": rule.max_scale,
					"random_yaw": rule.random_yaw,
					"min_height": rule.min_elevation, "max_height": rule.max_elevation,
					"min_slope": rule.min_slope, "max_slope": rule.max_slope,
					"normal_align": rule.normal_alignment, "spacing": rule.minimum_spacing,
					"cluster": rule.clustering_strength, "cluster_scale": rule.clustering_scale,
					"excluded_materials": rule.excluded_material_ids})
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
	if _active or _terrain == null or _preset == null:
		return
	var regions: Array[Vector2i] = _selected_regions()
	if regions.is_empty():
		_status.text = "No valid regions in scope."
		return
	_preset.ensure_uid()
	_pending_mode = mode
	_pending_regions = regions
	if mode == "clear":
		var batches: Array[Dictionary] = []
		for location: Vector2i in regions:
			for record: Dictionary in _preset.manifest.region_records(location):
				batches.append({"region": location, "owner": int(record["owner"]),
					"mesh_id": clampi(int(record.get("mesh_id", 0)), 0, maxi(0, _terrain.assets.get_mesh_count() - 1)),
					"transforms": []})
		if batches.size() > 0:
			_commit(batches)
		return
	var config: Dictionary = _build_config(regions, mode)
	if config.is_empty() or (config["rules"] as Array).is_empty():
		_status.text = "Nothing to generate (existing groups remain unchanged)."
		return
	_active = _generator.start(_terrain, config, regions)
	if not _active:
		_status.text = String(_generator.status().get("error", "Generation could not start"))
		return
	_set_busy(true)
	set_process(true)

func _process(_delta: float) -> void:
	if not _active:
		return
	var result: Dictionary = _generator.step(2048)
	_progress.value = float(result.get("progress", 0.0))
	_status.text = "Generating region %d/%d — %d instances" % [int(result.get("region_index", 0)) + 1,
		int(result.get("region_count", 0)), int(result.get("instances", 0))]
	if bool(result.get("running", false)):
		return
	_active = false
	set_process(false)
	_set_busy(false)
	if not bool(result.get("ready", false)):
		_status.text = "Generation failed: " + String(result.get("error", "Unknown error"))
		return
	var batches: Array = _generator.take_batches()
	_commit(batches)

func _abort() -> void:
	if _generator != null:
		_generator.cancel()
	_active = false
	set_process(false)
	_set_busy(false)
	_status.text = "Cancelled. Previously committed instances preserved."

func _set_busy(value: bool) -> void:
	for control: Button in [_generate, _regenerate, _clear]:
		if control != null:
			control.disabled = value
	if _cancel != null:
		_cancel.disabled = not value

func _snapshot_path(index: int) -> String:
	return "user://terrain3d_foliage_undo_%d_%d.res" % [_snapshot_serial, index]

const JOURNAL_PATH: String = "user://terrain3d_foliage_pending.dat"

func _write_journal(regions: Array[Vector2i], paths: PackedStringArray) -> bool:
	var file: FileAccess = FileAccess.open(JOURNAL_PATH, FileAccess.WRITE)
	if file == null:
		return false
	file.store_var({"directory": _terrain.data_directory, "regions": regions, "snapshots": paths})
	file.close()
	return true

func _clear_journal() -> void:
	if FileAccess.file_exists(JOURNAL_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(JOURNAL_PATH))

func _recover_journal() -> void:
	if _terrain == null or not FileAccess.file_exists(JOURNAL_PATH):
		return
	var file: FileAccess = FileAccess.open(JOURNAL_PATH, FileAccess.READ)
	if file == null:
		return
	var journal: Dictionary = file.get_var()
	file.close()
	if String(journal.get("directory", "")) != _terrain.data_directory:
		return
	var regions: Array[Vector2i] = []
	regions.assign(journal.get("regions", []))
	var paths: PackedStringArray = journal.get("snapshots", PackedStringArray())
	_restore_snapshots(regions, paths)
	_clear_journal()
	_status.text = "Recovered interrupted foliage commit from disk snapshots."

func _capture_snapshots(regions: Array[Vector2i]) -> PackedStringArray:
	_snapshot_serial += 1
	var paths := PackedStringArray()
	for index: int in range(regions.size()):
		var terrain_region: Terrain3DRegion = _terrain.data.get_region(regions[index])
		var path: String = _snapshot_path(index)
		if terrain_region == null or ResourceSaver.save(terrain_region.duplicate(true), path) != OK:
			return PackedStringArray()
		paths.append(path)
	return paths

func _commit(batches: Array) -> void:
	if _saving or _terrain == null or batches.is_empty():
		return
	_saving = true
	_snapshot_serial += 1
	var affected: Array[Vector2i] = []
	for batch: Dictionary in batches:
		var region: Vector2i = batch["region"]
		if not affected.has(region):
			affected.append(region)
	# Persist the precommit state before touching live instance stores.
	var before_paths: PackedStringArray = _capture_snapshots(affected)
	if before_paths.size() != affected.size() or not _write_journal(affected, before_paths):
		_status.text = "Unable to stage recovery files. No changes committed."
		_saving = false
		return
	var result: Dictionary = _terrain.instancer.replace_foliage_owned_regions(batches, true)
	if not bool(result.get("ok", false)):
		_clear_journal()
		_status.text = "Commit failed: " + String(result.get("error", "Unknown error"))
		_saving = false
		return
	var output_dir: String = _terrain.data_directory
	var saved: bool = not output_dir.is_empty()
	if saved:
		for region: Vector2i in affected:
			_terrain.data.save_region(region, output_dir, _terrain.save_16_bit)
			var item: Terrain3DRegion = _terrain.data.get_region(region)
			if item != null and item.modified:
				saved = false
	if not saved:
		_restore_snapshots(affected, before_paths)
		_clear_journal()
		_status.text = "Save failed; restored previous instance data."
		_saving = false
		return
	var previous_manifest: Array[Dictionary] = _preset.manifest.entries.duplicate(true)
	if _pending_mode == "clear":
		for region: Vector2i in affected:
			for record: Dictionary in _preset.manifest.region_records(region):
				_preset.manifest.entries.erase(record)
	else:
		for record: Dictionary in _pending_records:
			var key: String = record["key"]
			_preset.manifest.mark_baked(key, _pending_keys.get(key, ""), _preset.random_seed)
			for index: int in range(_preset.manifest.entries.size()):
				if String(_preset.manifest.entries[index].get("key", "")) == key:
					var edited: Dictionary = _preset.manifest.entries[index].duplicate(true)
					edited["mesh_id"] = int(record["mesh_id"])
					_preset.manifest.entries[index] = edited
	if not _preset.resource_path.is_empty():
		if ResourceSaver.save(_preset, _preset.resource_path) != OK:
			_preset.manifest.entries = previous_manifest
			_restore_snapshots(affected, before_paths)
			_clear_journal()
			_status.text = "Preset save failed; restored old placements."
			_saving = false
			return
	var after_paths: PackedStringArray = _capture_snapshots(affected)
	if after_paths.size() == affected.size():
		var manager: EditorUndoRedoManager = EditorInterface.get_editor_undo_redo()
		manager.create_action("Generate Terrain3D Foliage")
		manager.add_do_method(self, "_restore_transaction_snapshot", affected, after_paths,
			_preset.manifest.entries.duplicate(true))
		manager.add_undo_method(self, "_restore_transaction_snapshot", affected, before_paths, previous_manifest)
		manager.commit_action(false)
	_clear_journal()
	EditorInterface.mark_scene_as_unsaved()
	_status.text = "Saved %d regions; %d new instances." % [int(result.get("regions", 0)), int(result.get("instances", 0))]
	_progress.value = 1.0
	_saving = false

func _restore_snapshots(regions: Array[Vector2i], paths: PackedStringArray) -> void:
	for index: int in range(mini(regions.size(), paths.size())):
		var snapshot: Terrain3DRegion = ResourceLoader.load(paths[index], "Terrain3DRegion", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DRegion
		var original: Terrain3DRegion = _terrain.data.get_region(regions[index])
		if snapshot != null and original != null:
			original.set_instances(snapshot.get_instances())
			_terrain.data.save_region(regions[index], _terrain.data_directory, _terrain.save_16_bit)
	_terrain.instancer.update_mmis()

func _restore_transaction_snapshot(regions: Array[Vector2i], paths: PackedStringArray, ledger: Array) -> void:
	_restore_snapshots(regions, paths)
	if _preset != null:
		_preset.manifest.entries.assign(ledger)
		if not _preset.resource_path.is_empty():
			ResourceSaver.save(_preset, _preset.resource_path)
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
