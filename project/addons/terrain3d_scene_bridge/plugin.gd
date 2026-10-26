@tool
extends EditorPlugin

const BridgeDock = preload("res://addons/terrain3d_scene_bridge/bridge_dock.gd")
const BridgeService = preload("res://addons/terrain3d_scene_bridge/bridge_service.gd")

var _editor_dock: EditorDock
var _scroll_container: ScrollContainer
var _dock: BridgeDock
var _service: BridgeService
var _dock_registered: bool = false


func _enter_tree() -> void:
	_service = BridgeService.new()
	_dock = BridgeDock.new()
	_dock.setup(self, _service)

	# Godot 4.7's EditorDock API owns dock layout state. Keep the bridge content
	# behind a ScrollContainer so its tall VBox minimum size can never force the
	# editor's bottom panel to collapse to satisfy the side-dock minimum height.
	_scroll_container = ScrollContainer.new()
	_scroll_container.name = "Terrain3DSceneBridgeScroll"
	_scroll_container.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_scroll_container.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	_scroll_container.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_scroll_container.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_scroll_container.add_child(_dock)
	_dock.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_dock.size_flags_vertical = Control.SIZE_SHRINK_BEGIN

	_editor_dock = EditorDock.new()
	_editor_dock.name = "Terrain3DSceneBridgeDock"
	_editor_dock.title = "Terrain3D Bridge"
	_editor_dock.layout_key = "terrain3d_scene_bridge"
	_editor_dock.default_slot = EditorDock.DOCK_SLOT_RIGHT_UL
	_editor_dock.available_layouts = EditorDock.DOCK_LAYOUT_VERTICAL | EditorDock.DOCK_LAYOUT_FLOATING
	_editor_dock.closable = false
	_editor_dock.set_global(false)
	_editor_dock.add_child(_scroll_container)
	add_dock(_editor_dock)
	_dock_registered = true

	_service.log_message.connect(_dock.append_log)
	_service.progress_changed.connect(_dock.update_progress)
	_service.operation_finished.connect(_dock.show_operation_result)
	_dock.call_deferred(&"refresh_anchors")


func _exit_tree() -> void:
	if _service != null and _dock != null and is_instance_valid(_dock):
		if _service.log_message.is_connected(_dock.append_log):
			_service.log_message.disconnect(_dock.append_log)
		if _service.progress_changed.is_connected(_dock.update_progress):
			_service.progress_changed.disconnect(_dock.update_progress)
		if _service.operation_finished.is_connected(_dock.show_operation_result):
			_service.operation_finished.disconnect(_dock.show_operation_result)

	if _editor_dock != null and is_instance_valid(_editor_dock):
		if _dock_registered:
			remove_dock(_editor_dock)
		_editor_dock.queue_free()

	_dock_registered = false
	_editor_dock = null
	_scroll_container = null
	_dock = null
	_service = null


func pick_terrain_from_selection() -> void:
	var selected: Node = _get_single_selected_node()
	if selected == null:
		_log("Select one Terrain3D node in the scene tree.")
		return
	if not selected.is_class(&"Terrain3D"):
		_log("Selected node is %s, not Terrain3D." % selected.get_class())
		return
	_dock.set_terrain_node(selected)
	_log("Terrain target: %s" % selected.get_path())


func auto_detect_terrain() -> void:
	var scene_root: Node = get_editor_interface().get_edited_scene_root()
	if scene_root == null:
		_log("No edited scene is open.")
		return
	var terrain: Node = _find_first_terrain(scene_root)
	if terrain == null:
		_log("No Terrain3D node exists in the edited scene.")
		return
	_dock.set_terrain_node(terrain)
	_log("Auto-detected Terrain3D: %s" % terrain.get_path())


func pick_source_from_selection() -> void:
	var selected: Node = _get_single_selected_node()
	if selected == null:
		_log("Select one Node3D source root containing Visual/Ground + Visual/Landmarks or regional instance nodes.")
		return
	if not selected is Node3D:
		_log("Selected node is %s, not Node3D." % selected.get_class())
		return
	if selected.is_class(&"Terrain3D"):
		_log("Terrain3D cannot be used as the editable source container.")
		return
	_dock.set_source_node(selected)
	_log("Scene source root: %s" % selected.get_path())


