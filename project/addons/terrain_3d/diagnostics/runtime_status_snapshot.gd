# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Read-only, source-key-exact view of existing Terrain3D diagnostic snapshots.
@tool
extends RefCounted

# These descriptors are deliberately keyed to the native publishers. A missing
# key is never inferred from sibling counters or interpreted as numeric zero.
const AUTHORING_FIELDS = [
	["World visibility prepared frames", "world", "visibility_prepared_frames", "count"],
	["World visibility failures", "world", "visibility_failures", "count"],
	["World visibility candidates", "world", "visibility_candidates", "count"],
	["World HiZ mip levels", "world", "hiz_mips", "count"],
	["VT feedback grid ready", "renderer", "vt_feedback_grid_enabled", "bool"],
	["VT feedback valid samples", "renderer", "vt_feedback_grid_valid_samples", "count"],
	["VT feedback stale captures", "renderer", "vt_feedback_grid_stale", "count"],
	["VT feedback accepted requests", "renderer", "vt_feedback_accepted_requests", "count"],
	["VT requested quality profile", "renderer", "vt_requested_profile", "count"],
	["VT quality generation", "renderer", "vt_quality_generation", "count"],
	["Preview V1 cook phase", "renderer", "vt_editor_cook_stage", "phase"],
	["Preview V1 cook failures", "renderer", "preview_vt_authoring_cook_failures", "count"],
	["Preview V1 last cook error", "renderer", "preview_vt_authoring_last_cook_error", "error"],
	["Preview V1 recooks", "renderer", "preview_vt_authoring_recooks", "count"],
	["Preview V1 publications", "renderer", "preview_vt_authoring_publications", "count"],
]

const CACHE_FIELDS = [
	["V1 package version", "renderer", "vt_pack_version", "count"],
	["V1 source database generation", "renderer", "virtual_texture_database_generation", "count"],
	["V1 package generation", "renderer", "virtual_texture_package_generation", "count"],
	["V1 publication generation", "renderer", "virtual_texture_publication_generation", "count"],
	["V1 content signature", "renderer", "virtual_texture_package_content_signature", "text"],
	["V1 preview package bytes (in memory)", "renderer", "preview_vt_authoring_package_bytes", "bytes"],
	["V1 preview dirty allocations", "renderer", "vt_editor_dirty_allocations", "count"],
	["V1 preview source generation", "renderer", "preview_vt_material_source_generation", "count"],
	["V1 preview published generation", "renderer", "preview_vt_authoring_published_package_generation", "count"],
	["V1 preview pending republish", "renderer", "preview_vt_republish_pending", "bool"],
	["VT physical page capacity", "renderer", "vt_physical_capacity", "count"],
	["VT physical resident pages", "renderer", "vt_physical_resident", "count"],
	["VT IO bytes (cumulative)", "renderer", "vt_io_bytes_total", "bytes"],
	["VT stream uploaded bytes (cumulative)", "renderer", "virtual_texture_stream_uploaded_bytes", "bytes"],
	["VT stream decode failures", "renderer", "virtual_texture_stream_decode_failures", "count"],
	["VT IO pending jobs", "renderer", "vt_io_jobs_pending", "count"],
	["VT quality fallback allocations", "renderer", "vt_quality_fallback_allocations", "count"],
	["VT resident HLOD pages", "renderer", "vt_resident_hlod", "count"],
	["World mesh published generation", "renderer", "published_mesh_generation", "count"],
	["World mesh published vertex bytes", "renderer", "published_mesh_vertex_bytes", "bytes"],
	["World mesh published index bytes", "renderer", "published_mesh_index32_bytes", "bytes"],
	["World GPU buffer bytes (not HLOD disk)", "world", "gpu_buffer_bytes", "bytes"],
]

