@tool
extends CanvasLayer
class_name Terrain3DP34DebugOverlay

# R12 live visualization reuses the existing runtime/P34 diagnostic snapshots.
# It deliberately does not create an alternate terrain/instance render path.
enum ViewMode {
	VT_RESIDENCY,
	RESIDENCY_TEMPERATURE,
	EVICTION_THRASH,
	REQUEST_SOURCE,
	VT_LEVEL_SELECTION,
	HLOD_LOD,
	COMMAND_OCCUPANCY,
	OCCLUSION,
	RESOURCE_REBUILD,
	UPLOAD_DIRTY_RANGES,
}

@export var terrain_path: NodePath
@export var view_mode: ViewMode = ViewMode.VT_RESIDENCY
@export_range(0.05, 5.0, 0.05) var refresh_seconds := 0.25
@export var visible_in_editor := false

var _terrain: Terrain3D
var _panel: PanelContainer
var _title: Label
var _body: RichTextLabel
var _elapsed := 0.0

func _ready() -> void:
	if Engine.is_editor_hint() and not visible_in_editor:
		return
	_build_ui()
	_resolve_terrain()
	_refresh()

func _process(delta: float) -> void:
	if _body == null:
		return
	_elapsed += delta
	if _elapsed < refresh_seconds:
		return
	_elapsed = 0.0
	if _terrain == null or not is_instance_valid(_terrain):
		_resolve_terrain()
	_refresh()

func _build_ui() -> void:
	_panel = PanelContainer.new()
	_panel.position = Vector2(16.0, 16.0)
	_panel.custom_minimum_size = Vector2(560.0, 300.0)
	add_child(_panel)
	var box := VBoxContainer.new()
	_panel.add_child(box)
	_title = Label.new()
	box.add_child(_title)
	_body = RichTextLabel.new()
	_body.fit_content = true
	_body.scroll_active = false
	_body.bbcode_enabled = true
	_body.custom_minimum_size = Vector2(540.0, 250.0)
	box.add_child(_body)

func _resolve_terrain() -> void:
	_terrain = null
	if not terrain_path.is_empty():
		_terrain = get_node_or_null(terrain_path) as Terrain3D
	if _terrain == null:
		_terrain = _find_terrain(get_tree().current_scene)

func _find_terrain(node: Node) -> Terrain3D:
	if node == null:
		return null
	if node is Terrain3D:
		return node as Terrain3D
	for child in node.get_children():
		var found := _find_terrain(child)
		if found != null:
			return found
	return null

func _value(source: Dictionary, key: StringName, fallback: Variant = 0) -> Variant:
	return source.get(key, fallback)

