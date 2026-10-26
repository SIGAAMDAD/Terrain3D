# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Bootstrap for a generated multi-region workbench scene.
@tool
extends Node3D

const WorkbenchMetadata = preload("res://addons/terrain_3d/tools/region_workbench_metadata.gd")

@export_file("*.json") var manifest_path: String


var _initializing: bool = false


func _enter_tree() -> void:
	if Engine.is_editor_hint():
		editor_description = "Isolated Terrain3D workspace. The Terrain3D node loads only checked-out editable regions; neighboring dependency snapshots remain outside its data directory and are used only for cooking/lightmapping. Edit streamed objects under RegionNodes/Region_X_Z, then use Terrain3D > Commit Workbench Regions to publish the batch."
	_ensure_workbench_terrain.call_deferred()


func _ensure_workbench_terrain() -> void:
	if _initializing:
		return
	_initializing = true
	var manifest: Terrain3DRegionWorkbenchManifest = _read_manifest()
	if manifest == null:
		_initializing = false
		return
	if manifest.format < 6:
		push_error("Terrain3D Workbench predates isolated cook-context storage. Reopen it from the source terrain so only selected regions are loaded.")
		_initializing = false
		return
	var terrain := get_node_or_null("Terrain3D") as Terrain3D
	var created_terrain: bool = false
	if not is_instance_valid(terrain):
		terrain = Terrain3D.new()
		created_terrain = true
		terrain.name = "Terrain3D"

		var available_properties: Dictionary[String, bool] = _property_name_set(terrain)
		var properties: Dictionary[String, Variant] = manifest.terrain_properties
		# Region geometry must be configured before data_directory causes Terrain3DData
		# to load. The manifest fields are authoritative even if an older property
		# snapshot omitted these settings.
		if available_properties.has("region_size"):
			terrain.set("region_size", manifest.region_size)
		if available_properties.has("vertex_spacing"):
			terrain.set("vertex_spacing", manifest.vertex_spacing)
		if properties.has("instancer_cell_size") and available_properties.has("instancer_cell_size"):
			terrain.set("instancer_cell_size", properties["instancer_cell_size"])
		terrain.set("data_directory", manifest.workbench_data_directory)
		add_child(terrain)
		if Engine.is_editor_hint():
			terrain.owner = self

		for property_name: String in properties.keys():
			if property_name in ["region_size", "vertex_spacing", "instancer_cell_size"] or not available_properties.has(property_name):
				continue
			terrain.set(property_name, properties[property_name])
		for property_name: String in manifest.terrain_resources.keys():
			if not available_properties.has(property_name):
				continue
			var resource_path: String = manifest.terrain_resources[property_name]
			if not resource_path.is_empty():
				var resource: Resource = load(resource_path)
				if is_instance_valid(resource):
					terrain.set(property_name, resource)
		terrain.set("editor_execution_mode", 0)
		terrain.set("light_target", null)
		terrain.set("collision_target", null)
		terrain.set("clipmap_target", null)

	_ensure_region_center(manifest)
	await _ensure_region_nodes(terrain, manifest)

	if Engine.is_editor_hint():
		EditorInterface.get_selection().clear()
		EditorInterface.get_selection().add_node(terrain)
		if created_terrain:
			EditorInterface.mark_scene_as_unsaved()
		_focus_workbench_view.call_deferred(manifest)
	_initializing = false


func _focus_workbench_view(manifest: Terrain3DRegionWorkbenchManifest) -> void:
	if not Engine.is_editor_hint():
		return
	await get_tree().process_frame
	var marker := get_node_or_null("RegionCenter") as Marker3D
	if not is_instance_valid(marker):
		return
	var viewport: SubViewport = EditorInterface.get_editor_viewport_3d(0)
	if not is_instance_valid(viewport):
		return
	var camera: Camera3D = viewport.get_camera_3d()
	if not is_instance_valid(camera):
		return
	var locations: Array[Vector2i] = manifest.get_region_locations()
	if locations.is_empty():
		return
	var minimum: Vector2i = locations[0]
	var maximum: Vector2i = locations[0]
	for location: Vector2i in locations:
		minimum.x = mini(minimum.x, location.x)
		minimum.y = mini(minimum.y, location.y)
		maximum.x = maxi(maximum.x, location.x)
		maximum.y = maxi(maximum.y, location.y)
	var world_region_size: float = float(manifest.region_size) * manifest.vertex_spacing
	var span_x: float = float(maximum.x - minimum.x + 1) * world_region_size
	var span_z: float = float(maximum.y - minimum.y + 1) * world_region_size
	var distance: float = maxf(maxf(span_x, span_z) * 1.15, world_region_size * 0.9)
	var target: Vector3 = marker.global_position
	# Oblique framing makes sculpted height readable while keeping the complete
	# editable checkout on screen. Terrain coordinates remain untouched.
	camera.global_position = target + Vector3(distance * 0.72, distance * 0.92, distance * 0.72)
	camera.look_at(target, Vector3.UP)
	var terrain := get_node_or_null("Terrain3D") as Terrain3D
	if is_instance_valid(terrain):
		# Bind the same editor camera immediately so the clipmap and residency center
		# move with the viewport before the first editor input event reaches the plugin.
		terrain.set_camera(camera)
		terrain.snap()


