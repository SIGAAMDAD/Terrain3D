@tool
extends EditorScript

# FG00 baseline. Run explicitly in the editor on a DISPOSABLE fixture only.
# The scene must contain two marked terrains with overlapping region coordinates,
# two adjacent regions each, two registered textures, one mesh and a preset.
# Existing F08 remains the generator/instance-format acceptance test.
const PROBE = preload("res://addons/terrain_3d/tests/foliage_panel_probe.gd")
const TRANSACTION = preload("res://addons/terrain_3d/tools/foliage_transaction.gd")
const REPORT_PATH: String = "user://terrain3d_fg00_baseline.json"
const JOURNAL: String = "user://terrain3d_foliage_pending.dat"
const REGIONS: Array[Vector2i] = [Vector2i(-1, 0), Vector2i(0, 0)]
var _results: Array[Dictionary] = []
var _captures: Array[Dictionary] = []
var _terrains: Array[Terrain3D] = []
var _originals: Array[Dictionary] = []
var _panels: Array = []

func _run() -> void:
	_find_terrains(EditorInterface.get_edited_scene_root())
	if _terrains.size() != 2 or FileAccess.file_exists(JOURNAL):
		push_error("FG00 needs exactly two marked disposable terrains and no pending foliage journal")
		return
	for terrain: Terrain3D in _terrains:
		if not _fixture_valid(terrain) or FileAccess.file_exists(TRANSACTION.journal_path(terrain)):
			push_error("FG00 fixture invalid; see tests/tests_tests_README.md")
			return
	# Save complete pages and resource references before any test mutation.
	for terrain: Terrain3D in _terrains:
		var pages: Dictionary = {}
		for region: Vector2i in REGIONS:
			pages[region] = terrain.data.get_region(region).get_instance_page()
		_originals.append({"pages": pages, "preset": terrain.foliage_profile})
		terrain.foliage_profile = terrain.foliage_profile.duplicate(true)
		var panel = PROBE.new()
		EditorInterface.get_base_control().add_child(panel)
		panel._scope.select(1)
		panel.set_terrain(terrain)
		panel.popup()
		_panels.append(panel)
	_capture("before", _terrains[0])
	_owner_only_delete()
	_reconciliation()
	_shared_preset()
	_journal_and_failures()
	_signatures_and_visibility()
	for index: int in range(_terrains.size()):
		var terrain: Terrain3D = _terrains[index]
		for region: Vector2i in REGIONS:
			terrain.data.get_region(region).set_instance_page(_originals[index]["pages"][region])
		terrain.foliage_profile = _originals[index]["preset"]
		terrain.instancer.update_mmis()
	for panel in _panels:
		panel._abort()
		panel.queue_free()
	var report: Dictionary = {"schema": 1, "stage": "FG00", "engine": Engine.get_version_info(),
		"results": _results, "captures": _captures,
		"limitations": ["Frame samples measure synchronous calls, not full editor frame latency",
			"I/O bytes are journal bytes only; total native file I/O is not exposed"]}
	var file: FileAccess = FileAccess.open(REPORT_PATH, FileAccess.WRITE)
	if file == null:
		push_error("FG00 cannot write baseline report")
		return
	file.store_string(JSON.stringify(report, "\t"))
	file.close()
	print("FG00 baseline: ", REPORT_PATH, " (FAIL records are known regressions, not accepted behavior)")

func _find_terrains(node: Node) -> void:
	if node == null:
		return
	if node is Terrain3D and bool(node.get_meta("terrain3d_foliage_fg00_fixture", false)):
		_terrains.append(node as Terrain3D)
	for child: Node in node.get_children():
		_find_terrains(child)

func _fixture_valid(terrain: Terrain3D) -> bool:
	if terrain.data == null or terrain.assets == null or terrain.assets.get_texture_count() < 2:
		return false
	if terrain.assets.get_mesh_count() < 1 or not terrain.data_directory.begins_with("user://terrain3d_fg00/"):
		return false
	var preset: Terrain3DFoliagePreset = terrain.foliage_profile as Terrain3DFoliagePreset
	if preset == null or preset.definitions.size() != 1 or preset.definitions[0] == null:
		return false
	if preset.definitions[0].mesh_bank.size() != 1 or preset.definitions[0].mesh_bank[0] == null:
		return false
	for region: Vector2i in REGIONS:
		if not terrain.data.has_region(region):
			return false
	return true

func _check(id: String, label: String, condition: bool) -> void:
	_results.append({"finding": id, "label": label, "status": "PASS" if condition else "FAIL"})
	print("FG00 ", _results.back())