# Contract requests; intentionally absent from the Merge-1 owner snapshots.
# A publisher may add these exact fields. The UI will then consume them without
# inventing a file scanner, live-array walk, codec estimator, or mutable state.
const AUTHORING_UNPUBLISHED = [
	["Foliage bake candidates", "foliage_authoring.bake_candidates", "Foliage authoring"],
	["Foliage bake rejections", "foliage_authoring.bake_rejections", "Foliage authoring"],
	["Foliage persisted instances", "foliage_authoring.persisted_instances", "Foliage authoring"],
	["Foliage bake phase", "foliage_authoring.bake_phase", "Foliage authoring"],
	["Foliage bake failure", "foliage_authoring.last_bake_error", "Foliage authoring"],
	["VT requested resource mip", "streaming_renderer.vt_requested_resource_mip", "Virtual texture"],
	["VT resident resource mip", "streaming_renderer.vt_resident_resource_mip", "Virtual texture"],
	["Authoring dependency generation", "authoring_dependencies.generation", "WG-B B-03 (pending integration)"],
	["Authoring dependency dirty regions", "authoring_dependencies.dirty_regions", "WG-B B-03 (pending integration)"],
	["Authoring dependency invalidation cause", "authoring_dependencies.invalidation_cause", "WG-B B-03 (pending integration)"],
	["Authoring preview dirty products", "authoring_dependencies.preview_dirty_products", "WG-B B-03 (pending integration)"],
]

const CACHE_UNPUBLISHED = [
	["Regional FAR dirty records", "far_regional.dirty_region_count", "FAR cooker"],
	["Regional FAR stored bytes", "far_regional.stored_bytes", "FAR cooker"],
	["Regional FAR decoded bytes", "far_regional.decoded_bytes", "FAR cooker"],
	["Regional FAR generation", "far_regional.generation", "FAR cooker"],
	["Regional FAR codec", "far_regional.codec", "FAR cooker"],
	["Regional FAR invalidation cause", "far_regional.invalidation_cause", "FAR cooker"],
	["V1 package stored bytes", "streaming_renderer.vt_package_stored_bytes", "V1 package writer"],
	["V1 package dirty sources", "streaming_renderer.vt_package_dirty_sources", "V1 package writer"],
	["V1 package invalidation cause", "streaming_renderer.vt_package_invalidation_cause", "V1 package writer"],
	["V1 package decoded bytes", "streaming_renderer.vt_package_decoded_bytes", "V1 package writer"],
	["V1 package codec", "streaming_renderer.vt_package_codec", "V1 package writer"],
	["Page/stream pack stored bytes", "vt_stream_pack.stored_bytes", "VT stream pack"],
	["Page/stream pack generation", "vt_stream_pack.generation", "VT stream pack"],
	["Page/stream pack dirty count", "vt_stream_pack.dirty_count", "VT stream pack"],
	["Page/stream pack decoded bytes", "vt_stream_pack.decoded_bytes", "VT stream pack"],
	["Page/stream pack invalidation", "vt_stream_pack.invalidation_cause", "VT stream pack"],
	["Page/stream pack codec", "vt_stream_pack.codec", "VT stream pack"],
	["Prediction sidecar generation", "vt_prediction_sidecar.generation", "VT predictor"],
	["Prediction sidecar dirty count", "vt_prediction_sidecar.dirty_count", "VT predictor"],
	["Prediction sidecar decoded bytes", "vt_prediction_sidecar.decoded_bytes", "VT predictor"],
	["Prediction sidecar invalidation", "vt_prediction_sidecar.invalidation_cause", "VT predictor"],
	["Prediction sidecar stored bytes", "vt_prediction_sidecar.stored_bytes", "VT predictor"],
	["Prediction sidecar codec", "vt_prediction_sidecar.codec", "VT predictor"],
	["HLOD package generation", "hlod_authoring.package_generation", "HLOD baker"],
	["HLOD stored bytes", "hlod_authoring.stored_bytes", "HLOD baker"],
	["HLOD dirty sectors", "hlod_authoring.dirty_sectors", "HLOD baker"],
	["HLOD invalidation cause", "hlod_authoring.invalidation_cause", "HLOD baker"],
	["HLOD decoded bytes", "hlod_authoring.decoded_bytes", "HLOD baker"],
	["HLOD codec", "hlod_authoring.codec", "HLOD baker"],
	["Foliage package generation", "foliage_authoring.package_generation", "Foliage baker"],
	["Foliage dirty regions", "foliage_authoring.dirty_regions", "Foliage baker"],
	["Foliage decoded bytes", "foliage_authoring.decoded_bytes", "Foliage baker"],
	["Foliage invalidation cause", "foliage_authoring.invalidation_cause", "Foliage baker"],
	["Foliage persisted bytes", "foliage_authoring.persisted_bytes", "Foliage baker"],
	["Foliage codec", "foliage_authoring.codec", "Foliage baker"],
]

