@tool
class_name Terrain3DRegionNodeBuildResult
extends RefCounted

var ok: bool = false
var error: String = ""
var descriptors: Array[Terrain3DRegionNodeDescriptor] = []
var generated_paths: PackedStringArray = PackedStringArray()
var node_directory: String = ""
var warnings: PackedStringArray = PackedStringArray()


static func success() -> Terrain3DRegionNodeBuildResult:
	var result := Terrain3DRegionNodeBuildResult.new()
	result.ok = true
	return result


static func failure(p_error: String) -> Terrain3DRegionNodeBuildResult:
	var result := Terrain3DRegionNodeBuildResult.new()
	result.error = p_error
	return result
