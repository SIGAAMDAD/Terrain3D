@tool
extends RefCounted

const AnchorScript = preload("res://addons/terrain3d_scene_bridge/bridge_anchor.gd")
const SceneRulesScript = preload("res://addons/terrain3d_scene_bridge/bridge_scene_rules.gd")
const RegionIndexScript = preload("res://addons/terrain3d_scene_bridge/bridge_region_index.gd")
const TerrainIOScript = preload("res://addons/terrain3d_scene_bridge/bridge_terrain_io.gd")
const META_ANCHOR: StringName = &"terrain3d_scene_bridge_anchor"
const META_BRIDGE_ID: StringName = &"terrain3d_scene_bridge_id"
const META_SCENE_PATH: StringName = &"terrain3d_scene_bridge_scene_path"
const META_MESH_PATH: StringName = &"terrain3d_scene_bridge_mesh_path"
const META_ASSET_KEY: StringName = &"terrain3d_scene_bridge_asset_key"
const META_TYPE_NAME: StringName = &"terrain3d_scene_bridge_type_name"
const META_INSTANCE_CATEGORY: StringName = &"terrain3d_instance_category"
const META_OWNER_ID: StringName = &"terrain3d_scene_bridge_owner_id"
const META_SOURCE_PATH: StringName = &"terrain3d_scene_bridge_source_path"
const META_OWNER_CLEARED: StringName = &"terrain3d_scene_bridge_owner_cleared"
const MAX_MESH_ASSETS: int = 256
const PROGRESS_MIN_INTERVAL_MSEC: int = 33
const PROGRESS_MIN_DELTA: float = 0.001
const PROGRESS_BATCH_STRIDE: int = 64
const UPDATE_ALL_REGIONS: Vector2i = Vector2i(2147483647, 2147483647)

signal log_message(message: String)
signal progress_changed(value: float, status: String)
signal operation_finished(success: bool, message: String)

var _scene_rules: SceneRulesScript = SceneRulesScript.new()
var _region_index: RegionIndexScript = RegionIndexScript.new()
var _terrain_io: TerrainIOScript = TerrainIOScript.new()
var _transactions: Dictionary[int, Dictionary] = {}
var _next_transaction_id: int = 1
var _last_progress_msec: int = 0
var _last_progress_value: float = -1.0
var _last_progress_status: String = ""

func scan_source(source_root: Node, report_progress: bool = true, progress_callback: Callable = Callable()) -> Dictionary:
	if report_progress:
		_begin_progress("Scanning scene source")
	if source_root == null or not is_instance_valid(source_root):
		return _failure("Source node is null.")
	if not source_root is Node3D:
		return _failure("Source must be a Node3D containing Visual/Ground + Visual/Landmarks or Region_<X>_<Y> nodes.")

	var scan_progress: Callable = progress_callback
	if not scan_progress.is_valid() and report_progress:
		scan_progress = Callable(self, "_forward_scan_progress")
	var scan: Dictionary = _region_index.scan_source(source_root, _scene_rules, scan_progress)
	if not bool(scan.get("ok", false)):
		return _failure(String(scan.get("error", "Region scan failed.")))

	var placements: Array = scan.get("placements", [])
	var type_counts: Dictionary = {}
	for placement: Dictionary in placements:
		var asset_key: String = String(placement.get("asset_key", placement.get("scene_path", "")))
		type_counts[asset_key] = int(type_counts.get(asset_key, 0)) + 1

	scan["placement_count"] = placements.size()
	scan["type_count"] = type_counts.size()
	scan["type_counts"] = type_counts
	if report_progress:
		_emit_progress(1.0, "Source scan complete", true)
	return scan

func _begin_progress(status: String) -> void:
	_last_progress_msec = 0
	_last_progress_value = -1.0
	_last_progress_status = ""
	_emit_progress(0.0, status, true)


func _emit_progress(value: float, status: String, force: bool = false) -> void:
	var clamped_value: float = clampf(value, 0.0, 1.0)
	var now_msec: int = Time.get_ticks_msec()
	var status_changed: bool = status != _last_progress_status
	var interval_elapsed: bool = now_msec - _last_progress_msec >= PROGRESS_MIN_INTERVAL_MSEC
	var value_changed: bool = absf(clamped_value - _last_progress_value) >= PROGRESS_MIN_DELTA
	if not force and not status_changed and not (interval_elapsed and value_changed):
		return
	_last_progress_msec = now_msec
	_last_progress_value = clamped_value
	_last_progress_status = status
	progress_changed.emit(clamped_value, status)


func _forward_scan_progress(value: float, status: String) -> void:
	_emit_progress(value, status)


func _forward_bake_scan_progress(value: float, status: String) -> void:
	_emit_progress(0.08 * clampf(value, 0.0, 1.0), status)


func _forward_bake_snapshot_progress(value: float, status: String) -> void:
	_emit_progress(0.48 + 0.07 * clampf(value, 0.0, 1.0), status)



func _forward_restore_read_progress(value: float, status: String) -> void:
	_emit_progress(0.08 + 0.20 * clampf(value, 0.0, 1.0), status)


func find_anchors(terrain: Node) -> Array[Node]:
	var anchors: Array[Node] = []
	if terrain == null or not is_instance_valid(terrain):
		return anchors
	_collect_anchors_recursive(terrain, anchors)
	anchors.sort_custom(_sort_anchor_by_name)
	return anchors

func get_subdivision_info(terrain: Node) -> Dictionary:
	return _terrain_io.get_subdivision_info(terrain)


