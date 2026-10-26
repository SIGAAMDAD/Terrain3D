# Copyright (c) 2023-2026 Terrain3D contributors.
@tool
extends RefCounted
class_name Terrain3DHLODBuildCheckpoint

# Recovery journal for editor-only HLOD package baking. The published runtime
# manifest is never read from this file. A completed job can only be resumed
# after the caller validates its cooked package against the current recipe and
# source signature. Child-first package dependencies are included in plan_key.
const FORMAT: int = 1
const FILE_NAME: String = "build_checkpoint.json"


static func plan_key(jobs: Array[Dictionary], build_signature: int, manifest_path: String) -> String:
	var lines: PackedStringArray = PackedStringArray()
	lines.append("terrain3d-hlod-checkpoint-v%d|build=%d|manifest=%s" % [
		FORMAT, build_signature, manifest_path])
	for job: Dictionary in jobs:
		var dependencies: PackedStringArray = job.get("dependencies", PackedStringArray())
		lines.append("%s|%d|%s|%s" % [
			String(job.get("id", "")), int(job.get("source_signature", -1)),
			",".join(dependencies), "split" if String(job.get("action", "")) == "split" else "package"])
	return "\n".join(lines).sha256_text()


static func load_completed(path: String, key: String) -> Dictionary:
	# A crash between backup and publication leaves only .previous. It is
	# the last complete checkpoint and can be recovered by save_completed().
	var readable_path: String = path
	if not FileAccess.file_exists(readable_path):
		readable_path = path + ".previous"
	if not FileAccess.file_exists(readable_path):
		return {}
	var file := FileAccess.open(readable_path, FileAccess.READ)
	if file == null:
		return {}
	var raw: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if not raw is Dictionary:
		return {}
	var journal: Dictionary = raw
	if int(journal.get("format", -1)) != FORMAT or String(journal.get("plan_key", "")) != key:
		return {}
	var completed: Variant = journal.get("completed", {})
	if not completed is Dictionary:
		return {}
	return completed


# Small, validated JSON checkpoint with durable staging/backup publication.
# Do not delete an ambiguous backup after a failed rename: preserve it for
# inspection or recovery instead of corrupting an existing work journal.
static func save_completed(path: String, key: String, completed: Dictionary) -> Error:
	if path.is_empty() or key.is_empty():
		return ERR_INVALID_PARAMETER
	var staging: String = path + ".staging"
	var previous: String = path + ".previous"
	var final_abs: String = ProjectSettings.globalize_path(path)
	var stage_abs: String = ProjectSettings.globalize_path(staging)
	var prev_abs: String = ProjectSettings.globalize_path(previous)
	if FileAccess.file_exists(previous):
		if FileAccess.file_exists(path):
			return ERR_ALREADY_EXISTS
		var recovered: Error = DirAccess.rename_absolute(prev_abs, final_abs)
		if recovered != OK:
			return recovered
	if FileAccess.file_exists(staging):
		var cleanup: Error = DirAccess.remove_absolute(stage_abs)
		if cleanup != OK:
			return cleanup
	var file := FileAccess.open(staging, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_string(JSON.stringify({
		"format": FORMAT, "plan_key": key, "completed": completed,
	}))
	file.flush()
	var write_error: Error = file.get_error()
	file.close()
	if write_error != OK:
		return write_error
	var backed_up: bool = FileAccess.file_exists(path)
	if backed_up:
		var backup_error: Error = DirAccess.rename_absolute(final_abs, prev_abs)
		if backup_error != OK:
			return backup_error
	var publication: Error = DirAccess.rename_absolute(stage_abs, final_abs)
	if publication != OK:
		if backed_up:
			var restored: Error = DirAccess.rename_absolute(prev_abs, final_abs)
			if restored != OK:
				return restored
		return publication
	if backed_up:
		var discard: Error = DirAccess.remove_absolute(prev_abs)
		if discard != OK:
			return discard
	return OK


static func clear_completed(path: String) -> Error:
	if not FileAccess.file_exists(path):
		return OK
	return DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
