# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
class_name Terrain3DHLODPanel
extends Window

var plugin: Terrain3DEditorPluginBase
var baker: Terrain3DHLODBaker
var terrain: Terrain3D

var _summary: Label
var _runtime_status: Label
var _cluster_list: ItemList
var _preview: CheckBox
var _show_bounds: CheckBox
var _force_level: OptionButton
var _conversion_min_level: SpinBox
var _poll_timer: Timer
var _debug_mesh: MeshInstance3D


func _ready() -> void:
	title = "Terrain3D HLOD"
	initial_position = Window.WINDOW_INITIAL_POSITION_CENTER_MAIN_WINDOW_SCREEN
	min_size = Vector2i(760, 560)
	size = Vector2i(860, 650)
	transient = true
	exclusive = false
	close_requested.connect(hide)
	hide()
	_build_ui()
	_poll_timer = Timer.new()
	_poll_timer.wait_time = 0.5
	_poll_timer.timeout.connect(_refresh_runtime)
	add_child(_poll_timer)


func open(p_terrain: Terrain3D) -> void:
	terrain = p_terrain
	_sync_controls_from_terrain()
	_analyze()
	_refresh_runtime()
	popup()
	_poll_timer.start()


func _notification(what: int) -> void:
	if what == NOTIFICATION_VISIBILITY_CHANGED and is_instance_valid(_poll_timer):
		if visible:
			_poll_timer.start()
		else:
			_poll_timer.stop()
			_clear_debug_mesh()


func _build_ui() -> void:
	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 14)
	margin.add_theme_constant_override("margin_right", 14)
	margin.add_theme_constant_override("margin_top", 12)
	margin.add_theme_constant_override("margin_bottom", 12)
	add_child(margin)
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)

	var content := VBoxContainer.new()
	content.add_theme_constant_override("separation", 10)
	margin.add_child(content)

	var heading := Label.new()
	heading.text = "Hierarchical LOD"
	heading.add_theme_font_size_override("font_size", 18)
	content.add_child(heading)

	var help := Label.new()
	help.text = "HLOD is derived static rendering data. Analyze and bake explicitly; normal scene saves never rebuild HLOD. Active proxies replace only sources recorded in the baked manifest."
	help.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	content.add_child(help)

	var conversion_row := HBoxContainer.new()
	conversion_row.add_theme_constant_override("separation", 8)
	content.add_child(conversion_row)
	var convert_button := Button.new()
	convert_button.text = "Convert Selected GLB Chunks to HLOD Visual Streaming"
	convert_button.tooltip_text = "Validate selected Node3D/GLB roots as static render-only scenes, then move them into Terrain3D streamed-node descriptors that participate in HLOD."
	convert_button.pressed.connect(_convert_selected_hlod_visual)
	conversion_row.add_child(convert_button)
	var min_label := Label.new()
	min_label.text = "Minimum HLOD"
	conversion_row.add_child(min_label)
	_conversion_min_level = SpinBox.new()
	_conversion_min_level.min_value = 1
	_conversion_min_level.max_value = 6
	_conversion_min_level.step = 1
	_conversion_min_level.value = 1
	_conversion_min_level.tooltip_text = "The first hierarchy level allowed to replace the selected detailed GLB chunks."
	conversion_row.add_child(_conversion_min_level)

	var authoring_row := HBoxContainer.new()
	authoring_row.add_theme_constant_override("separation", 8)
	content.add_child(authoring_row)

	var analyze_button := Button.new()
	analyze_button.text = "Analyze Sources"
	analyze_button.pressed.connect(_analyze)
	authoring_row.add_child(analyze_button)

	var validate_button := Button.new()
	validate_button.text = "Validate HLOD"
	validate_button.tooltip_text = "Validate the published manifest and every referenced content-addressed HLOD package without rebuilding anything."
	validate_button.pressed.connect(_validate_generated)
	authoring_row.add_child(validate_button)

	var dirty_button := Button.new()
	dirty_button.text = "Bake Dirty"
	dirty_button.tooltip_text = "Reuse unchanged content-addressed packages and rebuild changed hierarchy branches."
	dirty_button.pressed.connect(_bake_dirty)
	authoring_row.add_child(dirty_button)

	var rebuild_button := Button.new()
	rebuild_button.text = "Rebuild All"
	rebuild_button.pressed.connect(_rebuild_all)
	authoring_row.add_child(rebuild_button)

	var clear_button := Button.new()
	clear_button.text = "Clear Generated HLOD"
	clear_button.pressed.connect(_clear_generated)
	authoring_row.add_child(clear_button)

	_summary = Label.new()
	_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_summary.custom_minimum_size = Vector2(0, 78)
	content.add_child(_summary)

	content.add_child(HSeparator.new())

	var preview_row := HBoxContainer.new()
	preview_row.add_theme_constant_override("separation", 12)
	content.add_child(preview_row)

	_preview = CheckBox.new()
	_preview.text = "Preview HLOD in Editor"
	_preview.tooltip_text = "Use the active 3D viewport camera to run HLOD selection while Terrain3D remains in Authoring mode."
	_preview.toggled.connect(_on_preview_toggled)
	preview_row.add_child(_preview)

	_show_bounds = CheckBox.new()
	_show_bounds.text = "Show Cluster Bounds"
	_show_bounds.toggled.connect(_on_show_bounds_toggled)
	preview_row.add_child(_show_bounds)

	var force_label := Label.new()
	force_label.text = "Debug level"
	preview_row.add_child(force_label)

	_force_level = OptionButton.new()
	_force_level.add_item("Auto", -1)
	for level: int in range(1, 7):
		_force_level.add_item("HLOD %d" % level, level)
	_force_level.item_selected.connect(_on_force_level_selected)
	preview_row.add_child(_force_level)

	_runtime_status = Label.new()
	_runtime_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_runtime_status.custom_minimum_size = Vector2(0, 60)
	content.add_child(_runtime_status)

	var cluster_heading := Label.new()
	cluster_heading.text = "Runtime hierarchy"
	cluster_heading.add_theme_font_size_override("font_size", 15)
	content.add_child(cluster_heading)

	_cluster_list = ItemList.new()
	_cluster_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_cluster_list.allow_reselect = true
	content.add_child(_cluster_list)