func prepare_bake(
		scene_root: Node,
		terrain: Node,
		source_root: Node3D,
		remove_source: bool,
		skip_missing_regions: bool
	) -> Dictionary:
	_begin_progress("Preparing Terrain3D bake")
	var prepare_started_msec: int = Time.get_ticks_msec()
	var validation_error: String = _validate_common(scene_root, terrain)
	if not validation_error.is_empty():
		return _failure(validation_error)
	if source_root == null or not is_instance_valid(source_root):
		return _failure("Source Node3D is missing.")
	if source_root == scene_root:
		return _failure("The edited scene root cannot be used as the removable source container.")
	if source_root.get_parent() == null:
		return _failure("The source node must be inside the edited scene.")
	if not scene_root.is_ancestor_of(source_root):
		return _failure("The source node is not inside the edited scene tree.")
	if source_root == terrain or source_root.is_ancestor_of(terrain):
		return _failure("The source container cannot contain the Terrain3D target.")
	var scan: Dictionary = scan_source(source_root, false, Callable(self, "_forward_bake_scan_progress"))
	if not bool(scan.get("ok", false)):
		return scan
	var placements: Array = scan.get("placements", [])
	if placements.is_empty():
		return _failure("No GLB/GLTF/PackedScene MeshInstance3D placements were found beneath Visual/Ground, Visual/Landmarks, or the regional source hierarchy.")
	var skipped_nodes: Array = scan.get("skipped_nodes", [])
	var source_mode: String = String(scan.get("source_mode", "regional"))
	var partial_source_removal: bool = source_mode == "visual_roots"
	if remove_source and not partial_source_removal and not skipped_nodes.is_empty():
		return _failure(
			"The source container has %d unsupported leaf node(s). Removing the whole container would delete them. Move those nodes elsewhere or disable source removal."
			% skipped_nodes.size()
		)

	# Visual/Ground + Visual/Landmarks is a mixed authored hierarchy. Removing the
	# entire Visual node would also delete scripts, helper meshes, empty grouping
	# nodes, and unrelated authored content. In this mode remove only the external
	# scene-instance roots that were actually accepted by the scanner.
	var source_removal_entries: Array[Dictionary] = []
	if remove_source and partial_source_removal:
		var removal_paths: Array = scan.get("converted_scene_node_paths", [])
		for path_variant in removal_paths:
			var removal_path: NodePath = NodePath(String(path_variant))
			var removal_node: Node = source_root.get_node_or_null(removal_path)
			if removal_node == null or removal_node == source_root:
				continue
			var removal_parent: Node = removal_node.get_parent()
			if removal_parent == null:
				continue
			source_removal_entries.append({
				"node": removal_node,
				"parent": removal_parent,
				"owner": removal_node.owner,
				"index": removal_node.get_index(),
				"path": removal_path,
			})

	var data: Object = terrain.call("get_data")
	var assets: Object = terrain.call("get_assets")
	var instancer: Object = terrain.call("get_instancer")
	if data == null or assets == null or instancer == null:
		return _failure("Terrain3D is not fully initialized. Data, assets, or instancer is null.")

	var subdivision_info: Dictionary = _terrain_io.get_subdivision_info(terrain)
	var subdivision_error: String = _terrain_io.validate_subdivision_info(subdivision_info)
	if not subdivision_error.is_empty():
		return _failure(subdivision_error)
	var native_owner_supported: bool = bool(subdivision_info.get("owner_supported", false))

	# Region_<X>_<Y> parenting is source organization only. Current Terrain3D
	# routes each native instance from its height-offset-adjusted transform origin,
	# then derives the local instancer cell. A scaled mesh may extend beyond that
	# cell or beyond the owning 128 m region without changing ownership.
	var missing_region_count: int = 0

	var bridge_id: String = String(source_root.get_meta(META_BRIDGE_ID, ""))
	if bridge_id.is_empty():
		bridge_id = _generate_bridge_id()
	var active_anchor: Node = _find_anchor_by_bridge_id(terrain, bridge_id)
	if active_anchor != null:
		return _failure(
			"This source already has an active Terrain3D bridge batch. Restore that batch with clearing enabled before baking the edited nodes again."
		)

	var owner_id: int = _resolve_owner_id(terrain, assets, bridge_id) if native_owner_supported else 0
	if native_owner_supported and owner_id <= 0:
		return _failure("Could not allocate a unique native instancer owner ID for this bridge batch.")

	var grouped_transforms: Dictionary = {}
	var source_node_paths: Dictionary = {}
	var source_regions_by_key: Dictionary = {}
	var descriptors_by_key: Dictionary = {}
	for placement: Dictionary in placements:
		var asset_key: String = String(placement.get("asset_key", placement["scene_path"]))
		if not grouped_transforms.has(asset_key):
			var new_transforms: Array[Transform3D] = []
			var new_paths: Array[NodePath] = []
			var new_source_regions: Array[Vector2i] = []
			grouped_transforms[asset_key] = new_transforms
			source_node_paths[asset_key] = new_paths
			source_regions_by_key[asset_key] = new_source_regions
			descriptors_by_key[asset_key] = {
				"asset_key": asset_key,
				"instance_category": int(placement.get("instance_category", SceneRulesScript.INSTANCE_CATEGORY_GROUND_COVER)),
				"scene_path": String(placement["scene_path"]),
				"mesh_path": placement.get("mesh_path", NodePath("")),
				"type_name": String(placement.get("type_name", _scene_rules.type_name_from_path(String(placement["scene_path"])))),
				"compound_scene": bool(placement.get("compound_scene", false)),
			}

		var transforms: Array[Transform3D] = grouped_transforms[asset_key]
		transforms.append(placement["transform"])
		grouped_transforms[asset_key] = transforms

		var paths: Array[NodePath] = source_node_paths[asset_key]
		paths.append(placement["node_path"])
		source_node_paths[asset_key] = paths

		var aligned_regions: Array[Vector2i] = source_regions_by_key[asset_key]
		aligned_regions.append(placement.get("source_region_location", Vector2i.ZERO))
		source_regions_by_key[asset_key] = aligned_regions

	_emit_progress(0.12, "Grouped source placements", true)
	var sorted_asset_keys: Array[String] = []
	for asset_key: String in grouped_transforms.keys():
		sorted_asset_keys.append(asset_key)
	sorted_asset_keys.sort()

	var existing_by_key: Dictionary = {}
	var asset_entries: Array[Dictionary] = []
	var mesh_count: int = int(assets.call("get_mesh_count"))
	for mesh_id: int in range(mesh_count):
		var existing_asset: Object = assets.call("get_mesh_asset", mesh_id)
		if existing_asset == null:
			continue
		if String(existing_asset.get_meta(META_BRIDGE_ID, "")) != bridge_id:
			continue
		var existing_key: String = String(existing_asset.get_meta(META_ASSET_KEY, ""))
		if existing_key.is_empty():
			existing_key = String(existing_asset.get_meta(META_SCENE_PATH, ""))
		if not existing_key.is_empty():
			var existing_category: int = int(existing_asset.call("get_instance_category"))
			existing_by_key[_scene_rules.asset_key_with_category(existing_key, existing_category)] = existing_asset

	var bindings: Array[Dictionary] = []
	var next_mesh_id: int = mesh_count
	var processed_placements: int = 0
	var total_placements: int = placements.size()
	var converted_cells: Dictionary = {}
	var shader_material_surface_count: int = 0
	var fallback_material_surface_count: int = 0
	var alpha_coerced_surface_count: int = 0
	for asset_key: String in sorted_asset_keys:
		var descriptor: Dictionary = descriptors_by_key[asset_key]
		var scene_path: String = String(descriptor["scene_path"])
		var mesh_path: NodePath = descriptor.get("mesh_path", NodePath(""))
		var type_name: String = String(descriptor["type_name"])
		var instance_category: int = int(descriptor.get("instance_category", SceneRulesScript.INSTANCE_CATEGORY_GROUND_COVER))
		var compound_scene: bool = bool(descriptor.get("compound_scene", false))
		var typed_transforms: Array[Transform3D] = grouped_transforms[asset_key]
		var typed_node_paths: Array[NodePath] = source_node_paths[asset_key]
		var typed_source_regions: Array[Vector2i] = source_regions_by_key[asset_key]

		var mesh_asset: Object = existing_by_key.get(asset_key, null)
		var mesh_id: int = -1
		var is_new: bool = mesh_asset == null
		if not is_new:
			mesh_id = int(mesh_asset.call("get_id"))
			if mesh_id < 0 or mesh_id >= mesh_count:
				mesh_id = _terrain_io.find_asset_id(assets, mesh_asset)
			if mesh_id < 0:
				return _failure("Could not resolve the Terrain3D mesh ID for %s." % asset_key)

		# A previous failed material import falls back to Terrain3D's generated
		# texture-card mode. Such an asset is not a valid bridge mesh and must be
		# re-imported from the original source instead of being silently reused.
		var requires_scene_import: bool = is_new or _mesh_asset_requires_scene_reimport(mesh_asset)
		if requires_scene_import:
			var source_node: Node = source_root.get_node_or_null(typed_node_paths[0])
			var mesh_build: Dictionary = {}
			if compound_scene:
				if not source_node is Node3D:
					return _failure("Could not resolve compound scene instance for %s from %s." % [asset_key, scene_path])
				mesh_build = _scene_rules.build_compound_packed_scene_result(source_node as Node3D, type_name)
			else:
				if not source_node is MeshInstance3D:
					return _failure("Could not resolve extracted MeshInstance3D %s from %s." % [String(mesh_path), scene_path])
				var mesh_error: String = _scene_rules.validate_mesh_instance_for_terrain(source_node as MeshInstance3D, asset_key)
				if not mesh_error.is_empty():
					return _failure(mesh_error)
				mesh_build = _scene_rules.build_single_mesh_packed_scene_result(source_node as MeshInstance3D, type_name)
			var mesh_scene: PackedScene = mesh_build.get("scene", null) as PackedScene
			if mesh_scene == null:
				return _failure("Could not build a Terrain3D mesh asset scene for %s." % asset_key)
			shader_material_surface_count += int(mesh_build.get("shader_surface_count", 0))
			fallback_material_surface_count += int(mesh_build.get("fallback_surface_count", 0))
			alpha_coerced_surface_count += int(mesh_build.get("alpha_coerced_surface_count", 0))
			if is_new:
				mesh_asset = ClassDB.instantiate(&"Terrain3DMeshAsset")
				if mesh_asset == null:
					return _failure("Terrain3DMeshAsset is unavailable. Enable Terrain3D before this plugin.")
				# Category must be established before scene import so native derived
				# collision/occlusion products are built under the correct contract.
				mesh_asset.call("set_instance_category", instance_category)
			mesh_asset.call("set_name", "[Bridge] %s" % type_name)
			mesh_asset.call("set_scene_file", mesh_scene)
			if _mesh_asset_requires_scene_reimport(mesh_asset):
				return _failure(
					"Terrain3D rejected the canonicalized mesh surfaces for %s and fell back to a generated mesh. The bridge will not install or reuse that invalid asset."
					% asset_key
				)
			mesh_asset.set_meta(META_BRIDGE_ID, bridge_id)
			mesh_asset.set_meta(META_SCENE_PATH, scene_path)
			mesh_asset.set_meta(META_MESH_PATH, String(mesh_path))
			mesh_asset.set_meta(META_ASSET_KEY, asset_key)
			mesh_asset.set_meta(META_TYPE_NAME, type_name)

		var accepted_transforms: Array[Transform3D] = []
		var accepted_node_paths: Array[NodePath] = []
		var accepted_source_regions: Array[Vector2i] = []
		var typed_regions: Array[Vector2i] = []
		var typed_region_set: Dictionary = {}
		var height_offset: float = float(mesh_asset.call("get_height_offset"))
		for transform_index: int in range(typed_transforms.size()):
			var transform: Transform3D = typed_transforms[transform_index]
			if not transform.origin.is_finite() or not transform.basis.is_finite():
				return _failure(
					"Extracted MeshInstance3D %s cannot be routed into Terrain3D instancing: transform contains non-finite values."
					% String(typed_node_paths[transform_index])
				)
			var route_key: Vector4i = _terrain_io.resolve_native_key_cached(data, subdivision_info, transform, height_offset)
			if route_key.z < 0 or route_key.w < 0:
				return _failure(
					"Extracted MeshInstance3D %s cannot be mapped to a valid Terrain3D instancer cell."
					% String(typed_node_paths[transform_index])
				)
			var native_region: Vector2i = Vector2i(route_key.x, route_key.y)
			processed_placements += 1
			if processed_placements % PROGRESS_BATCH_STRIDE == 0 or processed_placements == total_placements:
				_emit_progress(
					0.12 + 0.34 * float(processed_placements) / float(maxi(total_placements, 1)),
					"Routing placements into Terrain3D regions"
				)
			if not bool(data.call("has_region", native_region)):
				missing_region_count += 1
				continue
			accepted_transforms.append(typed_transforms[transform_index])
			accepted_node_paths.append(typed_node_paths[transform_index])
			accepted_source_regions.append(typed_source_regions[transform_index])
			if not typed_region_set.has(native_region):
				typed_region_set[native_region] = true
				typed_regions.append(native_region)
			converted_cells[route_key] = true
		if accepted_transforms.is_empty():
			continue
		typed_regions.sort_custom(_sort_vector2i)

		if is_new:
			if next_mesh_id >= MAX_MESH_ASSETS:
				return _failure(
					"Terrain3D has no append slot for %s. The bridge requires one dedicated mesh asset per logical instanced source; the hard limit is %d."
					% [asset_key, MAX_MESH_ASSETS]
				)
			mesh_id = next_mesh_id
			next_mesh_id += 1
			asset_entries.append({
				"mesh_id": mesh_id,
				"asset": mesh_asset,
				"is_new": true,
				"had_owner_meta": false,
				"old_owner_meta": 0,
			})
		else:
			asset_entries.append({
				"mesh_id": mesh_id,
				"asset": mesh_asset,
				"is_new": false,
				"had_owner_meta": mesh_asset.has_meta(META_OWNER_ID),
				"old_owner_meta": mesh_asset.get_meta(META_OWNER_ID, 0),
			})

		var effective_instance_category: int = int(mesh_asset.call("get_instance_category"))
		bindings.append({
			"mesh_id": mesh_id,
			"asset_key": asset_key,
			"instance_category": effective_instance_category,
			"scene_path": scene_path,
			"mesh_path": mesh_path,
			"type_name": type_name,
			"compound_scene": compound_scene,
			"instance_count": accepted_transforms.size(),
			"transforms": accepted_transforms,
			"source_node_paths": accepted_node_paths,
			"source_regions": accepted_source_regions,
			"region_locations": typed_regions,
			"asset": mesh_asset,
			"is_new": is_new,
		})

	if missing_region_count > 0 and not skip_missing_regions:
		return _failure(
			"%d placement(s) route to Terrain3D regions that are not active after native height-offset placement. Add those regions or enable explicit skipping; the bridge will not silently lose placements."
			% missing_region_count
		)
	if missing_region_count > 0 and skip_missing_regions and remove_source:
		return _failure(
			"Skipping placements whose native owner region is not active cannot be combined with removing the source container, because the skipped nodes would be deleted. Disable source removal for this bake."
		)
	if bindings.is_empty():
		return _failure("Every placement routes outside the active Terrain3D regions.")

	asset_entries.sort_custom(_sort_asset_entry_by_id)
	var affected_ids: Array[int] = []
	var affected_id_set: Dictionary = {}
	for entry: Dictionary in asset_entries:
		var entry_id: int = int(entry["mesh_id"])
		if affected_id_set.has(entry_id):
			continue
		affected_id_set[entry_id] = true
		affected_ids.append(entry_id)
	affected_ids.sort()
	var converted_placement_count: int = 0
	for binding: Dictionary in bindings:
		converted_placement_count += int(binding.get("instance_count", 0))

	var converted_regions: Array[Vector2i] = []
	var converted_region_set: Dictionary = {}
	for binding: Dictionary in bindings:
		var binding_regions: Array[Vector2i] = binding["region_locations"]
		for region_location: Vector2i in binding_regions:
			if converted_region_set.has(region_location):
				continue
			converted_region_set[region_location] = true
			converted_regions.append(region_location)
	converted_regions.sort_custom(_sort_vector2i)

	var converted_cell_count: int = converted_cells.size()
	# Fresh native-owner batches do not need an instance-page snapshot. A source
	# restored with clearing enabled is also known empty. Preserve the old cleanup
	# path only for stale/external bridge metadata where an owner may already exist.
	var source_had_bridge_meta: bool = source_root.has_meta(META_BRIDGE_ID)
	var owner_cleanup_required: bool = owner_id > 0 and source_had_bridge_meta and _bridge_owner_exists_in_assets(assets, bridge_id, owner_id) and not bool(source_root.get_meta(META_OWNER_CLEARED, false))
	_emit_progress(0.48, "Preparing undo state", true)
	var previous_owned_entries: Dictionary = {}
	var legacy_regions_before: Array[Dictionary] = []
	if owner_cleanup_required:
		previous_owned_entries = _terrain_io.read_global_entries_by_mesh(
			terrain,
			bindings,
			owner_id,
			Callable(self, "_forward_bake_snapshot_progress")
		)
	elif owner_id <= 0:
		legacy_regions_before = _terrain_io.capture_region_instances(terrain, affected_ids)
	var source_parent: Node = source_root.get_parent()
	var source_owner: Node = source_root.owner
	var source_old_bridge_meta: String = String(source_root.get_meta(META_BRIDGE_ID, ""))

	var anchor: Node = AnchorScript.new()
	anchor.name = _unique_anchor_name(terrain, source_root.name, bridge_id)
	anchor.set_meta(META_ANCHOR, true)
	anchor.set("bridge_id", bridge_id)
	anchor.set("owner_id", owner_id)
	anchor.set("source_root_name", String(source_root.name))
	anchor.set("source_scene_path", source_root.scene_file_path)
	anchor.set("source_parent_path", scene_root.get_path_to(source_parent))
	anchor.set("source_sibling_index", source_root.get_index())
	anchor.set("source_root_global_transform", source_root.global_transform)
	anchor.set("region_count", converted_regions.size())
	anchor.set("cell_count", converted_cell_count)
	anchor.set("instancer_cell_size", int(subdivision_info.get("cell_size", 0)))
	anchor.set("cells_per_region_axis", int(subdivision_info.get("cells_per_region_axis", 0)))
	anchor.set("asset_bindings", _strip_runtime_binding_fields(bindings))

	var transaction: Dictionary = {
		"kind": "bake",
		"scene_root": scene_root,
		"terrain": terrain,
		"assets": assets,
		"instancer": instancer,
		"source_root": source_root,
		"source_parent": source_parent,
		"source_owner": source_owner,
		"source_index": source_root.get_index(),
		"source_had_bridge_meta": source_had_bridge_meta,
		"source_old_bridge_meta": source_old_bridge_meta,
		"remove_source": remove_source,
		"partial_source_removal": partial_source_removal,
		"source_removal_entries": source_removal_entries,
		"bridge_id": bridge_id,
		"owner_id": owner_id,
		"subdivision_info": subdivision_info,
		"cell_count": converted_cell_count,
		"anchor": anchor,
		"bindings": bindings,
		"asset_entries": asset_entries,
		"affected_ids": affected_ids,
		"previous_owned_entries": previous_owned_entries,
		"owner_cleanup_required": owner_cleanup_required,
		"regions_before": legacy_regions_before,
		"missing_region_count": missing_region_count,
		"region_count": converted_regions.size(),
		"shader_material_surface_count": shader_material_surface_count,
		"fallback_material_surface_count": fallback_material_surface_count,
		"alpha_coerced_surface_count": alpha_coerced_surface_count,
		"prepare_msec": Time.get_ticks_msec() - prepare_started_msec,
	}
	var transaction_id: int = _store_transaction(transaction)
	_emit_progress(0.55, "Bake prepared", true)
	return {
		"ok": true,
		"transaction_id": transaction_id,
		"anchor": anchor,
		"source_root": source_root,
		"remove_source": remove_source,
		"source_root_removed": remove_source and not partial_source_removal,
		"removed_source_nodes": source_removal_entries,
		"placement_count": converted_placement_count,
		"type_count": bindings.size(),
		"region_count": converted_regions.size(),
		"cell_count": converted_cell_count,
		"cell_size": int(subdivision_info.get("cell_size", 0)),
		"skipped_count": missing_region_count,
		"shader_material_surface_count": shader_material_surface_count,
		"fallback_material_surface_count": fallback_material_surface_count,
		"alpha_coerced_surface_count": alpha_coerced_surface_count,
	}

