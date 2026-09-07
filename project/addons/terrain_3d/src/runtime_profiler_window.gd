# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends Terrain3DRuntimeProfilerWindowBase

const SettingMetadata = preload("res://addons/terrain_3d/src/setting_metadata.gd")
const SettingControlFactory = preload("res://addons/terrain_3d/src/setting_control_factory.gd")

const CONTROL_GROUP_PREFIXES = [
	"Performance Profile",
	"Terrain Rendering / Shader Tier Ranges",
	"Streaming & Residency",
	"Runtime Scheduling - Advanced",
]

const HistoryGraph = preload("res://addons/terrain_3d/src/runtime_profiler_history_graph.gd")
const ResidencyGrid = preload("res://addons/terrain_3d/src/runtime_profiler_residency_grid.gd")
const RuntimeOverlay = preload("res://addons/terrain_3d/src/runtime_profiler_overlay.gd")

const HISTORY_SAMPLES: int = 600
const DEFAULT_REFRESH_HZ: float = 4.0

var terrain: Terrain3D
var _status: Label
var _pause: CheckButton
var _refresh_rate: OptionButton
var _tabs: TabContainer
var _metrics_tree: Tree
var _problems_tree: Tree
var _pressure_tree: Tree
var _efficiency_tree: Tree
var _diagnostics_memory_tree: Tree
var _diagnostics_pipeline_tree: Tree
var _diagnostics_cache_tree: Tree
var _diagnostics_cpu_tree: Tree
var _diagnostics_gpu_tree: Tree
var _diagnostics_rd_tree: Tree
var _deep_introspection: CheckButton
var _subsystem_tree: Tree
var _history_graph: HistoryGraph
var _history_tree: Tree
var _history_detail: Tree
var _residency_grid: ResidencyGrid
var _residency_detail: RichTextLabel
var _priority_summary: Label
var _priority_tree: Tree
var _raw_tree: Tree
var _validation: RichTextLabel
var _range_summary: Label
var _adaptive_summary: Label
var _adaptive_tree: Tree
var _phasing_summary: Label
var _profile_diff_tree: Tree
var _export_dialog: FileDialog
var _overlay: RuntimeOverlay
var _overlay_button: Button
var _metric_items: Dictionary[String, TreeItem] = {}
var _problem_items: Dictionary[String, TreeItem] = {}
var _pressure_items: Dictionary[String, TreeItem] = {}
var _efficiency_items: Dictionary[String, TreeItem] = {}
var _adaptive_items: Dictionary[String, TreeItem] = {}
var _priority_items: Dictionary[int, TreeItem] = {}
var _subsystem_items: Dictionary[String, TreeItem] = {}
var _history_items: Dictionary[int, TreeItem] = {}
var _raw_items: Dictionary[String, TreeItem] = {}
var _control_bindings: Dictionary[String, Control] = {}
var _control_descriptors: Dictionary[String, Dictionary] = {}
var _refresh_accumulator: float = 0.0
var _refresh_interval_sec: float = 1.0 / DEFAULT_REFRESH_HZ
var _last_snapshot: Terrain3DRuntimeValueMap = Terrain3DRuntimeValueMap.new()
var _history: Array[Terrain3DRuntimeValueMap] = []
var _grid_snapshot: Terrain3DRuntimeValueMap = Terrain3DRuntimeValueMap.new()
var _restore_profiling_on_close: bool = false
var _updating_controls: bool = false
var _last_history_sequence: int = -1
var _history_fetch_sequence: int = -1
var _selected_history_index: int = -1
var _auto_freeze: CheckButton
var _freeze_threshold: SpinBox
var _freeze_on_missing: CheckButton
var _freeze_on_failure: CheckButton
var _freeze_on_shader: CheckButton
var _post_trigger_samples: SpinBox
var _freeze_trigger_sequence: int = -1
var _freeze_target_sequence: int = -1


func _ready() -> void:
	title = "Terrain3D Runtime Diagnostics"
	initial_position = Window.WINDOW_INITIAL_POSITION_CENTER_PRIMARY_SCREEN
	min_size = Vector2i(1080, 700)
	size = Vector2i(1360, 860)
	close_requested.connect(_close_profiler)

	var root: VBoxContainer = VBoxContainer.new()
	root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	root.add_theme_constant_override("separation", 6)
	add_child(root)
	_build_toolbar(root)

	_tabs = TabContainer.new()
	_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tabs.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	root.add_child(_tabs)
	_build_overview_tab()
	_build_diagnostics_tab()
	_build_timeline_tab()
	_build_spatial_tab()
	_build_settings_tab()
	_build_subsystems_tab()
	_build_raw_tab()

	_export_dialog = FileDialog.new()
	_export_dialog.title = "Export Terrain3D Diagnostics"
	_export_dialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE
	_export_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_export_dialog.filters = PackedStringArray(["*.json ; JSON diagnostics"])
	_export_dialog.current_file = "terrain3d-runtime-diagnostics.json"
	_export_dialog.file_selected.connect(_write_snapshot)
	add_child(_export_dialog)
	set_process(true)
	refresh_now()


func _build_toolbar(p_root: VBoxContainer) -> void:
	var toolbar: HBoxContainer = HBoxContainer.new()
	p_root.add_child(toolbar)
	_status = Label.new()
	_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_status.text = "No Terrain3D selected"
	toolbar.add_child(_status)
	_pause = CheckButton.new()
	_pause.text = "Freeze display"
	_pause.tooltip_text = "Stop UI updates while Terrain3D continues collecting history"
	toolbar.add_child(_pause)
	_refresh_rate = OptionButton.new()
	_refresh_rate.tooltip_text = "Profiler UI refresh frequency only. This does not change Terrain3D simulation/render scheduling or the underlying diagnostic sampling counters."
	for rate: int in [2, 4, 10, 20]:
		_refresh_rate.add_item("%d Hz" % rate, rate)
	_refresh_rate.select(1)
	_refresh_rate.item_selected.connect(_on_refresh_rate_selected)
	toolbar.add_child(_refresh_rate)
	var refresh_button: Button = Button.new()
	refresh_button.text = "Refresh"
	refresh_button.tooltip_text = "Refresh the visible profiler snapshot immediately without changing runtime settings."
	refresh_button.pressed.connect(refresh_now)
	toolbar.add_child(refresh_button)
	var reset_button: Button = Button.new()
	reset_button.text = "Reset"
	reset_button.tooltip_text = "Reset Terrain3D diagnostic counters/history used by this profiler view."
	reset_button.pressed.connect(_reset_counters)
	toolbar.add_child(reset_button)
	var export_button: Button = Button.new()
	export_button.text = "Export..."
	export_button.tooltip_text = "Export the current Terrain3D diagnostic/history snapshot without changing terrain configuration."
	export_button.pressed.connect(_show_export_dialog)
	toolbar.add_child(export_button)
	_overlay_button = Button.new()
	_overlay_button.text = "Show overlay"
	_overlay_button.tooltip_text = "Attach a temporary live diagnostics overlay to this Terrain3D. The overlay script can also be added to debug/export scenes."
	_overlay_button.pressed.connect(_toggle_runtime_overlay)
	toolbar.add_child(_overlay_button)


func _build_overview_tab() -> void:
	var split: HSplitContainer = HSplitContainer.new()
	split.name = "Overview"
	_tabs.add_child(split)
	var left: VBoxContainer = VBoxContainer.new()
	left.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	split.add_child(left)
	var metrics_title: Label = Label.new()
	metrics_title.text = "Frame phases"
	left.add_child(metrics_title)
	_metrics_tree = Tree.new()
	_metrics_tree.columns = 5
	_metrics_tree.column_titles_visible = true
	_metrics_tree.hide_root = true
	for column: int in range(5):
		_metrics_tree.set_column_expand(column, true)
	_metrics_tree.set_column_title(0, "Phase")
	_metrics_tree.set_column_title(1, "Current")
	_metrics_tree.set_column_title(2, "Recent avg")
	_metrics_tree.set_column_title(3, "P95")
	_metrics_tree.set_column_title(4, "Peak")
	_metrics_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	left.add_child(_metrics_tree)
	_initialize_metric_rows()

	var right: VBoxContainer = VBoxContainer.new()
	right.custom_minimum_size.x = 480.0
	split.add_child(right)
	var problems_title: Label = Label.new()
	problems_title.text = "Active constraints and problems"
	right.add_child(problems_title)
	_problems_tree = Tree.new()
	_problems_tree.columns = 3
	_problems_tree.column_titles_visible = true
	_problems_tree.hide_root = true
	_problems_tree.set_column_title(0, "Severity")
	_problems_tree.set_column_title(1, "Problem")
	_problems_tree.set_column_title(2, "Value")
	_problems_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right.add_child(_problems_tree)
	var pressure_title: Label = Label.new()
	pressure_title.text = "Budget pressure and backlog"
	right.add_child(pressure_title)
	_pressure_tree = Tree.new()
	_pressure_tree.columns = 5
	_pressure_tree.column_titles_visible = true
	_pressure_tree.hide_root = true
	_pressure_tree.custom_minimum_size.y = 280.0
	var pressure_titles: PackedStringArray = PackedStringArray(["Budget", "Current", "Limit", "Pressure", "Recent trend"])
	for column: int in range(pressure_titles.size()):
		_pressure_tree.set_column_title(column, pressure_titles[column])
	right.add_child(_pressure_tree)
	var efficiency_title: Label = Label.new()
	efficiency_title.text = "Streaming efficiency"
	right.add_child(efficiency_title)
	_efficiency_tree = Tree.new()
	_efficiency_tree.columns = 3
	_efficiency_tree.column_titles_visible = true
	_efficiency_tree.hide_root = true
	_efficiency_tree.custom_minimum_size.y = 190.0
	_efficiency_tree.set_column_title(0, "Metric")
	_efficiency_tree.set_column_title(1, "Value")
	_efficiency_tree.set_column_title(2, "Context")
	right.add_child(_efficiency_tree)



