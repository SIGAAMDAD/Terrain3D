@tool
extends RefCounted

const BridgeSceneRules = preload("res://addons/terrain3d_scene_bridge/bridge_scene_rules.gd")

const META_SCENE_PATH: StringName = &"terrain3d_scene_bridge_scene_path"
const META_MESH_PATH: StringName = &"terrain3d_scene_bridge_mesh_path"
const META_ASSET_KEY: StringName = &"terrain3d_scene_bridge_asset_key"
const META_TYPE_NAME: StringName = &"terrain3d_scene_bridge_type_name"
const VISUAL_NODE_NAME: StringName = &"Visual"
const GROUND_NODE_NAME: StringName = &"Ground"
const LANDMARKS_NODE_NAME: StringName = &"Landmarks"


func scan_source(source_root: Node, scene_rules: BridgeSceneRules, progress_callback: Callable = Callable()) -> Dictionary:
	if progress_callback.is_valid():
		progress_callback.call(0.0, "Scanning source hierarchy")

	var visual_root: Node3D = _resolve_visual_root(source_root)
	if visual_root != null:
		return _scan_visual_source(source_root, visual_root, scene_rules, progress_callback)
	return _scan_regional_source(source_root, scene_rules, progress_callback)


func _scan_visual_source(
		scan_root: Node,
		visual_root: Node3D,
		scene_rules: BridgeSceneRules,
		progress_callback: Callable
	) -> Dictionary:
	var source_roots: Array[Dictionary] = []
	var ground: Node = visual_root.get_node_or_null(NodePath(String(GROUND_NODE_NAME)))
	var landmarks: Node = visual_root.get_node_or_null(NodePath(String(LANDMARKS_NODE_NAME)))
	if ground is Node3D:
		source_roots.append({"node": ground, "label": String(GROUND_NODE_NAME)})
	if landmarks is Node3D:
		source_roots.append({"node": landmarks, "label": String(LANDMARKS_NODE_NAME)})
	if source_roots.is_empty():
		return {
			"ok": false,
			"error": "Visual source has neither a Ground nor Landmarks Node3D child.",
		}

	var regions: Array[Dictionary] = []
	for source_entry: Dictionary in source_roots:
		_collect_region_nodes(scan_root, source_entry["node"], scene_rules, regions)
	regions.sort_custom(_sort_region_entry)

	var placements: Array[Dictionary] = []
	var skipped_nodes: Array[NodePath] = []
	var converted_scene_node_paths: Array[NodePath] = []
	var converted_scene_path_set: Dictionary = {}
	var glb_instance_count: int = 0
	var root_total: int = source_roots.size()
	for root_index: int in range(root_total):
		var source_entry: Dictionary = source_roots[root_index]
		var source_node: Node3D = source_entry["node"] as Node3D
		glb_instance_count += _collect_visual_scene_instances(
			scan_root,
			source_node,
			source_node,
			Vector2i.ZERO,
			false,
			String(source_entry["label"]),
			scene_rules,
			placements,
			skipped_nodes,
			converted_scene_node_paths,
			converted_scene_path_set
		)
		if progress_callback.is_valid():
			progress_callback.call(
				0.1 + 0.75 * float(root_index + 1) / float(maxi(root_total, 1)),
				"Scanning Visual/Ground and Visual/Landmarks scene instances"
			)

	if progress_callback.is_valid():
		progress_callback.call(0.9, "Sorting source placements")
	placements.sort_custom(_sort_placement)
	skipped_nodes.sort_custom(_sort_node_path)
	converted_scene_node_paths.sort_custom(_sort_node_path)
	if progress_callback.is_valid():
		progress_callback.call(1.0, "Source scan complete")

	return {
		"ok": true,
		"source_mode": "visual_roots",
		"visual_root_path": scan_root.get_path_to(visual_root),
		"source_scope_count": source_roots.size(),
		"regions": regions,
		"region_count": regions.size(),
		"glb_instance_count": glb_instance_count,
		"placements": placements,
		"mesh_instance_count": placements.size(),
		"skipped_nodes": skipped_nodes,
		"converted_scene_node_paths": converted_scene_node_paths,
	}


