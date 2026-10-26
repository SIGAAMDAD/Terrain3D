@tool
class_name Terrain3DRegionWorkbenchManifest
extends RefCounted

const EditorUtil = preload("res://addons/terrain_3d/src/editor_util.gd")
const WorkbenchMetadata = preload("res://addons/terrain_3d/tools/region_workbench_metadata.gd")

var format: int = 6
var source_scene: String = ""
var source_data_directory: String = ""
var source_far_atlas_md5: String = ""
# Primary region is retained as the focus/compatibility coordinate for tools such
# as regional lightmap baking. `regions` is the authoritative checkout set.
var region_location: Vector2i = Vector2i.ZERO
var source_region_path: String = ""
var source_region_md5: String = ""
var source_page_md5: String = ""
var region_size: int = 128
var vertex_spacing: float = 1.0
var workbench_data_directory: String = ""
var workbench_context_directory: String = ""
var workbench_region_path: String = ""
var workbench_scene: String = ""
var created_unix: int = 0
var terrain_properties: Dictionary[String, Variant] = {}
var terrain_resources: Dictionary[String, String] = {}
var regions: Array[Terrain3DRegionWorkbenchRegion] = []
# Read-only terrain copied around the editable checkout. Two dependency rings
# let cooked pages for the editable set and its immediate neighbors resolve the
# same height/control halo they would see in the full source data set.
var context_regions: Array[Terrain3DRegionWorkbenchRegion] = []
var lightmap_cluster_size: int = 2
var lighting_outputs: Array[Terrain3DRegionWorkbenchLightingOutput] = []
var last_archive_directory: String = ""
var last_region_node_directory: String = ""
var streamed_node_count: int = 0
var committed_unix: int = 0


static func from_value(p_value: Variant) -> Terrain3DRegionWorkbenchManifest:
	if not p_value is Dictionary:
		return null
	var values := Terrain3DRuntimeValueMap.from_value(p_value)
	if values == null:
		return null
	var manifest := Terrain3DRegionWorkbenchManifest.new()
	manifest.format = values.int_value("format", 2)
	manifest.source_scene = values.string_value("source_scene")
	manifest.source_data_directory = values.string_value("source_data_directory")
	manifest.source_far_atlas_md5 = values.string_value("source_far_atlas_md5")
	manifest.source_region_path = values.string_value("source_region_path")
	manifest.source_region_md5 = values.string_value("source_region_md5")
	manifest.source_page_md5 = values.string_value("source_page_md5")
	manifest.region_location = EditorUtil.vector2i_from_value(values.raw_value("region_location"))
	manifest.region_size = values.int_value("region_size", 128)
	manifest.vertex_spacing = values.float_value("vertex_spacing", 1.0)
	manifest.workbench_data_directory = values.string_value("workbench_data_directory")
	manifest.workbench_context_directory = values.string_value("workbench_context_directory")
	manifest.workbench_region_path = values.string_value("workbench_region_path")
	manifest.workbench_scene = values.string_value("workbench_scene")
	manifest.created_unix = values.int_value("created_unix")
	var property_values := values.map_value("terrain_properties")
	if property_values != null:
		manifest.terrain_properties = property_values.duplicate_values()
	var resource_values := values.map_value("terrain_resources")
	if resource_values != null:
		for key: String in resource_values.keys():
			manifest.terrain_resources[key] = resource_values.string_value(key)
	var raw_regions: Variant = values.raw_value("regions")
	if raw_regions is Array:
		for raw_entry: Variant in raw_regions:
			var entry := Terrain3DRegionWorkbenchRegion.from_value(raw_entry)
			if entry != null:
				manifest.regions.push_back(entry)
	var raw_context_regions: Variant = values.raw_value("context_regions")
	if raw_context_regions is Array:
		for raw_entry: Variant in raw_context_regions:
			var entry := Terrain3DRegionWorkbenchRegion.from_value(raw_entry)
			if entry != null:
				manifest.context_regions.push_back(entry)
	manifest.lightmap_cluster_size = maxi(1, values.int_value("lightmap_cluster_size", 2))
	var raw_lighting_outputs: Variant = values.raw_value("lighting_outputs")
	if raw_lighting_outputs is Array:
		for raw_output: Variant in raw_lighting_outputs:
			var output := Terrain3DRegionWorkbenchLightingOutput.from_value(raw_output)
			if output != null:
				manifest.lighting_outputs.push_back(output)
	# Version-2 single-region manifests remain usable and are promoted in memory.
	if manifest.regions.is_empty() and not manifest.source_region_path.is_empty():
		var legacy := Terrain3DRegionWorkbenchRegion.new()
		legacy.location = manifest.region_location
		legacy.source_region_path = manifest.source_region_path
		legacy.source_region_md5 = manifest.source_region_md5
		legacy.source_page_md5 = manifest.source_page_md5
		legacy.workbench_region_path = manifest.workbench_region_path
		manifest.regions.push_back(legacy)
	manifest.last_archive_directory = values.string_value("last_archive_directory")
	manifest.last_region_node_directory = values.string_value("last_region_node_directory")
	manifest.streamed_node_count = values.int_value("streamed_node_count")
	manifest.committed_unix = values.int_value("committed_unix")
	manifest._sync_primary_fields()
	return manifest




