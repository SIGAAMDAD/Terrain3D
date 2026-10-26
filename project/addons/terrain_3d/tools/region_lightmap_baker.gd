# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Builds a temporary, region-scoped LightmapGI hierarchy from the active Terrain3D
# scene and publishes one independent lighting package per authored terrain region.
@tool
class_name Terrain3DRegionLightmapBaker
extends Node

const TransactionalResourcePublisher = preload("res://addons/terrain_3d/tools/transactional_resource_publisher.gd")
const EditorUtil = preload("res://addons/terrain_3d/src/editor_util.gd")

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
const META_WORKING_DATA_PATH: StringName = &"terrain3d_lightmap_working_data_path"
const META_FINAL_DATA_PATH: StringName = &"terrain3d_lightmap_data_path"
const META_PACKAGE_PATH: StringName = &"terrain3d_lightmap_package_path"
const META_CONTRIBUTOR_COUNT: StringName = &"terrain3d_lightmap_contributor_count"
const META_RECEIVER_COUNT: StringName = &"terrain3d_lightmap_receiver_count"
const META_SHADOWMASK_EXPECTED: StringName = &"terrain3d_lightmap_shadowmask_expected"
const META_BAKE_SIGNATURE: StringName = &"terrain3d_lightmap_bake_signature"
const META_DIRECTIONAL_LIGHT_SIGNATURE: StringName = &"terrain3d_directional_light_signature"
const LIGHTMAP_EDITOR_BAKE_METHOD: StringName = &"_bake"
const LIGHTMAP_USER_DATA_METHOD: StringName = &"_get_user_data"
const LIGHTMAP_PROBE_DATA_METHOD: StringName = &"_get_probe_data"

var plugin: Terrain3DEditorPluginBase


func get_data_directory_regions(terrain: Terrain3D = null) -> Array[Vector2i]:
	var source_terrain: Terrain3D = terrain if is_instance_valid(terrain) else _get_active_terrain()
	var locations: Array[Vector2i] = []
	if not _validate_source_terrain(source_terrain, false):
		return locations
	var directory := DirAccess.open(source_terrain.data_directory)
	if directory == null:
		return locations
	var files: PackedStringArray = directory.get_files()
	files.sort()
	for file_name: String in files:
		if not file_name.begins_with("terrain3d") or not file_name.ends_with(".res"):
			continue
		var location: Vector2i = Terrain3DUtil.filename_to_location(file_name)
		if location == Vector2i(2147483647, 2147483647):
			continue
		locations.append(location)
	locations.sort_custom(EditorUtil.region_location_less)
	return locations


func prepare_output_directory(terrain: Terrain3D = null) -> Dictionary:
	var source_terrain: Terrain3D = terrain if is_instance_valid(terrain) else _get_active_terrain()
	if not _validate_source_terrain(source_terrain):
		return {"ok": false, "path": "", "error": "Terrain3D data is unavailable."}
	var configured: String = source_terrain.region_lightmap_output_directory.strip_edges()
	if configured.is_empty():
		configured = source_terrain.data_directory.trim_suffix("/").path_join("lighting")
		source_terrain.region_lightmap_output_directory = configured
	var normalized: String = ProjectSettings.localize_path(configured.simplify_path()).simplify_path()
	if normalized != "res://":
		normalized = normalized.trim_suffix("/")
	if normalized.is_empty():
		return {"ok": false, "path": "", "error": "The regional lightmap output path is empty."}
	if not normalized.begins_with("res://"):
		return {
			"ok": false,
			"path": normalized,
			"error": "Regional LightmapGI output must be inside the project (res://) so Godot can import the generated lightmap textures.",
		}
	source_terrain.region_lightmap_output_directory = normalized
	var mkdir_error: Error = DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(normalized))
	if mkdir_error != OK:
		return {"ok": false, "path": normalized, "error": error_string(mkdir_error)}
	var probe_path: String = normalized.path_join(".terrain3d-lightmap-write-test.tmp")
	var probe: FileAccess = FileAccess.open(probe_path, FileAccess.WRITE)
	if probe == null:
		return {"ok": false, "path": normalized, "error": "The output directory is not writable."}
	probe.store_8(0)
	probe.close()
	_remove_file_if_present(probe_path)
	_cleanup_interrupted_publications(normalized)
	return {"ok": true, "path": normalized, "error": ""}


