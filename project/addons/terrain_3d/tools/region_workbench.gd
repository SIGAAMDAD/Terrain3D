# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Isolated multi-region authoring workbench for Terrain3D.
@tool
extends Node


const WORKBENCH_ROOT: String = "res://Terrain3D_Workbench"
const MANIFEST_FILE: String = "workbench.json"
const SCENE_FILE: String = "region_workbench.tscn"
const DATA_DIR_NAME: String = "data"
const CONTEXT_DIR_NAME: String = "context"
const ARCHIVE_DIR_NAME: String = "_terrain3d_archive"
const WORKBENCH_SCENE_SCRIPT = preload("res://addons/terrain_3d/tools/region_workbench_scene.gd")
const WORKBENCH_META: StringName = &"terrain3d_region_workbench"
const MANIFEST_META: StringName = &"terrain3d_workbench_manifest"
const REGION_NODES_NAME: StringName = &"RegionNodes"
const REGION_CONTAINER_META: StringName = &"terrain3d_workbench_region_container"
const REGION_LOCATION_META: StringName = &"terrain3d_workbench_region_location"
const NODE_META_LIGHTING_CLASS: StringName = &"terrain3d_lighting_class"
const NODE_META_BAKED_LIGHTING_MODE: StringName = &"terrain3d_baked_lighting_mode"
const NODE_META_CONTRIBUTE_BAKED_GI: StringName = &"terrain3d_contribute_baked_gi"
const NODE_META_RECEIVE_DYNAMIC_DIRECTIONAL: StringName = &"terrain3d_receive_dynamic_directional"
const NODE_META_DYNAMIC_SHADOW_ELIGIBLE: StringName = &"terrain3d_dynamic_shadow_eligible"
const NODE_META_SHADOW_PROXY_SCENE_PATH: StringName = &"terrain3d_shadow_proxy_scene_path"
const REGION_NODE_DIR_NAME: String = "_terrain3d_region_nodes"
const NODE_META_ID: StringName = &"terrain3d_stream_id"
const NODE_META_FLAGS: StringName = &"terrain3d_stream_flags"
const NODE_META_STATE: StringName = &"terrain3d_stream_state"
const NODE_META_SOURCE_PATH: StringName = &"terrain3d_stream_source_path"
const NODE_META_LOAD_ERROR: StringName = &"terrain3d_stream_load_error"
const NODE_META_BOUNDARY_POLICY: StringName = &"terrain3d_boundary_policy"
const NODE_META_COVERAGE_MIN: StringName = &"terrain3d_coverage_min"
const NODE_META_COVERAGE_MAX: StringName = &"terrain3d_coverage_max"
const BOUNDARY_POLICY_ANCHOR: int = 0
const BOUNDARY_POLICY_BOUNDS: int = 1
const WORKBENCH_CONTEXT_RADIUS: int = 2
const PUBLISH_PAGE_DEPENDENCY_RADIUS: int = 1
const WORKBENCH_REGION_LIST_DISPLAY_LIMIT: int = 512
const LIGHTMAP_CLUSTER_SIZE: int = 2
const CHECKOUT_PROGRESS_BATCH: int = 8

# Only terrain configuration that materially affects isolated region authoring is
# carried into the workbench. Copying every storage property also copies Node
# internals and process-static ownership state, which is unsafe.
const TERRAIN_SCALAR_PROPERTIES: PackedStringArray = [
	"debug_level",
	"editor_execution_mode",
	"performance_target",
	"runtime_shader_tiering_enabled",
	"runtime_near_shader_lods",
	"runtime_near_range_end_m",
	"runtime_middle_range_end_m",
	"runtime_sync_detail_lods",
	"runtime_middle_shader_lods",
	"runtime_clipmap_update_budget",
	"runtime_page_prepare_threads",
	"runtime_profiling_enabled",
	"save_16_bit",
	"region_size",
	"vertex_spacing",
	"mesh_lods",
	"mesh_size",
	"tessellation_level",
	"cull_margin",
	"cast_shadows",
	"shadow_lod_count",
	"gi_mode",
	"render_layers",
	"displacement_scale",
	"displacement_sharpness",
	"compute_displacement_enabled",
	"displacement_update_distance",
	"buffer_shader_override_enabled",
	"total_memory_budget_mb",
	"memory_transient_reserve_mb",
	"gpu_region_cache_budget_auto",
	"cpu_region_cache_budget_auto",
	"instance_region_cache_budget_auto",
	"lightmap_residency_budget_auto",
	"shader_cache_budget_auto",
	"lightmap_residency_budget_mb",
	"gpu_region_cache_enabled",
	"gpu_region_cache_budget_mb",
	"gpu_region_cache_page_limit",
	"gpu_region_upload_budget_layers",
	"gpu_region_upload_budget_mb",
	"gpu_region_spare_pages",
	"gpu_region_retire_frames",
	"gpu_region_detail_lods",
	"gpu_region_detail_range_end_m",
	"gpu_region_guard_regions",
	"gpu_region_prefetch_rows",
	"gpu_region_hysteresis_ticks",
	"gpu_region_color_resolution_divisor",
	"gpu_region_derived_resolution_divisor",
	"gpu_middle_height_resolution_divisor",
	"editor_gpu_region_cache_pages",
	"runtime_material_streaming_enabled",
	"runtime_material_cache_layers",
	"runtime_material_upload_budget_layers",
	"gpu_region_cooked_pages_enabled",
	"gpu_region_cook_on_save",
	"cpu_region_cache_enabled",
	"cpu_region_cache_budget_mb",
	"cpu_region_load_budget_layers",
	"cpu_region_load_budget_mb",
	"instance_region_cache_enabled",
	"instance_region_cache_budget_mb",
	"instance_region_load_budget_pages",
	"instance_region_load_budget_mb",
	"instancer_cell_size",
	"instancer_cell_streaming_enabled",
	"instancer_single_lod_buffers",
	"instancer_cell_streaming_margin",
	"instancer_cell_streaming_update_distance",
	"instancer_update_cell_budget",
	"dynamic_node_streaming_enabled",
	"dynamic_node_stream_radius",
	"dynamic_node_load_region_budget",
	"dynamic_node_load_node_budget",
	"dynamic_node_unload_region_budget",
	"ocean_enabled",
	"ocean_performance_preset",
	"ocean_mesh_lods",
	"ocean_mesh_size",
	"ocean_vertex_spacing",
	"ocean_tessellation_level",
	"ocean_vertex_wave_iterations",
	"ocean_fragment_wave_iterations",
	"ocean_shading_mode",
	"ocean_foam_mode",
	"ocean_refraction_mode",
	"ocean_scattering_mode",
	"ocean_cast_shadows",
	"ocean_gi_mode",
	"ocean_render_layers",
	"ocean_cull_margin",
]

const TERRAIN_RESOURCE_PROPERTIES: PackedStringArray = [
	"material",
	"assets",
	"auto_shader_profile",
	"physics_material",
	"buffer_shader_override",
	"ocean_material",
]

var plugin: Terrain3DEditorPluginBase
var _dialog: ConfirmationDialog
var _x_spin: SpinBox
var _z_spin: SpinBox
var _region_list: ItemList
var _region_map: Terrain3DRegionWorkbenchMap
var _radius_spin: SpinBox
var _status_label: Label
var _dialog_selection: Dictionary[Vector2i, bool] = {}
var _checkout_progress_dialog: AcceptDialog
var _checkout_progress_bar: ProgressBar
var _checkout_progress_label: Label
var _checkout_cancelled: bool = false
var _abandon_dialog: ConfirmationDialog
var _available_locations: Array[Vector2i] = []
var _updating_location_controls: bool = false
var _operations_dock: EditorDock
var _operations_panel: VBoxContainer
var _operations_summary: Label
var _operations_regions: ItemList
var _operations_memory: Label
var _operations_status: Label
var _operations_refresh_accum: float = 0.0
var _operations_manifest_path: String = ""
var _operations_manifest_modified_time: int = 0
var _operations_manifest: Terrain3DRegionWorkbenchManifest
var _operations_region_list_dirty: bool = true


func _ready() -> void:
	set_process(true)
	_ensure_operations_panel.call_deferred()


func _exit_tree() -> void:
	if is_instance_valid(_operations_dock):
		if is_instance_valid(plugin):
			plugin.remove_dock(_operations_dock)
		_operations_dock.queue_free()
	_operations_dock = null
	_operations_panel = null


func _process(delta: float) -> void:
	_operations_refresh_accum += delta
	if _operations_refresh_accum < 0.5:
		return
	_operations_refresh_accum = 0.0
	_refresh_operations_panel()


func open_dialog() -> void:
	var source_terrain: Terrain3D = _get_active_terrain()
	if not is_instance_valid(source_terrain):
		push_error("Select a Terrain3D node before opening a region workbench")
		return
	if is_active_workbench():
		push_error("This scene is already a Terrain3D region workbench")
		return
	if source_terrain.data_directory.is_empty():
		push_error("The selected Terrain3D node has no data directory")
		return

	_ensure_dialog()
	_populate_region_list(source_terrain)
	var location: Vector2i = _get_default_region_location(source_terrain)
	_set_dialog_location(source_terrain, location)
	_select_radius(location, 0)
	_dialog.popup_centered(Vector2i(1000, 720))


func open_region(location: Vector2i) -> Terrain3DRegionWorkbenchResult:
	return await open_regions([location])


func open_regions(locations: Array[Vector2i]) -> Terrain3DRegionWorkbenchResult:
	var source_terrain: Terrain3D = _get_active_terrain()
	if Engine.is_editor_hint():
		var save_error: Error = EditorInterface.save_scene()
		if save_error != OK:
			return _failure("Source scene must be saved before opening a region workbench")
		var edited_root: Node = EditorInterface.get_edited_scene_root()
		if not is_instance_valid(edited_root) or edited_root.scene_file_path.is_empty():
			return _failure("Save the source scene to disk before opening a region workbench")

	var result: Terrain3DRegionWorkbenchResult = await create_workbench_regions(source_terrain, locations)
	if not result.ok:
		push_error(result.error if not result.error.is_empty() else "Unable to create Terrain3D region workbench")
		return result

	if Engine.is_editor_hint():
		var close_error: Error = EditorInterface.close_scene()
		if close_error != OK:
			return _failure("Unable to close the source scene before opening the workbench")
		_open_scene_after_owner_release.call_deferred(result.scene_path)
	return result


func create_workbench(source_terrain: Terrain3D, location: Vector2i) -> Terrain3DRegionWorkbenchResult:
	return await create_workbench_regions(source_terrain, [location])


