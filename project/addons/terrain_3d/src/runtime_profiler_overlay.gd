# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends CanvasLayer

@export var terrain_path: NodePath
@export_range(1.0, 20.0, 1.0) var refresh_hz: float = 4.0
@export var maximum_problem_lines: int = 4
@export var show_pipeline_timings: bool = true

var terrain: Terrain3D
var _panel: PanelContainer
var _label: RichTextLabel
var _refresh_accumulator: float = 0.0
var _restore_profiling: bool = false


func _ready() -> void:
	layer = 120
	_build_ui()
	if not terrain_path.is_empty():
		set_terrain(get_node_or_null(terrain_path) as Terrain3D)
	set_process(true)
	_refresh()


func _exit_tree() -> void:
	_restore_profiling_state()


func _process(p_delta: float) -> void:
	if not visible:
		return
	_refresh_accumulator += p_delta
	if _refresh_accumulator >= 1.0 / maxf(refresh_hz, 1.0):
		_refresh_accumulator = 0.0
		_refresh()


func set_terrain(p_terrain: Terrain3D) -> void:
	_restore_profiling_state()
	terrain = p_terrain
	if is_instance_valid(terrain) and not terrain.is_runtime_profiling_enabled():
		terrain.set_runtime_profiling_enabled(true)
		_restore_profiling = true
	_refresh()


func _restore_profiling_state() -> void:
	if _restore_profiling and is_instance_valid(terrain):
		terrain.set_runtime_profiling_enabled(false)
	_restore_profiling = false


func _build_ui() -> void:
	_panel = PanelContainer.new()
	_panel.position = Vector2(12.0, 12.0)
	_panel.custom_minimum_size = Vector2(470.0, 0.0)
	add_child(_panel)
	_label = RichTextLabel.new()
	_label.bbcode_enabled = true
	_label.fit_content = true
	_label.scroll_active = false
	_label.custom_minimum_size = Vector2(450.0, 0.0)
	_panel.add_child(_label)


func _refresh() -> void:
	if not is_instance_valid(_label):
		return
	if not is_instance_valid(terrain):
		_label.text = "[b]Terrain3D diagnostics[/b]\nNo terrain assigned"
		return
	var snapshot: Terrain3DRuntimeValueMap = Terrain3DRuntimeValueMap.from_value(terrain.get_runtime_diagnostics())
	var frame: Terrain3DRuntimeValueMap = snapshot.map_value("frame")
	var gpu: Terrain3DRuntimeValueMap = snapshot.map_value("gpu")
	var cpu: Terrain3DRuntimeValueMap = snapshot.map_value("cpu")
	var instances: Terrain3DRuntimeValueMap = snapshot.map_value("instance_cache")
	var instancer: Terrain3DRuntimeValueMap = snapshot.map_value("instancer")
	var nodes: Terrain3DRuntimeValueMap = snapshot.map_value("nodes")
	var streaming: Terrain3DRuntimeValueMap = snapshot.map_value("region_streaming")
	var lines: PackedStringArray = PackedStringArray()
	lines.append("[b]Terrain3D frame %d[/b]  %.3f ms" % [frame.int_value("sequence"), frame.float_value("physics_total_usec") / 1000.0])
	lines.append("Residency %.3f ms | Clipmap %.3f ms | Instances %.3f ms | Nodes %.3f ms | Collision %.3f ms" % [
		frame.float_value("data_residency_usec") / 1000.0,
		frame.float_value("clipmap_usec") / 1000.0,
		frame.float_value("instancer_usec") / 1000.0,
		frame.float_value("node_streaming_usec") / 1000.0,
		frame.float_value("collision_usec") / 1000.0,
	])
	lines.append("Region %s | movement %s" % [str(frame.vector2i_value("target_region")), str(frame.vector2_value("movement"))])
	lines.append("Tiers A:%d H:%d C:%d U:%d | predicted %d | center %s → %s" % [
		streaming.int_value("active"), streaming.int_value("hot"), streaming.int_value("cold"),
		streaming.int_value("unloaded"), streaming.int_value("predicted_promotions"),
		str(streaming.vector2i_value("center")), str(streaming.vector2i_value("predicted_center")),
	])
	lines.append("GPU %d/%d | missing %d | uploaded %d tick / %d since reset (%s) | prepared %d + pending %d%s" % [
		gpu.int_value("resident"), gpu.int_value("capacity"), gpu.int_value("missing_required_last_tick"),
		gpu.int_value("uploads_last_tick"), gpu.int_value("uploads_since_reset"),
		_format_bytes(gpu.float_value("upload_bytes_last_tick")),
		gpu.int_value("prepare_ready_pages"), gpu.int_value("prepare_job_pending_pages"),
		"" if gpu.bool_value("enabled") else " | cache disabled",
	])
	lines.append("CPU %d/%d | loads %d tick / %d since reset | Instance pages %d/%d loads %d | cell backlog %d%s" % [
		cpu.int_value("resident"), cpu.int_value("capacity"), cpu.int_value("loads_last_tick"),
		cpu.int_value("loads_since_reset"),
		instances.int_value("resident"), instances.int_value("capacity"), instances.int_value("loads_last_tick"),
		instancer.int_value("pending_updates_total"),
		"" if cpu.bool_value("enabled") else " | CPU cache disabled",
	])
	lines.append("Nodes %d in %d regions | +%d -%d failures %d | far dirty H:%d C:%d" % [
		nodes.int_value("active_nodes"), nodes.int_value("active_regions"), nodes.int_value("loads_last_tick"),
		nodes.int_value("unloads_last_tick"), nodes.int_value("load_failures_last_tick"),
		gpu.int_value("far_height_dirty_regions"), gpu.int_value("far_color_dirty_regions"),
	])
	var stalls: PackedStringArray = frame.string_array("stall_reasons")
	if not stalls.is_empty():
		lines.append("[color=orange]Stopped by: %s[/color]" % ", ".join(stalls))
	if show_pipeline_timings:
		lines.append("Page pipeline queue %.3f | read %.3f | validate %.3f | materialize %.3f | RenderingServer publication CPU %.3f | E2E %.3f ms" % [
			gpu.float_value("prepare_queue_wait_usec_last") / 1000.0,
			gpu.float_value("prepare_file_read_usec_last") / 1000.0,
			gpu.float_value("prepare_validation_usec_last") / 1000.0,
			gpu.float_value("prepare_materialize_usec_last") / 1000.0,
			gpu.float_value("prepare_publication_usec_last") / 1000.0,
			gpu.float_value("prepare_end_to_end_usec_last") / 1000.0,
		])
	var problems: Array[Terrain3DRuntimeValueMap] = snapshot.map_array("problems")
	for index: int in range(mini(problems.size(), maxi(maximum_problem_lines, 0))):
		var problem: Terrain3DRuntimeValueMap = problems[index]
		lines.append("[color=%s]%s: %s (%s)[/color]" % [
			_problem_color(problem.string_value("severity", "info")),
			problem.string_value("severity", "info").to_upper(),
			problem.string_value("message", problem.string_value("code", "problem")),
			str(problem.int_value("value")),
		])
	_label.text = "\n".join(lines)


func _problem_color(p_severity: String) -> String:
	match p_severity:
		"error":
			return "red"
		"warning":
			return "orange"
		_:
			return "light_blue"


func _format_bytes(p_bytes: float) -> String:
	if p_bytes >= 1048576.0:
		return "%.2f MiB" % (p_bytes / 1048576.0)
	if p_bytes >= 1024.0:
		return "%.1f KiB" % (p_bytes / 1024.0)
	return "%d B" % int(p_bytes)
