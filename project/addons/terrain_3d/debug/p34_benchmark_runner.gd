extends SceneTree

const WORKLOAD_MANIFEST := "res://addons/terrain_3d/debug/p34_workloads.json"
const FIXED_DT := 1.0 / 60.0
const SAMPLE_STRIDE := 10

var _workload_id := "stationary_warm"
var _output_path := ""
var _scene_path := ""
var _workload: Dictionary = {}
var _terrain: Terrain3D
var _driver: Node3D
var _origin := Vector3.ZERO
var _edit_restore: Array[Dictionary] = []

func _initialize() -> void:
	_parse_args()
	call_deferred("_run")

func _parse_args() -> void:
	var args := OS.get_cmdline_user_args()
	var i := 0
	while i < args.size():
		match args[i]:
			"--workload":
				i += 1
				if i < args.size(): _workload_id = args[i]
			"--output":
				i += 1
				if i < args.size(): _output_path = args[i]
			"--scene":
				i += 1
				if i < args.size(): _scene_path = args[i]
		i += 1

func _load_manifest() -> Dictionary:
	var file := FileAccess.open(WORKLOAD_MANIFEST, FileAccess.READ)
	if file == null:
		return {}
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	return parsed if parsed is Dictionary else {}

func _select_workload(manifest: Dictionary) -> bool:
	for candidate in manifest.get("workloads", []):
		if candidate is Dictionary and candidate.get("id", "") == _workload_id:
			_workload = candidate
			return true
	return false

func _find_terrain(node: Node) -> Terrain3D:
	if node == null:
		return null
	if node is Terrain3D:
		return node as Terrain3D
	for child in node.get_children():
		var found := _find_terrain(child)
		if found != null:
			return found
	return null

func _find_driver(scene: Node) -> Node3D:
	var player := scene.find_child("Player", true, false)
	if player is Node3D:
		return player as Node3D
	var camera := scene.find_child("Camera3D", true, false)
	if camera is Node3D:
		return camera as Node3D
	return null

func _move_driver(frame: int) -> void:
	if _driver == null:
		return
	var movement := String(_workload.get("movement", "stationary"))
	var speed := float(_workload.get("speed_mps", 0.0))
	var t := float(frame) * FIXED_DT
	match movement:
		"linear":
			_driver.global_position = _origin + Vector3(speed * t, 0.0, 0.0)
		"boundary_pingpong":
			var boundary_x := round(_origin.x / 128.0) * 128.0
			_driver.global_position.x = boundary_x + sin(t * speed / 8.0) * 24.0
		"orbit":
			var radius := 80.0
			_driver.global_position = _origin + Vector3(cos(t * speed / radius) * radius, 0.0, sin(t * speed / radius) * radius)
		"hlod_sweep":
			_driver.global_position = _origin + Vector3(sin(t * 0.15) * 900.0, 0.0, cos(t * 0.15) * 900.0)
		"zigzag":
			_driver.global_position = _origin + Vector3(speed * t, 0.0, sin(t * 5.0) * 96.0)
		"loop":
			var radius := 512.0
			_driver.global_position = _origin + Vector3(cos(t * speed / radius) * radius, 0.0, sin(t * speed / radius) * radius)
		_:
			pass

func _apply_edit_probe(frame: int) -> void:
	var interval := int(_workload.get("edit_interval_frames", 0))
	if interval <= 0 or frame % interval != 0 or _terrain == null:
		return
	var data: Terrain3DData = _terrain.get_data()
	if data == null:
		return
	var p := _origin + Vector3(float((frame / interval) % 8) * 2.0, 0.0, 0.0)
	var original := data.get_height(p)
	_edit_restore.append({"position": p, "height": original})
	data.set_height(p, original + 0.02)

func _restore_edits() -> void:
	if _terrain == null:
		return
	var data: Terrain3DData = _terrain.get_data()
	if data == null:
		return
	for item in _edit_restore:
		data.set_height(item["position"], item["height"])
	_edit_restore.clear()

func _num(source: Dictionary, key: String, fallback := 0) -> Variant:
	return source.get(key, fallback)

