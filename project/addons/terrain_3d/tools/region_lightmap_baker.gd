# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Builds a temporary, region-scoped LightmapGI hierarchy from the active Terrain3D
# scene and publishes one independent lighting package per authored terrain region.
@tool
class_name Terrain3DRegionLightmapBaker
extends Node

const BAKE_WORLD_NAME: StringName = &"Terrain3DLightmapBakeWorld"
const LIGHTMAP_NAME: StringName = &"Terrain3DRegionalLightmapGI"
const STREAM_HOST_NAME: StringName = &"Terrain3DNodeStreamHost"
const LIGHTING_CONTEXT_NAME: StringName = &"Terrain3DBakeLightingContext"
const GEOMETRY_CONTEXT_NAME: StringName = &"Terrain3DBakeGeometryContext"
const NODE_META_BAKED_LIGHTING_MODE: StringName = &"terrain3d_baked_lighting_mode"
const NODE_META_CONTRIBUTE_BAKED_GI: StringName = &"terrain3d_contribute_baked_gi"
const BAKED_LIGHTMAP: int = 1
const SHADOWMASK_OVERLAY: int = 2
const DEFAULT_PROXY_LOD: int = 2
const DEFAULT_CONTEXT_RADIUS: int = 1
const DEFAULT_TEXEL_SIZE: float = 1.0
const META_REGION_LOCATION: StringName = &"terrain3d_lightmap_region_location"
const META_PROXY_LOD: StringName = &"terrain3d_lightmap_proxy_lod"
const META_CONTEXT_RADIUS: StringName = &"terrain3d_lightmap_context_radius"
const META_SHADOWMASK_MODE: StringName = &"terrain3d_lightmap_shadowmask_mode"
const META_FINAL_DATA_PATH: StringName = &"terrain3d_lightmap_data_path"
const META_PACKAGE_PATH: StringName = &"terrain3d_lightmap_package_path"
const META_CONTRIBUTOR_COUNT: StringName = &"terrain3d_lightmap_contributor_count"
const META_RECEIVER_COUNT: StringName = &"terrain3d_lightmap_receiver_count"
const META_SHADOWMASK_EXPECTED: StringName = &"terrain3d_lightmap_shadowmask_expected"
const META_BAKE_SIGNATURE: StringName = &"terrain3d_lightmap_bake_signature"

var plugin: EditorPlugin
var _dialog: ConfirmationDialog
var _region_list: ItemList
var _status_label: Label
var _available_locations: Array[Vector2i] = []


func prepare_active_bake() -> bool:
	return open_dialog()


func open_dialog() -> bool:
	var terrain: Terrain3D = _get_active_terrain()
	if not _validate_source_terrain(terrain):
		return false
	_ensure_dialog()
	_populate_region_list(terrain)
	if _available_locations.is_empty():
		push_error("The selected Terrain3D data set has no authored regions to bake")
		return false
	var location: Vector2i = _get_default_region_location(terrain)
	_select_location(location)
	_dialog.popup_centered(Vector2i(620, 520))
	return true


