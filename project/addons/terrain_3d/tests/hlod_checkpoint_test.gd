# Test with: godot --headless --path project --script res://addons/terrain_3d/tests/hlod_checkpoint_test.gd
extends SceneTree

const Checkpoint = preload("res://addons/terrain_3d/tools/hlod_build_checkpoint.gd")


func _initialize() -> void:
	var jobs: Array[Dictionary] = [
		{"id": "1:0:0", "source_signature": 500, "dependencies": PackedStringArray()},
		{"id": "2:0:0", "source_signature": 700, "dependencies": PackedStringArray(["1:0:0"])},
	]
	var key: String = Checkpoint.plan_key(jobs, 12345, "res://test_hlod_manifest.tres")
	var reordered: Array[Dictionary] = [jobs[1], jobs[0]]
	if key == Checkpoint.plan_key(reordered, 12345, "res://test_hlod_manifest.tres"):
		_fail("Reordered dependencies accepted by fingerprint")
		return
	if key == Checkpoint.plan_key(jobs, 12346, "res://test_hlod_manifest.tres"):
		_fail("Changed build recipe accepted by fingerprint")
		return
	var checkpoint_path: String = "user://terrain3d_hlod_checkpoint_contract_test.json"
	var completed: Dictionary = {"1:0:0": {
		"package_path": "res://unchanged.res",
		"source_signature": "9223372036854775806",
		"build_signature": "12345",
	}}
	var saved: Error = Checkpoint.save_completed(checkpoint_path, key, completed)
	if saved != OK:
		_fail("Unable to save recovery checkpoint: %s" % error_string(saved))
		return
	var loaded: Dictionary = Checkpoint.load_completed(checkpoint_path, key)
	if loaded.size() != 1 or String(loaded["1:0:0"]["source_signature"]) != "9223372036854775806":
		_fail("Checkpoint roundtrip lost the exact 64-bit signature")
		return
	if not Checkpoint.load_completed(checkpoint_path, key + "stale").is_empty():
		_fail("Stale plan fingerprint accepted")
		return
	# Simulate interruption after moving the old journal to its backup path.
	var backup_path: String = checkpoint_path + ".previous"
	if DirAccess.rename_absolute(ProjectSettings.globalize_path(checkpoint_path),
			ProjectSettings.globalize_path(backup_path)) != OK:
		_fail("Unable to stage interruption simulation")
		return
	if Checkpoint.load_completed(checkpoint_path, key).size() != 1:
		_fail("Recovery journal backup was not readable")
		return
	if Checkpoint.save_completed(checkpoint_path, key, completed) != OK:
		_fail("Recovery journal could not restore interrupted publication")
		return
	if Checkpoint.clear_completed(checkpoint_path) != OK:
		_fail("Checkpoint cleanup failed")
		return
	if not Checkpoint.load_completed(checkpoint_path, key).is_empty():
		_fail("Cleared journal was reused")
		return
	print("HLOD checkpoint contracts passed")
	quit(0)


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
