# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends RefCounted
class_name Terrain3DHLODBaker

const TransactionalResourcePublisher = preload("res://addons/terrain_3d/tools/transactional_resource_publisher.gd")

const HLODVisualValidator = preload("res://addons/terrain_3d/tools/hlod_visual_validator.gd")

const NODE_FLAG_HLOD_VISUAL: int = 1 << 2
const MAX_LEVEL: int = 6
const REGION_SIZE: int = 128
const REGION_MAP_SIZE: int = Terrain3DData.REGION_MAP_SIZE
const REGION_MAP_MIN: int = -REGION_MAP_SIZE / 2
const DEFAULT_REDUCTION: float = 0.42
const HLOD_DIR: String = "_hlod"
const MANIFEST_NAME: String = "terrain3d_hlod_manifest.tres"
const BUILDER_VERSION: int = 7

class GeometryPart extends RefCounted:
	var mesh: Mesh
	var transform: Transform3D = Transform3D.IDENTITY
	var materials: Array[Material] = []
	var rendering_materials: Array[Terrain3DRenderingMaterial] = []

class Source extends RefCounted:
	var owner_region: Vector2i = Vector2i.ZERO
	var mesh_id: int = -1
	var spawn_id: int = 0
	var requested_min_level: int = 1
	var min_level: int = 1
	var parts: Array[GeometryPart] = []
	var bounds: AABB = AABB()
	var source_key: String = ""
	var source_signature_token: String = ""
	var source_triangles: int = 0
	var base_geometric_error: float = 0.0
	var deferred_instancer: bool = false
	var deferred_region: Terrain3DRegion

class BuiltCluster extends RefCounted:
	var level: int = 1
	var cluster: Vector2i = Vector2i.ZERO
	var region_min: Vector2i = Vector2i.ZERO
	var region_max: Vector2i = Vector2i.ZERO
	var package_path: String = ""
	var package: Terrain3DHLODPackage
	var source_signature: int = 0

var reduction_ratio: float = DEFAULT_REDUCTION
var _native_builder: Terrain3DHLODBuilder = Terrain3DHLODBuilder.new()
var _warnings: PackedStringArray = PackedStringArray()
var _errors: PackedStringArray = PackedStringArray()


func analyze(terrain: Terrain3D) -> Dictionary:
	_warnings.clear()
	_errors.clear()
	var max_level: int = clampi(terrain.hlod_max_level, 1, MAX_LEVEL)
	var sources: Array[Source] = _collect_sources(terrain, max_level)
	var per_level: PackedInt32Array = PackedInt32Array()
	per_level.resize(MAX_LEVEL + 1)
	var instancer_sources: int = 0
	var node_sources: int = 0
	var triangles: int = 0
	for source: Source in sources:
		per_level[source.min_level] += 1
		triangles += source.source_triangles
		if source.mesh_id >= 0:
			instancer_sources += 1
		else:
			node_sources += 1
	return {
		"sources": sources.size(),
		"instancer_sources": instancer_sources,
		"node_sources": node_sources,
		"source_triangles": triangles,
		"per_min_level": per_level,
		"warnings": _warnings.duplicate(),
		"errors": _errors.duplicate(),
	}


