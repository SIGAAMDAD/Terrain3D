@tool
class_name Terrain3DLandformNativeResult
extends RefCounted

var ok: bool = false
var error: String = ""
var before: Array[Variant] = []
var after: Array[Variant] = []
var edited_area: AABB = AABB()
var modified_regions: int = 0
var modules_ok: bool = false
var modules_error: String = ""
var placements: Terrain3DLandformPlacements = Terrain3DLandformPlacements.new()
var module_count: int = 0
var mesh: ArrayMesh
var collision_mesh: ArrayMesh


static func from_apply_value(p_value: Variant) -> Terrain3DLandformNativeResult:
	var result := Terrain3DLandformNativeResult.new()
	if not p_value is Dictionary:
		result.error = "Landform baker returned an invalid result."
		return result
	var values := Terrain3DRuntimeValueMap.from_value(p_value)
	result.ok = values.bool_value("ok")
	result.error = values.string_value("error")
	result.before = values.variant_array("before")
	result.after = values.variant_array("after")
	result.edited_area = values.aabb_value("edited_area", AABB())
	result.modified_regions = values.int_value("modified_regions")
	result.mesh = values.object_value("mesh") as ArrayMesh
	result.collision_mesh = values.object_value("collision_mesh") as ArrayMesh
	var modules := values.map_value("modules")
	if modules != null and not modules.is_empty():
		result.modules_ok = modules.bool_value("ok")
		result.modules_error = modules.string_value("error")
		result.placements = Terrain3DLandformPlacements.from_value(modules.raw_value("placements"))
		result.module_count = modules.int_value("count")
	return result


static func from_modules_value(p_value: Variant) -> Terrain3DLandformNativeResult:
	var result := Terrain3DLandformNativeResult.new()
	if not p_value is Dictionary:
		result.modules_error = "Landform baker returned an invalid module result."
		return result
	var values := Terrain3DRuntimeValueMap.from_value(p_value)
	result.modules_ok = values.bool_value("ok")
	result.modules_error = values.string_value("error")
	result.placements = Terrain3DLandformPlacements.from_value(values.raw_value("placements"))
	result.module_count = values.int_value("count")
	return result