func _convert_selected_hlod_visual() -> void:
	_sync_active_terrain()
	if not is_instance_valid(plugin) or not is_instance_valid(terrain):
		return
	plugin.convert_selected_to_hlod_visual(int(_conversion_min_level.value))
	_analyze.call_deferred()


func _analyze() -> void:
	_sync_active_terrain()
	if not _ready_for_authoring():
		_summary.text = "Select a Terrain3D node with loaded data and assets."
		return
	var result: Dictionary = baker.analyze(terrain)
	var per_level: PackedInt32Array = result.get("per_min_level", PackedInt32Array())
	var levels: PackedStringArray = PackedStringArray()
	for level: int in range(1, per_level.size()):
		if per_level[level] > 0:
			levels.append("L%d:%d" % [level, per_level[level]])
	_summary.text = "Eligible sources: %d (%d instancer, %d streamed GLB)  •  source triangles: %d  •  minimum levels: %s" % [
		int(result.get("sources", 0)), int(result.get("instancer_sources", 0)), int(result.get("node_sources", 0)),
		int(result.get("source_triangles", 0)), ", ".join(levels) if not levels.is_empty() else "none"]
	var generated: Dictionary = baker.validate_generated(terrain)
	if bool(generated.get("missing_manifest", false)):
		_summary.text += "\nGenerated hierarchy: not baked yet."
	else:
		_summary.text += "\nGenerated hierarchy: %d valid, %d missing, %d invalid." % [
			int(generated.get("valid", 0)), int(generated.get("missing", 0)), int(generated.get("invalid", 0))]
	_append_messages(result)