func _ensure_region_center(manifest: Terrain3DRegionWorkbenchManifest) -> void:
	if has_node("RegionCenter"):
		return
	var locations: Array[Vector2i] = manifest.get_region_locations()
	if locations.is_empty():
		locations = [manifest.region_location]
	var minimum: Vector2i = locations[0]
	var maximum: Vector2i = locations[0]
	for location: Vector2i in locations:
		minimum.x = mini(minimum.x, location.x)
		minimum.y = mini(minimum.y, location.y)
		maximum.x = maxi(maximum.x, location.x)
		maximum.y = maxi(maximum.y, location.y)
	var marker := Marker3D.new()
	marker.name = "RegionCenter"
	var world_region_size: float = float(manifest.region_size) * manifest.vertex_spacing
	marker.position = Vector3(
		(float(minimum.x + maximum.x + 1) * 0.5) * world_region_size,
		0.0,
		(float(minimum.y + maximum.y + 1) * 0.5) * world_region_size)
	add_child(marker)
	if Engine.is_editor_hint():
		marker.owner = self


func _ensure_region_nodes(terrain: Node3D, manifest: Terrain3DRegionWorkbenchManifest) -> void:
	var region_nodes := get_node_or_null(NodePath(String(WorkbenchMetadata.REGION_NODES_NAME))) as Node3D
	if not is_instance_valid(region_nodes):
		region_nodes = Node3D.new()
		region_nodes.name = WorkbenchMetadata.REGION_NODES_NAME
		region_nodes.editor_description = "Per-region streamed-object ownership. Each Region_X_Z child is positioned at that region's world origin; edit streamed roots inside the owning region container."
		add_child(region_nodes)
		if Engine.is_editor_hint():
			region_nodes.owner = self

	var typed_terrain := terrain as Terrain3D
	var data: Terrain3DData = null
	for _attempt: int in range(8):
		data = typed_terrain.data if is_instance_valid(typed_terrain) else null
		if is_instance_valid(data):
			break
		await get_tree().process_frame
	if not is_instance_valid(data):
		push_error("Unable to initialize Terrain3D data for the region workbench")
		return

	var world_region_size: float = float(manifest.region_size) * manifest.vertex_spacing
	for entry: Terrain3DRegionWorkbenchRegion in manifest.regions:
		var location: Vector2i = entry.location
		var container_name: String = "Region_%d_%d" % [location.x, location.y]
		var region_container := region_nodes.get_node_or_null(NodePath(container_name)) as Node3D
		if is_instance_valid(region_container) and bool(region_container.get_meta(WorkbenchMetadata.REGION_NODES_LOADED_META, false)):
			continue
		if not is_instance_valid(region_container):
			region_container = Node3D.new()
			region_container.name = container_name
			region_container.position = Vector3(
				float(location.x) * world_region_size, 0.0,
				float(location.y) * world_region_size)
			region_container.set_meta(WorkbenchMetadata.REGION_CONTAINER_META, true)
			region_container.set_meta(WorkbenchMetadata.REGION_LOCATION_META, location)
			region_container.editor_description = "Streamed roots owned by terrain region %s. Direct children are serialized back into this region." % location
			region_nodes.add_child(region_container, true)
			if Engine.is_editor_hint():
				region_container.owner = self

		var region: Terrain3DRegion = null
		for _attempt: int in range(8):
			region = data.get_region(location)
			if is_instance_valid(region):
				break
			await get_tree().process_frame
		if not is_instance_valid(region):
			push_error("Unable to load region %s for region-node editing" % location)
			continue

		var descriptors: Array[Terrain3DRegionNodeDescriptor] = \
			Terrain3DRegionNodeDescriptor.list_from_value(region.get_streamed_nodes())
		for descriptor: Terrain3DRegionNodeDescriptor in descriptors:
			var node: Node = _instantiate_streamed_node(descriptor)
			if not is_instance_valid(node):
				node = _make_missing_node_placeholder(descriptor)
			region_container.add_child(node, true)
			node.owner = self
			_set_owner_recursive(node, self)
			if Engine.is_editor_hint():
				set_editable_instance(node, true)
			if node is Node3D:
				(node as Node3D).transform = descriptor.transform
			_apply_descriptor_metadata(node, descriptor)
		region_container.set_meta(WorkbenchMetadata.REGION_NODES_LOADED_META, true)

	region_nodes.set_meta(WorkbenchMetadata.REGION_NODES_LOADED_META, true)
	if Engine.is_editor_hint():
		EditorInterface.mark_scene_as_unsaved()


