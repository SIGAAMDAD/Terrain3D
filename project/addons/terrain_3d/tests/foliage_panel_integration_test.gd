@tool
extends EditorScript

# FG00 baseline. Run explicitly in the editor on a DISPOSABLE fixture only.
# The scene must contain two marked terrains with overlapping region coordinates,
# two adjacent regions each, two registered textures, one mesh and a preset.
# Existing F08 remains the generator/instance-format acceptance test.
const PROBE = preload("res://addons/terrain_3d/tests/foliage_panel_probe.gd")
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
		if not _fixture_valid(terrain):
			push_error("FG00 fixture invalid; see tests/F08_README.md")
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
	var preset: Terrain3DFoliagePreset = terrain.foliage_profile as Terrain3DFoliagePreset
	_captures.append({"label": label, "terrain": str(terrain.get_path()), "regions": records,
		"ledger": var_to_str(preset.manifest.entries), "next_owner_id": preset.manifest.next_owner_id,
		"raster_stats": terrain.data.get_sparse_raster_stats(), "call_cpu_us": elapsed_us,
		"journal_bytes": FileAccess.get_file_as_bytes(JOURNAL).size() if FileAccess.file_exists(JOURNAL) else 0,
		"total_io_bytes": null, "editor_frame_ms": null})

func _owner_only_delete() -> void:
	var terrain: Terrain3D = _terrains[0]
	var transforms: Array[Transform3D] = [Transform3D(Basis.IDENTITY, Vector3(4, 0, 4))]
	var insert: Dictionary = terrain.instancer.replace_foliage_owned_regions([
		{"region": Vector2i.ZERO, "owner": 1900000000, "mesh_id": 0, "transforms": transforms}], false)
	_check("fixture", "Native seed insertion succeeds", bool(insert.get("ok", false)))
	var before: int = Time.get_ticks_usec()
	var result: Dictionary = terrain.instancer.replace_foliage_owned_regions([
		{"region": Vector2i.ZERO, "owner": 1900000000, "mesh_id": -1, "transforms": []}], false)
	_check("FOL-01", "Owner-only deletion accepts missing mesh slot", bool(result.get("ok", false)))
	_capture("owner_only_delete", terrain, Time.get_ticks_usec() - before)

func _reconciliation() -> void:
	var panel = _panels[0]
	var definition: Terrain3DFoliageDefinition = panel._preset.definitions[0]
	var rule: Terrain3DFoliageMeshRule = definition.mesh_bank[0]
	var ledger: Array[Dictionary] = panel._preset.manifest.entries.duplicate(true)
	var config: Dictionary = panel._build_config(REGIONS, "regenerate")
	_check("fixture", "Panel compiles registered fixture rules", not config.is_empty() and not config.get("rules", []).is_empty())
	panel._abort()
	_check("FOL-08", "Cancel leaves owner ledger unchanged", ledger == panel._preset.manifest.entries)
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
	_check("FOL-03", "Preset swap preserves discoverable terrain owners", not panel._preset.manifest.entries.is_empty())
	panel._set_preset(old_preset)
	_capture("configuration_and_cancel", _terrains[0])

func _shared_preset() -> void:
	var shared: Terrain3DFoliagePreset = _panels[0]._preset
	_panels[1]._set_preset(shared)
	var before: Array[Dictionary] = shared.manifest.entries.duplicate(true)
	_panels[1]._build_config(REGIONS, "regenerate")
	# Terrain B must not inherit terrain A's bake ledger just by assigning its preset.
	_check("FOL-04", "Shared preset has isolated terrain ledgers", before.is_empty() or
		_panels[0]._preset.manifest != _panels[1]._preset.manifest)
	_capture("shared_preset_terrain_b", _terrains[1])

func _journal_and_failures() -> void:
	var panel = _panels[0]
	var paths := PackedStringArray(["user://terrain3d_fg00/nonexistent_snapshot.res"])
	var written: bool = panel._write_journal(REGIONS, paths)
	_check("fixture", "Recovery journal can be staged", written)
	if written:
		var file: FileAccess = FileAccess.open(JOURNAL, FileAccess.READ)
		var journal: Dictionary = file.get_var()
		file.close()
		_check("FOL-05", "Journal contains owner ledger snapshot", journal.has("before_ledger"))
		panel.fail_restore = true
		panel._recover_journal()
		_check("fixture", "Recovery failure was injected", panel.restore_attempted)
		_check("FOL-06", "Failed recovery retains journal", FileAccess.file_exists(JOURNAL))
		panel._clear_journal()
	var batch: Array = [{"region": Vector2i.ZERO, "owner": 1900000001, "mesh_id": 0, "transforms": []}]
	var before: PackedByteArray = _terrains[0].data.get_region(Vector2i.ZERO).get_instance_page()
	panel.commit_with_staging_failure(batch)
	_check("failure injection", "Snapshot save failure leaves instances intact",
		before == _terrains[0].data.get_region(Vector2i.ZERO).get_instance_page())
	_check("failure injection", "Staging error is visible", panel._status.text.contains("Unable to stage"))
	var serial: int = panel._snapshot_serial
	panel._snapshot_serial = _panels[1]._snapshot_serial
	_check("FOL-07", "Panel lifetimes use distinct undo filenames",
		panel.production_snapshot_path(0) != _panels[1].production_snapshot_path(0))
	panel._snapshot_serial = serial

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
	_check("FOL-09", "Changed source blocks publication", not panel.publication_requested)
	terrain.data.set_height(Vector3(4, 0, 4), height)
	panel._start("regenerate")
	panel.set_process(false)
	panel.popup()
	panel.hide()
	_check("FOL-22", "Hiding popup cancels operation or keeps it visible", not panel._active or panel.visible)
	panel._abort()
	_capture("after_panel_cases", terrain)