func _validate_generated() -> void:
	_sync_active_terrain()
	if not _ready_for_authoring():
		return
	var result: Dictionary = baker.validate_generated(terrain)
	if bool(result.get("missing_manifest", false)):
		_summary.text = "HLOD validation: no generated manifest exists yet."
		return
	_summary.text = "HLOD validation: %d valid, %d missing, %d invalid package entries.\nManifest: %s" % [
		int(result.get("valid", 0)), int(result.get("missing", 0)), int(result.get("invalid", 0)),
		String(result.get("manifest_path", ""))]
	var messages: PackedStringArray = result.get("messages", PackedStringArray())
	if not messages.is_empty():
		_summary.text += "\n" + " | ".join(messages)


func _bake_dirty() -> void:
	_bake(true)


func _rebuild_all() -> void:
	_bake(false)


func _bake(dirty_only: bool) -> void:
	_sync_active_terrain()
	if not _ready_for_authoring():
		return
	_summary.text = "HLOD bake in progress…"
	await get_tree().process_frame
	var result: Dictionary = baker.bake(terrain, dirty_only)
	if not bool(result.get("ok", false)):
		_summary.text = "HLOD bake failed: %s" % String(result.get("error", "unknown error"))
		_append_messages(result)
		return
	_summary.text = "HLOD bake complete: %d sources, %d clusters, %d built, %d reused.\nManifest: %s" % [
		int(result.get("sources", 0)), int(result.get("clusters", 0)), int(result.get("built", 0)),
		int(result.get("reused", 0)), String(result.get("manifest_path", ""))]
	_append_messages(result)
	_refresh_runtime()


func _clear_generated() -> void:
	_sync_active_terrain()
	if not _ready_for_authoring():
		return
	var result: Dictionary = baker.clear_generated(terrain)
	if bool(result.get("ok", false)):
		_summary.text = "Generated HLOD packages and manifest cleared. Detailed sources remain unchanged."
	else:
		_summary.text = "Unable to clear generated HLOD: %s" % String(result.get("error", "unknown error"))
	_refresh_runtime()


func _append_messages(result: Dictionary) -> void:
	var warnings: PackedStringArray = result.get("warnings", PackedStringArray())
	var errors: PackedStringArray = result.get("errors", PackedStringArray())
	if not errors.is_empty():
		_summary.text += "\nErrors: " + " | ".join(errors)
	if not warnings.is_empty():
		_summary.text += "\nWarnings: " + " | ".join(warnings)


func _refresh_runtime() -> void:
	_sync_active_terrain()
	_cluster_list.clear()
	if not is_instance_valid(terrain):
		_runtime_status.text = "No active Terrain3D node."
		_clear_debug_mesh()
		return
	_preview.set_pressed_no_signal(terrain.is_editor_hlod_preview_enabled())
	var stats: Dictionary = terrain.get_hlod_streaming_stats()
	var mib: float = float(int(stats.get("resident_estimated_bytes", 0))) / (1024.0 * 1024.0)
	_runtime_status.text = "Manifest entries: %d  •  active: %d  •  desired: %d  •  retiring: %d  •  failed: %d  •  managed/detail-required regions: %d/%d  •  proxy memory: %.1f MiB / %d MiB" % [
		int(stats.get("entries", 0)), int(stats.get("active", 0)), int(stats.get("desired", 0)),
		int(stats.get("retiring", 0)), int(stats.get("failed", 0)), int(stats.get("managed_regions", 0)),
		int(stats.get("detail_required_regions", 0)), mib, int(stats.get("memory_budget_mb", 0))]
	var entries: Array = terrain.get_hlod_debug_entries()
	for value: Variant in entries:
		if not value is Dictionary:
			continue
		var entry: Dictionary = value
		var state: String = "idle"
		if bool(entry.get("load_failed", false)):
			state = "FAILED"
		elif bool(entry.get("retiring", false)):
			state = "retiring"
		elif bool(entry.get("active", false)):
			state = "ACTIVE"
		elif bool(entry.get("request_started", false)):
			state = "loading"
		elif bool(entry.get("desired", false)):
			state = "desired"
		_cluster_list.add_item("H%d  %s  %s  [%s → %s]" % [int(entry.get("level", 0)), entry.get("cluster", Vector2i.ZERO), state, entry.get("region_min", Vector2i.ZERO), entry.get("region_max", Vector2i.ZERO)])
	if _show_bounds.button_pressed:
		_rebuild_debug_mesh(entries)
	else:
		_clear_debug_mesh()


