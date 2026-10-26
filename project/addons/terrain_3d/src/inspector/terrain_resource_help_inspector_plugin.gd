# Copyright © 2023-2026 Terrain3D Contributors.
# Documentation-only Inspector extension; preserves native Godot resource editors.
@tool
extends EditorInspectorPlugin

const PropertyHelp = preload("res://addons/terrain_3d/src/inspector/terrain_property_help.gd")
const SettingMetadata = preload("res://addons/terrain_3d/src/setting_metadata.gd")


func _can_handle(p_object: Object) -> bool:
	if not p_object is Resource:
		return false
	var native_class: String = p_object.get_class()
	if not native_class.begins_with("Terrain3D"):
		return false
	for info: Dictionary in p_object.get_property_list():
		if int(info.get("usage", 0)) & PROPERTY_USAGE_EDITOR:
			if not SettingMetadata.tooltip(native_class, StringName(str(info.get("name", "")))).is_empty():
				return true
	return false


func _parse_begin(p_object: Object) -> void:
	add_custom_control(PropertyHelp.create_panel(p_object))