func prepare_region_bake(location: Vector2i) -> bool:
	var root: Node = EditorInterface.get_edited_scene_root()
	var terrain: Terrain3D = _get_active_terrain()
	if not is_instance_valid(root) or not _validate_source_terrain(terrain):
		return false
	if not is_instance_valid(terrain.data.get_region(location)):
		push_error("Terrain3D region %s does not exist in the active data set" % location)
		return false
	if root.scene_file_path.is_empty():
		push_error("Save the Terrain3D scene before preparing a regional lightmap bake")
		return false
	var save_error: Error = EditorInterface.save_scene()
	if save_error != OK:
		push_error("Save the Terrain3D scene before preparing its lightmap bake: %s" % error_string(save_error))
		return false

	_remove_existing_bake_world(root)
	var output_paths: Dictionary = _region_output_paths(terrain, location)
	var output_dir: String = String(output_paths["output_dir"])
	var mkdir_error: Error = DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(output_dir))
	if mkdir_error != OK:
		push_error("Unable to create Terrain3D lighting directory %s: %s" % [output_dir, error_string(mkdir_error)])
		return false
	var light_data_path: String = String(output_paths["data"])
	_remove_file_if_present(light_data_path)

	var bake_world := Node3D.new()
	bake_world.name = BAKE_WORLD_NAME
	bake_world.set_meta(META_REGION_LOCATION, location)
	bake_world.set_meta(META_PROXY_LOD, DEFAULT_PROXY_LOD)
	bake_world.set_meta(META_CONTEXT_RADIUS, DEFAULT_CONTEXT_RADIUS)
	bake_world.set_meta(META_SHADOWMASK_MODE, SHADOWMASK_OVERLAY)
	bake_world.set_meta(META_FINAL_DATA_PATH, light_data_path)
	bake_world.set_meta(META_PACKAGE_PATH, String(output_paths["package"]))
	root.add_child(bake_world)
	bake_world.owner = root
	bake_world.global_transform = terrain.global_transform

	var lightmap := LightmapGI.new()
	lightmap.name = LIGHTMAP_NAME
	lightmap.directional = true
	lightmap.environment_mode = LightmapGI.ENVIRONMENT_MODE_SCENE
	lightmap.quality = LightmapGI.BAKE_QUALITY_HIGH
	lightmap.shadowmask_mode = SHADOWMASK_OVERLAY
	bake_world.add_child(lightmap)
	lightmap.owner = root

	var light_data := LightmapGIData.new()
	var data_save_error: Error = ResourceSaver.save(light_data, light_data_path)
	if data_save_error != OK:
		push_error("Unable to create regional LightmapGIData: %s" % error_string(data_save_error))
		_remove_existing_bake_world(root)
		return false
	lightmap.light_data = light_data

	var proxy_mesh: Mesh = terrain.bake_region_lightmap_mesh(
		location, DEFAULT_PROXY_LOD, DEFAULT_CONTEXT_RADIUS, DEFAULT_TEXEL_SIZE)
	if not is_instance_valid(proxy_mesh):
		push_error("Terrain3D did not generate a lightmap proxy mesh for region %s" % location)
		_remove_existing_bake_world(root)
		return false
	var proxy := MeshInstance3D.new()
	proxy.name = "Terrain3DBakeProxy"
	proxy.mesh = proxy_mesh
	proxy.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	proxy.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	bake_world.add_child(proxy)
	proxy.owner = root

	var host := Node3D.new()
	host.name = STREAM_HOST_NAME
	bake_world.add_child(host)
	host.owner = root
	var world_region_size: float = float(terrain.region_size) * terrain.vertex_spacing
	var region_root := Node3D.new()
	region_root.name = "Region_%d_%d" % [location.x, location.y]
	region_root.position = Vector3(
		float(location.x) * world_region_size, 0.0,
		float(location.y) * world_region_size)
	host.add_child(region_root)
	region_root.owner = root
	var receiver_count: int = _populate_region_receivers(region_root, terrain, location)

	var lighting_context: Dictionary = _copy_scene_lighting_context(bake_world, root, terrain)
	var geometry_context_count: int = _populate_geometry_context(
		bake_world, terrain, location, DEFAULT_CONTEXT_RADIUS)
	bake_world.set_meta(META_CONTRIBUTOR_COUNT,
		int(lighting_context.get("count", 0)) + geometry_context_count + 1)
	bake_world.set_meta(META_RECEIVER_COUNT, receiver_count)
	bake_world.set_meta(META_SHADOWMASK_EXPECTED,
		bool(lighting_context.get("shadowmask_expected", false)))
	bake_world.set_meta(META_BAKE_SIGNATURE, _build_bake_signature(
		root, terrain, location, DEFAULT_PROXY_LOD, DEFAULT_CONTEXT_RADIUS,
		DEFAULT_TEXEL_SIZE, SHADOWMASK_OVERLAY))

	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(lightmap)
	if Engine.has_singleton("TerrainLightmapBakeBridge"):
		var bridge: Object = Engine.get_singleton("TerrainLightmapBakeBridge")
		if bridge.has_method("bake_lightmap"):
			var result: Variant = bridge.call("bake_lightmap", lightmap)
			if result is int and int(result) != OK:
				push_error("TerrainLightmapBakeBridge failed for region %s: %s" % [location, error_string(int(result))])
				return false
			print("Terrain3D: region %s lightmap submitted through TerrainLightmapBakeBridge" % location)
			return true

	print("Terrain3D: prepared lightmap bake for region %s in the active Terrain3D scene. Run Godot's Bake Lightmaps command, then Finalize Region Lightmap Bake." % location)
	return true


