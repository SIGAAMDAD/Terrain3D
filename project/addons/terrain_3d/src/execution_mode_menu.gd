# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Compact editor execution-policy and live profiler control.
@tool
extends Terrain3DExecutionModeMenuBase

const RUNTIME_PROFILER_WINDOW_PATH: String = "res://addons/terrain_3d/src/runtime_profiler_window.gd"
const MEMORY_TRACKER_WINDOW_PATH: String = "res://addons/terrain_3d/src/memory_tracker_window.gd"

const MENU_AUTHORING: int = 0
const MENU_RUNTIME_PROFILE: int = 1
const MENU_NODE_STREAM_PREVIEW: int = 2
const MENU_OPEN_PROFILER: int = 10
const MENU_RESET_PROFILER: int = 11
const MENU_EXPORT_SNAPSHOT: int = 12
const MENU_MEMORY_TRACKER: int = 13

var terrain: Terrain3D
var _profiler_window: Terrain3DRuntimeProfilerWindowBase
var _memory_tracker_window: Window
var _export_dialog: FileDialog
var _menu_initialized: bool = false


func _enter_tree() -> void:
	name = "Terrain3DExecutionMode"
	text = "Authoring"
	tooltip_text = "Terrain3D editor execution policy and live diagnostics"
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	custom_minimum_size = Vector2(126.0, 0.0)

	var popup: PopupMenu = get_popup()
	popup.clear()
	popup.add_radio_check_item("Authoring — Full Residency", MENU_AUTHORING)
	popup.add_radio_check_item("Node Streaming Preview", MENU_NODE_STREAM_PREVIEW)
	popup.add_radio_check_item("Runtime Profile", MENU_RUNTIME_PROFILE)
	popup.add_separator()
	popup.add_item("Open Live Profiler...", MENU_OPEN_PROFILER)
	popup.add_item("Memory Tracker & Budgets...", MENU_MEMORY_TRACKER)
	popup.add_item("Reset Live Counters", MENU_RESET_PROFILER)
	popup.add_item("Export Live Snapshot...", MENU_EXPORT_SNAPSHOT)
	if not popup.id_pressed.is_connected(_on_menu_pressed):
		popup.id_pressed.connect(_on_menu_pressed)
	if not about_to_popup.is_connected(_sync_state):
		about_to_popup.connect(_sync_state)
	_menu_initialized = true
	_sync_state()


func _exit_tree() -> void:
	set_terrain(null)
	_menu_initialized = false


func _get_or_create_profiler_window() -> Terrain3DRuntimeProfilerWindowBase:
	if is_instance_valid(_profiler_window):
		return _profiler_window
	var profiler_script: GDScript = load(RUNTIME_PROFILER_WINDOW_PATH) as GDScript
	if profiler_script == null:
		push_error("Unable to load Terrain3D runtime profiler window")
		return null
	_profiler_window = profiler_script.new() as Terrain3DRuntimeProfilerWindowBase
	if not is_instance_valid(_profiler_window):
		push_error("Unable to instantiate Terrain3D runtime profiler window")
		return null
	add_child(_profiler_window)
	_profiler_window.set_terrain(terrain)
	return _profiler_window


func _get_or_create_memory_tracker() -> Window:
	if is_instance_valid(_memory_tracker_window):
		return _memory_tracker_window
	var script: GDScript = load(MEMORY_TRACKER_WINDOW_PATH) as GDScript
	if script == null:
		push_error("Unable to load Terrain3D memory tracker")
		return null
	_memory_tracker_window = script.new() as Window
	add_child(_memory_tracker_window)
	_memory_tracker_window.call("set_terrain", terrain)
	return _memory_tracker_window


func _get_or_create_export_dialog() -> FileDialog:
	if is_instance_valid(_export_dialog):
		return _export_dialog
	_export_dialog = FileDialog.new()
	_export_dialog.title = "Export Terrain3D Live Profiler Snapshot"
	_export_dialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE
	_export_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_export_dialog.filters = PackedStringArray(["*.json ; JSON snapshot"])
	_export_dialog.current_file = "terrain3d-live-profile.json"
	_export_dialog.file_selected.connect(_write_snapshot)
	add_child(_export_dialog)
	return _export_dialog