func prepare_restore(
		scene_root: Node,
		terrain: Node,
		anchor: Node,
		clear_terrain_instances: bool
	) -> Dictionary:
	_begin_progress("Preparing Terrain3D restore")
	var validation_error: String = _validate_common(scene_root, terrain)
	if not validation_error.is_empty():
		return _failure(validation_error)
	if anchor == null or not is_instance_valid(anchor):
		return _failure("Select a valid bridge batch anchor.")
	if not bool(anchor.get_meta(META_ANCHOR, false)):
		return _failure("The selected node is not a Terrain3D Scene Bridge anchor.")
	if anchor != terrain and not terrain.is_ancestor_of(anchor):
		return _failure("The selected bridge anchor belongs to a different Terrain3D node.")

	var bindings: Array[Dictionary] = _copy_dictionary_array(anchor.get("asset_bindings") as Array)
	if bindings.is_empty():
		return _failure("The bridge anchor has no asset bindings.")
	bindings.sort_custom(_sort_binding_by_path)

	var assets: Object = terrain.call("get_assets")
	var instancer: Object = terrain.call("get_instancer")
	if assets == null or instancer == null:
		return _failure("Terrain3D assets or instancer is unavailable.")

	var subdivision_info: Dictionary = _terrain_io.get_subdivision_info(terrain)
	var subdivision_error: String = _terrain_io.validate_subdivision_info(subdivision_info)
	if not subdivision_error.is_empty():
		return _failure(subdivision_error)
	var bridge_id: String = String(anchor.get("bridge_id"))
	var owner_id: int = int(anchor.get("owner_id"))
	for binding: Dictionary in bindings:
		var mesh_id: int = int(binding.get("mesh_id", -1))
		var scene_path: String = String(binding.get("scene_path", ""))
		if mesh_id < 0 or scene_path.is_empty():
			return _failure("Bridge anchor contains an invalid asset binding.")
		var mesh_asset: Object = assets.call("get_mesh_asset", mesh_id)
		var mesh_path_text: String = String(binding.get("mesh_path", ""))
		var asset_key: String = String(binding.get("asset_key", ""))
		var asset_error: String = _terrain_io.validate_asset_identity(mesh_asset, bridge_id, scene_path, mesh_id, owner_id, mesh_path_text, asset_key)
		if not asset_error.is_empty():
			return _failure(asset_error)
		binding["asset"] = mesh_asset
		binding["is_new"] = false

	_emit_progress(0.08, "Reading Terrain3D instance storage", true)
	var entry_map: Dictionary = _terrain_io.read_global_entries_by_mesh(
		terrain,
		bindings,
		owner_id,
		Callable(self, "_forward_restore_read_progress")
	)
	var total_instances: int = 0
	var restored_regions: Array[Vector2i] = []
	var restored_region_set: Dictionary = {}
	var restored_cells: Dictionary = {}
	for binding: Dictionary in bindings:
		var mesh_id: int = int(binding["mesh_id"])
		var entries: Array = entry_map.get(mesh_id, [])
		var binding_regions: Array[Vector2i] = []
		var binding_region_set: Dictionary = {}
		total_instances += entries.size()
		for entry: Dictionary in entries:
			var region_location: Vector2i = entry["region_location"]
			if not restored_region_set.has(region_location):
				restored_region_set[region_location] = true
				restored_regions.append(region_location)
			if not binding_region_set.has(region_location):
				binding_region_set[region_location] = true
				binding_regions.append(region_location)
			var cell_location: Vector2i = entry.get("cell_location", Vector2i.ZERO)
			restored_cells[Vector4i(region_location.x, region_location.y, cell_location.x, cell_location.y)] = true
		binding_regions.sort_custom(_sort_vector2i)
		binding["region_locations"] = binding_regions
	if total_instances <= 0:
		return _failure("The selected bridge batch contains no Terrain3D instances to restore.")
	restored_regions.sort_custom(_sort_vector2i)

	var source_parent_path: NodePath = anchor.get("source_parent_path")
	var source_parent: Node = scene_root.get_node_or_null(source_parent_path)
	if source_parent == null:
		source_parent = scene_root
	var output_root: Node3D = Node3D.new()
	var source_root_name: String = String(anchor.get("source_root_name"))
	var source_scene_path: String = String(anchor.get("source_scene_path"))
	var source_root_transform: Transform3D = anchor.get("source_root_global_transform")
	var source_root_inverse: Transform3D = source_root_transform.affine_inverse()
	output_root.name = _unique_child_name(source_parent, _scene_rules.safe_node_name(source_root_name))
	output_root.set_meta(META_BRIDGE_ID, bridge_id)
	output_root.set_meta(META_SOURCE_PATH, source_scene_path)

	var region_nodes: Dictionary = {}
	for region_location: Vector2i in restored_regions:
		region_nodes[region_location] = _region_index.get_or_create_region_node(output_root, region_location)

	var groups_by_region: Dictionary = {}
	var built_instances: int = 0
	for binding: Dictionary in bindings:
		var mesh_id: int = int(binding["mesh_id"])
		var scene_path: String = String(binding["scene_path"])
		var mesh_path: NodePath = binding.get("mesh_path", NodePath(""))
		var mesh_path_text: String = String(mesh_path)
		var asset_key: String = String(binding.get("asset_key", scene_path))
		var instance_category: int = int(binding.get("instance_category", SceneRulesScript.INSTANCE_CATEGORY_GROUND_COVER))
		var entries: Array = entry_map.get(mesh_id, [])
		if entries.is_empty():
			continue

		var mesh_asset: Object = binding.get("asset", null)
		var mesh_template: MeshInstance3D = null
		var legacy_packed_scene: PackedScene = null
		if mesh_path_text.is_empty():
			legacy_packed_scene = ResourceLoader.load(scene_path) as PackedScene
			if legacy_packed_scene == null or not legacy_packed_scene.can_instantiate():
				output_root.free()
				return _failure("Cannot reload legacy PackedScene for restoration: %s" % scene_path)
		else:
			# Build the source-style mesh template once per binding. The previous path
			# loaded and instantiated the PackedScene for every restored instance.
			mesh_template = _scene_rules.instantiate_source_mesh(scene_path, mesh_path)
			if mesh_template == null:
				mesh_template = MeshInstance3D.new()
			if mesh_asset != null and not _mesh_asset_requires_scene_reimport(mesh_asset):
				var current_mesh: Mesh = mesh_asset.call("get_mesh", 0) as Mesh
				if current_mesh != null:
					mesh_template.mesh = current_mesh

		var type_name: String = String(binding.get("type_name", _scene_rules.type_name_from_path(scene_path)))
		for entry: Dictionary in entries:
			var region_location: Vector2i = entry["region_location"]
			var region_node: Node3D = region_nodes[region_location]
			var region_groups: Dictionary = groups_by_region.get(region_location, {})
			var group: Node3D = region_groups.get(asset_key, null) as Node3D
			if group == null:
				group = Node3D.new()
				group.name = _unique_child_name(region_node, _scene_rules.safe_node_name(type_name))
				group.set_meta(META_SCENE_PATH, scene_path)
				group.set_meta(META_INSTANCE_CATEGORY, instance_category)
				if not mesh_path_text.is_empty():
					group.set_meta(META_MESH_PATH, mesh_path_text)
					group.set_meta(META_ASSET_KEY, asset_key)
				region_node.add_child(group)
				region_groups[asset_key] = group
				groups_by_region[region_location] = region_groups

			var instance_3d: Node3D = null
			if not mesh_path_text.is_empty():
				var duplicate_node: Node = mesh_template.duplicate() if mesh_template != null else null
				var mesh_instance: MeshInstance3D = duplicate_node as MeshInstance3D
				if mesh_instance == null:
					if duplicate_node != null:
						duplicate_node.free()
					mesh_instance = MeshInstance3D.new()
					if mesh_template != null:
						mesh_instance.mesh = mesh_template.mesh
				instance_3d = mesh_instance
				instance_3d.set_meta(META_SCENE_PATH, scene_path)
				instance_3d.set_meta(META_MESH_PATH, mesh_path_text)
				instance_3d.set_meta(META_ASSET_KEY, asset_key)
				instance_3d.set_meta(META_TYPE_NAME, type_name)
				instance_3d.set_meta(META_INSTANCE_CATEGORY, instance_category)
			else:
				var legacy_instance: Node = legacy_packed_scene.instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
				if not legacy_instance is Node3D:
					legacy_instance.free()
					output_root.free()
					return _failure("PackedScene root is not Node3D: %s" % scene_path)
				instance_3d = legacy_instance as Node3D

			instance_3d.name = "%s_%05d" % [_scene_rules.safe_node_name(type_name), group.get_child_count()]
			group.add_child(instance_3d)
			var global_transform: Transform3D = entry["transform"]
			instance_3d.transform = source_root_inverse * global_transform
			instance_3d.set_meta(&"terrain3d_scene_bridge_cell", entry.get("cell_location", Vector2i.ZERO))
			if owner_id > 0:
				instance_3d.set_meta(META_OWNER_ID, owner_id)
			built_instances += 1
			if built_instances % PROGRESS_BATCH_STRIDE == 0 or built_instances == total_instances:
				_emit_progress(
					0.30 + 0.48 * float(built_instances) / float(maxi(total_instances, 1)),
					"Building editable MeshInstance3D nodes"
				)
		if mesh_template != null:
			mesh_template.free()

	_emit_progress(0.79, "Finalizing restore transaction", true)
	var affected_ids: Array[int] = []
	var affected_id_set: Dictionary = {}
	for binding: Dictionary in bindings:
		var mesh_id: int = int(binding["mesh_id"])
		if affected_id_set.has(mesh_id):
			continue
		affected_id_set[mesh_id] = true
		affected_ids.append(mesh_id)
	affected_ids.sort()

	var anchor_parent: Node = anchor.get_parent()
	var transaction: Dictionary = {
		"kind": "restore",
		"scene_root": scene_root,
		"terrain": terrain,
		"assets": assets,
		"instancer": instancer,
		"bridge_id": bridge_id,
		"owner_id": owner_id,
		"subdivision_info": subdivision_info,
		"cell_count": restored_cells.size(),
		"entry_map": entry_map,
		"bindings": bindings,
		"anchor": anchor,
		"anchor_parent": anchor_parent,
		"anchor_owner": anchor.owner,
		"anchor_index": anchor.get_index(),
		"output_root": output_root,
		"output_parent": source_parent,
		"output_index": int(anchor.get("source_sibling_index")),
		"output_global_transform": source_root_transform,
		"clear_terrain_instances": clear_terrain_instances,
		"affected_ids": affected_ids,
		"regions_before": _terrain_io.capture_region_instances(terrain, affected_ids) if owner_id <= 0 else [],
		"instance_count": built_instances,
		"type_count": bindings.size(),
		"region_count": restored_regions.size(),
	}
	var transaction_id: int = _store_transaction(transaction)
	_emit_progress(0.80, "Restore prepared", true)
	return {
		"ok": true,
		"transaction_id": transaction_id,
		"anchor": anchor,
		"output_root": output_root,
		"clear_terrain_instances": clear_terrain_instances,
		"instance_count": built_instances,
		"type_count": bindings.size(),
		"region_count": restored_regions.size(),
		"cell_count": restored_cells.size(),
		"cell_size": int(subdivision_info.get("cell_size", 0)),
	}

