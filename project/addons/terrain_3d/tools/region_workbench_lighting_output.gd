@tool
class_name T3DRegionWorkbenchLightingOutput
extends RefCounted

var cluster_min: Vector2i = Vector2i.ZERO
var cluster_max: Vector2i = Vector2i.ZERO
var region_locations: Array[Vector2i] = []
var workbench_light_data_path: String = ""
var workbench_package_path: String = ""
var source_light_data_path: String = ""
var source_package_path: String = ""
var source_light_data_md5: String = ""
var source_package_md5: String = ""
var bake_signature: int = 0
var finalized_unix: int = 0


static func from_value(value: Variant) -> T3DRegionWorkbenchLightingOutput:
	if not value is Dictionary:
		return null
	var map := T3DRuntimeValueMap.from_value(value)
	if map == null:
		return null
	var output := T3DRegionWorkbenchLightingOutput.new()
	output.cluster_min = _vector2i_from_value(map.raw_value("cluster_min"))
	output.cluster_max = _vector2i_from_value(map.raw_value("cluster_max"))
	var raw_locations: Variant = map.raw_value("region_locations")
	if raw_locations is Array:
		for raw_location: Variant in raw_locations:
			output.region_locations.push_back(_vector2i_from_value(raw_location))
	output.workbench_light_data_path = map.string_value("workbench_light_data_path")
	output.workbench_package_path = map.string_value("workbench_package_path")
	output.source_light_data_path = map.string_value("source_light_data_path")
	output.source_package_path = map.string_value("source_package_path")
	output.source_light_data_md5 = map.string_value("source_light_data_md5")
	output.source_package_md5 = map.string_value("source_package_md5")
	output.bake_signature = map.int_value("bake_signature")
	output.finalized_unix = map.int_value("finalized_unix")
	return output


func to_dictionary() -> Dictionary[String, Variant]:
	var locations: Array[Variant] = []
	for location: Vector2i in region_locations:
		locations.push_back([location.x, location.y])
	return {
		"cluster_min": [cluster_min.x, cluster_min.y],
		"cluster_max": [cluster_max.x, cluster_max.y],
		"region_locations": locations,
		"workbench_light_data_path": workbench_light_data_path,
		"workbench_package_path": workbench_package_path,
		"source_light_data_path": source_light_data_path,
		"source_package_path": source_package_path,
		"source_light_data_md5": source_light_data_md5,
		"source_package_md5": source_package_md5,
		"bake_signature": bake_signature,
		"finalized_unix": finalized_unix,
	}


func contains(location: Vector2i) -> bool:
	if not region_locations.is_empty():
		return region_locations.has(location)
	return location.x >= cluster_min.x and location.x <= cluster_max.x and \
		location.y >= cluster_min.y and location.y <= cluster_max.y


func is_staged() -> bool:
	return not workbench_light_data_path.is_empty() and not workbench_package_path.is_empty() and \
		FileAccess.file_exists(workbench_light_data_path) and FileAccess.file_exists(workbench_package_path)


static func _vector2i_from_value(value: Variant) -> Vector2i:
	if value is Vector2i:
		return value
	if value is Array:
		var values: Array = value
		if values.size() == 2:
			return Vector2i(int(values[0]), int(values[1]))
	return Vector2i.ZERO