func validate_generated(terrain: Terrain3D) -> Dictionary:
	if not is_instance_valid(terrain) or terrain.data_directory.is_empty():
		return {"ok": false, "error": "Terrain3D data directory is unavailable", "valid": 0, "missing": 0, "invalid": 0}
	var manifest_path: String = _resolve_manifest_path(terrain)
	if manifest_path.is_empty() or not FileAccess.file_exists(manifest_path):
		return {"ok": false, "error": "HLOD manifest is missing", "missing_manifest": true, "valid": 0, "missing": 0, "invalid": 0}
	var manifest := ResourceLoader.load(manifest_path, "Terrain3DHLODManifest", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DHLODManifest
	if not is_instance_valid(manifest):
		return {"ok": false, "error": "HLOD manifest could not be loaded", "valid": 0, "missing": 0, "invalid": 1}

	var max_level: int = clampi(terrain.hlod_max_level, 1, MAX_LEVEL)
	var expected_build_signature: int = _current_build_signature(max_level)
	var messages: PackedStringArray = PackedStringArray()
	var manifest_invalid: bool = false
	if manifest.format != 3:
		manifest_invalid = true
		messages.append("Manifest format %d is unsupported; expected format 3" % manifest.format)
	if manifest.max_level != max_level:
		manifest_invalid = true
		messages.append("Manifest max level %d does not match active HLOD max level %d" % [manifest.max_level, max_level])
	if manifest.build_signature != expected_build_signature:
		manifest_invalid = true
		messages.append("Manifest build signature is stale for the active HLOD configuration")
	if manifest.runtime_blob.is_empty() or not manifest.validate_runtime_blob():
		manifest_invalid = true
		messages.append("Packed HLOD runtime hierarchy is missing or invalid")

	var valid: int = 0
	var missing: int = 0
	var invalid: int = 1 if manifest_invalid else 0
	for value: Variant in manifest.entries:
		if not value is Dictionary:
			invalid += 1
			continue
		var entry: Dictionary = value
		var path: String = String(entry.get("package_path", ""))
		if path.is_empty() or not FileAccess.file_exists(path):
			missing += 1
			continue
		var package := ResourceLoader.load(path, "Terrain3DHLODPackage", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DHLODPackage
		if not is_instance_valid(package) or not is_instance_valid(package.mesh) or \
				package.level != int(entry.get("level", 0)) or \
				package.cluster_location != entry.get("cluster", Vector2i.ZERO) or \
				package.region_min != entry.get("region_min", Vector2i.ZERO) or \
				package.region_max != entry.get("region_max", Vector2i.ZERO) or \
				package.source_signature != int(entry.get("source_signature", -1)) or \
				package.build_signature != int(entry.get("build_signature", -1)):
			invalid += 1
			continue
		valid += 1
	return {
		"ok": not manifest_invalid and missing == 0 and invalid == 0,
		"manifest_path": manifest_path,
		"entries": manifest.entries.size(),
		"valid": valid,
		"missing": missing,
		"invalid": invalid,
		"messages": messages,
	}


func bake(terrain: Terrain3D, dirty_only: bool = true) -> Dictionary:
	_warnings.clear()
	_errors.clear()
	if not is_instance_valid(terrain) or not is_instance_valid(terrain.data) or not is_instance_valid(terrain.assets):
		return _failure("Terrain3D data/assets are unavailable")
	if terrain.data_directory.is_empty():
		return _failure("Terrain3D data_directory must be configured before HLOD baking")
	var data_dir: String = terrain.data_directory.trim_suffix("/")
	var output_dir: String = data_dir.path_join(HLOD_DIR)
	var dir_error: Error = DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(output_dir))
	if dir_error != OK and dir_error != ERR_ALREADY_EXISTS:
		return _failure("Unable to create HLOD directory: %s" % error_string(dir_error))

	var max_level: int = clampi(terrain.hlod_max_level, 1, MAX_LEVEL)
	var sources: Array[Source] = _collect_sources(terrain, max_level)
	if not _errors.is_empty():
		return {"ok": false, "error": "HLOD source validation failed", "errors": _errors.duplicate(), "warnings": _warnings.duplicate()}
	if sources.is_empty():
		return _failure("No HLOD-eligible Terrain3D static sources were found")

	var existing: Terrain3DHLODManifest = null
	var manifest_path: String = _resolve_manifest_path(terrain)
	var manifest_dir_error: Error = DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(manifest_path.get_base_dir()))
	if manifest_dir_error != OK and manifest_dir_error != ERR_ALREADY_EXISTS:
		return _failure("Unable to create HLOD manifest directory: %s" % error_string(manifest_dir_error))
	if dirty_only and ResourceLoader.exists(manifest_path):
		existing = ResourceLoader.load(manifest_path, "Terrain3DHLODManifest", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DHLODManifest
	var existing_entries: Dictionary[String, Dictionary] = _index_existing_entries(existing)

	var build_signature: int = _current_build_signature(max_level)
	var level_clusters: Dictionary[Vector2i, BuiltCluster] = {}
	var all_clusters: Array[BuiltCluster] = []
	var package_updates: PackedStringArray = PackedStringArray()
	var reused: int = 0
	var built: int = 0

	for level: int in range(1, max_level + 1):
		var plans: Dictionary[Vector2i, Array] = {}
		# Every child proxy feeds exactly one parent cluster.
		if level > 1:
			for child_key: Vector2i in level_clusters:
				var child: BuiltCluster = level_clusters[child_key]
				var parent_key := Vector2i(_floor_div(child.cluster.x, 2), _floor_div(child.cluster.y, 2))
				if not plans.has(parent_key):
					plans[parent_key] = []
				plans[parent_key].append(child)
		# Sources enter the hierarchy once, at their legal minimum level.
		for source: Source in sources:
			if source.min_level != level:
				continue
			var span: int = 1 << level
			var key := _cluster_key_for_region(source.owner_region, span)
			if not plans.has(key):
				plans[key] = []
			plans[key].append(source)
		var next_clusters: Dictionary[Vector2i, BuiltCluster] = {}
		for cluster_key: Vector2i in plans:
			var result: Dictionary = _build_cluster(
				terrain, level, cluster_key, plans[cluster_key], output_dir,
				build_signature, existing_entries, dirty_only)
			if not bool(result.get("ok", false)):
				return result
			var cluster: BuiltCluster = result["cluster"]
			next_clusters[cluster_key] = cluster
			all_clusters.append(cluster)
			if bool(result.get("reused", false)):
				reused += 1
			else:
				built += 1
				package_updates.append(cluster.package_path)
		level_clusters = next_clusters

	var manifest := Terrain3DHLODManifest.new()
	manifest.format = 3
	manifest.max_level = max_level
	manifest.build_signature = build_signature
	manifest.built_unix = int(Time.get_unix_time_from_system())
	var entries: Array = []
	for cluster: BuiltCluster in all_clusters:
		var package: Terrain3DHLODPackage = cluster.package
		entries.append({
			"level": cluster.level,
			"cluster": cluster.cluster,
			"region_min": cluster.region_min,
			"region_max": cluster.region_max,
			"local_bounds": package.local_bounds,
			"package_path": cluster.package_path,
			"geometric_error": package.geometric_error,
			"estimated_gpu_bytes": package.estimated_gpu_bytes,
			"source_signature": package.source_signature,
			"build_signature": package.build_signature,
		})
	manifest.entries = entries
	_populate_source_policy(manifest, sources)
	if not manifest.compile_runtime_blob():
		return _failure("Unable to compile packed HLOD runtime hierarchy")
	if not manifest.validate_runtime_blob():
		return _failure("Compiled HLOD runtime hierarchy failed validation")

	# Packages are complete before the fixed manifest is published. A failed
	# package write therefore cannot make runtime detail suppression point at a
	# half-built hierarchy.
	var save_error: Error = _publish_manifest_transactionally(manifest, manifest_path)
	if save_error != OK:
		return _failure("Unable to publish HLOD manifest transactionally: %s" % error_string(save_error))
	_update_editor_file(manifest_path)
	for path: String in package_updates:
		_update_editor_file(path)
	var referenced_packages := PackedStringArray()
	for cluster: BuiltCluster in all_clusters:
		referenced_packages.append(cluster.package_path)
	_garbage_collect_packages(output_dir, referenced_packages)
	terrain.hlod_manifest_path = manifest_path
	terrain.reload_hlod_manifest()
	return {
		"ok": true,
		"manifest_path": manifest_path,
		"sources": sources.size(),
		"clusters": all_clusters.size(),
		"built": built,
		"reused": reused,
		"warnings": _warnings.duplicate(),
		"errors": _errors.duplicate(),
	}


func clear_generated(terrain: Terrain3D) -> Dictionary:
	if not is_instance_valid(terrain) or terrain.data_directory.is_empty():
		return _failure("Terrain3D data directory is unavailable")
	var data_dir: String = terrain.data_directory.trim_suffix("/")
	var manifest_path: String = _resolve_manifest_path(terrain)
	var output_dir: String = data_dir.path_join(HLOD_DIR)
	if FileAccess.file_exists(manifest_path):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(manifest_path))
		_update_editor_file(manifest_path)
	_remove_directory_recursive(output_dir)
	terrain.reload_hlod_manifest()
	return {"ok": true}