func create_workbench_regions(source_terrain: Terrain3D,
		locations: Array[Vector2i]) -> Terrain3DRegionWorkbenchResult:
	if not is_instance_valid(source_terrain):
		return _failure("No valid Terrain3D source node was provided")
	if source_terrain.data_directory.is_empty():
		return _failure("The source Terrain3D node has no data directory")
	if source_terrain.data == null:
		return _failure("The source Terrain3D node has no terrain data")

	var checkout_locations: Array[Vector2i] = _normalize_locations(locations)
	if checkout_locations.is_empty():
		return _failure("Choose at least one existing terrain region")
	for location: Vector2i in checkout_locations:
		if not is_instance_valid(source_terrain.data.get_region(location)):
			return _failure("No terrain region exists at %s" % location)
	var context_locations: Array[Vector2i] = _collect_context_locations(
		source_terrain, checkout_locations, WORKBENCH_CONTEXT_RADIUS)

	var source_directory: String = source_terrain.data_directory.trim_suffix("/")
	var source_scene_path: String = ""
	if Engine.is_editor_hint():
		var edited_root: Node = EditorInterface.get_edited_scene_root()
		if is_instance_valid(edited_root):
			source_scene_path = edited_root.scene_file_path
	elif source_terrain.is_inside_tree() and is_instance_valid(source_terrain.get_tree().current_scene):
		source_scene_path = source_terrain.get_tree().current_scene.scene_file_path

	var source_id: String = _source_identifier(source_scene_path, source_directory)
	var workbench_directory: String = _unique_session_directory(source_id, checkout_locations.size())
	var workbench_data_directory: String = workbench_directory.path_join(DATA_DIR_NAME)
	var workbench_context_directory: String = workbench_directory.path_join(CONTEXT_DIR_NAME)
	var manifest_path: String = workbench_directory.path_join(MANIFEST_FILE)
	var scene_path: String = workbench_directory.path_join(SCENE_FILE)
	var mkdir_error: Error = DirAccess.make_dir_recursive_absolute(
		ProjectSettings.globalize_path(workbench_data_directory))
	if mkdir_error != OK:
		return _failure("Unable to create workbench directory: %s" % error_string(mkdir_error))
	if not context_locations.is_empty():
		mkdir_error = DirAccess.make_dir_recursive_absolute(
			ProjectSettings.globalize_path(workbench_context_directory))
		if mkdir_error != OK:
			_remove_directory_recursive(workbench_directory)
			return _failure("Unable to create workbench context directory: %s" % error_string(mkdir_error))

	var terrain_configuration: Terrain3DRegionWorkbenchConfiguration = _capture_terrain_configuration(
		source_terrain, workbench_directory)
	if not terrain_configuration.ok:
		return _failure(terrain_configuration.error)

	var manifest := Terrain3DRegionWorkbenchManifest.new()
	manifest.source_scene = source_scene_path
	manifest.source_data_directory = source_directory
	var source_far_atlas_path: String = source_directory.path_join("terrain3d_far_atlas.t3dfar")
	manifest.source_far_atlas_md5 = FileAccess.get_md5(source_far_atlas_path) \
		if FileAccess.file_exists(source_far_atlas_path) else ""
	manifest.region_location = checkout_locations[0]
	manifest.region_size = source_terrain.region_size
	manifest.vertex_spacing = source_terrain.vertex_spacing
	manifest.workbench_data_directory = workbench_data_directory
	manifest.workbench_context_directory = workbench_context_directory
	manifest.workbench_scene = scene_path
	manifest.created_unix = int(Time.get_unix_time_from_system())
	manifest.terrain_properties = terrain_configuration.properties
	manifest.terrain_resources = terrain_configuration.resources
	manifest.lightmap_cluster_size = LIGHTMAP_CLUSTER_SIZE
	_initialize_lighting_outputs(manifest, checkout_locations, workbench_directory)

	var copied_paths: PackedStringArray = PackedStringArray()
	var editable_set: Dictionary[Vector2i, bool] = {}
	for location: Vector2i in checkout_locations:
		editable_set[location] = true
	var copy_locations: Array[Vector2i] = checkout_locations.duplicate()
	copy_locations.append_array(context_locations)
	_begin_checkout_progress(copy_locations.size())
	for copy_index: int in range(copy_locations.size()):
		var location: Vector2i = copy_locations[copy_index]
		var source_region: Terrain3DRegion = source_terrain.data.get_region(location)
		if not is_instance_valid(source_region):
			_end_checkout_progress()
			_remove_directory_recursive(workbench_directory)
			return _failure("Unable to retrieve terrain region %s" % location)
		var source_region_path: String = source_region.resource_path
		if source_region_path.is_empty() or not FileAccess.file_exists(source_region_path):
			_end_checkout_progress()
			_remove_directory_recursive(workbench_directory)
			return _failure("Terrain region %s has no saved external resource. Save the source terrain before checkout." % location)
		var snapshot_directory: String = workbench_data_directory if editable_set.has(location) else workbench_context_directory
		var workbench_region_path: String = snapshot_directory.path_join(source_region_path.get_file())
		var snapshot_error: Error = _snapshot_region_to_workbench(source_region, workbench_region_path)
		if snapshot_error != OK:
			_end_checkout_progress()
			_remove_directory_recursive(workbench_directory)
			return _failure("Unable to snapshot source region %s into workbench: %s" % [location, error_string(snapshot_error)])
		copied_paths.push_back(workbench_region_path)
		_update_checkout_progress(copy_index + 1, copy_locations.size(), location)
		if ((copy_index + 1) % CHECKOUT_PROGRESS_BATCH) == 0:
			await get_tree().process_frame
			if _checkout_cancelled:
				_end_checkout_progress()
				_remove_directory_recursive(workbench_directory)
				return _failure("Region Workbench checkout was cancelled")
		var source_page_path: String = source_region_path.get_basename() + ".t3dpage"
		var entry := Terrain3DRegionWorkbenchRegion.new()
		entry.location = location
		entry.source_region_path = source_region_path
		entry.source_region_md5 = FileAccess.get_md5(source_region_path)
		entry.source_page_md5 = FileAccess.get_md5(source_page_path) if FileAccess.file_exists(source_page_path) else ""
		entry.workbench_region_path = workbench_region_path
		if editable_set.has(location):
			manifest.regions.push_back(entry)
		else:
			manifest.context_regions.push_back(entry)

	_end_checkout_progress()
	manifest._sync_primary_fields()
	var manifest_error: Error = _write_manifest(manifest_path, manifest)
	if manifest_error != OK:
		_remove_directory_recursive(workbench_directory)
		return _failure("Unable to write workbench manifest: %s" % error_string(manifest_error))

	var scene_error: Error = _write_workbench_scene(source_terrain, checkout_locations,
		workbench_data_directory, manifest_path, scene_path)
	if scene_error != OK:
		_remove_directory_recursive(workbench_directory)
		return _failure("Unable to create workbench scene: %s" % error_string(scene_error))

	var result := Terrain3DRegionWorkbenchResult.success()
	result.scene_path = scene_path
	result.manifest_path = manifest_path
	result.region_count = checkout_locations.size()
	result.context_region_count = context_locations.size()
	result.region_locations = checkout_locations.duplicate()
	for entry: Terrain3DRegionWorkbenchRegion in manifest.regions:
		result.workbench_region_paths.push_back(entry.workbench_region_path)
	result.workbench_region_path = manifest.workbench_region_path
	result.region_location = manifest.region_location
	return result


func archive_active_region() -> Terrain3DRegionWorkbenchResult:
	var workbench_root: Node = EditorInterface.get_edited_scene_root() if Engine.is_editor_hint() else null
	var active_manifest_path: String = String(workbench_root.get_meta(MANIFEST_META, "")) if is_instance_valid(workbench_root) else ""
	var session_directory: String = active_manifest_path.get_base_dir() if not active_manifest_path.is_empty() else ""
	var workbench_terrain: Terrain3D = _get_active_terrain()
	if not is_instance_valid(workbench_terrain):
		push_error("The workbench Terrain3D node could not be found. Reopen the workbench scene before committing.")
		return _failure("Workbench Terrain3D node is unavailable")
	if Engine.is_editor_hint():
		var save_error: Error = EditorInterface.save_scene()
		if save_error != OK:
			return _failure("Save the workbench scene before committing: %s" % error_string(save_error))
	var result: Terrain3DRegionWorkbenchResult = await archive_region(workbench_terrain)
	if not result.ok:
		push_error(result.error if not result.error.is_empty() else "Unable to commit Terrain3D regions")
	else:
		print("Terrain3D: committed %d workbench regions to source" % result.region_count)
		for warning: String in result.warnings:
			push_warning(warning)
		if Engine.is_editor_hint() and not result.source_scene.is_empty():
			var close_error: Error = EditorInterface.close_scene()
			if close_error == OK:
				_open_source_after_commit.call_deferred(result.source_scene, session_directory)
	return result


