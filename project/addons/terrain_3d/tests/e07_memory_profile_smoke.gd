# Execute with the matching Godot 4.7.2 custom editor or runtime template.
# --headless --editor --path project --script res://addons/terrain_3d/tests/e07_memory_profile_smoke.gd
@tool
extends SceneTree


func _initialize() -> void:
	call_deferred("_run_memory_profile_smoke")


func _run_memory_profile_smoke() -> void:
	if not ClassDB.class_exists("Terrain3D"):
		_fail("Terrain3D native class is not registered")
		return
	var terrain := Terrain3D.new()
	var runtime_total: int = terrain.get_total_memory_budget_mb()
	var runtime_transient: int = terrain.get_memory_transient_reserve_mb()
	var runtime_cooking: int = terrain.get_cooking_memory_budget_mb()
	var snapshot: Dictionary = terrain.get_memory_tracker_stats()
	if Engine.is_editor_hint():
		if snapshot.get("profile", "") != "editor_authoring":
			_fail("Editor did not select authoring memory profile", terrain)
			return
		if not terrain.set_editor_authoring_memory_profile_mb(6144, 384, 768):
			_fail("Editor profile override was rejected", terrain)
			return
		snapshot = terrain.get_memory_tracker_stats()
		if int(snapshot.get("effective_total_budget_mb", 0)) != 6144 or int(snapshot.get("effective_transient_reserve_mb", 0)) != 384:
			_fail("Editor effective account limits differ from overrides", terrain)
			return
		if int(snapshot.get("cooking_working_limit_bytes", 0)) != 768 * 1024 * 1024:
			_fail("Editor manual cooking cap was not selected", terrain)
			return
		if terrain.set_editor_authoring_memory_profile_mb(128, 384, 768):
			_fail("Invalid authoring account shrink was accepted", terrain)
			return
		terrain.set_editor_execution_mode(Terrain3D.EDITOR_EXECUTION_RUNTIME_PROFILE)
		snapshot = terrain.get_memory_tracker_stats()
		if snapshot.get("profile", "") != "runtime" or int(snapshot.get("effective_total_budget_mb", 0)) != runtime_total:
			_fail("Runtime profile did not restore scene budget", terrain)
			return
		terrain.set_editor_execution_mode(Terrain3D.EDITOR_EXECUTION_AUTHORING)
		snapshot = terrain.get_memory_tracker_stats()
		if int(snapshot.get("effective_total_budget_mb", 0)) != 6144:
			_fail("Returning to authoring lost editor override", terrain)
			return
	else:
		if terrain.set_editor_authoring_memory_profile_mb(6144, 384, 768):
			_fail("Runtime host accepted editor-only profile mutation", terrain)
			return
		if snapshot.get("profile", "") != "runtime" or int(snapshot.get("effective_total_budget_mb", 0)) != runtime_total:
			_fail("Runtime host did not retain shipping budget", terrain)
			return
	if terrain.get_total_memory_budget_mb() != runtime_total or terrain.get_memory_transient_reserve_mb() != runtime_transient or terrain.get_cooking_memory_budget_mb() != runtime_cooking:
		_fail("Editor override modified serialized runtime settings", terrain)
		return
	terrain.free()
	print("E07 MEMORY PROFILE HOST SMOKE PASS")
	quit(0)


func _fail(reason: String, terrain: Terrain3D = null) -> void:
	push_error("E07 MEMORY PROFILE HOST SMOKE FAIL: " + reason)
	if is_instance_valid(terrain):
		terrain.free()
	quit(1)
