@tool
extends Node

## Persistent scene marker for one reversible Terrain3D placement batch.
## owner_id is carried by native instance records so cell repartitioning keeps
## batch identity. Native ownership is transform-origin based; scaled mesh bounds
## may extend outside the owning cell or region.

@export var bridge_id: String = ""
@export var owner_id: int = 0
@export var source_root_name: String = ""
@export var source_scene_path: String = ""
@export var source_parent_path: NodePath = NodePath("")
@export var source_sibling_index: int = -1
@export var source_root_global_transform: Transform3D = Transform3D.IDENTITY
@export var region_count: int = 0
@export var cell_count: int = 0
@export var instancer_cell_size: int = 0
@export var cells_per_region_axis: int = 0
@export var asset_bindings: Array[Dictionary] = []


func get_summary() -> String:
	var instance_count: int = 0
	for binding: Dictionary in asset_bindings:
		instance_count += int(binding.get("instance_count", 0))
	if instancer_cell_size > 0:
		return "%s: %d regions, %d cells @ %d samples, %d types, %d instances" % [
			source_root_name,
			region_count,
			cell_count,
			instancer_cell_size,
			asset_bindings.size(),
			instance_count,
		]
	return "%s: %d regions, %d types, %d instances" % [source_root_name, region_count, asset_bindings.size(), instance_count]