func _capture(label: String, terrain: Terrain3D, elapsed_us: int = 0) -> void:
	var records: Array[Dictionary] = []
	for region: Vector2i in REGIONS:
		var item: Terrain3DRegion = terrain.data.get_region(region)
		var owners: Dictionary = {}
		var count: int = 0
		for cells: Dictionary in item.get_instances().values():
			for payload: Array in cells.values():
				count += (payload[0] as Array).size()
				if payload.size() > 3:
					for owner: int in payload[3]:
						owners[str(owner)] = int(owners.get(str(owner), 0)) + 1
		records.append({"region": str(region), "owners": owners, "instances": count,
			"page_sha256": var_to_bytes(item.get_instance_page()).sha256_text()})
	var ledger: Terrain3DFoliageManifest
	for panel in _panels:
		if panel._terrain == terrain:
			ledger = panel._manifest
	_captures.append({"label": label, "terrain": str(terrain.get_path()), "regions": records,
		"ledger": var_to_str(ledger.entries), "next_owner_id": ledger.next_owner_id,
		"raster_stats": terrain.data.get_sparse_raster_stats(), "call_cpu_us": elapsed_us,
		"journal_bytes": FileAccess.get_file_as_bytes(TRANSACTION.journal_path(terrain)).size() if FileAccess.file_exists(TRANSACTION.journal_path(terrain)) else 0,
		"total_io_bytes": null, "editor_frame_ms": null})

func _owner_only_delete() -> void:
	var terrain: Terrain3D = _terrains[0]
	var transforms: Array[Transform3D] = [Transform3D(Basis.IDENTITY, Vector3(4, 0, 4))]
	var insert: Dictionary = _foliage_editor(terrain).replace_owned_regions([
		{"region": Vector2i.ZERO, "owner": 1900000000, "mesh_id": 0, "transforms": transforms}], false)
	_check("fixture", "Native seed insertion succeeds", bool(insert.get("ok", false)))
	var before: int = Time.get_ticks_usec()
	var result: Dictionary = _foliage_editor(terrain).replace_owned_regions([
		{"region": Vector2i.ZERO, "owner": 1900000000, "mesh_id": -1, "transforms": []}], false)
	_check("FOL-01", "Owner-only deletion accepts missing mesh slot", bool(result.get("ok", false)))
	_capture("owner_only_delete", terrain, Time.get_ticks_usec() - before)

func _reconciliation() -> void:
	var panel = _panels[0]
	var definition: Terrain3DFoliageDefinition = panel._preset.definitions[0]
	var rule: Terrain3DFoliageMeshRule = definition.mesh_bank[0]
	var ledger: Array[Dictionary] = panel._manifest.entries.duplicate(true)
	var next_owner: int = panel._manifest.next_owner_id
	var config: Dictionary = panel._build_config(REGIONS, "regenerate")
	_check("fixture", "Panel compiles registered fixture rules", not config.is_empty() and not config.get("rules", []).is_empty())
	panel._abort()
	_check("FOL-08", "Cancel leaves owner ledger unchanged", ledger == panel._manifest.entries
		and next_owner == panel._manifest.next_owner_id)
	# Seed a published owner/ledger pair for retirement tests. This setup is test
	# only; it does not save regions, mutate the preset ledger or register undo.
	config = panel._build_config(REGIONS, "regenerate")
	var seeds: Array[Dictionary] = []
	for compiled: Dictionary in config.get("rules", []):
		var region: Vector2i = compiled["region"]
		var transforms: Array[Transform3D] = [Transform3D(Basis.IDENTITY,
			Vector3(region.x * int(_terrains[0].region_size) + 4, 0, region.y * int(_terrains[0].region_size) + 4))]
		seeds.append({"region": region, "owner": compiled["owner"],
			"mesh_id": compiled["mesh_id"], "transforms": transforms})
		panel._pending_manifest.mark_baked(String(compiled["key"]), "fg00-seed", panel._preset.random_seed)
	if not seeds.is_empty():
		var seeded: Dictionary = _foliage_editor(_terrains[0]).replace_owned_regions(seeds, false)
		_check("fixture", "Retirement fixture seeds owned instances", bool(seeded.get("ok", false)))
		panel._manifest = panel._pending_manifest
	panel._discard_pending()
	rule.enabled = false
	panel._build_config(REGIONS, "regenerate")
	_check("FOL-02", "Disabled owners remain in reconciliation records", not panel._pending_records.is_empty())
	rule.enabled = true
	definition.mesh_bank.erase(rule)
	panel._build_config(REGIONS, "regenerate")
	_check("FOL-02", "Deleted owners remain in reconciliation records", not panel._pending_records.is_empty())
	definition.mesh_bank.append(rule)
	var mesh: Terrain3DMeshAsset = rule.mesh_asset
	rule.mesh_asset = null
	panel._build_config(REGIONS, "regenerate")
	_check("FOL-02", "Missing mesh does not prevent owner cleanup", not panel._pending_records.is_empty())
	rule.mesh_asset = mesh
	var old_preset: Terrain3DFoliagePreset = panel._preset
	var replacement := Terrain3DFoliagePreset.new()
	replacement.stable_uid = "fg00-replacement"
	panel._set_preset(replacement)
	_check("FOL-03", "Preset swap preserves discoverable terrain owners", not panel._manifest.entries.is_empty())
	panel._set_preset(old_preset)
	_capture("configuration_and_cancel", _terrains[0])

