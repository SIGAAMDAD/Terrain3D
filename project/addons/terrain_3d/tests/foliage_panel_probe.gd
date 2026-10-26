@tool
extends "res://addons/terrain_3d/tools/foliage_panel.gd"

# Test-only seams: native generation/configuration remain the real panel code.
# Successful publication is intercepted so no editor undo actions are registered.
var publication_requested: bool = false
var fail_staging: bool = false
var fail_restore: bool = false
var restore_attempted: bool = false
var snapshot_prefix: String = "user://fg00"

func _commit(_batches: Array) -> void:
	publication_requested = true

func commit_with_staging_failure(batches: Array) -> void:
	fail_staging = true
	super._commit(batches)

func _capture_snapshots(regions: Array[Vector2i]) -> PackedStringArray:
	if fail_staging:
		return PackedStringArray()
	return super._capture_snapshots(regions)

func _snapshot_path(index: int) -> String:
	return "%s_%d_%d.res" % [snapshot_prefix, _snapshot_serial, index]

func _restore_snapshots(regions: Array[Vector2i], paths: PackedStringArray) -> void:
	restore_attempted = true
	if not fail_restore:
		super._restore_snapshots(regions, paths)

func production_snapshot_path(index: int) -> String:
	return super._snapshot_path(index)