func _instantiate_streamed_node(descriptor: Terrain3DRegionNodeDescriptor) -> Node:
	var packed_scene: PackedScene = null
	if not descriptor.scene_path.is_empty():
		var resource: Resource = load(descriptor.scene_path)
		packed_scene = resource as PackedScene
	elif is_instance_valid(descriptor.scene):
		packed_scene = descriptor.scene
	if not is_instance_valid(packed_scene):
		return null
	var node: Node = packed_scene.instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	if not node is Node3D:
		if is_instance_valid(node):
			node.free()
		return null
	return node


func _make_missing_node_placeholder(descriptor: Terrain3DRegionNodeDescriptor) -> Node3D:
	var placeholder := Node3D.new()
	placeholder.name = "MissingRegionNode_%d" % descriptor.id
	placeholder.set_meta(WorkbenchMetadata.NODE_META_LOAD_ERROR, true)
	return placeholder


func _apply_descriptor_metadata(node: Node, descriptor: Terrain3DRegionNodeDescriptor) -> void:
	node.set_meta(WorkbenchMetadata.NODE_META_ID, descriptor.id)
	node.set_meta(WorkbenchMetadata.NODE_META_FLAGS, descriptor.flags)
	node.set_meta(WorkbenchMetadata.NODE_META_STATE, descriptor.state.duplicate(true))
	if not descriptor.scene_path.is_empty():
		node.set_meta(WorkbenchMetadata.NODE_META_SOURCE_PATH, descriptor.scene_path)
	node.set_meta(WorkbenchMetadata.NODE_META_BOUNDARY_POLICY, descriptor.boundary_policy)
	node.set_meta(WorkbenchMetadata.NODE_META_COVERAGE_MIN, descriptor.coverage_min)
	node.set_meta(WorkbenchMetadata.NODE_META_COVERAGE_MAX, descriptor.coverage_max)
	node.set_meta(WorkbenchMetadata.NODE_META_LIGHTING_CLASS, descriptor.lighting_class)
	node.set_meta(WorkbenchMetadata.NODE_META_BAKED_LIGHTING_MODE, descriptor.baked_lighting_mode)
	node.set_meta(WorkbenchMetadata.NODE_META_CONTRIBUTE_BAKED_GI, descriptor.contribute_baked_gi)
	node.set_meta(WorkbenchMetadata.NODE_META_RECEIVE_DYNAMIC_DIRECTIONAL, descriptor.receive_dynamic_directional)
	node.set_meta(WorkbenchMetadata.NODE_META_DYNAMIC_SHADOW_ELIGIBLE, descriptor.dynamic_shadow_eligible)
	if not descriptor.shadow_proxy_scene_path.is_empty():
		node.set_meta(WorkbenchMetadata.NODE_META_SHADOW_PROXY_SCENE_PATH, descriptor.shadow_proxy_scene_path)


func _set_owner_recursive(node: Node, scene_owner: Node) -> void:
	for child: Node in node.get_children():
		child.owner = scene_owner
		_set_owner_recursive(child, scene_owner)


func _property_name_set(object: Object) -> Dictionary[String, bool]:
	var names: Dictionary[String, bool] = {}
	for property_info: Dictionary in object.get_property_list():
		names[String(property_info.get("name", ""))] = true
	return names


func _read_manifest() -> Terrain3DRegionWorkbenchManifest:
	if manifest_path.is_empty() or not FileAccess.file_exists(manifest_path):
		push_error("Terrain3D workbench manifest is missing: %s" % manifest_path)
		return null
	var manifest: Terrain3DRegionWorkbenchManifest = Terrain3DRegionWorkbenchManifest.from_value(
		JSON.parse_string(FileAccess.get_file_as_string(manifest_path)))
	if manifest == null:
		push_error("Terrain3D workbench manifest is invalid: %s" % manifest_path)
	return manifest
