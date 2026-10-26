@tool
extends VBoxContainer

const BridgeService = preload("res://addons/terrain3d_scene_bridge/bridge_service.gd")

var _plugin: EditorPlugin
var _service: BridgeService
var _source_node: Node3D
var _terrain_node: Node
var _anchors: Array[Node] = []

var _source_value: Label
var _terrain_value: Label
var _subdivision_value: Label
var _scan_summary: Label
var _anchor_picker: OptionButton
var _remove_source: CheckBox
var _skip_missing_regions: CheckBox
var _clear_after_restore: CheckBox
var _progress: ProgressBar
var _status: Label
var _log: RichTextLabel


func setup(plugin: EditorPlugin, service: BridgeService) -> void:
	_plugin = plugin
	_service = service


func _ready() -> void:
	name = "Terrain3D Bridge"
	custom_minimum_size = Vector2(320.0, 0.0)
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	_build_interface()


func get_source_node() -> Node3D:
	return _source_node if is_instance_valid(_source_node) else null


func get_terrain_node() -> Node:
	return _terrain_node if is_instance_valid(_terrain_node) else null


func get_selected_anchor() -> Node:
	var selected_index: int = _anchor_picker.selected
	if selected_index < 0 or selected_index >= _anchors.size():
		return null
	var anchor: Node = _anchors[selected_index]
	return anchor if is_instance_valid(anchor) else null


func should_remove_source() -> bool:
	return _remove_source.button_pressed


func should_skip_missing_regions() -> bool:
	return _skip_missing_regions.button_pressed


func should_clear_after_restore() -> bool:
	return _clear_after_restore.button_pressed


func set_source_node(source_node: Node) -> void:
	_source_node = source_node as Node3D
	_source_value.text = _describe_node(_source_node, "No source selected")
	_scan_summary.text = "Not scanned"


func set_terrain_node(terrain_node: Node) -> void:
	_terrain_node = terrain_node
	_terrain_value.text = _describe_node(_terrain_node, "No Terrain3D selected")
	_refresh_subdivision_summary()
	refresh_anchors()


func show_scan(scan: Dictionary) -> void:
	if not bool(scan.get("ok", false)):
		_scan_summary.text = String(scan.get("error", "Scan failed"))
		return
	var skipped_count: int = (scan.get("skipped_nodes", []) as Array).size()
	var source_mode: String = String(scan.get("source_mode", "regional"))
	var mode_label: String = "Visual/Ground + Visual/Landmarks" if source_mode == "visual_roots" else "Region hierarchy"
	_scan_summary.text = "%s: %d regions, %d GLB/PackedScene instances, %d extracted meshes, %d mesh types, %d unsupported leaves" % [
		mode_label,
		int(scan.get("region_count", 0)),
		int(scan.get("glb_instance_count", 0)),
		int(scan.get("mesh_instance_count", scan.get("placement_count", 0))),
		int(scan.get("type_count", 0)),
		skipped_count,
	]


func refresh_anchors() -> void:
	_anchors.clear()
	_anchor_picker.clear()
	if _service == null or not is_instance_valid(_terrain_node):
		_anchor_picker.add_item("No bridge batches")
		_anchor_picker.disabled = true
		return
	var found_anchors: Array[Node] = _service.find_anchors(_terrain_node)
	for anchor: Node in found_anchors:
		_anchors.append(anchor)
	if _anchors.is_empty():
		_anchor_picker.add_item("No bridge batches")
		_anchor_picker.disabled = true
		return
	_anchor_picker.disabled = false
	for anchor: Node in _anchors:
		var label: String = String(anchor.name)
		if anchor.has_method("get_summary"):
			label = String(anchor.call("get_summary"))
		_anchor_picker.add_item(label)
	_anchor_picker.select(0)


func append_log(message: String) -> void:
	if _log == null:
		return
	_log.append_text("%s\n" % message)


func update_progress(value: float, status_text: String) -> void:
	if _progress == null or _status == null:
		return
	_keep_progress_visible()
	_progress.value = clampf(value, 0.0, 1.0) * 100.0
	_status.text = status_text
	_progress.queue_redraw()
	_status.queue_redraw()
	# Bridge work is synchronous on the editor thread. Force the queued Control
	# redraw now so the progress bar advances instead of jumping to 100% afterward.
	RenderingServer.force_draw(false)


func show_operation_result(success: bool, message: String) -> void:
	_keep_progress_visible()
	_status.text = message
	_status.queue_redraw()
	RenderingServer.force_draw(false)
	if success:
		refresh_anchors()


func _keep_progress_visible() -> void:
	var parent: Node = get_parent()
	if parent is ScrollContainer:
		var scroll: ScrollContainer = parent as ScrollContainer
		if scroll.get_v_scroll() > 0:
			scroll.ensure_control_visible(_progress)