func finalize_active_bake() -> bool:
	var root: Node = EditorInterface.get_edited_scene_root()
	var terrain: Terrain3D = _get_active_terrain()
	if not is_instance_valid(root) or not _validate_source_terrain(terrain):
		return false
	var bake_world := root.get_node_or_null(NodePath(String(BAKE_WORLD_NAME))) as Node3D
	if not is_instance_valid(bake_world):
		push_error("No prepared Terrain3D regional lightmap bake exists in the active scene")
		return false
	var lightmap := bake_world.get_node_or_null(NodePath(String(LIGHTMAP_NAME))) as LightmapGI
	if not is_instance_valid(lightmap) or not is_instance_valid(lightmap.light_data):
		push_error("The prepared LightmapGI has no baked LightmapGIData")
		return false
	var light_data: LightmapGIData = lightmap.light_data
	if light_data.lightmap_textures.is_empty():
		push_error("LightmapGIData contains no lightmap textures. Run Godot's Bake Lightmaps command first.")
		return false

	var location: Vector2i = bake_world.get_meta(META_REGION_LOCATION, Vector2i(9999, 9999))
	var region: Terrain3DRegion = terrain.data.get_region(location)
	if not is_instance_valid(region):
		push_error("Terrain3D region %s disappeared after the bake was prepared" % location)
		return false
	var raw_user_data: Array = light_data.call("_get_user_data")
	if (raw_user_data.size() % 4) != 0:
		push_error("LightmapGIData returned malformed baked-user metadata")
		return false
	var filtered_result: Dictionary = _filter_baked_user_data(raw_user_data, PackedStringArray([
		"Terrain3DBakeProxy", String(GEOMETRY_CONTEXT_NAME)]))
	var baked_user_data: Array = filtered_result["users"]
	var removed_temporary_users: int = int(filtered_result["removed"])
	_rebuild_lightmap_users(light_data, baked_user_data)

	var final_data_path: String = String(bake_world.get_meta(META_FINAL_DATA_PATH, ""))
	if final_data_path.is_empty():
		push_error("The prepared bake has no final LightmapGIData path")
		return false
	var light_data_save_error: Error = ResourceSaver.save(light_data, final_data_path)
	if light_data_save_error != OK:
		push_error("Unable to publish regional LightmapGIData: %s" % error_string(light_data_save_error))
		return false

	var package := ClassDB.instantiate("Terrain3DRegionLighting") as Terrain3DRegionLighting
	if not is_instance_valid(package):
		push_error("Terrain3DRegionLighting is not registered. Rebuild/reload the Terrain3D extension.")
		return false
	package.set("light_data_path", final_data_path)
	package.set("baked_user_data", baked_user_data)
	package.set("cluster_location", location)
	package.set("region_min", location)
	package.set("region_max", location)
	package.set("cluster_size_regions", 1)
	package.set("terrain_proxy_lod", int(bake_world.get_meta(META_PROXY_LOD, DEFAULT_PROXY_LOD)))
	package.set("context_radius_regions", int(bake_world.get_meta(META_CONTEXT_RADIUS, DEFAULT_CONTEXT_RADIUS)))
	package.set("shadowmask_mode", int(lightmap.shadowmask_mode))
	package.set("bake_signature", int(bake_world.get_meta(META_BAKE_SIGNATURE, 0)))
	package.set("baked_unix", int(Time.get_unix_time_from_system()))
	package.set("receiver_count", int(baked_user_data.size() / 4))
	package.set("contributor_count", int(bake_world.get_meta(META_CONTRIBUTOR_COUNT, 1)))
	package.set("lightmap_texture_count", light_data.lightmap_textures.size())
	package.set("shadowmask_texture_count", light_data.shadowmask_textures.size())
	var probe_data: Dictionary = light_data.call("_get_probe_data")
	var probe_points: PackedVector3Array = probe_data.get("points", PackedVector3Array())
	package.set("has_probe_data", not probe_points.is_empty())
	package.set("estimated_gpu_bytes", _estimate_baked_texture_bytes(light_data))
	package.set("disk_bytes", _resource_file_bytes(final_data_path))
	package.set("source_scene", root.scene_file_path)

	var shadowmask_count: int = light_data.shadowmask_textures.size()
	var shadowmask_expected: bool = bool(bake_world.get_meta(META_SHADOWMASK_EXPECTED, false))
	if shadowmask_expected and int(lightmap.shadowmask_mode) != 0 and shadowmask_count == 0:
		push_error("Regional bake expected a directional shadowmask, but LightmapGIData contains no shadowmask textures")
		return false

	var package_path: String = String(bake_world.get_meta(META_PACKAGE_PATH, ""))
	if package_path.is_empty():
		push_error("The prepared bake has no regional lighting package path")
		return false
	var package_error: Error = ResourceSaver.save(package, package_path)
	if package_error != OK:
		push_error("Unable to save regional lighting package: %s" % error_string(package_error))
		return false

	region.set_lighting(package)
	terrain.data.save_region(location, terrain.data_directory, terrain.save_16_bit)
	bake_world.queue_free()
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(terrain)
	print("Terrain3D: published per-region lighting %s for region %s (%d lightmaps, %d shadowmasks, %d receivers, %d temporary users stripped)" % [
		package_path, location, light_data.lightmap_textures.size(), shadowmask_count,
		baked_user_data.size() / 4, removed_temporary_users])
	return true