func archive_region(workbench_terrain: Terrain3D) -> Terrain3DRegionWorkbenchResult:
	if not is_instance_valid(workbench_terrain):
		return _failure("No valid workbench Terrain3D node was provided")
	var scene_root: Node = null
	if Engine.is_editor_hint():
		scene_root = EditorInterface.get_edited_scene_root()
	elif workbench_terrain.is_inside_tree():
		scene_root = workbench_terrain.get_tree().current_scene
	else:
		scene_root = workbench_terrain.get_parent()
	if not is_instance_valid(scene_root) or not bool(scene_root.get_meta(WORKBENCH_META, false)):
		return _failure("The active scene is not a Terrain3D region workbench")

	var manifest_path: String = String(scene_root.get_meta(MANIFEST_META, ""))
	if manifest_path.is_empty() or not FileAccess.file_exists(manifest_path):
		return _failure("Workbench manifest is missing: %s" % manifest_path)
	var manifest: Terrain3DRegionWorkbenchManifest = _read_manifest(manifest_path)
	if manifest == null or manifest.regions.is_empty():
		return _failure("Workbench manifest is invalid or contains no regions")
	if manifest.format < 6:
		return _failure("This workbench predates isolated cook-context storage. Reopen a fresh Region Workbench before committing.")
	if workbench_terrain.region_size != manifest.region_size:
		return _failure("Workbench region size no longer matches the source terrain")
	if not is_equal_approx(workbench_terrain.vertex_spacing, manifest.vertex_spacing):
		return _failure("Workbench vertex spacing no longer matches the source terrain")
	_begin_commit_progress(manifest.get_all_region_entries().size())
	var source_directory: String = manifest.source_data_directory
	if source_directory.is_empty():
		source_directory = manifest.regions[0].source_region_path.get_base_dir()
	var source_far_atlas_path: String = source_directory.path_join("terrain3d_far_atlas.t3dfar")
	var current_far_atlas_md5: String = FileAccess.get_md5(source_far_atlas_path) \
		if FileAccess.file_exists(source_far_atlas_path) else ""
	if current_far_atlas_md5 != manifest.source_far_atlas_md5:
		return _commit_preflight_failure("The source far atlas changed after checkout. Reopen a fresh workbench before committing.")

	# Lightmaps are part of the same source transaction as regions/pages. A staged
	# Workbench bake may only replace the exact source lighting state captured at
	# checkout.
	for output: Terrain3DRegionWorkbenchLightingOutput in manifest.lighting_outputs:
		var current_light_md5: String = FileAccess.get_md5(output.source_light_data_path) if FileAccess.file_exists(output.source_light_data_path) else ""
		var current_package_md5: String = FileAccess.get_md5(output.source_package_path) if FileAccess.file_exists(output.source_package_path) else ""
		if current_light_md5 != output.source_light_data_md5 or current_package_md5 != output.source_package_md5:
			return _commit_preflight_failure("Source lighting cluster %s → %s changed after checkout. Reopen a fresh workbench before committing." % [output.cluster_min, output.cluster_max])

	# Validate every source before generating or replacing anything. This makes a
	# multi-region commit all-or-nothing with respect to concurrent source edits.
	# The read-only two-ring context is also hashed because freshly cooked pages
	# for the editable set depend on those neighboring height/control samples.
	var validation_entries: Array[Terrain3DRegionWorkbenchRegion] = manifest.get_all_region_entries()
	for validation_index: int in range(validation_entries.size()):
		var entry: Terrain3DRegionWorkbenchRegion = validation_entries[validation_index]
		if (validation_index % CHECKOUT_PROGRESS_BATCH) == 0:
			_update_commit_progress(validation_index, validation_entries.size(), "Validating source region %s" % entry.location)
			await get_tree().process_frame
			if _checkout_cancelled:
				_end_checkout_progress()
				return _commit_preflight_failure("Region Workbench commit was cancelled before source mutation")
		if entry.source_region_path.is_empty() or not FileAccess.file_exists(entry.source_region_path):
			return _commit_preflight_failure("Source region file no longer exists for %s: %s" % [entry.location, entry.source_region_path])
		var current_source_md5: String = FileAccess.get_md5(entry.source_region_path)
		if not entry.source_region_md5.is_empty() and current_source_md5 != entry.source_region_md5:
			return _commit_preflight_failure("Source region %s changed after checkout. Reopen a fresh workbench instead of overwriting it." % entry.location)
		var source_page_path: String = entry.source_region_path.get_basename() + ".t3dpage"
		var current_page_md5: String = FileAccess.get_md5(source_page_path) if FileAccess.file_exists(source_page_path) else ""
		if current_page_md5 != entry.source_page_md5:
			return _commit_preflight_failure("Cooked runtime page for source region %s changed after checkout. Reopen a fresh workbench instead of overwriting it." % entry.location)
		if manifest.get_region_entry(entry.location) != null:
			if workbench_terrain.data == null or not is_instance_valid(workbench_terrain.data.get_region(entry.location)):
				return _commit_preflight_failure("Workbench no longer contains editable region %s" % entry.location)
		elif entry.workbench_region_path.is_empty() or not FileAccess.file_exists(entry.workbench_region_path):
			return _commit_preflight_failure("Workbench cook-context snapshot is missing for %s" % entry.location)

	var world_region_size: float = float(workbench_terrain.region_size) * workbench_terrain.vertex_spacing
	var generated_paths: PackedStringArray = PackedStringArray()
	var node_results: Dictionary[Vector2i, Terrain3DRegionNodeBuildResult] = {}
	var warnings: PackedStringArray = PackedStringArray()
	var total_nodes: int = 0

	for node_index: int in range(manifest.regions.size()):
		var entry: Terrain3DRegionWorkbenchRegion = manifest.regions[node_index]
		if (node_index % CHECKOUT_PROGRESS_BATCH) == 0:
			_update_commit_progress(node_index, manifest.regions.size(), "Preparing streamed-node data for %s" % entry.location)
			await get_tree().process_frame
			if _checkout_cancelled:
				_cleanup_generated_files(generated_paths)
				return _commit_preflight_failure("Region Workbench commit was cancelled before source mutation")
		var node_result: Terrain3DRegionNodeBuildResult = _build_region_node_descriptors(
			scene_root, source_directory, entry.location, world_region_size)
		if not node_result.ok:
			_cleanup_generated_files(generated_paths)
			return _commit_preflight_failure(node_result.error)
		for path: String in node_result.generated_paths:
			generated_paths.push_back(path)
		for warning: String in node_result.warnings:
			warnings.push_back("%s: %s" % [entry.location, warning])
		node_results[entry.location] = node_result
		total_nodes += node_result.descriptors.size()
		var region: Terrain3DRegion = workbench_terrain.data.get_region(entry.location)
		region.set_streamed_nodes(Terrain3DRegionNodeDescriptor.values_from_list(node_result.descriptors))
		workbench_terrain.data.save_region(entry.location, manifest.workbench_data_directory,
			workbench_terrain.save_16_bit)
		region = workbench_terrain.data.get_region(entry.location)
		if not is_instance_valid(region) or region.resource_path.is_empty() or not FileAccess.file_exists(region.resource_path):
			_cleanup_generated_files(generated_paths)
			return _commit_preflight_failure("Edited workbench region %s could not be saved" % entry.location)
		entry.workbench_region_path = region.resource_path

	# Region pages are part of the transaction, not an optional follow-up. The
	# workbench contains two read-only dependency rings so pages for edited regions
	# and their immediate neighbors are cooked against complete cross-region inputs.
	var cook_result: Dictionary = _cook_gpu_region_pages_with_context(workbench_terrain, manifest)
	if not bool(cook_result.get("complete", false)):
		_cleanup_generated_files(generated_paths)
		return _commit_preflight_failure("Workbench runtime-page cook failed before commit: %s" % [
			String(cook_result.get("errors", cook_result.get("error", "unknown error")))])
	var publish_page_entries: Array[Terrain3DRegionWorkbenchRegion] = _runtime_page_publish_entries(manifest)
	for entry: Terrain3DRegionWorkbenchRegion in publish_page_entries:
		var workbench_page_path: String = entry.workbench_region_path.get_basename() + ".t3dpage"
		if not FileAccess.file_exists(workbench_page_path):
			_cleanup_generated_files(generated_paths)
			return _commit_preflight_failure("Cook completed without a publishable runtime page for %s" % entry.location)

	var archive_directory: String = _unique_archive_directory(source_directory)
	var mkdir_error: Error = DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(archive_directory))
	if mkdir_error != OK:
		_cleanup_generated_files(generated_paths)
		return _failure("Unable to create source archive directory: %s" % error_string(mkdir_error))

	# Snapshot editable source regions before staging replacements. This phase is
	# still cancellable because no source path has been mutated yet.
	for archive_index: int in range(manifest.regions.size()):
		var entry: Terrain3DRegionWorkbenchRegion = manifest.regions[archive_index]
		if (archive_index % CHECKOUT_PROGRESS_BATCH) == 0:
			_update_commit_progress(archive_index, manifest.regions.size(), "Archiving source region %s" % entry.location)
			await get_tree().process_frame
			if _checkout_cancelled:
				_cleanup_generated_files(generated_paths)
				_remove_directory_recursive(archive_directory)
				return _commit_preflight_failure("Region Workbench commit was cancelled before source mutation")
		var copy_error: Error = _copy_file(entry.source_region_path,
			archive_directory.path_join(entry.source_region_path.get_file()))
		if copy_error != OK:
			_cleanup_generated_files(generated_paths)
			_remove_directory_recursive(archive_directory)
			return _failure("Unable to archive source region %s: %s" % [entry.location, error_string(copy_error)])
	# Pages for the immediate dependency ring are also replaced because their
	# compiled border/control data can depend on an edited neighbor.
	for page_archive_index: int in range(publish_page_entries.size()):
		var entry: Terrain3DRegionWorkbenchRegion = publish_page_entries[page_archive_index]
		if (page_archive_index % CHECKOUT_PROGRESS_BATCH) == 0:
			_update_commit_progress(page_archive_index, publish_page_entries.size(), "Archiving cooked page %s" % entry.location)
			await get_tree().process_frame
			if _checkout_cancelled:
				_cleanup_generated_files(generated_paths)
				_remove_directory_recursive(archive_directory)
				return _commit_preflight_failure("Region Workbench commit was cancelled before source mutation")
		var source_page_path: String = entry.source_region_path.get_basename() + ".t3dpage"
		if FileAccess.file_exists(source_page_path):
			var copy_error: Error = _copy_file(source_page_path, archive_directory.path_join(source_page_path.get_file()))
			if copy_error != OK:
				_cleanup_generated_files(generated_paths)
				_remove_directory_recursive(archive_directory)
				return _failure("Unable to archive cooked page for %s: %s" % [entry.location, error_string(copy_error)])
	for output: Terrain3DRegionWorkbenchLightingOutput in manifest.lighting_outputs:
		if not output.is_staged():
			continue
		for source_path: String in [output.source_light_data_path, output.source_package_path]:
			if FileAccess.file_exists(source_path):
				var lighting_archive_error: Error = _copy_file(source_path, archive_directory.path_join(source_path.get_file()))
				if lighting_archive_error != OK:
					_cleanup_generated_files(generated_paths)
					return _failure("Unable to archive source lighting %s: %s" % [source_path, error_string(lighting_archive_error)])
	if FileAccess.file_exists(source_far_atlas_path):
		var far_archive_error: Error = _copy_file(source_far_atlas_path,
			archive_directory.path_join(source_far_atlas_path.get_file()))
		if far_archive_error != OK:
			_cleanup_generated_files(generated_paths)
			return _failure("Unable to archive the source far atlas: %s" % error_string(far_archive_error))

	# Stage runtime pages and lighting before the first source replacement. Region
	# resources are staged immediately after lighting publication so their external
	# lighting references point at the final source package paths, never at the
	# disposable Workbench session.
	var staged_region_paths: Dictionary[Vector2i, String] = {}
	var staged_page_paths: Dictionary[Vector2i, String] = {}
	var staged_light_data_paths: Dictionary[String, String] = {}
	var staged_light_package_paths: Dictionary[String, String] = {}
	for page_stage_index: int in range(publish_page_entries.size()):
		var entry: Terrain3DRegionWorkbenchRegion = publish_page_entries[page_stage_index]
		if (page_stage_index % CHECKOUT_PROGRESS_BATCH) == 0:
			_update_commit_progress(page_stage_index, publish_page_entries.size(), "Staging cooked page %s" % entry.location)
			await get_tree().process_frame
			if _checkout_cancelled:
				_cleanup_staged_paths(staged_region_paths, staged_page_paths)
				_cleanup_staged_lighting_paths(staged_light_data_paths, staged_light_package_paths)
				_cleanup_generated_files(generated_paths)
				_remove_directory_recursive(archive_directory)
				return _commit_preflight_failure("Region Workbench commit was cancelled before source mutation")
		var workbench_page_path: String = entry.workbench_region_path.get_basename() + ".t3dpage"
		var stage_page_path: String = entry.source_region_path.get_basename() + ".t3dpage.workbench_incoming"
		var copy_error: Error = _copy_file(workbench_page_path, stage_page_path)
		if copy_error != OK:
			_cleanup_staged_paths(staged_region_paths, staged_page_paths)
			_cleanup_staged_lighting_paths(staged_light_data_paths, staged_light_package_paths)
			_cleanup_generated_files(generated_paths)
			return _failure("Unable to stage cooked page for %s: %s" % [entry.location, error_string(copy_error)])
		staged_page_paths[entry.location] = stage_page_path

	for output: Terrain3DRegionWorkbenchLightingOutput in manifest.lighting_outputs:
		if not output.is_staged():
			continue
		var stage_light_path: String = output.source_light_data_path + ".workbench_incoming"
		var light_copy_error: Error = _copy_file(output.workbench_light_data_path, stage_light_path)
		if light_copy_error != OK:
			_cleanup_staged_paths(staged_region_paths, staged_page_paths)
			_cleanup_staged_lighting_paths(staged_light_data_paths, staged_light_package_paths)
			_cleanup_generated_files(generated_paths)
			return _failure("Unable to stage Workbench lightmap %s: %s" % [output.cluster_min, error_string(light_copy_error)])
		staged_light_data_paths[output.source_light_data_path] = stage_light_path
		var workbench_package := ResourceLoader.load(output.workbench_package_path, "Terrain3DRegionLighting", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as Terrain3DRegionLighting
		if not is_instance_valid(workbench_package):
			_cleanup_staged_paths(staged_region_paths, staged_page_paths)
			_cleanup_staged_lighting_paths(staged_light_data_paths, staged_light_package_paths)
			_cleanup_generated_files(generated_paths)
			return _failure("Unable to load staged lighting package for cluster %s" % output.cluster_min)
		var source_package := workbench_package.duplicate(true) as Terrain3DRegionLighting
		source_package.light_data_path = output.source_light_data_path
		var source_lighting_dir: String = output.source_package_path.get_base_dir()
		var lighting_mkdir_error: Error = DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(source_lighting_dir))
		if lighting_mkdir_error != OK:
			_cleanup_staged_paths(staged_region_paths, staged_page_paths)
			_cleanup_staged_lighting_paths(staged_light_data_paths, staged_light_package_paths)
			_cleanup_generated_files(generated_paths)
			return _failure("Unable to create source lighting directory: %s" % error_string(lighting_mkdir_error))
		var stage_package_path: String = output.source_package_path + ".workbench_incoming"
		var package_stage_error: Error = ResourceSaver.save(source_package, stage_package_path)
		if package_stage_error != OK:
			_cleanup_staged_paths(staged_region_paths, staged_page_paths)
			_cleanup_staged_lighting_paths(staged_light_data_paths, staged_light_package_paths)
			_cleanup_generated_files(generated_paths)
			return _failure("Unable to stage lighting package for cluster %s: %s" % [output.cluster_min, error_string(package_stage_error)])
		staged_light_package_paths[output.source_package_path] = stage_package_path

	# From this point onward source paths may be replaced. Cancellation is disabled
	# so the transaction either publishes completely or executes rollback.
	_end_checkout_progress()

	# Publish staged lighting first. Every replacement is archived above and rolls
	# back with the rest of the checkout if any later step fails.
	for source_path: String in staged_light_data_paths.keys():
		var replace_error: Error = _replace_file(staged_light_data_paths[source_path], source_path)
		if replace_error != OK:
			_restore_checkout_from_archive(manifest, archive_directory)
			_cleanup_staged_paths(staged_region_paths, staged_page_paths)
			_cleanup_staged_lighting_paths(staged_light_data_paths, staged_light_package_paths)
			_cleanup_generated_files(generated_paths)
			return _failure("Unable to publish Workbench lightmap; the checkout was restored: %s" % error_string(replace_error))
	for source_path: String in staged_light_package_paths.keys():
		var replace_error: Error = _replace_file(staged_light_package_paths[source_path], source_path)
		if replace_error != OK:
			_restore_checkout_from_archive(manifest, archive_directory)
			_cleanup_staged_paths(staged_region_paths, staged_page_paths)
			_cleanup_staged_lighting_paths(staged_light_data_paths, staged_light_package_paths)
			_cleanup_generated_files(generated_paths)
			return _failure("Unable to publish Workbench lighting package; the checkout was restored: %s" % error_string(replace_error))

	# Now stage edited regions with lighting references resolved against the source
	# package namespace rather than Workbench-local paths.
	for entry: Terrain3DRegionWorkbenchRegion in manifest.regions:
		var stage_region_path: String = entry.source_region_path + ".workbench_incoming"
		var stage_error: Error = _stage_workbench_region_for_source(workbench_terrain, manifest, entry, stage_region_path)
		if stage_error != OK:
			_restore_checkout_from_archive(manifest, archive_directory)
			_cleanup_staged_paths(staged_region_paths, staged_page_paths)
			_cleanup_staged_lighting_paths(staged_light_data_paths, staged_light_package_paths)
			_cleanup_generated_files(generated_paths)
			return _failure("Unable to stage edited region %s; the checkout was restored: %s" % [entry.location, error_string(stage_error)])
		staged_region_paths[entry.location] = stage_region_path

	for entry: Terrain3DRegionWorkbenchRegion in manifest.regions:
		var replace_error: Error = _replace_file(staged_region_paths[entry.location], entry.source_region_path)
		if replace_error != OK:
			_restore_checkout_from_archive(manifest, archive_directory)
			_cleanup_staged_paths(staged_region_paths, staged_page_paths)
			_cleanup_staged_lighting_paths(staged_light_data_paths, staged_light_package_paths)
			_cleanup_generated_files(generated_paths)
			return _failure("Unable to replace source region %s; the checkout was restored: %s" % [entry.location, error_string(replace_error)])

	for entry: Terrain3DRegionWorkbenchRegion in publish_page_entries:
		var source_page_path: String = entry.source_region_path.get_basename() + ".t3dpage"
		var replace_error: Error = _replace_file(staged_page_paths[entry.location], source_page_path)
		if replace_error != OK:
			_restore_checkout_from_archive(manifest, archive_directory)
			_cleanup_staged_paths(staged_region_paths, staged_page_paths)
			_cleanup_staged_lighting_paths(staged_light_data_paths, staged_light_package_paths)
			_cleanup_generated_files(generated_paths)
			return _failure("Unable to replace cooked page for %s; the checkout was restored: %s" % [entry.location, error_string(replace_error)])

	# A local workbench cannot authoritatively rebuild the whole-world far atlas.
	# Invalidate the source copy after the region/page transaction so the next full
	# source cook regenerates it from every authored region instead of reusing stale
	# macro data from before this checkout.
	var far_remove_error: Error = _remove_file_if_present(source_far_atlas_path)
	if far_remove_error != OK:
		_restore_checkout_from_archive(manifest, archive_directory)
		_cleanup_staged_paths(staged_region_paths, staged_page_paths)
		_cleanup_staged_lighting_paths(staged_light_data_paths, staged_light_package_paths)
		_cleanup_generated_files(generated_paths)
		return _failure("Unable to invalidate the source far atlas; the checkout was restored: %s" % error_string(far_remove_error))
	warnings.push_back("Source far atlas invalidated after commit. Run Cook Runtime Dataset in the full source terrain before shipping.")

	var committed_unix: int = int(Time.get_unix_time_from_system())
	for entry: Terrain3DRegionWorkbenchRegion in manifest.regions:
		var node_result: Terrain3DRegionNodeBuildResult = node_results[entry.location]
		entry.source_region_md5 = FileAccess.get_md5(entry.source_region_path)
		entry.last_archive_directory = archive_directory
		entry.last_region_node_directory = node_result.node_directory
		entry.streamed_node_count = node_result.descriptors.size()
		entry.committed_unix = committed_unix
	for entry: Terrain3DRegionWorkbenchRegion in publish_page_entries:
		var source_page_path: String = entry.source_region_path.get_basename() + ".t3dpage"
		entry.source_page_md5 = FileAccess.get_md5(source_page_path) if FileAccess.file_exists(source_page_path) else ""
	manifest.last_archive_directory = archive_directory
	manifest.last_region_node_directory = node_results[manifest.region_location].node_directory if node_results.has(manifest.region_location) else ""
	manifest.streamed_node_count = total_nodes
	manifest.committed_unix = committed_unix
	manifest.source_far_atlas_md5 = ""
	for output: Terrain3DRegionWorkbenchLightingOutput in manifest.lighting_outputs:
		if output.is_staged():
			output.source_light_data_md5 = FileAccess.get_md5(output.source_light_data_path) if FileAccess.file_exists(output.source_light_data_path) else ""
			output.source_package_md5 = FileAccess.get_md5(output.source_package_path) if FileAccess.file_exists(output.source_package_path) else ""
	manifest._sync_primary_fields()
	var manifest_error: Error = _write_manifest(manifest_path, manifest)
	if manifest_error != OK:
		_restore_checkout_from_archive(manifest, archive_directory)
		return _failure("Commit metadata could not be finalized; the entire checkout was restored: %s" % error_string(manifest_error))
	_operations_manifest = manifest
	_operations_manifest_path = manifest_path
	_operations_manifest_modified_time = int(FileAccess.get_modified_time(manifest_path)) if FileAccess.file_exists(manifest_path) else 0
	_operations_region_list_dirty = true

	var result := Terrain3DRegionWorkbenchResult.success()
	result.source_region_path = manifest.source_region_path
	result.archive_directory = archive_directory
	result.region_node_directory = manifest.last_region_node_directory
	result.streamed_node_count = total_nodes
	result.region_count = manifest.regions.size()
	result.region_locations = manifest.get_region_locations()
	result.warnings = warnings
	result.region_location = manifest.region_location
	result.source_scene = manifest.source_scene
	return result


