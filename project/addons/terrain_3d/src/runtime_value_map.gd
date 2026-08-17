# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Typed access facade for heterogeneous native/JSON dictionaries.
@tool
class_name Terrain3DRuntimeValueMap
extends RefCounted

var _values: Dictionary[String, Variant] = {}


static func from_value(p_value: Variant) -> Terrain3DRuntimeValueMap:
	var result: Terrain3DRuntimeValueMap = Terrain3DRuntimeValueMap.new()
	if p_value is Dictionary:
		result._values.assign(p_value)
	return result


static func maps_from_value(p_value: Variant) -> Array[Terrain3DRuntimeValueMap]:
	var result: Array[Terrain3DRuntimeValueMap] = []
	if not p_value is Array:
		return result
	var source: Array[Variant] = []
	source.assign(p_value)
	for entry: Variant in source:
		if entry is Dictionary:
			result.push_back(from_value(entry))
	return result


static func values_from_maps(p_maps: Array[Terrain3DRuntimeValueMap]) -> Array[Variant]:
	var result: Array[Variant] = []
	for entry: Terrain3DRuntimeValueMap in p_maps:
		result.push_back(entry.duplicate_values())
	return result


func is_empty() -> bool:
	return _values.is_empty()


func size() -> int:
	return _values.size()


func has(p_key: String) -> bool:
	return _values.has(p_key)


func keys() -> PackedStringArray:
	var result: PackedStringArray = PackedStringArray()
	for key: String in _values:
		result.push_back(key)
	return result


func int_value(p_key: String, p_default: int = 0) -> int:
	return int(_values.get(p_key, p_default))


func float_value(p_key: String, p_default: float = 0.0) -> float:
	return float(_values.get(p_key, p_default))


func bool_value(p_key: String, p_default: bool = false) -> bool:
	return bool(_values.get(p_key, p_default))


func string_value(p_key: String, p_default: String = "") -> String:
	return String(_values.get(p_key, p_default))


func vector2_value(p_key: String, p_default: Vector2 = Vector2.ZERO) -> Vector2:
	var value: Variant = _values.get(p_key, p_default)
	return value if value is Vector2 else p_default


func vector2i_value(p_key: String, p_default: Vector2i = Vector2i.ZERO) -> Vector2i:
	var value: Variant = _values.get(p_key, p_default)
	return value if value is Vector2i else p_default


func transform3d_value(p_key: String, p_default: Transform3D = Transform3D.IDENTITY) -> Transform3D:
	var value: Variant = _values.get(p_key, p_default)
	return value if value is Transform3D else p_default


func object_value(p_key: String) -> Object:
	var value: Variant = _values.get(p_key)
	return value as Object if value is Object else null


func string_array(p_key: String) -> PackedStringArray:
	var value: Variant = _values.get(p_key, [])
	if value is PackedStringArray:
		return value
	if value is Array:
		return PackedStringArray(value)
	return PackedStringArray()


func int32_array(p_key: String) -> PackedInt32Array:
	var value: Variant = _values.get(p_key, PackedInt32Array())
	if value is PackedInt32Array:
		return value
	if value is Array:
		return PackedInt32Array(value)
	return PackedInt32Array()


func variant_array(p_key: String) -> Array[Variant]:
	var result: Array[Variant] = []
	var value: Variant = _values.get(p_key, [])
	if value is Array:
		result.assign(value)
	return result


func aabb_value(p_key: String, p_default: AABB = AABB()) -> AABB:
	var value: Variant = _values.get(p_key, p_default)
	return value if value is AABB else p_default


func map_value(p_key: String) -> Terrain3DRuntimeValueMap:
	return from_value(_values.get(p_key, {}))


func map_array(p_key: String) -> Array[Terrain3DRuntimeValueMap]:
	return maps_from_value(_values.get(p_key, []))


func raw_value(p_key: String) -> Variant:
	return _values.get(p_key)


func duplicate_values() -> Dictionary[String, Variant]:
	var result: Dictionary[String, Variant] = {}
	result.assign(_values.duplicate(true))
	return result


func formatted_value(p_key: String) -> String:
	return format_external_value(p_key, _values.get(p_key))


static func format_external_value(p_name: String, p_value: Variant) -> String:
	var key: String = p_name.to_lower()
	if p_value is int or p_value is float:
		var numeric: float = float(p_value)
		if key.ends_with("_usec"):
			return "%.3f ms" % (numeric / 1000.0)
		if key.ends_with("_seconds"):
			return "%.3f ms" % (numeric * 1000.0)
		if key.ends_with("_bytes"):
			return format_bytes(numeric)
		if key.ends_with("_mb"):
			return "%.2f MiB" % numeric
		if p_value is float:
			return "%.4f" % numeric
	return str(p_value)


static func format_bytes(p_bytes: float) -> String:
	if p_bytes >= 1073741824.0:
		return "%.2f GiB" % (p_bytes / 1073741824.0)
	if p_bytes >= 1048576.0:
		return "%.2f MiB" % (p_bytes / 1048576.0)
	if p_bytes >= 1024.0:
		return "%.2f KiB" % (p_bytes / 1024.0)
	return "%.0f B" % p_bytes
