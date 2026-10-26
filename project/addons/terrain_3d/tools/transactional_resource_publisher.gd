# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends RefCounted

# The operation boundary permits deterministic IO failure tests without touching
# user assets. Production callers use the ordinary Godot filesystem operations.
class FileOperations extends RefCounted:
	func exists(path: String) -> bool:
		return FileAccess.file_exists(path)

	func rename(source: String, destination: String) -> Error:
		return DirAccess.rename_absolute(ProjectSettings.globalize_path(source), ProjectSettings.globalize_path(destination))

	func remove(path: String) -> Error:
		return DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


static func _group_paths_are_distinct(final_paths: PackedStringArray, staging_paths: PackedStringArray,
		previous_paths: PackedStringArray) -> bool:
	if final_paths.is_empty() or staging_paths.size() != final_paths.size() or previous_paths.size() != final_paths.size():
		return false
	var seen: Dictionary[String, bool] = {}
	for group: PackedStringArray in [final_paths, staging_paths, previous_paths]:
		for path: String in group:
			if path.is_empty():
				return false
			var absolute: String = ProjectSettings.globalize_path(path).simplify_path()
			if OS.get_name() == "Windows":
				absolute = absolute.to_lower()
			if seen.has(absolute):
				return false
			seen[absolute] = true
	return true


# Recover before writing any staging file. Ambiguous recovery is preserved.
static func prepare_staged_group(final_paths: PackedStringArray, staging_paths: PackedStringArray,
		previous_paths: PackedStringArray, operations: FileOperations = null) -> Error:
	if not _group_paths_are_distinct(final_paths, staging_paths, previous_paths):
		return ERR_INVALID_PARAMETER
	var io: FileOperations = operations if operations != null else FileOperations.new()
	for i: int in range(final_paths.size()):
		if io.exists(previous_paths[i]) and io.exists(final_paths[i]):
			push_error("Unresolved previous publication retained at %s." % previous_paths[i])
			return ERR_ALREADY_EXISTS
	for i: int in range(final_paths.size()):
		if io.exists(previous_paths[i]):
			var recovery: Error = io.rename(previous_paths[i], final_paths[i])
			if recovery != OK:
				return recovery
		if io.exists(staging_paths[i]):
			var cleanup: Error = io.remove(staging_paths[i])
			if cleanup != OK:
				return cleanup
	return OK


static func _rollback_group(final_paths: PackedStringArray, previous_paths: PackedStringArray,
		backed_up: PackedByteArray, published: PackedByteArray, io: FileOperations) -> Error:
	var result: Error = OK
	for i: int in range(final_paths.size() - 1, -1, -1):
		if published[i] != 0:
			var removal: Error = io.remove(final_paths[i])
			if removal != OK:
				result = removal
				push_error("Rejected publication could not be removed: %s; backup preserved at %s." % [final_paths[i], previous_paths[i]])
				continue
		if backed_up[i] != 0:
			var restoration: Error = io.rename(previous_paths[i], final_paths[i])
			if restoration != OK:
				result = restoration
				push_error("Publication rollback failed; backup preserved at %s." % previous_paths[i])
	return result


# All prior members are backed up before any replacement becomes visible. A
# failed member restores the whole group; failed recovery retains its backup.
# This is an IO transaction, not an atomic multi-file update across power loss.
static func publish_staged_group(final_paths: PackedStringArray, staging_paths: PackedStringArray,
		previous_paths: PackedStringArray, operations: FileOperations = null) -> Error:
	if not _group_paths_are_distinct(final_paths, staging_paths, previous_paths):
		return ERR_INVALID_PARAMETER
	var io: FileOperations = operations if operations != null else FileOperations.new()
	for i: int in range(final_paths.size()):
		if io.exists(previous_paths[i]):
			return ERR_ALREADY_EXISTS
		if not io.exists(staging_paths[i]):
			return ERR_FILE_NOT_FOUND
	var backed_up: PackedByteArray = []
	var published: PackedByteArray = []
	backed_up.resize(final_paths.size())
	published.resize(final_paths.size())
	for i: int in range(final_paths.size()):
		if io.exists(final_paths[i]):
			var backup: Error = io.rename(final_paths[i], previous_paths[i])
			if backup != OK:
				var recovery: Error = _rollback_group(final_paths, previous_paths, backed_up, published, io)
				return recovery if recovery != OK else backup
			backed_up[i] = 1
	for i: int in range(final_paths.size()):
		var publication: Error = io.rename(staging_paths[i], final_paths[i])
		if publication != OK:
			var recovery: Error = _rollback_group(final_paths, previous_paths, backed_up, published, io)
			return recovery if recovery != OK else publication
		published[i] = 1
	var result: Error = OK
	for i: int in range(final_paths.size()):
		if backed_up[i] != 0:
			var cleanup: Error = io.remove(previous_paths[i])
			if cleanup != OK:
				result = cleanup
				push_error("Publication succeeded but backup cleanup failed at %s." % previous_paths[i])
	return result

