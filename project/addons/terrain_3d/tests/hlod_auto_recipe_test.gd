# Invoke with custom editor: godot --headless --path project --script \
# res://addons/terrain_3d/tests/hlod_auto_recipe_test.gd
extends SceneTree


func _initialize() -> void:
	var baker := Terrain3DHLODBaker.new()
	baker.layer_settings.layer_type = 4
	var repeated := {"triangles": 950000, "sections": 4,
		"instances": 128, "prototypes": 2, "instance_candidates": true}
	if baker._resolve_cluster_recipe(repeated) != 1:
		_fail("Repeated instances were not grouped")
		return
	var small := {"triangles": 14000, "sections": 4,
		"instances": 1, "prototypes": 1, "instance_candidates": false}
	if baker._resolve_cluster_recipe(small) != 2:
		_fail("Small compatible cluster did not preserve topology")
		return
	var large := {"triangles": 280000, "sections": 28,
		"instances": 1, "prototypes": 0, "instance_candidates": false}
	if baker._resolve_cluster_recipe(large) != 3:
		_fail("High-cost cluster did not choose simplification")
		return
	var excessive := repeated.duplicate()
	excessive["instances"] = 65537
	if baker._resolve_cluster_recipe(excessive) == 1:
		_fail("Oversized instance group was accepted")
		return
	var unique := repeated.duplicate()
	unique["prototypes"] = 128
	if baker._resolve_cluster_recipe(unique) == 1:
		_fail("Unique-mesh group was incorrectly instanced")
		return
	var auto_hash: int = baker.layer_settings.get_recipe_hash()
	baker.layer_settings.loading_distance += 128.0
	if baker.layer_settings.get_recipe_hash() == auto_hash:
		_fail("Loading distance did not invalidate the cook recipe")
		return
	for explicit_layer: int in [1, 2, 3]:
		baker.layer_settings.layer_type = explicit_layer
		if baker._resolve_cluster_recipe(repeated) != explicit_layer:
			_fail("Explicit layer override was ignored")
			return
	baker.layer_settings.layer_type = 0
	if baker.layer_settings.layer_type != 3 or baker._resolve_cluster_recipe(repeated) != 3:
		_fail("Legacy recipe did not migrate to meshoptimizer simplification")
		return
	print("HLOD AUTO cost policy contracts passed")
	quit(0)


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