func apply_transaction(transaction_id: int) -> void:
	var transaction: Dictionary = _transactions.get(transaction_id, {})
	if transaction.is_empty():
		operation_finished.emit(false, "Bridge transaction no longer exists.")
		return
	match String(transaction.get("kind", "")):
		"bake":
			_apply_bake(transaction)
		"restore":
			_apply_restore(transaction)
		_:
			operation_finished.emit(false, "Unknown bridge transaction kind.")

func revert_transaction(transaction_id: int) -> void:
	var transaction: Dictionary = _transactions.get(transaction_id, {})
	if transaction.is_empty():
		operation_finished.emit(false, "Bridge transaction no longer exists.")
		return
	match String(transaction.get("kind", "")):
		"bake":
			_revert_bake(transaction)
		"restore":
			_revert_restore(transaction)
		_:
			operation_finished.emit(false, "Unknown bridge transaction kind.")

func _apply_bake(transaction: Dictionary) -> void:
	var terrain: Node = transaction["terrain"]
	var scene_root: Node = transaction["scene_root"]
	var assets: Object = transaction["assets"]
	var instancer: Object = transaction["instancer"]
	if not _objects_are_valid([terrain, scene_root, assets, instancer]):
		operation_finished.emit(false, "Bake target became invalid.")
		return

	var asset_entries: Array = transaction["asset_entries"]
	var existing_error: String = _terrain_io.validate_asset_entries(assets, asset_entries, false)
	if not existing_error.is_empty():
		operation_finished.emit(false, existing_error)
		return

	var apply_started_msec: int = Time.get_ticks_msec()
	_emit_progress(0.56, "Creating dedicated Terrain3D mesh assets", true)
	var new_count: int = 0
	for entry: Dictionary in asset_entries:
		if bool(entry["is_new"]):
			new_count += 1
	var asset_started_msec: int = Time.get_ticks_msec()
	_install_new_mesh_assets(assets, asset_entries, new_count)
	var asset_install_msec: int = Time.get_ticks_msec() - asset_started_msec

	var installed_error: String = _terrain_io.validate_asset_entries(assets, asset_entries, true)
	if not installed_error.is_empty():
		operation_finished.emit(false, installed_error)
		return

	var owner_id: int = int(transaction.get("owner_id", 0))
	if owner_id > 0:
		for owner_entry: Dictionary in asset_entries:
			var owner_asset: Object = owner_entry.get("asset", null)
			if owner_asset != null:
				owner_asset.set_meta(META_OWNER_ID, owner_id)

	# Do not pre-clear fresh/known-empty owners. remove_owned_instances() walks the
	# whole terrain region table and may demand-page storage. Keep it only for the
	# stale-owner recovery path captured during preparation.
	if bool(transaction.get("owner_cleanup_required", false)):
		_terrain_io.remove_batch_instances(instancer, owner_id, transaction["affected_ids"], false, false)
	var bindings: Array = transaction["bindings"]
	var placed_count: int = 0
	var total_count: int = 0
	for binding: Dictionary in bindings:
		total_count += int(binding["instance_count"])
	var write_started_msec: int = Time.get_ticks_msec()
	for binding: Dictionary in bindings:
		var transforms: Array[Transform3D] = binding["transforms"]
		var mesh_id: int = int(binding["mesh_id"])
		if owner_id > 0 and instancer.has_method("add_owned_transforms"):
			instancer.call("add_owned_transforms", mesh_id, transforms, owner_id, PackedColorArray(), false, false)
		else:
			instancer.call("add_transforms", mesh_id, transforms, PackedColorArray(), false)
		placed_count += transforms.size()
		_emit_progress(
			0.66 + 0.26 * float(placed_count) / float(maxi(total_count, 1)),
			"Writing Terrain3D instance transforms"
		)

	# Publish once after all authoritative store mutations. Per-binding/per-region
	# update_mmis() calls multiplied renderer reconciliation and editor overhead.
	_emit_progress(0.93, "Publishing Terrain3D instance batch", true)
	instancer.call("update_mmis", -1, UPDATE_ALL_REGIONS, false)
	var transform_write_msec: int = Time.get_ticks_msec() - write_started_msec

	var previous_subdivision: Dictionary = transaction.get("subdivision_info", {})
	var current_subdivision: Dictionary = _terrain_io.get_subdivision_info(terrain)
	var subdivision_unchanged: bool = (
		int(current_subdivision.get("cell_size", 0)) == int(previous_subdivision.get("cell_size", -1))
		and int(current_subdivision.get("region_size", 0)) == int(previous_subdivision.get("region_size", -1))
		and is_equal_approx(
			float(current_subdivision.get("vertex_spacing", 0.0)),
			float(previous_subdivision.get("vertex_spacing", -1.0))
		)
	)
	if not subdivision_unchanged:
		var current_stats: Dictionary = _terrain_io.count_subdivision_cells_for_bindings(terrain, bindings)
		transaction["cell_count"] = int(current_stats.get("cell_count", 0))
	transaction["subdivision_info"] = current_subdivision

	var anchor: Node = transaction["anchor"]
	anchor.set("cell_count", int(transaction["cell_count"]))
	anchor.set("instancer_cell_size", int(current_subdivision.get("cell_size", 0)))
	anchor.set("cells_per_region_axis", int(current_subdivision.get("cells_per_region_axis", 0)))
	if anchor.get_parent() == null:
		terrain.add_child(anchor)
	anchor.owner = scene_root

	var source_root: Node3D = transaction["source_root"]
	if is_instance_valid(source_root):
		source_root.set_meta(META_BRIDGE_ID, transaction["bridge_id"])
		if bool(transaction["remove_source"]):
			if bool(transaction.get("partial_source_removal", false)):
				var removal_entries: Array = transaction.get("source_removal_entries", [])
				# Remove deepest nodes first so future schema extensions can safely
				# include nested conversion roots without invalidating parent links.
				for removal_index: int in range(removal_entries.size() - 1, -1, -1):
					var removal_entry: Dictionary = removal_entries[removal_index]
					var removal_node: Node = removal_entry.get("node", null)
					if is_instance_valid(removal_node) and removal_node.get_parent() != null:
						removal_node.get_parent().remove_child(removal_node)
			elif source_root.get_parent() != null:
				source_root.get_parent().remove_child(source_root)

	_emit_progress(1.0, "Bake complete", true)
	var apply_msec: int = Time.get_ticks_msec() - apply_started_msec
	var message: String = "Converted %d extracted GLB MeshInstance3D placements across %d regions / %d instancer cells at %d samples and %d mesh types into Terrain3D. Prepare %d ms; apply %d ms (assets %d ms, transforms/publication %d ms)." % [
		total_count,
		int(transaction.get("region_count", 0)),
		int(transaction.get("cell_count", 0)),
		int((transaction.get("subdivision_info", {}) as Dictionary).get("cell_size", 0)),
		bindings.size(),
		int(transaction.get("prepare_msec", 0)),
		apply_msec,
		asset_install_msec,
		transform_write_msec,
	]
	var shader_surface_count: int = int(transaction.get("shader_material_surface_count", 0))
	var fallback_surface_count: int = int(transaction.get("fallback_material_surface_count", 0))
	var alpha_surface_count: int = int(transaction.get("alpha_coerced_surface_count", 0))
	if shader_surface_count > 0 or fallback_surface_count > 0 or alpha_surface_count > 0:
		message += " Material canonicalization: %d shader surface(s), %d neutral fallback surface(s), %d alpha mode coercion(s)." % [
			shader_surface_count,
			fallback_surface_count,
			alpha_surface_count,
		]
	_log(message)
	operation_finished.emit(true, message)