func is_active_workbench() -> bool:
	var root: Node = EditorInterface.get_edited_scene_root() if Engine.is_editor_hint() else null
	return is_instance_valid(root) and bool(root.get_meta(WORKBENCH_META, false))


func _manifest_for_root(root: Node) -> Terrain3DRegionWorkbenchManifest:
	if not is_instance_valid(root):
		return null
	var path: String = String(root.get_meta(MANIFEST_META, ""))
	return _read_manifest(path) if not path.is_empty() else null



# The native cook scans the active Terrain3D data directory, while Workbench
# dependency snapshots intentionally live outside it. Load every context region
# into the repository so edge sampling sees the real neighbors, and stage only
# the dependency pages that Commit is required to publish.
func _cook_gpu_region_pages_with_context(terrain: Terrain3D,
		manifest: Terrain3DRegionWorkbenchManifest) -> Dictionary:
	if not is_instance_valid(terrain) or not is_instance_valid(terrain.data):
		return {"complete": false, "error": "Workbench Terrain3D data is unavailable"}
	if manifest == null:
		return {"complete": false, "error": "Workbench manifest is unavailable"}

	var loaded_context: Array[Vector2i] = []
	var staged_sources: Dictionary[Vector2i, String] = {}
	var publish_context: Dictionary[Vector2i, Terrain3DRegionWorkbenchRegion] = {}
	for entry: Terrain3DRegionWorkbenchRegion in _runtime_page_publish_entries(manifest):
		if manifest.get_region_entry(entry.location) == null:
			publish_context[entry.location] = entry

	for entry: Terrain3DRegionWorkbenchRegion in manifest.context_regions:
		if entry.workbench_region_path.is_empty() or not FileAccess.file_exists(entry.workbench_region_path):
			_cleanup_runtime_cook_context(terrain, loaded_context, staged_sources)
			return {"complete": false, "error": "Missing Workbench context snapshot for %s" % entry.location}
		terrain.data.load_region(entry.location, manifest.workbench_context_directory, false)
		var loaded: Terrain3DRegion = terrain.data.get_region(entry.location)
		if not is_instance_valid(loaded):
			_cleanup_runtime_cook_context(terrain, loaded_context, staged_sources)
			return {"complete": false, "error": "Unable to load Workbench context region %s" % entry.location}
		loaded_context.push_back(entry.location)

		if publish_context.has(entry.location):
			var staged_source: String = manifest.workbench_data_directory.path_join(entry.workbench_region_path.get_file())
			if FileAccess.file_exists(staged_source):
				_cleanup_runtime_cook_context(terrain, loaded_context, staged_sources)
				return {"complete": false, "error": "Context cook staging would overwrite %s" % staged_source}
			var stage_error: Error = _copy_file(entry.workbench_region_path, staged_source)
			if stage_error != OK:
				_cleanup_runtime_cook_context(terrain, loaded_context, staged_sources)
				return {"complete": false, "error": "Unable to stage context region %s: %s" % [entry.location, error_string(stage_error)]}
			staged_sources[entry.location] = staged_source

	var context_result: Dictionary = terrain.data.cook_gpu_region_pages()
	if bool(context_result.get("complete", false)):
		for location: Vector2i in staged_sources.keys():
			var entry: Terrain3DRegionWorkbenchRegion = publish_context.get(location)
			var staged_page: String = staged_sources[location].get_basename() + ".t3dpage"
			var context_page: String = entry.workbench_region_path.get_basename() + ".t3dpage"
			if not FileAccess.file_exists(staged_page):
				context_result["complete"] = false
				context_result["error"] = "Context runtime page was not produced for %s" % location
				break
			var copy_error: Error = _copy_file(staged_page, context_page)
			if copy_error != OK:
				context_result["complete"] = false
				context_result["error"] = "Unable to preserve context runtime page %s: %s" % [location, error_string(copy_error)]
				break

	_cleanup_runtime_cook_context(terrain, loaded_context, staged_sources)

	# The first cook temporarily included publish-neighbor sources in the Workbench
	# data directory. Re-publish the local manifest/far-atlas/cook database after
	# removing those staging files. Current editable pages remain unchanged.
	if bool(context_result.get("complete", false)):
		var local_result: Dictionary = terrain.data.cook_gpu_region_pages()
		if not bool(local_result.get("complete", false)):
			return local_result
	return context_result


func _cleanup_runtime_cook_context(terrain: Terrain3D, loaded_context: Array[Vector2i],
		staged_sources: Dictionary[Vector2i, String]) -> void:
	if is_instance_valid(terrain) and is_instance_valid(terrain.data):
		for location: Vector2i in loaded_context:
			terrain.data.remove_regionl(location, false)
	for staged_source: String in staged_sources.values():
		_remove_file_if_present(staged_source.get_basename() + ".t3dpage")
		_remove_file_if_present(staged_source)


func _ensure_operations_panel() -> void:
	if is_instance_valid(_operations_dock) or not is_instance_valid(plugin):
		return
	_operations_dock = EditorDock.new()
	_operations_dock.name = "Terrain3DRegionWorkbenchDock"
	_operations_dock.title = "Terrain3D Workbench"
	_operations_dock.layout_key = "terrain3d_region_workbench"
	_operations_dock.default_slot = EditorDock.DOCK_SLOT_RIGHT_BL
	_operations_dock.available_layouts = EditorDock.DOCK_LAYOUT_ALL
	_operations_dock.closable = false
	_operations_dock.transient = true
	_operations_dock.set_global(false)

	_operations_panel = VBoxContainer.new()
	_operations_panel.name = "Terrain3DRegionWorkbench"
	_operations_panel.add_theme_constant_override("separation", 8)
	_operations_panel.visible = false

	var title := Label.new()
	title.text = "Terrain3D Region Workbench"
	title.add_theme_font_size_override("font_size", 16)
	_operations_panel.add_child(title)
	_operations_summary = Label.new()
	_operations_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_operations_panel.add_child(_operations_summary)

	_operations_regions = ItemList.new()
	_operations_regions.custom_minimum_size = Vector2(260, 150)
	_operations_regions.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_operations_regions.tooltip_text = "Checked-out regions. Runtime-page status is refreshed from the isolated workbench data directory."
	_operations_panel.add_child(_operations_regions)

	var cook_button := Button.new()
	cook_button.text = "Cook Local Runtime Preview"
	cook_button.tooltip_text = "Preflight the checkout by cooking v4 pages for editable regions plus cook-only neighboring snapshots. Context snapshots remain isolated at rest and are staged only transiently while the native cooker resolves dependency edges. Commit publishes authoritative region/dependency pages; the full-world source far atlas is intentionally invalidated for a later source cook."
	cook_button.pressed.connect(_on_cook_runtime_pressed)
	_operations_panel.add_child(cook_button)

	var lightmap_button := Button.new()
	lightmap_button.text = "Open Region Lightmap Baker"
	lightmap_button.tooltip_text = "Open the current regional lightmap workflow on the Workbench Terrain3D node for preview and inspection. Workbench commit only publishes lightmap packages already staged by its own transaction data."
	lightmap_button.pressed.connect(_on_open_lightmap_baker_pressed)
	_operations_panel.add_child(lightmap_button)

	var commit_button := Button.new()
	commit_button.text = "Commit Workbench Regions"
	commit_button.tooltip_text = "Verify region/page/lighting source hashes, cook runtime pages, archive all affected files, then publish edited regions, dependency pages, and staged lightmap clusters as one transaction."
	commit_button.pressed.connect(_on_commit_workbench_pressed)
	_operations_panel.add_child(commit_button)

	var abandon_button := Button.new()
	abandon_button.text = "Abandon Workbench"
	abandon_button.tooltip_text = "Close and delete this isolated session without changing source terrain data."
	abandon_button.pressed.connect(abandon_active_workbench)
	_operations_panel.add_child(abandon_button)

	_operations_memory = Label.new()
	_operations_memory.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_operations_panel.add_child(_operations_memory)
	_operations_status = Label.new()
	_operations_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_operations_panel.add_child(_operations_status)
	_operations_dock.add_child(_operations_panel)
	plugin.add_dock(_operations_dock)
	_operations_dock.make_visible()
	_refresh_operations_panel()


