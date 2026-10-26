@tool
extends RefCounted

# F00/T00: opt-in, owner-side reference capture. The engine's native history
# retains only 120 frames; call capture_tick() every physics frame to collect
# longer runs without increasing persistent native allocations.
const MAX_SAMPLES: int = 32768
# Snapshot the already-published renderer counters only while profiling.
# Counters and capacities have different units; the reporter keeps them separate.
const VT_FIELDS = [
	"vt_page_hits", "vt_page_misses", "vt_parent_fallback_hits",
	"vt_tail_fallback_hits", "vt_request_attempts", "vt_request_unique",
	"vt_request_hash_overflow", "vt_upload_bytes_frame", "vt_upload_bytes_total",
	"vt_upload_page_sets_frame", "vt_evictions_frame", "vt_evictions_total",
	"vt_thrash_count", "vt_physical_capacity", "vt_physical_resident",
	"vt_physical_retiring", "vt_io_jobs_pending",
	"vt_feedback_accepted_requests", "vt_prediction_incomplete_fallbacks",
	"vt_prediction_attempts",
]

var _terrain: Object
var _scenario: String
var _scene_id: String
var _settings: Dictionary
var _samples: Array[Dictionary] = []
var _vt_samples: Array[Dictionary] = []
var _last_vt_sequence: int = -1
var _last_sequence: int = -1
var _lost_samples: int = 0
var _active: bool = false

func begin(terrain: Object, scenario: String, scene_id: String, settings: Dictionary) -> Error:
	if terrain == null or not terrain.has_method("get_runtime_profile_history_since"):
		return ERR_INVALID_PARAMETER
	if scenario.is_empty() or scene_id.is_empty() or settings.is_empty():
		return ERR_INVALID_PARAMETER
	_terrain = terrain
	_scenario = scenario
	_scene_id = scene_id
	_settings = settings.duplicate(true)
	_samples.clear()
	_vt_samples.clear()
	_last_vt_sequence = -1
	_last_sequence = -1
	_lost_samples = 0
	_active = true
	_terrain.set("runtime_profiling_enabled", true)
	return OK

func capture_tick() -> void:
	if not _active or _terrain == null:
		return
	var frames: Array = _terrain.call("get_runtime_profile_history_since", _last_sequence, 120)
	for frame: Variant in frames:
		if not frame is Dictionary:
			continue
		var sample: Dictionary = frame
		var sequence: int = int(sample.get("sequence", -1))
		if sequence <= _last_sequence:
			continue
		if _last_sequence >= 0 and sequence > _last_sequence + 1:
			_lost_samples += sequence - _last_sequence - 1
		_last_sequence = sequence
		if _samples.size() >= MAX_SAMPLES:
			_lost_samples += 1
			continue
		_samples.append(sample)
	if _last_sequence > _last_vt_sequence:
		# This is an observational snapshot, taken after the native tick;
		# it does not claim exact GPU-frame synchronization.
		var diagnostics: Dictionary = _terrain.call("get_runtime_diagnostics", false)
		var renderer: Dictionary = diagnostics.get("streaming_renderer", {})
		var point: Dictionary = {"sequence": _last_sequence}
		for key in VT_FIELDS:
			if renderer.has(key):
				point[key] = renderer[key]
		if _vt_samples.size() < MAX_SAMPLES:
			_vt_samples.append(point)
		_last_vt_sequence = _last_sequence

func finish(output_path: String) -> Error:
	if not _active or _terrain == null or output_path.is_empty():
		return ERR_INVALID_PARAMETER
	capture_tick()
	_active = false
	var diagnostics: Dictionary = _terrain.call("get_runtime_diagnostics", false)
	var capture: Dictionary = {
		"schema": "t3d_streaming_baseline_v1",
		"scenario": _scenario,
		"scene_id": _scene_id,
		"settings": _settings,
		"lost_samples": _lost_samples,
		"samples": _samples,
		"vt_samples": _vt_samples,
		"aggregate": diagnostics.get("aggregate", {}),
	}
	var file: FileAccess = FileAccess.open(output_path, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_string(JSON.stringify(capture, "\t"))
	file.close()
	return OK

# For short (<=120 frame) captures, no per-frame polling is needed.
static func save_capture(terrain: Object, output_path: String, scenario: String, scene_id: String, settings: Dictionary) -> Error:
	var recorder: RefCounted = load("res://addons/terrain_3d/tools/streaming_baseline_capture.gd").new()
	var error: Error = recorder.call("begin", terrain, scenario, scene_id, settings)
	if error != OK:
		return error
	return recorder.call("finish", output_path)