func _build_diagnostics_tab() -> void:
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.name = "Diagnostics"
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tabs.add_child(scroll)
	var root: VBoxContainer = VBoxContainer.new()
	root.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	root.add_theme_constant_override("separation", 10)
	scroll.add_child(root)
	var intro: Label = Label.new()
	intro.text = "Terrain3D-owned diagnostics. GPU data is explicitly classified as Measured, Modeled, or Scene-wide."
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(intro)
	_deep_introspection = CheckButton.new()
	_deep_introspection.text = "Deep introspection (slower debug scan)"
	_deep_introspection.tooltip_text = "Explicitly enables O(N) node, instance, collision, and residency-detail scans. Leave disabled for normal profiler refresh."
	root.add_child(_deep_introspection)
	_diagnostics_memory_tree = _create_diagnostics_tree(root, "Memory Budget", PackedStringArray(["Metric", "Value", "Status"]), 210.0)
	_diagnostics_pipeline_tree = _create_diagnostics_tree(root, "Active Render Pipeline", PackedStringArray([
		"Tier", "Range", "Shader RID", "Variant", "Valid", "Params", "Fallback", "Profile", "Filtering / mip", "Lighting", "Batches V/S", "Modeled fetches", "Source"
	]), 190.0)
	_diagnostics_cache_tree = _create_diagnostics_tree(root, "Cache Inventory", PackedStringArray([
		"Cache", "Resident / cap", "Current / peak", "Hits / misses", "Hit rate", "Evictions", "Queued", "Preparing", "Uploading", "Requested", "Effective", "Limit"
	]), 220.0)
	_diagnostics_cpu_tree = _create_diagnostics_tree(root, "CPU Profiling", PackedStringArray(["Subsystem", "Last", "Meaning"]), 210.0)
	_diagnostics_gpu_tree = _create_diagnostics_tree(root, "GPU Information", PackedStringArray(["Class", "Terrain3D value", "Scope / meaning"]), 150.0)
	_diagnostics_rd_tree = _create_diagnostics_tree(root, "RenderingDevice Experiment", PackedStringArray(["Metric", "Value", "Meaning"]), 240.0)


func _create_diagnostics_tree(p_parent: VBoxContainer, p_title: String, p_titles: PackedStringArray, p_height: float) -> Tree:
	var title_label: Label = Label.new()
	title_label.text = p_title
	p_parent.add_child(title_label)
	var tree: Tree = Tree.new()
	tree.columns = p_titles.size()
	tree.column_titles_visible = true
	tree.hide_root = true
	tree.custom_minimum_size.y = p_height
	tree.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for column: int in range(p_titles.size()):
		tree.set_column_title(column, p_titles[column])
		tree.set_column_expand(column, true)
	p_parent.add_child(tree)
	return tree


func _update_diagnostics_products(p_snapshot: Terrain3DRuntimeValueMap) -> void:
	_update_memory_budget_product(p_snapshot.map_value("memory"))
	_update_render_pipeline_product(p_snapshot.map_array("active_render_pipeline"))
	_update_cache_inventory_product(p_snapshot.map_array("cache_inventory"))
	_update_cpu_profile_product(p_snapshot.map_value("cpu_profile"))
	_update_gpu_information_product(p_snapshot.map_value("gpu_information"))
	_update_rd_renderer_product()


func _update_memory_budget_product(p_memory: Terrain3DRuntimeValueMap) -> void:
	_diagnostics_memory_tree.clear()
	var root: TreeItem = _diagnostics_memory_tree.create_item()
	var rows: Array[Array] = [
		["Configured total", _format_bytes(p_memory.float_value("configured_budget_bytes")), p_memory.string_value("pressure_state", "normal")],
		["Current known", _format_bytes(p_memory.float_value("current_known_bytes")), "tracked"],
		["Peak known", _format_bytes(p_memory.float_value("peak_known_bytes")), "tracked peak"],
		["Fixed / unavoidable", _format_bytes(p_memory.float_value("fixed_bytes")), "committed"],
		["Managed resident", _format_bytes(p_memory.float_value("managed_resident_bytes")), "budget coordinated"],
		["Transient / staging", _format_bytes(p_memory.float_value("transient_bytes")), "observed"],
		["Known headroom", _format_bytes(p_memory.float_value("known_headroom_bytes")), "overcommit" if p_memory.bool_value("overcommitted") else "available"],
		["External / untracked", p_memory.string_value("external_untracked", "unavailable"), "unavailable"],
	]
	for row: Array in rows:
		var item: TreeItem = _diagnostics_memory_tree.create_item(root)
		item.set_text(0, str(row[0]))
		item.set_text(1, str(row[1]))
		item.set_text(2, str(row[2]))


func _update_render_pipeline_product(p_tiers: Array[Terrain3DRuntimeValueMap]) -> void:
	_diagnostics_pipeline_tree.clear()
	var root: TreeItem = _diagnostics_pipeline_tree.create_item()
	for tier: Terrain3DRuntimeValueMap in p_tiers:
		var item: TreeItem = _diagnostics_pipeline_tree.create_item(root)
		var range_text: String = "%.0f m+" % tier.float_value("range_start_m") if tier.bool_value("range_unbounded") else "%.0f–%.0f m" % [tier.float_value("range_start_m"), tier.float_value("range_end_m")]
		item.set_text(0, tier.string_value("tier_name"))
		item.set_text(1, range_text)
		item.set_text(2, str(tier.int_value("shader_rid")))
		item.set_text(3, tier.string_value("variant_signature"))
		item.set_text(4, "yes" if tier.bool_value("compile_valid") else "NO")
		item.set_text(5, str(tier.int_value("reflected_parameter_count")))
		item.set_text(6, "Near" if tier.bool_value("fallback_to_near") else "none")
		item.set_text(7, "%s / %s" % [tier.string_value("profile_name"), tier.string_value("profile_status_name")])
		item.set_text(8, "%s / %.2f" % [tier.string_value("texture_filtering_name"), tier.float_value("material_lod_bias")])
		item.set_text(9, tier.string_value("lighting_model_name"))
		item.set_text(10, "%d / %d" % [tier.int_value("render_batch_count"), tier.int_value("shadow_batch_count")])
		item.set_text(11, str(tier.int_value("modeled_major_texture_fetches")))
		item.set_text(12, _format_bytes(tier.float_value("shader_source_bytes")))


func _update_cache_inventory_product(p_caches: Array[Terrain3DRuntimeValueMap]) -> void:
	_diagnostics_cache_tree.clear()
	var root: TreeItem = _diagnostics_cache_tree.create_item()
	for cache: Terrain3DRuntimeValueMap in p_caches:
		var item: TreeItem = _diagnostics_cache_tree.create_item(root)
		item.set_text(0, _humanize_name(cache.string_value("cache_name")))
		item.set_text(1, "%d / %d" % [cache.int_value("resident"), cache.int_value("effective_capacity")])
		item.set_text(2, "%s / %s" % [_format_bytes(cache.float_value("current_bytes")), _format_bytes(cache.float_value("peak_bytes"))])
		item.set_text(3, "unavailable" if cache.int_value("hits", -1) < 0 else "%d / %d" % [cache.int_value("hits"), cache.int_value("misses")])
		item.set_text(4, "unavailable" if cache.float_value("hit_rate", -1.0) < 0.0 else "%.1f%%" % (cache.float_value("hit_rate") * 100.0))
		item.set_text(5, str(cache.int_value("evictions")))
		item.set_text(6, str(cache.int_value("queued")))
		item.set_text(7, str(cache.int_value("preparing")))
		item.set_text(8, str(cache.int_value("uploading")))
		item.set_text(9, _format_bytes(cache.float_value("requested_budget_bytes")))
		item.set_text(10, _format_bytes(cache.float_value("effective_budget_bytes")))
		item.set_text(11, cache.string_value("limiting_constraint", "none"))


func _update_cpu_profile_product(p_cpu: Terrain3DRuntimeValueMap) -> void:
	_diagnostics_cpu_tree.clear()
	var root: TreeItem = _diagnostics_cpu_tree.create_item()
	var rows: Array[Array] = [
		["GPU page cache service", "gpu_page_cache_service_cpu_usec", "Terrain3D CPU residency/service time"],
		["GPU page preparation workers", "gpu_page_preparation_worker_cpu_usec", "Worker CPU duration, not GPU execution"],
		["RD publication CPU", "rendering_server_publication_cpu_usec", "CPU time spent preparing/publishing RenderingDevice work"],
		["CPU cache service", "cpu_cache_service_cpu_usec", "Terrain3D CPU cache service"],
		["Instance cache service", "instance_cache_service_cpu_usec", "Terrain3D instance-page service"],
		["Instance streaming / LOD", "instance_streaming_cpu_usec", "Instancer CPU work"],
		["Node streaming / migration", "node_streaming_migration_cpu_usec", "Node streamer CPU work"],
		["Baked lighting streaming", "baked_lighting_streaming_cpu_usec", "Lightmap residency CPU work"],
		["Collision rebuild", "collision_rebuild_cpu_usec", "Collision CPU work"],
		["Far height rebuild", "far_height_rebuild_cpu_usec", "Far atlas CPU rebuild"],
		["Far color rebuild", "far_color_rebuild_cpu_usec", "Far atlas CPU rebuild"],
		["Far atlas publication", "far_atlas_publication_cpu_usec", "CPU time spent submitting the full far Texture2D payload"],
		["Far atlas rebuild / publication", "far_atlas_rebuild_publication_cpu_usec", "Combined far-atlas rebuild and texture publication CPU"],
		["Profiler collection", "profiler_collection_cpu_usec_last", "Diagnostics collection overhead"],
	]
	for row: Array in rows:
		var item: TreeItem = _diagnostics_cpu_tree.create_item(root)
		item.set_text(0, str(row[0]))
		item.set_text(1, _format_usec(p_cpu.float_value(str(row[1]))))
		item.set_text(2, str(row[2]))


func _update_gpu_information_product(p_gpu: Terrain3DRuntimeValueMap) -> void:
	_diagnostics_gpu_tree.clear()
	var root: TreeItem = _diagnostics_gpu_tree.create_item()
	var measured: Terrain3DRuntimeValueMap = p_gpu.map_value("measured")
	var modeled: Terrain3DRuntimeValueMap = p_gpu.map_value("modeled")
	var engine: Dictionary[String, float] = _get_engine_monitors()
	var measured_item: TreeItem = _diagnostics_gpu_tree.create_item(root)
	measured_item.set_text(0, "Measured")
	measured_item.set_text(1, "unavailable" if not measured.bool_value("available") else str(measured.raw_value("terrain_gpu_time_usec")))
	measured_item.set_text(2, measured.string_value("note", "No isolated Terrain3D GPU timestamp scope available"))
	var modeled_item: TreeItem = _diagnostics_gpu_tree.create_item(root)
	modeled_item.set_text(0, "Modeled")
	modeled_item.set_text(1, "%d render / %d shadow batches" % [modeled.int_value("render_batches"), modeled.int_value("shadow_batches")])
	modeled_item.set_text(2, modeled.string_value("note", "Submitted Terrain3D work model; not GPU milliseconds"))
	var scene_item: TreeItem = _diagnostics_gpu_tree.create_item(root)
	scene_item.set_text(0, "Scene-wide")
	scene_item.set_text(1, "%d draw calls / %d primitives" % [int(engine.get("draw_calls", 0.0)), int(engine.get("render_primitives", 0.0))])
	scene_item.set_text(2, "Godot scene-wide counters: %.1f MiB video, %.1f MiB textures, %.1f MiB buffers; not Terrain3D-only" % [
		float(engine.get("video_memory_bytes", 0.0)) / 1048576.0,
		float(engine.get("texture_memory_bytes", 0.0)) / 1048576.0,
		float(engine.get("buffer_memory_bytes", 0.0)) / 1048576.0,
	])