func _ensure_dialog() -> void:
	if is_instance_valid(_dialog):
		return
	_dialog = ConfirmationDialog.new()
	_dialog.title = "Prepare Terrain3D Region Lightmap Bake"
	_dialog.ok_button_text = "Prepare Bake"
	_dialog.cancel_button_text = "Cancel"
	_dialog.min_size = Vector2i(560, 460)
	_dialog.confirmed.connect(_on_dialog_confirmed)
	add_child(_dialog)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 12)
	margin.add_theme_constant_override("margin_right", 12)
	margin.add_theme_constant_override("margin_top", 8)
	margin.add_theme_constant_override("margin_bottom", 8)
	_dialog.add_child(margin)
	var content := VBoxContainer.new()
	content.add_theme_constant_override("separation", 8)
	margin.add_child(content)
	var explanation := Label.new()
	explanation.text = "Choose one authored terrain region. Terrain3D builds an isolated bake subtree inside the current scene, uses neighboring regions only as lighting context, and publishes one independent lighting package for the selected region."
	explanation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	content.add_child(explanation)
	_region_list = ItemList.new()
	_region_list.select_mode = ItemList.SELECT_SINGLE
	_region_list.allow_reselect = true
	_region_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_region_list.custom_minimum_size = Vector2(500, 300)
	_region_list.item_selected.connect(_on_region_selected)
	content.add_child(_region_list)
	_status_label = Label.new()
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status_label.custom_minimum_size = Vector2(500, 48)
	content.add_child(_status_label)


func _populate_region_list(terrain: Terrain3D) -> void:
	_available_locations.clear()
	_available_locations.assign(terrain.data.get_region_locations())
	_available_locations.sort_custom(_region_location_less)
	_region_list.clear()
	for location: Vector2i in _available_locations:
		var region: Terrain3DRegion = terrain.data.get_region(location)
		var suffix: String = ""
		if is_instance_valid(region) and is_instance_valid(region.get_lighting()):
			suffix = "  • baked"
		var index: int = _region_list.add_item("%d, %d%s" % [location.x, location.y, suffix])
		_region_list.set_item_tooltip(index, "Terrain3D region (%d, %d)" % [location.x, location.y])


func _select_location(location: Vector2i) -> void:
	var index: int = _available_locations.find(location)
	if index < 0:
		index = 0
	_region_list.select(index)
	_region_list.ensure_current_is_visible()
	_update_dialog_status(index)


func _on_region_selected(index: int) -> void:
	_update_dialog_status(index)


func _update_dialog_status(index: int) -> void:
	if index < 0 or index >= _available_locations.size():
		_dialog.get_ok_button().disabled = true
		_status_label.text = "Choose an authored region."
		return
	_dialog.get_ok_button().disabled = false
	var terrain: Terrain3D = _get_active_terrain()
	var location: Vector2i = _available_locations[index]
	var region: Terrain3DRegion = terrain.data.get_region(location) if is_instance_valid(terrain) and terrain.data != null else null
	if is_instance_valid(region) and is_instance_valid(region.get_lighting()):
		_status_label.text = "Region %s already has baked lighting. Finalizing this bake replaces that region's lighting package." % location
	else:
		_status_label.text = "Region %s will receive its own .lmbake and Terrain3DRegionLighting package." % location


func _on_dialog_confirmed() -> void:
	var selected: PackedInt32Array = _region_list.get_selected_items()
	if selected.is_empty():
		return
	var index: int = int(selected[0])
	if index < 0 or index >= _available_locations.size():
		return
	prepare_region_bake(_available_locations[index])