func _scan_regional_source(source_root: Node, scene_rules: BridgeSceneRules, progress_callback: Callable) -> Dictionary:
	var regions: Array[Dictionary] = []
	_collect_region_nodes(source_root, source_root, scene_rules, regions)
	regions.sort_custom(_sort_region_entry)
	if regions.is_empty():
		return {
			"ok": false,
			"error": "No Visual/Ground + Visual/Landmarks hierarchy or Region_<X>_<Y> nodes were found under the selected source node.",
		}

	var placements: Array[Dictionary] = []
	var skipped_nodes: Array[NodePath] = []
	var glb_instance_count: int = 0
	var region_total: int = regions.size()
	for region_index: int in range(region_total):
		var region: Dictionary = regions[region_index]
		var region_node: Node = region["node"]
		var region_location: Vector2i = region["location"]
		glb_instance_count += _collect_scene_instances(
			source_root,
			region_node,
			region_node,
			region_location,
			scene_rules,
			placements,
			skipped_nodes
		)
		if progress_callback.is_valid():
			progress_callback.call(
				0.1 + 0.75 * float(region_index + 1) / float(maxi(region_total, 1)),
				"Scanning regional scene instances"
			)

	var skipped_lookup: Dictionary = {}
	for skipped_path: NodePath in skipped_nodes:
		skipped_lookup[skipped_path] = true
	_collect_unscoped_leaves(source_root, source_root, scene_rules, skipped_nodes, skipped_lookup)
	if progress_callback.is_valid():
		progress_callback.call(0.9, "Sorting source placements")
	placements.sort_custom(_sort_placement)
	skipped_nodes.sort_custom(_sort_node_path)
	if progress_callback.is_valid():
		progress_callback.call(1.0, "Source scan complete")

	return {
		"ok": true,
		"source_mode": "regional",
		"regions": regions,
		"region_count": regions.size(),
		"glb_instance_count": glb_instance_count,
		"placements": placements,
		"mesh_instance_count": placements.size(),
		"skipped_nodes": skipped_nodes,
		"converted_scene_node_paths": [],
	}


func get_region_name(location: Vector2i) -> String:
	return "Region_%d_%d" % [location.x, location.y]


func get_or_create_region_node(output_root: Node3D, location: Vector2i) -> Node3D:
	var region_name: String = get_region_name(location)
	var existing: Node = output_root.get_node_or_null(NodePath(region_name))
	if existing is Node3D:
		return existing as Node3D
	var region_node: Node3D = Node3D.new()
	region_node.name = region_name
	region_node.set_meta(&"terrain3d_scene_bridge_region", location)
	output_root.add_child(region_node)
	return region_node


func _resolve_visual_root(source_root: Node) -> Node3D:
	if source_root == null:
		return null
	if source_root is Node3D:
		var direct_ground: Node = source_root.get_node_or_null(NodePath(String(GROUND_NODE_NAME)))
		var direct_landmarks: Node = source_root.get_node_or_null(NodePath(String(LANDMARKS_NODE_NAME)))
		if String(source_root.name) == String(VISUAL_NODE_NAME) and (direct_ground is Node3D or direct_landmarks is Node3D):
			return source_root as Node3D
	var visual: Node = source_root.get_node_or_null(NodePath(String(VISUAL_NODE_NAME)))
	if visual is Node3D:
		var ground: Node = visual.get_node_or_null(NodePath(String(GROUND_NODE_NAME)))
		var landmarks: Node = visual.get_node_or_null(NodePath(String(LANDMARKS_NODE_NAME)))
		if ground is Node3D or landmarks is Node3D:
			return visual as Node3D
	return null


func _collect_region_nodes(
		scan_root: Node,
		current: Node,
		scene_rules: BridgeSceneRules,
		regions: Array[Dictionary]
	) -> void:
	var parsed: Dictionary = scene_rules.parse_region_name(String(current.name))
	if bool(parsed.get("ok", false)):
		regions.append({
			"node": current,
			"location": parsed["location"],
			"node_path": scan_root.get_path_to(current),
		})
	for child: Node in current.get_children():
		_collect_region_nodes(scan_root, child, scene_rules, regions)