func _build_interface() -> void:
	var title: Label = Label.new()
	title.text = "Terrain3D Scene Bridge"
	title.add_theme_font_size_override("font_size", 18)
	add_child(title)

	var description: Label = Label.new()
	description.text = "Round-trip placed GLB/PackedScene meshes through Terrain3D's native owner/cell instancer routing. Visual/Ground and Visual/Landmarks are scanned directly; legacy Region nodes remain supported. Native ownership follows each transform origin."
	description.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(description)

	_progress = ProgressBar.new()
	_progress.min_value = 0.0
	_progress.max_value = 100.0
	_progress.value = 0.0
	_progress.show_percentage = true
	_progress.custom_minimum_size = Vector2(0.0, 20.0)
	_progress.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(_progress)

	_status = Label.new()
	_status.text = "Idle"
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_status)
	add_child(HSeparator.new())

	_add_section_label("Terrain target")
	var terrain_row: HBoxContainer = HBoxContainer.new()
	_terrain_value = Label.new()
	_terrain_value.text = "No Terrain3D selected"
	_terrain_value.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_terrain_value.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	terrain_row.add_child(_terrain_value)
	var pick_terrain: Button = Button.new()
	pick_terrain.text = "Use Selected"
	pick_terrain.pressed.connect(_on_pick_terrain_pressed)
	terrain_row.add_child(pick_terrain)
	add_child(terrain_row)
	var auto_terrain: Button = Button.new()
	auto_terrain.text = "Auto-detect Terrain3D in Scene"
	auto_terrain.pressed.connect(_on_auto_detect_terrain_pressed)
	add_child(auto_terrain)
	_subdivision_value = Label.new()
	_subdivision_value.text = "Instancer subdivision: no Terrain3D selected"
	_subdivision_value.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_subdivision_value)

	_add_section_label("Scene source root")
	var source_row: HBoxContainer = HBoxContainer.new()
	_source_value = Label.new()
	_source_value.text = "No source selected"
	_source_value.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_source_value.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	source_row.add_child(_source_value)
	var pick_source: Button = Button.new()
	pick_source.text = "Use Selected"
	pick_source.pressed.connect(_on_pick_source_pressed)
	source_row.add_child(pick_source)
	add_child(source_row)

	var scan_button: Button = Button.new()
	scan_button.text = "Scan Source"
	scan_button.pressed.connect(_on_scan_pressed)
	add_child(scan_button)
	_scan_summary = Label.new()
	_scan_summary.text = "Not scanned"
	_scan_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_scan_summary)

	_remove_source = CheckBox.new()
	_remove_source.text = "Remove converted source instances after bake (undoable)"
	_remove_source.button_pressed = true
	_remove_source.tooltip_text = "Visual mode removes only converted GLB/PackedScene instance roots; legacy regional mode removes the selected source root. Undo restores the original hierarchy."
	add_child(_remove_source)

	_skip_missing_regions = CheckBox.new()
	_skip_missing_regions.text = "Skip placements outside active terrain regions"
	_skip_missing_regions.button_pressed = false
	_skip_missing_regions.tooltip_text = "Off by default because silently dropping placements breaks reversibility."
	add_child(_skip_missing_regions)

	var bake_button: Button = Button.new()
	bake_button.text = "Nodes  →  Terrain3D"
	bake_button.pressed.connect(_on_bake_pressed)
	add_child(bake_button)
	add_child(HSeparator.new())

	_add_section_label("Stored bridge batch")
	_anchor_picker = OptionButton.new()
	_anchor_picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(_anchor_picker)
	var refresh_button: Button = Button.new()
	refresh_button.text = "Refresh Bridge Batches"
	refresh_button.pressed.connect(refresh_anchors)
	add_child(refresh_button)

	_clear_after_restore = CheckBox.new()
	_clear_after_restore.text = "Clear Terrain3D batch after restore"
	_clear_after_restore.button_pressed = true
	_clear_after_restore.tooltip_text = "Makes restoration a move instead of a duplicate. Undo restores the Terrain3D batch."
	add_child(_clear_after_restore)

	var restore_button: Button = Button.new()
	restore_button.text = "Terrain3D  →  Nodes"
	restore_button.pressed.connect(_on_restore_pressed)
	add_child(restore_button)
	add_child(HSeparator.new())

	_log = RichTextLabel.new()
	_log.custom_minimum_size = Vector2(0.0, 150.0)
	_log.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_log.scroll_active = true
	_log.scroll_following = true
	_log.selection_enabled = true
	add_child(_log)


func _refresh_subdivision_summary() -> void:
	if _subdivision_value == null:
		return
	if _service == null or not is_instance_valid(_terrain_node):
		_subdivision_value.text = "Instancer subdivision: no Terrain3D selected"
		return
	var info: Dictionary = _service.get_subdivision_info(_terrain_node)
	if not bool(info.get("supported", false)):
		_subdivision_value.text = "Instancer subdivision API unavailable"
		return
	_subdivision_value.text = "Instancer subdivision: %d samples / %.2f world units, %d x %d cells per region%s" % [
		int(info.get("cell_size", 0)),
		float(info.get("cell_world_size", 0.0)),
		int(info.get("cells_per_region_axis", 0)),
		int(info.get("cells_per_region_axis", 0)),
		"; native batch ownership enabled" if bool(info.get("owner_supported", false)) else "",
	]


func _add_section_label(text: String) -> void:
	var label: Label = Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", 14)
	add_child(label)


func _describe_node(node: Node, fallback: String) -> String:
	if node == null or not is_instance_valid(node):
		return fallback
	return "%s  (%s)" % [node.name, node.get_class()]


func _on_pick_terrain_pressed() -> void:
	_plugin.call("pick_terrain_from_selection")


func _on_auto_detect_terrain_pressed() -> void:
	_plugin.call("auto_detect_terrain")


func _on_pick_source_pressed() -> void:
	_plugin.call("pick_source_from_selection")


func _on_scan_pressed() -> void:
	_plugin.call("scan_source")


func _on_bake_pressed() -> void:
	_plugin.call("bake_source_to_terrain")


func _on_restore_pressed() -> void:
	_plugin.call("restore_terrain_to_nodes")