func _update_rd_renderer_product() -> void:
	_diagnostics_rd_tree.clear()
	var root: TreeItem = _diagnostics_rd_tree.create_item()
	if not is_instance_valid(terrain):
		return
	var diagnostics: Dictionary = terrain.get_rd_renderer_diagnostics()
	var rows: Array[Array] = [
		["Rendering path", "RenderingDevice / Forward+", "Terrain3D production rendering is RD-owned and submits into native Forward+ passes"],
		["RD color writes", "yes" if bool(diagnostics.get("color_write_enabled", false)) else "no", "Terrain3D-owned Forward+ color submission active for the RD backend"],
		["RD render enabled", "yes" if bool(diagnostics.get("render_enabled", false)) else "no", "Native Forward+ Terrain3D submission enabled"],
		["RD initialized", "yes" if bool(diagnostics.get("renderer_initialized", false)) else "no", "Direct RD resources and pipelines initialized"],
		["Failure latched", "YES" if bool(diagnostics.get("renderer_failure_latched", false)) else "no", "RD submission halted after repeated failures; native Terrain3D rendering remains disabled"],
		["Compositor debug callbacks", str(int(diagnostics.get("callbacks", 0))), "Callbacks received only by the explicit compositor debug lane"],
		["Forward+ bridge", "ready" if bool(diagnostics.get("forward_plus_bridge_ready", false)) else "unavailable", "Patched Godot native submission bridge availability"],
		["Bridge callback", "registered" if bool(diagnostics.get("forward_plus_callback_registered", false)) else "not registered", "Terrain3D callback registration state"],
		["Bridge ABI", str(int(diagnostics.get("forward_plus_bridge_abi", 0))), "Observed Terrain3D Forward+ bridge ABI"],
		["Native depth", "active" if bool(diagnostics.get("native_depth_prepass_participation", false)) else "not observed", "Native Forward+ depth pass participation observed"],
		["Native shadows", "active" if bool(diagnostics.get("native_shadow_participation", false)) else "not observed", "Native Forward+ shadow pass participation observed"],
		["Native motion", "active" if bool(diagnostics.get("native_motion_vectors_active", false)) else "not observed", "Native Forward+ motion pass participation observed"],
		["Clustered lighting", "ready" if bool(diagnostics.get("clustered_light_path_active", false)) else "not ready", "Borrowed native Forward+ clustered-light resources are valid"],
		["Native view/sample", "%d / %d" % [int(diagnostics.get("current_forward_view_count", 0)), int(diagnostics.get("current_forward_sample_count", 0))], "Most recent native Forward+ view and sample count"],
		["Frames rendered", str(int(diagnostics.get("frames_rendered", 0))), "Frames successfully submitted by the RD terrain renderer"],
		["Draws rendered", str(int(diagnostics.get("draws_rendered", 0))), "Indexed RD terrain draws submitted"],
		["Indirect draws", str(int(diagnostics.get("indirect_draws", 0))), "Indexed indirect draw commands submitted"],
		["Cull dispatches", str(int(diagnostics.get("cull_dispatches", 0))), "GPU clipmap culling compute dispatches"],
		["GPU culling", "yes" if bool(diagnostics.get("gpu_culling", false)) else "CPU fallback", "Active clipmap visible-list generation path"],
		["Topology", _format_bytes(float(diagnostics.get("topology_bytes", 0))), "Persistent RD clipmap vertex/index memory"],
		["Index formats", "%d / %d bit" % [int(diagnostics.get("lod0_index_bits", 0)), int(diagnostics.get("outer_index_bits", 0))], "LOD0 / outer-ring index width selected at upload"],
		["Live RD buffers", _format_bytes(float(diagnostics.get("live_buffer_bytes", 0))), "Terrain3D-owned RenderingDevice buffer memory"],
		["Peak RD buffers", _format_bytes(float(diagnostics.get("peak_buffer_bytes", 0))), "High-water Terrain3D-owned RD buffer memory"],
		["Live RD textures", _format_bytes(float(diagnostics.get("live_texture_bytes", 0))), "Terrain3D-owned RD texture memory; borrowed native renderer textures excluded"],
		["Buffer updates", str(int(diagnostics.get("buffer_updates", 0))), "Explicit RD buffer update calls"],
		["Bytes uploaded", _format_bytes(float(diagnostics.get("bytes_uploaded", 0))), "Total explicit RD buffer upload traffic"],
		["Consecutive failures", str(int(diagnostics.get("consecutive_render_failures", 0))), "RD failures since the most recent successful render"],
		["Near height binding", "READY" if bool(diagnostics.get("published_height_texture_valid", false)) else "PENDING", "Published Near height-array RID; PENDING uses Middle/Far/background fallback without latching the renderer"],
		["Renderer recoveries", str(int(diagnostics.get("renderer_recoveries", 0))), "Automatic retries after a latched failure when terrain/material resource generations advance"],
		["Initialization error", str(diagnostics.get("initialization_error", "")), "Last RD initialization error"],
		["Last render error", str(diagnostics.get("last_render_error", "")), "Last direct-render failure"],
		["Multiview", "supported" if bool(diagnostics.get("multiview_supported", false)) else "unsupported", "Per-view native Forward+ framebuffer/pipeline support"],
		["Native shadow proxy", "ACTIVE" if bool(diagnostics.get("native_shadow_proxy_active", false)) else "off", "Must remain off for RD production"],
	]
	for row: Array in rows:
		var item: TreeItem = _diagnostics_rd_tree.create_item(root)
		item.set_text(0, str(row[0]))
		item.set_text(1, str(row[1]))
		item.set_text(2, str(row[2]))


func _build_timeline_tab() -> void:
	var root: VBoxContainer = VBoxContainer.new()
	root.name = "Timeline"
	_tabs.add_child(root)
	var controls: HBoxContainer = HBoxContainer.new()
	root.add_child(controls)
	_auto_freeze = CheckButton.new()
	_auto_freeze.tooltip_text = "Automatically freeze the displayed timeline when any enabled trigger below fires; Terrain3D itself continues running."
	_auto_freeze.text = "Auto-freeze"
	controls.add_child(_auto_freeze)
	controls.add_child(_make_label("Terrain time"))
	_freeze_threshold = SpinBox.new()
	_freeze_threshold.tooltip_text = "Terrain3D frame-time threshold that triggers timeline auto-freeze when Auto-freeze is enabled."
	_freeze_threshold.min_value = 0.1
	_freeze_threshold.max_value = 50.0
	_freeze_threshold.step = 0.1
	_freeze_threshold.value = 2.0
	_freeze_threshold.suffix = " ms"
	controls.add_child(_freeze_threshold)
	_freeze_on_missing = CheckButton.new()
	_freeze_on_missing.tooltip_text = "Trigger timeline auto-freeze when a required terrain page is missing."
	_freeze_on_missing.text = "Missing required page"
	_freeze_on_missing.button_pressed = true
	controls.add_child(_freeze_on_missing)
	_freeze_on_failure = CheckButton.new()
	_freeze_on_failure.tooltip_text = "Trigger timeline auto-freeze when a streaming/load failure is reported."
	_freeze_on_failure.text = "Load failure"
	_freeze_on_failure.button_pressed = true
	controls.add_child(_freeze_on_failure)
	_freeze_on_shader = CheckButton.new()
	_freeze_on_shader.tooltip_text = "Trigger timeline auto-freeze when a terrain shader variant changes, rebuilds, or reports a compile/validation event."
	_freeze_on_shader.text = "Shader update"
	_freeze_on_shader.button_pressed = true
	controls.add_child(_freeze_on_shader)
	controls.add_child(_make_label("Post-trigger"))
	_post_trigger_samples = SpinBox.new()
	_post_trigger_samples.tooltip_text = "Number of profiler samples to continue collecting after an auto-freeze trigger before the display freezes."
	_post_trigger_samples.min_value = 0.0
	_post_trigger_samples.max_value = 120.0
	_post_trigger_samples.value = 8.0
	_post_trigger_samples.suffix = " samples"
	controls.add_child(_post_trigger_samples)

	_history_graph = HistoryGraph.new()
	_history_graph.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_history_graph.sample_selected.connect(_on_history_graph_selected)
	root.add_child(_history_graph)
	var split: HSplitContainer = HSplitContainer.new()
	split.custom_minimum_size.y = 260.0
	root.add_child(split)
	_history_tree = Tree.new()
	_history_tree.columns = 7
	_history_tree.column_titles_visible = true
	_history_tree.hide_root = true
	for column: int in range(7):
		_history_tree.set_column_expand(column, true)
	var history_titles: PackedStringArray = PackedStringArray(["Frame", "Total", "Residency", "Clipmap", "Instances", "Events", "Stalls"])
	for column: int in range(history_titles.size()):
		_history_tree.set_column_title(column, history_titles[column])
	_history_tree.item_selected.connect(_on_history_tree_selected)
	split.add_child(_history_tree)
	_history_detail = Tree.new()
	_history_detail.columns = 2
	_history_detail.column_titles_visible = true
	_history_detail.hide_root = true
	_history_detail.set_column_title(0, "Metric")
	_history_detail.set_column_title(1, "Value")
	split.add_child(_history_detail)