func _refresh_operations_panel() -> void:
	if not is_instance_valid(_operations_panel):
		return
	var active: bool = is_active_workbench()
	_operations_panel.visible = active
	if is_instance_valid(_operations_dock):
		_operations_dock.visible = active
	if not active:
		return
	var root: Node = EditorInterface.get_edited_scene_root()
	var manifest_path: String = String(root.get_meta(MANIFEST_META, "")) if is_instance_valid(root) else ""
	var manifest_modified_time: int = int(FileAccess.get_modified_time(manifest_path)) if not manifest_path.is_empty() and FileAccess.file_exists(manifest_path) else 0
	if manifest_path != _operations_manifest_path or manifest_modified_time != _operations_manifest_modified_time:
		_operations_manifest_path = manifest_path
		_operations_manifest_modified_time = manifest_modified_time
		_operations_manifest = _read_manifest(manifest_path) if not manifest_path.is_empty() else null
		_operations_region_list_dirty = true
	var manifest: Terrain3DRegionWorkbenchManifest = _operations_manifest
	if manifest == null:
		_operations_summary.text = "Workbench manifest unavailable"
		return
	var staged_lighting: int = 0
	for output: Terrain3DRegionWorkbenchLightingOutput in manifest.lighting_outputs:
		if output.is_staged():
			staged_lighting += 1
	_operations_summary.text = "%d editable loaded · %d cook-only context snapshots · lightmaps %d/%d staged · source %s" % [
		manifest.regions.size(), manifest.context_regions.size(), staged_lighting,
		manifest.lighting_outputs.size(), manifest.source_data_directory]
	if _operations_region_list_dirty:
		_operations_regions.clear()
		var display_count: int = mini(manifest.regions.size(), WORKBENCH_REGION_LIST_DISPLAY_LIMIT)
		for index: int in range(display_count):
			var entry: Terrain3DRegionWorkbenchRegion = manifest.regions[index]
			var page_path: String = entry.workbench_region_path.get_basename() + ".t3dpage"
			var page_state: String = "cooked" if FileAccess.file_exists(page_path) else "uncooked"
			_operations_regions.add_item("%d, %d    %s" % [entry.location.x, entry.location.y, page_state])
		if manifest.regions.size() > display_count:
			_operations_regions.add_item("… %d more editable regions (list capped for editor responsiveness)" % [
				manifest.regions.size() - display_count])
		_operations_region_list_dirty = false

	var terrain: Terrain3D = _get_active_terrain()
	if not is_instance_valid(terrain):
		_operations_memory.text = "Terrain3D node unavailable"
		return
	var memory: Dictionary = terrain.get_memory_estimate()
	var gpu: Dictionary = terrain.data.get_gpu_region_cache_stats() if is_instance_valid(terrain.data) else {}
	var middle: Dictionary = terrain.data.get_middle_height_cache_stats() if is_instance_valid(terrain.data) else {}
	var known_bytes: int = int(memory.get("tracked_current_residency_bytes", memory.get("current_known_residency_bytes", 0)))
	_operations_memory.text = "Tracked terrain memory %.1f MiB · detail %d/%d pages · republish pending %d · Middle %d/%d pages" % [
		float(known_bytes) / 1048576.0, int(gpu.get("resident", 0)), int(gpu.get("physical_layers", 0)),
		int(gpu.get("republish_pending", 0)), int(middle.get("resident", 0)), int(middle.get("physical_layers", 0))]


func cook_active_runtime_data() -> Dictionary:
	var result: Dictionary = {"complete": false, "error": "No active workbench"}
	if not is_active_workbench():
		return result
	if Engine.is_editor_hint():
		var save_error: Error = EditorInterface.save_scene()
		if save_error != OK:
			return {"complete": false, "error": "Save the workbench before cooking: %s" % error_string(save_error)}
	var terrain: Terrain3D = _get_active_terrain()
	if not is_instance_valid(terrain) or not is_instance_valid(terrain.data):
		return {"complete": false, "error": "Workbench Terrain3D data is unavailable"}
	var root: Node = EditorInterface.get_edited_scene_root() if Engine.is_editor_hint() else null
	var manifest: Terrain3DRegionWorkbenchManifest = _manifest_for_root(root)
	if manifest == null:
		return {"complete": false, "error": "Workbench manifest is unavailable"}
	result = _cook_gpu_region_pages_with_context(terrain, manifest)
	_operations_region_list_dirty = true
	_refresh_operations_panel()
	return result


func _on_cook_runtime_pressed() -> void:
	var result: Dictionary = cook_active_runtime_data()
	if bool(result.get("complete", false)):
		_operations_status.text = "Local preview cook complete: %d cooked, %d unchanged, %d usable regions. The local far atlas is preview-only; commit will invalidate the full source far atlas." % [
			int(result.get("cooked", 0)), int(result.get("unchanged", 0)), int(result.get("usable", 0))]
	else:
		_operations_status.text = "Runtime cook failed: %s" % String(result.get("error", result.get("errors", "unknown error")))


func _on_open_lightmap_baker_pressed() -> void:
	if is_instance_valid(plugin):
		plugin.open_region_lightmap_baker()


func _on_commit_workbench_pressed() -> void:
	await archive_active_region()


func _ensure_dialog() -> void:
	if is_instance_valid(_dialog):
		return
	_dialog = ConfirmationDialog.new()
	_dialog.title = "Open Terrain Region Workbench"
	_dialog.ok_button_text = "Open Workbench"
	_dialog.cancel_button_text = "Cancel"
	_dialog.min_size = Vector2i(840, 620)
	_dialog.max_size = Vector2i(1320, 980)
	_dialog.unresizable = false
	_dialog.confirmed.connect(_on_dialog_confirmed)
	add_child(_dialog)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 12)
	margin.add_theme_constant_override("margin_right", 12)
	margin.add_theme_constant_override("margin_top", 8)
	margin.add_theme_constant_override("margin_bottom", 8)
	_dialog.add_child(margin)

	var content := VBoxContainer.new()
	content.add_theme_constant_override("separation", 10)
	margin.add_child(content)

	var explanation := Label.new()
	explanation.text = "Check out one or more terrain regions into an isolated editing session. The Terrain3D node loads only the selected editable regions. Neighboring dependency snapshots stay outside its data directory and are consumed only by cook/lightmap operations. Runtime pages and regional lightmaps remain staged inside the Workbench until Commit publishes them transactionally."
	explanation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	content.add_child(explanation)

	# Keep controls responsive at narrower dock/editor widths instead of packing the
	# full selector into one horizontal row.
	var coordinate_grid := GridContainer.new()
	coordinate_grid.columns = 6
	coordinate_grid.add_theme_constant_override("h_separation", 8)
	coordinate_grid.add_theme_constant_override("v_separation", 6)
	content.add_child(coordinate_grid)
	coordinate_grid.add_child(_make_axis_label("Center X"))
	_x_spin = _make_coordinate_spin()
	_x_spin.tooltip_text = "Region-space X coordinate used as the center for radius checkout selection."
	coordinate_grid.add_child(_x_spin)
	coordinate_grid.add_child(_make_axis_label("Z"))
	_z_spin = _make_coordinate_spin()
	_z_spin.tooltip_text = "Region-space Z coordinate used as the center for radius checkout selection."
	coordinate_grid.add_child(_z_spin)
	coordinate_grid.add_child(_make_axis_label("Radius"))
	_radius_spin = SpinBox.new()
	_radius_spin.min_value = 0
	_radius_spin.max_value = 16
	_radius_spin.step = 1
	_radius_spin.value = 1
	_radius_spin.custom_minimum_size.x = 86
	_radius_spin.tooltip_text = "Chebyshev radius used by Select Radius. Only authored regions inside the square are selected."
	coordinate_grid.add_child(_radius_spin)

	var selector_actions := HBoxContainer.new()
	selector_actions.add_theme_constant_override("separation", 8)
	content.add_child(selector_actions)
	var cursor_button := Button.new()
	cursor_button.text = "Use Cursor"
	cursor_button.tooltip_text = "Move the selection center to the Terrain3D editor cursor."
	cursor_button.pressed.connect(_use_cursor_region)
	selector_actions.add_child(cursor_button)
	var radius_button := Button.new()
	radius_button.text = "Select Radius"
	radius_button.tooltip_text = "Select all authored regions inside the configured square radius around the center."
	radius_button.pressed.connect(_select_dialog_radius)
	selector_actions.add_child(radius_button)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	selector_actions.add_child(spacer)
	var all_button := Button.new()
	all_button.text = "Select All"
	all_button.tooltip_text = "Select every authored region. Large selections are processed incrementally and can be cancelled."
	all_button.pressed.connect(_select_all_regions)
	selector_actions.add_child(all_button)
	var clear_button := Button.new()
	clear_button.text = "Clear"
	clear_button.pressed.connect(_clear_region_selection)
	selector_actions.add_child(clear_button)
	_x_spin.value_changed.connect(_on_location_changed)
	_z_spin.value_changed.connect(_on_location_changed)

	var selector_split := HSplitContainer.new()
	selector_split.size_flags_vertical = Control.SIZE_EXPAND_FILL
	selector_split.custom_minimum_size = Vector2(800, 360)
	content.add_child(selector_split)

	var map_column := VBoxContainer.new()
	map_column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	map_column.size_flags_stretch_ratio = 1.35
	var map_label := Label.new()
	map_label.text = "Spatial selection"
	map_column.add_child(map_label)
	_region_map = Terrain3DRegionWorkbenchMap.new()
	_region_map.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_region_map.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_region_map.selection_changed.connect(_on_map_selection_changed)
	_region_map.center_requested.connect(_on_map_center_requested)
	map_column.add_child(_region_map)
	selector_split.add_child(map_column)

	var list_column := VBoxContainer.new()
	list_column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	list_column.size_flags_stretch_ratio = 1.0
	var list_label := Label.new()
	list_label.text = "Authored regions"
	list_column.add_child(list_label)
	_region_list = ItemList.new()
	_region_list.select_mode = ItemList.SELECT_MULTI
	_region_list.allow_reselect = true
	_region_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_region_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_region_list.custom_minimum_size = Vector2(300, 280)
	_region_list.tooltip_text = "Exact region list synchronized with the spatial selector."
	_region_list.multi_selected.connect(_on_region_multi_selected)
	_region_list.item_clicked.connect(_on_region_item_clicked)
	list_column.add_child(_region_list)
	selector_split.add_child(list_column)

	_status_label = Label.new()
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status_label.custom_minimum_size = Vector2(700, 54)
	content.add_child(_status_label)


func _make_axis_label(text_value: String) -> Label:
	var label := Label.new()
	label.text = text_value
	return label


func _make_coordinate_spin() -> SpinBox:
	var spin := SpinBox.new()
	spin.min_value = -32
	spin.max_value = 31
	spin.step = 1
	spin.allow_greater = false
	spin.allow_lesser = false
	spin.custom_minimum_size.x = 92
	return spin


func _on_dialog_confirmed() -> void:
	var selected: Array[Vector2i] = _selected_dialog_locations()
	if selected.is_empty():
		return
	await open_regions(selected)


func _use_cursor_region() -> void:
	var source_terrain: Terrain3D = _get_active_terrain()
	if not is_instance_valid(source_terrain) or source_terrain.data == null:
		return
	var location: Vector2i = _get_default_region_location(source_terrain)
	_set_dialog_location(source_terrain, location)
	_select_radius(location, int(_radius_spin.value))


func _on_location_changed(_value: float) -> void:
	if _updating_location_controls:
		return
	var source_terrain: Terrain3D = _get_active_terrain()
	if not is_instance_valid(source_terrain):
		return
	_set_dialog_location(source_terrain, Vector2i(int(_x_spin.value), int(_z_spin.value)))


func _select_dialog_radius() -> void:
	_select_radius(Vector2i(int(_x_spin.value), int(_z_spin.value)), int(_radius_spin.value))


func _select_radius(center: Vector2i, radius: int) -> void:
	if not is_instance_valid(_region_list):
		return
	_dialog_selection.clear()
	for location: Vector2i in _available_locations:
		if maxi(absi(location.x - center.x), absi(location.y - center.y)) <= radius:
			_dialog_selection[location] = true
	_apply_dialog_selection()


func _select_all_regions() -> void:
	if not is_instance_valid(_region_list):
		return
	_dialog_selection.clear()
	for location: Vector2i in _available_locations:
		_dialog_selection[location] = true
	_apply_dialog_selection()


func _clear_region_selection() -> void:
	_dialog_selection.clear()
	_apply_dialog_selection()


func _on_region_multi_selected(_index: int, _selected: bool) -> void:
	_sync_dialog_selection_from_list.call_deferred()


func _on_region_item_clicked(_index: int, _at_position: Vector2, _mouse_button_index: int) -> void:
	_sync_dialog_selection_from_list.call_deferred()


func _sync_dialog_selection_from_list() -> void:
	if not is_instance_valid(_region_list):
		return
	_dialog_selection.clear()
	for raw_index: int in _region_list.get_selected_items():
		if raw_index >= 0 and raw_index < _available_locations.size():
			_dialog_selection[_available_locations[raw_index]] = true
	if is_instance_valid(_region_map):
		_region_map.set_selection(_selected_dialog_locations())
	_update_selection_status()


func _on_map_selection_changed(locations: Array) -> void:
	_dialog_selection.clear()
	for value: Variant in locations:
		if value is Vector2i:
			_dialog_selection[value] = true
	_apply_dialog_selection(false)


func _on_map_center_requested(location: Vector2i) -> void:
	var source_terrain: Terrain3D = _get_active_terrain()
	if is_instance_valid(source_terrain):
		_set_dialog_location(source_terrain, location)


func _apply_dialog_selection(update_map: bool = true) -> void:
	if is_instance_valid(_region_list):
		_region_list.deselect_all()
		for index: int in range(_available_locations.size()):
			if _dialog_selection.has(_available_locations[index]):
				_region_list.select(index, false)
	if update_map and is_instance_valid(_region_map):
		_region_map.set_selection(_selected_dialog_locations())
	_update_selection_status()


func _populate_region_list(source_terrain: Terrain3D) -> void:
	_available_locations.clear()
	if source_terrain.data != null:
		_available_locations.assign(source_terrain.data.get_region_locations())
	_available_locations.sort_custom(_region_location_less)
	_dialog_selection.clear()
	_region_list.clear()
	for location: Vector2i in _available_locations:
		var index: int = _region_list.add_item("%d, %d" % [location.x, location.y])
		_region_list.set_item_tooltip(index, "Terrain region (%d, %d)" % [location.x, location.y])
	if is_instance_valid(_region_map):
		_region_map.set_regions(_available_locations)


