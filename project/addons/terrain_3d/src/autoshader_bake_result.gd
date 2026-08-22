@tool
class_name Terrain3DAutoShaderBakeResult
extends RefCounted

var ok: bool = false
var error: String = ""
var errors: PackedStringArray = PackedStringArray()
var region_signatures: Dictionary[String, int] = {}
var processed_regions: int = 0
var changed_regions: int = 0
var changed_pixels: int = 0


static func failure(p_error: String) -> Terrain3DAutoShaderBakeResult:
	var result := Terrain3DAutoShaderBakeResult.new()
	result.error = p_error
	return result


static func from_value(p_value: Variant) -> Terrain3DAutoShaderBakeResult:
	if not p_value is Dictionary:
		return failure("Autoshader baker returned an invalid result.")
	var values := Terrain3DRuntimeValueMap.from_value(p_value)
	var result := Terrain3DAutoShaderBakeResult.new()
	result.ok = values.bool_value("ok")
	result.error = values.string_value("error")
	result.errors = values.string_array("errors")
	result.processed_regions = values.int_value("processed_regions")
	result.changed_regions = values.int_value("changed_regions")
	result.changed_pixels = values.int_value("changed_pixels")
	if result.processed_regions == 0:
		result.processed_regions = result.changed_regions
	var signatures := values.map_value("region_signatures")
	for key: String in signatures.keys():
		result.region_signatures[key] = signatures.int_value(key)
	return result
