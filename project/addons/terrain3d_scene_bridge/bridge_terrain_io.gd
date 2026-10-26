@tool
extends RefCounted

const META_BRIDGE_ID: StringName = &"terrain3d_scene_bridge_id"
const META_SCENE_PATH: StringName = &"terrain3d_scene_bridge_scene_path"
const META_MESH_PATH: StringName = &"terrain3d_scene_bridge_mesh_path"
const META_ASSET_KEY: StringName = &"terrain3d_scene_bridge_asset_key"
const META_OWNER_ID: StringName = &"terrain3d_scene_bridge_owner_id"
const OWNER_ID_MAX: int = 2147483647
const READ_PROGRESS_STRIDE: int = 256


func get_subdivision_info(terrain: Node) -> Dictionary:
	var result: Dictionary = {
		"supported": false,
		"owner_supported": false,
		"cell_size": 0,
		"cells_per_region_axis": 0,
		"region_size": 0,
		"vertex_spacing": 0.0,
		"cell_world_size": 0.0,
	}
	if terrain == null or not is_instance_valid(terrain):
		return result
	var instancer: Object = terrain.call("get_instancer")
	if instancer == null:
		return result
	var subdivision_supported: bool = (
		instancer.has_method("get_cell_size")
		and instancer.has_method("get_cells_per_region_axis")
	)
	var owner_supported: bool = (
		subdivision_supported
		and instancer.has_method("add_owned_transforms")
		and instancer.has_method("remove_owned_instances")
	)
	if not subdivision_supported:
		return result
	var cell_size: int = int(instancer.call("get_cell_size"))
	var cells_per_axis: int = int(instancer.call("get_cells_per_region_axis"))
	var region_size: int = int(terrain.call("get_region_size"))
	var spacing: float = float(terrain.call("get_vertex_spacing"))
	result["supported"] = cell_size > 0 and cells_per_axis > 0 and region_size > 0
	result["owner_supported"] = owner_supported
	result["cell_size"] = cell_size
	result["cells_per_region_axis"] = cells_per_axis
	result["region_size"] = region_size
	result["vertex_spacing"] = spacing
	result["cell_world_size"] = float(cell_size) * spacing
	return result


func validate_subdivision_info(info: Dictionary) -> String:
	if not bool(info.get("supported", false)):
		return "This Terrain3D build does not expose the fork's instancer subdivision API."
	var cell_size: int = int(info.get("cell_size", 0))
	var cells_per_axis: int = int(info.get("cells_per_region_axis", 0))
	var region_size: int = int(info.get("region_size", 0))
	if cell_size < 8 or cell_size > 128 or (cell_size & (cell_size - 1)) != 0:
		return "Terrain3D reports an invalid instancer cell size: %d." % cell_size
	if region_size <= 0 or region_size % cell_size != 0:
		return "Terrain3D instancer cell size %d does not divide region size %d." % [cell_size, region_size]
	var expected_cells_per_axis: int = int(region_size / cell_size)
	if cells_per_axis != expected_cells_per_axis:
		return "Terrain3D reports %d cells per region axis, but region size %d / cell size %d implies %d." % [
			cells_per_axis,
			region_size,
			cell_size,
			expected_cells_per_axis,
		]
	return ""


func resolve_native_placement(
		terrain: Node,
		global_transform: Transform3D,
		height_offset: float = 0.0
	) -> Dictionary:
	# Terrain3D owns an instance by the height-offset-adjusted transform origin.
	# Mesh bounds and instance scale are intentionally irrelevant to ownership:
	# the native cell AABB expands to the transformed mesh bounds at render time.
	if terrain == null or not is_instance_valid(terrain):
		return _route_failure(global_transform.origin, "Terrain3D target is unavailable.")
	var data: Object = terrain.call("get_data")
	if data == null:
		return _route_failure(global_transform.origin, "Terrain3D data is unavailable.")
	var info: Dictionary = get_subdivision_info(terrain)
	var subdivision_error: String = validate_subdivision_info(info)
	if not subdivision_error.is_empty():
		return _route_failure(global_transform.origin, subdivision_error)
	return resolve_native_placement_cached(data, info, global_transform, height_offset)


