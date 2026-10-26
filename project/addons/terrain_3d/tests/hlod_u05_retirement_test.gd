# Invoke with the custom editor: godot --headless --path project --script \
# res://addons/terrain_3d/tests/hlod_u05_retirement_test.gd
extends SceneTree

func _initialize() -> void:
	var recipe := Terrain3DHLODLayerSettings.new()
	if recipe.layer_type != 3:
		_fail("New HLOD recipe default must use meshoptimizer SIMPLIFIED")
		return
	var original_hash: int = recipe.get_recipe_hash()
	recipe.layer_type = 0
	if recipe.layer_type != 3 or recipe.get_recipe_hash() != original_hash:
		_fail("Legacy authoring recipe 0 did not migrate deterministically")
		return
	var baker := Terrain3DHLODBaker.new()
	baker.layer_settings.layer_type = 0
	if baker._resolve_cluster_recipe({}) != 3:
		_fail("Legacy recipe was allowed to schedule obsolete geometry cooking")
		return
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 0, 1)])
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2])
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var builder := Terrain3DHLODBuilder.new()
	var invalid: Dictionary = builder.simplify_mesh_with_error(mesh, 0.5, -1.0)
	if bool(invalid.get("ok", false)):
		_fail("Negative world-error budget was accepted")
		return
	var exact: Dictionary = builder.simplify_mesh_with_error(mesh, 1.0, 0.0)
	if not bool(exact.get("ok", false)) or int(exact.get("proxy_triangles", 0)) != 1:
		_fail("Zero-error meshoptimizer cook rejected a valid triangle")
		return
	var compat: Dictionary = builder.simplify_mesh(mesh, 1.0)
	if not bool(compat.get("ok", false)):
		_fail("Legacy native method binding no longer dispatches to meshoptimizer")
		return
	print("HLOD U05 retirement and recipe migration checks passed")
	quit(0)

func _fail(message: String) -> void:
	push_error(message)
	quit(1)
