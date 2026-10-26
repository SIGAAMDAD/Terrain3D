# TOOL-04 GDScript source/key and availability regression fixture.
# Run with: godot --headless --path project --script res://addons/terrain_3d/validation/tool04_snapshot_smoke.gd
extends SceneTree

const StatusSnapshot = preload("res://addons/terrain_3d/diagnostics/runtime_status_snapshot.gd")


func _initialize() -> void:
	var empty: Terrain3DRuntimeValueMap = Terrain3DRuntimeValueMap.new()
	_assert_row(StatusSnapshot.authoring_rows(empty), "VT feedback valid samples", "unavailable", "unavailable")
	_assert_row(StatusSnapshot.cache_rows(empty), "V1 package generation", "unavailable", "unavailable")
	var missing_capabilities: Array[Dictionary] = StatusSnapshot.capability_rows(empty)
	assert(not missing_capabilities.is_empty())
	assert(str(missing_capabilities[0].get("compiled")) == "unavailable")
	var snapshot: Terrain3DRuntimeValueMap = Terrain3DRuntimeValueMap.from_value({
		"frame": {"sequence": 42},
		"shaders": {
			"catalog_count_terrain": 8, "spirv_ready_terrain": 8,
			"physical_ready_terrain": 7, "shader_compile_ready": 9,
			"shader_compile_expected": 10, "physical_shader_ready": 7,
			"physical_shader_expected": 8, "physical_shader_failed": 1,
		},
		"streaming_renderer": {
			"snapshot_generation_lag": 0,
			"world_renderer": {"visibility_candidates": 0, "visibility_failures": 0},
			"vt_feedback_grid_valid_samples": 0,
			"vt_pack_version": 1,
			"virtual_texture_package_generation": 13,
			"vt_physical_resident": 3,
			"preview_vt_authoring_package_bytes": 0,
			"vt_editor_cook_stage": 4,
			"preview_vt_authoring_last_cook_error": "",
		},
	})
	_assert_row(StatusSnapshot.authoring_rows(snapshot), "VT feedback valid samples", "0", "published")
	_assert_row(StatusSnapshot.authoring_rows(snapshot), "World visibility candidates", "0", "published")
	_assert_row(StatusSnapshot.authoring_rows(snapshot), "Preview V1 cook phase", "encode", "published")
	_assert_row(StatusSnapshot.authoring_rows(snapshot), "Preview V1 last cook error", "none", "published")
	_assert_row(StatusSnapshot.cache_rows(snapshot), "V1 preview package bytes (in memory)", "0 B", "published")
	_assert_row(StatusSnapshot.cache_rows(snapshot), "V1 package generation", "13", "published")
	_assert_row(StatusSnapshot.cache_rows(snapshot), "Regional FAR stored bytes", "unavailable", "not published by FAR cooker")
	var capabilities: Array[Dictionary] = StatusSnapshot.capability_rows(snapshot)
	assert(str(capabilities[0].get("compiled")) == "8 / 8")
	assert(str(capabilities[0].get("available")) == "7 / 8")
	var stale: Terrain3DRuntimeValueMap = Terrain3DRuntimeValueMap.from_value({
		"frame": {"sequence": 43},
		"streaming_renderer": {"snapshot_generation_lag": 2, "vt_physical_resident": 0},
	})
	_assert_row(StatusSnapshot.cache_rows(stale), "VT physical resident pages", "0", "snapshot lag 2")
	print("TOOL-04 snapshot smoke PASS")
	quit(0)


func _assert_row(p_rows: Array[Dictionary], p_name: String, p_value: String, p_state: String) -> void:
	for row: Dictionary in p_rows:
		if str(row.get("name")) == p_name:
			assert(str(row.get("value")) == p_value)
			assert(str(row.get("status")).begins_with(p_state))
			return
	assert(false, "Missing TOOL-04 row: " + p_name)
