@tool
class_name T3DRegionWorkbenchConfiguration
extends RefCounted

var ok: bool = false
var error: String = ""
var properties: Dictionary[String, Variant] = {}
var resources: Dictionary[String, String] = {}


static func success(p_properties: Dictionary[String, Variant],
		p_resources: Dictionary[String, String]) -> T3DRegionWorkbenchConfiguration:
	var result := T3DRegionWorkbenchConfiguration.new()
	result.ok = true
	result.properties = p_properties
	result.resources = p_resources
	return result


static func failure(p_error: String) -> T3DRegionWorkbenchConfiguration:
	var result := T3DRegionWorkbenchConfiguration.new()
	result.error = p_error
	return result