func _on_preview_toggled(enabled: bool) -> void:
	_sync_active_terrain()
	if is_instance_valid(terrain):
		terrain.set_editor_hlod_preview(enabled)
		terrain.reload_hlod_manifest()
	_refresh_runtime()


func _on_show_bounds_toggled(enabled: bool) -> void:
	if not enabled:
		_clear_debug_mesh()
	_refresh_runtime()


func _on_force_level_selected(index: int) -> void:
	_sync_active_terrain()
	if not is_instance_valid(terrain):
		return
	terrain.hlod_forced_debug_level = _force_level.get_item_id(index)
	_refresh_runtime()


func _sync_controls_from_terrain() -> void:
	if not is_instance_valid(terrain):
		return
	_preview.set_pressed_no_signal(terrain.is_editor_hlod_preview_enabled())
	var forced: int = terrain.hlod_forced_debug_level
	for index: int in range(_force_level.item_count):
		if _force_level.get_item_id(index) == forced:
			_force_level.select(index)
			break


func _sync_active_terrain() -> void:
	if is_instance_valid(plugin) and is_instance_valid(plugin.terrain):
		terrain = plugin.terrain


func _ready_for_authoring() -> bool:
	return is_instance_valid(baker) and is_instance_valid(terrain) and is_instance_valid(terrain.data) and is_instance_valid(terrain.assets)


func _rebuild_debug_mesh(entries: Array) -> void:
	if not is_instance_valid(terrain):
		return
	_clear_debug_mesh()
	var immediate := ImmediateMesh.new()
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_color = Color(1.0, 1.0, 1.0, 0.8)
	immediate.surface_begin(Mesh.PRIMITIVE_LINES, material)
	var world_region_size: float = float(terrain.region_size) * terrain.vertex_spacing
	for value: Variant in entries:
		if not value is Dictionary:
			continue
		var entry: Dictionary = value
		var bounds: AABB = entry.get("local_bounds", AABB())
		var region_min: Vector2i = entry.get("region_min", Vector2i.ZERO)
		var offset := Vector3(float(region_min.x) * world_region_size, 0.0, float(region_min.y) * world_region_size)
		_draw_aabb(immediate, AABB(bounds.position + offset, bounds.size))
	immediate.surface_end()
	_debug_mesh = MeshInstance3D.new()
	_debug_mesh.name = "__Terrain3DHLODDebug"
	_debug_mesh.mesh = immediate
	_debug_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	terrain.add_child(_debug_mesh, false, Node.INTERNAL_MODE_FRONT)


func _draw_aabb(mesh: ImmediateMesh, bounds: AABB) -> void:
	var p: Array[Vector3] = []
	for i: int in range(8):
		p.append(bounds.get_endpoint(i))
	var edges: Array[Vector2i] = [
		Vector2i(0, 1), Vector2i(0, 2), Vector2i(0, 4), Vector2i(1, 3), Vector2i(1, 5),
		Vector2i(2, 3), Vector2i(2, 6), Vector2i(3, 7), Vector2i(4, 5), Vector2i(4, 6),
		Vector2i(5, 7), Vector2i(6, 7)]
	for edge: Vector2i in edges:
		mesh.surface_add_vertex(p[edge.x])
		mesh.surface_add_vertex(p[edge.y])


func _clear_debug_mesh() -> void:
	if is_instance_valid(_debug_mesh):
		_debug_mesh.queue_free()
	_debug_mesh = null