func _collect_visual_scene_instances(
		scan_root: Node,
		scope_root: Node3D,
		current: Node,
		region_location: Vector2i,
		has_region: bool,
		source_scope: String,
		scene_rules: BridgeSceneRules,
		placements: Array[Dictionary],
		skipped_nodes: Array[NodePath],
		converted_scene_node_paths: Array[NodePath],
		converted_scene_path_set: Dictionary
	) -> int:
	var scene_instance_count: int = 0
	for child: Node in current.get_children():
		var child_region_location: Vector2i = region_location
		var child_has_region: bool = has_region
		var parsed_region: Dictionary = scene_rules.parse_region_name(String(child.name))
		if bool(parsed_region.get("ok", false)):
			child_region_location = parsed_region["location"]
			child_has_region = true

		if not child is Node3D:
			scene_instance_count += _collect_visual_scene_instances(
				scan_root,
				scope_root,
				child,
				child_region_location,
				child_has_region,
				source_scope,
				scene_rules,
				placements,
				skipped_nodes,
				converted_scene_node_paths,
				converted_scene_path_set
			)
			continue

		var child_3d: Node3D = child as Node3D
		if child_3d is MeshInstance3D and child_3d.has_meta(META_SCENE_PATH) and child_3d.has_meta(META_MESH_PATH):
			var restored_scene_path: String = String(child_3d.get_meta(META_SCENE_PATH, ""))
			var restored_mesh_path: NodePath = NodePath(String(child_3d.get_meta(META_MESH_PATH, "")))
			if bool(scene_rules.is_supported_scene_path(restored_scene_path)):
				var restored_category: int = scene_rules.resolve_instance_category(child_3d, restored_scene_path, scope_root)
				var restored_base_key: String = String(child_3d.get_meta(META_ASSET_KEY, "%s::mesh::%s" % [restored_scene_path, String(restored_mesh_path)]))
				var restored_asset_key: String = scene_rules.asset_key_with_category(restored_base_key, restored_category)
				var restored_type_name: String = String(child_3d.get_meta(META_TYPE_NAME, scene_rules.type_name_from_mesh(restored_scene_path, restored_mesh_path, String(child_3d.name))))
				placements.append({
					"asset_key": restored_asset_key,
					"instance_category": restored_category,
					"scene_path": restored_scene_path,
					"mesh_path": restored_mesh_path,
					"mesh_name": String(child_3d.name),
					"type_name": restored_type_name,
					"transform": child_3d.global_transform,
					"node_path": scan_root.get_path_to(child_3d),
					"source_scene_node_path": scan_root.get_path_to(child_3d),
					"source_scene_transform": child_3d.global_transform,
					"source_region_location": child_region_location if child_has_region else Vector2i.ZERO,
					"source_region_path": _source_region_path(scan_root, scope_root, child_3d, scene_rules),
					"source_scope": source_scope,
				})
				_append_unique_node_path(scan_root.get_path_to(child_3d), converted_scene_node_paths, converted_scene_path_set)
				continue

		var scene_path: String = child_3d.scene_file_path
		if not scene_path.is_empty():
			if bool(scene_rules.is_supported_scene_path(scene_path)):
				scene_instance_count += 1
				var found_mesh_count: int = _collect_mesh_instances_from_scene(
					scan_root,
					scope_root,
					child_3d,
					child_region_location if child_has_region else Vector2i.ZERO,
					scene_path,
					scene_rules,
					placements,
					source_scope
				)
				if found_mesh_count > 0:
					_append_unique_node_path(scan_root.get_path_to(child_3d), converted_scene_node_paths, converted_scene_path_set)
				else:
					skipped_nodes.append(scan_root.get_path_to(child_3d))
			else:
				skipped_nodes.append(scan_root.get_path_to(child_3d))
			# A scene instance is a conversion boundary. Do not recurse into its
			# instantiated children and accidentally import one GLB more than once.
			continue

		scene_instance_count += _collect_visual_scene_instances(
			scan_root,
			scope_root,
			child_3d,
			child_region_location,
			child_has_region,
			source_scope,
			scene_rules,
			placements,
			skipped_nodes,
			converted_scene_node_paths,
			converted_scene_path_set
		)
	return scene_instance_count