func scan_source() -> void:
	var source: Node3D = _dock.get_source_node()
	var scan: Dictionary = _service.scan_source(source)
	_dock.show_scan(scan)
	if bool(scan.get("ok", false)):
		var source_mode: String = String(scan.get("source_mode", "regional"))
		var mode_label: String = "Visual roots" if source_mode == "visual_roots" else "regional hierarchy"
		_log("Scan (%s): %d regions, %d GLB/PackedScene instances, %d extracted MeshInstance3D placements across %d mesh types." % [
			mode_label,
			int(scan.get("region_count", 0)),
			int(scan.get("glb_instance_count", 0)),
			int(scan.get("mesh_instance_count", scan.get("placement_count", 0))),
			int(scan.get("type_count", 0)),
		])


func bake_source_to_terrain() -> void:
	var scene_root: Node = get_editor_interface().get_edited_scene_root()
	var terrain: Node = _dock.get_terrain_node()
	var source: Node3D = _dock.get_source_node()
	var prepared: Dictionary = _service.prepare_bake(
		scene_root,
		terrain,
		source,
		_dock.should_remove_source(),
		_dock.should_skip_missing_regions()
	)
	if not bool(prepared.get("ok", false)):
		_log(String(prepared.get("error", "Bake preparation failed.")))
		return

	var transaction_id: int = int(prepared["transaction_id"])
	var undo_redo: EditorUndoRedoManager = get_undo_redo()
	undo_redo.create_action(
		"Extract GLB meshes to Terrain3D",
		UndoRedo.MERGE_DISABLE,
		scene_root
	)
	undo_redo.add_do_method(_service, &"apply_transaction", transaction_id)
	undo_redo.add_undo_method(_service, &"revert_transaction", transaction_id)
	undo_redo.add_do_reference(prepared["anchor"])
	if bool(prepared["remove_source"]):
		undo_redo.add_undo_reference(prepared["source_root"])
		var removed_source_nodes: Array = prepared.get("removed_source_nodes", [])
		for removal_entry_variant in removed_source_nodes:
			var removal_entry: Dictionary = removal_entry_variant as Dictionary
			var removed_node: Node = removal_entry.get("node", null)
			if removed_node != null:
				undo_redo.add_undo_reference(removed_node)
	undo_redo.commit_action()
	_mark_scene_unsaved()
	if bool(prepared.get("source_root_removed", false)):
		_dock.set_source_node(null)
	_dock.call_deferred(&"refresh_anchors")


func restore_terrain_to_nodes() -> void:
	var scene_root: Node = get_editor_interface().get_edited_scene_root()
	var terrain: Node = _dock.get_terrain_node()
	var anchor: Node = _dock.get_selected_anchor()
	var prepared: Dictionary = _service.prepare_restore(
		scene_root,
		terrain,
		anchor,
		_dock.should_clear_after_restore()
	)
	if not bool(prepared.get("ok", false)):
		_log(String(prepared.get("error", "Restore preparation failed.")))
		return

	var transaction_id: int = int(prepared["transaction_id"])
	var undo_redo: EditorUndoRedoManager = get_undo_redo()
	undo_redo.create_action(
		"Restore Terrain3D placements to scene nodes",
		UndoRedo.MERGE_DISABLE,
		scene_root
	)
	undo_redo.add_do_method(_service, &"apply_transaction", transaction_id)
	undo_redo.add_undo_method(_service, &"revert_transaction", transaction_id)
	undo_redo.add_do_reference(prepared["output_root"])
	if bool(prepared["clear_terrain_instances"]):
		undo_redo.add_undo_reference(prepared["anchor"])
	undo_redo.commit_action()
	_mark_scene_unsaved()
	var output_root: Node = prepared["output_root"] as Node
	if output_root != null:
		_dock.set_source_node(output_root)
	_dock.call_deferred(&"refresh_anchors")


func _get_single_selected_node() -> Node:
	var selection: EditorSelection = get_editor_interface().get_selection()
	var selected_nodes: Array[Node] = selection.get_selected_nodes()
	if selected_nodes.size() != 1:
		return null
	return selected_nodes[0]


func _find_first_terrain(current: Node) -> Node:
	if current.is_class(&"Terrain3D"):
		return current
	for child: Node in current.get_children():
		var terrain: Node = _find_first_terrain(child)
		if terrain != null:
			return terrain
	return null


func _mark_scene_unsaved() -> void:
	var editor_interface: EditorInterface = get_editor_interface()
	if editor_interface.has_method(&"mark_scene_as_unsaved"):
		editor_interface.call(&"mark_scene_as_unsaved")


func _log(message: String) -> void:
	if _dock != null and is_instance_valid(_dock):
		_dock.append_log(message)
	push_warning("Terrain3D Scene Bridge: %s" % message)
