# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends RefCounted
class_name Terrain3DHLODBaker

const NODE_FLAG_HLOD_VISUAL: int = 1 << 2
const MAX_LEVEL: int = 6
const REGION_MAP_SIZE: int = Terrain3DData.REGION_MAP_SIZE
const REGION_MAP_MIN: int = -REGION_MAP_SIZE / 2
const DEFAULT_REDUCTION: float = 0.42
const HLOD_DIR: String = "_hlod"
const MANIFEST_NAME: String = "terrain3d_hlod_manifest.tres"
const BUILDER_VERSION: int = 3

class GeometryPart extends RefCounted:
	var mesh: Mesh
	var transform: Transform3D = Transform3D.IDENTITY
	var materials: Array[Material] = []

class Source extends RefCounted:
	var owner_region: Vector2i = Vector2i.ZERO
	var mesh_id: int = -1
	var spawn_id: int = 0
	var requested_min_level: int = 1
	var min_level: int = 1
	var parts: Array[GeometryPart] = []
	var bounds: AABB = AABB()
	var source_key: String = ""
	var source_triangles: int = 0

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
	var manifest_path: String = terrain.hlod_manifest_path.strip_edges()
	if manifest_path.is_empty():
		manifest_path = terrain.data_directory.trim_suffix("/").path_join(MANIFEST_NAME)
	if not FileAccess.file_exists(manifest_path):
		return {"ok": false, "error": "HLOD manifest is missing", "missing_manifest": true, "valid": 0, "missing": 0, "invalid": 0}
	var manifest := ResourceLoader.load(manifest_path, "Terrain3DHLODManifest", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DHLODManifest
	if not is_instance_valid(manifest):
		return {"ok": false, "error": "HLOD manifest could not be loaded", "valid": 0, "missing": 0, "invalid": 1}
	var manifest_invalid: bool = manifest.format != 1 or manifest.region_map_size != REGION_MAP_SIZE or \
		manifest.region_size != terrain.region_size or not is_equal_approx(manifest.vertex_spacing, terrain.vertex_spacing)
	var valid: int = 0
	var missing: int = 0
	var invalid: int = 1 if manifest_invalid else 0
	var messages: PackedStringArray = PackedStringArray()
	if manifest_invalid:
		messages.append("Manifest terrain/build geometry contract does not match the active Terrain3D node")
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
	var manifest_path: String = data_dir.path_join(MANIFEST_NAME)
	if dirty_only and ResourceLoader.exists(manifest_path):
		existing = ResourceLoader.load(manifest_path, "Terrain3DHLODManifest", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DHLODManifest
	var existing_entries: Dictionary[String, Dictionary] = _index_existing_entries(existing)

	var build_signature: int = _signature_int(
		"terrain3d-hlod-v%d|max=%d|reduction=%.8f|region_size=%d|vertex_spacing=%.8f" % [
			BUILDER_VERSION, max_level, reduction_ratio, terrain.region_size, terrain.vertex_spacing])
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
	manifest.format = 1
	manifest.region_map_size = REGION_MAP_SIZE
	manifest.region_size = terrain.region_size
	manifest.vertex_spacing = terrain.vertex_spacing
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
	var manifest_path: String = data_dir.path_join(MANIFEST_NAME)
	var output_dir: String = data_dir.path_join(HLOD_DIR)
	if FileAccess.file_exists(manifest_path):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(manifest_path))
		_update_editor_file(manifest_path)
	_remove_directory_recursive(output_dir)
	terrain.reload_hlod_manifest()
	return {"ok": true}


func _collect_sources(terrain: Terrain3D, max_level: int) -> Array[Source]:
	var sources: Array[Source] = []
	var world_region_size: float = float(terrain.region_size) * terrain.vertex_spacing
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
	# Read the native instance store directly. Exporting the full instance dictionary here
	# duplicates every transform into Variant containers and scales terribly on
	# authored worlds with dense static placement.
	for mesh_id: int in _native_builder.get_instancer_mesh_ids(region):
		var asset := terrain.assets.get_mesh_asset(mesh_id) as Terrain3DMeshAsset
		if not is_instance_valid(asset) or not asset.is_enabled() or not asset.is_hlod_eligible():
			continue
		var built: Dictionary = _native_builder.build_instancer_source(terrain, region, mesh_id)
		if not bool(built.get("ok", false)):
			_warnings.append("Mesh asset %d in region %s excluded from HLOD: %s" % [
				mesh_id, location, String(built.get("error", "unsupported source"))])
			continue
		var mesh: ArrayMesh = built.get("mesh") as ArrayMesh
		if not is_instance_valid(mesh) or mesh.get_surface_count() == 0:
			continue
		var source := Source.new()
		source.owner_region = location
		source.mesh_id = mesh_id
		source.requested_min_level = clampi(asset.hlod_min_level, 1, MAX_LEVEL)
		source.source_key = "instancer:%s:%d" % [location, mesh_id]
		var region_transform := Transform3D(Basis.IDENTITY,
			Vector3(float(location.x) * world_region_size, 0.0, float(location.y) * world_region_size))
		var part := GeometryPart.new()
		part.mesh = mesh
		part.transform = region_transform
		for surface: int in range(mesh.get_surface_count()):
			part.materials.append(mesh.surface_get_material(surface))
		source.parts.append(part)
		source.bounds = region_transform * AABB(built.get("bounds", mesh.get_aabb()))
		source.source_triangles = int(built.get("source_triangles", _mesh_triangle_count(mesh)))
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
		var unsafe_reason: String = _validate_hlod_visual_node(root)
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
	var world_region_size: float = float(terrain.region_size) * terrain.vertex_spacing
	var cluster_origin := Vector3(float(region_min.x) * world_region_size, 0.0,
		float(region_min.y) * world_region_size)
	var signature_parts: PackedStringArray = PackedStringArray(["L%d" % level, str(cluster_key), str(build_signature)])
	for input: Variant in inputs:
		if input is Source:
			var source: Source = input
			signature_parts.append(source.source_key)
			signature_parts.append(str(source.bounds))
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
					loaded.source_signature == source_signature and loaded.build_signature == build_signature:
				var reused := BuiltCluster.new()
				reused.level = level; reused.cluster = cluster_key; reused.region_min = region_min; reused.region_max = region_max
				reused.package_path = old_path; reused.package = loaded; reused.source_signature = source_signature
				return {"ok": true, "cluster": reused, "reused": true}

	var exact_mesh: ArrayMesh = _merge_cluster_geometry(inputs, cluster_origin, world_region_size)
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


func _merge_cluster_geometry(inputs: Array, cluster_origin: Vector3, world_region_size: float) -> ArrayMesh:
	var tools: Dictionary[String, SurfaceTool] = {}
	var materials: Dictionary[String, Material] = {}
	for input: Variant in inputs:
		if input is Source:
			var source: Source = input
			for part: GeometryPart in source.parts:
				_append_part(tools, materials, part.mesh, _cluster_local_transform(part.transform, cluster_origin), part.materials)
		elif input is BuiltCluster:
			var child: BuiltCluster = input
			if not is_instance_valid(child.package) or not is_instance_valid(child.package.mesh):
				continue
			var package_mesh: Mesh = child.package.mesh
			var child_origin := Vector3(float(child.region_min.x) * world_region_size, 0.0,
				float(child.region_min.y) * world_region_size)
			var child_transform := Transform3D(Basis.IDENTITY, child_origin - cluster_origin)
			var child_materials: Array[Material] = []
			for surface: int in range(package_mesh.get_surface_count()):
				child_materials.append(package_mesh.surface_get_material(surface))
			_append_part(tools, materials, package_mesh, child_transform, child_materials)
	var merged := ArrayMesh.new()
	for key: String in tools:
		var tool: SurfaceTool = tools[key]
		if tools.size() > 0:
			tool.index()
		tool.set_material(materials.get(key))
		merged = tool.commit(merged)
	return merged



func _mesh_surface_primitive(mesh: Mesh, surface: int) -> int:
	if mesh is ArrayMesh:
		var array_mesh := mesh as ArrayMesh
		return array_mesh.surface_get_primitive_type(surface)
	# Generic Mesh exposes its primitive query through the virtual surface API.
	return int(mesh.call("_surface_get_primitive_type", surface))


func _append_part(tools: Dictionary[String, SurfaceTool], materials: Dictionary[String, Material],
		mesh: Mesh, transform: Transform3D, effective_materials: Array[Material]) -> void:
	if not is_instance_valid(mesh):
		return
	for surface: int in range(mesh.get_surface_count()):
		if _mesh_surface_primitive(mesh, surface) != Mesh.PRIMITIVE_TRIANGLES:
			continue
		var material: Material = effective_materials[surface] if surface < effective_materials.size() else mesh.surface_get_material(surface)
		var key: String = _resource_key(material)
		if not tools.has(key):
			var tool := SurfaceTool.new()
			tool.begin(Mesh.PRIMITIVE_TRIANGLES)
			tools[key] = tool
			materials[key] = material
		var tool: SurfaceTool = tools[key]
		tool.append_from(mesh, surface, transform)



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


func _validate_hlod_visual_node(node: Node) -> String:
	if node.get_script() != null:
		return "scripted nodes are behavioral"
	if node is CollisionObject3D or node is CollisionShape3D or node is CollisionPolygon3D:
		return "physics/collision nodes must remain detailed"
	if node is AnimationPlayer or node is AnimationTree or node is Skeleton3D:
		return "animated/skeletal content is unsupported"
	if node is AudioStreamPlayer3D or node is GPUParticles3D or node is CPUParticles3D:
		return "audio/particle content is not render-only"
	if node is NavigationRegion3D or node is NavigationLink3D or node is NavigationObstacle3D:
		return "navigation content must remain detailed"
	if node is Light3D:
		return "lights are not HLOD proxy geometry"
	if node is MultiMeshInstance3D:
		return "MultiMesh content must use Terrain3D instancer HLOD"
	if node is MeshInstance3D:
		var mesh_node := node as MeshInstance3D
		if not is_instance_valid(mesh_node.mesh):
			return "MeshInstance3D has no mesh"
		if mesh_node.mesh is ArrayMesh and (mesh_node.mesh as ArrayMesh).get_blend_shape_count() > 0:
			return "blend shapes are unsupported"
		if is_instance_valid(mesh_node.material_overlay):
			return "material overlays are unsupported"
	for child: Node in node.get_children():
		var reason: String = _validate_hlod_visual_node(child)
		if not reason.is_empty():
			return reason
	return ""


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
		if value is Resource:
			parts.append(String(property_name) + "=" + _resource_content_signature(value as Resource))
		else:
			parts.append(String(property_name) + "=" + var_to_str(value))
	return "|".join(parts).md5_text()


func _resource_content_signature(resource: Resource) -> String:
	if not is_instance_valid(resource):
		return "null"
	if not resource.resource_path.is_empty() and FileAccess.file_exists(resource.resource_path):
		return resource.resource_path + ":" + FileAccess.get_md5(resource.resource_path)
	return resource.get_class() + ":" + str(resource.get_instance_id())


func _mesh_asset_materials(asset: Terrain3DMeshAsset, mesh: Mesh) -> Array[Material]:
	var result: Array[Material] = []
	for surface: int in range(mesh.get_surface_count()):
		var material: Material = asset.material_override
		if not is_instance_valid(material):
			material = mesh.surface_get_material(surface)
		result.append(material)
	return result


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
	var staging_path: String = directory.path_join("terrain3d_hlod_manifest.staging.tres")
	var previous_path: String = directory.path_join("terrain3d_hlod_manifest.previous.tres")
	for cleanup_path: String in [staging_path, previous_path]:
		if FileAccess.file_exists(cleanup_path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(cleanup_path))
	var save_error: Error = ResourceSaver.save(manifest, staging_path)
	if save_error != OK:
		return save_error
	var validation := ResourceLoader.load(staging_path, "Terrain3DHLODManifest", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DHLODManifest
	if not is_instance_valid(validation) or validation.entries.size() != manifest.entries.size():
		DirAccess.remove_absolute(ProjectSettings.globalize_path(staging_path))
		return ERR_FILE_CORRUPT
	var absolute_final: String = ProjectSettings.globalize_path(manifest_path)
	var absolute_staging: String = ProjectSettings.globalize_path(staging_path)
	var absolute_previous: String = ProjectSettings.globalize_path(previous_path)
	var had_previous: bool = FileAccess.file_exists(manifest_path)
	if had_previous:
		var backup_error: Error = DirAccess.rename_absolute(absolute_final, absolute_previous)
		if backup_error != OK:
			DirAccess.remove_absolute(absolute_staging)
			return backup_error
	var publish_error: Error = DirAccess.rename_absolute(absolute_staging, absolute_final)
	if publish_error != OK:
		if had_previous:
			DirAccess.rename_absolute(absolute_previous, absolute_final)
		DirAccess.remove_absolute(absolute_staging)
		return publish_error
	if had_previous and FileAccess.file_exists(previous_path):
		DirAccess.remove_absolute(absolute_previous)
	return OK


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