func _build_spatial_tab() -> void:
	var split: HSplitContainer = HSplitContainer.new()
	split.name = "Residency map"
	_tabs.add_child(split)
	_residency_grid = ResidencyGrid.new()
	_residency_grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_residency_grid.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_residency_grid.cell_selected.connect(_on_residency_cell_selected)
	split.add_child(_residency_grid)
	var sidebar: VBoxContainer = VBoxContainer.new()
	sidebar.custom_minimum_size.x = 390.0
	split.add_child(sidebar)
	var legend: RichTextLabel = RichTextLabel.new()
	legend.fit_content = true
	legend.bbcode_enabled = true
	legend.text = "[b]Region state[/b]\nRed: ACTIVE\nGold: HOT\nBlue: COLD\nDark: UNLOADED\nCyan outline: predicted promotion\n\n[b]Residency markers[/b]\nGreen inner box: full surface GPU page\nPurple bottom line: Middle height page\nBlue top line: CPU full maps\nOrange dot: instance resident\nYellow dot: Far atlas dirty\nPurple center: preparing/ready\nGray outline: retiring\nWhite box: target/selection"
	sidebar.add_child(legend)
	_residency_detail = RichTextLabel.new()
	_residency_detail.bbcode_enabled = true
	_residency_detail.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_residency_detail.text = "Select a region cell."
	sidebar.add_child(_residency_detail)
	_priority_summary = Label.new()
	_priority_summary.text = "Pending residency requests"
	sidebar.add_child(_priority_summary)
	_priority_tree = Tree.new()
	_priority_tree.columns = 6
	_priority_tree.column_titles_visible = true
	_priority_tree.hide_root = true
	_priority_tree.custom_minimum_size.y = 230.0
	var priority_titles: PackedStringArray = PackedStringArray(["Rank", "Class", "Region", "State", "Distance", "Ahead"])
	for column: int in range(priority_titles.size()):
		_priority_tree.set_column_title(column, priority_titles[column])
	sidebar.add_child(_priority_tree)


func _build_settings_tab() -> void:
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.name = "Controls"
	_tabs.add_child(scroll)
	var root: VBoxContainer = VBoxContainer.new()
	root.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(root)

	var mode_note: Label = Label.new()
	mode_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	mode_note.text = "Edit modes: Serialized configuration changes the scene/resource. Profile-owned serialized override keeps the selected base profile and marks it Modified. Temporary runtime override is used only by the adaptive effective-budget controller shown below."
	root.add_child(mode_note)

	_add_metadata_control_groups(root)

	_range_summary = Label.new()
	_range_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_range_summary)

	root.add_child(_section_label("Adaptive effective budgets — Temporary runtime override"))
	_adaptive_summary = Label.new()
	_adaptive_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_adaptive_summary)
	_adaptive_tree = Tree.new()
	_adaptive_tree.columns = 5
	_adaptive_tree.column_titles_visible = true
	_adaptive_tree.hide_root = true
	_adaptive_tree.custom_minimum_size.y = 220.0
	var adaptive_titles: PackedStringArray = PackedStringArray(["Subsystem", "Effective", "Minimum", "Maximum", "State"])
	for column: int in range(adaptive_titles.size()):
		_adaptive_tree.set_column_title(column, adaptive_titles[column])
	root.add_child(_adaptive_tree)

	_phasing_summary = Label.new()
	_phasing_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_phasing_summary)

	root.add_child(_section_label("Validation"))
	_validation = RichTextLabel.new()
	_validation.bbcode_enabled = true
	_validation.fit_content = true
	root.add_child(_validation)
	var action_row: HBoxContainer = HBoxContainer.new()
	root.add_child(action_row)
	var restore_button: Button = Button.new()
	restore_button.text = "Reset to selected profile"
	restore_button.tooltip_text = "Reapply the authoritative selected profile and clear its serialized overrides."
	restore_button.pressed.connect(_reset_selected_profile)
	action_row.add_child(restore_button)
	var fit_detail_button: Button = Button.new()
	fit_detail_button.text = "Fit detail residency to capacity"
	fit_detail_button.tooltip_text = "Adjust the detailed GPU residency range to the current mapped-page capacity. This is an explicit residency control; page capacity does not rewrite Near/Middle/Far render tiers automatically."
	fit_detail_button.pressed.connect(_fit_detail_range_to_capacity)
	action_row.add_child(fit_detail_button)

	root.add_child(_section_label("Profile differences"))
	_profile_diff_tree = Tree.new()
	_profile_diff_tree.columns = 3
	_profile_diff_tree.column_titles_visible = true
	_profile_diff_tree.hide_root = true
	_profile_diff_tree.set_column_title(0, "Setting")
	_profile_diff_tree.set_column_title(1, "Current")
	_profile_diff_tree.set_column_title(2, "Profile")
	_profile_diff_tree.custom_minimum_size.y = 260.0
	root.add_child(_profile_diff_tree)


func _add_metadata_control_groups(p_root: VBoxContainer) -> void:
	var descriptors: Array[Dictionary] = SettingMetadata.descriptors_for_class("Terrain3D", CONTROL_GROUP_PREFIXES)
	for group: Dictionary in SettingMetadata.group_descriptors(descriptors):
		var supported: Array[Dictionary] = []
		for descriptor: Dictionary in group.get("settings", []):
			if SettingControlFactory.is_supported(descriptor):
				supported.append(descriptor)
		if supported.is_empty():
			continue
		var group_path: String = str(group.get("group_path", "General"))
		var foldable: FoldableContainer = FoldableContainer.new()
		foldable.title = group_path
		foldable.folded = _group_is_advanced(supported)
		foldable.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		p_root.add_child(foldable)
		var margin: MarginContainer = MarginContainer.new()
		margin.add_theme_constant_override("margin_left", 8)
		margin.add_theme_constant_override("margin_top", 4)
		margin.add_theme_constant_override("margin_right", 8)
		margin.add_theme_constant_override("margin_bottom", 8)
		foldable.add_child(margin)
		var grid: GridContainer = GridContainer.new()
		grid.columns = 3
		grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		margin.add_child(grid)
		for descriptor: Dictionary in supported:
			_add_metadata_control_row(grid, descriptor)


func _group_is_advanced(p_descriptors: Array[Dictionary]) -> bool:
	var advanced_count: int = 0
	for descriptor: Dictionary in p_descriptors:
		if str(descriptor.get("visibility", "basic")) == "advanced":
			advanced_count += 1
	return advanced_count > 0 and advanced_count * 2 >= p_descriptors.size()


func _add_metadata_control_row(p_grid: GridContainer, p_descriptor: Dictionary) -> void:
	var property_name: String = str(p_descriptor.get("property_name", ""))
	var tooltip: String = SettingControlFactory.tooltip_with_mode(p_descriptor)
	var label: Label = _make_label(str(p_descriptor.get("display_name", property_name)))
	label.tooltip_text = tooltip
	p_grid.add_child(label)
	var control: Control = SettingControlFactory.create_control(p_descriptor, func(p_value: Variant) -> void: _on_metadata_setting_changed(property_name, p_value))
	if control == null:
		label.queue_free()
		return
	control.tooltip_text = tooltip
	control.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	control.set_meta("terrain3d_setting_key", str(p_descriptor.get("key", "")))
	control.set_meta("terrain3d_mutation_mode", str(p_descriptor.get("mutation_mode", "serialized_configuration")))
	p_grid.add_child(control)
	var mode_label: Label = Label.new()
	mode_label.text = SettingControlFactory.mutation_label(p_descriptor)
	mode_label.tooltip_text = tooltip
	mode_label.modulate.a = 0.72
	p_grid.add_child(mode_label)
	_control_bindings[property_name] = control
	_control_descriptors[property_name] = p_descriptor


func _on_metadata_setting_changed(p_property: String, p_value: Variant) -> void:
	if _updating_controls or not is_instance_valid(terrain):
		return
	terrain.set(p_property, p_value)
	terrain.notify_property_list_changed()
	if Engine.is_editor_hint():
		EditorInterface.mark_scene_as_unsaved()
	refresh_now()


func _build_raw_tab() -> void:
	_raw_tree = Tree.new()
	_raw_tree.name = "Raw"
	_raw_tree.columns = 3
	_raw_tree.column_titles_visible = true
	_raw_tree.hide_root = true
	_raw_tree.set_column_title(0, "Subsystem / metric")
	_raw_tree.set_column_title(1, "Formatted")
	_raw_tree.set_column_title(2, "Raw")
	_tabs.add_child(_raw_tree)


func _build_subsystems_tab() -> void:
	_subsystem_tree = Tree.new()
	_subsystem_tree.name = "Subsystems"
	_subsystem_tree.columns = 3
	_subsystem_tree.column_titles_visible = true
	_subsystem_tree.hide_root = true
	_subsystem_tree.set_column_title(0, "Subsystem / metric")
	_subsystem_tree.set_column_title(1, "Formatted")
	_subsystem_tree.set_column_title(2, "Raw")
	_tabs.add_child(_subsystem_tree)


func set_terrain(p_terrain: Terrain3D) -> void:
	_remove_runtime_overlay()
	_restore_previous_profiling_state()
	terrain = p_terrain
	_refresh_accumulator = _refresh_interval_sec
	_reset_ui_state()
	if is_node_ready():
		refresh_now()


func open_for_terrain(p_terrain: Terrain3D) -> void:
	if not is_node_ready():
		open_for_terrain.call_deferred(p_terrain)
		return
	set_terrain(p_terrain)
	if is_instance_valid(terrain) and not terrain.is_runtime_profiling_enabled():
		terrain.set_runtime_profiling_enabled(true)
		_restore_profiling_on_close = true
	popup_centered()
	refresh_now()


func _restore_previous_profiling_state() -> void:
	if _restore_profiling_on_close and is_instance_valid(terrain):
		terrain.set_runtime_profiling_enabled(false)
	_restore_profiling_on_close = false


func _close_profiler() -> void:
	_remove_runtime_overlay()
	_restore_previous_profiling_state()
	hide()


func _toggle_runtime_overlay() -> void:
	if is_instance_valid(_overlay):
		_remove_runtime_overlay()
		return
	if not is_instance_valid(terrain):
		return
	_overlay = RuntimeOverlay.new()
	_overlay.name = "Terrain3DRuntimeDiagnosticsOverlay"
	terrain.add_child(_overlay)
	_overlay.set_terrain(terrain)
	_overlay.tree_exited.connect(_on_runtime_overlay_exited)
	_overlay_button.text = "Hide overlay"


func _remove_runtime_overlay() -> void:
	if is_instance_valid(_overlay):
		_overlay.queue_free()
	_overlay = null
	if is_instance_valid(_overlay_button):
		_overlay_button.text = "Show overlay"


func _on_runtime_overlay_exited() -> void:
	_overlay = null
	if is_instance_valid(_overlay_button):
		_overlay_button.text = "Show overlay"


func _process(p_delta: float) -> void:
	if not is_instance_valid(_pause) or not visible or _pause.button_pressed:
		return
	_refresh_accumulator += p_delta
	if _refresh_accumulator >= _refresh_interval_sec:
		_refresh_accumulator = 0.0
		refresh_now()