func audit_region_packages(terrain: Terrain3D = null) -> Dictionary:
	var source_terrain: Terrain3D = terrain if is_instance_valid(terrain) else _get_active_terrain()
	var report: Dictionary = {
		"expected": 0, "current": 0, "missing": [], "stale_source_signatures": [],
		"incompatible_light_signature": [], "incompatible_shadowmask_mode": [], "orphans": [],
	}
	if not _validate_source_terrain(source_terrain, false):
		return report
	var output: Dictionary = prepare_output_directory(source_terrain)
	if not bool(output.get("ok", false)):
		return report
	var locations: Array[Vector2i] = get_data_directory_regions(source_terrain)
	report["expected"] = locations.size()
	var expected_files: Dictionary = {}
	var scene_root: Node = EditorInterface.get_edited_scene_root()
	var settings: Dictionary = _get_bake_settings(source_terrain)
	var active_light_signature: String = _find_primary_directional_signature(scene_root)
	for location: Vector2i in locations:
		var paths: Dictionary = _region_output_paths(source_terrain, location)
		var package_path: String = String(paths["package"])
		expected_files[package_path.get_file()] = true
		if not FileAccess.file_exists(package_path) or not FileAccess.file_exists(String(paths["data"])):
			(report["missing"] as Array).append(location)
			continue
		var package := ResourceLoader.load(package_path, "Terrain3DRegionLighting", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DRegionLighting
		if not is_instance_valid(package):
			(report["missing"] as Array).append(location)
			continue
		report["current"] = int(report["current"]) + 1
		if package.shadowmask_mode != int(settings["shadowmask_mode"]):
			(report["incompatible_shadowmask_mode"] as Array).append(location)
		var package_light_signature: String = package.directional_light_signature
		if not active_light_signature.is_empty() and package_light_signature != active_light_signature:
			(report["incompatible_light_signature"] as Array).append(location)
		if not _ensure_region_loaded(source_terrain, location):
			(report["stale_source_signatures"] as Array).append(location)
			continue
		var expected_signature: int = _build_bake_signature(
			scene_root, source_terrain, location, int(settings["proxy_lod"]),
			int(settings["context_radius"]), float(settings["texel_size"]), int(settings["shadowmask_mode"]))
		if package.bake_signature != expected_signature:
			(report["stale_source_signatures"] as Array).append(location)
	var output_dir: DirAccess = DirAccess.open(String(output["path"]))
	if output_dir != null:
		for file_name: String in output_dir.get_files():
			if file_name.begins_with("Region_[") and file_name.ends_with(".t3dlight.tres") \
					and not expected_files.has(file_name):
				(report["orphans"] as Array).append(file_name)
	return report


func bake_prepared_with_editor() -> Dictionary:
	var lightmap: LightmapGI = get_prepared_lightmap()
	if not is_instance_valid(lightmap):
		return {"ok": false, "automatic": false, "error": "No prepared LightmapGI exists."}
	if not is_instance_valid(lightmap.light_data):
		return {"ok": false, "automatic": false, "error": "The prepared LightmapGI has no LightmapGIData."}
	var data_path: String = lightmap.light_data.resource_path.simplify_path()
	var expected_path: String = ""
	var bake_world: Node = lightmap.get_parent()
	if is_instance_valid(bake_world):
		expected_path = String(bake_world.get_meta(META_WORKING_DATA_PATH, "")).simplify_path()
	if data_path.is_empty() or not data_path.begins_with("res://") or not FileAccess.file_exists(data_path):
		return {
			"ok": false,
			"automatic": false,
			"error": "The prepared LightmapGIData has no valid res:// bake path. Re-prepare the region before baking.",
		}
	if not expected_path.is_empty() and data_path != expected_path:
		return {
			"ok": false,
			"automatic": false,
			"error": "The prepared LightmapGIData path does not match the Terrain3D working bake path.",
		}
	if not select_prepared_lightmap():
		return {"ok": false, "automatic": false, "error": "Unable to select the prepared LightmapGI."}
	# Selection drives Godot's built-in LightmapGI editor plugin. Give the editor one
	# frame to process the selection before invoking the same bound action as its
	# Bake Lightmaps toolbar button.
	await get_tree().process_frame
	var tree: SceneTree = EditorInterface.get_base_control().get_tree()
	var editor_baker: Node = _find_editor_lightmap_plugin(tree.root)
	if not is_instance_valid(editor_baker):
		return {
			"ok": false,
			"automatic": false,
			"error": "Godot's LightmapGI editor baker could not be located. The prepared region is still selected for manual baking.",
		}
	editor_baker.call(LIGHTMAP_EDITOR_BAKE_METHOD)
	await get_tree().process_frame
	if not is_prepared_bake_complete():
		return {
			"ok": false,
			"automatic": true,
			"error": "Godot finished the bake command without producing lightmap textures. Review the editor bake error before continuing.",
		}
	return {"ok": true, "automatic": true, "error": ""}


func prepare_region_bake(location: Vector2i) -> bool:
	var root: Node = EditorInterface.get_edited_scene_root()
	var terrain: Terrain3D = _get_active_terrain()
	if not is_instance_valid(root) or not _validate_source_terrain(terrain):
		return false
	if not _ensure_region_loaded(terrain, location):
		push_error("Terrain3D region %s exists in the data directory but could not be loaded for baking" % location)
		return false
	if root.scene_file_path.is_empty():
		push_error("Save the Terrain3D scene before preparing a regional lightmap bake")
		return false
	var save_error: Error = EditorInterface.save_scene()
	if save_error != OK:
		push_error("Save the Terrain3D scene before preparing its lightmap bake: %s" % error_string(save_error))
		return false

	_remove_existing_bake_world(root)
	var settings: Dictionary = _get_bake_settings(terrain)
	var proxy_lod: int = int(settings["proxy_lod"])
	var context_radius: int = int(settings["context_radius"])
	var texel_size: float = float(settings["texel_size"])
	var bake_quality: int = int(settings["bake_quality"])
	var shadowmask_mode: int = int(settings["shadowmask_mode"])
	var output_ready: Dictionary = prepare_output_directory(terrain)
	if not bool(output_ready.get("ok", false)):
		push_error("Unable to prepare Terrain3D lighting directory %s: %s" % [
			String(output_ready.get("path", "")), String(output_ready.get("error", "Unknown error"))])
		return false
	var output_paths: Dictionary = _region_output_paths(terrain, location)
	var output_dir: String = String(output_paths["output_dir"])
	var working_data_path: String = String(output_paths["working_data"])
	var light_data_path: String = String(output_paths["data"])
	_remove_file_if_present(working_data_path)

	var bake_world := Node3D.new()
	bake_world.name = BAKE_WORLD_NAME
	bake_world.set_meta(META_REGION_LOCATION, location)
	bake_world.set_meta(META_PROXY_LOD, proxy_lod)
	bake_world.set_meta(META_CONTEXT_RADIUS, context_radius)
	bake_world.set_meta(META_SHADOWMASK_MODE, shadowmask_mode)
	bake_world.set_meta(META_WORKING_DATA_PATH, working_data_path)
	bake_world.set_meta(META_FINAL_DATA_PATH, light_data_path)
	bake_world.set_meta(META_PACKAGE_PATH, String(output_paths["package"]))
	root.add_child(bake_world)
	bake_world.owner = root
	bake_world.global_transform = terrain.global_transform

	var lightmap := LightmapGI.new()
	lightmap.name = LIGHTMAP_NAME
	lightmap.directional = true
	lightmap.environment_mode = LightmapGI.ENVIRONMENT_MODE_SCENE
	lightmap.quality = bake_quality
	lightmap.shadowmask_mode = shadowmask_mode
	bake_world.add_child(lightmap)
	lightmap.owner = root

	var light_data := LightmapGIData.new()
	var data_save_error: Error = ResourceSaver.save(light_data, working_data_path)
	if data_save_error != OK:
		push_error("Unable to create regional LightmapGIData: %s" % error_string(data_save_error))
		_remove_existing_bake_world(root)
		return false
	# ResourceSaver.save() does not make a newly-created Resource adopt the save
	# path. LightmapGI::bake() requires light_data.resource_path when the editor
	# invokes its normal no-argument bake action, otherwise Godot returns
	# BAKE_ERROR_NO_SAVE_PATH and opens its Save dialog instead of baking. Reload
	# the working resource so the in-memory LightmapGIData owns the real res:// path.
	light_data = ResourceLoader.load(
		working_data_path, "LightmapGIData", ResourceLoader.CACHE_MODE_IGNORE) as LightmapGIData
	if not is_instance_valid(light_data) or light_data.resource_path.simplify_path() != working_data_path.simplify_path():
		push_error("Unable to bind regional LightmapGIData to its working bake path: %s" % working_data_path)
		_remove_existing_bake_world(root)
		return false
	lightmap.light_data = light_data

	var proxy_mesh: Mesh = terrain.bake_region_lightmap_mesh(
		location, proxy_lod, context_radius, texel_size)
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
		bake_world, terrain, location, context_radius)
	bake_world.set_meta(META_CONTRIBUTOR_COUNT,
		int(lighting_context.get("count", 0)) + geometry_context_count + 1)
	bake_world.set_meta(META_RECEIVER_COUNT, receiver_count)
	bake_world.set_meta(META_SHADOWMASK_EXPECTED,
		bool(lighting_context.get("shadowmask_expected", false)))
	bake_world.set_meta(META_DIRECTIONAL_LIGHT_SIGNATURE,
		String(lighting_context.get("directional_light_signature", "")))
	bake_world.set_meta(META_BAKE_SIGNATURE, _build_bake_signature(
		root, terrain, location, proxy_lod, context_radius, texel_size, shadowmask_mode))

	select_prepared_lightmap()
	print("Terrain3D: prepared regional lightmap bake for %s (LOD %d, context %d, %.2f m/texel)." % [
		location, proxy_lod, context_radius, texel_size])
	return true


