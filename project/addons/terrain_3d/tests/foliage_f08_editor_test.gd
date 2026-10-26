@tool
extends EditorScript

# Explicit editor acceptance test. Open a disposable two-region terrain fixture,
# assign a foliage preset with at least three terrain materials and mesh rules,
# and set metadata "terrain3d_foliage_f08_fixture" = true on its Terrain3D node.
# Run from File > Run in Godot's script editor. Never use on production terrain.

const MAX_STEPS: int = 25000
const STEP_BUDGET: int = 4096
var _passes: int = 0
var _failures: Array[String] = []

func _run() -> void:
	var terrain: Terrain3D = _find_terrain(EditorInterface.get_edited_scene_root())
	if terrain == null or not bool(terrain.get_meta("terrain3d_foliage_f08_fixture", false)):
		push_error("F08 requires a disposable Terrain3D fixture with terrain3d_foliage_f08_fixture=true")
		return
	var preset: Terrain3DFoliagePreset = terrain.foliage_profile as Terrain3DFoliagePreset
	if preset == null:
		push_error("F08 fixture requires Terrain3D.foliage_profile")
		return
	preset.ensure_uid()
	var locations: Array[Vector2i] = []
	locations.assign(terrain.data.get_region_locations())
	locations.sort_custom(func(a: Vector2i, b: Vector2i) -> bool: return a.x < b.x or (a.x == b.x and a.y < b.y))
	if locations.size() < 2:
		push_error("F08 fixture requires two neighboring regions")
		return
	var neighbor_pair: Array[Vector2i] = []
	for location: Vector2i in locations:
		if locations.has(location + Vector2i(1, 0)):
			neighbor_pair.assign([location, location + Vector2i(1, 0)])
			break
	if neighbor_pair.size() != 2:
		push_error("F08 fixture requires two horizontally adjacent regions")
		return
	var saved_preset: String = "user://terrain3d_f08_preset.res"
	var preset_saved: bool = ResourceSaver.save(preset.duplicate(true), saved_preset) == OK
	_check(preset_saved, "Preset resource saves")
	if preset_saved:
		var restored_preset: Terrain3DFoliagePreset = ResourceLoader.load(saved_preset,
			"Terrain3DFoliagePreset", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DFoliagePreset
		_check(restored_preset != null and restored_preset.stable_uid == preset.stable_uid
			and restored_preset.definitions.size() == preset.definitions.size(), "Preset reload preserves definitions")
	var rule_config: Dictionary = _config_for_fixture(terrain, preset, neighbor_pair)
	if rule_config.is_empty():
		return
	var initial: Dictionary = _generate(terrain, rule_config, neighbor_pair)
	_check(bool(initial.get("ok", false)), "Initial generation")
	if not bool(initial.get("ok", false)):
		_report()
		return
	var repeated: Dictionary = _generate(terrain, rule_config, neighbor_pair)
	_check(bool(repeated.get("ok", false)), "Repeat generation")
	_check(_batch_digest(initial.get("batches", [])) == _batch_digest(repeated.get("batches", [])),
		"Identical inputs produce identical output batches")
	var reversed: Array[Vector2i] = neighbor_pair.duplicate()
	reversed.reverse()
	var reordered: Dictionary = _generate(terrain, rule_config, reversed)
	_check(bool(reordered.get("ok", false)), "Region order independence job")
	_check(_batch_digest(initial.get("batches", [])) == _batch_digest(reordered.get("batches", [])),
		"Reordering regions preserves sorted placements")
	var generator := Terrain3DFoliageGenerator.new()
	_check(generator.start(terrain, rule_config, neighbor_pair), "Cancellation job starts")
	generator.step(32)
	generator.cancel()
	_check(not bool(generator.status().get("running", true)) and generator.take_batches().is_empty(),
		"Cancellation leaves no staged batches")
	# Save exact packed pages, including generated/manual ownership and behavior sidecars.
	var originals: Dictionary = {}
	var manual_before: Dictionary = {}
	for region: Vector2i in neighbor_pair:
		var resource: Terrain3DRegion = terrain.data.get_region(region)
		originals[region] = resource.get_instance_page()
		manual_before[region] = _manual_signature(resource.get_instances())
	var sample: Array = initial["batches"]
	var instance_total: int = 0
	for batch: Dictionary in sample:
		instance_total += (batch["transforms"] as Array).size()
	_check(instance_total > 0, "Foliage fixture produces nonempty placements")
	var positive_status: Dictionary = initial.get("status", {})
	_check(int(positive_status.get("instances", -1)) == instance_total and
		int(positive_status.get("rejection_total", -1)) + instance_total == int(positive_status.get("candidate_tests", -2)),
		"Positive placements and rejection categories account for all tested candidates")
	var preview: Dictionary = _foliage_editor(terrain).prepare_owned_regions(sample)
	_check(bool(preview.get("ok", false)) and int(preview.get("instances", -1)) == instance_total and
		int(preview.get("staged_pages", -1)) == neighbor_pair.size() * 2 and
		int(preview.get("staged_bytes", 0)) > 0 and
		int(preview.get("logical_instances", -1)) >= instance_total,
		"Non-publishing staged pages report exact transform and byte counts")
	# Every eligible material is excluded to produce a completed zero-placement job.
	# This must still supply empty owner-replacement batches for an explicit save.
	var empty_config: Dictionary = rule_config.duplicate(true)
	var empty_rules: Array = empty_config["rules"]
	var exclusions := PackedInt32Array()
	for material_index: int in range(mini(128, terrain.assets.get_texture_count())):
		exclusions.append(material_index)
	for index: int in range(empty_rules.size()):
		var empty_rule: Dictionary = empty_rules[index]
		empty_rule["excluded_materials"] = exclusions
		empty_rules[index] = empty_rule
	empty_config["rules"] = empty_rules
	var empty_generation: Dictionary = _generate(terrain, empty_config, neighbor_pair)
	var empty_status: Dictionary = empty_generation.get("status", {})
	var empty_batches: Array = empty_generation.get("batches", [])
	var empty_count: int = 0
	for batch: Dictionary in empty_batches:
		empty_count += (batch["transforms"] as Array).size()
	_check(bool(empty_generation.get("ok", false)) and not empty_batches.is_empty() and
		empty_count == 0 and String(empty_status.get("placement_state", "")) == "zero_instances" and
		int(empty_status.get("rejected", {}).get("excluded_material", 0)) > 0,
		"Zero placement returns valid empty replacement batches with exclusion cause")
	var empty_preview: Dictionary = _foliage_editor(terrain).prepare_owned_regions(empty_batches)
	_check(bool(empty_preview.get("ok", false)) and int(empty_preview.get("instances", -1)) == 0 and
		int(empty_preview.get("staged_pages", -1)) == neighbor_pair.size() * 2,
		"Zero-placement replacement stages valid empty owner pages without publication")
	var valid_terrain: bool = true
	var unique_positions: Dictionary[String, bool] = {}
	for batch: Dictionary in sample:
		var owner_region: Vector2i = batch["region"]
		for transform: Transform3D in batch["transforms"]:
			var position: Vector3 = transform.origin
			if terrain.data.get_region_location(position) != owner_region:
				valid_terrain = false
			if Terrain3DUtil.is_hole(terrain.data.get_control(position)):
				valid_terrain = false
			if absf(terrain.data.get_height(position) - position.y) > 0.1:
				valid_terrain = false
			var key: String = "%s:%s" % [batch["mesh_id"], str(position)]
			if unique_positions.has(key):
				valid_terrain = false
			unique_positions[key] = true
	_check(valid_terrain, "Placements respect holes, sampled height, region bounds and unique positions")
	var invalid: Array[Dictionary] = [{"region": neighbor_pair[0], "owner": 0,
		"mesh_id": -1, "transforms": []}]
	var rejected: Dictionary = _foliage_editor(terrain).replace_owned_regions(invalid, false)
	_check(not bool(rejected.get("ok", true)), "Invalid owner is rejected")
	_check(_same_pages(terrain, originals), "Invalid commit leaves all pages intact")
	var committed: Dictionary = _foliage_editor(terrain).replace_owned_regions(sample, false)
	_check(bool(committed.get("ok", false)), "Valid owned placement batch commits")
	if bool(committed.get("ok", false)):
		for region: Vector2i in neighbor_pair:
			var current: Terrain3DRegion = terrain.data.get_region(region)
			_check(_manual_signature(current.get_instances()) == manual_before[region],
				"Manual placements preserved in region " + str(region))
			var destination: String = "user://terrain3d_f08_region_%d_%d.res" % [region.x, region.y]
			var saved: Error = ResourceSaver.save(current.duplicate(true), destination)
			_check(saved == OK, "Region snapshot saved " + str(region))
			if saved == OK:
				var loaded: Terrain3DRegion = ResourceLoader.load(destination, "Terrain3DRegion",
					ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DRegion
				_check(loaded != null and loaded.get_instance_page() == current.get_instance_page(),
					"Encoded ownership round trips " + str(region))
		# Owner-only deletion is valid even if the original mesh asset was removed.
		var removals: Array[Dictionary] = []
		for batch: Dictionary in sample:
			removals.append({"region": batch["region"], "owner": batch["owner"],
				"mesh_id": -1, "transforms": []})
		var removal_result: Dictionary = _foliage_editor(terrain).replace_owned_regions(removals, false)
		_check(bool(removal_result.get("ok", false)), "Retired owner removal accepts missing mesh slot")
	# Restore fixture regardless of pass/fail. Never change the production scene.
	for region: Vector2i in neighbor_pair:
		var resource: Terrain3DRegion = terrain.data.get_region(region)
		resource.set_instance_page(originals[region])
		resource.modified = true
	terrain.instancer.update_mmis()
	_check(_same_pages(terrain, originals), "Fixture restored after test")
	_report()

func _find_terrain(node: Node) -> Terrain3D:
	if node == null:
		return null
	if node is Terrain3D:
		return node as Terrain3D
	for child: Node in node.get_children():
		var found: Terrain3D = _find_terrain(child)
		if found != null:
			return found
	return null

func _config_for_fixture(terrain: Terrain3D, preset: Terrain3DFoliagePreset,
		regions: Array[Vector2i]) -> Dictionary:
	var rules: Array[Dictionary] = []
	var materials: Dictionary = {}
	for definition: Terrain3DFoliageDefinition in preset.definitions:
		if definition == null or not definition.enabled or definition.terrain_material == null:
			continue
		var material: int = -1
		for i: int in range(terrain.assets.get_texture_count()):
			if terrain.assets.get_texture_asset(i) == definition.terrain_material:
				material = i
				break
		if material < 0:
			continue
		materials[material] = true
		for rule: Terrain3DFoliageMeshRule in definition.mesh_bank:
			if rule == null or not rule.enabled or rule.density_per_square_meter <= 0.0:
				continue
			var mesh: int = -1
			for j: int in range(terrain.assets.get_mesh_count()):
				if terrain.assets.get_mesh_asset(j) == rule.mesh_asset:
					mesh = j
					break
			if mesh < 0:
				continue
			for region: Vector2i in regions:
				var owner: int = 1600000000 + mesh * 2048 + (region.x + 64) * 8 + region.y + 64
				if owner > 2147483647 or terrain.instancer.is_owner_id_used(owner):
					push_error("F08 fixture owner collision; use empty generated owner IDs")
					return {}
				rules.append({"uid": definition.stable_uid + "/" + rule.stable_uid,
					"material_id": material, "mesh_id": mesh, "owner": owner, "region": region,
					"density": rule.density_per_square_meter * preset.global_density_multiplier,
					"weight": rule.relative_weight, "min_scale": rule.min_scale,
					"max_scale": rule.max_scale, "random_yaw": rule.random_yaw,
					"min_height": rule.min_elevation, "max_height": rule.max_elevation,
					"min_slope": rule.min_slope, "max_slope": rule.max_slope,
					"normal_align": rule.normal_alignment, "spacing": rule.minimum_spacing,
					"cluster": rule.clustering_strength, "cluster_scale": rule.clustering_scale,
					"excluded_materials": rule.excluded_material_ids})
	if materials.size() < 3 or rules.is_empty():
		push_error("F08 fixture needs three registered foliage materials and mesh rules")
		return {}
	return {"seed": preset.random_seed, "rules": rules, "region_limit": preset.region_instance_limit,
		"job_limit": preset.job_instance_limit, "scratch_mb": preset.scratch_budget_mb}

func _generate(terrain: Terrain3D, config: Dictionary, regions: Array[Vector2i]) -> Dictionary:
	var generator := Terrain3DFoliageGenerator.new()
	if not generator.start(terrain, config, regions):
		return {"ok": false, "error": generator.status().get("error", "start failed")}
	var steps: int = 0
	while steps < MAX_STEPS and bool(generator.status().get("running", false)):
		generator.step(STEP_BUDGET)
		steps += 1
	var status: Dictionary = generator.status()
	if not bool(status.get("ready", false)):
		return {"ok": false, "error": status.get("error", "budget exceeded")}
	return {"ok": true, "batches": generator.take_batches(), "steps": steps, "status": status}

func _batch_digest(batches: Array) -> PackedByteArray:
	var records: Array[Dictionary] = []
	for batch: Dictionary in batches:
		var region: Vector2i = batch["region"]
		var xforms: Array = Array(batch["transforms"])
		var tokens: Array[String] = []
		for transform: Transform3D in xforms:
			tokens.append(str(var_to_bytes(transform).hex_encode()))
		tokens.sort()
		records.append({"region": str(region), "owner": batch["owner"], "mesh": batch["mesh_id"],
			"transforms": tokens})
	records.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return str(a["region"]) + str(a["owner"]) < str(b["region"]) + str(b["owner"]))
	return var_to_bytes(records)

func _manual_signature(instances: Dictionary) -> PackedByteArray:
	var manual: Array[String] = []
	for mesh_id: Variant in instances.keys():
		var cells: Dictionary = instances[mesh_id]
		for cell_id: Variant in cells.keys():
			var payload: Array = cells[cell_id]
			var transforms: Array = payload[0]
			var colors: PackedColorArray = payload[1]
			var owners: PackedInt32Array = payload[3] if payload.size() > 3 and payload[3] is PackedInt32Array else PackedInt32Array()
			var behaviors: PackedInt32Array = payload[4] if payload.size() > 4 and payload[4] is PackedInt32Array else PackedInt32Array()
			for i: int in range(transforms.size()):
				if i < owners.size() and owners[i] != 0:
					continue
				var color: Color = colors[i] if i < colors.size() else Color.WHITE
				var behavior: int = behaviors[i] if i < behaviors.size() else 0
				manual.append("%s:%s:%s" % [str(mesh_id), str(cell_id),
					var_to_bytes([transforms[i], color, behavior]).hex_encode()])
	manual.sort()
	return var_to_bytes(manual)

func _same_pages(terrain: Terrain3D, snapshots: Dictionary) -> bool:
	for region: Vector2i in snapshots.keys():
		if terrain.data.get_region(region).get_instance_page() != snapshots[region]:
			return false
	return true

func _check(condition: bool, label: String) -> void:
	if condition:
		_passes += 1
	else:
		_failures.append(label)
		push_error("F08 FAIL: " + label)

func _report() -> void:
	print("Terrain3D F08: %d passed, %d failed" % [_passes, _failures.size()])
	for failure: String in _failures:
		print("  FAIL: " + failure)

func _foliage_editor(terrain: Terrain3D) -> Terrain3DFoliageEditor:
	var editor := Terrain3DFoliageEditor.new()
	editor.set_terrain(terrain)
	return editor
