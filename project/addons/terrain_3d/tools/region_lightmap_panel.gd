# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Batch editor UI for baking independent lightmap packages for every Terrain3D
# region authored in the configured data directory.
@tool
class_name Terrain3DRegionLightmapPanel
extends Window

var plugin: Terrain3DEditorPluginBase
var baker: Terrain3DRegionLightmapBaker
var terrain: Terrain3D

var _dataset_summary: Label
var _settings_summary: Label
var _workflow_status: Label
var _progress: ProgressBar
var _bake_all_button: Button
var _stop_button: Button
var _select_lightmap_button: Button
var _finalize_continue_button: Button
var _poll_timer: Timer

var _batch_locations: Array[Vector2i] = []
var _next_index: int = 0
var _completed_count: int = 0
var _failed_count: int = 0
var _is_running: bool = false
var _cancel_requested: bool = false
var _waiting_for_manual_bake: bool = false


func _ready() -> void:
	title = "Bake Terrain3D Region Lightmaps"
	initial_position = Window.WINDOW_INITIAL_POSITION_CENTER_MAIN_WINDOW_SCREEN
	min_size = Vector2i(650, 360)
	size = Vector2i(720, 420)
	unresizable = false
	transient = true
	exclusive = false
	close_requested.connect(_on_close_requested)
	_build_ui()

	_poll_timer = Timer.new()
	_poll_timer.wait_time = 0.35
	_poll_timer.one_shot = false
	_poll_timer.timeout.connect(_refresh_recovery_state)
	add_child(_poll_timer)
	# Window nodes default visible. This tool must only appear after the explicit
	# Terrain3D menu command is invoked.
	hide()


func open(p_terrain: Terrain3D) -> void:
	terrain = p_terrain
	_refresh_dataset_summary()
	_refresh_settings_summary()
	_refresh_recovery_state()
	popup()
	_poll_timer.start()


func _notification(p_what: int) -> void:
	if p_what == NOTIFICATION_VISIBILITY_CHANGED and is_instance_valid(_poll_timer):
		if visible:
			_poll_timer.start()
		else:
			_poll_timer.stop()


func _build_ui() -> void:
	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 16)
	margin.add_theme_constant_override("margin_right", 16)
	margin.add_theme_constant_override("margin_top", 14)
	margin.add_theme_constant_override("margin_bottom", 14)
	add_child(margin)
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)

	var content := VBoxContainer.new()
	content.add_theme_constant_override("separation", 10)
	margin.add_child(content)

	var heading := Label.new()
	heading.text = "Bake Region Lightmaps"
	heading.add_theme_font_size_override("font_size", 18)
	content.add_child(heading)

	var explanation := Label.new()
	explanation.text = "Bakes every authored Terrain3D region in the active data directory. Each region keeps its own streamable lighting package; neighboring regions are temporary lighting context only."
	explanation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	content.add_child(explanation)

	_dataset_summary = Label.new()
	_dataset_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_dataset_summary.custom_minimum_size = Vector2(0, 42)
	content.add_child(_dataset_summary)

	_settings_summary = Label.new()
	_settings_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	content.add_child(_settings_summary)

	var settings_row := HBoxContainer.new()
	settings_row.add_theme_constant_override("separation", 8)
	content.add_child(settings_row)

	var settings_button := Button.new()
	settings_button.text = "Edit Bake Settings in Inspector"
	settings_button.tooltip_text = "Select the Terrain3D node. Regional Lightmap Baking settings, including the persistent output directory, live under General / Data & Authoring in its Inspector."
	settings_button.pressed.connect(_select_terrain_for_settings)
	settings_row.add_child(settings_button)

	var refresh_button := Button.new()
	refresh_button.text = "Refresh Region List"
	refresh_button.tooltip_text = "Re-enumerate terrain3d*.res files from the configured data directory."
	refresh_button.pressed.connect(_refresh_all)
	settings_row.add_child(refresh_button)

	var audit_button := Button.new()
	audit_button.text = "Audit Packages"
	audit_button.tooltip_text = "Compare every authored region with its deterministic package, source signature, light signature, and shadowmask mode."
	audit_button.pressed.connect(_audit_packages)
	settings_row.add_child(audit_button)

	content.add_child(HSeparator.new())

	var action_row := HBoxContainer.new()
	action_row.add_theme_constant_override("separation", 8)
	content.add_child(action_row)

	_bake_all_button = Button.new()
	_bake_all_button.text = "Bake All Region Lightmaps"
	_bake_all_button.tooltip_text = "Sequentially bake and publish an independent lightmap package for every authored region in the Terrain3D data directory."
	_bake_all_button.pressed.connect(_start_batch)
	action_row.add_child(_bake_all_button)

	_stop_button = Button.new()
	_stop_button.text = "Stop After Current Region"
	_stop_button.tooltip_text = "Finish the region currently being baked, then stop before preparing another one."
	_stop_button.disabled = true
	_stop_button.pressed.connect(_request_stop)
	action_row.add_child(_stop_button)

	_progress = ProgressBar.new()
	_progress.min_value = 0.0
	_progress.max_value = 1.0
	_progress.value = 0.0
	_progress.show_percentage = false
	content.add_child(_progress)

	_workflow_status = Label.new()
	_workflow_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_workflow_status.custom_minimum_size = Vector2(0, 68)
	content.add_child(_workflow_status)

	var recovery_row := HBoxContainer.new()
	recovery_row.add_theme_constant_override("separation", 8)
	content.add_child(recovery_row)

	_select_lightmap_button = Button.new()
	_select_lightmap_button.text = "Select Prepared LightmapGI"
	_select_lightmap_button.tooltip_text = "Recovery control: select the current prepared region if Godot's automatic editor bake action is unavailable or a bake needs inspection."
	_select_lightmap_button.disabled = true
	_select_lightmap_button.pressed.connect(_select_prepared_lightmap)
	recovery_row.add_child(_select_lightmap_button)

	_finalize_continue_button = Button.new()
	_finalize_continue_button.text = "Finalize & Continue"
	_finalize_continue_button.tooltip_text = "Recovery control: publish a manually completed LightmapGI bake, then resume the remaining regions."
	_finalize_continue_button.disabled = true
	_finalize_continue_button.pressed.connect(_finalize_and_continue)
	recovery_row.add_child(_finalize_continue_button)