func _shared_preset() -> void:
	var shared: Terrain3DFoliagePreset = _panels[0]._preset
	_panels[1]._set_preset(shared)
	var before: Array[Dictionary] = _panels[0]._manifest.entries.duplicate(true)
	_panels[1]._build_config(REGIONS, "regenerate")
	# Terrain B must not inherit terrain A's bake ledger just by assigning its preset.
	_check("FOL-04", "Shared preset has isolated terrain ledgers",
		_panels[0]._manifest != _panels[1]._manifest and before == _panels[0]._manifest.entries)
	_capture("shared_preset_terrain_b", _terrains[1])

func _journal_and_failures() -> void:
	var panel = _panels[0]
	var terrain: Terrain3D = _terrains[0]
	var pages: Dictionary = {}
	for region: Vector2i in REGIONS:
		pages[region] = terrain.data.get_region(region).get_instance_page()
	var transaction = TRANSACTION.new()
	transaction.failure_phase = "PREPARED"
	transaction.fail_restore = true
	var result: Dictionary = transaction.execute_pages(terrain, pages, pages, panel._manifest,
		panel._manifest.duplicate(true), {})
	var journal: String = TRANSACTION.journal_path(terrain)
	_check("fixture", "Prepared failure leaves a recoverable journal", not bool(result.get("ok", true)) and FileAccess.file_exists(journal))
	_check("FOL-05", "Journal contains both owner ledger snapshots",
		transaction.record.has("ledger_before") and transaction.record.has("ledger_after"))
	var failed: Dictionary = transaction.recover(terrain)
	_check("FOL-06", "Failed recovery retains journal", not bool(failed.get("ok", true)) and FileAccess.file_exists(journal))
	transaction.fail_restore = false
	var recovered: Dictionary = transaction.recover(terrain)
	_check("fixture", "Recovery completes when failure is removed", bool(recovered.get("ok", false)))
	_check("failure injection", "Prepared failure preserves live page bytes",
		pages[Vector2i.ZERO] == terrain.data.get_region(Vector2i.ZERO).get_instance_page())
	var second = TRANSACTION.new()
	second.failure_phase = "PREPARED"
	second.execute_pages(terrain, pages, pages, panel._manifest, panel._manifest.duplicate(true), {})
	_check("FOL-07", "Undo backing files have distinct transaction namespaces",
		transaction.record.get("directory", "") != second.record.get("directory", ""))
	second.recover(terrain)

func _signatures_and_visibility() -> void:
	var panel = _panels[0]
	var terrain: Terrain3D = _terrains[0]
	var source: String = panel._source_signature(Vector2i.ZERO)
	var first: Terrain3DTextureAsset = terrain.assets.get_texture_asset(0)
	var second: Terrain3DTextureAsset = terrain.assets.get_texture_asset(1)
	terrain.assets.set_texture_asset(0, second)
	terrain.assets.set_texture_asset(1, first)
	_check("FOL-10", "Material slot reorder changes placement signature",
		source != panel._source_signature(Vector2i.ZERO))
	terrain.assets.set_texture_asset(0, first)
	terrain.assets.set_texture_asset(1, second)
	var rule: Terrain3DFoliageMeshRule = panel._preset.definitions[0].mesh_bank[0]
	var mesh: Terrain3DMeshAsset = rule.mesh_asset
	rule.mesh_asset = null
	panel._start("regenerate")
	_check("FOL-19", "Missing mesh is shown as an error, not an empty job",
		not panel._status.text.contains("Nothing to generate"))
	rule.mesh_asset = mesh
	panel.publication_requested = false
	panel.use_real_commit = true
	panel._start("regenerate")
	_check("fixture", "Lifecycle job starts", panel._active)
	panel.set_process(false) # Drive ticks explicitly; no hidden automatic publication.
	var height: float = terrain.data.get_height(Vector3(4, 0, 4))
	terrain.data.set_height(Vector3(4, 0, 4), height + 1.0)
	var ticks: int = 0
	while panel._active and ticks < 25000:
		panel._process(0.0)
		ticks += 1
	_check("fixture", "Source mutation job completed within guard", not panel._active)
	_check("FOL-09", "Changed source blocks publication", not panel.publication_requested and panel._status.text.contains("changed"))
	panel.use_real_commit = false
	terrain.data.set_height(Vector3(4, 0, 4), height)
	panel._start("regenerate")
	panel.set_process(false)
	panel.popup()
	panel.hide()
	_check("FOL-22", "Hiding popup cancels operation or keeps it visible", not panel._active or panel.visible)
	panel._abort()
	_capture("after_panel_cases", terrain)

func _foliage_editor(terrain: Terrain3D) -> Terrain3DFoliageEditor:
	var editor := Terrain3DFoliageEditor.new()
	editor.set_terrain(terrain)
	return editor
