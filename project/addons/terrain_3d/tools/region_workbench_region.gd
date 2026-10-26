@tool
class_name Terrain3DRegionWorkbenchRegion
extends RefCounted

const EditorUtil = preload("res://addons/terrain_3d/src/editor_util.gd")
const WorkbenchMetadata = preload("res://addons/terrain_3d/tools/region_workbench_metadata.gd")

var location: Vector2i = Vector2i.ZERO
var source_region_path: String = ""
var source_region_md5: String = ""
var source_page_md5: String = ""
var workbench_region_path: String = ""
var last_archive_directory: String = ""
var last_region_node_directory: String = ""
var streamed_node_count: int = 0
var committed_unix: int = 0


static func from_value(p_value: Variant) -> Terrain3DRegionWorkbenchRegion:
	if not p_value is Dictionary:
		return null
	var values := Terrain3DRuntimeValueMap.from_value(p_value)
	if values == null:
		return null
	var entry := Terrain3DRegionWorkbenchRegion.new()
	entry.location = EditorUtil.vector2i_from_value(values.raw_value("location"))
	entry.source_region_path = values.string_value("source_region_path")
	entry.source_region_md5 = values.string_value("source_region_md5")
	entry.source_page_md5 = values.string_value("source_page_md5")
	entry.workbench_region_path = values.string_value("workbench_region_path")
	entry.last_archive_directory = values.string_value("last_archive_directory")
	entry.last_region_node_directory = values.string_value("last_region_node_directory")
	entry.streamed_node_count = values.int_value("streamed_node_count")
	entry.committed_unix = values.int_value("committed_unix")
	return entry




func to_dictionary() -> Dictionary[String, Variant]:
	var result: Dictionary[String, Variant] = {
		"location": EditorUtil.vector2i_to_array(location),
		"source_region_path": source_region_path,
		"source_region_md5": source_region_md5,
		"source_page_md5": source_page_md5,
		"workbench_region_path": workbench_region_path,
	}
	WorkbenchMetadata.append_checkout_state(
			result, last_archive_directory, last_region_node_directory, streamed_node_count, committed_unix)
	return result