func _refresh_all() -> void:
	_sync_active_terrain()
	_refresh_dataset_summary()
	_refresh_settings_summary()
	_refresh_recovery_state()


func _refresh_dataset_summary() -> void:
	if not _is_terrain_ready() or not is_instance_valid(baker):
		_dataset_summary.text = "Select a Terrain3D node with a valid data directory and loaded authored data."
		_bake_all_button.disabled = true
		return
	var locations: Array[Vector2i] = baker.get_data_directory_regions(terrain)
	var baked_count: int = 0
	for location: Vector2i in locations:
		var region: Terrain3DRegion = terrain.data.get_region(location)
		if is_instance_valid(region) and is_instance_valid(region.get_lighting()):
			baked_count += 1
	var output_directory: String = String(terrain.get("region_lightmap_output_directory")).strip_edges()
	_dataset_summary.text = "%d authored regions found in %s. %d currently have regional lighting packages. Output: %s" % [
		locations.size(), terrain.data_directory, baked_count,
		output_directory if not output_directory.is_empty() else "not configured"]
	_bake_all_button.text = "Bake All %d Region Lightmaps" % locations.size() if not locations.is_empty() else "Bake All Region Lightmaps"
	_bake_all_button.disabled = locations.is_empty() or _is_running or _waiting_for_manual_bake


func _refresh_settings_summary() -> void:
	if not _is_terrain_ready():
		_settings_summary.text = ""
		return
	var quality_names: PackedStringArray = PackedStringArray(["Low", "Medium", "High", "Ultra"])
	var shadowmask_names: PackedStringArray = PackedStringArray(["None", "Replace", "Overlay"])
	var quality: int = clampi(int(terrain.get("region_lightmap_bake_quality")), 0, quality_names.size() - 1)
	var shadowmask: int = clampi(int(terrain.get("region_lightmap_shadowmask_mode")), 0, shadowmask_names.size() - 1)
	var output_directory: String = String(terrain.get("region_lightmap_output_directory")).strip_edges()
	_settings_summary.text = "Output: %s\nSettings: proxy LOD %d · context radius %d · %.2f m/texel · %s quality · %s shadowmask" % [
		output_directory if not output_directory.is_empty() else "not configured",
		int(terrain.get("region_lightmap_proxy_lod")),
		int(terrain.get("region_lightmap_context_radius")),
		float(terrain.get("region_lightmap_texel_size")),
		quality_names[quality], shadowmask_names[shadowmask]]


func _start_batch() -> void:
	_sync_active_terrain()
	if _is_running or _waiting_for_manual_bake or not _is_terrain_ready() or not is_instance_valid(baker):
		return
	var output_ready: Dictionary = baker.prepare_output_directory(terrain)
	if not bool(output_ready.get("ok", false)):
		_workflow_status.text = "Cannot start regional lightmap bake: %s" % String(output_ready.get("error", "Output path unavailable."))
		return
	_batch_locations = baker.get_data_directory_regions(terrain)
	if _batch_locations.is_empty():
		_workflow_status.text = "No authored terrain3d*.res regions were found in the configured data directory."
		return
	_next_index = 0
	_completed_count = 0
	_failed_count = 0
	_cancel_requested = false
	_waiting_for_manual_bake = false
	_progress.max_value = float(_batch_locations.size())
	_progress.value = 0.0
	_run_batch()