func _collect_sources(terrain: Terrain3D, max_level: int) -> Array[Source]:
	var sources: Array[Source] = []
	var world_region_size: float = float(REGION_SIZE)
	for location_value: Variant in terrain.data.get_region_locations():
		var location: Vector2i = location_value
		var region: Terrain3DRegion = terrain.data.get_region(location)
		if not is_instance_valid(region):
			continue
		_collect_instancer_sources(terrain, region, location, world_region_size, max_level, sources)
		_collect_node_sources(region, location, world_region_size, max_level, sources)
	return sources


func _collect_instancer_sources(terrain: Terrain3D, region: Terrain3DRegion,
		location: Vector2i, world_region_size: float, max_level: int, output: Array[Source]) -> void:
	# Build only a compact descriptor here. Geometry is materialized lazily in
	# _build_cluster after dirty-package reuse has already been decided. This
	# keeps Analyze Sources cheap and makes Bake Dirty skip unchanged instancer
	# geometry instead of rebuilding it merely to discover that it is unchanged.
	for mesh_id: int in _native_builder.get_instancer_mesh_ids(region):
		var asset := terrain.assets.get_mesh_asset(mesh_id) as Terrain3DMeshAsset
		if not is_instance_valid(asset) or not asset.is_enabled():
			continue
		if asset.instance_category == Terrain3DMeshAsset.INSTANCE_CATEGORY_CLUTTER:
			continue
		var analyzed: Dictionary = _native_builder.analyze_instancer_source(terrain, region, mesh_id)
		if not bool(analyzed.get("ok", false)):
			_warnings.append("Mesh asset %d in region %s excluded from HLOD: %s" % [
				mesh_id, location, String(analyzed.get("error", "unsupported source"))])
			continue
		var mesh: Mesh = asset.get_mesh(0)
		if not is_instance_valid(mesh):
			continue
		var source := Source.new()
		source.owner_region = location
		source.mesh_id = mesh_id
		source.requested_min_level = clampi(asset.hlod_min_level, 1, MAX_LEVEL)
		source.source_key = "instancer:%s:%d" % [location, mesh_id]
		source.source_triangles = int(analyzed.get("source_triangles", 0))
		source.deferred_instancer = true
		source.deferred_region = region
		var signature_parts: PackedStringArray = PackedStringArray([
			source.source_key,
			"category:%d" % int(asset.instance_category),
			"hlod_min:%d" % source.requested_min_level,
			str(analyzed.get("instance_signature", 0)),
			_mesh_content_signature(mesh),
		])
		for material: Material in _mesh_asset_materials(asset, mesh):
			signature_parts.append(_material_signature(material))
		if is_instance_valid(asset.rendering_material_override):
			signature_parts.append(_resource_content_signature(asset.rendering_material_override))
		source.source_signature_token = "|".join(signature_parts).md5_text()
		var region_transform := Transform3D(Basis.IDENTITY,
			Vector3(float(location.x) * world_region_size, 0.0, float(location.y) * world_region_size))
		source.bounds = region_transform * AABB(analyzed.get("bounds", AABB()))
		source.min_level = _minimum_legal_level(source.owner_region, source.bounds,
			source.requested_min_level, world_region_size, max_level)
		if source.min_level <= max_level:
			output.append(source)
		else:
			_warnings.append("Mesh asset %d in region %s spans beyond configured HLOD level %d and remains detailed" % [mesh_id, location, max_level])


func _collect_node_sources(region: Terrain3DRegion, location: Vector2i,
		world_region_size: float, max_level: int, output: Array[Source]) -> void:
	for descriptor: Terrain3DRegionNodeDescriptor in Terrain3DRegionNodeDescriptor.list_from_value(region.get_streamed_nodes()):
		if (descriptor.flags & NODE_FLAG_HLOD_VISUAL) == 0:
			continue
		var packed: PackedScene = descriptor.scene
		if not is_instance_valid(packed) and not descriptor.scene_path.is_empty():
			packed = load(descriptor.scene_path) as PackedScene
		if not is_instance_valid(packed):
			_warnings.append("HLOD visual %d in %s has no loadable scene" % [descriptor.id, location])
			continue
		var root: Node = packed.instantiate()
		if not root is Node3D:
			root.free()
			_warnings.append("HLOD visual %d in %s is not Node3D-rooted" % [descriptor.id, location])
			continue
		var unsafe_reason: String = HLODVisualValidator.validate(root)
		if not unsafe_reason.is_empty():
			root.free()
			_errors.append("HLOD visual %d in %s is unsafe to proxy: %s" % [descriptor.id, location, unsafe_reason])
			continue
		var source := Source.new()
		source.owner_region = location
		source.spawn_id = descriptor.id
		source.requested_min_level = clampi(descriptor.hlod_min_level, 1, MAX_LEVEL)
		source.source_key = "node:%s:%d:%s" % [location, descriptor.id, descriptor.scene_path]
		var region_transform := Transform3D(Basis.IDENTITY,
			Vector3(float(location.x) * world_region_size, 0.0, float(location.y) * world_region_size))
		var base_transform: Transform3D = region_transform * descriptor.transform
		_collect_node_geometry(root as Node3D, Transform3D.IDENTITY, base_transform, source, false)
		root.free()
		if source.parts.is_empty():
			_warnings.append("HLOD visual %d in %s contains no supported MeshInstance3D geometry" % [descriptor.id, location])
			continue
		source.source_triangles = _parts_triangle_count(source.parts)
		source.min_level = _minimum_legal_level(source.owner_region, source.bounds,
			source.requested_min_level, world_region_size, max_level)
		if source.min_level <= max_level:
			output.append(source)
		else:
			_warnings.append("HLOD visual %d in %s cannot fit within configured HLOD max level %d and remains detailed" % [descriptor.id, location, max_level])


