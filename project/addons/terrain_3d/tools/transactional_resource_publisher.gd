# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends RefCounted

# Publish a Resource through validated staging with rollback to the previous file
# if the final rename fails. Callers retain resource-specific validation policy.
static func publish(resource: Resource, final_path: String, staging_path: String, previous_path: String,
		expected_type: String, validator: Callable) -> Error:
	if resource == null or final_path.is_empty() or staging_path.is_empty() or previous_path.is_empty():
		return ERR_INVALID_PARAMETER
	for cleanup_path: String in [staging_path, previous_path]:
		_remove_if_present(cleanup_path)
	var save_error: Error = ResourceSaver.save(resource, staging_path)
	if save_error != OK:
		return save_error
	var validation: Resource = ResourceLoader.load(
		staging_path, expected_type, ResourceLoader.CACHE_MODE_IGNORE) as Resource
	if not is_instance_valid(validation) or (validator.is_valid() and not bool(validator.call(validation))):
		_remove_if_present(staging_path)
		return ERR_FILE_CORRUPT

	var absolute_final: String = ProjectSettings.globalize_path(final_path)
	var absolute_staging: String = ProjectSettings.globalize_path(staging_path)
	var absolute_previous: String = ProjectSettings.globalize_path(previous_path)
	var had_previous: bool = FileAccess.file_exists(final_path)
	if had_previous:
		var backup_error: Error = DirAccess.rename_absolute(absolute_final, absolute_previous)
		if backup_error != OK:
			_remove_if_present(staging_path)
			return backup_error
	var publish_error: Error = DirAccess.rename_absolute(absolute_staging, absolute_final)
	if publish_error != OK:
		if had_previous:
			DirAccess.rename_absolute(absolute_previous, absolute_final)
		_remove_if_present(staging_path)
		return publish_error
	if had_previous:
		_remove_if_present(previous_path)
	return OK


static func _remove_if_present(path: String) -> Error:
	if path.is_empty() or not FileAccess.file_exists(path):
		return OK
	return DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