func set_terrain(p_terrain: Terrain3D) -> void:
	if is_instance_valid(terrain) and terrain.editor_execution_mode_changed.is_connected(_on_execution_mode_changed):
		terrain.editor_execution_mode_changed.disconnect(_on_execution_mode_changed)
	terrain = p_terrain
	if is_instance_valid(terrain):
		terrain.editor_execution_mode_changed.connect(_on_execution_mode_changed)
	if is_instance_valid(_profiler_window):
		_profiler_window.set_terrain(terrain)
	if is_instance_valid(_memory_tracker_window):
		_memory_tracker_window.call("set_terrain", terrain)
	_sync_state()


func _sync_state() -> void:
	disabled = not is_instance_valid(terrain)
	if disabled:
		text = "Execution"
		tooltip_text = "Select a Terrain3D node to choose an execution policy or inspect live diagnostics"
		return

	var mode: int = terrain.get_editor_execution_mode()
	var node_preview: bool = terrain.is_editor_node_streaming_preview_active()
	if mode == Terrain3D.EDITOR_EXECUTION_RUNTIME_PROFILE:
		text = "Runtime Profile"
		tooltip_text = "Exact shipping ACTIVE/HOT/COLD/UNLOADED residency policy with runtime budgets and prediction"
	elif node_preview:
		text = "Node Preview"
		tooltip_text = "Keep terrain authoring residency while streaming real node scenes around the editor camera using dynamic_node_stream_radius and node budgets"
	else:
		text = "Authoring"
		tooltip_text = "Full Residency: pin the complete authored terrain, instances, streamed nodes, collision, and authoring-relevant lighting"
	if not _menu_initialized:
		return

	var popup: PopupMenu = get_popup()
	popup.set_item_checked(popup.get_item_index(MENU_AUTHORING), mode == Terrain3D.EDITOR_EXECUTION_AUTHORING and not node_preview)
	popup.set_item_checked(popup.get_item_index(MENU_NODE_STREAM_PREVIEW), mode == Terrain3D.EDITOR_EXECUTION_AUTHORING and node_preview)
	popup.set_item_checked(popup.get_item_index(MENU_RUNTIME_PROFILE), mode == Terrain3D.EDITOR_EXECUTION_RUNTIME_PROFILE)


func _on_menu_pressed(p_id: int) -> void:
	if not is_instance_valid(terrain):
		return
	match p_id:
		MENU_AUTHORING:
			terrain.set_editor_node_streaming_preview(false)
			terrain.set_editor_execution_mode(int(Terrain3D.EDITOR_EXECUTION_AUTHORING))
		MENU_NODE_STREAM_PREVIEW:
			terrain.set_editor_execution_mode(int(Terrain3D.EDITOR_EXECUTION_AUTHORING))
			terrain.set_editor_node_streaming_preview(true)
		MENU_RUNTIME_PROFILE:
			terrain.set_editor_node_streaming_preview(false)
			terrain.set_editor_execution_mode(int(Terrain3D.EDITOR_EXECUTION_RUNTIME_PROFILE))
		MENU_OPEN_PROFILER:
			var profiler_window: Terrain3DRuntimeProfilerWindowBase = _get_or_create_profiler_window()
			if is_instance_valid(profiler_window):
				profiler_window.open_for_terrain(terrain)
		MENU_MEMORY_TRACKER:
			var tracker: Window = _get_or_create_memory_tracker()
			if is_instance_valid(tracker):
				tracker.call("open_for_terrain", terrain)
		MENU_RESET_PROFILER:
			terrain.reset_runtime_diagnostics()
			if is_instance_valid(_profiler_window) and _profiler_window.visible:
				_profiler_window.refresh_now()
		MENU_EXPORT_SNAPSHOT:
			var export_dialog: FileDialog = _get_or_create_export_dialog()
			if is_instance_valid(export_dialog):
				export_dialog.popup_centered_ratio(0.55)
	_sync_state()


func _write_snapshot(p_path: String) -> void:
	if not is_instance_valid(terrain):
		return
	var snapshot: Dictionary[String, Variant] = {
		"terrain3d": terrain.get_runtime_diagnostics(),
	}
	var file: FileAccess = FileAccess.open(p_path, FileAccess.WRITE)
	if not file:
		push_error("Unable to write Terrain3D live profiler snapshot: %s" % p_path)
		return
	file.store_string(JSON.stringify(snapshot, "\t"))
	file.close()


func _on_execution_mode_changed(_p_mode: int) -> void:
	_sync_state()