func _region_location_less(a: Vector2i, b: Vector2i) -> bool:
	return a.y < b.y or (a.y == b.y and a.x < b.x)


func _set_dialog_location(source_terrain: Terrain3D, location: Vector2i) -> void:
	_updating_location_controls = true
	_x_spin.value = location.x
	_z_spin.value = location.y
	_updating_location_controls = false
	if is_instance_valid(_region_map):
		_region_map.set_center(location)
	_status_label.text = _describe_region(source_terrain, location)


func _selected_dialog_locations() -> Array[Vector2i]:
	var result: Array[Vector2i] = []
	for location: Vector2i in _dialog_selection.keys():
		result.push_back(location)
	return _normalize_locations(result)


func _update_selection_status() -> void:
	if not is_instance_valid(_status_label) or not is_instance_valid(_dialog):
		return
	var selected: Array[Vector2i] = _selected_dialog_locations()
	_dialog.get_ok_button().disabled = selected.is_empty()
	if selected.is_empty():
		_status_label.text = "No regions selected. Choose arbitrary entries or use Select Radius around the workbench center."
		return
	var minimum: Vector2i = selected[0]
	var maximum: Vector2i = selected[0]
	for location: Vector2i in selected:
		minimum.x = mini(minimum.x, location.x)
		minimum.y = mini(minimum.y, location.y)
		maximum.x = maxi(maximum.x, location.x)
		maximum.y = maxi(maximum.y, location.y)
	_status_label.text = "%d editable regions selected, bounds %s → %s. Only these regions load into the Workbench Terrain3D node; neighboring dependency snapshots remain cook-only." % [selected.size(), minimum, maximum]


func _describe_region(source_terrain: Terrain3D, location: Vector2i) -> String:
	if source_terrain.data != null and is_instance_valid(source_terrain.data.get_region(location)):
		return "Center %s is authored. %d terrain regions exist in the source data set." % [location, _available_locations.size()]
	return "Center %s has no authored region. Radius selection still includes any authored neighbors inside the requested footprint." % location

func _normalize_locations(locations: Array[Vector2i]) -> Array[Vector2i]:
	var seen: Dictionary[Vector2i, bool] = {}
	var result: Array[Vector2i] = []
	for location: Vector2i in locations:
		if seen.has(location):
			continue
		seen[location] = true
		result.push_back(location)
	result.sort_custom(_region_location_less)
	return result


func _collect_context_locations(source_terrain: Terrain3D, editable_locations: Array[Vector2i],
		radius: int) -> Array[Vector2i]:
	if not is_instance_valid(source_terrain) or source_terrain.data == null or radius <= 0:
		return []
	var available: Dictionary[Vector2i, bool] = {}
	for location: Vector2i in source_terrain.data.get_region_locations():
		available[location] = true
	var editable: Dictionary[Vector2i, bool] = {}
	for location: Vector2i in editable_locations:
		editable[location] = true
	var context: Dictionary[Vector2i, bool] = {}
	for location: Vector2i in editable_locations:
		for z_offset: int in range(-radius, radius + 1):
			for x_offset: int in range(-radius, radius + 1):
				var candidate := location + Vector2i(x_offset, z_offset)
				if editable.has(candidate) or not available.has(candidate):
					continue
				context[candidate] = true
	var result: Array[Vector2i] = []
	for location: Vector2i in context.keys():
		result.push_back(location)
	result.sort_custom(_region_location_less)
	return result


func _runtime_page_publish_entries(manifest: Terrain3DRegionWorkbenchManifest) -> Array[Terrain3DRegionWorkbenchRegion]:
	var result: Array[Terrain3DRegionWorkbenchRegion] = []
	result.append_array(manifest.regions)
	var affected: Dictionary[Vector2i, bool] = {}
	for location: Vector2i in manifest.get_region_locations():
		for z_offset: int in range(-PUBLISH_PAGE_DEPENDENCY_RADIUS, PUBLISH_PAGE_DEPENDENCY_RADIUS + 1):
			for x_offset: int in range(-PUBLISH_PAGE_DEPENDENCY_RADIUS, PUBLISH_PAGE_DEPENDENCY_RADIUS + 1):
				affected[location + Vector2i(x_offset, z_offset)] = true
	for entry: Terrain3DRegionWorkbenchRegion in manifest.context_regions:
		if affected.has(entry.location):
			result.push_back(entry)
	return result


