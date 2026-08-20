@tool
class_name T3DRegionNodeBuildResult
extends RefCounted

var ok: bool = false
var error: String = ""
var descriptors: Array[T3DRegionNodeDescriptor] = []
var generated_paths: PackedStringArray = PackedStringArray()
var node_directory: String = ""
var warnings: PackedStringArray = PackedStringArray()


static func success() -> T3DRegionNodeBuildResult:
	var result := T3DRegionNodeBuildResult.new()
	result.ok = true
	return result


static func failure(p_error: String) -> T3DRegionNodeBuildResult:
	var result := T3DRegionNodeBuildResult.new()
	result.error = p_error
	return result