func _validate_source_terrain(terrain: Terrain3D) -> bool:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before preparing a regional lightmap bake")
		return false
	if terrain.data == null:
		push_error("The selected Terrain3D node has no loaded terrain data")
		return false
	if terrain.data_directory.is_empty():
		push_error("The selected Terrain3D node has no data directory")
		return false
	return true


func _get_active_terrain() -> Terrain3D:
	if is_instance_valid(plugin) and is_instance_valid(plugin.get("terrain")):
		return plugin.get("terrain") as Terrain3D
	var root: Node = EditorInterface.get_edited_scene_root()
	return _find_terrain(root) if is_instance_valid(root) else null


func _get_default_region_location(terrain: Terrain3D) -> Vector2i:
	if is_instance_valid(plugin):
		var mouse_position: Variant = plugin.get("mouse_global_position")
		if mouse_position is Vector3:
			var mouse_location: Vector2i = terrain.data.get_region_location(mouse_position)
			if is_instance_valid(terrain.data.get_region(mouse_location)):
				return mouse_location
	var locations: Array[Vector2i] = terrain.data.get_region_locations()
	return locations[0] if not locations.is_empty() else Vector2i.ZERO


func _region_output_paths(terrain: Terrain3D, location: Vector2i) -> Dictionary:
	var output_dir: String = terrain.data_directory.trim_suffix("/").path_join("lighting")
	var base_name: String = "region_%d_%d" % [location.x, location.y]
	var bake_token: String = "%d_%d" % [int(Time.get_unix_time_from_system()), Time.get_ticks_msec()]
	return {
		"output_dir": output_dir,
		"data": output_dir.path_join(base_name + "_bake_" + bake_token + ".lmbake"),
		"package": output_dir.path_join(base_name + ".t3dlight.tres"),
	}


func _populate_geometry_context(bake_world: Node3D, terrain: Terrain3D,
		location: Vector2i, context_radius: int) -> int:
	var context := Node3D.new()
	context.name = GEOMETRY_CONTEXT_NAME
	bake_world.add_child(context)
	context.owner = bake_world.owner
	var count: int = 0
	var world_region_size: float = float(terrain.region_size) * terrain.vertex_spacing
	for region_y: int in range(location.y - context_radius, location.y + context_radius + 1):
		for region_x: int in range(location.x - context_radius, location.x + context_radius + 1):
			var context_location := Vector2i(region_x, region_y)
			var region: Terrain3DRegion = terrain.data.get_region(context_location)
			if not is_instance_valid(region):
				continue
			var region_root := Node3D.new()
			region_root.name = "Region_%d_%d" % [context_location.x, context_location.y]
			region_root.position = Vector3(
				float(context_location.x) * world_region_size, 0.0,
				float(context_location.y) * world_region_size)
			context.add_child(region_root)
			region_root.owner = context.owner
			var include_lightmap_receivers: bool = context_location != location
			count += _populate_region_contributors(
				region_root, terrain, context_location, include_lightmap_receivers)
	return count


func _populate_region_contributors(region_root: Node3D, terrain: Terrain3D,
		location: Vector2i, include_lightmap_receivers: bool) -> int:
	var region: Terrain3DRegion = terrain.data.get_region(location)
	if not is_instance_valid(region):
		return 0
	var count: int = 0
	var descriptors: Array[Terrain3DRegionNodeDescriptor] = Terrain3DRegionNodeDescriptor.list_from_value(
		region.get_streamed_nodes())
	for descriptor: Terrain3DRegionNodeDescriptor in descriptors:
		if not descriptor.contribute_baked_gi:
			continue
		if not include_lightmap_receivers and descriptor.baked_lighting_mode == BAKED_LIGHTMAP:
			continue
		var node: Node3D = _instantiate_descriptor(descriptor)
		if not is_instance_valid(node):
			continue
		node.name = "Contributor_%d" % descriptor.id
		_configure_static_lightmap_receiver(node)
		region_root.add_child(node)
		_set_owner_recursive(node, region_root.owner)
		count += 1
	return count