func _collect_node_geometry(node: Node3D, parent_transform: Transform3D,
		base_transform: Transform3D, source: Source, apply_node_transform: bool = true) -> void:
	# Runtime activation overwrites the streamed scene root transform with the
	# region-local descriptor transform. Mirror that contract here: the packed
	# root's authored transform is ignored, while child-local transforms remain.
	var node_transform: Transform3D = node.transform if apply_node_transform else Transform3D.IDENTITY
	var local: Transform3D = parent_transform * node_transform
	if node is MeshInstance3D:
		var mesh_node := node as MeshInstance3D
		if is_instance_valid(mesh_node.mesh) and mesh_node.mesh.get_blend_shape_count() == 0 and not is_instance_valid(mesh_node.material_overlay):
			var part := GeometryPart.new()
			part.mesh = mesh_node.mesh
			part.transform = base_transform * local
			part.materials = _mesh_instance_materials(mesh_node)
			for material: Material in part.materials:
				part.rendering_materials.append(_canonical_rendering_material(material))
			source.parts.append(part)
			var bounds: AABB = part.transform * part.mesh.get_aabb()
			if source.parts.size() == 1:
				source.bounds = bounds
			else:
				source.bounds = source.bounds.merge(bounds)
	for child: Node in node.get_children():
		if child is Node3D:
			_collect_node_geometry(child as Node3D, local, base_transform, source, true)


func _build_cluster(terrain: Terrain3D, level: int, cluster_key: Vector2i, inputs: Array,
		output_dir: String, build_signature: int, existing_entries: Dictionary[String, Dictionary],
		dirty_only: bool) -> Dictionary:
	var span: int = 1 << level
	var region_min := _cluster_region_min(cluster_key, span)
	var region_max := region_min + Vector2i(span - 1, span - 1)
	var world_region_size: float = float(REGION_SIZE)
	var cluster_origin := Vector3(float(region_min.x) * world_region_size, 0.0,
		float(region_min.y) * world_region_size)
	var signature_parts: PackedStringArray = PackedStringArray(["L%d" % level, str(cluster_key), str(build_signature)])
	for input: Variant in inputs:
		if input is Source:
			var source: Source = input
			signature_parts.append(source.source_key)
			signature_parts.append(str(source.bounds))
			if not source.source_signature_token.is_empty():
				signature_parts.append(source.source_signature_token)
			else:
				for part: GeometryPart in source.parts:
					signature_parts.append(str(part.transform))
					signature_parts.append(_mesh_content_signature(part.mesh))
					for material: Material in part.materials:
						signature_parts.append(_material_signature(material))
		elif input is BuiltCluster:
			var child: BuiltCluster = input
			signature_parts.append(str(child.source_signature))
	var source_signature: int = _signature_int("\n".join(signature_parts))
	var entry_key: String = "%d:%d:%d" % [level, cluster_key.x, cluster_key.y]
	if dirty_only and existing_entries.has(entry_key):
		var old: Dictionary = existing_entries[entry_key]
		var old_path: String = String(old.get("package_path", ""))
		if int(old.get("source_signature", -1)) == source_signature and int(old.get("build_signature", -1)) == build_signature and FileAccess.file_exists(old_path):
			var loaded := ResourceLoader.load(old_path, "Terrain3DHLODPackage", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DHLODPackage
			if is_instance_valid(loaded) and is_instance_valid(loaded.mesh) and \
					loaded.level == level and loaded.cluster_location == cluster_key and \
					loaded.region_min == region_min and loaded.region_max == region_max and \
					loaded.source_signature == source_signature and loaded.build_signature == build_signature and \
					loaded.has_canonical_rendering_materials():
				var reused := BuiltCluster.new()
				reused.level = level; reused.cluster = cluster_key; reused.region_min = region_min; reused.region_max = region_max
				reused.package_path = old_path; reused.package = loaded; reused.source_signature = source_signature
				return {"ok": true, "cluster": reused, "reused": true}

	var materialized: Dictionary = _materialize_deferred_instancer_sources(terrain, inputs)
	if not bool(materialized.get("ok", false)):
		return materialized
	var merged_geometry: Dictionary = _merge_cluster_geometry(inputs, cluster_origin, world_region_size)
	if not bool(merged_geometry.get("ok", false)):
		return merged_geometry
	var exact_mesh: ArrayMesh = merged_geometry.get("mesh") as ArrayMesh
	var exact_rendering_materials: Array[Terrain3DRenderingMaterial] = merged_geometry.get("rendering_materials", [])
	if not is_instance_valid(exact_mesh) or exact_mesh.get_surface_count() == 0:
		return _failure("HLOD cluster L%d %s produced no geometry" % [level, cluster_key])
	var simplified: Dictionary = _native_builder.simplify_mesh(exact_mesh, reduction_ratio)
	if not bool(simplified.get("ok", false)):
		return _failure("HLOD cluster L%d %s simplification failed: %s" % [
			level, cluster_key, String(simplified.get("error", "unknown error"))])
	var proxy_mesh: ArrayMesh = simplified.get("mesh") as ArrayMesh
	if not is_instance_valid(proxy_mesh) or proxy_mesh.get_surface_count() == 0:
		return _failure("HLOD cluster L%d %s simplifier produced no proxy mesh" % [level, cluster_key])
	var package := Terrain3DHLODPackage.new()
	package.level = level
	package.cluster_location = cluster_key
	package.region_min = region_min
	package.region_max = region_max
	package.mesh = proxy_mesh
	package.rendering_materials = exact_rendering_materials
	if not package.has_canonical_rendering_materials():
		return _failure("HLOD cluster L%d %s produced an invalid canonical Terrain3DRenderingMaterial table" % [level, cluster_key])
	package.local_bounds = proxy_mesh.get_aabb()
	package.geometric_error = _inherited_geometric_error(inputs) + float(simplified.get("geometric_error", 0.0))
	package.source_signature = source_signature
	package.build_signature = build_signature
	package.source_triangles = int(simplified.get("source_triangles", _mesh_triangle_count(exact_mesh)))
	package.proxy_triangles = int(simplified.get("proxy_triangles", _mesh_triangle_count(proxy_mesh)))
	package.estimated_gpu_bytes = _estimate_mesh_bytes(proxy_mesh)
	package.built_unix = int(Time.get_unix_time_from_system())
	var level_dir: String = output_dir.path_join("L%d" % level)
	var dir_error: Error = DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(level_dir))
	if dir_error != OK and dir_error != ERR_ALREADY_EXISTS:
		return _failure("Unable to create HLOD level directory: %s" % error_string(dir_error))
	var content_signature: int = _signature_int("%d|%d" % [source_signature, build_signature])
	var signature_hex: String = "%016x" % (content_signature & 0x7fffffffffffffff)
	var package_path: String = level_dir.path_join("L%d_%d_%d_%s.res" % [level, cluster_key.x, cluster_key.y, signature_hex])
	var error: Error = ResourceSaver.save(package, package_path)
	if error != OK:
		return _failure("Unable to save HLOD package %s: %s" % [package_path, error_string(error)])
	var built := BuiltCluster.new()
	built.level = level; built.cluster = cluster_key; built.region_min = region_min; built.region_max = region_max
	built.package_path = package_path; built.package = package; built.source_signature = source_signature
	return {"ok": true, "cluster": built, "reused": false}