func _install_new_mesh_assets(assets: Object, asset_entries: Array, new_count: int) -> void:
	if new_count <= 0:
		_emit_progress(0.66, "Terrain3D mesh assets already installed", true)
		return

	# Never rebuild the complete mesh list to install a subset. Terrain3D instance
	# storage persists mesh IDs; whole-list replacement can reorder unrelated assets
	# when a resource's embedded ID is stale, making existing transforms draw another
	# mesh. Prefer the native slot-preserving batch mutation when available.
	if assets.has_method("set_mesh_assets_batch"):
		var assets_by_id: Dictionary = {}
		for entry: Dictionary in asset_entries:
			if not bool(entry["is_new"]):
				continue
			var mesh_id: int = int(entry["mesh_id"])
			var mesh_asset: Object = entry["asset"]
			if mesh_asset != null and mesh_asset.has_method("set_id"):
				mesh_asset.call("set_id", mesh_id)
			assets_by_id[mesh_id] = mesh_asset
		var installed: bool = bool(assets.call("set_mesh_assets_batch", assets_by_id))
		if installed:
			_emit_progress(0.66, "Created Terrain3D mesh assets as one slot-preserving batch", true)
			return

	# Compatibility fallback for older Terrain3D builds: slower, but each mutation
	# is slot-addressed and therefore cannot remap unrelated mesh IDs.
	var created_count: int = 0
	for entry: Dictionary in asset_entries:
		if not bool(entry["is_new"]):
			continue
		assets.call("set_mesh_asset", int(entry["mesh_id"]), entry["asset"])
		created_count += 1
		_emit_progress(
			0.56 + 0.10 * float(created_count) / float(maxi(new_count, 1)),
			"Creating dedicated Terrain3D mesh assets"
		)