func resolve_native_placement_cached(
		data: Object,
		info: Dictionary,
		global_transform: Transform3D,
		height_offset: float = 0.0
	) -> Dictionary:
	if data == null:
		return _route_failure(global_transform.origin, "Terrain3D data is unavailable.")
	if not global_transform.origin.is_finite() or not global_transform.basis.is_finite():
		return _route_failure(global_transform.origin, "Instance transform contains non-finite values.")
	var route_key: Vector4i = resolve_native_key_cached(data, info, global_transform, height_offset)
	var adjusted: Transform3D = global_transform
	adjusted.origin += adjusted.basis.y * height_offset
	if route_key.z < 0 or route_key.w < 0:
		return _route_failure(adjusted.origin, "Instance transform cannot be mapped to a valid Terrain3D instancer cell.")
	return {
		"ok": true,
		"region_location": Vector2i(route_key.x, route_key.y),
		"cell_location": Vector2i(route_key.z, route_key.w),
		"adjusted_origin": adjusted.origin,
		"error": "",
	}


func resolve_native_key_cached(
		data: Object,
		info: Dictionary,
		global_transform: Transform3D,
		height_offset: float = 0.0
	) -> Vector4i:
	if data == null or not global_transform.origin.is_finite() or not global_transform.basis.is_finite():
		return Vector4i(0, 0, -1, -1)
	var adjusted: Transform3D = global_transform
	adjusted.origin += adjusted.basis.y * height_offset
	var region_location: Vector2i = data.call("get_region_location", adjusted.origin)
	var cell_location: Vector2i = _cell_for_global_adjusted_origin(region_location, adjusted.origin, info)
	return Vector4i(region_location.x, region_location.y, cell_location.x, cell_location.y)


func get_cell_for_global_transform(
		terrain: Node,
		region_location: Vector2i,
		global_transform: Transform3D,
		height_offset: float = 0.0
	) -> Vector2i:
	var info: Dictionary = get_subdivision_info(terrain)
	if not bool(info.get("supported", false)):
		return Vector2i(-1, -1)
	var adjusted: Transform3D = global_transform
	adjusted.origin += adjusted.basis.y * height_offset
	return _cell_for_global_adjusted_origin(region_location, adjusted.origin, info)


func count_subdivision_cells_for_bindings(terrain: Node, bindings: Array[Dictionary]) -> Dictionary:
	var info: Dictionary = get_subdivision_info(terrain)
	var result: Dictionary = {
		"supported": bool(info.get("supported", false)),
		"cell_count": 0,
		"cells": [],
	}
	if not bool(info.get("supported", false)):
		return result
	var data: Object = terrain.call("get_data")
	if data == null:
		return result

	# This is intentionally a cached routing pass. The previous implementation
	# re-fetched Terrain3D data/subdivision state twice per instance through
	# resolve_native_placement(), which dominated large bridge conversions.
	var unique: Dictionary = {}
	for binding: Dictionary in bindings:
		var transforms: Array[Transform3D] = binding.get("transforms", [])
		var mesh_asset: Object = binding.get("asset", null)
		var height_offset: float = float(mesh_asset.call("get_height_offset")) if mesh_asset != null else 0.0
		for transform: Transform3D in transforms:
			var key: Vector4i = resolve_native_key_cached(data, info, transform, height_offset)
			if key.z < 0 or key.w < 0:
				continue
			var region_location: Vector2i = Vector2i(key.x, key.y)
			var cell: Vector2i = Vector2i(key.z, key.w)
			if not unique.has(key):
				unique[key] = {
					"region_location": region_location,
					"cell_location": cell,
					"count": 0,
				}
			var cell_entry: Dictionary = unique[key]
			cell_entry["count"] = int(cell_entry["count"]) + 1
			unique[key] = cell_entry
	var cells: Array[Dictionary] = []
	for cell_entry: Dictionary in unique.values():
		cells.append(cell_entry)
	cells.sort_custom(_sort_cell_entry)
	result["cell_count"] = cells.size()
	result["cells"] = cells
	return result