func finalize_prepared_bake() -> bool:
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
	if not light_data.has_method(LIGHTMAP_USER_DATA_METHOD):
		push_error("Godot 4.7 LightmapGIData no longer exposes the internal baked-user payload required by Terrain3D regional lightmaps.")
		return false
	var raw_user_data: Array = light_data.call(LIGHTMAP_USER_DATA_METHOD)
	if (raw_user_data.size() % 4) != 0 or raw_user_data.size() / 4 != light_data.get_user_count():
		push_error("LightmapGIData returned malformed baked-user metadata")
		return false
	var shadowmask_count: int = light_data.shadowmask_textures.size()
	var shadowmask_expected: bool = bool(bake_world.get_meta(META_SHADOWMASK_EXPECTED, false))
	if shadowmask_expected and int(lightmap.shadowmask_mode) != 0 and shadowmask_count == 0:
		push_error("Regional bake expected a directional shadowmask, but LightmapGIData contains no shadowmask textures")
		return false
	var terrain_shadow_mask: Image = null
	if shadowmask_count > 0 and int(lightmap.shadowmask_mode) != 0:
		terrain_shadow_mask = _extract_terrain_shadow_mask(
			light_data, raw_user_data, int(bake_world.get_meta(META_CONTEXT_RADIUS, DEFAULT_CONTEXT_RADIUS)))
		if terrain_shadow_mask == null:
			push_error("Unable to extract the Terrain3D proxy shadowmask from LightmapGIData")
			return false
	var filtered_result: Dictionary = _filter_baked_user_data(raw_user_data, PackedStringArray([
		"Terrain3DBakeProxy", String(GEOMETRY_CONTEXT_NAME)]))
	var baked_user_data: Array = filtered_result["users"]
	var removed_temporary_users: int = int(filtered_result["removed"])
	if _contains_temporary_baked_users(baked_user_data):
		push_error("Regional LightmapGIData still contains temporary Terrain3D bake users after filtering")
		return false
	_rebuild_lightmap_users(light_data, baked_user_data)

	var final_data_path: String = String(bake_world.get_meta(META_FINAL_DATA_PATH, ""))
	if final_data_path.is_empty():
		push_error("The prepared bake has no final LightmapGIData path")
		return false
	var light_data_save_error: Error = _publish_light_data_transactionally(light_data, final_data_path)
	if light_data_save_error != OK:
		push_error("Unable to publish regional LightmapGIData: %s" % error_string(light_data_save_error))
		return false

	var terrain_shadow_path: String = ""
	if terrain_shadow_mask != null:
		terrain_shadow_path = String(_region_output_paths(terrain, location)["terrain_shadow"])
		var shadow_save_error: Error = _publish_image_transactionally(terrain_shadow_mask, terrain_shadow_path)
		if shadow_save_error != OK:
			push_error("Unable to publish Terrain3D terrain shadow mask: %s" % error_string(shadow_save_error))
			return false

	var package := Terrain3DRegionLighting.new()
	if not is_instance_valid(package):
		push_error("Terrain3DRegionLighting is not registered. Rebuild/reload the Terrain3D extension.")
		return false
	package.light_data_path = final_data_path
	package.terrain_shadow_mask_path = terrain_shadow_path
	package.baked_user_data = baked_user_data
	package.cluster_location = location
	package.region_min = location
	package.region_max = location
	package.cluster_size_regions = 1
	package.terrain_proxy_lod = int(bake_world.get_meta(META_PROXY_LOD, DEFAULT_PROXY_LOD))
	package.context_radius_regions = int(bake_world.get_meta(META_CONTEXT_RADIUS, DEFAULT_CONTEXT_RADIUS))
	package.shadowmask_mode = int(lightmap.shadowmask_mode)
	package.bake_signature = int(bake_world.get_meta(META_BAKE_SIGNATURE, 0))
	package.directional_light_signature = String(
		bake_world.get_meta(META_DIRECTIONAL_LIGHT_SIGNATURE, ""))
	package.baked_unix = int(Time.get_unix_time_from_system())
	package.receiver_count = int(baked_user_data.size() / 4)
	package.contributor_count = int(bake_world.get_meta(META_CONTRIBUTOR_COUNT, 1))
	package.lightmap_texture_count = light_data.lightmap_textures.size()
	package.shadowmask_texture_count = light_data.shadowmask_textures.size()
	if not light_data.has_method(LIGHTMAP_PROBE_DATA_METHOD):
		push_error("Godot 4.7 LightmapGIData no longer exposes the internal probe payload required by Terrain3D regional lightmaps.")
		return false
	var probe_data: Dictionary = light_data.call(LIGHTMAP_PROBE_DATA_METHOD)
	var probe_points: PackedVector3Array = probe_data.get("points", PackedVector3Array())
	package.has_probe_data = not probe_points.is_empty()
	package.estimated_gpu_bytes = _estimate_baked_texture_bytes(light_data) + \
		(terrain_shadow_mask.get_data().size() if terrain_shadow_mask != null else 0)
	package.disk_bytes = _resource_file_bytes(final_data_path) + \
		(_resource_file_bytes(terrain_shadow_path) if not terrain_shadow_path.is_empty() else 0)
	package.source_scene = root.scene_file_path

	var package_path: String = String(bake_world.get_meta(META_PACKAGE_PATH, ""))
	if package_path.is_empty():
		push_error("The prepared bake has no regional lighting package path")
		return false
	var package_error: Error = _publish_package_transactionally(package, package_path)
	if package_error != OK:
		push_error("Unable to save regional lighting package: %s" % error_string(package_error))
		return false
	var working_data_path: String = String(bake_world.get_meta(META_WORKING_DATA_PATH, ""))
	if not working_data_path.is_empty() and working_data_path != final_data_path:
		_remove_file_if_present(working_data_path)

	region.set_lighting(package)
	terrain.data.save_region(location, terrain.data_directory, terrain.save_16_bit)
	bake_world.queue_free()
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(terrain)
	print("Terrain3D: published per-region lighting %s for region %s (%d lightmaps, %d shadowmasks, %d receivers, %d temporary users stripped)" % [
		package_path, location, light_data.lightmap_textures.size(), shadowmask_count,
		baked_user_data.size() / 4, removed_temporary_users])
	return true


