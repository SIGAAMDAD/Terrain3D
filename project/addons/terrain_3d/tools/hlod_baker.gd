# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends RefCounted
class_name Terrain3DHLODBaker

const TransactionalResourcePublisher = preload("res://addons/terrain_3d/tools/transactional_resource_publisher.gd")

const HLODVisualValidator = preload("res://addons/terrain_3d/tools/hlod_visual_validator.gd")
const HLODBuildSchedule = preload("res://addons/terrain_3d/tools/hlod_build_schedule.gd")
const HLODBuildCheckpoint = preload("res://addons/terrain_3d/tools/hlod_build_checkpoint.gd")

const NODE_FLAG_HLOD_VISUAL: int = 1 << 2
const MAX_LEVEL: int = 6
const REGION_SIZE: int = 128
const REGION_MAP_SIZE: int = Terrain3DData.REGION_MAP_SIZE
const REGION_MAP_MIN: int = -REGION_MAP_SIZE / 2
const DEFAULT_REDUCTION: float = 0.42
const HLOD_DIR: String = "_hlod"
const MANIFEST_NAME: String = "terrain3d_hlod_manifest.tres"
const BUILDER_VERSION: int = 13
# AUTO is resolved once in Plan Dirty; the baked representation remains 1/2/3.
const AUTO_RECIPE: int = 4
const AUTO_MERGE_TRIANGLE_LIMIT: int = 32000
const AUTO_MERGE_SECTION_LIMIT: int = 16
const AUTO_INSTANCING_MIN_RECORDS: int = 8
const AUTO_MAX_PROTOTYPES: int = 24
const AUTO_MAX_INSTANCES_PER_GROUP: int = 65536

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
	var subregion_slot: int = -1
	var source_signature_token: String = ""
	var source_triangles: int = 0
	var instance_count: int = 1
	var base_geometric_error: float = 0.0
	var deferred_instancer: bool = false
	var deferred_region: Terrain3DRegion
	var direct_instance_transforms: Array[Transform3D] = []
	var force_merged_instancer: bool = false

class BuiltCluster extends RefCounted:
	var level: int = 1
	var cluster: Vector2i = Vector2i.ZERO
	var region_min: Vector2i = Vector2i.ZERO
	var region_max: Vector2i = Vector2i.ZERO
	var package_path: String = ""
	var package: Terrain3DHLODPackage
	var source_signature: int = 0
	var planned_triangles: int = 0
	var planned_sections: int = 0
	var partition_terminal: bool = false

# New bakes use the meshoptimizer recipe. Historical W1-W7 V1 resources
# remain readable but are never emitted by this baker.
var reduction_ratio: float = DEFAULT_REDUCTION
var layer_settings: Terrain3DHLODLayerSettings = Terrain3DHLODLayerSettings.new()
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