func read_global_entries_by_mesh(
		terrain: Node,
		bindings: Array[Dictionary],
		owner_id: int = 0,
		progress_callback: Callable = Callable()
	) -> Dictionary:
	var result: Dictionary = {}
	var height_offsets: Dictionary = {}
	var target_mesh_ids: Array[int] = []
	var target_mesh_id_set: Dictionary = {}
	var assets: Object = terrain.call("get_assets")
	var data: Object = terrain.call("get_data")
	if assets == null or data == null:
		return result

	for binding: Dictionary in bindings:
		var mesh_id: int = int(binding.get("mesh_id", -1))
		if mesh_id < 0:
			continue
		if not target_mesh_id_set.has(mesh_id):
			var empty_entries: Array[Dictionary] = []
			result[mesh_id] = empty_entries
			target_mesh_ids.append(mesh_id)
			target_mesh_id_set[mesh_id] = true
			var mesh_asset: Object = assets.call("get_mesh_asset", mesh_id)
			height_offsets[mesh_id] = float(mesh_asset.call("get_height_offset")) if mesh_asset != null else 0.0

	target_mesh_ids.sort()
	var subdivision: Dictionary = get_subdivision_info(terrain)
	var subdivision_supported: bool = bool(subdivision.get("supported", false))
	var current_cell_size: int = int(subdivision.get("cell_size", 0))
	var vertex_spacing: float = float(terrain.call("get_vertex_spacing"))
	var instancer: Object = terrain.call("get_instancer")
	var region_locations: Array[Vector2i] = _get_binding_region_locations(data, bindings)
	var region_total: int = region_locations.size()
	for region_index: int in range(region_total):
		var region_location: Vector2i = region_locations[region_index]
		if progress_callback.is_valid():
			progress_callback.call(float(region_index) / float(maxi(region_total, 1)), "Reading Terrain3D instance regions")
		# Use Terrain3DData's demand-load path so the fork's persistent instance-page
		# cache/budget remains authoritative. Calling Region.get_instances() directly
		# on a paged region would materialize it outside the cache accounting.
		if data.has_method("ensure_instance_region_resident"):
			if not bool(data.call("ensure_instance_region_resident", region_location)):
				continue
		var region: Object = data.call("get_region", region_location)
		if region == null:
			continue
		# Older bridge-compatible forks exposed this helper to GDScript. Current
		# Terrain3D repartitions through native mutation/page-access paths instead,
		# so this remains an optional compatibility hook only.
		if instancer != null and instancer.has_method("ensure_region_layout"):
			if not bool(instancer.call("ensure_region_layout", region)):
				continue
		var instances: Dictionary = region.call("get_instances")
		var region_size: int = int(region.call("get_region_size"))
		var global_offset: Vector3 = Vector3(
			float(region_location.x * region_size) * vertex_spacing,
			0.0,
			float(region_location.y * region_size) * vertex_spacing
		)

		var mesh_total: int = target_mesh_ids.size()
		for mesh_index: int in range(mesh_total):
			var mesh_id: int = target_mesh_ids[mesh_index]
			if not instances.has(mesh_id):
				continue
			var cells: Dictionary = instances[mesh_id]
			var cell_keys: Array[Vector2i] = []
			for cell_location: Vector2i in cells.keys():
				cell_keys.append(cell_location)
			cell_keys.sort_custom(_sort_vector2i)

			var mesh_entries: Array[Dictionary] = result[mesh_id]
			var height_offset: float = float(height_offsets.get(mesh_id, 0.0))
			var cell_total: int = cell_keys.size()
			for cell_index: int in range(cell_total):
				var stored_cell: Vector2i = cell_keys[cell_index]
				var triple: Array = cells[stored_cell]
				if triple.is_empty():
					continue
				var local_transforms: Array[Transform3D] = triple[0]
				var colors: PackedColorArray = PackedColorArray()
				if triple.size() > 1 and typeof(triple[1]) == TYPE_PACKED_COLOR_ARRAY:
					colors = triple[1]
				var owners: PackedInt32Array = PackedInt32Array()
				if triple.size() > 3 and typeof(triple[3]) == TYPE_PACKED_INT32_ARRAY:
					owners = triple[3]
				var transform_total: int = local_transforms.size()
				for index: int in range(transform_total):
					if progress_callback.is_valid() and index > 0 and index % READ_PROGRESS_STRIDE == 0:
						var transform_fraction: float = float(index) / float(maxi(transform_total, 1))
						var cell_fraction: float = (float(cell_index) + transform_fraction) / float(maxi(cell_total, 1))
						var mesh_fraction: float = (float(mesh_index) + cell_fraction) / float(maxi(mesh_total, 1))
						progress_callback.call(
							(float(region_index) + mesh_fraction) / float(maxi(region_total, 1)),
							"Reading Terrain3D instance regions"
						)
					var entry_owner: int = int(owners[index]) if index < owners.size() else 0
					if owner_id > 0 and entry_owner != owner_id:
						continue
					var local_transform: Transform3D = local_transforms[index]
					var global_transform: Transform3D = local_transform
					global_transform.origin += global_offset
					global_transform.origin -= global_transform.basis.y * height_offset
					var current_cell: Vector2i = stored_cell
					if subdivision_supported:
						current_cell = _cell_for_region_local_origin(local_transform.origin, subdivision)
					mesh_entries.append({
						"transform": global_transform,
						"color": colors[index] if index < colors.size() else Color.WHITE,
						"owner_id": entry_owner,
						"region_location": region_location,
						"cell_location": current_cell,
						"stored_cell_location": stored_cell,
						"cell_size": current_cell_size,
					})
			result[mesh_id] = mesh_entries
	if progress_callback.is_valid():
		progress_callback.call(1.0, "Reading Terrain3D instance regions")
	for mesh_id: int in result.keys():
		var entries: Array[Dictionary] = result[mesh_id]
		entries.sort_custom(_sort_global_entry)
		result[mesh_id] = entries
	return result