func _populate_region_receivers(region_root: Node3D, terrain: Terrain3D, location: Vector2i) -> int:
	var region: Terrain3DRegion = terrain.data.get_region(location)
	if not is_instance_valid(region):
		return 0
	var count: int = 0
	var descriptors: Array[Terrain3DRegionNodeDescriptor] = Terrain3DRegionNodeDescriptor.list_from_value(
		region.get_streamed_nodes())
	for descriptor: Terrain3DRegionNodeDescriptor in descriptors:
		if descriptor.baked_lighting_mode != BAKED_LIGHTMAP:
			continue
		var node: Node3D = _instantiate_descriptor(descriptor)
		if not is_instance_valid(node):
			continue
		node.name = "Spawn_%d" % descriptor.id
		_configure_static_lightmap_receiver(node)
		region_root.add_child(node)
		_set_owner_recursive(node, region_root.owner)
		count += 1
	return count


func _instantiate_descriptor(descriptor: Terrain3DRegionNodeDescriptor) -> Node3D:
	var packed: PackedScene = descriptor.scene
	if not descriptor.scene_path.is_empty():
		packed = load(descriptor.scene_path) as PackedScene
	if not is_instance_valid(packed):
		return null
	var node: Node = packed.instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	if not node is Node3D:
		if is_instance_valid(node):
			node.free()
		return null
	(node as Node3D).transform = descriptor.transform
	return node as Node3D


func _configure_static_lightmap_receiver(node: Node) -> void:
	if node is GeometryInstance3D:
		var geometry := node as GeometryInstance3D
		geometry.gi_mode = GeometryInstance3D.GI_MODE_STATIC
		geometry.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	for child: Node in node.get_children():
		_configure_static_lightmap_receiver(child)


func _copy_scene_lighting_context(bake_world: Node3D, scene_root: Node,
		terrain: Terrain3D) -> Dictionary:
	var context := Node3D.new()
	context.name = LIGHTING_CONTEXT_NAME
	bake_world.add_child(context)
	context.owner = scene_root
	var state: Dictionary = {
		"count": 0,
		"shadowmask_expected": false,
		"primary_directional_assigned": false,
		"terrain_inverse": terrain.global_transform.affine_inverse(),
		"bake_world": bake_world,
	}
	_copy_lighting_nodes_recursive(scene_root, context, state)
	if not bool(state["shadowmask_expected"]):
		push_warning("Terrain3D: no visible shadow-enabled DirectionalLight3D was found; this regional bake will not produce a shadowmask")
	return {
		"count": int(state["count"]),
		"shadowmask_expected": bool(state["shadowmask_expected"]),
	}


func _copy_lighting_nodes_recursive(source: Node, context: Node3D, state: Dictionary) -> void:
	if source == state["bake_world"]:
		return
	var terrain_inverse: Transform3D = state["terrain_inverse"]
	if source is Light3D:
		var light_clone: Node = source.duplicate()
		if light_clone is Light3D:
			_clear_children(light_clone)
			var clone_light := light_clone as Light3D
			clone_light.transform = terrain_inverse * (source as Light3D).global_transform
			if (clone_light is DirectionalLight3D and clone_light.visible
					and clone_light.shadow_enabled and not bool(state["primary_directional_assigned"])):
				clone_light.light_bake_mode = Light3D.BAKE_DYNAMIC
				state["primary_directional_assigned"] = true
				state["shadowmask_expected"] = true
			context.add_child(clone_light)
			clone_light.owner = context.owner
			state["count"] = int(state["count"]) + 1
		else:
			light_clone.free()
	elif source is WorldEnvironment:
		var environment_clone: Node = source.duplicate()
		if environment_clone is WorldEnvironment:
			_clear_children(environment_clone)
			context.add_child(environment_clone)
			environment_clone.owner = context.owner
			state["count"] = int(state["count"]) + 1
		else:
			environment_clone.free()
	elif source is LightmapProbe:
		var probe_clone: Node = source.duplicate()
		if probe_clone is LightmapProbe:
			_clear_children(probe_clone)
			(probe_clone as LightmapProbe).transform = terrain_inverse * (source as LightmapProbe).global_transform
			context.add_child(probe_clone)
			probe_clone.owner = context.owner
			state["count"] = int(state["count"]) + 1
		else:
			probe_clone.free()
	for child: Node in source.get_children():
		_copy_lighting_nodes_recursive(child, context, state)