func _capture(frame_index: int) -> Dictionary:
	var runtime: Dictionary = _terrain.get_runtime_diagnostics(false)
	var rd: Dictionary = _terrain.get_rd_renderer_diagnostics()
	var frame: Dictionary = runtime.get("frame", {})
	var memory: Dictionary = runtime.get("memory", {})
	var inst: Dictionary = rd.get("instances", {})
	var world: Dictionary = rd.get("world_renderer", {})
	var renderer: Dictionary = rd.get("renderer", {})
	var shaders: Dictionary = runtime.get("shaders", {})
	return {
		"frame": frame_index,
		"physics_total_usec": _num(frame, "physics_total_usec"),
		"data_residency_usec": _num(frame, "data_residency_usec"),
		"node_streaming_usec": _num(frame, "node_streaming_usec"),
		"lightmap_streaming_usec": _num(frame, "lightmap_streaming_usec"),
		"clipmap_usec": _num(frame, "clipmap_usec"),
		"instancer_usec": _num(frame, "instancer_usec"),
		"collision_usec": _num(frame, "collision_usec"),
		"streaming_usec": _num(frame, "streaming_usec"),
		"vt_request_attempts": _num(rd, "vt_request_attempts"),
		"vt_request_unique": _num(rd, "vt_request_unique"),
		"vt_prediction_attempts": _num(rd, "vt_prediction_attempts"),
		"vt_fragment_faults": _num(rd, "vt_fragment_faults"),
		"vt_feedback_accepted_requests": _num(rd, "vt_feedback_accepted_requests"),
		"vt_upload_bytes_total": _num(rd, "vt_upload_bytes_total"),
		"vt_upload_bytes_frame": _num(rd, "vt_upload_bytes_frame"),
		"vt_evictions_total": _num(rd, "vt_evictions_total"),
		"vt_thrash_count": _num(rd, "vt_thrash_count"),
		"vt_physical_capacity": _num(rd, "vt_physical_capacity"),
		"vt_physical_resident": _num(rd, "vt_physical_resident"),
		"vt_io_jobs_pending": _num(rd, "vt_io_jobs_pending"),
		"virtual_texture_stream_active_jobs": _num(rd, "virtual_texture_stream_active_jobs"),
		"vt_descriptor_rebuilds_runtime": _num(rd, "vt_descriptor_rebuilds_runtime"),
		"vt_gpu_resource_creates_after_seal": _num(rd, "vt_gpu_resource_creates_after_seal"),
		"resource_creates": _num(renderer, "resource_creates"),
		"shader_creates": _num(renderer, "shader_creates"),
		"pipeline_creates": _num(renderer, "pipeline_creates"),
		"pipeline_rebuilds": _num(renderer, "pipeline_rebuilds"),
		"uniform_set_creates": _num(renderer, "uniform_set_creates"),
		"uniform_set_rebuilds": _num(renderer, "uniform_set_rebuilds"),
		"bytes_uploaded_frame": _num(renderer, "bytes_uploaded_frame"),
		"buffer_updates_frame": _num(renderer, "buffer_updates_frame"),
		"shader_physical_ready": _num(shaders, "physical_shader_ready"),
		"shader_physical_expected": _num(shaders, "physical_shader_expected"),
		"shader_misses_after_ready": _num(shaders, "physical_shader_misses_after_ready"),
		"instance_dirty_upload_bytes": _num(inst, "instance_dirty_upload_bytes"),
		"instance_dirty_upload_ranges": _num(inst, "instance_dirty_upload_ranges"),
		"owner_buffer_rebuilds": _num(inst, "owner_buffer_rebuilds"),
		"mesh_owner_arena_rebuilds": _num(inst, "mesh_owner_arena_rebuilds"),
		"instance_one_submit_main_commands": _num(inst, "instance_one_submit_main_commands"),
		"instance_one_submit_shadow_commands": _num(inst, "instance_one_submit_shadow_commands"),
		"instance_submissions_opaque": _num(inst, "instance_submissions_opaque"),
		"instance_submissions_shadow": _num(inst, "instance_submissions_shadow"),
		"visible_instances": _num(inst, "visible_instances"),
		"hlod_gpu_cull_descriptors": _num(inst, "hlod_gpu_cull_descriptors"),
		"world_hiz_unavailable_frames": _num(world, "hiz_unavailable_frames"),
		"memory_resident_bytes": _num(memory, "resident_bytes", _num(memory, "physical_bytes", 0)),
		"memory_high_water_bytes": _num(memory, "high_water_bytes", _num(memory, "peak_bytes", 0)),
	}

func _write_report(report: Dictionary) -> bool:
	var text := JSON.stringify(report, "  ")
	if _output_path.is_empty():
		print("T3D_P34_BENCHMARK_REPORT=" + text)
		return true
	var file := FileAccess.open(_output_path, FileAccess.WRITE)
	if file == null:
		push_error("Cannot open benchmark output: " + _output_path)
		return false
	file.store_string(text + "\n")
	return true

func _run() -> void:
	var manifest := _load_manifest()
	if manifest.is_empty() or not _select_workload(manifest):
		push_error("Unknown/missing P34 workload: " + _workload_id)
		quit(2)
		return
	if _scene_path.is_empty():
		_scene_path = String(manifest.get("scene", ""))
	if _scene_path.is_empty():
		push_error("P34 benchmark requires --scene <res://...> or a manifest scene path")
		quit(3)
		return
	var packed := load(_scene_path) as PackedScene
	if packed == null:
		push_error("Cannot load P34 benchmark scene: " + _scene_path)
		quit(3)
		return
	var scene := packed.instantiate()
	root.add_child(scene)
	current_scene = scene
	await process_frame
	await process_frame
	_terrain = _find_terrain(scene)
	_driver = _find_driver(scene)
	if _terrain == null:
		push_error("P34 benchmark scene contains no Terrain3D")
		quit(4)
		return
	_terrain.set_runtime_profiling_enabled(true)
	_terrain.reset_runtime_diagnostics()
	_origin = _driver.global_position if _driver != null else Vector3.ZERO
	var warmup_frames := int(_workload.get("warmup_frames", 0))
	var sample_frames := int(_workload.get("sample_frames", 600))
	var samples: Array[Dictionary] = []
	for frame_index in range(warmup_frames + sample_frames):
		_move_driver(frame_index)
		if _workload_id == "edit_megavt_recook":
			_apply_edit_probe(frame_index)
		await process_frame
		if frame_index >= warmup_frames and ((frame_index - warmup_frames) % SAMPLE_STRIDE == 0 or frame_index + 1 == warmup_frames + sample_frames):
			samples.append(_capture(frame_index - warmup_frames))
	if bool(_workload.get("restore_edits", false)):
		_restore_edits()
	var report := {
		"schema_version": 1,
		"workload": _workload_id,
		"scene": _scene_path,
		"rendering_driver": RenderingServer.get_current_rendering_driver_name(),
		"warmup_frames": warmup_frames,
		"sample_frames": sample_frames,
		"sample_stride": SAMPLE_STRIDE,
		"fixture": _workload.get("fixture", ""),
		"samples": samples,
	}
	var ok := _write_report(report)
	scene.queue_free()
	await process_frame
	quit(0 if ok else 5)