func read_global_transforms_by_mesh(
		terrain: Node,
		bindings: Array[Dictionary],
		owner_id: int = 0
	) -> Dictionary:
	var result: Dictionary = {}
	var entries_by_mesh: Dictionary = read_global_entries_by_mesh(terrain, bindings, owner_id)
	for mesh_id: int in entries_by_mesh.keys():
		var transforms: Array[Transform3D] = []
		var entries: Array = entries_by_mesh[mesh_id]
		for entry: Dictionary in entries:
			transforms.append(entry["transform"])
		result[mesh_id] = transforms
	return result


func add_entry_map(
		instancer: Object,
		entry_map: Dictionary,
		owner_id: int,
		update: bool = false,
		backup: bool = false
	) -> int:
	if instancer == null:
		return 0
	var mesh_ids: Array[int] = []
	for mesh_id: int in entry_map.keys():
		mesh_ids.append(mesh_id)
	mesh_ids.sort()
	var total: int = 0
	for mesh_id: int in mesh_ids:
		var transforms: Array[Transform3D] = []
		var colors: PackedColorArray = PackedColorArray()
		var entries: Array = entry_map.get(mesh_id, [])
		for entry: Dictionary in entries:
			transforms.append(entry["transform"])
			var entry_color: Color = entry.get("color", Color.WHITE)
			colors.append(entry_color)
		if transforms.is_empty():
			continue
		if owner_id > 0 and instancer.has_method("add_owned_transforms"):
			instancer.call("add_owned_transforms", mesh_id, transforms, owner_id, colors, update, backup)
		else:
			instancer.call("add_transforms", mesh_id, transforms, colors, update)
		total += transforms.size()
	return total


func remove_batch_instances(
		instancer: Object,
		owner_id: int,
		affected_ids: Array,
		update: bool = true,
		backup: bool = false
	) -> int:
	if instancer == null:
		return 0
	if owner_id > 0 and instancer.has_method("remove_owned_instances"):
		return int(instancer.call("remove_owned_instances", owner_id, update, backup))
	for mesh_id: int in affected_ids:
		instancer.call("clear_by_mesh", mesh_id)
	return -1


func queue_binding_updates(instancer: Object, bindings: Array) -> void:
	if instancer == null:
		return
	for binding: Dictionary in bindings:
		var mesh_id: int = int(binding.get("mesh_id", -1))
		var region_locations: Array = binding.get("region_locations", [])
		for region_location: Vector2i in region_locations:
			instancer.call("update_mmis", mesh_id, region_location, false)


func get_transform_array(dictionary: Dictionary, key: int) -> Array[Transform3D]:
	var result: Array[Transform3D] = []
	if dictionary.has(key):
		result = dictionary[key]
	return result


# Legacy snapshot support remains for v1.0/v1.1 bridge anchors and stock Terrain3D.
func capture_region_instances(terrain: Node, affected_ids: Array[int]) -> Array[Dictionary]:
	var snapshots: Array[Dictionary] = []
	var data: Object = terrain.call("get_data")
	if data == null:
		return snapshots
	var regions: Array = data.call("get_regions_active")
	for region: Object in regions:
		if region == null:
			continue
		var instances: Dictionary = region.call("get_instances")
		var entries: Dictionary = {}
		for mesh_id: int in affected_ids:
			if instances.has(mesh_id):
				var cell_dictionary: Dictionary = instances[mesh_id]
				entries[mesh_id] = cell_dictionary.duplicate(true)
		snapshots.append({
			"location": region.call("get_location"),
			"entries": entries,
			"modified": bool(region.call("is_modified")),
		})
	return snapshots