# Publish a Resource through validated staging with rollback to the previous file
# if the final rename fails. Callers retain resource-specific validation policy.
static func publish(resource: Resource, final_path: String, staging_path: String, previous_path: String,
		expected_type: String, validator: Callable) -> Error:
	if resource == null or not _paths_are_distinct(final_path, staging_path, previous_path):
		return ERR_INVALID_PARAMETER
	# A previous file left by a failed rollback is recoverable state, not
	# disposable staging. Never erase it at the start of a later publication.
	var recovery_error: Error = _recover_previous(final_path, previous_path)
	if recovery_error != OK:
		return recovery_error
	var cleanup_error: Error = _remove_if_present(staging_path)
	if cleanup_error != OK:
		return cleanup_error
	var save_error: Error = ResourceSaver.save(resource, staging_path)
	if save_error != OK:
		return save_error
	var validation: Resource = ResourceLoader.load(
		staging_path, expected_type, ResourceLoader.CACHE_MODE_IGNORE) as Resource
	if not is_instance_valid(validation) or (validator.is_valid() and not bool(validator.call(validation))):
		_remove_if_present(staging_path)
		return ERR_FILE_CORRUPT
	return publish_staged(final_path, staging_path, previous_path)


# Text and binary writers share the same publication/recovery transaction after
# staging and validating their own format. A failed rollback keeps the backup.
static func publish_staged(final_path: String, staging_path: String, previous_path: String) -> Error:
	if not _paths_are_distinct(final_path, staging_path, previous_path):
		return ERR_INVALID_PARAMETER
	var recovery_error: Error = _recover_previous(final_path, previous_path)
	if recovery_error != OK:
		return recovery_error
	if not FileAccess.file_exists(staging_path):
		return ERR_FILE_NOT_FOUND
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
		var rollback_error: Error = OK
		if had_previous:
			rollback_error = DirAccess.rename_absolute(absolute_previous, absolute_final)
		_remove_if_present(staging_path)
		if rollback_error != OK:
			push_error("Publication and rollback failed; previous resource retained at %s: %s" % [previous_path, error_string(rollback_error)])
			return rollback_error
		return publish_error
	if had_previous:
		var cleanup_error: Error = _remove_if_present(previous_path)
		if cleanup_error != OK:
			push_warning("Publication succeeded, but the previous file was retained at %s: %s" % [previous_path, error_string(cleanup_error)])
	return OK


static func _paths_are_distinct(final_path: String, staging_path: String, previous_path: String) -> bool:
	if final_path.is_empty() or staging_path.is_empty() or previous_path.is_empty():
		return false
	var paths: PackedStringArray = []
	for path: String in [final_path, staging_path, previous_path]:
		var absolute_path: String = ProjectSettings.globalize_path(path).simplify_path()
		if OS.get_name() == "Windows":
			absolute_path = absolute_path.to_lower()
		if paths.has(absolute_path):
			return false
		paths.append(absolute_path)
	return true


static func _recover_previous(final_path: String, previous_path: String) -> Error:
	if not FileAccess.file_exists(previous_path):
		return OK
	if FileAccess.file_exists(final_path):
		# The helper cannot determine which file is authoritative after an
		# interrupted operation. Retain both rather than guess and delete one.
		push_error("Unresolved previous publication retained at %s; publication refused." % previous_path)
		return ERR_ALREADY_EXISTS
	var recovery_error: Error = DirAccess.rename_absolute(
		ProjectSettings.globalize_path(previous_path), ProjectSettings.globalize_path(final_path))
	if recovery_error != OK:
		push_error("Previous publication could not be restored from %s: %s" % [previous_path, error_string(recovery_error)])
	return recovery_error


static func _remove_if_present(path: String) -> Error:
	if path.is_empty() or not FileAccess.file_exists(path):
		return OK
	return DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