func _revert_bake(transaction: Dictionary) -> void:
	_begin_progress("Undoing Terrain3D bake")
	var terrain: Node = transaction["terrain"]
	var scene_root: Node = transaction["scene_root"]
	var assets: Object = transaction["assets"]
	var instancer: Object = transaction["instancer"]
	if not _objects_are_valid([terrain, scene_root, assets, instancer]):
		operation_finished.emit(false, "Bake undo target became invalid.")
		return

	var asset_entries: Array = transaction["asset_entries"]
	var identity_error: String = _terrain_io.validate_asset_entries(assets, asset_entries, true)
	if not identity_error.is_empty():
		operation_finished.emit(false, identity_error)
		return

	var anchor: Node = transaction["anchor"]
	if is_instance_valid(anchor) and anchor.get_parent() != null:
		anchor.get_parent().remove_child(anchor)

	var affected_ids: Array = transaction["affected_ids"]
	var owner_id: int = int(transaction.get("owner_id", 0))
	_terrain_io.remove_batch_instances(instancer, owner_id, affected_ids, true, false)
	_emit_progress(0.25, "Removing baked Terrain3D instances", true)

	for index: int in range(asset_entries.size() - 1, -1, -1):
		var entry: Dictionary = asset_entries[index]
		var asset: Object = entry.get("asset", null)
		if bool(entry["is_new"]):
			assets.call("set_mesh_asset", int(entry["mesh_id"]), null)
		elif asset != null:
			if bool(entry.get("had_owner_meta", false)):
				asset.set_meta(META_OWNER_ID, entry.get("old_owner_meta", 0))
			else:
				asset.remove_meta(META_OWNER_ID)

	_emit_progress(0.45, "Restoring Terrain3D mesh assets", true)
	if owner_id > 0:
		_terrain_io.add_entry_map(instancer, transaction.get("previous_owned_entries", {}), owner_id, false, false)
		_terrain_io.queue_binding_updates(instancer, transaction["bindings"])
	else:
		_terrain_io.restore_region_instances(terrain, affected_ids, transaction["regions_before"])
		_terrain_io.queue_snapshot_updates(instancer, transaction["regions_before"])
	_emit_progress(0.85, "Restoring previous Terrain3D instance data", true)

	var source_root: Node3D = transaction["source_root"]
	var source_parent: Node = transaction["source_parent"]
	if is_instance_valid(source_root):
		if bool(transaction["remove_source"]):
			if bool(transaction.get("partial_source_removal", false)):
				var removal_entries: Array = transaction.get("source_removal_entries", [])
				for removal_entry_variant in removal_entries:
					var removal_entry: Dictionary = removal_entry_variant as Dictionary
					var removal_node: Node = removal_entry.get("node", null)
					var removal_parent: Node = removal_entry.get("parent", null)
					if not is_instance_valid(removal_node) or not is_instance_valid(removal_parent):
						continue
					if removal_node.get_parent() == null:
						removal_parent.add_child(removal_node)
						_move_child_safely(removal_parent, removal_node, int(removal_entry.get("index", -1)))
						removal_node.owner = removal_entry.get("owner", null)
			elif source_root.get_parent() == null and is_instance_valid(source_parent):
				source_parent.add_child(source_root)
				_move_child_safely(source_parent, source_root, int(transaction["source_index"]))
				source_root.owner = transaction["source_owner"]
		if bool(transaction["source_had_bridge_meta"]):
			source_root.set_meta(META_BRIDGE_ID, transaction["source_old_bridge_meta"])
		else:
			source_root.remove_meta(META_BRIDGE_ID)

	_emit_progress(1.0, "Bake undone", true)
	operation_finished.emit(true, "Restored the editable source hierarchy and previous Terrain3D instance data.")

