# Copyright © 2023-2026 Terrain3D Contributors.
# One native-property mutation path for editor controls and shared Resources.
@tool
class_name Terrain3DEditTransaction
extends RefCounted

var manager: EditorUndoRedoManager


func _init(p_manager: EditorUndoRedoManager = null) -> void:
	manager = p_manager


func apply(p_target: Object, p_property: String, p_value: Variant,
		p_action_name: String = "", p_merge: bool = false) -> bool:
	return apply_batch([{"target": p_target, "property": p_property, "value": p_value}],
		p_action_name if not p_action_name.is_empty() else "Change Terrain %s" % p_property.capitalize(), p_merge)


# Native preset commands are one action. The old scalar overrides are the undo
# snapshot, while redo uses the native preset setter (one cold rebuild).
func apply_native_command(p_target: Object, p_method: StringName,
		p_undo_edits: Array[Dictionary], p_action_name: String) -> bool:
	if manager == null or not is_instance_valid(p_target) or not p_target.has_method(p_method):
		return false
	if p_undo_edits.is_empty():
		return false
	for edit: Dictionary in p_undo_edits:
		var owner: Object = edit.get("target")
		if not is_instance_valid(owner) or str(edit.get("property", "")).is_empty():
			return false
	manager.create_action(p_action_name, UndoRedo.MERGE_DISABLE, p_target)
	manager.add_do_method(p_target, p_method)
	for edit: Dictionary in p_undo_edits:
		var owner: Object = edit["target"]
		manager.add_undo_property(owner, edit["property"], edit["value"])
		if owner is Resource:
			manager.add_undo_method(owner, "emit_changed")
	manager.add_do_method(p_target, "notify_property_list_changed")
	manager.add_undo_method(p_target, "notify_property_list_changed")
	manager.commit_action()
	return true


func apply_batch(p_edits: Array[Dictionary], p_action_name: String,
		p_merge: bool = false) -> bool:
	var changes: Array[Dictionary] = []
	for edit: Dictionary in p_edits:
		var target: Object = edit.get("target")
		var property: String = str(edit.get("property", ""))
		if not is_instance_valid(target) or property.is_empty():
			return false
		var info_found: bool = false
		for info: Dictionary in target.get_property_list():
			if str(info.get("name", "")) == property:
				if int(info.get("usage", 0)) & PROPERTY_USAGE_READ_ONLY:
					return false
				info_found = true
				break
		if not info_found:
			return false
		var old: Variant = target.get(property)
		var new: Variant = edit.get("value")
		if old != new:
			changes.append({"target": target, "property": property, "old": old, "new": new})
	if changes.is_empty():
		return false
	if manager == null:
		push_error("Terrain3D editor transaction requires EditorUndoRedoManager")
		return false
	var context: Object = changes[0]["target"]
	manager.create_action(p_action_name, UndoRedo.MERGE_ENDS if p_merge else UndoRedo.MERGE_DISABLE, context)
	for change: Dictionary in changes:
		var target: Object = change["target"]
		manager.add_do_property(target, change["property"], change["new"])
		manager.add_undo_property(target, change["property"], change["old"])
		if target is Resource:
			manager.add_do_method(target, "emit_changed")
			manager.add_undo_method(target, "emit_changed")
		elif target is Terrain3D:
			manager.add_do_method(target, "notify_property_list_changed")
			manager.add_undo_method(target, "notify_property_list_changed")
	manager.commit_action()
	return true


# Execution has one registered native property plus a native node-preview method.
# Both changes belong to one Undo/Redo action, without reviving the old alias.
func apply_execution_policy(p_terrain: Terrain3D, p_mode: int, p_preview: bool) -> bool:
	if manager == null or not is_instance_valid(p_terrain):
		return false
	var old_mode: int = p_terrain.get_editor_execution_mode()
	var old_preview: bool = p_terrain.is_editor_node_streaming_preview_enabled()
	if old_mode == p_mode and old_preview == p_preview:
		return false
	manager.create_action("Change Terrain Editor Execution", UndoRedo.MERGE_DISABLE, p_terrain)
	manager.add_do_method(p_terrain, "set_editor_execution_mode", p_mode)
	manager.add_do_method(p_terrain, "set_editor_node_streaming_preview", p_preview)
	manager.add_undo_method(p_terrain, "set_editor_execution_mode", old_mode)
	manager.add_undo_method(p_terrain, "set_editor_node_streaming_preview", old_preview)
	manager.commit_action()
	return true