func get_prepared_region() -> Variant:
	var root: Node = EditorInterface.get_edited_scene_root()
	if not is_instance_valid(root):
		return null
	var bake_world: Node = root.get_node_or_null(NodePath(String(BAKE_WORLD_NAME)))
	if not is_instance_valid(bake_world):
		return null
	var location: Variant = bake_world.get_meta(META_REGION_LOCATION, null)
	return location if location is Vector2i else null


func get_prepared_lightmap() -> LightmapGI:
	var root: Node = EditorInterface.get_edited_scene_root()
	if not is_instance_valid(root):
		return null
	var bake_world: Node = root.get_node_or_null(NodePath(String(BAKE_WORLD_NAME)))
	if not is_instance_valid(bake_world):
		return null
	return bake_world.get_node_or_null(NodePath(String(LIGHTMAP_NAME))) as LightmapGI


func is_prepared_bake_complete() -> bool:
	var lightmap: LightmapGI = get_prepared_lightmap()
	return is_instance_valid(lightmap) and is_instance_valid(lightmap.light_data) \
		and not lightmap.light_data.lightmap_textures.is_empty()


func select_prepared_lightmap() -> bool:
	var lightmap: LightmapGI = get_prepared_lightmap()
	if not is_instance_valid(lightmap):
		return false
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(lightmap)
	return true