func _stage_workbench_region_for_source(workbench_terrain: Terrain3D,
		manifest: Terrain3DRegionWorkbenchManifest, entry: Terrain3DRegionWorkbenchRegion,
		destination_path: String) -> Error:
	var edited: Terrain3DRegion = workbench_terrain.data.get_region(entry.location) if is_instance_valid(workbench_terrain.data) else null
	if not is_instance_valid(edited):
		return ERR_DOES_NOT_EXIST
	var snapshot := edited.duplicate(true) as Terrain3DRegion
	if not is_instance_valid(snapshot):
		return ERR_CANT_CREATE
	# Lighting is source-owned outside the Workbench. If this cluster was rebaked,
	# load the just-published source package; otherwise preserve the source region's
	# existing lighting reference exactly.
	var output: Terrain3DRegionWorkbenchLightingOutput = manifest.get_lighting_output_for_location(entry.location)
	var lighting: Terrain3DRegionLighting = null
	if output != null and output.is_staged() and FileAccess.file_exists(output.source_package_path):
		lighting = ResourceLoader.load(output.source_package_path, "Terrain3DRegionLighting", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as Terrain3DRegionLighting
	else:
		var source_region := ResourceLoader.load(entry.source_region_path, "Terrain3DRegion", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as Terrain3DRegion
		if is_instance_valid(source_region):
			lighting = source_region.get_lighting()
	snapshot.set_lighting(lighting)
	return ResourceSaver.save(snapshot, destination_path)


func _cleanup_staged_lighting_paths(light_data_paths: Dictionary[String, String],
		package_paths: Dictionary[String, String]) -> void:
	for path: String in light_data_paths.values():
		_remove_file_if_present(path)
	for path: String in package_paths.values():
		_remove_file_if_present(path)


func _cleanup_staged_paths(region_paths: Dictionary[Vector2i, String],
		page_paths: Dictionary[Vector2i, String]) -> void:
	for path: String in region_paths.values():
		_remove_file_if_present(path)
	for path: String in page_paths.values():
		_remove_file_if_present(path)


func _restore_checkout_from_archive(manifest: Terrain3DRegionWorkbenchManifest,
		archive_directory: String) -> void:
	for entry: Terrain3DRegionWorkbenchRegion in manifest.regions:
		var archived_region_path: String = archive_directory.path_join(entry.source_region_path.get_file())
		if FileAccess.file_exists(archived_region_path):
			_copy_file(archived_region_path, entry.source_region_path)
	for entry: Terrain3DRegionWorkbenchRegion in _runtime_page_publish_entries(manifest):
		var source_page_path: String = entry.source_region_path.get_basename() + ".t3dpage"
		var archived_page_path: String = archive_directory.path_join(source_page_path.get_file())
		if FileAccess.file_exists(archived_page_path):
			_copy_file(archived_page_path, source_page_path)
		else:
			_remove_file_if_present(source_page_path)
	for output: Terrain3DRegionWorkbenchLightingOutput in manifest.lighting_outputs:
		if not output.is_staged():
			continue
		for source_path: String in [output.source_light_data_path, output.source_package_path]:
			var archived_path: String = archive_directory.path_join(source_path.get_file())
			if FileAccess.file_exists(archived_path):
				_copy_file(archived_path, source_path)
			else:
				_remove_file_if_present(source_path)
	var source_directory: String = manifest.source_data_directory
	if source_directory.is_empty() and not manifest.regions.is_empty():
		source_directory = manifest.regions[0].source_region_path.get_base_dir()
	if not source_directory.is_empty():
		var source_far_atlas_path: String = source_directory.path_join("terrain3d_far_atlas.t3dfar")
		var archived_far_atlas_path: String = archive_directory.path_join(source_far_atlas_path.get_file())
		if FileAccess.file_exists(archived_far_atlas_path):
			_copy_file(archived_far_atlas_path, source_far_atlas_path)
		else:
			_remove_file_if_present(source_far_atlas_path)


func _get_default_region_location(source_terrain: Terrain3D) -> Vector2i:
	if is_instance_valid(plugin):
		var mouse_location: Vector2i = source_terrain.data.get_region_location(plugin.mouse_global_position)
		if is_instance_valid(source_terrain.data.get_region(mouse_location)):
			return mouse_location
	var locations: Array[Vector2i] = source_terrain.data.get_region_locations()
	return Vector2i(locations[0]) if not locations.is_empty() else Vector2i.ZERO


func _get_active_terrain() -> Terrain3D:
	if is_instance_valid(plugin) and is_instance_valid(plugin.terrain):
		return plugin.terrain
	if Engine.is_editor_hint():
		var root: Node = EditorInterface.get_edited_scene_root()
		if is_instance_valid(root):
			return _find_terrain(root)
	return null


func _find_terrain(node: Node) -> Terrain3D:
	if node is Terrain3D:
		return node as Terrain3D
	for child: Node in node.get_children():
		var found: Terrain3D = _find_terrain(child)
		if is_instance_valid(found):
			return found
	return null


func _write_workbench_scene(_source_terrain: Terrain3D, _locations: Array[Vector2i], _data_directory: String,
		manifest_path: String, scene_path: String) -> Error:
	var root := Node3D.new()
	root.name = "Terrain3DRegionWorkbench"
	root.set_meta(WORKBENCH_META, true)
	root.set_meta(MANIFEST_META, manifest_path)
	root.set_script(WORKBENCH_SCENE_SCRIPT)
	root.set("manifest_path", manifest_path)

	var packed_scene := PackedScene.new()
	var pack_error: Error = packed_scene.pack(root)
	if pack_error != OK:
		root.free()
		return pack_error
	var save_error: Error = ResourceSaver.save(packed_scene, scene_path)
	root.free()
	return save_error


func _capture_terrain_configuration(source_terrain: Terrain3D,
		workbench_directory: String) -> Terrain3DRegionWorkbenchConfiguration:
	var properties: Dictionary[String, Variant] = {}
	var resources: Dictionary[String, String] = {}
	var available_properties: Dictionary[String, bool] = _property_name_set(source_terrain)

	for property_name: String in TERRAIN_SCALAR_PROPERTIES:
		if available_properties.has(property_name):
			properties[property_name] = source_terrain.get(property_name)

	var resource_directory: String = workbench_directory.path_join("resources")
	for property_name: String in TERRAIN_RESOURCE_PROPERTIES:
		if not available_properties.has(property_name):
			continue
		var resource := source_terrain.get(property_name) as Resource
		if not is_instance_valid(resource):
			continue
		var mkdir_error: Error = DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(resource_directory))
		if mkdir_error != OK:
			return Terrain3DRegionWorkbenchConfiguration.failure(
				"Unable to create workbench resource directory: %s" % error_string(mkdir_error))
		var resource_path: String = resource_directory.path_join(
			property_name.validate_filename() + ".tres")
		var save_error: Error = ResourceSaver.save(resource.duplicate(true), resource_path)
		if save_error != OK:
			return Terrain3DRegionWorkbenchConfiguration.failure(
				"Unable to save workbench resource %s: %s" % [property_name, error_string(save_error)])
		resources[property_name] = resource_path
	return Terrain3DRegionWorkbenchConfiguration.success(properties, resources)


func _region_container_name(location: Vector2i) -> String:
	return "Region_%d_%d" % [location.x, location.y]


func _get_region_node_container(region_nodes: Node3D, location: Vector2i) -> Node3D:
	if not is_instance_valid(region_nodes):
		return null
	var by_name := region_nodes.get_node_or_null(NodePath(_region_container_name(location))) as Node3D
	if is_instance_valid(by_name) and bool(by_name.get_meta(REGION_CONTAINER_META, false)):
		return by_name
	for child: Node in region_nodes.get_children():
		if child is Node3D and bool(child.get_meta(REGION_CONTAINER_META, false)) and \
			child.get_meta(REGION_LOCATION_META, Vector2i(9999, 9999)) == location:
			return child as Node3D
	return null


func _has_region_node_containers(region_nodes: Node3D) -> bool:
	if not is_instance_valid(region_nodes):
		return false
	for child: Node in region_nodes.get_children():
		if bool(child.get_meta(REGION_CONTAINER_META, false)):
			return true
	return false


func _build_region_node_descriptors(scene_root: Node, source_directory: String,
		location: Vector2i, world_region_size: float) -> Terrain3DRegionNodeBuildResult:
	var region_nodes := scene_root.get_node_or_null(NodePath(String(REGION_NODES_NAME))) as Node3D
	if not is_instance_valid(region_nodes):
		return Terrain3DRegionNodeBuildResult.failure("Workbench scene is missing the RegionNodes root")
	var region_container: Node3D = _get_region_node_container(region_nodes, location)
	if not is_instance_valid(region_container):
		# Version-2 workbenches stored the single region's nodes directly under RegionNodes.
		if _has_region_node_containers(region_nodes):
			return Terrain3DRegionNodeBuildResult.failure(
				"Workbench is missing the RegionNodes/%s container for checked-out region %s" % [
					_region_container_name(location), location])
		region_container = region_nodes
	if world_region_size <= 0.0:
		return Terrain3DRegionNodeBuildResult.failure("Workbench terrain has an invalid world region size")

	var result := Terrain3DRegionNodeBuildResult.success()
	var children: Array[Node] = region_container.get_children()
	if children.is_empty():
		return result

	result.node_directory = _unique_region_node_directory(source_directory, location)
	var mkdir_error: Error = DirAccess.make_dir_recursive_absolute(
		ProjectSettings.globalize_path(result.node_directory))
	if mkdir_error != OK:
		return Terrain3DRegionNodeBuildResult.failure(
			"Unable to create region-node directory: %s" % error_string(mkdir_error))

	var used_ids: Dictionary[int, bool] = {}
	var next_id: int = 1
	for child: Node in children:
		if not child is Node3D:
			_cleanup_generated_files(result.generated_paths)
			return Terrain3DRegionNodeBuildResult.failure(
				"Every direct child of RegionNodes must be a Node3D: %s" % child.name)
		if bool(child.get_meta(NODE_META_LOAD_ERROR, false)):
			_cleanup_generated_files(result.generated_paths)
			return Terrain3DRegionNodeBuildResult.failure(
				"Cannot archive missing region node %s. Restore its source scene first." % child.name)

		var node_root := child as Node3D
		var anchor_error: String = _validate_region_node_anchor(node_root, world_region_size)
		if not anchor_error.is_empty():
			_cleanup_generated_files(result.generated_paths)
			return Terrain3DRegionNodeBuildResult.failure(anchor_error)

		var requested_id: int = int(child.get_meta(NODE_META_ID, 0))
		var spawn_id: int = requested_id
		if spawn_id <= 0 or used_ids.has(spawn_id):
			while used_ids.has(next_id):
				next_id += 1
			spawn_id = next_id
			next_id += 1
		used_ids[spawn_id] = true
		child.set_meta(NODE_META_ID, spawn_id)

		var boundary_policy: int = int(child.get_meta(
			NODE_META_BOUNDARY_POLICY, BOUNDARY_POLICY_BOUNDS))
		if boundary_policy != BOUNDARY_POLICY_ANCHOR:
			boundary_policy = BOUNDARY_POLICY_BOUNDS
		child.set_meta(NODE_META_BOUNDARY_POLICY, boundary_policy)

		var coverage: Terrain3DRegionWorkbenchCoverage = _calculate_region_node_coverage(
			node_root, region_container, world_region_size)
		var coverage_min := Vector2i.ZERO
		var coverage_max := Vector2i.ZERO
		if boundary_policy == BOUNDARY_POLICY_BOUNDS:
			coverage_min = coverage.minimum
			coverage_max = coverage.maximum
		elif coverage.minimum != Vector2i.ZERO or coverage.maximum != Vector2i.ZERO:
			result.warnings.push_back(
				"Region node %s crosses its owner boundary but uses Anchor Only; it can pop when viewed from the neighboring region." % child.name)
		child.set_meta(NODE_META_COVERAGE_MIN, coverage_min)
		child.set_meta(NODE_META_COVERAGE_MAX, coverage_max)

		var covered_region_count: int = (coverage_max.x - coverage_min.x + 1) * \
			(coverage_max.y - coverage_min.y + 1)
		if boundary_policy == BOUNDARY_POLICY_BOUNDS and covered_region_count > 16:
			result.warnings.push_back(
				"Region node %s covers %d regions. It remains single-owned, but its owner region can be retained across that footprint." % [child.name, covered_region_count])

		var safe_name: String = String(child.name).validate_filename()
		if safe_name.is_empty():
			safe_name = "Node"
		var node_scene_path: String = result.node_directory.path_join(
			"%06d_%s.tscn" % [spawn_id, safe_name])
		var save_error: Error = _save_region_node_scene(node_root, node_scene_path)
		if save_error != OK:
			_cleanup_generated_files(result.generated_paths)
			return Terrain3DRegionNodeBuildResult.failure(
				"Unable to save region node %s: %s" % [child.name, error_string(save_error)])
		result.generated_paths.push_back(node_scene_path)

		var descriptor := Terrain3DRegionNodeDescriptor.new()
		descriptor.id = spawn_id
		descriptor.scene_path = node_scene_path
		descriptor.transform = node_root.transform
		descriptor.state = _metadata_state(child)
		descriptor.flags = int(child.get_meta(NODE_META_FLAGS, 0))
		descriptor.boundary_policy = boundary_policy
		descriptor.coverage_min = coverage_min
		descriptor.coverage_max = coverage_max
		descriptor.lighting_class = int(child.get_meta(NODE_META_LIGHTING_CLASS, 2))
		descriptor.baked_lighting_mode = int(child.get_meta(NODE_META_BAKED_LIGHTING_MODE, 1))
		descriptor.contribute_baked_gi = bool(child.get_meta(NODE_META_CONTRIBUTE_BAKED_GI, true))
		descriptor.receive_dynamic_directional = bool(child.get_meta(NODE_META_RECEIVE_DYNAMIC_DIRECTIONAL, true))
		descriptor.dynamic_shadow_eligible = bool(child.get_meta(NODE_META_DYNAMIC_SHADOW_ELIGIBLE, true))
		descriptor.shadow_proxy_scene_path = String(child.get_meta(NODE_META_SHADOW_PROXY_SCENE_PATH, ""))
		result.descriptors.push_back(descriptor)

	return result


func _metadata_state(node: Node) -> Dictionary[StringName, Variant]:
	var state: Dictionary[StringName, Variant] = {}
	var state_value: Variant = node.get_meta(NODE_META_STATE, {})
	if state_value is Dictionary:
		state.assign(state_value.duplicate(true))
	return state


func _validate_region_node_anchor(node_root: Node3D, world_region_size: float) -> String:
	var anchor: Vector3 = node_root.position
	var epsilon: float = maxf(0.0001, world_region_size * 0.000001)
	if anchor.x < -epsilon or anchor.z < -epsilon or \
			anchor.x >= world_region_size or anchor.z >= world_region_size:
		return "Region node %s has a root/pivot outside the checked-out region. Move its root inside this region or edit it from the destination region workbench." % node_root.name
	return ""


func _calculate_region_node_coverage(node_root: Node3D, region_nodes: Node3D,
		world_region_size: float) -> Terrain3DRegionWorkbenchCoverage:
	var points: Array[Vector3] = [node_root.position]
	var stack: Array[Node] = [node_root]
	var region_inverse: Transform3D = region_nodes.global_transform.affine_inverse()
	while not stack.is_empty():
		var current: Node = stack.pop_back()
		for descendant: Node in current.get_children():
			stack.push_back(descendant)
		if current is VisualInstance3D:
			var visual := current as VisualInstance3D
			var visual_bounds: AABB = visual.get_aabb()
			_append_transformed_aabb_points(points, visual_bounds,
				region_inverse * visual.global_transform)
		elif current is CollisionShape3D:
			var collision := current as CollisionShape3D
			if is_instance_valid(collision.shape):
				var debug_mesh: ArrayMesh = collision.shape.get_debug_mesh()
				if is_instance_valid(debug_mesh):
					_append_transformed_aabb_points(points, debug_mesh.get_aabb(),
						region_inverse * collision.global_transform)

	var bounds_min: Vector3 = points[0]
	var bounds_max: Vector3 = points[0]
	for point: Vector3 in points:
		bounds_min = Vector3(
			minf(bounds_min.x, point.x),
			minf(bounds_min.y, point.y),
			minf(bounds_min.z, point.z))
		bounds_max = Vector3(
			maxf(bounds_max.x, point.x),
			maxf(bounds_max.y, point.y),
			maxf(bounds_max.z, point.z))

	var epsilon: float = maxf(0.0001, world_region_size * 0.000001)
	var maximum_x: float = bounds_max.x - epsilon if bounds_max.x - bounds_min.x > epsilon else bounds_max.x
	var maximum_z: float = bounds_max.z - epsilon if bounds_max.z - bounds_min.z > epsilon else bounds_max.z
	return Terrain3DRegionWorkbenchCoverage.create(
		Vector2i(
			int(floorf(bounds_min.x / world_region_size)),
			int(floorf(bounds_min.z / world_region_size))),
		Vector2i(
			int(floorf(maximum_x / world_region_size)),
			int(floorf(maximum_z / world_region_size))))


func _append_transformed_aabb_points(points: Array[Vector3], bounds: AABB,
		transform_to_region: Transform3D) -> void:
	var start: Vector3 = bounds.position
	var finish: Vector3 = bounds.end
	for mask: int in range(8):
		var local_point := Vector3(
			finish.x if (mask & 1) != 0 else start.x,
			finish.y if (mask & 2) != 0 else start.y,
			finish.z if (mask & 4) != 0 else start.z)
		points.push_back(transform_to_region * local_point)


func _save_region_node_scene(source: Node3D, destination_path: String) -> Error:
	var duplicate_flags: int = Node.DUPLICATE_SIGNALS | Node.DUPLICATE_GROUPS | \
		Node.DUPLICATE_SCRIPTS | Node.DUPLICATE_USE_INSTANTIATION
	var clone := source.duplicate(duplicate_flags) as Node3D
	if not is_instance_valid(clone):
		return ERR_CANT_CREATE
	clone.transform = Transform3D.IDENTITY
	_remove_region_node_metadata(clone)
	_set_scene_owner_recursive(clone, clone)
	var packed_scene := PackedScene.new()
	var pack_error: Error = packed_scene.pack(clone)
	if pack_error != OK:
		clone.free()
		return pack_error
	var save_error: Error = ResourceSaver.save(packed_scene, destination_path)
	clone.free()
	return save_error


func _remove_region_node_metadata(node: Node) -> void:
	for key: StringName in [NODE_META_ID, NODE_META_FLAGS, NODE_META_STATE,
			NODE_META_SOURCE_PATH, NODE_META_LOAD_ERROR, NODE_META_BOUNDARY_POLICY,
			NODE_META_COVERAGE_MIN, NODE_META_COVERAGE_MAX]:
		if node.has_meta(key):
			node.remove_meta(key)


func _set_scene_owner_recursive(node: Node, scene_owner: Node) -> void:
	for child: Node in node.get_children():
		child.owner = scene_owner
		_set_scene_owner_recursive(child, scene_owner)


func _unique_region_node_directory(source_directory: String, location: Vector2i) -> String:
	var region_root: String = source_directory.path_join(REGION_NODE_DIR_NAME).path_join(
		"%d_%d" % [location.x, location.y])
	var base_name: String = _timestamp_id()
	var candidate: String = region_root.path_join(base_name)
	var suffix: int = 1
	while DirAccess.dir_exists_absolute(ProjectSettings.globalize_path(candidate)):
		candidate = region_root.path_join("%s_%02d" % [base_name, suffix])
		suffix += 1
	return candidate


func _cleanup_generated_files(paths: PackedStringArray) -> void:
	for path: String in paths:
		_remove_file_if_present(path)


func _property_name_set(object: Object) -> Dictionary[String, bool]:
	var names: Dictionary[String, bool] = {}
	for property_info: Dictionary in object.get_property_list():
		names[String(property_info.get("name", ""))] = true
	return names


func _open_scene_after_owner_release(scene_path: String) -> void:
	# Terrain3DData uses one process-static active storage owner. Defer opening
	# until the closed scene has completed teardown, then load the isolated owner.
	await get_tree().process_frame
	EditorInterface.get_resource_filesystem().scan_sources()
	EditorInterface.open_scene_from_path(scene_path)


func _open_source_after_commit(scene_path: String, session_directory: String) -> void:
	await get_tree().process_frame
	if not session_directory.is_empty():
		var remove_error: Error = _remove_directory_recursive(session_directory)
		if remove_error != OK:
			push_warning("Terrain3D: commit succeeded, but the completed Workbench session could not be fully removed: %s" % error_string(remove_error))
	EditorInterface.get_resource_filesystem().scan_sources()
	EditorInterface.open_scene_from_path(scene_path)


func _write_manifest(path: String, manifest: Terrain3DRegionWorkbenchManifest) -> Error:
	if path.is_empty():
		return ERR_INVALID_PARAMETER
	var staged_path: String = path + ".incoming"
	var backup_path: String = path + ".previous"
	var file := FileAccess.open(staged_path, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_string(JSON.stringify(manifest.to_dictionary(), "\t"))
	file.flush()
	file = null
	if FileAccess.file_exists(backup_path):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(backup_path))
	var had_original: bool = FileAccess.file_exists(path)
	if had_original:
		var backup_error: Error = DirAccess.rename_absolute(
			ProjectSettings.globalize_path(path), ProjectSettings.globalize_path(backup_path))
		if backup_error != OK:
			DirAccess.remove_absolute(ProjectSettings.globalize_path(staged_path))
			return backup_error
	var publish_error: Error = DirAccess.rename_absolute(
		ProjectSettings.globalize_path(staged_path), ProjectSettings.globalize_path(path))
	if publish_error != OK:
		if had_original and FileAccess.file_exists(backup_path):
			DirAccess.rename_absolute(ProjectSettings.globalize_path(backup_path), ProjectSettings.globalize_path(path))
		return publish_error
	if FileAccess.file_exists(backup_path):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(backup_path))
	return OK