func _build_bake_signature(scene_root: Node, terrain: Terrain3D, location: Vector2i,
		proxy_lod: int, context_radius: int, texel_size: float, shadowmask_mode: int) -> int:
	var parts: PackedStringArray = PackedStringArray([
		"terrain3d-region-lightmap-v2",
		str(location),
		str(proxy_lod),
		str(context_radius),
		str(texel_size),
		str(shadowmask_mode),
	])
	if not scene_root.scene_file_path.is_empty() and FileAccess.file_exists(scene_root.scene_file_path):
		parts.append(scene_root.scene_file_path)
		parts.append(FileAccess.get_md5(scene_root.scene_file_path))
	for region_y: int in range(location.y - context_radius, location.y + context_radius + 1):
		for region_x: int in range(location.x - context_radius, location.x + context_radius + 1):
			var context_location := Vector2i(region_x, region_y)
			var region: Terrain3DRegion = terrain.data.get_region(context_location)
			if not is_instance_valid(region):
				continue
			var region_path: String = terrain.data_directory.trim_suffix("/").path_join(
				Terrain3DUtil.location_to_filename(context_location))
			if FileAccess.file_exists(region_path):
				parts.append(region_path)
				parts.append(FileAccess.get_md5(region_path))
			var descriptors: Array[Terrain3DRegionNodeDescriptor] = Terrain3DRegionNodeDescriptor.list_from_value(
				region.get_streamed_nodes())
			for descriptor: Terrain3DRegionNodeDescriptor in descriptors:
				if not descriptor.scene_path.is_empty() and FileAccess.file_exists(descriptor.scene_path):
					parts.append(descriptor.scene_path)
					parts.append(FileAccess.get_md5(descriptor.scene_path))
	var digest: String = "\n".join(parts).md5_text()
	return digest.substr(0, 15).hex_to_int()


func _estimate_baked_texture_bytes(light_data: LightmapGIData) -> int:
	var total: int = 0
	var texture_sets: Array = [light_data.lightmap_textures, light_data.shadowmask_textures]
	for texture_set: Array in texture_sets:
		for value: Variant in texture_set:
			var texture := value as TextureLayered
			if not is_instance_valid(texture):
				continue
			for layer_index: int in range(texture.get_layers()):
				var image: Image = texture.get_layer_data(layer_index)
				if is_instance_valid(image):
					total += image.get_data().size()
	return total


func _filter_baked_user_data(raw: Array, fragments: PackedStringArray) -> Dictionary:
	var kept: Array = []
	var removed: int = 0
	for index: int in range(0, raw.size(), 4):
		var path_text: String = String(raw[index])
		var strip_user: bool = false
		for fragment: String in fragments:
			if path_text.contains(fragment):
				strip_user = true
				break
		if strip_user:
			removed += 1
			continue
		kept.append(raw[index])
		kept.append(raw[index + 1])
		kept.append(raw[index + 2])
		kept.append(raw[index + 3])
	return {"users": kept, "removed": removed}


func _rebuild_lightmap_users(light_data: LightmapGIData, users: Array) -> void:
	light_data.clear_users()
	for index: int in range(0, users.size(), 4):
		light_data.add_user(
			NodePath(users[index]), Rect2(users[index + 1]),
			int(users[index + 2]), int(users[index + 3]))


func _resource_file_bytes(path: String) -> int:
	var file: FileAccess = FileAccess.open(path, FileAccess.READ)
	if file == null:
		return 0
	return file.get_length()


func _remove_existing_bake_world(root: Node) -> void:
	var old: Node = root.get_node_or_null(NodePath(String(BAKE_WORLD_NAME)))
	if is_instance_valid(old):
		root.remove_child(old)
		old.free()


func _remove_file_if_present(path: String) -> Error:
	if path.is_empty() or not FileAccess.file_exists(path):
		return OK
	return DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


func _set_owner_recursive(node: Node, scene_owner: Node) -> void:
	if not is_instance_valid(scene_owner):
		return
	node.owner = scene_owner
	for child: Node in node.get_children():
		_set_owner_recursive(child, scene_owner)


func _clear_children(node: Node) -> void:
	for child: Node in node.get_children():
		node.remove_child(child)
		child.free()


func _find_terrain(node: Node) -> Terrain3D:
	if node is Terrain3D:
		return node as Terrain3D
	for child: Node in node.get_children():
		var found: Terrain3D = _find_terrain(child)
		if is_instance_valid(found):
			return found
	return null


func _region_location_less(a: Vector2i, b: Vector2i) -> bool:
	return a.y < b.y or (a.y == b.y and a.x < b.x)
