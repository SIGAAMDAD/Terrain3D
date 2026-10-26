extends SceneTree
## Run with Godot --headless --path <project> -s res://addons/terrain_3d/validation/vt_streaming_acceptance.gd -- --scene res://world.tscn --frames 600 --output user://vt_acceptance.json
## Captures CPU frame intervals and the render-thread's published (non-blocking) streaming statistics.

var failures: PackedStringArray = []


func _initialize() -> void:
	call_deferred("_run")


func _arg_value(arguments: PackedStringArray, name: String, default_value: String) -> String:
	var index: int = arguments.find(name)
	if index < 0 or index + 1 >= arguments.size():
		return default_value
	return arguments[index + 1]


func _find_first(node: Node, class_name_value: String) -> Node:
	if node.is_class(class_name_value):
		return node
	for child: Node in node.get_children():
		var result: Node = _find_first(child, class_name_value)
		if result != null:
			return result
	return null


func _percentile(sorted_samples: Array[float], percentile: float) -> float:
	if sorted_samples.is_empty():
		return 0.0
	var index: int = clampi(ceili(float(sorted_samples.size()) * percentile) - 1, 0, sorted_samples.size() - 1)
	return sorted_samples[index]


func _require(condition: bool, description: String) -> void:
	if not condition:
		failures.append(description)


func _run() -> void:
	var args: PackedStringArray = OS.get_cmdline_user_args()
	var scene_path: String = _arg_value(args, "--scene", "")
	var output_path: String = _arg_value(args, "--output", "user://vt_streaming_acceptance.json")
	var frames: int = maxi(120, int(_arg_value(args, "--frames", "600")))
	var allow_fallback: bool = args.has("--allow-fallback")
	if scene_path.is_empty():
		push_error("Missing --scene res://path/to/world.tscn")
		quit(2)
		return
	var packed: PackedScene = load(scene_path) as PackedScene
	if packed == null:
		push_error("Could not load scene: " + scene_path)
		quit(2)
		return
	var scene: Node = packed.instantiate()
	root.add_child(scene)
	var terrain: Node = _find_first(scene, "Terrain3D")
	if terrain == null or not terrain.has_method("get_runtime_diagnostics"):
		push_error("Scene does not contain a Terrain3D with get_runtime_diagnostics")
		quit(2)
		return
	var camera: Camera3D = _find_first(scene, "Camera3D") as Camera3D
	var origin: Vector3 = camera.global_position if camera != null else Vector3.ZERO
	var intervals_ms: Array[float] = []
	var samples: Array[Dictionary] = []
	var previous_usec: int = Time.get_ticks_usec()
	var last_snapshot: Dictionary = {}
	for frame: int in range(frames):
		# Includes directional movement and a teleport. The same sequence can be
		# replayed for predictor-on/off or budget-on/off comparisons.
		if camera != null and frame % 30 == 0:
			var position: Vector3 = origin + Vector3(float(int(frame / 30) % 8) * 64.0, 0.0, float(int(frame / 90) % 3) * 128.0)
			if frame >= frames / 2:
				position += Vector3(512.0, 0.0, -512.0)
			camera.global_position = position
		await process_frame
		var now_usec: int = Time.get_ticks_usec()
		intervals_ms.append(float(now_usec - previous_usec) / 1000.0)
		previous_usec = now_usec
		if frame % 30 == 0 or frame == frames - 1:
			last_snapshot = terrain.call("get_runtime_diagnostics", false) as Dictionary
			var render: Dictionary = last_snapshot.get("streaming_renderer", {}) as Dictionary
			var admission: Dictionary = render.get("gpu_stream_transfer_budget", {}) as Dictionary
			var resident: Dictionary = last_snapshot.get("authoring_texture_residency", {}) as Dictionary
			samples.append({
				"frame": frame,
				"material_view": resident.get("runtime_metadata_only", false),
				"material_count": resident.get("logical_material_count", 0),
				"authoring_references_released": resident.get("released_authoring_references", 0),
				"admission": admission,
				"demand_status": render.get("vt_demand_index_status", ""),
				"demand_path": render.get("vt_demand_index_path", ""),
				"force_syncs": render.get("sync_count_by_reason", {}),
				"package_file_backed": render.get("vt_package_file_backed", null),
				"package_metadata_bytes": render.get("vt_package_metadata_bytes", null),
				"package_file_bytes": render.get("vt_package_file_bytes", null),
				"prediction_incomplete_fallbacks": render.get("vt_prediction_incomplete_fallbacks", 0),
				"prediction_current_cell_complete": render.get("vt_prediction_current_cell_complete", false),
				"feedback_mask_uploads_saved": render.get("vt_feedback_mask_uploads_saved", 0)
			})
	var final_startup: Dictionary = last_snapshot.get("startup", {}) as Dictionary
	var final_residency: Dictionary = last_snapshot.get("authoring_texture_residency", {}) as Dictionary
	var final_render: Dictionary = last_snapshot.get("streaming_renderer", {}) as Dictionary
	_require(bool(final_startup.get("succeeded_last", false)), "Startup did not commit")
	_require(final_render.get("diagnostics_mode", "") == "published_no_sync", "Renderer diagnostics are not on the nonblocking published path")
	var configuration: Dictionary = last_snapshot.get("configuration", {}) as Dictionary
	var cooked: bool = bool(configuration.get("gpu_cooked_pages_enabled", false))
	var requested: bool = bool(final_residency.get("release_requested", false))
	if cooked:
		_require(bool(final_render.get("vt_package_file_backed", false)), "Cooked runtime VT is not file-backed")
		var package_size: int = int(final_render.get("vt_package_file_bytes", 0))
		var metadata_size: int = int(final_render.get("vt_package_metadata_bytes", 0))
		_require(package_size > 0 and metadata_size > 0 and metadata_size < package_size,
			"VT metadata residency is not smaller than its source package")
		if not allow_fallback:
			_require(final_render.get("vt_demand_index_status", "") == "staged",
				"VT predictor sidecar is missing, invalid, or mismatched")
	if cooked and requested:
		_require(bool(final_residency.get("runtime_metadata_only", false)), "Cooked runtime failed to publish compact material view")
		_require(int(final_residency.get("logical_material_count", -1)) >= 0, "Material registry unavailable")
	var intervals_sorted: Array[float] = intervals_ms.duplicate()
	intervals_sorted.sort()
	var report: Dictionary = {
		"scene": scene_path,
		"frames": frames,
		"note": "CPU frame intervals, not isolated GPU timestamp measurements; replay equal workloads for comparisons",
		"cpu_frame_ms_p50": _percentile(intervals_sorted, 0.50),
		"cpu_frame_ms_p95": _percentile(intervals_sorted, 0.95),
		"cpu_frame_ms_p99": _percentile(intervals_sorted, 0.99),
		"cpu_frame_ms_max": _percentile(intervals_sorted, 1.0),
		"samples": samples,
		"failures": failures,
		"passed": failures.is_empty()
	}
	var output: FileAccess = FileAccess.open(output_path, FileAccess.WRITE)
	if output == null:
		push_error("Cannot write output to " + output_path)
		quit(2)
		return
	output.store_string(JSON.stringify(report, "\t"))
	output.close()
	print("VT acceptance: ", "PASS" if failures.is_empty() else "FAIL", "; output: ", output_path)
	quit(0 if failures.is_empty() else 1)