func _materialize_deferred_instancer_sources(terrain: Terrain3D, inputs: Array) -> Dictionary:
	for input: Variant in inputs:
		if not input is Source:
			continue
		var source: Source = input
		if not source.deferred_instancer or not source.parts.is_empty():
			continue
		if not is_instance_valid(source.deferred_region):
			return _failure("HLOD instancer source region became unavailable during bake")
		var built: Dictionary = _native_builder.build_instancer_source(terrain, source.deferred_region, source.mesh_id)
		if not bool(built.get("ok", false)):
			return _failure("HLOD instancer source %s failed: %s" % [source.source_key, String(built.get("error", "unknown error"))])
		var chunks: Array = built.get("chunks", [])
		if chunks.is_empty():
			return _failure("HLOD instancer source %s produced no bounded chunks" % source.source_key)
		var region_transform := Transform3D(Basis.IDENTITY,
			Vector3(float(source.owner_region.x) * float(REGION_SIZE), 0.0, float(source.owner_region.y) * float(REGION_SIZE)))
		var asset := terrain.assets.get_mesh_asset(source.mesh_id) as Terrain3DMeshAsset
		if not is_instance_valid(asset):
			return _failure("HLOD instancer source %s lost its mesh asset during bake" % source.source_key)
		for chunk_value: Variant in chunks:
			if not chunk_value is Dictionary:
				continue
			var chunk: Dictionary = chunk_value
			var mesh: ArrayMesh = chunk.get("mesh") as ArrayMesh
			if not is_instance_valid(mesh) or mesh.get_surface_count() == 0:
				continue
			var part := GeometryPart.new()
			part.mesh = mesh
			part.transform = region_transform
			for surface: int in range(mesh.get_surface_count()):
				var material: Material = mesh.surface_get_material(surface)
				part.materials.append(material)
				var rendering_material: Terrain3DRenderingMaterial = asset.rendering_material_override
				if not is_instance_valid(rendering_material):
					rendering_material = _canonical_rendering_material(material)
				part.rendering_materials.append(rendering_material)
			source.parts.append(part)
		if source.parts.is_empty():
			return _failure("HLOD instancer source %s produced no usable chunk geometry" % source.source_key)
		source.base_geometric_error = float(built.get("geometric_error", 0.0))
		if int(built.get("chunk_count", 1)) > 1:
			_warnings.append("%s: %d bounded chunks, %d source -> %d preproxy triangles" % [
				source.source_key, int(built.get("chunk_count", 0)), source.source_triangles, int(built.get("proxy_triangles", 0))])
	return {"ok": true}


func _merge_cluster_geometry(inputs: Array, cluster_origin: Vector3, world_region_size: float) -> Dictionary:
	var tools: Dictionary[String, SurfaceTool] = {}
	var materials: Dictionary[String, Material] = {}
	var rendering_materials: Dictionary[String, Terrain3DRenderingMaterial] = {}
	for input: Variant in inputs:
		if input is Source:
			var source: Source = input
			for part: GeometryPart in source.parts:
				if not _append_part(tools, materials, rendering_materials, part.mesh,
						_cluster_local_transform(part.transform, cluster_origin), part.materials, part.rendering_materials):
					return _failure("HLOD source %s contains a material that cannot be converted to Terrain3DRenderingMaterial" % source.source_key)
		elif input is BuiltCluster:
			var child: BuiltCluster = input
			if not is_instance_valid(child.package) or not is_instance_valid(child.package.mesh):
				continue
			if not child.package.has_canonical_rendering_materials():
				return _failure("HLOD child package L%d %s is missing canonical rendering materials" % [child.level, child.cluster])
			var package_mesh: Mesh = child.package.mesh
			var child_origin := Vector3(float(child.region_min.x) * world_region_size, 0.0,
				float(child.region_min.y) * world_region_size)
			var child_transform := Transform3D(Basis.IDENTITY, child_origin - cluster_origin)
			var child_materials: Array[Material] = []
			var child_rendering_materials: Array[Terrain3DRenderingMaterial] = []
			for surface: int in range(package_mesh.get_surface_count()):
				child_materials.append(package_mesh.surface_get_material(surface))
				child_rendering_materials.append(child.package.rendering_materials[surface])
			if not _append_part(tools, materials, rendering_materials, package_mesh, child_transform,
					child_materials, child_rendering_materials):
				return _failure("HLOD child package L%d %s is missing canonical rendering materials" % [child.level, child.cluster])
	var merged := ArrayMesh.new()
	var merged_rendering_materials: Array[Terrain3DRenderingMaterial] = []
	for key: String in tools:
		var tool: SurfaceTool = tools[key]
		if tools.size() > 0:
			tool.index()
		tool.set_material(materials.get(key))
		merged = tool.commit(merged)
		merged_rendering_materials.append(rendering_materials.get(key))
	return {"ok": true, "mesh": merged, "rendering_materials": merged_rendering_materials}