func _read_manifest(path: String) -> Terrain3DRegionWorkbenchManifest:
	if path.is_empty() or not FileAccess.file_exists(path):
		return null
	return Terrain3DRegionWorkbenchManifest.from_value(
		JSON.parse_string(FileAccess.get_file_as_string(path)))


func _copy_file(source_path: String, destination_path: String) -> Error:
	var source := FileAccess.open(source_path, FileAccess.READ)
	if source == null:
		return FileAccess.get_open_error()
	var destination_parent: String = destination_path.get_base_dir()
	var mkdir_error: Error = DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(destination_parent))
	if mkdir_error != OK:
		return mkdir_error
	var destination := FileAccess.open(destination_path, FileAccess.WRITE)
	if destination == null:
		return FileAccess.get_open_error()
	const CHUNK_SIZE: int = 1024 * 1024
	while source.get_position() < source.get_length():
		var remaining: int = source.get_length() - source.get_position()
		destination.store_buffer(source.get_buffer(mini(CHUNK_SIZE, remaining)))
	destination.flush()
	return OK


func _replace_file(staged_path: String, destination_path: String) -> Error:
	var absolute_staged: String = ProjectSettings.globalize_path(staged_path)
	var absolute_destination: String = ProjectSettings.globalize_path(destination_path)
	if FileAccess.file_exists(destination_path):
		var remove_error: Error = DirAccess.remove_absolute(absolute_destination)
		if remove_error != OK:
			return remove_error
	return DirAccess.rename_absolute(absolute_staged, absolute_destination)


func _source_identifier(scene_path: String, data_directory: String) -> String:
	var basis: String = scene_path if not scene_path.is_empty() else data_directory
	var readable: String = basis.get_file().get_basename().validate_filename()
	if readable.is_empty():
		readable = "terrain"
	return "%s_%s" % [readable, basis.md5_text().left(8)]


func _timestamp_id() -> String:
	var time: Dictionary = Time.get_datetime_dict_from_system()
	return "%04d%02d%02d_%02d%02d%02d" % [
		int(time["year"]), int(time["month"]), int(time["day"]),
		int(time["hour"]), int(time["minute"]), int(time["second"]),
	]


func _unique_archive_directory(source_directory: String) -> String:
	var archive_root: String = source_directory.path_join(ARCHIVE_DIR_NAME)
	var base_name: String = _timestamp_id()
	var candidate: String = archive_root.path_join(base_name)
	var suffix: int = 1
	while DirAccess.dir_exists_absolute(ProjectSettings.globalize_path(candidate)):
		candidate = archive_root.path_join("%s_%02d" % [base_name, suffix])
		suffix += 1
	return candidate


func _remove_file_if_present(path: String) -> Error:
	if FileAccess.file_exists(path):
		return DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	return OK


func _snapshot_region_to_workbench(source_region: Terrain3DRegion, destination_path: String) -> Error:
	var snapshot := source_region.duplicate(true) as Terrain3DRegion
	if not is_instance_valid(snapshot):
		return ERR_CANT_CREATE
	return ResourceSaver.save(snapshot, destination_path)


func _initialize_lighting_outputs(manifest: Terrain3DRegionWorkbenchManifest,
		editable_locations: Array[Vector2i], workbench_directory: String) -> void:
	manifest.lighting_outputs.clear()
	var clusters: Dictionary[Vector2i, Array] = {}
	for location: Vector2i in editable_locations:
		var cluster_min := Vector2i(
			floori(float(location.x) / float(LIGHTMAP_CLUSTER_SIZE)) * LIGHTMAP_CLUSTER_SIZE,
			floori(float(location.y) / float(LIGHTMAP_CLUSTER_SIZE)) * LIGHTMAP_CLUSTER_SIZE)
		if not clusters.has(cluster_min):
			clusters[cluster_min] = []
		clusters[cluster_min].push_back(location)
	var source_dir: String = manifest.source_data_directory.path_join("lighting")
	var workbench_dir: String = workbench_directory.path_join("lighting")
	for cluster_min: Vector2i in clusters.keys():
		var members: Array[Vector2i] = []
		for raw_location: Variant in clusters[cluster_min]:
			if raw_location is Vector2i:
				members.push_back(raw_location)
		members.sort_custom(_region_location_less)
		var output := Terrain3DRegionWorkbenchLightingOutput.new()
		output.cluster_min = cluster_min
		output.cluster_max = cluster_min + Vector2i(LIGHTMAP_CLUSTER_SIZE - 1, LIGHTMAP_CLUSTER_SIZE - 1)
		output.region_locations = members
		var member_mask: int = 0
		for location: Vector2i in members:
			var local := location - cluster_min
			var bit_index: int = local.y * LIGHTMAP_CLUSTER_SIZE + local.x
			if bit_index >= 0 and bit_index < 31:
				member_mask |= 1 << bit_index
		# The membership mask prevents a partial 2x2 checkout from overwriting a
		# different package that shares the same global cluster origin.
		var base_name: String = "cluster_%d_%d_%dx%d_m%d" % [
			cluster_min.x, cluster_min.y, LIGHTMAP_CLUSTER_SIZE, LIGHTMAP_CLUSTER_SIZE, member_mask]
		output.source_light_data_path = source_dir.path_join(base_name + ".lmbake")
		output.source_package_path = source_dir.path_join(base_name + ".t3dlight.tres")
		output.workbench_light_data_path = workbench_dir.path_join(base_name + ".lmbake")
		output.workbench_package_path = workbench_dir.path_join(base_name + ".t3dlight.tres")
		output.source_light_data_md5 = FileAccess.get_md5(output.source_light_data_path) if FileAccess.file_exists(output.source_light_data_path) else ""
		output.source_package_md5 = FileAccess.get_md5(output.source_package_path) if FileAccess.file_exists(output.source_package_path) else ""
		manifest.lighting_outputs.push_back(output)
	manifest.lighting_outputs.sort_custom(func(a: Terrain3DRegionWorkbenchLightingOutput, b: Terrain3DRegionWorkbenchLightingOutput) -> bool:
		return a.cluster_min.y < b.cluster_min.y or (a.cluster_min.y == b.cluster_min.y and a.cluster_min.x < b.cluster_min.x))


func _unique_session_directory(source_id: String, checkout_count: int) -> String:
	var root: String = WORKBENCH_ROOT.path_join(source_id)
	var nonce: int = int(Time.get_ticks_usec() & 0x7fffffff)
	var base: String = "session_%s_%d_%08x" % [_timestamp_id(), checkout_count, nonce]
	var candidate: String = root.path_join(base)
	var suffix: int = 1
	while DirAccess.dir_exists_absolute(ProjectSettings.globalize_path(candidate)):
		candidate = root.path_join("%s_%02d" % [base, suffix])
		suffix += 1
	return candidate


func _ensure_checkout_progress_dialog() -> void:
	if is_instance_valid(_checkout_progress_dialog):
		return
	_checkout_progress_dialog = AcceptDialog.new()
	_checkout_progress_dialog.title = "Creating Terrain3D Region Workbench"
	_checkout_progress_dialog.min_size = Vector2i(520, 150)
	_checkout_progress_dialog.get_ok_button().text = "Cancel"
	_checkout_progress_dialog.confirmed.connect(func() -> void: _checkout_cancelled = true)
	_checkout_progress_dialog.close_requested.connect(func() -> void: _checkout_cancelled = true)
	var content := VBoxContainer.new()
	content.add_theme_constant_override("separation", 10)
	_checkout_progress_label = Label.new()
	_checkout_progress_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	content.add_child(_checkout_progress_label)
	_checkout_progress_bar = ProgressBar.new()
	_checkout_progress_bar.min_value = 0.0
	_checkout_progress_bar.max_value = 1.0
	_checkout_progress_bar.show_percentage = true
	content.add_child(_checkout_progress_bar)
	_checkout_progress_dialog.add_child(content)
	add_child(_checkout_progress_dialog)


func _begin_checkout_progress(total: int) -> void:
	_checkout_cancelled = false
	if total < CHECKOUT_PROGRESS_BATCH * 2:
		return
	_ensure_checkout_progress_dialog()
	_checkout_progress_dialog.title = "Creating Terrain3D Region Workbench"
	_checkout_progress_bar.value = 0.0
	_checkout_progress_label.text = "Preparing %d editable/context terrain snapshots..." % total
	_checkout_progress_dialog.popup_centered(Vector2i(560, 170))


func _update_checkout_progress(done: int, total: int, location: Vector2i) -> void:
	if not is_instance_valid(_checkout_progress_dialog) or not _checkout_progress_dialog.visible:
		return
	_checkout_progress_bar.value = float(done) / float(maxi(total, 1))
	_checkout_progress_label.text = "Snapshotting region %s — %d / %d" % [location, done, total]


func _end_checkout_progress() -> void:
	if is_instance_valid(_checkout_progress_dialog):
		_checkout_progress_dialog.hide()
	_checkout_cancelled = false


func _commit_preflight_failure(message: String) -> Terrain3DRegionWorkbenchResult:
	_end_checkout_progress()
	return _failure(message)


func _begin_commit_progress(total: int) -> void:
	_checkout_cancelled = false
	if total < CHECKOUT_PROGRESS_BATCH * 2:
		return
	_ensure_checkout_progress_dialog()
	_checkout_progress_dialog.title = "Preflighting Terrain3D Region Workbench Commit"
	_checkout_progress_bar.value = 0.0
	_checkout_progress_label.text = "Validating %d editable/context regions before any source files are changed..." % total
	_checkout_progress_dialog.popup_centered(Vector2i(600, 180))


func _update_commit_progress(done: int, total: int, message: String) -> void:
	if not is_instance_valid(_checkout_progress_dialog) or not _checkout_progress_dialog.visible:
		return
	_checkout_progress_bar.value = float(done) / float(maxi(total, 1))
	_checkout_progress_label.text = "%s — %d / %d" % [message, done, total]


func abandon_active_workbench() -> void:
	if not is_active_workbench():
		return
	if not is_instance_valid(_abandon_dialog):
		_abandon_dialog = ConfirmationDialog.new()
		_abandon_dialog.title = "Abandon Terrain3D Region Workbench"
		_abandon_dialog.dialog_text = "Discard this isolated Workbench session and return to the source scene? No source terrain files are changed by abandoning the session."
		_abandon_dialog.ok_button_text = "Abandon Workbench"
		_abandon_dialog.confirmed.connect(_confirm_abandon_workbench)
		add_child(_abandon_dialog)
	_abandon_dialog.popup_centered()


func _confirm_abandon_workbench() -> void:
	var root: Node = EditorInterface.get_edited_scene_root() if Engine.is_editor_hint() else null
	if not is_instance_valid(root):
		return
	var manifest_path: String = String(root.get_meta(MANIFEST_META, ""))
	var manifest: Terrain3DRegionWorkbenchManifest = _read_manifest(manifest_path)
	if manifest == null:
		return
	var session_directory: String = manifest.workbench_scene.get_base_dir()
	var source_scene: String = manifest.source_scene
	var close_error: Error = EditorInterface.close_scene()
	if close_error != OK:
		push_error("Unable to close the Workbench before abandoning it: %s" % error_string(close_error))
		return
	_finish_abandon_workbench.call_deferred(session_directory, source_scene)


func _finish_abandon_workbench(session_directory: String, source_scene: String) -> void:
	await get_tree().process_frame
	var remove_error: Error = _remove_directory_recursive(session_directory)
	if remove_error != OK:
		push_warning("Terrain3D: Workbench closed, but its session directory could not be fully removed: %s" % error_string(remove_error))
	if not source_scene.is_empty():
		EditorInterface.open_scene_from_path(source_scene)


func _remove_directory_recursive(path: String) -> Error:
	if path.is_empty() or not DirAccess.dir_exists_absolute(ProjectSettings.globalize_path(path)):
		return OK
	var dir := DirAccess.open(path)
	if dir == null:
		return DirAccess.get_open_error()
	dir.list_dir_begin()
	var name: String = dir.get_next()
	while not name.is_empty():
		if name != "." and name != "..":
			var child: String = path.path_join(name)
			var error: Error = _remove_directory_recursive(child) if dir.current_is_dir() else DirAccess.remove_absolute(ProjectSettings.globalize_path(child))
			if error != OK:
				dir.list_dir_end()
				return error
		name = dir.get_next()
	dir.list_dir_end()
	return DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


func _failure(message: String) -> Terrain3DRegionWorkbenchResult:
	return Terrain3DRegionWorkbenchResult.failure(message)