func _apply_restore(transaction: Dictionary) -> void:
	var scene_root: Node = transaction["scene_root"]
	var terrain: Node = transaction["terrain"]
	var assets: Object = transaction["assets"]
	var instancer: Object = transaction["instancer"]
	var output_root: Node3D = transaction["output_root"]
	var output_parent: Node = transaction["output_parent"]
	if not _objects_are_valid([scene_root, terrain, assets, instancer, output_root, output_parent]):
		operation_finished.emit(false, "Restore target became invalid.")
		return

	var owner_id: int = int(transaction.get("owner_id", 0))
	var identity_error: String = _terrain_io.validate_restore_bindings(assets, transaction["bindings"], String(transaction["bridge_id"]), owner_id)
	if not identity_error.is_empty():
		operation_finished.emit(false, identity_error)
		return

	_emit_progress(0.82, "Attaching editable scene hierarchy", true)
	if output_root.get_parent() == null:
		output_parent.add_child(output_root)
	_move_child_safely(output_parent, output_root, int(transaction["output_index"]))
	output_root.global_transform = transaction["output_global_transform"]
	_assign_saved_owners(output_root, scene_root)

	if bool(transaction["clear_terrain_instances"]):
		var affected_ids: Array = transaction["affected_ids"]
		_terrain_io.remove_batch_instances(instancer, owner_id, affected_ids, true, false)
		if owner_id > 0:
			output_root.set_meta(META_OWNER_CLEARED, true)
		var anchor: Node = transaction["anchor"]
		if is_instance_valid(anchor) and anchor.get_parent() != null:
			anchor.get_parent().remove_child(anchor)

	_emit_progress(1.0, "Restore complete", true)
	var message: String = "Restored %d editable MeshInstance3D nodes from %d regions / %d current instancer cells at %d samples and %d mesh types." % [
		int(transaction["instance_count"]),
		int(transaction.get("region_count", 0)),
		int(transaction.get("cell_count", 0)),
		int((transaction.get("subdivision_info", {}) as Dictionary).get("cell_size", 0)),
		int(transaction["type_count"]),
	]
	_log(message)
	operation_finished.emit(true, message)

func _revert_restore(transaction: Dictionary) -> void:
	_begin_progress("Undoing Terrain3D restore")
	var scene_root: Node = transaction["scene_root"]
	var terrain: Node = transaction["terrain"]
	var assets: Object = transaction["assets"]
	var instancer: Object = transaction["instancer"]
	if not _objects_are_valid([scene_root, terrain, assets, instancer]):
		operation_finished.emit(false, "Restore undo target became invalid.")
		return

	if bool(transaction["clear_terrain_instances"]):
		var identity_error: String = _terrain_io.validate_restore_bindings(assets, transaction["bindings"], String(transaction["bridge_id"]), int(transaction.get("owner_id", 0)))
		if not identity_error.is_empty():
			operation_finished.emit(false, identity_error)
			return

	var output_root: Node3D = transaction["output_root"]
	if is_instance_valid(output_root) and output_root.get_parent() != null:
		output_root.get_parent().remove_child(output_root)
	_emit_progress(0.30, "Removing restored scene hierarchy", true)

	if bool(transaction["clear_terrain_instances"]):
		var affected_ids: Array = transaction["affected_ids"]
		var owner_id: int = int(transaction.get("owner_id", 0))
		_terrain_io.remove_batch_instances(instancer, owner_id, affected_ids, false, false)
		_emit_progress(0.45, "Clearing restored Terrain3D batch", true)

		var anchor: Node = transaction["anchor"]
		var anchor_parent: Node = transaction["anchor_parent"]
		if is_instance_valid(anchor) and anchor.get_parent() == null and is_instance_valid(anchor_parent):
			anchor_parent.add_child(anchor)
			_move_child_safely(anchor_parent, anchor, int(transaction["anchor_index"]))
			anchor.owner = transaction["anchor_owner"]

		_emit_progress(0.60, "Restoring Terrain3D bridge anchor", true)
		if owner_id > 0:
			_terrain_io.add_entry_map(instancer, transaction.get("entry_map", {}), owner_id, false, false)
			_terrain_io.queue_binding_updates(instancer, transaction["bindings"])
		else:
			_terrain_io.restore_region_instances(terrain, affected_ids, transaction["regions_before"])
			_terrain_io.queue_snapshot_updates(instancer, transaction["regions_before"])
		_emit_progress(0.90, "Restoring Terrain3D instance data", true)

	_emit_progress(1.0, "Restore undone", true)
	operation_finished.emit(true, "Removed the editable hierarchy and restored the Terrain3D batch.")