func _mesh_surface_primitive(mesh: Mesh, surface: int) -> int:
	if mesh is ArrayMesh:
		var array_mesh := mesh as ArrayMesh
		return array_mesh.surface_get_primitive_type(surface)
	# Generic Mesh exposes its primitive query through the virtual surface API.
	return int(mesh.call("_surface_get_primitive_type", surface))


func _append_part(tools: Dictionary[String, SurfaceTool], materials: Dictionary[String, Material],
		rendering_materials: Dictionary[String, Terrain3DRenderingMaterial], mesh: Mesh, transform: Transform3D,
		effective_materials: Array[Material], effective_rendering_materials: Array[Terrain3DRenderingMaterial]) -> bool:
	if not is_instance_valid(mesh):
		return true
	for surface: int in range(mesh.get_surface_count()):
		if _mesh_surface_primitive(mesh, surface) != Mesh.PRIMITIVE_TRIANGLES:
			continue
		var material: Material = effective_materials[surface] if surface < effective_materials.size() else mesh.surface_get_material(surface)
		var rendering_material: Terrain3DRenderingMaterial = effective_rendering_materials[surface] if surface < effective_rendering_materials.size() else _canonical_rendering_material(material)
		if not is_instance_valid(rendering_material) or not rendering_material.is_one_submit_compatible():
			return false
		var key: String = _resource_key(material) + "|t3drm:" + _resource_content_signature(rendering_material)
		if not tools.has(key):
			var tool := SurfaceTool.new()
			tool.begin(Mesh.PRIMITIVE_TRIANGLES)
			tools[key] = tool
			materials[key] = material
			rendering_materials[key] = rendering_material
		var tool: SurfaceTool = tools[key]
		tool.append_from(mesh, surface, transform)
	return true



func _populate_source_policy(manifest: Terrain3DHLODManifest, sources: Array[Source]) -> void:
	var mesh_policy := PackedByteArray()
	mesh_policy.resize(4096 * Terrain3DAssets.MAX_MESH_ASSETS)
	var node_regions := PackedInt32Array()
	var node_ids := PackedInt64Array()
	var node_levels := PackedByteArray()
	for source: Source in sources:
		var region_index: int = _region_map_index(source.owner_region)
		if region_index < 0:
			continue
		if source.mesh_id >= 0:
			var offset: int = region_index * Terrain3DAssets.MAX_MESH_ASSETS + source.mesh_id
			mesh_policy[offset] = maxi(mesh_policy[offset], source.min_level)
		elif source.spawn_id > 0:
			node_regions.append(region_index)
			node_ids.append(source.spawn_id)
			node_levels.append(source.min_level)
	manifest.instancer_min_levels = mesh_policy
	manifest.node_region_indices = node_regions
	manifest.node_spawn_ids = node_ids
	manifest.node_min_levels = node_levels


func _minimum_legal_level(owner_region: Vector2i, bounds: AABB,
		requested: int, world_region_size: float, max_level: int) -> int:
	var epsilon: float = maxf(world_region_size * 0.000001, 0.0001)
	var bounds_end: Vector3 = bounds.end - Vector3(epsilon, 0.0, epsilon)
	var source_min := Vector2i(floori(bounds.position.x / world_region_size), floori(bounds.position.z / world_region_size))
	var source_max := Vector2i(floori(bounds_end.x / world_region_size), floori(bounds_end.z / world_region_size))
	for level: int in range(clampi(requested, 1, max_level), max_level + 1):
		var span: int = 1 << level
		var cluster_min := _cluster_region_min(_cluster_key_for_region(owner_region, span), span)
		var cluster_max := cluster_min + Vector2i(span - 1, span - 1)
		if source_min.x >= cluster_min.x and source_min.y >= cluster_min.y and source_max.x <= cluster_max.x and source_max.y <= cluster_max.y:
			return level
	return max_level + 1



func _cluster_local_transform(world_transform: Transform3D, cluster_origin: Vector3) -> Transform3D:
	var local: Transform3D = world_transform
	local.origin -= cluster_origin
	return local


func _mesh_content_signature(mesh: Mesh) -> String:
	if not is_instance_valid(mesh):
		return "mesh:null"
	var parts := PackedStringArray([mesh.resource_path])
	if not mesh.resource_path.is_empty() and FileAccess.file_exists(mesh.resource_path):
		parts.append(FileAccess.get_md5(mesh.resource_path))
	for surface: int in range(mesh.get_surface_count()):
		var arrays: Array = mesh.surface_get_arrays(surface)
		parts.append(var_to_bytes(arrays).hex_encode().md5_text())
	return "|".join(parts).md5_text()


