# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Transient editor-only Node3D used to expose Godot's native transform gizmo for one Terrain3D instance.
@tool
extends Node3D

signal transform_edited(global_transform: Transform3D)

var _handle: Dictionary = {}
var _syncing: bool = false
var _last_valid_transform: Transform3D = Transform3D.IDENTITY


func _ready() -> void:
	assert(Engine.is_editor_hint())
	set_notify_transform(true)
	set_as_top_level(true)
	set_meta("_terrain3d_instance_edit_proxy", true)


func _notification(what: int) -> void:
	if what == NOTIFICATION_TRANSFORM_CHANGED and not _syncing and not _handle.is_empty():
		transform_edited.emit(global_transform)


func bind_handle(p_handle: Dictionary) -> void:
	_handle = p_handle.duplicate(true)
	var handle_transform: Transform3D = p_handle.get("transform", Transform3D.IDENTITY)
	_last_valid_transform = handle_transform
	_syncing = true
	global_transform = _last_valid_transform
	_syncing = false


func accept_handle(p_handle: Dictionary) -> void:
	_handle = p_handle.duplicate(true)
	var handle_transform: Transform3D = p_handle.get("transform", global_transform)
	_last_valid_transform = handle_transform


func restore_last_valid_transform() -> void:
	_syncing = true
	global_transform = _last_valid_transform
	_syncing = false


func clear_handle() -> void:
	_handle.clear()


func get_handle() -> Dictionary:
	return _handle


func has_handle() -> bool:
	return not _handle.is_empty() and bool(_handle.get("valid", false))


func is_same_handle(p_handle: Dictionary) -> bool:
	if not has_handle() or p_handle.is_empty() or not bool(p_handle.get("valid", false)):
		return false
	if not _handle.has("revision") or not p_handle.has("revision"):
		return false
	return _handle.get("region", Vector2i(2147483647, 2147483647)) == p_handle.get("region", Vector2i(2147483647, 2147483647)) \
		and _handle.get("cell", Vector2i(2147483647, 2147483647)) == p_handle.get("cell", Vector2i(2147483647, 2147483647)) \
		and int(_handle.get("mesh_id", -1)) == int(p_handle.get("mesh_id", -1)) \
		and int(_handle.get("instance_index", -1)) == int(p_handle.get("instance_index", -1)) \
		and int(_handle.get("revision", 0)) == int(p_handle.get("revision", 0))