func _collect_scene_instances(
		scan_root: Node,
		region_root: Node,
		current: Node,
		region_location: Vector2i,
		scene_rules: BridgeSceneRules,
		placements: Array[Dictionary],
		skipped_nodes: Array[NodePath]
	) -> int:
	var scene_instance_count: int = 0
	for child: Node in current.get_children():
		if child != region_root:
			var nested_region: Dictionary = scene_rules.parse_region_name(String(child.name))
			if bool(nested_region.get("ok", false)):
				continue

		if not child is Node3D:
			if child.get_child_count() == 0:
				skipped_nodes.append(scan_root.get_path_to(child))
			scene_instance_count += _collect_scene_instances(
				scan_root,
				region_root,
				child,
				region_location,
				scene_rules,
				placements,
				skipped_nodes
			)
			continue

		var child_3d: Node3D = child as Node3D
		if child_3d is MeshInstance3D and child_3d.has_meta(META_SCENE_PATH) and child_3d.has_meta(META_MESH_PATH):
			var restored_scene_path: String = String(child_3d.get_meta(META_SCENE_PATH, ""))
			var restored_mesh_path: NodePath = NodePath(String(child_3d.get_meta(META_MESH_PATH, "")))
			if bool(scene_rules.is_supported_scene_path(restored_scene_path)):
				var restored_category: int = scene_rules.resolve_instance_category(child_3d, restored_scene_path, region_root)
				var restored_base_key: String = String(child_3d.get_meta(META_ASSET_KEY, "%s::mesh::%s" % [restored_scene_path, String(restored_mesh_path)]))
				var restored_asset_key: String = scene_rules.asset_key_with_category(restored_base_key, restored_category)
				var restored_type_name: String = String(child_3d.get_meta(META_TYPE_NAME, scene_rules.type_name_from_mesh(restored_scene_path, restored_mesh_path, String(child_3d.name))))
				placements.append({
					"asset_key": restored_asset_key,
					"instance_category": restored_category,
					"scene_path": restored_scene_path,
					"mesh_path": restored_mesh_path,
					"mesh_name": String(child_3d.name),
					"type_name": restored_type_name,
					"transform": child_3d.global_transform,
					"node_path": scan_root.get_path_to(child_3d),
					"source_scene_node_path": scan_root.get_path_to(child_3d),
					"source_scene_transform": child_3d.global_transform,
					"source_region_location": region_location,
					"source_region_path": scan_root.get_path_to(region_root),
				})
				continue

		var scene_path: String = child_3d.scene_file_path
		if bool(scene_rules.is_supported_scene_path(scene_path)):
			scene_instance_count += 1
			var found_mesh_count: int = _collect_mesh_instances_from_scene(
				scan_root,
				region_root,
				child_3d,
				region_location,
				scene_path,
				scene_rules,
				placements
			)
			if found_mesh_count == 0:
				skipped_nodes.append(scan_root.get_path_to(child_3d))
			continue

		if child_3d.get_child_count() == 0:
			skipped_nodes.append(scan_root.get_path_to(child_3d))
		scene_instance_count += _collect_scene_instances(
			scan_root,
			region_root,
			child_3d,
			region_location,
			scene_rules,
			placements,
			skipped_nodes
		)
	return scene_instance_count


func _collect_mesh_instances_from_scene(
		scan_root: Node,
		region_root: Node,
		scene_root: Node3D,
		region_location: Vector2i,
		scene_path: String,
		scene_rules: BridgeSceneRules,
		placements: Array[Dictionary],
		source_scope: String = ""
	) -> int:
	var mesh_nodes: Array[MeshInstance3D] = []
	_collect_mesh_nodes(scene_root, mesh_nodes)
	var instance_category: int = scene_rules.resolve_instance_category(scene_root, scene_path, region_root)
	# Non-LOD multi-mesh GLBs are one logical object, not an accidental LOD chain
	# and not N unrelated Terrain3D asset IDs. Preserve the scene-root transform and
	# let the bake build one compound mesh asset from all components.
	if mesh_nodes.size() > 1 and not scene_rules.has_explicit_lod_meshes(mesh_nodes):
		placements.append({
			"asset_key": scene_rules.compound_asset_key(scene_path, instance_category),
			"instance_category": instance_category,
			"scene_path": scene_path,
			"mesh_path": NodePath(""),
			"mesh_name": String(scene_root.name),
			"type_name": scene_rules.type_name_from_path(scene_path),
			"compound_scene": true,
			"transform": scene_root.global_transform,
			"node_path": scan_root.get_path_to(scene_root),
			"source_scene_node_path": scan_root.get_path_to(scene_root),
			"source_scene_transform": scene_root.global_transform,
			"source_region_location": region_location,
			"source_region_path": scan_root.get_path_to(region_root),
			"source_scope": source_scope,
		})
		return 1

	var mesh_entries: Array[Dictionary] = []
	for mesh_node: MeshInstance3D in mesh_nodes:
		mesh_entries.append({
			"node": mesh_node,
			"path": _relative_mesh_path(scene_root, mesh_node),
		})
	mesh_entries.sort_custom(_sort_mesh_entry)
	for mesh_entry: Dictionary in mesh_entries:
		var mesh_node: MeshInstance3D = mesh_entry["node"]
		var mesh_path: NodePath = mesh_entry["path"]
		var asset_key: String = String(scene_rules.mesh_asset_key(scene_path, mesh_path, instance_category))
		var type_name: String = String(scene_rules.type_name_from_mesh(scene_path, mesh_path, String(mesh_node.name)))
		placements.append({
			"asset_key": asset_key,
			"instance_category": instance_category,
			"scene_path": scene_path,
			"mesh_path": mesh_path,
			"mesh_name": String(mesh_node.name),
			"type_name": type_name,
			"transform": mesh_node.global_transform,
			"node_path": scan_root.get_path_to(mesh_node),
			"source_scene_node_path": scan_root.get_path_to(scene_root),
			"source_scene_transform": scene_root.global_transform,
			"source_region_location": region_location,
			"source_region_path": scan_root.get_path_to(region_root),
			"source_scope": source_scope,
		})
	return mesh_nodes.size()