func _material_signature(material: Material) -> String:
	if not is_instance_valid(material):
		return "material:null"
	var parts := PackedStringArray([material.get_class(), material.resource_path])
	if not material.resource_path.is_empty() and FileAccess.file_exists(material.resource_path):
		parts.append(FileAccess.get_md5(material.resource_path))
	for property_value: Dictionary in material.get_property_list():
		if (int(property_value.get("usage", 0)) & PROPERTY_USAGE_STORAGE) == 0:
			continue
		var property_name: StringName = property_value.get("name", &"")
		if property_name == &"resource_path" or property_name == &"resource_name":
			continue
		var value: Variant = material.get(property_name)
		var visited: Dictionary[int, bool] = {}
		parts.append(String(property_name) + "=" + _variant_content_signature(value, visited, 0))
	return "|".join(parts).md5_text()


func _resource_content_signature(resource: Resource) -> String:
	var visited: Dictionary[int, bool] = {}
	return _resource_content_signature_recursive(resource, visited, 0)


func _resource_content_signature_recursive(resource: Resource, visited: Dictionary[int, bool], depth: int) -> String:
	if not is_instance_valid(resource):
		return "null"
	if not resource.resource_path.is_empty() and FileAccess.file_exists(resource.resource_path):
		return resource.resource_path + ":" + FileAccess.get_md5(resource.resource_path)
	if depth >= 12:
		return resource.get_class() + ":depth-limit"
	var instance_id: int = resource.get_instance_id()
	if visited.has(instance_id):
		return resource.get_class() + ":cycle"
	visited[instance_id] = true
	var names: PackedStringArray = PackedStringArray()
	var values: Dictionary[String, String] = {}
	for property_value: Dictionary in resource.get_property_list():
		if (int(property_value.get("usage", 0)) & PROPERTY_USAGE_STORAGE) == 0:
			continue
		var property_name: StringName = property_value.get("name", &"")
		if property_name == &"resource_path" or property_name == &"resource_name":
			continue
		var name: String = String(property_name)
		names.append(name)
		values[name] = _variant_content_signature(resource.get(property_name), visited, depth + 1)
	names.sort()
	var parts: PackedStringArray = PackedStringArray([resource.get_class()])
	for name: String in names:
		parts.append(name + "=" + values[name])
	visited.erase(instance_id)
	return "|".join(parts).md5_text()


func _variant_content_signature(value: Variant, visited: Dictionary[int, bool], depth: int) -> String:
	if value is Resource:
		return _resource_content_signature_recursive(value as Resource, visited, depth)
	if value is Array:
		var array_value: Array = value
		var array_parts: PackedStringArray = PackedStringArray()
		for item: Variant in array_value:
			array_parts.append(_variant_content_signature(item, visited, depth + 1))
		return "array[" + ",".join(array_parts) + "]"
	if value is Dictionary:
		var dictionary_value: Dictionary = value
		var dictionary_parts: PackedStringArray = PackedStringArray()
		for key: Variant in dictionary_value:
			dictionary_parts.append(_variant_content_signature(key, visited, depth + 1) + ":" + _variant_content_signature(dictionary_value[key], visited, depth + 1))
		dictionary_parts.sort()
		return "dict{" + ",".join(dictionary_parts) + "}"
	return var_to_bytes(value).hex_encode().md5_text()


func _mesh_asset_materials(asset: Terrain3DMeshAsset, mesh: Mesh) -> Array[Material]:
	var result: Array[Material] = []
	for surface: int in range(mesh.get_surface_count()):
		var material: Material = asset.material_override
		if not is_instance_valid(material):
			material = mesh.surface_get_material(surface)
		result.append(material)
	return result


func _canonical_rendering_material(material: Material) -> Terrain3DRenderingMaterial:
	var rendering_material := Terrain3DRenderingMaterial.new()
	if is_instance_valid(material) and not rendering_material.import_base_material(material):
		return null
	return rendering_material


func _mesh_instance_materials(node: MeshInstance3D) -> Array[Material]:
	var result: Array[Material] = []
	for surface: int in range(node.mesh.get_surface_count()):
		var material: Material = node.material_override
		if not is_instance_valid(material):
			material = node.get_surface_override_material(surface)
		if not is_instance_valid(material):
			material = node.mesh.surface_get_material(surface)
		result.append(material)
	return result


func _parts_triangle_count(parts: Array[GeometryPart]) -> int:
	var total: int = 0
	for part: GeometryPart in parts:
		total += _mesh_triangle_count(part.mesh)
	return total


func _mesh_triangle_count(mesh: Mesh) -> int:
	var count: int = 0
	if not is_instance_valid(mesh):
		return 0
	for surface: int in range(mesh.get_surface_count()):
		if _mesh_surface_primitive(mesh, surface) != Mesh.PRIMITIVE_TRIANGLES:
			continue
		var arrays: Array = mesh.surface_get_arrays(surface)
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		count += (indices.size() if not indices.is_empty() else vertices.size()) / 3
	return count


func _estimate_mesh_bytes(mesh: Mesh) -> int:
	var bytes: int = 0
	for surface: int in range(mesh.get_surface_count()):
		var arrays: Array = mesh.surface_get_arrays(surface)
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
		var tangents: PackedFloat32Array = arrays[Mesh.ARRAY_TANGENT]
		var uv: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV]
		var uv2: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV2]
		var colors: PackedColorArray = arrays[Mesh.ARRAY_COLOR]
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		bytes += vertices.size() * 12 + normals.size() * 12 + tangents.size() * 4
		bytes += uv.size() * 8 + uv2.size() * 8 + colors.size() * 16 + indices.size() * 4
	return bytes


func _inherited_geometric_error(inputs: Array) -> float:
	var error: float = 0.0
	for input: Variant in inputs:
		if input is BuiltCluster:
			var child: BuiltCluster = input
			if is_instance_valid(child.package):
				error = maxf(error, child.package.geometric_error)
		elif input is Source:
			var source: Source = input
			error = maxf(error, source.base_geometric_error)
	return error