func refresh_now() -> void:
	if not is_node_ready() or not is_instance_valid(_status):
		return
	if not is_instance_valid(terrain):
		_status.text = "No Terrain3D selected"
		return
	var terrain_snapshot: Terrain3DRuntimeValueMap = Terrain3DRuntimeValueMap.from_value(terrain.get_runtime_diagnostics(_deep_introspection.button_pressed))
	var current_frame: Terrain3DRuntimeValueMap = terrain_snapshot.map_value("frame")
	var current_sequence: int = current_frame.int_value("sequence")
	if _history_fetch_sequence > current_sequence:
		_history.clear()
		_history_items.clear()
		_history_tree.clear()
		_history_detail.clear()
		_last_history_sequence = -1
		_history_fetch_sequence = -1
		_selected_history_index = -1
	var new_samples: Array[Terrain3DRuntimeValueMap] = Terrain3DRuntimeValueMap.maps_from_value(terrain.get_runtime_profile_history_since(_history_fetch_sequence, HISTORY_SAMPLES))
	for sample: Terrain3DRuntimeValueMap in new_samples:
		_history.push_back(sample)
		_history_fetch_sequence = sample.int_value("sequence", _history_fetch_sequence)
	while _history.size() > HISTORY_SAMPLES:
		_history.pop_front()
	if is_instance_valid(terrain.data):
		_grid_snapshot = Terrain3DRuntimeValueMap.from_value(terrain.data.get_runtime_residency_grid(6))
	else:
		_grid_snapshot = Terrain3DRuntimeValueMap.new()
	_last_snapshot = Terrain3DRuntimeValueMap.from_value({
		"engine": _get_engine_monitors(),
		"terrain3d": terrain_snapshot.duplicate_values(),
		"history_summary": {
			"count": _history.size(),
			"oldest_sequence": _history.front().int_value("sequence") if not _history.is_empty() else 0,
			"newest_sequence": _history.back().int_value("sequence") if not _history.is_empty() else 0,
		},
		"residency_grid_summary": {
			"center": _grid_snapshot.vector2i_value("center"),
			"radius": _grid_snapshot.int_value("radius"),
			"active_count": _grid_snapshot.int_value("active_count"),
			"hot_count": _grid_snapshot.int_value("hot_count"),
			"cold_count": _grid_snapshot.int_value("cold_count"),
			"unloaded_count": _grid_snapshot.int_value("unloaded_count"),
			"predicted_count": _grid_snapshot.int_value("predicted_count"),
			"required_count": _grid_snapshot.int_value("required_count"),
			"guard_count": _grid_snapshot.int_value("guard_count"),
			"prefetch_count": _grid_snapshot.int_value("prefetch_count"),
		},
	})
	var frame: Terrain3DRuntimeValueMap = terrain_snapshot.map_value("frame")
	var sequence: int = frame.int_value("sequence")
	_status.text = "Frame %d — Terrain3D %.3f ms — history %d/%d" % [sequence, frame.float_value("physics_total_usec") / 1000.0, _history.size(), HISTORY_SAMPLES]
	var adaptive_status: Terrain3DRuntimeValueMap = terrain_snapshot.map_value("adaptive_budgets")
	if adaptive_status.bool_value("enabled"):
		_status.text += " — adaptive %s %.3f/%.3f ms" % [adaptive_status.string_value("reason", "steady"),
			adaptive_status.float_value("last_measured_usec") / 1000.0,
			adaptive_status.float_value("target_usec") / 1000.0]
	var phasing_status: Terrain3DRuntimeValueMap = terrain_snapshot.map_value("boundary_phasing")
	if phasing_status.bool_value("node_deferred_last_tick") or phasing_status.bool_value("instancer_deferred_last_tick"):
		var deferred: PackedStringArray = PackedStringArray()
		if phasing_status.bool_value("node_deferred_last_tick"):
			deferred.append("nodes")
		if phasing_status.bool_value("instancer_deferred_last_tick"):
			deferred.append("instances")
		_status.text += " — boundary phase deferred %s" % ", ".join(deferred)
	_update_overview(terrain_snapshot)
	_update_diagnostics_products(terrain_snapshot)
	_update_subsystems(terrain_snapshot)
	_update_timeline(sequence)
	_update_spatial(terrain_snapshot)
	_update_settings(terrain_snapshot)
	_update_raw_tree(_last_snapshot)
	_evaluate_auto_freeze(frame)


func _initialize_metric_rows() -> void:
	var root: TreeItem = _metrics_tree.create_item()
	var frame_group: TreeItem = _metrics_tree.create_item(root)
	frame_group.set_text(0, "Frame phases")
	var frame_entries: Array[PackedStringArray] = [
		PackedStringArray(["physics_total_usec", "Terrain total"]),
		PackedStringArray(["data_residency_usec", "Data residency"]),
		PackedStringArray(["hlod_streaming_usec", "HLOD streaming"]),
		PackedStringArray(["node_streaming_usec", "Node streaming"]),
		PackedStringArray(["clipmap_usec", "Clipmap"]),
		PackedStringArray(["ocean_usec", "Ocean"]),
		PackedStringArray(["instancer_usec", "Instancer"]),
		PackedStringArray(["light_sync_usec", "Light sync"]),
		PackedStringArray(["collision_usec", "Collision"]),
		PackedStringArray(["streaming_usec", "Streaming total"]),
	]
	for entry: PackedStringArray in frame_entries:
		var item: TreeItem = _metrics_tree.create_item(frame_group)
		item.set_text(0, entry[1])
		_metric_items[entry[0]] = item
	var pipeline_group: TreeItem = _metrics_tree.create_item(root)
	pipeline_group.set_text(0, "Cooked page pipeline")
	var pipeline_entries: Array[PackedStringArray] = [
		PackedStringArray(["gpu_prepare_latency_usec", "Batch wall time"]),
		PackedStringArray(["gpu_prepare_queue_wait_usec", "Worker queue wait (max)"]),
		PackedStringArray(["gpu_prepare_file_open_usec", "File open (total)"]),
		PackedStringArray(["gpu_prepare_file_read_usec", "File read (total)"]),
		PackedStringArray(["gpu_prepare_validation_usec", "Validation (total)"]),
		PackedStringArray(["gpu_prepare_materialize_usec", "Image materialization (total)"]),
		PackedStringArray(["gpu_prepare_worker_usec", "Worker duration (max)"]),
		PackedStringArray(["gpu_prepare_ready_wait_usec", "Ready queue wait"]),
		PackedStringArray(["gpu_prepare_publication_usec", "RD publication CPU"]),
		PackedStringArray(["gpu_prepare_end_to_end_usec", "Request to publication"]),
	]
	for entry: PackedStringArray in pipeline_entries:
		var item: TreeItem = _metrics_tree.create_item(pipeline_group)
		item.set_text(0, entry[1])
		_metric_items[entry[0]] = item


func _update_overview(p_snapshot: Terrain3DRuntimeValueMap) -> void:
	var frame: Terrain3DRuntimeValueMap = p_snapshot.map_value("frame")
	for key: String in _metric_items:
		var item: TreeItem = _metric_items[key]
		var stats: Dictionary[String, float] = _history_stats(key)
		item.set_text(1, _format_usec(frame.float_value(key)))
		item.set_text(2, _format_usec(float(stats.get("average", 0.0))))
		item.set_text(3, _format_usec(float(stats.get("p95", 0.0))))
		item.set_text(4, _format_usec(float(stats.get("peak", 0.0))))
	_update_problems(p_snapshot.map_array("problems"))
	_update_pressure(p_snapshot)
	_update_efficiency(p_snapshot)


func _history_stats(p_key: String) -> Dictionary[String, float]:
	if _history.is_empty():
		return {"average": 0.0, "p95": 0.0, "peak": 0.0}
	var values: Array[float] = []
	var total: float = 0.0
	var peak: float = 0.0
	for sample: Terrain3DRuntimeValueMap in _history:
		var value: float = sample.float_value(p_key)
		values.push_back(value)
		total += value
		peak = maxf(peak, value)
	values.sort()
	var percentile_index: int = clampi(int(ceilf(float(values.size()) * 0.95)) - 1, 0, values.size() - 1)
	return {"average": total / float(values.size()), "p95": values[percentile_index], "peak": peak}


func _update_problems(p_problems: Array[Terrain3DRuntimeValueMap]) -> void:
	var root: TreeItem = _problems_tree.get_root()
	if root == null:
		root = _problems_tree.create_item()
	var seen: Dictionary[String, bool] = {}
	for problem: Terrain3DRuntimeValueMap in p_problems:
		var code: String = problem.string_value("code", "unknown")
		seen[code] = true
		var item: TreeItem = _problem_items.get(code)
		if item == null:
			item = _problems_tree.create_item(root)
			_problem_items[code] = item
		item.set_text(0, problem.string_value("severity", "info").to_upper())
		item.set_text(1, problem.string_value("message", code))
		item.set_text(2, str(problem.int_value("value")))
		item.set_tooltip_text(1, "%s — %s" % [problem.string_value("subsystem"), code])
	for code: String in _problem_items:
		if not seen.has(code):
			var item: TreeItem = _problem_items[code]
			item.free()
			_problem_items.erase(code)


func _update_pressure(p_snapshot: Terrain3DRuntimeValueMap) -> void:
	var gpu: Terrain3DRuntimeValueMap = p_snapshot.map_value("gpu")
	var cpu: Terrain3DRuntimeValueMap = p_snapshot.map_value("cpu")
	var instance_cache: Terrain3DRuntimeValueMap = p_snapshot.map_value("instance_cache")
	var instancer: Terrain3DRuntimeValueMap = p_snapshot.map_value("instancer")
	var nodes: Terrain3DRuntimeValueMap = p_snapshot.map_value("nodes")
	_set_pressure("gpu_residency", "GPU mapped pages", gpu.float_value("resident"),
		gpu.float_value("capacity"), "gpu_missing_required")
	_set_pressure("gpu_upload_pages", "GPU upload pages / tick", gpu.float_value("uploads_last_tick"),
		gpu.float_value("effective_upload_limit_last_tick"), "gpu_missing_required")
	_set_pressure("gpu_upload_bytes", "GPU upload bytes", gpu.float_value("upload_bytes_last_tick"),
		gpu.float_value("upload_budget_mb") * 1048576.0, "gpu_upload_bytes", true)
	_set_pressure("gpu_prepare_backlog", "Prepared / pending pages",
		gpu.float_value("prepare_ready_pages") + gpu.float_value("prepare_job_pending_pages"),
		0.0, "gpu_prepare_pending_pages")
	_set_pressure("cpu_residency", "CPU resident pages", cpu.float_value("resident"),
		cpu.float_value("capacity"), "cpu_selection_pending")
	_set_pressure("cpu_load_pages", "CPU load pages / tick", cpu.float_value("loads_last_tick"),
		cpu.float_value("effective_load_layer_budget", cpu.float_value("load_layer_budget")), "cpu_selection_pending")
	_set_pressure("cpu_load_bytes", "CPU load bytes", cpu.float_value("load_bytes_last_tick"),
		cpu.float_value("load_budget_mb") * 1048576.0, "cpu_load_bytes", true)
	_set_pressure("instance_residency", "Instance resident pages", instance_cache.float_value("resident"),
		instance_cache.float_value("capacity"), "instance_selection_pending")
	_set_pressure("instance_load_pages", "Instance load pages", instance_cache.float_value("loads_last_tick"),
		instance_cache.float_value("effective_load_page_budget", instance_cache.float_value("load_page_budget")), "instance_selection_pending")
	_set_pressure("instance_updates", "Instance cell backlog", instancer.float_value("pending_updates_total"),
		instancer.float_value("effective_update_cell_budget", instancer.float_value("update_cell_budget")), "pending_cell_updates")
	_set_pressure("node_loads", "Node region loads", nodes.float_value("loads_last_tick"),
		nodes.float_value("effective_load_region_budget", nodes.float_value("load_region_budget")), "node_loads")
	_set_pressure("node_unloads", "Node region unloads", nodes.float_value("unloads_last_tick"),
		nodes.float_value("effective_unload_region_budget", nodes.float_value("unload_region_budget")), "node_unloads")