func _source_region_path(scan_root: Node, scope_root: Node, current: Node, scene_rules: BridgeSceneRules) -> NodePath:
	var cursor: Node = current
	while cursor != null and cursor != scope_root:
		var parsed: Dictionary = scene_rules.parse_region_name(String(cursor.name))
		if bool(parsed.get("ok", false)):
			return scan_root.get_path_to(cursor)
		cursor = cursor.get_parent()
	return scan_root.get_path_to(scope_root)


func _append_unique_node_path(path: NodePath, output: Array[NodePath], lookup: Dictionary) -> void:
	if lookup.has(path):
		return
	lookup[path] = true
	output.append(path)


func _collect_mesh_nodes(current: Node, mesh_nodes: Array[MeshInstance3D]) -> void:
	if current is MeshInstance3D:
		mesh_nodes.append(current as MeshInstance3D)
	for child: Node in current.get_children():
		_collect_mesh_nodes(child, mesh_nodes)


func _relative_mesh_path(scene_root: Node, mesh_node: Node) -> NodePath:
	if mesh_node == scene_root:
		return NodePath(".")
	return scene_root.get_path_to(mesh_node)


func _collect_unscoped_leaves(
		scan_root: Node,
		current: Node,
		scene_rules: BridgeSceneRules,
		skipped_nodes: Array[NodePath],
		skipped_lookup: Dictionary
	) -> void:
	var current_region: Dictionary = scene_rules.parse_region_name(String(current.name))
	if bool(current_region.get("ok", false)):
		return
	for child: Node in current.get_children():
		var child_region: Dictionary = scene_rules.parse_region_name(String(child.name))
		if bool(child_region.get("ok", false)):
			continue
		if child.get_child_count() == 0:
			var path: NodePath = scan_root.get_path_to(child)
			if not skipped_lookup.has(path):
				skipped_lookup[path] = true
				skipped_nodes.append(path)
			continue
		_collect_unscoped_leaves(scan_root, child, scene_rules, skipped_nodes, skipped_lookup)


func _sort_mesh_entry(a: Dictionary, b: Dictionary) -> bool:
	return String(a["path"]) < String(b["path"])


func _sort_region_entry(a: Dictionary, b: Dictionary) -> bool:
	var location_a: Vector2i = a["location"]
	var location_b: Vector2i = b["location"]
	if location_a.y == location_b.y:
		if location_a.x == location_b.x:
			return String(a["node_path"]) < String(b["node_path"])
		return location_a.x < location_b.x
	return location_a.y < location_b.y


func _sort_placement(a: Dictionary, b: Dictionary) -> bool:
	var region_a: Vector2i = a["source_region_location"]
	var region_b: Vector2i = b["source_region_location"]
	if region_a != region_b:
		if region_a.y == region_b.y:
			return region_a.x < region_b.x
		return region_a.y < region_b.y
	var scene_a: String = String(a["scene_path"])
	var scene_b: String = String(b["scene_path"])
	if scene_a != scene_b:
		return scene_a < scene_b
	var mesh_a: String = String(a.get("mesh_path", NodePath(".")))
	var mesh_b: String = String(b.get("mesh_path", NodePath(".")))
	if mesh_a != mesh_b:
		return mesh_a < mesh_b
	return String(a["node_path"]) < String(b["node_path"])


func _sort_node_path(a: NodePath, b: NodePath) -> bool:
	return String(a) < String(b)