func get_region_entry(location: Vector2i) -> Terrain3DRegionWorkbenchRegion:
	for entry: Terrain3DRegionWorkbenchRegion in regions:
		if entry.location == location:
			return entry
	return null


func get_context_region_entry(location: Vector2i) -> Terrain3DRegionWorkbenchRegion:
	for entry: Terrain3DRegionWorkbenchRegion in context_regions:
		if entry.location == location:
			return entry
	return null


func get_region_locations() -> Array[Vector2i]:
	var result: Array[Vector2i] = []
	result.resize(regions.size())
	for index: int in range(regions.size()):
		result[index] = regions[index].location
	return result


func get_context_region_locations() -> Array[Vector2i]:
	var result: Array[Vector2i] = []
	result.resize(context_regions.size())
	for index: int in range(context_regions.size()):
		result[index] = context_regions[index].location
	return result


func get_all_region_entries() -> Array[Terrain3DRegionWorkbenchRegion]:
	var result: Array[Terrain3DRegionWorkbenchRegion] = []
	result.append_array(regions)
	result.append_array(context_regions)
	return result


func get_lighting_output_for_location(location: Vector2i) -> Terrain3DRegionWorkbenchLightingOutput:
	for output: Terrain3DRegionWorkbenchLightingOutput in lighting_outputs:
		if output.contains(location):
			return output
	return null


func get_lighting_output_for_cluster(cluster_min: Vector2i, cluster_max: Vector2i) -> Terrain3DRegionWorkbenchLightingOutput:
	for output: Terrain3DRegionWorkbenchLightingOutput in lighting_outputs:
		if output.cluster_min == cluster_min and output.cluster_max == cluster_max:
			return output
	return null


func get_next_unstaged_lighting_output() -> Terrain3DRegionWorkbenchLightingOutput:
	for output: Terrain3DRegionWorkbenchLightingOutput in lighting_outputs:
		if not output.is_staged():
			return output
	return null


func _sync_primary_fields() -> void:
	if regions.is_empty():
		return
	var primary: Terrain3DRegionWorkbenchRegion = get_region_entry(region_location)
	if primary == null:
		primary = regions[0]
		region_location = primary.location
	source_region_path = primary.source_region_path
	source_region_md5 = primary.source_region_md5
	source_page_md5 = primary.source_page_md5
	workbench_region_path = primary.workbench_region_path


func to_dictionary() -> Dictionary[String, Variant]:
	_sync_primary_fields()
	var region_values: Array[Variant] = []
	for entry: Terrain3DRegionWorkbenchRegion in regions:
		region_values.push_back(entry.to_dictionary())
	var context_region_values: Array[Variant] = []
	for entry: Terrain3DRegionWorkbenchRegion in context_regions:
		context_region_values.push_back(entry.to_dictionary())
	var lighting_output_values: Array[Variant] = []
	for output: Terrain3DRegionWorkbenchLightingOutput in lighting_outputs:
		lighting_output_values.push_back(output.to_dictionary())
	var result: Dictionary[String, Variant] = {
		"format": 6,
		"source_scene": source_scene,
		"source_data_directory": source_data_directory,
		"source_far_atlas_md5": source_far_atlas_md5,
		"source_region_path": source_region_path,
		"source_region_md5": source_region_md5,
		"source_page_md5": source_page_md5,
		"region_location": EditorUtil.vector2i_to_array(region_location),
		"region_size": region_size,
		"vertex_spacing": vertex_spacing,
		"workbench_data_directory": workbench_data_directory,
		"workbench_context_directory": workbench_context_directory,
		"workbench_region_path": workbench_region_path,
		"workbench_scene": workbench_scene,
		"created_unix": created_unix,
		"terrain_properties": terrain_properties,
		"terrain_resources": terrain_resources,
		"regions": region_values,
		"context_regions": context_region_values,
		"lightmap_cluster_size": lightmap_cluster_size,
		"lighting_outputs": lighting_output_values,
	}
	WorkbenchMetadata.append_checkout_state(
			result, last_archive_directory, last_region_node_directory, streamed_node_count, committed_unix)
	return result