func _audit_packages() -> void:
	if not _is_terrain_ready() or not is_instance_valid(baker):
		return
	_workflow_status.text = "Auditing regional lightmap packages…"
	var report: Dictionary = baker.audit_region_packages(terrain)
	_workflow_status.text = ("Package audit: %d expected, %d current, %d missing, %d stale, " +
		"%d light-signature mismatches, %d shadowmask mismatches, %d orphans.") % [
		int(report.get("expected", 0)), int(report.get("current", 0)),
		(report.get("missing", []) as Array).size(),
		(report.get("stale_source_signatures", []) as Array).size(),
		(report.get("incompatible_light_signature", []) as Array).size(),
		(report.get("incompatible_shadowmask_mode", []) as Array).size(),
		(report.get("orphans", []) as Array).size()]


func _run_batch() -> void:
	_is_running = true
	_bake_all_button.disabled = true
	_stop_button.disabled = false
	_select_lightmap_button.disabled = true
	_finalize_continue_button.disabled = true

	while _next_index < _batch_locations.size():
		if _cancel_requested:
			_finish_batch("Stopped after %d of %d regions." % [_completed_count, _batch_locations.size()])
			return
		var location: Vector2i = _batch_locations[_next_index]
		_workflow_status.text = "Region %d/%d — preparing %s…" % [
			_next_index + 1, _batch_locations.size(), location]
		await get_tree().process_frame
		if not baker.prepare_region_bake(location):
			_failed_count += 1
			_next_index += 1
			_progress.value = float(_next_index)
			continue

		_workflow_status.text = "Region %d/%d — baking %s with Godot LightmapGI…" % [
			_next_index + 1, _batch_locations.size(), location]
		var bake_result: Dictionary = await baker.bake_prepared_with_editor()
		if not bool(bake_result.get("ok", false)):
			_pause_for_manual_bake(location, String(bake_result.get("error", "Lightmap bake failed.")))
			return

		_workflow_status.text = "Region %d/%d — publishing %s…" % [
			_next_index + 1, _batch_locations.size(), location]
		if not baker.finalize_prepared_bake():
			_pause_for_manual_bake(location, "The bake completed, but Terrain3D could not publish the regional package. The prepared LightmapGI was left in the scene for inspection.")
			return
		_completed_count += 1
		_next_index += 1
		_progress.value = float(_next_index)
		await get_tree().process_frame

	_finish_batch("Finished %d region lightmaps%s." % [
		_completed_count,
		" with %d preparation failures" % _failed_count if _failed_count > 0 else ""])


func _pause_for_manual_bake(location: Vector2i, reason: String) -> void:
	_is_running = false
	_waiting_for_manual_bake = true
	_stop_button.disabled = true
	_select_lightmap_button.disabled = false
	_finalize_continue_button.disabled = not baker.is_prepared_bake_complete()
	_workflow_status.text = "%s\nRegion %s remains prepared. If needed, use Godot's Bake Lightmaps command, then press Finalize & Continue." % [reason, location]


func _finalize_and_continue() -> void:
	if not _waiting_for_manual_bake or not is_instance_valid(baker):
		return
	if not baker.is_prepared_bake_complete():
		_workflow_status.text = "The prepared LightmapGI still has no baked textures. Run Godot's Bake Lightmaps command first."
		return
	if not baker.finalize_prepared_bake():
		_workflow_status.text = "Terrain3D could not publish the prepared regional lightmap package. The prepared bake remains available for inspection."
		return
	_completed_count += 1
	_next_index += 1
	_progress.value = float(_next_index)
	_waiting_for_manual_bake = false
	_cancel_requested = false
	_refresh_dataset_summary()
	_run_batch()


func _finish_batch(message: String) -> void:
	_is_running = false
	_waiting_for_manual_bake = false
	_stop_button.disabled = true
	_select_lightmap_button.disabled = true
	_finalize_continue_button.disabled = true
	_workflow_status.text = message
	_refresh_dataset_summary()


func _request_stop() -> void:
	_cancel_requested = true
	_stop_button.disabled = true
	_workflow_status.text = "Stop requested. The current region will finish before the batch stops."


func _select_prepared_lightmap() -> void:
	if is_instance_valid(baker):
		baker.select_prepared_lightmap()
	_refresh_recovery_state()


func _refresh_recovery_state() -> void:
	_sync_active_terrain()
	if not is_instance_valid(baker):
		return
	var prepared: Variant = baker.get_prepared_region()
	var has_prepared: bool = prepared is Vector2i
	_select_lightmap_button.disabled = not has_prepared or _is_running
	if _waiting_for_manual_bake:
		_finalize_continue_button.disabled = not baker.is_prepared_bake_complete()
	elif not _is_running:
		_finalize_continue_button.disabled = true


func _select_terrain_for_settings() -> void:
	_sync_active_terrain()
	if not _is_terrain_ready():
		return
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(terrain)
	_refresh_settings_summary()


func _on_close_requested() -> void:
	if _is_running:
		_cancel_requested = true
	hide()


func _sync_active_terrain() -> void:
	if is_instance_valid(plugin):
		var active_terrain: Variant = plugin.get("terrain")
		if active_terrain is Terrain3D and active_terrain != terrain:
			terrain = active_terrain


func _is_terrain_ready() -> bool:
	return is_instance_valid(terrain) and terrain.data != null and not terrain.data_directory.is_empty()