func _collect_anchors_recursive(current: Node, anchors: Array[Node]) -> void:
	for child: Node in current.get_children():
		if bool(child.get_meta(META_ANCHOR, false)):
			anchors.append(child)
		_collect_anchors_recursive(child, anchors)

func _find_anchor_by_bridge_id(terrain: Node, bridge_id: String) -> Node:
	for anchor: Node in find_anchors(terrain):
		if String(anchor.get("bridge_id")) == bridge_id:
			return anchor
	return null

func _strip_runtime_binding_fields(bindings: Array[Dictionary]) -> Array[Dictionary]:
	var persistent: Array[Dictionary] = []
	for binding: Dictionary in bindings:
		var persisted_regions: Array[Vector2i] = []
		var runtime_regions: Array = binding.get("region_locations", [])
		for region_location: Vector2i in runtime_regions:
			persisted_regions.append(region_location)
		persisted_regions.sort_custom(_sort_vector2i)
		persistent.append({
			"mesh_id": int(binding["mesh_id"]),
			"asset_key": String(binding.get("asset_key", binding["scene_path"])),
			"scene_path": String(binding["scene_path"]),
			"mesh_path": binding.get("mesh_path", NodePath("")),
			"type_name": String(binding["type_name"]),
			"compound_scene": bool(binding.get("compound_scene", false)),
			"instance_count": int(binding["instance_count"]),
			"region_locations": persisted_regions,
		})
	return persistent

func _copy_dictionary_array(value: Array) -> Array[Dictionary]:
	var result: Array[Dictionary] = []
	for item: Dictionary in value:
		result.append(item.duplicate(true))
	return result

func _assign_saved_owners(output_root: Node3D, scene_root: Node) -> void:
	_assign_owner_recursive(output_root, scene_root)

func _assign_owner_recursive(current: Node, scene_root: Node) -> void:
	current.owner = scene_root
	if not current.scene_file_path.is_empty():
		return
	for child: Node in current.get_children():
		_assign_owner_recursive(child, scene_root)

func _mesh_asset_requires_scene_reimport(mesh_asset: Object) -> bool:
	if mesh_asset == null:
		return true
	if mesh_asset.has_method("get_generated_type") and int(mesh_asset.call("get_generated_type")) != 0:
		return true
	if mesh_asset.has_method("get_mesh") and mesh_asset.call("get_mesh", 0) == null:
		return true
	return false


func _validate_common(scene_root: Node, terrain: Node) -> String:
	if scene_root == null or not is_instance_valid(scene_root):
		return "No edited scene is open."
	if terrain == null or not is_instance_valid(terrain):
		return "Terrain3D target is missing."
	if not terrain.is_class(&"Terrain3D"):
		return "Selected terrain target is not a Terrain3D node."
	if terrain != scene_root and not scene_root.is_ancestor_of(terrain):
		return "The Terrain3D target is not inside the edited scene tree."
	if not ClassDB.class_exists(&"Terrain3DMeshAsset"):
		return "Terrain3DMeshAsset is unavailable. Enable Terrain3D before this plugin."
	return ""

func _unique_child_name(parent: Node, base_name: String) -> String:
	var candidate: String = base_name
	var suffix: int = 2
	while parent.has_node(NodePath(candidate)):
		candidate = "%s_%d" % [base_name, suffix]
		suffix += 1
	return candidate

func _unique_anchor_name(terrain: Node, source_name: StringName, bridge_id: String) -> String:
	return _unique_child_name(terrain, "__T3D_Bridge_%s_%s" % [_scene_rules.safe_node_name(String(source_name)), bridge_id.left(8)])

func _bridge_owner_exists_in_assets(assets: Object, bridge_id: String, owner_id: int) -> bool:
	if assets == null or owner_id <= 0:
		return false
	var mesh_count: int = int(assets.call("get_mesh_count"))
	for mesh_id: int in range(mesh_count):
		var asset: Object = assets.call("get_mesh_asset", mesh_id)
		if asset == null:
			continue
		if String(asset.get_meta(META_BRIDGE_ID, "")) == bridge_id and int(asset.get_meta(META_OWNER_ID, 0)) == owner_id:
			return true
	return false


func _resolve_owner_id(terrain: Node, assets: Object, bridge_id: String) -> int:
	var used: Dictionary = {}
	var resolved: int = 0
	for anchor: Node in find_anchors(terrain):
		var candidate: int = int(anchor.get("owner_id"))
		if candidate <= 0:
			continue
		var candidate_bridge: String = String(anchor.get("bridge_id"))
		if used.has(candidate) and String(used[candidate]) != candidate_bridge:
			return -1
		used[candidate] = candidate_bridge
		if candidate_bridge == bridge_id:
			if resolved > 0 and resolved != candidate:
				return -1
			resolved = candidate
	var mesh_count: int = int(assets.call("get_mesh_count")) if assets != null else 0
	for mesh_id: int in range(mesh_count):
		var asset: Object = assets.call("get_mesh_asset", mesh_id)
		if asset == null:
			continue
		var candidate: int = int(asset.get_meta(META_OWNER_ID, 0))
		if candidate <= 0:
			continue
		var candidate_bridge: String = String(asset.get_meta(META_BRIDGE_ID, ""))
		if used.has(candidate) and String(used[candidate]) != candidate_bridge:
			return -1
		used[candidate] = candidate_bridge
		if candidate_bridge == bridge_id:
			if resolved > 0 and resolved != candidate:
				return -1
			resolved = candidate
	if resolved > 0:
		return resolved
	var start: int = int(bridge_id.hash()) & 0x7fffffff
	if start <= 0:
		start = 1
	var candidate: int = start
	while used.has(candidate):
		candidate += 1
		if candidate > 2147483647:
			candidate = 1
		if candidate == start:
			return -1
	return candidate


func _generate_bridge_id() -> String:
	var crypto: Crypto = Crypto.new()
	return crypto.generate_random_bytes(16).hex_encode()

func _store_transaction(transaction: Dictionary) -> int:
	var transaction_id: int = _next_transaction_id
	_next_transaction_id += 1
	_transactions[transaction_id] = transaction
	return transaction_id

func _move_child_safely(parent: Node, child: Node, requested_index: int) -> void:
	if child.get_parent() != parent:
		return
	var maximum_index: int = maxi(parent.get_child_count() - 1, 0)
	parent.move_child(child, clampi(requested_index, 0, maximum_index))

func _objects_are_valid(objects: Array) -> bool:
	for object: Object in objects:
		if object == null or not is_instance_valid(object):
			return false
	return true

func _failure(message: String) -> Dictionary:
	_emit_progress(maxf(_last_progress_value, 0.0), "Failed: %s" % message, true)
	_log("ERROR: %s" % message)
	return {"ok": false, "error": message}

func _log(message: String) -> void:
	log_message.emit(message)

func _sort_placement_by_path_then_node(a: Dictionary, b: Dictionary) -> bool:
	var path_a: String = String(a["scene_path"])
	var path_b: String = String(b["scene_path"])
	return String(a["node_path"]) < String(b["node_path"]) if path_a == path_b else path_a < path_b

func _sort_binding_by_path(a: Dictionary, b: Dictionary) -> bool:
	return String(a.get("asset_key", a.get("scene_path", ""))) < String(b.get("asset_key", b.get("scene_path", "")))

func _sort_asset_entry_by_id(a: Dictionary, b: Dictionary) -> bool:
	return int(a.get("mesh_id", -1)) < int(b.get("mesh_id", -1))

func _sort_anchor_by_name(a: Node, b: Node) -> bool:
	return String(a.name) < String(b.name)

func _sort_vector2i(a: Vector2i, b: Vector2i) -> bool:
	return a.x < b.x if a.y == b.y else a.y < b.y
