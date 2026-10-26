# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Transactionally offsets every authored region in a Terrain3D data directory.
# Runtime pages/far data/HLOD are invalidated because their spatial keys embed
# the old region locations. Regional lightmap references are cleared for the
# same reason and must be rebaked at their new world positions.
@tool
extends Terrain3D

const INVALIDATED_DATASET_FILES := [
	"terrain3d_instances.t3dvt",
	"terrain3d_far_atlas.t3dfar", # One-time cleanup of obsolete cooked assets.
	"terrain3d_far.t3dfar",
	"terrain3d_world.t3dmanifest",
	"terrain3d_cook.t3ddb",
	"terrain3d_hlod_manifest.tres",
]

class MovePlan extends RefCounted:
	var source_location: Vector2i
	var target_location: Vector2i
	var source_name: String
	var target_name: String
	var temporary_name: String

@export var offset: Vector2i = Vector2i.ZERO
@export_tool_button("Move Regions") var run: Callable = move_regions


func move_regions() -> void:
	if offset == Vector2i.ZERO:
		return
	var directory_path: String = data_directory.simplify_path().trim_suffix("/")
	if directory_path.is_empty():
		push_error("Terrain3D region move requires a valid data_directory.")
		return
	var directory := DirAccess.open(directory_path)
	if directory == null:
		push_error("Unable to open Terrain3D data directory: %s" % directory_path)
		return

	var plans: Array[MovePlan] = _build_move_plan(directory_path, directory)
	if plans.is_empty():
		return
	if not _validate_move_plan(directory_path, plans):
		return

	# Unload the repository before touching serialized region files so no live
	# Resource path remains authoritative while the two-phase rename is active.
	data_directory = ""
	var staged_count: int = _stage_sources(directory, plans)
	if staged_count != plans.size():
		_restore_staged_sources(directory, plans, staged_count)
		data_directory = directory_path
		return

	var published_count: int = _publish_targets(directory, plans)
	if published_count != plans.size():
		_restore_published_targets(directory, plans, published_count)
		_restore_staged_sources(directory, plans, plans.size())
		data_directory = directory_path
		return

	_invalidate_location_dependent_artifacts(directory_path, directory, plans)
	data_directory = directory_path
	_finalize_relocated_regions(directory_path, plans)
	EditorInterface.get_resource_filesystem().scan()
	print("Terrain3D: moved %d regions by %s and invalidated location-dependent runtime/HLOD/lightmap data." % [plans.size(), offset])


func _build_move_plan(directory_path: String, directory: DirAccess) -> Array[MovePlan]:
	var plans: Array[MovePlan] = []
	var files: PackedStringArray = directory.get_files()
	files.sort()
	var serial: int = 0
	for source_name: String in files:
		if not source_name.begins_with("terrain3d") or not source_name.ends_with(".res"):
			continue
		var source_location: Vector2i = Terrain3DUtil.filename_to_location(source_name)
		if Terrain3DData.get_region_map_index(source_location) < 0:
			push_error("Invalid Terrain3D region filename: %s" % directory_path.path_join(source_name))
			return []
		var target_location: Vector2i = source_location + offset
		if Terrain3DData.get_region_map_index(target_location) < 0:
			push_error("Region %s would move outside the current Terrain3D region map to %s." % [source_location, target_location])
			return []
		var target_name: String = Terrain3DUtil.location_to_filename(target_location)
		var temporary_name: String = ".terrain3d_move_%d_%d.tmp" % [Time.get_ticks_usec(), serial]
		serial += 1
		var plan := MovePlan.new()
		plan.source_location = source_location
		plan.target_location = target_location
		plan.source_name = source_name
		plan.target_name = target_name
		plan.temporary_name = temporary_name
		plans.append(plan)
	return plans


func _validate_move_plan(directory_path: String, plans: Array[MovePlan]) -> bool:
	var source_names: Dictionary[String, bool] = {}
	var target_names: Dictionary[String, bool] = {}
	for plan: MovePlan in plans:
		source_names[plan.source_name] = true
	for plan: MovePlan in plans:
		var target_name: String = plan.target_name
		if target_names.has(target_name):
			push_error("Terrain3D region move generated duplicate target: %s" % target_name)
			return false
		target_names[target_name] = true
		if FileAccess.file_exists(directory_path.path_join(target_name)) and not source_names.has(target_name):
			push_error("Terrain3D region move would overwrite existing region file: %s" % target_name)
			return false
	return true