func restore_region_instances(terrain: Node, affected_ids: Array, snapshots: Array[Dictionary]) -> void:
	var data: Object = terrain.call("get_data")
	if data == null:
		return
	for snapshot: Dictionary in snapshots:
		var location: Vector2i = snapshot["location"]
		var region: Object = data.call("get_region", location)
		if region == null:
			continue
		var instances: Dictionary = region.call("get_instances")
		for mesh_id: int in affected_ids:
			instances.erase(mesh_id)
		var entries: Dictionary = snapshot["entries"]
		for mesh_id: int in entries.keys():
			var cell_dictionary: Dictionary = entries[mesh_id]
			instances[mesh_id] = cell_dictionary.duplicate(true)
		region.call("set_instances", instances)
		region.call("set_modified", bool(snapshot["modified"]))


func queue_snapshot_updates(instancer: Object, snapshots: Array) -> void:
	for snapshot: Dictionary in snapshots:
		var location: Vector2i = snapshot["location"]
		var entries: Dictionary = snapshot["entries"]
		for mesh_id: int in entries.keys():
			instancer.call("update_mmis", mesh_id, location, false)


func validate_asset_identity(
		mesh_asset: Object,
		bridge_id: String,
		scene_path: String,
		mesh_id: int,
		owner_id: int = 0,
		mesh_path: String = "",
		asset_key: String = ""
	) -> String:
	if mesh_asset == null:
		return "Terrain3D mesh ID %d is missing for %s. The bridge will not restore from a stale mapping." % [mesh_id, scene_path]
	if String(mesh_asset.get_meta(META_BRIDGE_ID, "")) != bridge_id:
		return "Terrain3D mesh ID %d no longer belongs to bridge batch %s. Refusing to touch unrelated instances." % [mesh_id, bridge_id]
	if String(mesh_asset.get_meta(META_SCENE_PATH, "")) != scene_path:
		return "Terrain3D mesh ID %d now maps to a different source scene. Refusing destructive conversion." % mesh_id
	if not mesh_path.is_empty() and String(mesh_asset.get_meta(META_MESH_PATH, "")) != mesh_path:
		return "Terrain3D mesh ID %d now maps to a different MeshInstance3D path inside %s." % [mesh_id, scene_path]
	if not asset_key.is_empty() and String(mesh_asset.get_meta(META_ASSET_KEY, "")) != asset_key:
		return "Terrain3D mesh ID %d no longer matches extracted mesh key %s." % [mesh_id, asset_key]
	if owner_id > 0 and int(mesh_asset.get_meta(META_OWNER_ID, 0)) != owner_id:
		return "Terrain3D mesh ID %d no longer carries owner ID %d for bridge batch %s." % [mesh_id, owner_id, bridge_id]
	return ""


func validate_asset_entries(assets: Object, entries: Array, require_new_installed: bool) -> String:
	for entry: Dictionary in entries:
		if bool(entry["is_new"]) and not require_new_installed:
			continue
		var mesh_id: int = int(entry["mesh_id"])
		var expected_asset: Object = entry["asset"]
		var current_asset: Object = assets.call("get_mesh_asset", mesh_id)
		if current_asset != expected_asset:
			return "Terrain3D mesh ID %d changed after the bridge transaction was prepared. Refusing to clear or overwrite it." % mesh_id
	return ""


func validate_restore_bindings(
		assets: Object,
		bindings: Array,
		bridge_id: String,
		owner_id: int = 0
	) -> String:
	for binding: Dictionary in bindings:
		var mesh_id: int = int(binding["mesh_id"])
		var scene_path: String = String(binding["scene_path"])
		var mesh_path: String = String(binding.get("mesh_path", ""))
		var asset_key: String = String(binding.get("asset_key", ""))
		var current_asset: Object = assets.call("get_mesh_asset", mesh_id)
		var expected_asset: Object = binding.get("asset", null)
		if expected_asset != null and current_asset != expected_asset:
			return "Terrain3D mesh ID %d changed after restore preparation. Refusing to clear or repopulate it." % mesh_id
		var identity_error: String = validate_asset_identity(current_asset, bridge_id, scene_path, mesh_id, owner_id, mesh_path, asset_key)
		if not identity_error.is_empty():
			return identity_error
	return ""


func find_asset_id(assets: Object, target_asset: Object) -> int:
	var mesh_count: int = int(assets.call("get_mesh_count"))
	for mesh_id: int in range(mesh_count):
		if assets.call("get_mesh_asset", mesh_id) == target_asset:
			return mesh_id
	return -1