func _index_existing_entries(manifest: Terrain3DHLODManifest) -> Dictionary[String, Dictionary]:
	var result: Dictionary[String, Dictionary] = {}
	if not is_instance_valid(manifest):
		return result
	for value: Variant in manifest.entries:
		if not value is Dictionary:
			continue
		var entry: Dictionary = value
		var cluster: Vector2i = entry.get("cluster", Vector2i.ZERO)
		var level: int = int(entry.get("level", 0))
		result["%d:%d:%d" % [level, cluster.x, cluster.y]] = entry
	return result


func _resolve_manifest_path(terrain: Terrain3D) -> String:
	if not is_instance_valid(terrain):
		return ""
	var configured: String = terrain.hlod_manifest_path.strip_edges()
	if not configured.is_empty():
		if configured.is_absolute_path():
			configured = ProjectSettings.localize_path(configured)
		elif not configured.begins_with("res://") and not configured.begins_with("user://"):
			configured = terrain.data_directory.trim_suffix("/").path_join(configured)
		return configured
	if terrain.data_directory.is_empty():
		return ""
	return terrain.data_directory.trim_suffix("/").path_join(MANIFEST_NAME)


func _current_build_signature(max_level: int) -> int:
	return _signature_int(
		"terrain3d-hlod-v%d|max=%d|reduction=%.8f|geometry=128x1m" % [
			BUILDER_VERSION, max_level, reduction_ratio])


func _signature_int(text: String) -> int:
	return text.md5_text().substr(0, 15).hex_to_int()


func _resource_key(resource: Resource) -> String:
	if not is_instance_valid(resource):
		return "material:null"
	return resource.resource_path if not resource.resource_path.is_empty() else "rid:%s" % resource.get_rid()


func _floor_div(value: int, divisor: int) -> int:
	return floori(float(value) / float(divisor))


func _cluster_key_for_region(region: Vector2i, span: int) -> Vector2i:
	# Cluster coordinates are map-relative, not world-zero-relative. This keeps
	# the fixed -32..31 Terrain3D domain as one exact power-of-two quadtree and
	# produces a single HLOD6 root for the complete 64x64 map.
	return Vector2i(
		_floor_div(region.x - REGION_MAP_MIN, span),
		_floor_div(region.y - REGION_MAP_MIN, span))


func _cluster_region_min(cluster: Vector2i, span: int) -> Vector2i:
	return Vector2i(REGION_MAP_MIN + cluster.x * span, REGION_MAP_MIN + cluster.y * span)


func _region_map_index(location: Vector2i) -> int:
	var x: int = location.x - REGION_MAP_MIN
	var z: int = location.y - REGION_MAP_MIN
	if x < 0 or x >= REGION_MAP_SIZE or z < 0 or z >= REGION_MAP_SIZE:
		return -1
	return z * REGION_MAP_SIZE + x


func _publish_manifest_transactionally(manifest: Terrain3DHLODManifest, manifest_path: String) -> Error:
	var directory: String = manifest_path.get_base_dir()
	var stem: String = manifest_path.get_file().get_basename()
	var extension: String = manifest_path.get_extension()
	if extension.is_empty():
		extension = "tres"
	var staging_path: String = directory.path_join(stem + ".staging." + extension)
	var previous_path: String = directory.path_join(stem + ".previous." + extension)
	var expected_entries: int = manifest.entries.size()
	return TransactionalResourcePublisher.publish(
		manifest, manifest_path, staging_path, previous_path, "Terrain3DHLODManifest",
		func(resource: Resource) -> bool:
			var validation := resource as Terrain3DHLODManifest
			return is_instance_valid(validation) and validation.entries.size() == expected_entries and validation.validate_runtime_blob())


func _garbage_collect_packages(output_dir: String, referenced_packages: PackedStringArray) -> void:
	var referenced: Dictionary[String, bool] = {}
	for path: String in referenced_packages:
		referenced[path] = true
	_garbage_collect_directory(output_dir, referenced)


func _garbage_collect_directory(path: String, referenced: Dictionary[String, bool]) -> bool:
	var absolute: String = ProjectSettings.globalize_path(path)
	if not DirAccess.dir_exists_absolute(absolute):
		return true
	var dir := DirAccess.open(absolute)
	if dir == null:
		return false
	var empty: bool = true
	dir.list_dir_begin()
	var name: String = dir.get_next()
	while not name.is_empty():
		if name != "." and name != "..":
			var child: String = path.path_join(name)
			if dir.current_is_dir():
				if _garbage_collect_directory(child, referenced):
					DirAccess.remove_absolute(ProjectSettings.globalize_path(child))
				else:
					empty = false
			elif name.get_extension().to_lower() in ["res", "tres"] and not referenced.has(child):
				DirAccess.remove_absolute(ProjectSettings.globalize_path(child))
				_update_editor_file(child)
			else:
				empty = false
		name = dir.get_next()
	dir.list_dir_end()
	return empty


func _update_editor_file(path: String) -> void:
	if not Engine.is_editor_hint():
		return
	var fs: EditorFileSystem = EditorInterface.get_resource_filesystem()
	if is_instance_valid(fs):
		fs.update_file(path)


func _remove_directory_recursive(path: String) -> void:
	var absolute: String = ProjectSettings.globalize_path(path)
	if not DirAccess.dir_exists_absolute(absolute):
		return
	var dir := DirAccess.open(absolute)
	if dir == null:
		return
	dir.list_dir_begin()
	var name: String = dir.get_next()
	while not name.is_empty():
		if name != "." and name != "..":
			var child: String = absolute.path_join(name)
			if dir.current_is_dir():
				_remove_directory_recursive(ProjectSettings.localize_path(child))
			else:
				DirAccess.remove_absolute(child)
				_update_editor_file(ProjectSettings.localize_path(child))
		name = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(absolute)


func _failure(message: String) -> Dictionary:
	return {"ok": false, "error": message, "warnings": _warnings.duplicate(), "errors": _errors.duplicate()}
