# Offline HLOD scheduling contract. Invoke with the Godot editor headless:
# godot --headless --path project --script res://addons/terrain_3d/tests/hlod_schedule_test.gd
extends SceneTree

const Schedule = preload("res://addons/terrain_3d/tools/hlod_build_schedule.gd")


func _initialize() -> void:
	var valid: Array[Dictionary] = [
		{"id": "1:0:0", "level": 1, "cluster": Vector2i(0, 0), "dependencies": PackedStringArray()},
		{"id": "1:1:0", "level": 1, "cluster": Vector2i(1, 0), "dependencies": PackedStringArray()},
		{"id": "2:0:0", "level": 2, "cluster": Vector2i(0, 0),
			"dependencies": PackedStringArray(["1:0:0", "1:1:0"])},
	]
	if not bool(Schedule.validate_jobs(valid).get("ok", false)):
		push_error("Valid child-first HLOD plan was rejected")
		quit(1)
		return
	var missing: Array[Dictionary] = [valid[2], valid[0], valid[1]]
	if bool(Schedule.validate_jobs(missing).get("ok", false)):
		push_error("Out-of-order HLOD plan was accepted")
		quit(1)
		return
	var duplicate: Array[Dictionary] = [valid[0], valid[0]]
	if bool(Schedule.validate_jobs(duplicate).get("ok", false)):
		push_error("Duplicate HLOD job was accepted")
		quit(1)
		return
	var wrong_parent: Array[Dictionary] = [valid[0],
		{"id": "2:1:0", "level": 2, "cluster": Vector2i(1, 0),
			"dependencies": PackedStringArray(["1:0:0"]}]
	if bool(Schedule.validate_jobs(wrong_parent).get("ok", false)):
		push_error("HLOD child outside parent cell was accepted")
		quit(1)
		return
	var incomplete_parent: Array[Dictionary] = [valid[0], valid[1],
		{"id": "2:0:0", "level": 2, "cluster": Vector2i(0, 0),
			"dependencies": PackedStringArray(["1:0:0"]}]
	if bool(Schedule.validate_jobs(incomplete_parent).get("ok", false)):
		push_error("Incomplete HLOD child list was accepted")
		quit(1)
		return
	var missing_parent: Array[Dictionary] = [valid[0], valid[1],
		{"id": "2:1:0", "level": 2, "cluster": Vector2i(1, 0), "dependencies": PackedStringArray()}]
	if bool(Schedule.validate_jobs(missing_parent).get("ok", false)):
		push_error("Missing required HLOD parent was accepted")
		quit(1)
		return
	var missing_child: Array[Dictionary] = [valid[0],
		{"id": "2:0:0", "level": 2, "cluster": Vector2i(0, 0),
			"dependencies": PackedStringArray(["1:0:0", "1:1:0"])}]
	if bool(Schedule.validate_jobs(missing_child).get("ok", false)):
		push_error("Missing HLOD child was accepted")
		quit(1)
		return
	print("HLOD build scheduling contracts passed")
	quit(0)