func _get_binding_region_locations(data: Object, bindings: Array[Dictionary]) -> Array[Vector2i]:
	var locations: Array[Vector2i] = []
	var location_set: Dictionary = {}
	var has_persisted_regions: bool = false
	for binding: Dictionary in bindings:
		var binding_locations: Array = binding.get("region_locations", [])
		if not binding_locations.is_empty():
			has_persisted_regions = true
		for location: Vector2i in binding_locations:
			if location_set.has(location):
				continue
			location_set[location] = true
			locations.append(location)

	# v1.0/v1.1 anchors did not persist their region list. Fall back to every
	# active region for those anchors, still using the fork's cache-aware demand
	# loading path in read_global_entries_by_mesh().
	if not has_persisted_regions and data != null:
		var active_locations: Array = data.call("get_region_locations")
		for location: Vector2i in active_locations:
			if location_set.has(location):
				continue
			location_set[location] = true
			locations.append(location)
	locations.sort_custom(_sort_vector2i)
	return locations


func _cell_for_global_adjusted_origin(region_location: Vector2i, adjusted_origin: Vector3, info: Dictionary) -> Vector2i:
	var region_size: int = int(info.get("region_size", 0))
	var spacing: float = float(info.get("vertex_spacing", 0.0))
	if region_size <= 0 or spacing <= 0.0:
		return Vector2i(-1, -1)
	var local_origin: Vector3 = adjusted_origin - Vector3(
		float(region_location.x * region_size) * spacing,
		0.0,
		float(region_location.y * region_size) * spacing
	)
	return _cell_for_region_local_origin(local_origin, info)


func _route_failure(origin: Vector3, error: String) -> Dictionary:
	return {
		"ok": false,
		"region_location": Vector2i.ZERO,
		"cell_location": Vector2i(-1, -1),
		"adjusted_origin": origin,
		"error": error,
	}


func _cell_for_region_local_origin(local_origin: Vector3, info: Dictionary) -> Vector2i:
	var region_size: int = int(info.get("region_size", 0))
	var cell_size: int = int(info.get("cell_size", 0))
	var cells_per_axis: int = int(info.get("cells_per_region_axis", 0))
	var spacing: float = float(info.get("vertex_spacing", 0.0))
	if region_size <= 0 or cell_size <= 0 or spacing <= 0.0:
		return Vector2i(-1, -1)
	var sample_x: int = floori(local_origin.x / spacing)
	var sample_y: int = floori(local_origin.z / spacing)
	var wrapped_x: int = ((sample_x % region_size) + region_size) % region_size
	var wrapped_y: int = ((sample_y % region_size) + region_size) % region_size
	var cell: Vector2i = Vector2i(int(wrapped_x / cell_size), int(wrapped_y / cell_size))
	if cell.x < 0 or cell.y < 0 or cell.x >= cells_per_axis or cell.y >= cells_per_axis:
		return Vector2i(-1, -1)
	return cell


func _sort_region_by_location(a: Object, b: Object) -> bool:
	var location_a: Vector2i = a.call("get_location")
	var location_b: Vector2i = b.call("get_location")
	if location_a.y == location_b.y:
		return location_a.x < location_b.x
	return location_a.y < location_b.y


func _sort_vector2i(a: Vector2i, b: Vector2i) -> bool:
	if a.y == b.y:
		return a.x < b.x
	return a.y < b.y


func _sort_cell_entry(a: Dictionary, b: Dictionary) -> bool:
	var region_a: Vector2i = a["region_location"]
	var region_b: Vector2i = b["region_location"]
	if region_a != region_b:
		return _sort_vector2i(region_a, region_b)
	return _sort_vector2i(a["cell_location"], b["cell_location"])


func _sort_global_entry(a: Dictionary, b: Dictionary) -> bool:
	var region_a: Vector2i = a["region_location"]
	var region_b: Vector2i = b["region_location"]
	if region_a != region_b:
		return _sort_vector2i(region_a, region_b)
	var cell_a: Vector2i = a.get("cell_location", Vector2i.ZERO)
	var cell_b: Vector2i = b.get("cell_location", Vector2i.ZERO)
	if cell_a != cell_b:
		return _sort_vector2i(cell_a, cell_b)
	var ta: Transform3D = a["transform"]
	var tb: Transform3D = b["transform"]
	if not is_equal_approx(ta.origin.z, tb.origin.z):
		return ta.origin.z < tb.origin.z
	if not is_equal_approx(ta.origin.x, tb.origin.x):
		return ta.origin.x < tb.origin.x
	return ta.origin.y < tb.origin.y