func _update_efficiency(p_snapshot: Terrain3DRuntimeValueMap) -> void:
	var root: TreeItem = _efficiency_tree.get_root()
	if root == null:
		root = _efficiency_tree.create_item()
	var gpu: Terrain3DRuntimeValueMap = p_snapshot.map_value("gpu")
	var memory: Terrain3DRuntimeValueMap = p_snapshot.map_value("memory")
	var aggregate_root: Terrain3DRuntimeValueMap = p_snapshot.map_value("aggregate")
	var aggregate: Terrain3DRuntimeValueMap = aggregate_root.map_value("gpu")
	var required_ratio: float = gpu.float_value("required_residency_ratio", 1.0)
	_add_efficiency_row(root, "Required residency", "%.1f%%" % (required_ratio * 100.0),
		"%d / %d pages" % [gpu.int_value("required_resident_last_tick"), gpu.int_value("required_last_tick")])
	var cooked_ratio: float = aggregate.float_value("cooked_hit_ratio", -1.0)
	_add_efficiency_row(root, "Cooked-page hit rate",
		"%.1f%%" % (cooked_ratio * 100.0) if cooked_ratio >= 0.0 else "No completed attempts",
		"%d hits / %d misses" % [aggregate.int_value("cooked_hits"), aggregate.int_value("cooked_misses")])
	var prefetch_accuracy: float = gpu.float_value("prefetch_accuracy", -1.0)
	_add_efficiency_row(root, "Prefetch accuracy",
		"%.1f%%" % (prefetch_accuracy * 100.0) if prefetch_accuracy >= 0.0 else "No completed prefetches",
		"%d used / %d discarded / %d pending" % [gpu.int_value("prefetch_used_total"),
			gpu.int_value("prefetch_discarded_total"), gpu.int_value("prefetch_pending")])
	var departed_pages: int = gpu.int_value("departed_pages_total")
	_add_efficiency_row(root, "Average GPU-page lifetime",
		"%.1f ticks" % gpu.float_value("average_resident_ticks") if departed_pages > 0 else "No departures",
		"%d departed pages since cache reset" % departed_pages)
	var distance_m: float = aggregate.float_value("distance_travelled_m")
	_add_efficiency_row(root, "Upload traffic", _format_bytes(aggregate.float_value("upload_bytes_per_m")) + "/m"
		if distance_m > 0.0 else "No movement", "%.1f m sampled" % distance_m)
	_add_efficiency_row(root, "Eviction rate", "%.2f / km" % aggregate.float_value("evictions_per_km")
		if distance_m > 0.0 else "No movement", "%d total evictions" % aggregate.int_value("evictions"))
	var fixed_memory: Terrain3DRuntimeValueMap = memory.map_value("fixed_unavoidable")
	var managed_memory: Terrain3DRuntimeValueMap = memory.map_value("managed_resident")
	_add_efficiency_row(root, "Terrain3D tracked memory", _format_bytes(memory.float_value("tracked_current_residency_bytes")),
		"Tracked current Terrain3D-owned memory; unavailable renderer/driver allocations remain explicit")
	_add_efficiency_row(root, "Terrain map CPU memory",
		_format_bytes(managed_memory.float_value("cpu_terrain_page_cache_bytes") +
			fixed_memory.float_value("repository_and_authoritative_storage_bytes")),
		"CPU page cache + authoritative repository/storage")
	_add_efficiency_row(root, "Terrain texture GPU memory",
		_format_bytes(managed_memory.float_value("gpu_terrain_page_pool_bytes") + fixed_memory.float_value("far_atlas_bytes") +
			fixed_memory.float_value("material_texture_arrays_bytes")), "managed terrain pages + fixed far atlas/material arrays")
	_add_efficiency_row(root, "Instance resident memory",
		_format_bytes(managed_memory.float_value("instance_region_page_bytes") +
			fixed_memory.float_value("active_multimesh_bytes") + fixed_memory.float_value("instancer_retained_bytes")),
		"instance pages + active RD instance data + retained instancer containers")


func _add_efficiency_row(p_root: TreeItem, p_metric: String, p_value: String, p_context: String) -> void:
	var item: TreeItem = _efficiency_items.get(p_metric)
	if item == null:
		item = _efficiency_tree.create_item(p_root)
		_efficiency_items[p_metric] = item
	item.set_text(0, p_metric)
	item.set_text(1, p_value)
	item.set_text(2, p_context)


func _set_pressure(p_key: String, p_label: String, p_current: float, p_limit: float,
		p_trend_key: String, p_bytes: bool = false) -> void:
	var root: TreeItem = _pressure_tree.get_root()
	if root == null:
		root = _pressure_tree.create_item()
	var item: TreeItem = _pressure_items.get(p_key)
	if item == null:
		item = _pressure_tree.create_item(root)
		_pressure_items[p_key] = item
	item.set_text(0, p_label)
	if p_bytes:
		item.set_text(1, _format_bytes(p_current))
		item.set_text(2, _format_bytes(p_limit) if p_limit > 0.0 else "—")
	else:
		item.set_text(1, str(int(p_current)))
		item.set_text(2, str(int(p_limit)) if p_limit > 0.0 else "—")
	var trend: String = _history_trend(p_trend_key)
	if p_limit > 0.0:
		var pressure: float = p_current / p_limit
		var pressure_text: String = "%.0f%%" % (pressure * 100.0)
		if pressure >= 1.0:
			pressure_text += " LIMITING" if trend == "growing" else " full"
		item.set_text(3, pressure_text)
	else:
		item.set_text(3, "queue")
	item.set_text(4, trend)


func _history_trend(p_key: String) -> String:
	if _history.size() < 6:
		return "—"
	var window: int = mini(30, _history.size() / 3)
	var first_total: float = 0.0
	var last_total: float = 0.0
	for index: int in range(window):
		var first_sample: Terrain3DRuntimeValueMap = _history[index]
		var last_sample: Terrain3DRuntimeValueMap = _history[_history.size() - window + index]
		first_total += first_sample.float_value(p_key)
		last_total += last_sample.float_value(p_key)
	var first_average: float = first_total / float(window)
	var last_average: float = last_total / float(window)
	var tolerance: float = maxf(1.0, absf(first_average) * 0.05)
	if last_average > first_average + tolerance:
		return "growing"
	if last_average < first_average - tolerance:
		return "falling"
	return "stable"


func _update_timeline(p_sequence: int) -> void:
	_history_graph.frame_threshold_usec = _freeze_threshold.value * 1000.0
	_history_graph.set_samples(_history)
	if p_sequence == _last_history_sequence:
		return
	_last_history_sequence = p_sequence
	var root: TreeItem = _history_tree.get_root()
	if root == null:
		root = _history_tree.create_item()
	var seen: Dictionary[int, bool] = {}
	for index: int in range(_history.size()):
		var sample: Terrain3DRuntimeValueMap = _history[index]
		var sequence: int = sample.int_value("sequence", index)
		seen[sequence] = true
		var item: TreeItem = _history_items.get(sequence)
		if item == null:
			item = _history_tree.create_item(root)
			_history_items[sequence] = item
		item.set_metadata(0, index)
		item.set_text(0, str(sequence))
		item.set_text(1, _format_usec(sample.float_value("physics_total_usec")))
		item.set_text(2, _format_usec(sample.float_value("data_residency_usec")))
		item.set_text(3, _format_usec(sample.float_value("clipmap_usec")))
		item.set_text(4, _format_usec(sample.float_value("instancer_usec")))
		var event_names: PackedStringArray = sample.string_array("events")
		var stall_names: PackedStringArray = sample.string_array("stall_reasons")
		item.set_text(5, ", ".join(event_names))
		item.set_text(6, ", ".join(stall_names))
	for sequence: int in _history_items:
		if not seen.has(sequence):
			var item: TreeItem = _history_items[sequence]
			item.free()
			_history_items.erase(sequence)
	if _selected_history_index < 0 and not _history.is_empty():
		_selected_history_index = _history.size() - 1
		_history_graph.select_latest()
		_render_history_detail(_selected_history_index)


func _on_history_graph_selected(p_index: int) -> void:
	_selected_history_index = p_index
	_render_history_detail(p_index)


func _on_history_tree_selected() -> void:
	var item: TreeItem = _history_tree.get_selected()
	if item == null:
		return
	_selected_history_index = int(item.get_metadata(0))
	_history_graph.selected_index = _selected_history_index
	_history_graph.queue_redraw()
	_render_history_detail(_selected_history_index)


func _render_history_detail(p_index: int) -> void:
	_history_detail.clear()
	var root: TreeItem = _history_detail.create_item()
	if p_index < 0 or p_index >= _history.size():
		return
	var sample: Terrain3DRuntimeValueMap = _history[p_index]
	for key: String in sample.keys():
		var item: TreeItem = _history_detail.create_item(root)
		item.set_text(0, _humanize_name(key))
		item.set_text(1, sample.formatted_value(key))