func _validate_source_terrain(terrain: Terrain3D, report_error: bool = true) -> bool:
	if not is_instance_valid(terrain):
		if report_error:
			push_error("Select a Terrain3D node before preparing regional lightmaps")
		return false
	if terrain.data == null:
		if report_error:
			push_error("The selected Terrain3D node has no loaded terrain data")
		return false
	if terrain.data_directory.is_empty():
		if report_error:
			push_error("The selected Terrain3D node has no data directory")
		return false
	return true


func _get_active_terrain() -> Terrain3D:
	if is_instance_valid(plugin) and is_instance_valid(plugin.terrain):
		return plugin.terrain
	var root: Node = EditorInterface.get_edited_scene_root()
	return EditorUtil.find_terrain(root) if is_instance_valid(root) else null


func _ensure_region_loaded(terrain: Terrain3D, location: Vector2i) -> bool:
	if is_instance_valid(terrain.data.get_region(location)):
		return true
	terrain.data.load_region(location, terrain.data_directory, true)
	return is_instance_valid(terrain.data.get_region(location))


func _get_bake_settings(terrain: Terrain3D) -> Dictionary:
	return {
		"output_directory": terrain.region_lightmap_output_directory.strip_edges().trim_suffix("/"),
		"proxy_lod": clampi(terrain.region_lightmap_proxy_lod, 0, 8),
		"context_radius": clampi(terrain.region_lightmap_context_radius, 0, 4),
		"texel_size": clampf(terrain.region_lightmap_texel_size, 0.05, 16.0),
		"bake_quality": clampi(terrain.region_lightmap_bake_quality, 0, 3),
		"shadowmask_mode": clampi(terrain.region_lightmap_shadowmask_mode, 0, 2),
	}


