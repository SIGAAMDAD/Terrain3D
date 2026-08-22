@tool
class_name Terrain3DLandformPlacements
extends RefCounted

class PlacementGroup extends RefCounted:
	var mesh_id: int = -1
	var transforms: Array[Transform3D] = []

var groups: Array[PlacementGroup] = []


static func from_value(p_value: Variant) -> Terrain3DLandformPlacements:
	var result := Terrain3DLandformPlacements.new()
	if not p_value is Dictionary:
		return result
	var source: Dictionary[Variant, Variant] = {}
	source.assign(p_value)
	for key: Variant in source.keys():
		var transforms_value: Variant = source[key]
		if not transforms_value is Array:
			continue
		var group := PlacementGroup.new()
		group.mesh_id = int(key)
		var source_transforms: Array[Variant] = transforms_value
		for transform_value: Variant in source_transforms:
			if transform_value is Transform3D:
				group.transforms.push_back(transform_value)
		result.groups.push_back(group)
	return result


func is_empty() -> bool:
	return groups.is_empty()


func to_dictionary() -> Dictionary[int, Variant]:
	var result: Dictionary[int, Variant] = {}
	for group: PlacementGroup in groups:
		result[group.mesh_id] = group.transforms
	return result