func _stage_sources(directory: DirAccess, plans: Array[MovePlan]) -> int:
	for index: int in range(plans.size()):
		var plan: MovePlan = plans[index]
		var error: Error = directory.rename(plan.source_name, plan.temporary_name)
		if error != OK:
			push_error("Unable to stage Terrain3D region %s: %s" % [plan.source_name, error_string(error)])
			return index
	return plans.size()


func _publish_targets(directory: DirAccess, plans: Array[MovePlan]) -> int:
	for index: int in range(plans.size()):
		var plan: MovePlan = plans[index]
		var error: Error = directory.rename(plan.temporary_name, plan.target_name)
		if error != OK:
			push_error("Unable to publish moved Terrain3D region %s: %s" % [plan.target_name, error_string(error)])
			return index
	return plans.size()


func _restore_published_targets(directory: DirAccess, plans: Array[MovePlan], published_count: int) -> void:
	for index: int in range(published_count - 1, -1, -1):
		var plan: MovePlan = plans[index]
		var error: Error = directory.rename(plan.target_name, plan.temporary_name)
		if error != OK:
			push_error("Failed to roll back moved Terrain3D region %s: %s" % [plan.target_name, error_string(error)])


func _restore_staged_sources(directory: DirAccess, plans: Array[MovePlan], staged_count: int) -> void:
	for index: int in range(staged_count - 1, -1, -1):
		var plan: MovePlan = plans[index]
		if not directory.file_exists(plan.temporary_name):
			continue
		var error: Error = directory.rename(plan.temporary_name, plan.source_name)
		if error != OK:
			push_error("Failed to restore Terrain3D region %s: %s" % [plan.source_name, error_string(error)])


func _invalidate_location_dependent_artifacts(directory_path: String, directory: DirAccess, plans: Array[MovePlan]) -> void:
	var page_names: Dictionary[String, bool] = {}
	for plan: MovePlan in plans:
		page_names[plan.source_name.get_basename() + ".t3dpage"] = true
		page_names[plan.target_name.get_basename() + ".t3dpage"] = true
	for page_name: String in page_names:
		_remove_relative_file(directory, page_name)
	for file_name: String in INVALIDATED_DATASET_FILES:
		_remove_relative_file(directory, file_name)
	# Regional V1 page IDs are location-keyed; all old pages must be retired.
	var regional_vt_directory: String = directory_path.path_join("terrain3d_far_vt")
	var vt_folder := DirAccess.open(regional_vt_directory)
	if vt_folder != null:
		for file_name: String in vt_folder.get_files():
			if file_name.ends_with(".t3avt"):
				_remove_relative_file(vt_folder, file_name)
	# A custom HLOD manifest inside the terrain data directory is just as stale as
	# the default manifest after a region-coordinate shift.
	var configured_hlod_path: String = hlod_manifest_path.simplify_path()
	if not configured_hlod_path.is_empty() and configured_hlod_path.get_base_dir() == directory_path:
		_remove_relative_file(directory, configured_hlod_path.get_file())


func _remove_relative_file(directory: DirAccess, file_name: String) -> void:
	if not directory.file_exists(file_name):
		return
	var error: Error = directory.remove(file_name)
	if error != OK:
		push_warning("Unable to invalidate generated Terrain3D artifact %s: %s" % [file_name, error_string(error)])


func _finalize_relocated_regions(directory_path: String, plans: Array[MovePlan]) -> void:
	if data == null:
		push_error("Terrain3D data failed to reload after moving regions.")
		return
	for plan: MovePlan in plans:
		var target_location: Vector2i = plan.target_location
		var region: Terrain3DRegion = data.get_region(target_location)
		if not is_instance_valid(region):
			push_error("Moved Terrain3D region failed to reload at %s." % target_location)
			continue
		# Filename normalization updates the serialized region location during load.
		# Baked regional lighting is world-position dependent and cannot follow the
		# region to its new coordinate unchanged.
		if region.has_lighting():
			region.set_lighting(null)
		region.set_modified(true)
	data.save_directory(directory_path)
	reload_hlod_manifest()
