@tool
class_name Terrain3DRegionNodeDescriptor
extends RefCounted

const BOUNDARY_POLICY_BOUNDS: int = 1

var id: int = 0
var scene_path: String = ""
var scene: PackedScene
var transform: Transform3D = Transform3D.IDENTITY
var state: Dictionary[StringName, Variant] = {}
var flags: int = 0
var hlod_min_level: int = 1
var boundary_policy: int = BOUNDARY_POLICY_BOUNDS
var coverage_min: Vector2i = Vector2i.ZERO
var coverage_max: Vector2i = Vector2i.ZERO
var lighting_class: int = 2
var baked_lighting_mode: int = 1
var contribute_baked_gi: bool = true
var receive_dynamic_directional: bool = true
var dynamic_shadow_eligible: bool = true
var shadow_proxy_scene_path: String = ""


static func from_value(p_value: Variant) -> Terrain3DRegionNodeDescriptor:
	if not p_value is Dictionary:
		return null
	var values := Terrain3DRuntimeValueMap.from_value(p_value)
	if values == null:
		return null
	var descriptor := Terrain3DRegionNodeDescriptor.new()
	descriptor.id = values.int_value("id")
	descriptor.scene_path = values.string_value("scene_path")
	descriptor.scene = values.object_value("scene") as PackedScene
	descriptor.transform = values.transform3d_value("transform", Transform3D.IDENTITY)
	var state_map := values.map_value("state")
	if state_map != null:
		for key: String in state_map.keys():
			descriptor.state[StringName(key)] = state_map.raw_value(key)
	descriptor.flags = values.int_value("flags")
	descriptor.hlod_min_level = clampi(values.int_value("hlod_min_level", 1), 1, 6)
	descriptor.boundary_policy = values.int_value("boundary_policy", BOUNDARY_POLICY_BOUNDS)
	descriptor.coverage_min = values.vector2i_value("coverage_min", Vector2i.ZERO)
	descriptor.coverage_max = values.vector2i_value("coverage_max", Vector2i.ZERO)
	descriptor.lighting_class = values.int_value("lighting_class", 2)
	descriptor.baked_lighting_mode = values.int_value("baked_lighting_mode", 1)
	descriptor.contribute_baked_gi = values.bool_value("contribute_baked_gi", true)
	descriptor.receive_dynamic_directional = values.bool_value("receive_dynamic_directional", true)
	descriptor.dynamic_shadow_eligible = values.bool_value("dynamic_shadow_eligible", true)
	descriptor.shadow_proxy_scene_path = values.string_value("shadow_proxy_scene_path")
	return descriptor


static func list_from_value(p_value: Variant) -> Array[Terrain3DRegionNodeDescriptor]:
	var result: Array[Terrain3DRegionNodeDescriptor] = []
	if not p_value is Array:
		return result
	var source: Array[Variant] = p_value
	for entry: Variant in source:
		var descriptor := Terrain3DRegionNodeDescriptor.from_value(entry)
		if descriptor != null:
			result.push_back(descriptor)
	return result


static func values_from_list(p_descriptors: Array[Terrain3DRegionNodeDescriptor]) -> Array[Variant]:
	var result: Array[Variant] = []
	result.resize(p_descriptors.size())
	for index: int in range(p_descriptors.size()):
		result[index] = p_descriptors[index].to_dictionary()
	return result


func to_dictionary() -> Dictionary[String, Variant]:
	var result: Dictionary[String, Variant] = {
		"id": id,
		"transform": transform,
		"state": state,
		"flags": flags,
		"hlod_min_level": hlod_min_level,
		"boundary_policy": boundary_policy,
		"coverage_min": coverage_min,
		"coverage_max": coverage_max,
		"lighting_class": lighting_class,
		"baked_lighting_mode": baked_lighting_mode,
		"contribute_baked_gi": contribute_baked_gi,
		"receive_dynamic_directional": receive_dynamic_directional,
		"dynamic_shadow_eligible": dynamic_shadow_eligible,
	}
	if not shadow_proxy_scene_path.is_empty():
		result["shadow_proxy_scene_path"] = shadow_proxy_scene_path
	if not scene_path.is_empty():
		result["scene_path"] = scene_path
	elif is_instance_valid(scene):
		result["scene"] = scene
	return result
