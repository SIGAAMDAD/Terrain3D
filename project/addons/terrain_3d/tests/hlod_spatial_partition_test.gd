# Editor-only spatial HLOD partition regression fixture.
# Run with custom Godot editor: godot --headless --path project --script res://addons/terrain_3d/tests/hlod_spatial_partition_test.gd
extends SceneTree


func _initialize() -> void:
	var baker := Terrain3DHLODBaker.new()
	baker.layer_settings.spatial_partitioning = true
	baker.layer_settings.partition_triangle_limit = 100000
	baker.layer_settings.partition_section_limit = 32
	var children: Array = []
	for coord: Vector2i in [Vector2i(0, 0), Vector2i(1, 0), Vector2i(0, 1), Vector2i(1, 1)]:
		var child := Terrain3DHLODBaker.BuiltCluster.new()
		child.level = 1
		child.cluster = coord
		child.planned_triangles = 30000
		child.planned_sections = 4
		children.append(child)
	var no_sources: Dictionary[String, bool] = {}
	var cost := {"triangles": 120000, "sections": 16}
	if baker._partition_reason(2, Vector2i.ZERO, children, cost, no_sources) != "triangle_budget":
		_fail("Parent triangle budget did not produce a split")
		return
	cost["triangles"] = 40000
	cost["sections"] = 34
	if baker._partition_reason(2, Vector2i.ZERO, children, cost, no_sources) != "section_budget":
		_fail("Parent material section budget did not produce a split")
		return
	cost["sections"] = 4
	if not baker._partition_reason(2, Vector2i.ZERO, children, cost, no_sources).is_empty():
		_fail("Cheap parent was incorrectly split")
		return
	children[0].partition_terminal = true
	if baker._partition_reason(2, Vector2i.ZERO, children, cost, no_sources) != "inherited_subpartition":
		_fail("Terminal child did not force parent omission")
		return
	children[0].partition_terminal = false
	var higher := Terrain3DHLODBaker.Source.new()
	higher.min_level = 3
	higher.owner_region = Vector2i(Terrain3DHLODBaker.REGION_MAP_MIN, Terrain3DHLODBaker.REGION_MAP_MIN)
	var protected_sources: Dictionary[String, bool] = baker._collect_coarse_source_roots([higher])
	cost["triangles"] = 200000
	if not baker._partition_reason(2, Vector2i.ZERO, children, cost, protected_sources).is_empty():
		_fail("Source with coarser authored minimum level was stranded")
		return
	if not baker._partition_reason(1, Vector2i.ZERO, children, cost, no_sources).is_empty():
		_fail("Leaf HLOD was incorrectly split")
		return
	baker.layer_settings.spatial_partitioning = false
	if not baker._partition_reason(2, Vector2i.ZERO, children, cost, no_sources).is_empty():
		_fail("Disabled partitioning still split parents")
		return
	var hash_before: int = baker.layer_settings.get_recipe_hash()
	baker.layer_settings.partition_triangle_limit += 1000
	if hash_before == baker.layer_settings.get_recipe_hash():
		_fail("Partition budget did not invalidate the cook signature")
		return
	var jobs: Array[Dictionary] = [
		{"id": "1:0:0", "level": 1, "cluster": Vector2i.ZERO, "dependencies": PackedStringArray(), "action": "build"},
		{"id": "2:0:0", "level": 2, "cluster": Vector2i.ZERO,
		"dependencies": PackedStringArray(["1:0:0"]), "action": "split"},
	]
	if not bool(Terrain3DHLODBuildSchedule.validate_jobs(jobs).get("ok", false)):
		_fail("Child-first planner rejected terminal split job")
		return
	print("Spatial partition planner regression passed")
	quit(0)


func _fail(message: String) -> void:
	push_error(message)
	quit(1)