const VT_COOK_PHASES = ["idle", "extract", "filter", "page", "encode", "pack", "complete"]


static func authoring_rows(p_snapshot: Terrain3DRuntimeValueMap) -> Array[Dictionary]:
	var rows: Array[Dictionary] = []
	_append_published(rows, p_snapshot, AUTHORING_FIELDS)
	_append_unpublished(rows, AUTHORING_UNPUBLISHED, p_snapshot)
	return rows


static func cache_rows(p_snapshot: Terrain3DRuntimeValueMap) -> Array[Dictionary]:
	var rows: Array[Dictionary] = []
	_append_published(rows, p_snapshot, CACHE_FIELDS)
	_append_unpublished(rows, CACHE_UNPUBLISHED, p_snapshot)
	return rows


static func capability_rows(p_snapshot: Terrain3DRuntimeValueMap) -> Array[Dictionary]:
	var rows: Array[Dictionary] = []
	var shader: Terrain3DRuntimeValueMap = p_snapshot.map_value("shaders")
	for category: String in ["terrain", "instance", "world", "ocean", "compute"]:
		var compiled: String = _ratio(shader, "spirv_ready_" + category, "catalog_count_" + category)
		var available: String = _ratio(shader, "physical_ready_" + category, "catalog_count_" + category)
		rows.append(_cap_row(category.capitalize() + " catalog", compiled, available, "unavailable", "unavailable", "unavailable", "unavailable", "shaders.spirv_ready_%s / shaders.catalog_count_%s; shaders.physical_ready_%s" % [category, category, category]))
	rows.append(_cap_row("Physical shader library", _ratio(shader, "shader_compile_ready", "shader_compile_expected"),
		_ratio(shader, "physical_shader_ready", "physical_shader_expected"), "unavailable", "unavailable",
		"unavailable", _field(shader, "physical_shader_failed", "count"), "shaders.shader_compile_*; shaders.physical_shader_*"))
	var tiers: Array[Terrain3DRuntimeValueMap] = p_snapshot.map_array("active_render_pipeline")
	for tier: Terrain3DRuntimeValueMap in tiers:
		var tier_name: String = tier.string_value("tier_name", "unknown")
		var shader_loaded_key: String = tier_name.to_lower() + "_loaded"
		var active: String = "unavailable"
		if tier.has("shader_rid") and tier.int_value("shader_rid") != 0:
			active = "RID %d" % tier.int_value("shader_rid")
		rows.append(_cap_row("Tier " + tier_name, _field(tier, "compile_valid", "bool"),
			_field(shader, shader_loaded_key, "bool"), _field(tier, "variant_signature", "text"),
			active, _field(tier, "fallback_to_near", "bool"), "unavailable",
			"active_render_pipeline[%s]; shaders.%s" % [tier_name, shader_loaded_key]))
	rows.append(_cap_row("Terrain source variants/tier", _field(shader, "terrain_shipping_source_variants_per_tier", "count"),
		"unavailable", "unavailable", "unavailable", "unavailable", "unavailable",
		"shaders.terrain_shipping_source_variants_per_tier"))
	rows.append(_cap_row("Viewport VRS", "unavailable", "unavailable", "unavailable", "unavailable",
		"unavailable", "unavailable", "VRS owner capability/profile/effective/fallback snapshot not published"))
	return rows


