@tool
class_name T3DRegionWorkbenchRegion
extends RefCounted

var location: Vector2i = Vector2i.ZERO
var source_region_path: String = ""
var source_region_md5: String = ""
var source_page_md5: String = ""
var workbench_region_path: String = ""
var last_archive_directory: String = ""
var last_region_node_directory: String = ""
var streamed_node_count: int = 0
var committed_unix: int = 0


static func from_value(p_value: Variant) -> T3DRegionWorkbenchRegion:
	if not p_value is Dictionary:
		return null
	var values := T3DRuntimeValueMap.from_value(p_value)
	if values == null:
		return null
	var entry := T3DRegionWorkbenchRegion.new()
	entry.location = _location_from_value(values.raw_value("location"))
	entry.source_region_path = values.string_value("source_region_path")
	entry.source_region_md5 = values.string_value("source_region_md5")
	entry.source_page_md5 = values.string_value("source_page_md5")
	entry.workbench_region_path = values.string_value("workbench_region_path")
	entry.last_archive_directory = values.string_value("last_archive_directory")
	entry.last_region_node_directory = values.string_value("last_region_node_directory")
	entry.streamed_node_count = values.int_value("streamed_node_count")
	entry.committed_unix = values.int_value("committed_unix")
	return entry


static func _location_from_value(p_value: Variant) -> Vector2i:
	if p_value is Vector2i:
		return p_value
	if p_value is Array:
		var values: Array[Variant] = p_value
		if values.size() == 2:
			return Vector2i(int(values[0]), int(values[1]))
	return Vector2i.ZERO


func to_dictionary() -> Dictionary[String, Variant]:
	var result: Dictionary[String, Variant] = {
		"location": [location.x, location.y],
		"source_region_path": source_region_path,
		"source_region_md5": source_region_md5,
		"source_page_md5": source_page_md5,
		"workbench_region_path": workbench_region_path,
	}
	if not last_archive_directory.is_empty():
		result["last_archive_directory"] = last_archive_directory
	if not last_region_node_directory.is_empty():
		result["last_region_node_directory"] = last_region_node_directory
	if streamed_node_count != 0:
		result["streamed_node_count"] = streamed_node_count
	if committed_unix != 0:
		result["committed_unix"] = committed_unix
	return result
