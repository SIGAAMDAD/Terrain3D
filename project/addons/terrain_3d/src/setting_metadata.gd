# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Generated-schema reader shared by editor setting surfaces.
@tool
extends RefCounted

const METADATA_PATH: String = "res://addons/terrain_3d/setting_metadata.json"

static var _loaded: bool = false
static var _properties: Dictionary = {}


static func _ensure_loaded() -> void:
	if _loaded:
		return
	_loaded = true
	if not FileAccess.file_exists(METADATA_PATH):
		push_error("Terrain3D setting metadata is missing: %s" % METADATA_PATH)
		return
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(METADATA_PATH))
	if not parsed is Dictionary:
		push_error("Terrain3D setting metadata is invalid JSON: %s" % METADATA_PATH)
		return
	_properties = (parsed as Dictionary).get("properties", {})


static func get_by_key(p_key: String) -> Dictionary:
	_ensure_loaded()
	return _properties.get(p_key, {})


static func get_record(p_class_name: String, p_property: StringName) -> Dictionary:
	return get_by_key("%s.%s" % [p_class_name, String(p_property)])


static func tooltip(p_class_name: String, p_property: StringName) -> String:
	return str(get_record(p_class_name, p_property).get("tooltip", ""))


static func tooltip_for_key(p_key: String) -> String:
	return str(get_by_key(p_key).get("tooltip", ""))

static func descriptors_for_class(p_class_name: String, p_group_prefixes: Array = []) -> Array[Dictionary]:
	_ensure_loaded()
	var result: Array[Dictionary] = []
	for property_info: Dictionary in ClassDB.class_get_property_list(StringName(p_class_name), true):
		var property_name: String = str(property_info.get("name", ""))
		if property_name.is_empty():
			continue
		var record: Dictionary = get_record(p_class_name, StringName(property_name))
		if record.is_empty() or str(record.get("state", "")) != "editable" or str(record.get("visibility", "hidden")) == "hidden":
			continue
		var group_path: String = str(record.get("group_path", ""))
		if not _matches_group(group_path, p_group_prefixes):
			continue
		var descriptor: Dictionary = record.duplicate(true)
		descriptor["class_name"] = p_class_name
		descriptor["property_name"] = property_name
		descriptor["key"] = "%s.%s" % [p_class_name, property_name]
		descriptor["variant_type"] = int(property_info.get("type", TYPE_NIL))
		descriptor["hint"] = int(property_info.get("hint", PROPERTY_HINT_NONE))
		descriptor["hint_string"] = str(property_info.get("hint_string", ""))
		descriptor["usage"] = int(property_info.get("usage", 0))
		descriptor["mutation_mode"] = mutation_mode(record)
		result.append(descriptor)
	return result


static func group_descriptors(p_descriptors: Array[Dictionary]) -> Array[Dictionary]:
	var groups: Array[Dictionary] = []
	var current_path: String = ""
	for descriptor: Dictionary in p_descriptors:
		var group_path: String = str(descriptor.get("group_path", "General"))
		if group_path != current_path:
			current_path = group_path
			groups.append({"group_path": current_path, "settings": []})
		var settings: Array = groups[-1]["settings"]
		settings.append(descriptor)
		groups[-1]["settings"] = settings
	return groups


static func mutation_mode(p_record: Dictionary) -> String:
	if str(p_record.get("profile_ownership", "none")) != "none":
		return "profile_owned_override"
	return "serialized_configuration"


static func mutation_mode_label(p_mode: String) -> String:
	match p_mode:
		"profile_owned_override":
			return "Profile-owned serialized override"
		"temporary_runtime_override":
			return "Temporary runtime override"
		_:
			return "Serialized configuration"


static func _matches_group(p_group_path: String, p_prefixes: Array) -> bool:
	if p_prefixes.is_empty():
		return true
	for prefix: String in p_prefixes:
		if p_group_path == prefix or p_group_path.begins_with(prefix + " / "):
			return true
	return false