static func _append_published(p_rows: Array[Dictionary], p_snapshot: Terrain3DRuntimeValueMap, p_fields: Array) -> void:
	var renderer: Terrain3DRuntimeValueMap = p_snapshot.map_value("streaming_renderer")
	var world: Terrain3DRuntimeValueMap = renderer.map_value("world_renderer")
	var frame: Terrain3DRuntimeValueMap = p_snapshot.map_value("frame")
	var lag: int = renderer.int_value("snapshot_generation_lag", -1)
	for descriptor: Array in p_fields:
		var source_name: String = str(descriptor[1])
		var source: Terrain3DRuntimeValueMap = world if source_name == "world" else renderer
		var key: String = str(descriptor[2])
		var path: String = "streaming_renderer.world_renderer." + key if source_name == "world" else "streaming_renderer." + key
		var state: String = "published"
		if not source.has(key) or not frame.has("sequence"):
			state = "unavailable"
		elif lag > 0:
			state = "snapshot lag %d" % lag
		p_rows.append({"name": str(descriptor[0]), "value": _field(source, key, str(descriptor[3])) if state != "unavailable" else "unavailable", "source": path, "status": state})


static func _append_unpublished(p_rows: Array[Dictionary], p_fields: Array, p_snapshot: Terrain3DRuntimeValueMap) -> void:
	for descriptor: Array in p_fields:
		var path: String = str(descriptor[1])
		var dot: int = path.find(".")
		var owner: String = path.substr(0, dot)
		var key: String = path.substr(dot + 1)
		var source: Terrain3DRuntimeValueMap = p_snapshot.map_value(owner)
		if owner == "streaming_renderer":
			source = p_snapshot.map_value("streaming_renderer")
		var is_published: bool = source.has(key) and p_snapshot.map_value("frame").has("sequence")
		var value: String = "unavailable"
		var state: String = "not published by " + str(descriptor[2])
		if is_published:
			value = _field(source, key, "bytes" if key.ends_with("_bytes") else "text")
			state = "published"
			if owner == "streaming_renderer":
				var lag: int = source.int_value("snapshot_generation_lag", -1)
				if lag > 0:
					state = "snapshot lag %d" % lag
		p_rows.append({"name": str(descriptor[0]), "value": value, "source": path, "status": state})


static func _ratio(p_map: Terrain3DRuntimeValueMap, p_value_key: String, p_total_key: String) -> String:
	if not p_map.has(p_value_key) or not p_map.has(p_total_key):
		return "unavailable"
	return "%d / %d" % [p_map.int_value(p_value_key), p_map.int_value(p_total_key)]


static func _field(p_map: Terrain3DRuntimeValueMap, p_key: String, p_kind: String) -> String:
	if not p_map.has(p_key):
		return "unavailable"
	var value: Variant = p_map.raw_value(p_key)
	match p_kind:
		"bool":
			return "yes" if bool(value) else "no"
		"bytes":
			return Terrain3DRuntimeValueMap.format_bytes(float(value))
		"phase":
			var index: int = int(value)
			return VT_COOK_PHASES[index] if index >= 0 and index < VT_COOK_PHASES.size() else "unknown (%d)" % index
		"error":
			return "none" if str(value).is_empty() else str(value)
		_:
			return str(value)


static func _cap_row(p_name: String, p_compiled: String, p_available: String, p_selected: String,
		p_active: String, p_fallback: String, p_failure: String, p_source: String) -> Dictionary:
	return {"name": p_name, "compiled": p_compiled, "available": p_available,
		"selected": p_selected, "active": p_active, "fallback": p_fallback,
		"failure": p_failure, "source": p_source}