# Editor-only setup/validation pass. Computes the exact child-first package
# signatures and dirty/reuse decisions without materializing instancer meshes
# or writing resources. It is also the contract consumed by bake().
func plan_bake(terrain: Terrain3D, dirty_only: bool = true, keep_packages: bool = false) -> Dictionary:
	_warnings.clear()
	_errors.clear()
	if not is_instance_valid(terrain) or not is_instance_valid(terrain.data) or not is_instance_valid(terrain.assets):
		return _failure("Terrain3D data/assets are unavailable")
	if terrain.data_directory.is_empty():
		return _failure("Terrain3D data_directory must be configured before HLOD baking")
	if not is_instance_valid(layer_settings):
		return _failure("HLOD bake recipe is unavailable")
	if layer_settings.cell_size != REGION_SIZE:
		return _failure("Per-layer cell size requires a compatible spatial partition runtime; use 128m")
	var max_level: int = clampi(terrain.hlod_max_level, 1, MAX_LEVEL)
	var sources: Array[Source] = _collect_sources(terrain, max_level)
	var protected_coarse_roots: Dictionary[String, bool] = _collect_coarse_source_roots(sources)
	var first_level: int = -1 if layer_settings.subregion_leaves else (0 if layer_settings.region_leaves else 1)
	if not _errors.is_empty():
		return {"ok": false, "error": "HLOD source validation failed", "errors": _errors.duplicate(),
			"warnings": _warnings.duplicate()}
	if sources.is_empty():
		return _failure("No HLOD-eligible Terrain3D static sources were found")

	var manifest_path: String = _resolve_manifest_path(terrain)
	var existing: Terrain3DHLODManifest = null
	if dirty_only and ResourceLoader.exists(manifest_path):
		existing = ResourceLoader.load(manifest_path, "Terrain3DHLODManifest", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DHLODManifest
	var existing_entries: Dictionary[String, Dictionary] = _index_existing_entries(existing)
	var build_signature: int = _current_build_signature(max_level)
	var previous_clusters: Dictionary[Vector2i, BuiltCluster] = {}
	var jobs: Array[Dictionary] = []
	var reasons: Dictionary[String, int] = {}
	var per_level_build: PackedInt32Array = PackedInt32Array()
	var per_level_reuse: PackedInt32Array = PackedInt32Array()
	per_level_build.resize(MAX_LEVEL + 1)
	per_level_reuse.resize(MAX_LEVEL + 1)
	var known_reused_gpu_bytes: int = 0
	var reusable_packages: Dictionary[String, Terrain3DHLODPackage] = {}
	var split_count: int = 0
	var subcell_build: int = 0
	var subcell_reuse: int = 0

	for level: int in range(first_level, max_level + 1):
		var plans: Dictionary[Vector2i, Array] = _collect_level_plans(level, previous_clusters, sources)
		var next_clusters: Dictionary[Vector2i, BuiltCluster] = {}
		for key: Vector2i in HLODBuildSchedule.sorted_clusters(plans):
			var inputs: Array = plans[key]
			var signature: int = _source_signature_for_inputs(level, key, build_signature, inputs)
			var cost: Dictionary = _estimate_cluster_cost(inputs)
			var selected_recipe: int = _resolve_cluster_recipe(cost)
			var partition_reason: String = _partition_reason(level, key, inputs, cost, protected_coarse_roots)
			var cached: Dictionary = {}
			if partition_reason.is_empty():
				cached = _find_cached_cluster(level, key, signature,
					build_signature, existing_entries, dirty_only, selected_recipe)
			var placeholder := BuiltCluster.new()
			placeholder.level = level
			placeholder.cluster = key
			placeholder.region_min = _level_region_min(level, key)
			placeholder.region_max = _level_region_max(level, key)
			placeholder.source_signature = signature
			placeholder.planned_triangles = int(cost["triangles"])
			placeholder.planned_sections = int(cost["sections"])
			placeholder.partition_terminal = not partition_reason.is_empty()
			next_clusters[key] = placeholder
			var children: PackedStringArray = PackedStringArray()
			for input: Variant in inputs:
				if input is BuiltCluster:
					var child: BuiltCluster = input
					children.append(HLODBuildSchedule.job_id(child.level, child.cluster))
			var reuse: bool = bool(cached.get("reused", false))
			var reason: String = String(cached.get("reason", "unchanged"))
			if not partition_reason.is_empty():
				reason = "split_" + partition_reason
				split_count += 1
				reasons[reason] = int(reasons.get(reason, 0)) + 1
			elif reuse:
				if level < 0:
					subcell_reuse += 1
				else:
					per_level_reuse[level] += 1
				var reusable: Terrain3DHLODPackage = cached.get("package") as Terrain3DHLODPackage
				known_reused_gpu_bytes += reusable.estimated_gpu_bytes
				if keep_packages:
					reusable_packages[HLODBuildSchedule.job_id(level, key)] = reusable
			else:
				if level < 0:
					subcell_build += 1
				else:
					per_level_build[level] += 1
				reasons[reason] = int(reasons.get(reason, 0)) + 1
			jobs.append({
				"id": HLODBuildSchedule.job_id(level, key),
				"level": level, "cluster": key, "dependencies": children,
				"source_signature": signature,
				"action": "split" if not partition_reason.is_empty() else ("reuse" if reuse else "build"),
				"reason": reason,
				"partition_terminal": not partition_reason.is_empty(),
				"resolved_layer": selected_recipe,
				"estimate_triangles": int(cost["triangles"]),
				"estimate_sections": int(cost["sections"]),
				"estimate_batches": int(cost["prototypes"]) if selected_recipe == 1 else int(cost["sections"]),
				"estimate_instances": int(cost["instances"]),
				"package_path": String(cached.get("path", "")) if reuse else "",
			})
		previous_clusters = next_clusters
	var validation: Dictionary = HLODBuildSchedule.validate_jobs(jobs)
	if not bool(validation.get("ok", false)):
		return _failure("HLOD workload rejected: %s" % String(validation.get("error", "invalid dependency order")))
	# World Partition-style resume journal: package results are reusable only
	# for the exact source/dependency graph. Validate each actual package using
	# the existing authoritative package checks before calling it complete.
	var checkpoint_path: String = terrain.data_directory.trim_suffix("/").path_join(HLOD_DIR).path_join(HLODBuildCheckpoint.FILE_NAME)
	var checkpoint_key: String = HLODBuildCheckpoint.plan_key(jobs, build_signature, manifest_path)
	var completed: Dictionary = HLODBuildCheckpoint.load_completed(checkpoint_path, checkpoint_key) if dirty_only else {}
	var validated_completed: Dictionary = {}
	var resumed: int = 0
	for job: Dictionary in jobs:
		var id: String = String(job["id"])
		if not completed.has(id):
			continue
		var checkpoint_entry: Variant = completed[id]
		if not checkpoint_entry is Dictionary:
			continue
		var entry: Dictionary = checkpoint_entry
		var checkpoint_lookup: Dictionary[String, Dictionary] = {}
		checkpoint_lookup[id] = entry
		var cached: Dictionary = _find_cached_cluster(int(job["level"]), job["cluster"],
			int(job["source_signature"]), build_signature, checkpoint_lookup, true,
			int(job["resolved_layer"]))
		if not bool(cached.get("reused", false)):
			continue
		validated_completed[id] = entry
		if String(job["action"]) == "build":
			var level: int = int(job["level"])
			var previous_reason: String = String(job["reason"])
			reasons[previous_reason] = maxi(0, int(reasons.get(previous_reason, 0)) - 1)
			if level < 0:
				subcell_build -= 1
				subcell_reuse += 1
			else:
				per_level_build[level] -= 1
				per_level_reuse[level] += 1
			known_reused_gpu_bytes += int((cached["package"] as Terrain3DHLODPackage).estimated_gpu_bytes)
			job["action"] = "reuse"
			job["reason"] = "resumed_package"
			job["package_path"] = String(cached["path"])
			if keep_packages:
				reusable_packages[id] = cached["package"]
			resumed += 1
	var rebuild_count: int = subcell_build
	var reuse_count: int = subcell_reuse
	for level: int in range(0, max_level + 1):
		rebuild_count += per_level_build[level]
		reuse_count += per_level_reuse[level]
	return {
		"ok": true, "format": HLODBuildSchedule.PLAN_FORMAT,
		"jobs": jobs, "sources": sources.size(), "clusters": jobs.size(),
		"built": rebuild_count, "reused": reuse_count, "split": split_count,
		"per_level_build": per_level_build, "per_level_reuse": per_level_reuse,
		"subcell_build": subcell_build, "subcell_reuse": subcell_reuse,
		"reasons": reasons, "known_reused_gpu_bytes": known_reused_gpu_bytes,
		"resumed": resumed, "checkpoint_path": checkpoint_path,
		"warnings": _warnings.duplicate(), "errors": _errors.duplicate(),
		"manifest_path": manifest_path,
		"_sources": sources, "_existing_manifest": existing,
		"_existing_entries": existing_entries, "_build_signature": build_signature,
		"_reusable_packages": reusable_packages,
		"_checkpoint_key": checkpoint_key, "_checkpoint_completed": validated_completed,
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
		if not is_instance_valid(package) or not (package.is_runtime_cooked_mesh() or package.is_runtime_instance_group()) or \
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
	# Validate the complete dependency graph and dirty decisions before
	# creating directories or publishing individual packages.
	var build_plan: Dictionary = plan_bake(terrain, dirty_only, true)
	if not bool(build_plan.get("ok", false)):
		return build_plan
	var data_dir: String = terrain.data_directory.trim_suffix("/")
	var output_dir: String = data_dir.path_join(HLOD_DIR)
	var dir_error: Error = DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(output_dir))
	if dir_error != OK and dir_error != ERR_ALREADY_EXISTS:
		return _failure("Unable to create HLOD directory: %s" % error_string(dir_error))
	var manifest_path: String = String(build_plan["manifest_path"])
	var manifest_dir_error: Error = DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(manifest_path.get_base_dir()))
	if manifest_dir_error != OK and manifest_dir_error != ERR_ALREADY_EXISTS:
		return _failure("Unable to create HLOD manifest directory: %s" % error_string(manifest_dir_error))

	var max_level: int = clampi(terrain.hlod_max_level, 1, MAX_LEVEL)
	var first_level: int = -1 if layer_settings.subregion_leaves else (0 if layer_settings.region_leaves else 1)
	var sources: Array[Source] = build_plan["_sources"]
	var existing: Terrain3DHLODManifest = build_plan["_existing_manifest"] as Terrain3DHLODManifest
	var existing_entries: Dictionary[String, Dictionary] = build_plan["_existing_entries"]
	var build_signature: int = int(build_plan["_build_signature"])
	var scheduled_jobs: Array[Dictionary] = build_plan["jobs"]
	var reusable_packages: Dictionary[String, Terrain3DHLODPackage] = build_plan["_reusable_packages"]
	var checkpoint_path: String = String(build_plan["checkpoint_path"])
	var checkpoint_key: String = String(build_plan["_checkpoint_key"])
	var checkpoint_completed: Dictionary = build_plan["_checkpoint_completed"]
	var bake_started_usec: int = Time.get_ticks_usec()
	var checkpoint_error: Error = HLODBuildCheckpoint.save_completed(checkpoint_path, checkpoint_key, checkpoint_completed)
	if checkpoint_error != OK:
		return _failure("HLOD recovery journal could not be prepared: %s" % error_string(checkpoint_error))
	var next_job_index: int = 0
	var level_clusters: Dictionary[Vector2i, BuiltCluster] = {}
	var all_clusters: Array[BuiltCluster] = []
	var package_updates: PackedStringArray = PackedStringArray()
	var reused: int = 0
	var built: int = 0

	for level: int in range(first_level, max_level + 1):
		var plans: Dictionary[Vector2i, Array] = _collect_level_plans(level, level_clusters, sources)
		var next_clusters: Dictionary[Vector2i, BuiltCluster] = {}
		for cluster_key: Vector2i in HLODBuildSchedule.sorted_clusters(plans):
			var inputs: Array = plans[cluster_key]
			if next_job_index >= scheduled_jobs.size():
				return _failure("HLOD source graph changed after workload validation")
			var scheduled: Dictionary = scheduled_jobs[next_job_index]
			if String(scheduled["id"]) != HLODBuildSchedule.job_id(level, cluster_key):
				return _failure("HLOD source graph changed after workload validation at L%d %s" % [level, cluster_key])
			next_job_index += 1
			if String(scheduled["action"]) == "split":
				var terminal := BuiltCluster.new()
				terminal.level = level
				terminal.cluster = cluster_key
				terminal.source_signature = int(scheduled["source_signature"])
				terminal.planned_triangles = int(scheduled["estimate_triangles"])
				terminal.planned_sections = int(scheduled["estimate_sections"])
				terminal.partition_terminal = true
				next_clusters[cluster_key] = terminal
				continue
			var result: Dictionary
			if String(scheduled["action"]) == "reuse":
				var cached_package: Terrain3DHLODPackage = reusable_packages.get(String(scheduled["id"]))
				if not is_instance_valid(cached_package):
					return _failure("HLOD validated reuse package disappeared at L%d %s" % [level, cluster_key])
				var reused_cluster := BuiltCluster.new()
				reused_cluster.level = level
				reused_cluster.cluster = cluster_key
				reused_cluster.region_min = _level_region_min(level, cluster_key)
				reused_cluster.region_max = _level_region_max(level, cluster_key)
				reused_cluster.package_path = String(scheduled["package_path"])
				reused_cluster.package = cached_package
				reused_cluster.source_signature = int(scheduled["source_signature"])
				result = {"ok": true, "cluster": reused_cluster, "reused": true}
			else:
				# This job was classified as dirty during preflight. Do not
				# reclassify it half-way through the build.
				result = _build_cluster(terrain, level, cluster_key, inputs, output_dir,
					build_signature, existing_entries, false, int(scheduled["source_signature"]),
					int(scheduled["resolved_layer"]))
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
				checkpoint_completed[String(scheduled["id"])] = {
					"package_path": cluster.package_path,
					"source_signature": str(cluster.source_signature),
					"build_signature": str(build_signature),
				}
				var progress_error: Error = HLODBuildCheckpoint.save_completed(checkpoint_path, checkpoint_key, checkpoint_completed)
				if progress_error != OK:
					return _failure("HLOD recovery journal could not save L%d %s: %s" % [
						level, cluster_key, error_string(progress_error)])
		level_clusters = next_clusters
	if next_job_index != scheduled_jobs.size():
		return _failure("HLOD build omitted one or more validated jobs")

	var manifest := Terrain3DHLODManifest.new()
	manifest.format = 3
	manifest.max_level = max_level
	manifest.build_signature = build_signature
	manifest.built_unix = int(Time.get_unix_time_from_system())
	var entries: Array = []
	var cooked_input_triangles: int = 0
	var cooked_output_triangles: int = 0
	var cooked_geometry_bytes: int = 0
	var cooked_draw_batches: int = 0
	var total_package_disk_bytes: int = 0
	var max_geometric_error: float = 0.0
	var per_layer_stats: Dictionary[int, Dictionary] = {}
	for cluster: BuiltCluster in all_clusters:
		var package: Terrain3DHLODPackage = cluster.package
		cooked_input_triangles += package.source_triangles
		cooked_output_triangles += package.proxy_triangles
		cooked_geometry_bytes += package.estimated_gpu_bytes
		var package_batches: int = package.prototype_meshes.size() if package.format_version == 4 else \
			(1 if package.is_runtime_instance_group() else package.rendering_materials.size())
		cooked_draw_batches += package_batches
		max_geometric_error = maxf(max_geometric_error, package.geometric_error)
		var disk_bytes: int = 0
		var package_file: FileAccess = FileAccess.open(cluster.package_path, FileAccess.READ)
		if is_instance_valid(package_file):
			disk_bytes = package_file.get_length()
			package_file.close()
		total_package_disk_bytes += disk_bytes
		var recipe_id: int = package.layer_type
		var layer_stats: Dictionary = per_layer_stats.get(recipe_id, {
			"packages": 0, "source_triangles": 0, "proxy_triangles": 0,
			"estimated_batches": 0, "estimated_gpu_bytes": 0,
			"package_disk_bytes": 0, "max_geometric_error": 0.0,
		})
		layer_stats["packages"] = int(layer_stats["packages"]) + 1
		layer_stats["source_triangles"] = int(layer_stats["source_triangles"]) + package.source_triangles
		layer_stats["proxy_triangles"] = int(layer_stats["proxy_triangles"]) + package.proxy_triangles
		layer_stats["estimated_batches"] = int(layer_stats["estimated_batches"]) + package_batches
		layer_stats["estimated_gpu_bytes"] = int(layer_stats["estimated_gpu_bytes"]) + package.estimated_gpu_bytes
		layer_stats["package_disk_bytes"] = int(layer_stats["package_disk_bytes"]) + disk_bytes
		layer_stats["max_geometric_error"] = maxf(float(layer_stats["max_geometric_error"]), package.geometric_error)
		per_layer_stats[recipe_id] = layer_stats
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
	# Source signatures include changed instancer transforms and material inputs.
	# If no branch changed, preserve the exact published manifest bytes/time.
	# Compare policy as well: region creation, deletion and source removal can
	# invalidate its detail-suppression table without rebuilding a package.
	if dirty_only and built == 0 and _same_published_manifest(existing, manifest):
		if terrain.hlod_manifest_path != manifest_path:
			terrain.hlod_manifest_path = manifest_path
			terrain.reload_hlod_manifest()
		var clear_error: Error = HLODBuildCheckpoint.clear_completed(checkpoint_path)
		if clear_error != OK:
			push_warning("HLOD completed but recovery journal remains: " + error_string(clear_error))
		return {
			"ok": true, "unchanged": true, "manifest_path": manifest_path,
			"sources": sources.size(), "clusters": all_clusters.size(),
			"built": 0, "reused": reused,
			"source_triangles": cooked_input_triangles, "proxy_triangles": cooked_output_triangles,
			"estimated_gpu_bytes": cooked_geometry_bytes, "estimated_batches": cooked_draw_batches,
			"package_disk_bytes": total_package_disk_bytes,
			"max_geometric_error": max_geometric_error, "per_layer": per_layer_stats,
			"bake_elapsed_ms": (Time.get_ticks_usec() - bake_started_usec) / 1000.0,
			"resumed": int(build_plan.get("resumed", 0)),
			"split": int(build_plan.get("split", 0)),
			"warnings": _warnings.duplicate(), "errors": _errors.duplicate(),
		}
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
	var clear_error: Error = HLODBuildCheckpoint.clear_completed(checkpoint_path)
	if clear_error != OK:
		push_warning("HLOD published but recovery journal remains: " + error_string(clear_error))
	return {
		"ok": true,
		"manifest_path": manifest_path,
		"sources": sources.size(),
		"clusters": all_clusters.size(),
		"built": built,
		"reused": reused,
		"source_triangles": cooked_input_triangles, "proxy_triangles": cooked_output_triangles,
		"estimated_gpu_bytes": cooked_geometry_bytes, "estimated_batches": cooked_draw_batches,
		"package_disk_bytes": total_package_disk_bytes,
		"max_geometric_error": max_geometric_error, "per_layer": per_layer_stats,
		"bake_elapsed_ms": (Time.get_ticks_usec() - bake_started_usec) / 1000.0,
		"resumed": int(build_plan.get("resumed", 0)),
		"split": int(build_plan.get("split", 0)),
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
		var removal: Error = DirAccess.remove_absolute(ProjectSettings.globalize_path(manifest_path))
		if removal != OK:
			return _failure("HLOD manifest removal failed: " + error_string(removal))
		_update_editor_file(manifest_path)
	terrain.reload_hlod_manifest()
	var cleanup: Error = _remove_directory_recursive(output_dir)
	if cleanup != OK:
		return _failure("HLOD package cleanup failed: " + error_string(cleanup))
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


# Cold-only, allocation-bounded cost model. Uses source metadata rather than
# instancer expansion or meshoptimizer to keep Plan Dirty non-writing/cheap.
# The planner freezes the selected layer into each job; all cooked packages
# still carry an explicit INSTANCING/MERGED/SIMPLIFIED layer type.
func _estimate_cluster_cost(inputs: Array) -> Dictionary:
	var triangles: int = 0
	var sections: int = 0
	var instances: int = 0
	var instance_candidates: bool = not inputs.is_empty()
	var prototype_keys: Dictionary[String, bool] = {}
	for value: Variant in inputs:
		if value is BuiltCluster:
			var child: BuiltCluster = value
			triangles += child.package.proxy_triangles if is_instance_valid(child.package) else child.planned_triangles
			sections += child.package.rendering_materials.size() if is_instance_valid(child.package) else child.planned_sections
			instance_candidates = false
		elif value is Source:
			var source: Source = value
			triangles += source.source_triangles
			instances += source.instance_count
			if source.mesh_id >= 0:
				prototype_keys["asset:%d" % source.mesh_id] = true
				sections += 1
			elif source.parts.size() == 1 and is_instance_valid(source.parts[0].mesh):
				var part: GeometryPart = source.parts[0]
				prototype_keys["mesh:%s" % _resource_key(part.mesh)] = true
				sections += part.mesh.get_surface_count()
			else:
				instance_candidates = false
				for part: GeometryPart in source.parts:
					if is_instance_valid(part.mesh):
						sections += part.mesh.get_surface_count()
		else:
			instance_candidates = false
	return {"triangles": triangles, "sections": sections,
		"instances": instances, "prototypes": prototype_keys.size(),
		"instance_candidates": instance_candidates}


func _resolve_cluster_recipe(cost: Dictionary) -> int:
	if layer_settings.layer_type != AUTO_RECIPE:
		return 3 if layer_settings.layer_type == 0 else layer_settings.layer_type
	var triangles: int = int(cost["triangles"])
	var sections: int = int(cost["sections"])
	var instances: int = int(cost["instances"])
	var prototypes: int = int(cost["prototypes"])
	if bool(cost["instance_candidates"]) and instances >= AUTO_INSTANCING_MIN_RECORDS and \
			prototypes > 0 and prototypes <= AUTO_MAX_PROTOTYPES and \
			instances <= AUTO_MAX_INSTANCES_PER_GROUP and prototypes * 3 <= instances:
		return 1
	if triangles <= AUTO_MERGE_TRIANGLE_LIMIT and sections <= AUTO_MERGE_SECTION_LIMIT:
		return 2
	return 3


# Build a compact lookup once, avoiding an O(clusters * sources) check in
# the editor preflight for every candidate parent.
func _collect_coarse_source_roots(sources: Array[Source]) -> Dictionary[String, bool]:
	var roots: Dictionary[String, bool] = {}
	for source: Source in sources:
		if source.min_level > 1:
			var key: Vector2i = _cluster_key_for_region(source.owner_region, 1 << source.min_level)
			roots[HLODBuildSchedule.job_id(source.min_level, key)] = true
	return roots


# An oversized L(n) parent is an optional optimization. The already-cooked
# L(n-1) quadrants remain independent roots if this parent is omitted, so each
# child retains its own bounds, residency, and WORLD GPU publication ACK.
# Terminal status propagates upward; no ancestor may swallow a missing child.
# Skip only when all inputs are real lower-level children. In particular,
# never strand a large source authored with a higher minimum HLOD level.
func _partition_reason(level: int, key: Vector2i, inputs: Array, cost: Dictionary,
		protected_coarse_roots: Dictionary[String, bool]) -> String:
	if level < (0 if layer_settings.subregion_leaves else (1 if layer_settings.region_leaves else 2)) or not layer_settings.spatial_partitioning:
		return ""
	var child_count: int = 0
	var inherited_terminal: bool = false
	for input: Variant in inputs:
		if input is Source:
			return ""
		if not input is BuiltCluster:
			return ""
		var child: BuiltCluster = input
		child_count += 1
		inherited_terminal = inherited_terminal or child.partition_terminal
	if child_count == 0:
		return ""
	# If an authored source appears for the first time at any coarser
	# ancestor of this cell, keep the entire branch available to represent it.
	for future_level: int in range(level + 1, MAX_LEVEL + 1):
		var shift: int = future_level - level
		var ancestor: Vector2i = Vector2i(key.x >> shift, key.y >> shift)
		if protected_coarse_roots.has(HLODBuildSchedule.job_id(future_level, ancestor)):
			return ""
	if inherited_terminal:
		return "inherited_subpartition"
	if child_count < 2:
		return ""
	if int(cost["triangles"]) > layer_settings.partition_triangle_limit:
		return "triangle_budget"
	if int(cost["sections"]) > layer_settings.partition_section_limit:
		return "section_budget"
	# Distant, mostly empty large cells benefit little from aggregating
	# separate occupied islands into a single always-resident proxy.
	if level >= 4 and child_count <= 2 and int(cost["triangles"]) >= 50000:
		return "sparse_coverage"
	return ""


func _collect_level_plans(level: int, children: Dictionary[Vector2i, BuiltCluster],
		sources: Array[Source]) -> Dictionary[Vector2i, Array]:
	var plans: Dictionary[Vector2i, Array] = {}
	if level > (-1 if layer_settings.subregion_leaves else (0 if layer_settings.region_leaves else 1)):
		for child_key: Vector2i in children:
			var child: BuiltCluster = children[child_key]
			var parent_key := Vector2i(child.cluster.x >> 1, child.cluster.y >> 1)
			if not plans.has(parent_key):
				plans[parent_key] = []
			plans[parent_key].append(child)
	for source: Source in sources:
		var source_level: int = source.min_level
		if source.min_level == 1:
			if layer_settings.subregion_leaves:
				source_level = -1 if _source_fits_subregion(source) else 0
			elif layer_settings.region_leaves:
				source_level = 0
		if source_level != level:
			continue
		var key: Vector2i = _subregion_key_for_source(source) if level == -1 else _cluster_key_for_region(source.owner_region, 1 << level)
		if not plans.has(key):
			plans[key] = []
		plans[key].append(source)
	# Resource scans and dictionary insertion order must not alter the cook hash,
	# geometry merge order or which dirty branches are rebuilt.
	for key: Vector2i in plans:
		var inputs: Array = plans[key]
		inputs.sort_custom(func(a: Variant, b: Variant) -> bool:
			return _input_sort_key(a) < _input_sort_key(b))
	return plans


func _input_sort_key(input: Variant) -> String:
	if input is Source:
		var source: Source = input
		return "S|" + source.source_key + "|" + source.source_signature_token
	if input is BuiltCluster:
		var child: BuiltCluster = input
		return "H|" + HLODBuildSchedule.job_id(child.level, child.cluster)
	return "Z"


func _source_signature_for_inputs(level: int, cluster_key: Vector2i,
		build_signature: int, inputs: Array) -> int:
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
	return _signature_int("\n".join(signature_parts))


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
		source.instance_count = maxi(1, int(analyzed.get("instance_count", 1)))
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
			# Only native-supported direct instancing can be partitioned without
			# changing per-instance tints, overlays or unsupported geometry.
			if layer_settings.subregion_leaves and source.min_level == 1 and \
					_split_instancer_subregions(terrain, region, asset, source, output):
				continue
			output.append(source)
		else:
			_warnings.append("Mesh asset %d in region %s spans beyond configured HLOD level %d and remains detailed" % [mesh_id, location, max_level])


func _split_instancer_subregions(terrain: Terrain3D, region: Terrain3DRegion,
		asset: Terrain3DMeshAsset, original: Source, output: Array[Source]) -> bool:
	# Cold-only editor extraction. Prototype mesh/material identity remains shared
	# and each transform is assigned exactly once to a 64m package owner.
	var direct: Dictionary = _native_builder.extract_instancer_transforms(terrain, region, original.mesh_id)
	if not bool(direct.get("ok", false)):
		return false
	var mesh: Mesh = direct.get("mesh") as Mesh
	var transforms: Array = direct.get("transforms", [])
	if not is_instance_valid(mesh) or transforms.is_empty():
		return false
	var batches: Array = [[], [], [], []]
	var bounds: Array[AABB] = [AABB(), AABB(), AABB(), AABB()]
	var region_origin := Vector3(float(original.owner_region.x * REGION_SIZE), 0.0,
		float(original.owner_region.y * REGION_SIZE))
	var primitive_bounds: AABB = mesh.get_aabb()
	for value: Variant in transforms:
		if not value is Transform3D:
			return false
		var transform: Transform3D = value
		var local_bounds: AABB = transform * primitive_bounds
		var center: Vector3 = local_bounds.get_center()
		var slot: int = (1 if center.x >= float(REGION_SIZE) * 0.5 else 0) + \
			(2 if center.z >= float(REGION_SIZE) * 0.5 else 0)
		bounds[slot] = local_bounds if batches[slot].is_empty() else bounds[slot].merge(local_bounds)
		batches[slot].append(transform)
	var pending: Array[Source] = []
	for slot: int in range(4):
		if batches[slot].is_empty():
			continue
		var source := Source.new()
		source.owner_region = original.owner_region
		source.mesh_id = original.mesh_id
		source.requested_min_level = original.requested_min_level
		source.min_level = original.min_level
		source.source_key = original.source_key + ":sub%d" % slot
		source.subregion_slot = slot
		source.source_triangles = roundi(float(original.source_triangles) * float(batches[slot].size()) / float(transforms.size()))
		source.instance_count = batches[slot].size()
		source.bounds = AABB(region_origin + bounds[slot].position, bounds[slot].size)
		source.source_signature_token = (original.source_signature_token + ":quadrant:" + str(slot)).md5_text()
		source.direct_instance_transforms.assign(batches[slot])
		var part := GeometryPart.new()
		part.mesh = mesh
		part.transform = Transform3D(Basis.IDENTITY, region_origin)
		for surface: int in range(mesh.get_surface_count()):
			var material: Material = asset.material_override
			if not is_instance_valid(material):
				material = mesh.surface_get_material(surface)
			part.materials.append(material)
			var canonical: Terrain3DRenderingMaterial = asset.rendering_material_override
			if not is_instance_valid(canonical):
				canonical = _canonical_rendering_material(material)
			part.rendering_materials.append(canonical)
		source.parts.append(part)
		pending.append(source)
	# A single instance with a large extent can cross a quadrant boundary.
	# Keep the original source whole at the 128m parent in that case.
	for entry: Source in pending:
		if not _source_fits_subregion(entry):
			return false
	for source: Source in pending:
		output.append(source)
	return not pending.is_empty()


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
		source.instance_count = 1
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


# Shared preview/build cache decision. The published package must match both
# its dependency fingerprint and the serialized layer recipe, not merely exist.
func _find_cached_cluster(level: int, cluster_key: Vector2i, source_signature: int,
		build_signature: int, existing_entries: Dictionary[String, Dictionary],
		dirty_only: bool, expected_layer: int = -1) -> Dictionary:
	if not dirty_only:
		return {"reused": false, "reason": "rebuild_all"}
	var compatible_layer: int = expected_layer if expected_layer >= 0 else layer_settings.layer_type
	var entry_key: String = HLODBuildSchedule.job_id(level, cluster_key)
	if dirty_only and existing_entries.has(entry_key):
		var old: Dictionary = existing_entries[entry_key]
		if int(old.get("build_signature", -1)) != build_signature:
			return {"reused": false, "reason": "recipe_changed"}
		if int(old.get("source_signature", -1)) != source_signature:
			return {"reused": false, "reason": "source_changed"}
		var old_path: String = String(old.get("package_path", ""))
		if old_path.is_empty() or not FileAccess.file_exists(old_path):
			return {"reused": false, "reason": "package_missing"}
		var loaded := ResourceLoader.load(old_path, "Terrain3DHLODPackage", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DHLODPackage
		var region_min: Vector2i = _level_region_min(level, cluster_key)
		var region_max: Vector2i = _level_region_max(level, cluster_key)
		if is_instance_valid(loaded) and (loaded.is_runtime_cooked_mesh() or loaded.is_runtime_instance_group()) and \
				loaded.level == level and loaded.cluster_location == cluster_key and \
				loaded.region_min == region_min and loaded.region_max == region_max and \
				loaded.source_signature == source_signature and loaded.build_signature == build_signature and \
				(loaded.layer_type == compatible_layer or (compatible_layer == 1 and loaded.layer_type == 2)) and \
				(loaded.has_canonical_rendering_materials() or loaded.is_runtime_instance_group()):
			if loaded.recipe_version == 2 and loaded.recipe_hash == layer_settings.get_recipe_hash():
				return {"reused": true, "reason": "unchanged", "path": old_path, "package": loaded}
		return {"reused": false, "reason": "package_invalid"}
	return {"reused": false, "reason": "new_cluster"}


func _build_cluster(terrain: Terrain3D, level: int, cluster_key: Vector2i, inputs: Array,
		output_dir: String, build_signature: int, existing_entries: Dictionary[String, Dictionary],
		dirty_only: bool, planned_source_signature: int = -1, planned_recipe_type: int = -1) -> Dictionary:
	var region_min: Vector2i = _level_region_min(level, cluster_key)
	var region_max: Vector2i = _level_region_max(level, cluster_key)
	var world_region_size: float = float(REGION_SIZE)
	var cluster_origin := Vector3(float(region_min.x) * world_region_size, 0.0,
		float(region_min.y) * world_region_size)
	# The planner and cooker operate synchronously on the same immutable source
	# snapshot. Reuse its verified digest; hashing every GLB vertex array again
	# at each stage would waste editor CPU time and temporary allocations.
	var source_signature: int = planned_source_signature if planned_source_signature >= 0 else \
		_source_signature_for_inputs(level, cluster_key, build_signature, inputs)
	var cached: Dictionary = _find_cached_cluster(level, cluster_key, source_signature,
		build_signature, existing_entries, dirty_only, planned_recipe_type)
	if bool(cached.get("reused", false)):
		var reused := BuiltCluster.new()
		reused.level = level; reused.cluster = cluster_key; reused.region_min = region_min; reused.region_max = region_max
		reused.package_path = String(cached["path"])
		reused.package = cached["package"] as Terrain3DHLODPackage
		reused.source_signature = source_signature
		return {"ok": true, "cluster": reused, "reused": true}

	# Freeze the policy before deferred instancer extraction: Merged must keep
	# original topology and Instancing must use its direct transform API.
	var resolved_recipe: int = planned_recipe_type if planned_recipe_type >= 0 else \
		_resolve_cluster_recipe(_estimate_cluster_cost(inputs))
	var materialized: Dictionary = _materialize_deferred_instancer_sources(terrain, inputs, resolved_recipe)
	if not bool(materialized.get("ok", false)):
		return materialized
	# Each compatible prototype becomes one immutable WORLD mesh source;
	# all its transforms share a single instance cell and an atomic group ACK.
	var instance_group: Dictionary = _try_build_instance_group(inputs, cluster_origin) if resolved_recipe == 1 else {}
	if resolved_recipe == 1 and not bool(instance_group.get("ok", false)):
		# Reuse the bounded native instancer chunk cooker on fallback. Never
		# explode a dense instancer into one GDScript object per instance.
		for fallback_source: Variant in inputs:
			if not fallback_source is Source:
				continue
			var expanded: Source = fallback_source
			if expanded.direct_instance_transforms.is_empty():
				continue
			expanded.parts.clear()
			expanded.direct_instance_transforms.clear()
			expanded.deferred_instancer = true
			expanded.force_merged_instancer = true
		var rematerialized: Dictionary = _materialize_deferred_instancer_sources(terrain, inputs, 2)
		if not bool(rematerialized.get("ok", false)):
			return rematerialized
	var merged_geometry: Dictionary = instance_group if bool(instance_group.get("ok", false)) else _merge_cluster_geometry(inputs, cluster_origin, world_region_size)
	if not bool(merged_geometry.get("ok", false)):
		return merged_geometry
	var exact_mesh: ArrayMesh = merged_geometry.get("mesh") as ArrayMesh
	var exact_rendering_materials: Array[Terrain3DRenderingMaterial] = merged_geometry.get("rendering_materials", [])
	if not is_instance_valid(exact_mesh) or exact_mesh.get_surface_count() == 0:
		return _failure("HLOD cluster L%d %s produced no geometry" % [level, cluster_key])
	var recipe_type: int = 1 if bool(instance_group.get("ok", false)) else (2 if resolved_recipe == 1 else resolved_recipe)
	# Merged retains topology and all material sections while avoiding the
	# topology-changing SurfaceTool LOD generator. Existing runtime proxy code
	# accepts the same cooked mesh representation.
	var simplified: Dictionary
	if recipe_type == 1:
		var instance_count: int = int(instance_group["count"])
		var triangle_count: int = int(instance_group["source_triangles"])
		simplified = {"ok": true, "mesh": exact_mesh, "geometric_error": 0.0,
			"source_triangles": triangle_count,
			"proxy_triangles": triangle_count}
	elif recipe_type == 2:
		var original_count: int = _mesh_triangle_count(exact_mesh)
		var merged_optimized: Dictionary = _native_builder.optimize_mesh(exact_mesh)
		if not bool(merged_optimized.get("ok", false)):
			return _failure("HLOD cluster L%d %s merge optimizer failed" % [level, cluster_key])
		simplified = {"ok": true, "mesh": merged_optimized.get("mesh", exact_mesh),
			"geometric_error": 0.0, "source_triangles": original_count,
			"proxy_triangles": original_count}
	elif recipe_type == 3:
		# Screen-space recipe at the configured loading distance. The 1080p,
		# 70-degree reference projection is fixed by the cook contract.
		var max_world_error: float = maxf(0.0, layer_settings.target_error_pixels) * \
			maxf(1.0, layer_settings.loading_distance) * (2.0 * tan(deg_to_rad(35.0))) / 1080.0
		# Parent errors must not exceed the global screen-error budget.
		max_world_error = maxf(0.0, max_world_error - _inherited_geometric_error(inputs))
		simplified = _native_builder.simplify_mesh_with_error(exact_mesh,
			layer_settings.reduction_ratio, max_world_error)
	else:
		return _failure("HLOD cluster L%d %s has an unsupported cook recipe %d" % [level, cluster_key, recipe_type])
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
	package.format_version = (4 if int(instance_group.get("prototype_count", 1)) > 1 else 3) if recipe_type == 1 else 2
	package.payload_type = 1 if recipe_type == 1 else 0
	if recipe_type == 1:
		package.instance_group_payload = instance_group["payload"]
		if package.format_version == 4:
			package.prototype_meshes = instance_group["prototype_meshes"]
	package.layer_type = recipe_type
	package.recipe_version = 2
	package.recipe_hash = layer_settings.get_recipe_hash()
	package.rendering_materials = exact_rendering_materials
	if not (package.has_canonical_rendering_materials() or package.is_runtime_instance_group()):
		return _failure("HLOD cluster L%d %s produced an invalid canonical Terrain3DRenderingMaterial table" % [level, cluster_key])
	package.local_bounds = instance_group["bounds"] if recipe_type == 1 else proxy_mesh.get_aabb()
	package.geometric_error = _inherited_geometric_error(inputs) + float(simplified.get("geometric_error", 0.0))
	package.source_signature = source_signature
	package.build_signature = build_signature
	package.source_triangles = int(simplified.get("source_triangles", _mesh_triangle_count(exact_mesh)))
	package.proxy_triangles = int(simplified.get("proxy_triangles", _mesh_triangle_count(proxy_mesh)))
	package.estimated_gpu_bytes = int(instance_group["gpu_bytes"]) if recipe_type == 1 else _estimate_mesh_bytes(proxy_mesh)
	package.built_unix = int(Time.get_unix_time_from_system())
	var level_dir: String = output_dir.path_join("L%d" % level)
	var dir_error: Error = DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(level_dir))
	if dir_error != OK and dir_error != ERR_ALREADY_EXISTS:
		return _failure("Unable to create HLOD level directory: %s" % error_string(dir_error))
	var content_signature: int = _signature_int("%d|%d" % [source_signature, build_signature])
	var signature_hex: String = "%016x" % (content_signature & 0x7fffffffffffffff)
	var package_path: String = level_dir.path_join("L%d_%d_%d_%s.res" % [level, cluster_key.x, cluster_key.y, signature_hex])
	var staging_path: String = level_dir.path_join("L%d_%d_%d_%s.staging.res" % [level, cluster_key.x, cluster_key.y, signature_hex])
	var previous_path: String = level_dir.path_join("L%d_%d_%d_%s.previous.res" % [level, cluster_key.x, cluster_key.y, signature_hex])
	var error: Error = TransactionalResourcePublisher.publish(
		package, package_path, staging_path, previous_path, "Terrain3DHLODPackage",
		func(resource: Resource) -> bool:
			var validation := resource as Terrain3DHLODPackage
			return is_instance_valid(validation) and (validation.is_runtime_cooked_mesh() or validation.is_runtime_instance_group()) and \
				validation.level == level and validation.cluster_location == cluster_key and \
				validation.source_signature == source_signature and validation.build_signature == build_signature and \
				(validation.has_canonical_rendering_materials() or validation.is_runtime_instance_group()))
	if error != OK:
		return _failure("Unable to publish HLOD package %s: %s" % [package_path, error_string(error)])
	var built := BuiltCluster.new()
	built.level = level; built.cluster = cluster_key; built.region_min = region_min; built.region_max = region_max
	built.package_path = package_path; built.package = package; built.source_signature = source_signature
	return {"ok": true, "cluster": built, "reused": false}


func _materialize_deferred_instancer_sources(terrain: Terrain3D, inputs: Array, resolved_recipe: int) -> Dictionary:
	var world_error: float = maxf(0.0, layer_settings.target_error_pixels) * \
		maxf(1.0, layer_settings.loading_distance) * (2.0 * tan(deg_to_rad(35.0))) / 1080.0
	var pre_error_budget: float = 0.5 * maxf(0.0, world_error - _inherited_geometric_error(inputs)) if resolved_recipe == 3 else 0.0
	for input: Variant in inputs:
		if not input is Source:
			continue
		var source: Source = input
		if not source.deferred_instancer or not source.parts.is_empty():
			continue
		if not is_instance_valid(source.deferred_region):
			return _failure("HLOD instancer source region became unavailable during bake")
		if resolved_recipe == 1 and not source.force_merged_instancer:
			var direct: Dictionary = _native_builder.extract_instancer_transforms(
				terrain, source.deferred_region, source.mesh_id)
			if bool(direct.get("ok", false)):
				var source_asset := terrain.assets.get_mesh_asset(source.mesh_id) as Terrain3DMeshAsset
				var direct_mesh: Mesh = direct.get("mesh") as Mesh
				if is_instance_valid(source_asset) and is_instance_valid(direct_mesh):
					var direct_part := GeometryPart.new()
					direct_part.mesh = direct_mesh
					direct_part.transform = Transform3D(Basis.IDENTITY, Vector3(
						float(source.owner_region.x) * float(REGION_SIZE), 0.0,
						float(source.owner_region.y) * float(REGION_SIZE)))
					for surface: int in range(direct_mesh.get_surface_count()):
						var direct_material: Material = source_asset.material_override
						if not is_instance_valid(direct_material):
							direct_material = direct_mesh.surface_get_material(surface)
						direct_part.materials.append(direct_material)
						var direct_rendering_material: Terrain3DRenderingMaterial = source_asset.rendering_material_override
						if not is_instance_valid(direct_rendering_material):
							direct_rendering_material = _canonical_rendering_material(direct_material)
						direct_part.rendering_materials.append(direct_rendering_material)
					source.parts.append(direct_part)
					source.direct_instance_transforms.assign(direct.get("transforms", []))
					source.deferred_instancer = false
					continue
		var built: Dictionary = _native_builder.build_instancer_source(terrain, source.deferred_region,
			source.mesh_id, resolved_recipe == 2 or resolved_recipe == 1, pre_error_budget)
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


# Instancing cook: preserve compatible materials per prototype. One geometry
# source and one cell are published for each distinct prototype; transform
# records are deterministic and cell-local. Unsupported affine transforms or
# instancer-specific source layouts select the exact Merged fallback instead.
func _try_build_instance_group(inputs: Array, cluster_origin: Vector3) -> Dictionary:
	if inputs.is_empty():
		return {}
	var identities: Dictionary[String, int] = {}
	var prototype_parts: Array[GeometryPart] = []
	var prototype_keys: Array[String] = []
	var prototype_indices := PackedInt32Array()
	var transforms: Array[Transform3D] = []
	var spatial_cell_keys: Dictionary[Vector3i, bool] = {}
	var group_bounds := AABB()
	for input: Variant in inputs:
		if not input is Source:
			return {}
		var source: Source = input
		if source.deferred_instancer or source.parts.size() != 1:
			return {}
		var part: GeometryPart = source.parts[0]
		if not is_instance_valid(part.mesh) or part.mesh.get_surface_count() == 0:
			return {}
		var tokens := PackedStringArray([_resource_content_signature(part.mesh)])
		for material: Material in part.materials:
			tokens.append(_resource_content_signature(material))
		for material: Terrain3DRenderingMaterial in part.rendering_materials:
			tokens.append(_resource_content_signature(material))
		var identity: String = "|".join(tokens)
		var prototype_index: int = -1
		if identities.has(identity):
			prototype_index = identities[identity]
		else:
			prototype_index = prototype_parts.size()
			identities[identity] = prototype_index
			prototype_parts.append(part)
			prototype_keys.append(identity)
		var source_transforms: Array[Transform3D] = source.direct_instance_transforms
		if source_transforms.is_empty():
			source_transforms = [Transform3D.IDENTITY]
		for instance_transform: Transform3D in source_transforms:
			if transforms.size() >= AUTO_MAX_INSTANCES_PER_GROUP:
				return {}
			var local: Transform3D = _cluster_local_transform(
				part.transform * instance_transform, cluster_origin)
			var basis: Basis = local.basis
			if basis.determinant() <= 0.0 or basis.x.length_squared() < 0.000001 or \
					basis.y.length_squared() < 0.000001 or basis.z.length_squared() < 0.000001:
				return {}
			if absf(basis.x.normalized().dot(basis.y.normalized())) > 0.0001 or \
					absf(basis.y.normalized().dot(basis.z.normalized())) > 0.0001 or \
					absf(basis.z.normalized().dot(basis.x.normalized())) > 0.0001:
				return {}
			transforms.append(local)
			prototype_indices.append(prototype_index)
			var bucket_metres: float = float(REGION_SIZE) * 0.5
			var spatial_key := Vector3i(prototype_index,
				floori(local.origin.x / bucket_metres), floori(local.origin.z / bucket_metres))
			spatial_cell_keys[spatial_key] = true
			var bounds: AABB = local * part.mesh.get_aabb()
			group_bounds = bounds if transforms.size() == 1 else group_bounds.merge(bounds)
	if prototype_parts.is_empty() or transforms.size() < 2:
		return {}
	# Avoid manufacturing one draw source per unique prop; those groups belong
	# in the topology-preserving Merged recipe.
	if prototype_parts.size() == transforms.size():
		return {}
	var prototype_meshes: Array[ArrayMesh] = []
	var rendering_materials: Array[Terrain3DRenderingMaterial] = []
	var signatures: Array[int] = []
	var gpu_bytes: int = 0
	var triangle_counts := PackedInt32Array()
	for i: int in range(prototype_parts.size()):
		var prototype := Source.new()
		var prototype_part := GeometryPart.new()
		prototype_part.mesh = prototype_parts[i].mesh
		prototype_part.materials = prototype_parts[i].materials
		prototype_part.rendering_materials = prototype_parts[i].rendering_materials
		prototype.parts.append(prototype_part)
		var exact: Dictionary = _merge_cluster_geometry([prototype], Vector3.ZERO, float(REGION_SIZE))
		if not bool(exact.get("ok", false)):
			return {}
		var mesh: ArrayMesh = exact["mesh"]
		if not is_instance_valid(mesh) or mesh.get_surface_count() == 0:
			return {}
		var ordered: Dictionary = _native_builder.optimize_mesh(mesh)
		if not bool(ordered.get("ok", false)):
			return {}
		mesh = ordered.get("mesh", mesh) as ArrayMesh
		if not is_instance_valid(mesh) or mesh.get_surface_count() != exact["rendering_materials"].size():
			return {}
		prototype_meshes.append(mesh)
		for material: Terrain3DRenderingMaterial in exact["rendering_materials"]:
			rendering_materials.append(material)
		signatures.append(_signature_int(prototype_keys[i]))
		triangle_counts.append(_mesh_triangle_count(mesh))
		gpu_bytes += _estimate_mesh_bytes(mesh)
	var payload: PackedByteArray = _native_builder.encode_multi_prototype_group(
		prototype_meshes, transforms, prototype_indices, signatures)
	if payload.is_empty():
		return {}
	var total_triangles: int = 0
	for index: int in prototype_indices:
		total_triangles += triangle_counts[index]
	return {"ok": true, "mesh": prototype_meshes[0],
		"prototype_meshes": prototype_meshes,
		"rendering_materials": rendering_materials,
		"payload": payload, "bounds": group_bounds.grow(0.01),
		"count": transforms.size(), "prototype_count": prototype_meshes.size(),
		"spatial_cell_count": spatial_cell_keys.size(),
		"source_triangles": total_triangles,
		# WORLD cells and culling descriptors are budgeted per 64m cell,
		# rather than incorrectly assuming one cell for each prototype.
		"gpu_bytes": gpu_bytes + transforms.size() * 32 + prototype_meshes.size() * 128 + spatial_cell_keys.size() * 192}


func _merge_cluster_geometry(inputs: Array, cluster_origin: Vector3, world_region_size: float) -> Dictionary:
	var tools: Dictionary[String, SurfaceTool] = {}
	var materials: Dictionary[String, Material] = {}
	var rendering_materials: Dictionary[String, Terrain3DRenderingMaterial] = {}
	for input: Variant in inputs:
		if input is Source:
			var source: Source = input
			for part: GeometryPart in source.parts:
				var direct_transforms: Array[Transform3D] = source.direct_instance_transforms
				if direct_transforms.is_empty():
					direct_transforms = [Transform3D.IDENTITY]
				for transform: Transform3D in direct_transforms:
					if not _append_part(tools, materials, rendering_materials, part.mesh,
							_cluster_local_transform(part.transform * transform, cluster_origin), part.materials, part.rendering_materials):
						return _failure("HLOD source %s contains a material that cannot be converted to Terrain3DRenderingMaterial" % source.source_key)
		elif input is BuiltCluster:
			var child: BuiltCluster = input
			if not is_instance_valid(child.package) or not is_instance_valid(child.package.mesh):
				continue
			if not (child.package.has_canonical_rendering_materials() or child.package.is_runtime_instance_group()):
				return _failure("HLOD child package L%d %s is missing canonical rendering materials" % [child.level, child.cluster])
			var child_origin := Vector3(float(child.region_min.x) * world_region_size, 0.0,
				float(child.region_min.y) * world_region_size)
			var child_transform := Transform3D(Basis.IDENTITY, child_origin - cluster_origin)
			var child_transforms: Array = [Transform3D.IDENTITY]
			var child_prototypes := PackedInt32Array([0])
			if child.package.is_runtime_instance_group():
				var decoded: Dictionary = _native_builder.decode_multi_prototype_group(child.package.instance_group_payload)
				if decoded.is_empty():
					return _failure("HLOD child group has invalid transform records")
				child_transforms = decoded["transforms"]
				child_prototypes = decoded["prototypes"]
			var group_meshes: Array[ArrayMesh] = [child.package.mesh]
			if child.package.format_version == 4:
				group_meshes = []
				for mesh: ArrayMesh in child.package.prototype_meshes:
					group_meshes.append(mesh)
			var first_materials := PackedInt32Array()
			var flattened_offset: int = 0
			for mesh: ArrayMesh in group_meshes:
				first_materials.append(flattened_offset)
				flattened_offset += mesh.get_surface_count()
			for g: int in range(child_transforms.size()):
				var prototype_index: int = child_prototypes[g]
				if prototype_index < 0 or prototype_index >= group_meshes.size():
					return _failure("HLOD child references an unavailable prototype")
				var mesh: ArrayMesh = group_meshes[prototype_index]
				var child_materials: Array[Material] = []
				var child_rendering_materials: Array[Terrain3DRenderingMaterial] = []
				var first: int = first_materials[prototype_index]
				for surface: int in range(mesh.get_surface_count()):
					child_materials.append(mesh.surface_get_material(surface))
					child_rendering_materials.append(child.package.rendering_materials[first + surface])
				if not _append_part(tools, materials, rendering_materials, mesh,
						child_transform * child_transforms[g], child_materials, child_rendering_materials):
					return _failure("HLOD child package L%d %s has invalid canonical rendering materials" % [child.level, child.cluster])
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



func _same_published_manifest(previous: Terrain3DHLODManifest, planned: Terrain3DHLODManifest) -> bool:
	if not is_instance_valid(previous) or not previous.validate_runtime_blob():
		return false
	if previous.format != planned.format or previous.max_level != planned.max_level or \
			previous.build_signature != planned.build_signature or previous.entries != planned.entries:
		return false
	return previous.instancer_min_levels == planned.instancer_min_levels and \
		previous.instancer_subcell_masks == planned.instancer_subcell_masks and \
		previous.node_subcell_masks == planned.node_subcell_masks and \
		previous.node_region_indices == planned.node_region_indices and \
		previous.node_spawn_ids == planned.node_spawn_ids and \
		previous.node_min_levels == planned.node_min_levels


func _populate_source_policy(manifest: Terrain3DHLODManifest, sources: Array[Source]) -> void:
    var mesh_policy := PackedByteArray()
    mesh_policy.resize(4096 * Terrain3DAssets.MAX_MESH_ASSETS)
    var node_regions := PackedInt32Array()
    var node_ids := PackedInt64Array()
    var node_levels := PackedByteArray()
    var node_masks := PackedByteArray()
    var mesh_masks := PackedByteArray()
    # A V5 ownership bit is granted only for a package that was actually
    # published, with the ENTIRE original source inside that quadrant.
    var published_subcells: Dictionary[String, bool] = {}
    if layer_settings.subregion_leaves:
        mesh_masks.resize(mesh_policy.size())
        for value: Variant in manifest.entries:
            if value is Dictionary and int(value.get("level", 99)) == -1:
                var cluster: Vector2i = value.get("cluster", Vector2i.ZERO)
                published_subcells["%d:%d" % [cluster.x, cluster.y]] = true
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
            node_masks.append(0)
    for source: Source in sources:
        if not layer_settings.subregion_leaves or source.min_level != 1 or \
                not _source_fits_subregion(source):
            continue
        var cluster: Vector2i = _subregion_key_for_source(source)
        if not published_subcells.has("%d:%d" % [cluster.x, cluster.y]):
            continue
        var bit: int = 1 << ((cluster.y & 1) * 2 + (cluster.x & 1))
        var region_index: int = _region_map_index(source.owner_region)
        if region_index < 0:
            continue
        if source.mesh_id >= 0:
            var offset: int = region_index * Terrain3DAssets.MAX_MESH_ASSETS + source.mesh_id
            if mesh_policy[offset] == 1:
                mesh_masks[offset] = mesh_masks[offset] | bit
        elif source.spawn_id > 0:
            for i: int in range(node_ids.size()):
                if node_ids[i] == source.spawn_id and node_regions[i] == region_index and node_levels[i] == 1:
                    node_masks[i] = node_masks[i] | bit
                    break
    # A region-level instancer mesh ID may also have larger objects which
    # cross a quadrant boundary. Those cells cannot be suppressed as one
    # record group. Revoke the affected quadrant ownership conservatively.
    for source: Source in sources:
        if source.mesh_id < 0 or _source_fits_subregion(source):
            continue
        var index: int = _region_map_index(source.owner_region)
        if index < 0:
            continue
        var offset: int = index * Terrain3DAssets.MAX_MESH_ASSETS + source.mesh_id
        if mesh_masks.is_empty() or mesh_masks[offset] == 0:
            continue
        var origin := Vector2(float(source.owner_region.x * REGION_SIZE),
            float(source.owner_region.y * REGION_SIZE))
        for slot: int in range(4):
            var lower := origin + Vector2(float(slot & 1) * 64.0, float(slot >> 1) * 64.0)
            var upper := lower + Vector2(64.0, 64.0)
            if source.bounds.position.x < upper.x and source.bounds.end.x > lower.x and \
                    source.bounds.position.z < upper.y and source.bounds.end.z > lower.y:
                mesh_masks[offset] = mesh_masks[offset] & ~(1 << slot)
    manifest.instancer_min_levels = mesh_policy
    manifest.instancer_subcell_masks = mesh_masks
    manifest.node_region_indices = node_regions
    manifest.node_spawn_ids = node_ids
    manifest.node_min_levels = node_levels
    manifest.node_subcell_masks = node_masks


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
	var meshopt_recipe: String = "|meshopt=v1.3|error=absolute|projection=1080p70v" if layer_settings.layer_type == 3 else ("|meshopt=v1.3|merge-order" if layer_settings.layer_type == 2 else ("|instances=v4|multi-prototype" if layer_settings.layer_type == 1 else ("|auto=v1|meshopt=v1.3" if layer_settings.layer_type == AUTO_RECIPE else "")))
	return _signature_int(("terrain3d-hlod-recipe-v5|builder=%d|max=%d|hash=%d|geometry=128x1m" % [
		BUILDER_VERSION, max_level, layer_settings.get_recipe_hash()]) + meshopt_recipe)


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


func _source_fits_subregion(source: Source) -> bool:
	# A subcell owns only geometry entirely inside its 64m XZ footprint.
	# Point-based assignment of wide geometry would produce incorrect culling.
	var key: Vector2i = _subregion_key_for_source(source)
	var region_origin := Vector2(float(source.owner_region.x * REGION_SIZE),
		float(source.owner_region.y * REGION_SIZE))
	var cell_min := region_origin + Vector2(float(key.x & 1) * 64.0,
		float(key.y & 1) * 64.0)
	var cell_max := cell_min + Vector2(64.0, 64.0)
	var lower: Vector3 = source.bounds.position
	var upper: Vector3 = source.bounds.end
	const BOUND_EPSILON: float = 0.01
	return lower.x >= cell_min.x - BOUND_EPSILON and lower.z >= cell_min.y - BOUND_EPSILON and \
		upper.x <= cell_max.x + BOUND_EPSILON and upper.z <= cell_max.y + BOUND_EPSILON


func _subregion_key_for_source(source: Source) -> Vector2i:
	# Assign the entire eligible source to one 64m owner. Preserve its full
	# geometry bounds if it crosses the quadrant boundary (no mesh clipping).
	var local_x: float = source.bounds.get_center().x - float(source.owner_region.x * REGION_SIZE)
	var local_z: float = source.bounds.get_center().z - float(source.owner_region.y * REGION_SIZE)
	var region_cell: Vector2i = _cluster_key_for_region(source.owner_region, 1)
	if source.subregion_slot >= 0:
		return region_cell * 2 + Vector2i(source.subregion_slot & 1, (source.subregion_slot >> 1) & 1)
	return region_cell * 2 + Vector2i(1 if local_x >= 64.0 else 0, 1 if local_z >= 64.0 else 0)


func _level_region_min(level: int, cluster: Vector2i) -> Vector2i:
	return _cluster_region_min(Vector2i(cluster.x >> 1, cluster.y >> 1), 1) if level == -1 else _cluster_region_min(cluster, 1 << level)


func _level_region_max(level: int, cluster: Vector2i) -> Vector2i:
	var first: Vector2i = _level_region_min(level, cluster)
	return first + Vector2i(maxi(1, 1 << maxi(level, 0)) - 1, maxi(1, 1 << maxi(level, 0)) - 1)


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
					if DirAccess.remove_absolute(ProjectSettings.globalize_path(child)) != OK:
						empty = false
						push_warning("HLOD unused directory could not be removed: " + child)
				else:
					empty = false
			elif name.get_extension().to_lower() in ["res", "tres"] and not referenced.has(child):
				if DirAccess.remove_absolute(ProjectSettings.globalize_path(child)) == OK:
					_update_editor_file(child)
				else:
					empty = false
					push_warning("HLOD unused package could not be removed: " + child)
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


func _remove_directory_recursive(path: String) -> Error:
	var absolute: String = ProjectSettings.globalize_path(path)
	if not DirAccess.dir_exists_absolute(absolute):
		return ERR_FILE_BAD_PATH if FileAccess.file_exists(absolute) else OK
	var dir := DirAccess.open(absolute)
	if dir == null:
		return DirAccess.get_open_error()
	var result: Error = dir.list_dir_begin()
	if result != OK:
		return result
	var name: String = dir.get_next()
	while not name.is_empty():
		if name != "." and name != "..":
			var child: String = absolute.path_join(name)
			if dir.current_is_dir():
				result = _remove_directory_recursive(ProjectSettings.localize_path(child))
			else:
				result = DirAccess.remove_absolute(child)
				if result == OK:
					_update_editor_file(ProjectSettings.localize_path(child))
			if result != OK:
				dir.list_dir_end()
				return result
		name = dir.get_next()
	dir.list_dir_end()
	return DirAccess.remove_absolute(absolute)


func _failure(message: String) -> Dictionary:
	return {"ok": false, "error": message, "warnings": _warnings.duplicate(), "errors": _errors.duplicate()}
