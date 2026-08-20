@tool
class_name T3DRegionWorkbenchResult
extends RefCounted

var ok: bool = false
var error: String = ""
var scene_path: String = ""
var manifest_path: String = ""
var workbench_region_path: String = ""
var workbench_region_paths: PackedStringArray = PackedStringArray()
var source_region_path: String = ""
var archive_directory: String = ""
var region_node_directory: String = ""
var streamed_node_count: int = 0
var region_count: int = 0
var context_region_count: int = 0
var region_locations: Array[Vector2i] = []
var warnings: PackedStringArray = PackedStringArray()
var region_location: Vector2i = Vector2i.ZERO
var source_scene: String = ""


static func success() -> T3DRegionWorkbenchResult:
	var result := T3DRegionWorkbenchResult.new()
	result.ok = true
	return result


static func failure(p_error: String) -> T3DRegionWorkbenchResult:
	var result := T3DRegionWorkbenchResult.new()
	result.error = p_error
	return result