func _find_editor_lightmap_plugin(node: Node) -> Node:
	if node.get_class() == "LightmapGIEditorPlugin" and node.has_method(LIGHTMAP_EDITOR_BAKE_METHOD):
		return node
	for child: Node in node.get_children():
		var found: Node = _find_editor_lightmap_plugin(child)
		if is_instance_valid(found):
			return found
	return null


func _region_output_paths(terrain: Terrain3D, location: Vector2i) -> Dictionary:
	var output_dir: String = terrain.region_lightmap_output_directory.strip_edges()
	if output_dir.is_empty():
		output_dir = terrain.data_directory.trim_suffix("/").path_join("lighting")
	output_dir = ProjectSettings.localize_path(output_dir.simplify_path()).simplify_path()
	if output_dir != "res://":
		output_dir = output_dir.trim_suffix("/")
	var base_name: String = "Region_[%d_%d]" % [location.x, location.y]
	return {
		"output_dir": output_dir,
		"working_data": output_dir.path_join(base_name + ".working.lmbake"),
		"data": output_dir.path_join(base_name + ".lmbake"),
		"terrain_shadow": output_dir.path_join(base_name + ".terrain_shadow.res"),
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
		"directional_light_signature": "",
		"terrain_inverse": terrain.global_transform.affine_inverse(),
		"bake_world": bake_world,
	}
	_copy_lighting_nodes_recursive(scene_root, context, state)
	if not bool(state["shadowmask_expected"]):
		push_warning("Terrain3D: no visible shadow-enabled DirectionalLight3D was found; this regional bake will not produce a shadowmask")
	return {
		"count": int(state["count"]),
		"shadowmask_expected": bool(state["shadowmask_expected"]),
		"directional_light_signature": String(state["directional_light_signature"]),
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
				state["directional_light_signature"] = _directional_light_signature(source as DirectionalLight3D)
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


func _directional_light_signature(light: DirectionalLight3D) -> String:
	if not is_instance_valid(light):
		return ""
	var direction: Vector3 = light.global_transform.basis.z.normalized()
	var effective_color: Color = light.light_color * light.light_energy
	return "dir=%.6f,%.6f,%.6f;color=%.6f,%.6f,%.6f,%.6f;shadow=%d" % [
		direction.x, direction.y, direction.z,
		effective_color.r, effective_color.g, effective_color.b, effective_color.a,
		1 if light.shadow_enabled else 0]


func _find_primary_directional_signature(node: Node) -> String:
	if not is_instance_valid(node):
		return ""
	if node is DirectionalLight3D:
		var light := node as DirectionalLight3D
		if light.visible and light.shadow_enabled:
			return _directional_light_signature(light)
	for child: Node in node.get_children():
		var signature: String = _find_primary_directional_signature(child)
		if not signature.is_empty():
			return signature
	return ""


func _contains_temporary_baked_users(users: Array) -> bool:
	for index: int in range(0, users.size(), 4):
		var path_text: String = String(users[index])
		if path_text.contains("Terrain3DBakeProxy") or path_text.contains(String(GEOMETRY_CONTEXT_NAME)):
			return true
	return false


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


func _find_terrain_proxy_user(raw: Array) -> Dictionary:
	for index: int in range(0, raw.size(), 4):
		if String(raw[index]).contains("Terrain3DBakeProxy"):
			return {
				"found": true,
				"uv_scale": Rect2(raw[index + 1]),
				"slice_index": int(raw[index + 2]),
			}
	return {"found": false}


func _shadowmask_slice_image(textures: Array, slice_index: int) -> Image:
	var remaining: int = slice_index
	for value: Variant in textures:
		var texture := value as TextureLayered
		if not is_instance_valid(texture):
			continue
		if remaining < texture.get_layers():
			var image: Image = texture.get_layer_data(remaining)
			if image == null or image.is_empty():
				return null
			var readable: Image = image.duplicate()
			if readable.is_compressed():
				var decompress_error: Error = readable.decompress()
				if decompress_error != OK:
					push_error("Unable to decompress baked shadowmask atlas slice: %s" % error_string(decompress_error))
					return null
			return readable
		remaining -= texture.get_layers()
	return null


func _nearest_power_of_two(value: int) -> int:
	var upper: int = 1
	while upper < max(1, value):
		upper <<= 1
	var lower: int = max(1, upper >> 1)
	return lower if value - lower < upper - value else upper


func _extract_terrain_shadow_mask(light_data: LightmapGIData, raw_users: Array,
		context_radius: int) -> Image:
	var proxy_user: Dictionary = _find_terrain_proxy_user(raw_users)
	if not bool(proxy_user.get("found", false)):
		return null
	var slice_index: int = int(proxy_user.get("slice_index", -1))
	if slice_index < 0:
		return null
	var source: Image = _shadowmask_slice_image(light_data.shadowmask_textures, slice_index)
	if source == null or source.is_empty():
		return null

	# The proxy UV2 is planar across the full context rectangle. LightmapGI's
	# baked user Rect2 maps that UV domain into the flattened shadowmask slice.
	var context_side: int = max(1, context_radius * 2 + 1)
	var owner_uv := Rect2(
		Vector2(float(context_radius), float(context_radius)) / float(context_side),
		Vector2.ONE / float(context_side))
	var uv_scale: Rect2 = proxy_user["uv_scale"]
	var atlas_position: Vector2 = uv_scale.position + owner_uv.position * uv_scale.size
	var atlas_size: Vector2 = owner_uv.size * uv_scale.size
	var image_size := Vector2(float(source.get_width()), float(source.get_height()))
	var x0: int = clampi(int(floor(atlas_position.x * image_size.x)), 0, source.get_width() - 1)
	var y0: int = clampi(int(floor(atlas_position.y * image_size.y)), 0, source.get_height() - 1)
	var x1: int = clampi(int(ceil((atlas_position.x + atlas_size.x) * image_size.x)), x0 + 1, source.get_width())
	var y1: int = clampi(int(ceil((atlas_position.y + atlas_size.y) * image_size.y)), y0 + 1, source.get_height())
	var cropped: Image = source.get_region(Rect2i(x0, y0, x1 - x0, y1 - y0))
	if cropped == null or cropped.is_empty():
		return null

	# Godot stores the lightmapper shadowmask atlas as RGBA8. The directional
	# visibility value is read from the red channel; copy it explicitly instead
	# of relying on color-to-luminance conversion semantics.
	var visibility := Image.create(cropped.get_width(), cropped.get_height(), false, Image.FORMAT_L8)
	for y: int in range(cropped.get_height()):
		for x: int in range(cropped.get_width()):
			var factor: float = cropped.get_pixel(x, y).r
			visibility.set_pixel(x, y, Color(factor, factor, factor, 1.0))
	cropped = visibility
	var target_size: int = clampi(_nearest_power_of_two(max(cropped.get_width(), cropped.get_height())), 64, 1024)
	if cropped.get_width() != target_size or cropped.get_height() != target_size:
		cropped.resize(target_size, target_size, Image.INTERPOLATE_LANCZOS)
	cropped.generate_mipmaps()
	return cropped


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


func _publish_light_data_transactionally(light_data: LightmapGIData, final_path: String) -> Error:
	var directory: String = final_path.get_base_dir()
	var base_name: String = final_path.get_file().get_basename()
	return TransactionalResourcePublisher.publish(
		light_data, final_path,
		directory.path_join(base_name + ".publishing.lmbake"),
		directory.path_join(base_name + ".previous.lmbake"),
		"LightmapGIData",
		func(resource: Resource) -> bool:
			var validation := resource as LightmapGIData
			return is_instance_valid(validation) and not validation.lightmap_textures.is_empty())


func _publish_image_transactionally(image: Image, final_path: String) -> Error:
	if image == null or image.is_empty():
		return ERR_INVALID_DATA
	var directory: String = final_path.get_base_dir()
	var base_name: String = final_path.get_file().get_basename()
	return TransactionalResourcePublisher.publish(
		image, final_path,
		directory.path_join(base_name + ".publishing.res"),
		directory.path_join(base_name + ".previous.res"),
		"Image",
		func(resource: Resource) -> bool:
			var validation := resource as Image
			return validation != null and not validation.is_empty() and validation.get_format() == Image.FORMAT_L8 and validation.has_mipmaps())


func _publish_package_transactionally(package: Terrain3DRegionLighting, final_path: String) -> Error:
	var directory: String = final_path.get_base_dir()
	var base_name: String = final_path.get_file().trim_suffix(".t3dlight.tres")
	return TransactionalResourcePublisher.publish(
		package, final_path,
		directory.path_join(base_name + ".publishing.t3dlight.tres"),
		directory.path_join(base_name + ".previous.t3dlight.tres"),
		"Terrain3DRegionLighting",
		func(resource: Resource) -> bool:
			var validation := resource as Terrain3DRegionLighting
			return is_instance_valid(validation) and not validation.light_data_path.is_empty())


func _remove_file_if_present(path: String) -> Error:
	if path.is_empty() or not FileAccess.file_exists(path):
		return OK
	return DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


func _cleanup_interrupted_publications(directory: String) -> void:
	var output_dir: DirAccess = DirAccess.open(directory)
	if output_dir == null:
		return
	for file_name: String in output_dir.get_files():
		if not file_name.begins_with("Region_["):
			continue
		if (file_name.ends_with(".working.lmbake") or file_name.ends_with(".publishing.lmbake")
				or file_name.ends_with(".publishing.t3dlight.tres")
				or file_name.ends_with(".terrain_shadow.publishing.res")
				or file_name.ends_with(".terrain_shadow.previous.res")):
			_remove_file_if_present(directory.path_join(file_name))


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