func _update_spatial(p_snapshot: Terrain3DRuntimeValueMap) -> void:
	_residency_grid.set_grid(_grid_snapshot)
	var gpu: Terrain3DRuntimeValueMap = p_snapshot.map_value("gpu")
	var requests: Array[Terrain3DRuntimeValueMap] = gpu.map_array("pending_requests")
	_priority_summary.text = "Active %d  •  Hot %d  •  Cold %d  •  Unloaded %d  •  Predicted %d\nPending residency requests: %d total, %d shown" % [
		_grid_snapshot.int_value("active_count"), _grid_snapshot.int_value("hot_count"),
		_grid_snapshot.int_value("cold_count"), _grid_snapshot.int_value("unloaded_count"),
		_grid_snapshot.int_value("predicted_count"),
		gpu.int_value("pending_requests_total", requests.size()), requests.size()]
	var root: TreeItem = _priority_tree.get_root()
	if root == null:
		root = _priority_tree.create_item()
	for index: int in range(16):
		var item: TreeItem = _priority_items.get(index)
		if index >= requests.size():
			if item != null:
				item.set_visible(false)
			continue
		if item == null:
			item = _priority_tree.create_item(root)
			_priority_items[index] = item
		item.set_visible(true)
		var request: Terrain3DRuntimeValueMap = requests[index]
		item.set_text(0, str(request.int_value("priority_rank", index)))
		item.set_text(1, request.string_value("class", "unknown"))
		item.set_text(2, str(request.vector2i_value("location")))
		var state: String = "waiting"
		if request.bool_value("prepared"):
			state = "ready"
		elif request.bool_value("prepare_pending"):
			state = "preparing"
		elif not request.bool_value("active", true):
			state = "inactive"
		elif not request.bool_value("runtime_maps_evictable"):
			state = "live maps"
		item.set_text(3, state)
		item.set_text(4, "%d r" % request.int_value("distance_regions"))
		item.set_text(5, str(request.int_value("motion_alignment")))


func _on_residency_cell_selected(p_cell: Terrain3DRuntimeValueMap) -> void:
	var lines: PackedStringArray = PackedStringArray()
	lines.append("[b]Region %s[/b]" % str(p_cell.vector2i_value("location")))
	for key: String in p_cell.keys():
		lines.append("%s: %s" % [_humanize_name(key), p_cell.formatted_value(key)])
	_residency_detail.text = "\n".join(lines)


func _update_settings(p_snapshot: Terrain3DRuntimeValueMap) -> void:
	_updating_controls = true
	for property_name: String in _control_bindings:
		SettingControlFactory.sync_control(_control_bindings[property_name], _control_descriptors[property_name], terrain.get(property_name))
	_updating_controls = false
	var clipmap: Terrain3DRuntimeValueMap = p_snapshot.map_value("clipmap")
	var configuration: Terrain3DRuntimeValueMap = p_snapshot.map_value("configuration")
	_range_summary.text = "Near %.0f m (%d rings)  •  Detail %.0f m (%d rings, %d/%d pages)  •  Middle %.0f m (%d rings)" % [
		clipmap.float_value("near_range_end_m"), clipmap.int_value("near_lods"),
		float(terrain.gpu_region_detail_range_end_m), configuration.int_value("gpu_detail_lods"),
		configuration.int_value("gpu_required_core_pages"), configuration.int_value("gpu_mapped_capacity"),
		clipmap.float_value("middle_range_end_m"), clipmap.int_value("middle_lods"),
	]
	_update_adaptive_settings(p_snapshot)
	_update_boundary_phasing(p_snapshot)
	_update_validation(p_snapshot)
	_update_profile_diff()


func _update_adaptive_settings(p_snapshot: Terrain3DRuntimeValueMap) -> void:
	var adaptive: Terrain3DRuntimeValueMap = p_snapshot.map_value("adaptive_budgets")
	var enabled: bool = adaptive.bool_value("enabled")
	var measured_ms: float = adaptive.float_value("last_measured_usec") / 1000.0
	var target_ms: float = adaptive.float_value("target_usec") / 1000.0
	var hard_ms: float = adaptive.float_value("hard_max_usec") / 1000.0
	_adaptive_summary.text = "%s  •  measured %.3f ms  •  target %.3f ms  •  hard %.3f ms  •  reason %s  •  controller %.3f ms  •  adjustments %d" % [
		"ACTIVE" if enabled else "DISABLED", measured_ms, target_ms, hard_ms,
		adaptive.string_value("reason", "disabled"), adaptive.float_value("controller_usec_last") / 1000.0,
		adaptive.int_value("adjustment_count"),
	]
	var root: TreeItem = _adaptive_tree.get_root()
	if root == null:
		root = _adaptive_tree.create_item()
	var effective: Terrain3DRuntimeValueMap = adaptive.map_value("effective")
	var minimum: Terrain3DRuntimeValueMap = adaptive.map_value("minimum")
	var maximum: Terrain3DRuntimeValueMap = adaptive.map_value("configured_maximum")
	var limited: Terrain3DRuntimeValueMap = adaptive.map_value("limited")
	var adaptive_rows: Array[PackedStringArray] = [
		PackedStringArray(["gpu_pages", "GPU page publication", "gpu"]),
		PackedStringArray(["cpu_pages", "CPU page loads", "cpu"]),
		PackedStringArray(["instance_pages", "Instance page loads", "instance"]),
		PackedStringArray(["instancer_cells", "Instance cells", "instancer"]),
		PackedStringArray(["node_load_regions", "Node region loads", "node_load"]),
		PackedStringArray(["node_unload_regions", "Node region unloads", "node_unload"]),
	]
	for row: PackedStringArray in adaptive_rows:
		var key: String = row[0]
		var item: TreeItem = _adaptive_items.get(key)
		if item == null:
			item = _adaptive_tree.create_item(root)
			_adaptive_items[key] = item
		var effective_value: int = effective.int_value(key)
		var minimum_value: int = minimum.int_value(key)
		var maximum_value: int = maximum.int_value(key)
		var is_limited: bool = limited.bool_value(row[2])
		item.set_text(0, row[1])
		item.set_text(1, str(effective_value))
		item.set_text(2, str(minimum_value))
		item.set_text(3, str(maximum_value))
		if not enabled:
			item.set_text(4, "fixed")
		elif is_limited:
			item.set_text(4, "LIMITED")
		elif effective_value <= minimum_value:
			item.set_text(4, "minimum")
		elif effective_value >= maximum_value:
			item.set_text(4, "maximum")
		else:
			item.set_text(4, "adaptive")


func _update_boundary_phasing(p_snapshot: Terrain3DRuntimeValueMap) -> void:
	var phasing: Terrain3DRuntimeValueMap = p_snapshot.map_value("boundary_phasing")
	var deferred: PackedStringArray = PackedStringArray()
	if phasing.bool_value("node_deferred_last_tick"):
		deferred.append("nodes")
	if phasing.bool_value("instancer_deferred_last_tick"):
		deferred.append("instances")
	var state: String = "none" if deferred.is_empty() else ", ".join(deferred)
	_phasing_summary.text = "%s  •  region %s  •  boundary age %d frames  •  deferred now: %s  •  totals nodes %d / instances %d" % [
		"ACTIVE" if phasing.bool_value("enabled") else "DISABLED",
		str(phasing.vector2i_value("target_region")), phasing.int_value("frames_since_boundary"), state,
		phasing.int_value("node_deferrals_total"), phasing.int_value("instancer_deferrals_total"),
	]


func _update_validation(p_snapshot: Terrain3DRuntimeValueMap) -> void:
	var configuration: Terrain3DRuntimeValueMap = p_snapshot.map_value("configuration")
	var gpu: Terrain3DRuntimeValueMap = p_snapshot.map_value("gpu")
	var cpu: Terrain3DRuntimeValueMap = p_snapshot.map_value("cpu")
	var lines: PackedStringArray = PackedStringArray()
	if terrain.get_performance_profile_status_name() != "Custom" and \
			terrain.editor_execution_mode == Terrain3D.EDITOR_EXECUTION_AUTHORING:
		lines.append("[color=yellow]The selected performance profile is in Authoring mode: editor-safe Near rendering and pinned object/source data are active. Switch Editor Execution Mode to Runtime Profile for the shipping Near/Middle/Far and streaming comparison.[/color]")
	if not configuration.bool_value("gpu_detail_range_valid", true):
		lines.append("[color=red]Detailed footprint exceeds mapped GPU capacity.[/color]")
	if gpu.bool_value("budget_overcommitted"):
		lines.append("[color=red]GPU cache allocation exceeds its configured memory budget.[/color]")
	if cpu.bool_value("budget_overcommitted"):
		lines.append("[color=orange]CPU cache capacity exceeds its configured memory budget.[/color]")
	if gpu.bool_value("cooked_pages_enabled") and gpu.int_value("cooked_misses_last_tick") > 0:
		lines.append("[color=orange]Cooked pages missed this tick; runtime fallback was used.[/color]")
	if gpu.int_value("far_height_dirty_regions") + gpu.int_value("far_color_dirty_regions") > 0:
		lines.append("Far-atlas patches remain dirty.")
	var adaptive: Terrain3DRuntimeValueMap = p_snapshot.map_value("adaptive_budgets")
	if adaptive.bool_value("enabled"):
		var requested_minimum: Terrain3DRuntimeValueMap = adaptive.map_value("requested_minimum")
		var effective_minimum: Terrain3DRuntimeValueMap = adaptive.map_value("minimum")
		for key: String in requested_minimum.keys():
			if requested_minimum.int_value(key) != effective_minimum.int_value(key):
				lines.append("[color=orange]%s minimum is clamped to its configured maximum.[/color]" % _humanize_name(key))
		if int(terrain.instancer_update_cell_budget) == 0:
			lines.append("Instancer ceiling 0 means unlimited in fixed mode; adaptive mode uses 65536 as its finite ceiling.")
		var effective: Terrain3DRuntimeValueMap = adaptive.map_value("effective")
		var all_at_minimum: bool = true
		for key: String in effective.keys():
			if effective.int_value(key) > effective_minimum.int_value(key):
				all_at_minimum = false
				break
		if all_at_minimum and adaptive.int_value("last_measured_usec") > adaptive.int_value("hard_max_usec"):
			lines.append("[color=red]Terrain time exceeds the hard maximum while every adaptive budget is already at minimum.[/color]")
	if lines.is_empty():
		lines.append("[color=green]No active configuration faults detected.[/color]")
	_validation.text = "\n".join(lines)