func _rows(runtime: Dictionary, rd: Dictionary) -> Array[Array]:
	var frame: Dictionary = runtime.get("frame", {})
	var memory: Dictionary = runtime.get("memory", {})
	var inst: Dictionary = rd.get("instances", {})
	match view_mode:
		ViewMode.VT_RESIDENCY:
			return [
				["physical resident", _value(rd, "vt_physical_resident")], ["physical free", _value(rd, "vt_physical_free")],
				["physical retiring", _value(rd, "vt_physical_retiring")], ["capacity", _value(rd, "vt_physical_capacity")],
				["page hits", _value(rd, "vt_page_hits")], ["page misses", _value(rd, "vt_page_misses")],
				["parent fallback", _value(rd, "vt_parent_fallback_hits")], ["tail fallback", _value(rd, "vt_tail_fallback_hits")],
			]
		ViewMode.RESIDENCY_TEMPERATURE:
			return [
				["HOT scans", _value(rd, "virtual_texture_residency_hot_scans")], ["WARM scans", _value(rd, "virtual_texture_residency_warm_scans")],
				["COLD scans", _value(rd, "virtual_texture_residency_cold_scans")], ["terrain resident", _value(rd, "vt_resident_terrain")],
				["clutter resident", _value(rd, "vt_resident_clutter")], ["ground cover resident", _value(rd, "vt_resident_ground_cover")],
				["occluder resident", _value(rd, "vt_resident_occluder")], ["HLOD resident", _value(rd, "vt_resident_hlod")],
			]
		ViewMode.EVICTION_THRASH:
			return [
				["evictions/frame", _value(rd, "vt_evictions_frame")], ["evictions total", _value(rd, "vt_evictions_total")],
				["thrash count", _value(rd, "vt_thrash_count")], ["reservation failures", _value(rd, "virtual_texture_residency_reservation_failures")],
				["retire deferred", _value(rd, "virtual_texture_residency_retire_deferred")], ["thrash window", _value(rd, "virtual_texture_residency_thrash_window_frames")],
			]
		ViewMode.REQUEST_SOURCE:
			return [
				["prediction attempts", _value(rd, "vt_prediction_attempts")], ["fragment faults", _value(rd, "vt_fragment_faults")],
				["accepted feedback", _value(rd, "vt_feedback_accepted_requests")], ["unique requests", _value(rd, "vt_request_unique")],
				["same-key merges", _value(rd, "vt_request_same_key")], ["hash overflow", _value(rd, "vt_request_hash_overflow")],
			]
		ViewMode.VT_LEVEL_SELECTION:
			return [
				["micro VT sparse v3", _value(rd, "terrain_micro_vt_sparse_v3")], ["micro VT generation", _value(rd, "terrain_micro_vt_shared_resource_generation")],
				["MegaVT ready", _value(rd, "terrain_megavt_ready")], ["MegaVT generation", _value(rd, "terrain_megavt_publication_generation")],
				["MegaVT dirty regions", _value(rd, "terrain_megavt_editor_dirty_regions")], ["MegaVT texels/m", _value(rd, "terrain_megavt_texels_per_meter")],
			]
		ViewMode.HLOD_LOD:
			return [
				["instance mesh LODs", _value(inst, "instance_mesh_lods")], ["HLOD cull descriptors", _value(inst, "hlod_gpu_cull_descriptors")],
				["HLOD cull dispatches", _value(inst, "hlod_gpu_cull_dispatches")], ["visible instances", _value(inst, "visible_instances")],
				["render cells", runtime.get("instancer", {}).get("render_cells", 0)], ["clipmap us", frame.get("clipmap_usec", 0)],
			]
		ViewMode.COMMAND_OCCUPANCY:
			return [
				["main commands", _value(inst, "instance_one_submit_main_commands")], ["shadow commands", _value(inst, "instance_one_submit_shadow_commands")],
				["draw packets", _value(inst, "instance_draw_packets")], ["visible indices", _value(inst, "visible_index_count")],
				["main fallback commands", _value(inst, "instance_one_submit_main_fallback_commands")], ["shadow fallback commands", _value(inst, "instance_one_submit_shadow_fallback_commands")],
			]
		ViewMode.OCCLUSION:
			return [
				["Hi-Z enabled", _value(inst, "instance_hiz_occlusion_enabled")], ["Hi-Z ready", _value(inst, "instance_hiz_ready_for_frame")],
				["Hi-Z build dispatches", _value(inst, "instance_hiz_build_dispatches")], ["Hi-Z unavailable frames", _value(inst, "instance_hiz_unavailable_frames")],
				["Hi-Z cell descriptors", _value(inst, "instance_hiz_cell_descriptors")], ["Hi-Z instance descriptors", _value(inst, "instance_hiz_per_instance_descriptors")],
			]
		ViewMode.RESOURCE_REBUILD:
			return [
				["VT descriptor rebuilds", _value(rd, "vt_descriptor_rebuilds_runtime")], ["VT creates after seal", _value(rd, "vt_gpu_resource_creates_after_seal")],
				["instance buffer rebuilds", _value(inst, "owner_buffer_rebuilds")], ["mesh arena rebuilds", _value(inst, "mesh_owner_arena_rebuilds")],
				["shader variant creates", frame.get("shader_variant_creations", 0)], ["shader updates", frame.get("shader_updates", 0)],
			]
		ViewMode.UPLOAD_DIRTY_RANGES:
			return [
				["VT upload bytes/frame", _value(rd, "vt_upload_bytes_frame")], ["VT page sets/frame", _value(rd, "vt_upload_page_sets_frame")],
				["instance dirty bytes", _value(inst, "instance_dirty_upload_bytes")], ["instance dirty ranges", _value(inst, "instance_dirty_upload_ranges")],
				["clipmap patch dirty ranges", rd.get("renderer", {}).get("patch_dirty_ranges", 0)], ["memory resident bytes", memory.get("resident_bytes", 0)],
			]
	return []

func _refresh() -> void:
	if _terrain == null or _title == null or _body == null:
		return
	var runtime: Dictionary = _terrain.get_runtime_diagnostics(false)
	var rd: Dictionary = _terrain.get_rd_renderer_diagnostics()
	_title.text = "P34 R12 — %s" % ViewMode.keys()[int(view_mode)]
	var lines := PackedStringArray()
	for row in _rows(runtime, rd):
		lines.append("[font_size=15][color=#d8d8d8]%-34s[/color]  [color=#ffffff]%s[/color][/font_size]" % [str(row[0]), str(row[1])])
	_body.text = "\n".join(lines)