func _update_profile_diff() -> void:
	_profile_diff_tree.clear()
	var root: TreeItem = _profile_diff_tree.create_item()
	var manifest: Dictionary = terrain.get_performance_profile_manifest()
	_profile_diff_tree.set_column_title(2, str(manifest.get("display_name", "Profile")))
	var differences: Array = terrain.get_performance_profile_diff()
	if differences.is_empty():
		var status_item: TreeItem = _profile_diff_tree.create_item(root)
		status_item.set_text(0, "Profile status")
		status_item.set_text(1, terrain.get_performance_profile_status_name())
		status_item.set_text(2, "Exact")
		return
	for difference: Dictionary in differences:
		var item: TreeItem = _profile_diff_tree.create_item(root)
		var setting_key: String = str(difference.get("key", difference.get("property", "")))
		item.set_text(0, setting_key)
		item.set_tooltip_text(0, SettingMetadata.tooltip_for_key(setting_key))
		item.set_text(1, str(difference.get("current", null)))
		item.set_text(2, str(difference.get("expected", null)))


func _reset_selected_profile() -> void:
	if not is_instance_valid(terrain):
		return
	terrain.reset_performance_profile()
	refresh_now()


func _fit_detail_range_to_capacity() -> void:
	if not is_instance_valid(terrain):
		return
	terrain.gpu_region_detail_range_end_m = terrain.gpu_region_max_supported_detail_range_m
	refresh_now()


func _evaluate_auto_freeze(p_frame: Terrain3DRuntimeValueMap) -> void:
	if not _auto_freeze.button_pressed or _pause.button_pressed:
		return
	var sequence: int = p_frame.int_value("sequence")
	if _freeze_trigger_sequence < 0:
		var triggered: bool = p_frame.float_value("physics_total_usec") >= _freeze_threshold.value * 1000.0
		var frame_events: PackedStringArray = p_frame.string_array("events")
		if _freeze_on_missing.button_pressed and frame_events.has("gpu_missing_required"):
			triggered = true
		if _freeze_on_failure.button_pressed and frame_events.has("node_load_failure"):
			triggered = true
		if _freeze_on_shader.button_pressed and frame_events.has("shader_update"):
			triggered = true
		if triggered:
			_freeze_trigger_sequence = sequence
			_freeze_target_sequence = sequence + int(_post_trigger_samples.value)
	elif sequence >= _freeze_target_sequence:
		_pause.button_pressed = true
		_status.text += " — auto-frozen after trigger frame %d" % _freeze_trigger_sequence
		_freeze_trigger_sequence = -1
		_freeze_target_sequence = -1


func _update_raw_tree(p_snapshot: Terrain3DRuntimeValueMap) -> void:
	var root: TreeItem = _raw_tree.get_root()
	if root == null:
		root = _raw_tree.create_item()
	var seen: Dictionary[String, bool] = {}
	_update_raw_value(root, "root", "Diagnostics", p_snapshot.duplicate_values(), seen)
	for path: String in _raw_items:
		if not seen.has(path):
			var item: TreeItem = _raw_items[path]
			item.set_text(1, "stale")


func _update_subsystems(p_snapshot: Terrain3DRuntimeValueMap) -> void:
	var root: TreeItem = _subsystem_tree.get_root()
	if root == null:
		root = _subsystem_tree.create_item()
	var seen: Dictionary[String, bool] = {"subsystems": true}
	var group: TreeItem = _subsystem_items.get("subsystems")
	if group == null:
		group = _subsystem_tree.create_item(root)
		_subsystem_items["subsystems"] = group
	group.set_text(0, "Subsystems")
	var subsystem_keys: PackedStringArray = PackedStringArray(["gpu", "cpu", "instance_cache", "instancer", "nodes", "collision", "shaders", "clipmap"])
	for key: String in subsystem_keys:
		if p_snapshot.has(key):
			_update_subsystem_value(group, "subsystems.%s" % key, key, p_snapshot.raw_value(key), seen)
	for path: String in _subsystem_items:
		if not seen.has(path):
			var item: TreeItem = _subsystem_items[path]
			item.set_text(1, "stale")


func _update_subsystem_value(p_parent: TreeItem, p_path: String, p_name: String,
		p_value: Variant, p_seen: Dictionary[String, bool]) -> void:
	p_seen[p_path] = true
	var item: TreeItem = _subsystem_items.get(p_path)
	if item == null:
		item = _subsystem_tree.create_item(p_parent)
		_subsystem_items[p_path] = item
	item.set_text(0, _humanize_name(p_name))
	item.set_tooltip_text(0, p_path)
	if p_value is Dictionary:
		var map: Terrain3DRuntimeValueMap = Terrain3DRuntimeValueMap.from_value(p_value)
		var keys: PackedStringArray = map.keys()
		keys.sort()
		item.set_text(1, "%d entries" % keys.size())
		for key: String in keys:
			_update_subsystem_value(item, "%s.%s" % [p_path, key], key, map.raw_value(key), p_seen)
	else:
		item.set_text(1, Terrain3DRuntimeValueMap.format_external_value(p_name, p_value))
		item.set_text(2, str(p_value))


func _update_raw_value(p_parent: TreeItem, p_path: String, p_name: String, p_value: Variant, p_seen: Dictionary[String, bool]) -> void:
	p_seen[p_path] = true
	var item: TreeItem = _raw_items.get(p_path)
	if item == null:
		item = _raw_tree.create_item(p_parent)
		_raw_items[p_path] = item
	item.set_text(0, _humanize_name(p_name))
	item.set_tooltip_text(0, p_path)
	if p_value is Dictionary:
		var map: Terrain3DRuntimeValueMap = Terrain3DRuntimeValueMap.from_value(p_value)
		var keys: PackedStringArray = map.keys()
		keys.sort()
		item.set_text(1, "%d entries" % keys.size())
		for key: String in keys:
			_update_raw_value(item, "%s.%s" % [p_path, key], key, map.raw_value(key), p_seen)
	elif p_value is Array:
		item.set_text(1, "%d entries" % p_value.size())
		for index: int in range(p_value.size()):
			_update_raw_value(item, "%s[%d]" % [p_path, index], "[%d]" % index, p_value[index], p_seen)
	else:
		item.set_text(1, Terrain3DRuntimeValueMap.format_external_value(p_name, p_value))
		item.set_text(2, str(p_value))


func _on_refresh_rate_selected(p_index: int) -> void:
	var rate: int = _refresh_rate.get_item_id(p_index)
	_refresh_interval_sec = 1.0 / float(maxi(rate, 1))


func _reset_counters() -> void:
	if is_instance_valid(terrain):
		terrain.reset_runtime_diagnostics()
	_reset_ui_state()
	refresh_now()


func _reset_ui_state() -> void:
	_history_items.clear()
	_raw_items.clear()
	_problem_items.clear()
	_pressure_items.clear()
	_efficiency_items.clear()
	_adaptive_items.clear()
	_priority_items.clear()
	_subsystem_items.clear()
	_last_history_sequence = -1
	_history_fetch_sequence = -1
	_selected_history_index = -1
	_freeze_trigger_sequence = -1
	_freeze_target_sequence = -1
	if is_instance_valid(_history_tree):
		_history_tree.clear()
	if is_instance_valid(_history_detail):
		_history_detail.clear()
	if is_instance_valid(_raw_tree):
		_raw_tree.clear()
	if is_instance_valid(_problems_tree):
		_problems_tree.clear()
	if is_instance_valid(_pressure_tree):
		_pressure_tree.clear()
	if is_instance_valid(_efficiency_tree):
		_efficiency_tree.clear()
	if is_instance_valid(_adaptive_tree):
		_adaptive_tree.clear()
	if is_instance_valid(_priority_tree):
		_priority_tree.clear()
	if is_instance_valid(_subsystem_tree):
		_subsystem_tree.clear()


func _show_export_dialog() -> void:
	refresh_now()
	_export_dialog.popup_centered_ratio(0.6)


func _write_snapshot(p_path: String) -> void:
	var file: FileAccess = FileAccess.open(p_path, FileAccess.WRITE)
	if not file:
		push_error("Unable to write Terrain3D diagnostics: %s" % p_path)
		return
	file.store_string(JSON.stringify({
		"snapshot": _last_snapshot.duplicate_values(),
		"history": Terrain3DRuntimeValueMap.values_from_maps(_history),
		"residency_grid": _grid_snapshot.duplicate_values(),
	}, "\t"))
	file.close()


func _get_engine_monitors() -> Dictionary[String, float]:
	return {
		"fps": Performance.get_monitor(Performance.TIME_FPS),
		"process_seconds": Performance.get_monitor(Performance.TIME_PROCESS),
		"physics_process_seconds": Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS),
		"navigation_process_seconds": Performance.get_monitor(Performance.TIME_NAVIGATION_PROCESS),
		"static_memory_bytes": Performance.get_monitor(Performance.MEMORY_STATIC),
		"static_memory_peak_bytes": Performance.get_monitor(Performance.MEMORY_STATIC_MAX),
		"object_count": Performance.get_monitor(Performance.OBJECT_COUNT),
		"resource_count": Performance.get_monitor(Performance.OBJECT_RESOURCE_COUNT),
		"node_count": Performance.get_monitor(Performance.OBJECT_NODE_COUNT),
		"orphan_node_count": Performance.get_monitor(Performance.OBJECT_ORPHAN_NODE_COUNT),
		"render_objects": Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME),
		"render_primitives": Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME),
		"draw_calls": Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME),
		"video_memory_bytes": Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED),
		"texture_memory_bytes": Performance.get_monitor(Performance.RENDER_TEXTURE_MEM_USED),
		"buffer_memory_bytes": Performance.get_monitor(Performance.RENDER_BUFFER_MEM_USED),
		"physics_3d_active_objects": Performance.get_monitor(Performance.PHYSICS_3D_ACTIVE_OBJECTS),
		"physics_3d_collision_pairs": Performance.get_monitor(Performance.PHYSICS_3D_COLLISION_PAIRS),
		"physics_3d_islands": Performance.get_monitor(Performance.PHYSICS_3D_ISLAND_COUNT),
	}


func _format_usec(p_usec: float) -> String:
	return "%.3f ms" % (p_usec / 1000.0)


func _format_bytes(p_bytes: float) -> String:
	if p_bytes >= 1073741824.0:
		return "%.2f GiB" % (p_bytes / 1073741824.0)
	if p_bytes >= 1048576.0:
		return "%.2f MiB" % (p_bytes / 1048576.0)
	if p_bytes >= 1024.0:
		return "%.2f KiB" % (p_bytes / 1024.0)
	return "%.0f B" % p_bytes


func _humanize_name(p_name: String) -> String:
	return p_name.replace("_", " ").capitalize()


func _make_label(p_text: String) -> Label:
	var label: Label = Label.new()
	label.text = p_text
	return label


func _section_label(p_text: String) -> Label:
	var label: Label = Label.new()
	label.text = p_text
	label.add_theme_font_size_override("font_size", 18)
	return label
