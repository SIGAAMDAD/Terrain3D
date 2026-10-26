# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Menu for Terrain3D.
#
# This control intentionally remains independent from optional editor tools. A parse or
# initialization failure in the erosion workflow must never prevent Terrain3D's primary
# toolbar menu from loading.
@tool
extends MenuButton


const DirectoryWizard = preload("res://addons/terrain_3d/menu/directory_setup.gd")
const ChannelPacker = preload("res://addons/terrain_3d/menu/channel_packer.gd")
const LodBaker = preload("res://addons/terrain_3d/menu/baker.gd")
const TERRAIN_ICON: Texture2D = preload("res://addons/terrain_3d/icons/terrain3d.svg")

const MENU_LABEL: String = "Terrain3D"
const MENU_TOOLTIP: String = "Terrain3D tools and utilities"

var plugin: Terrain3DEditorPluginBase
var directory_setup: DirectoryWizard = DirectoryWizard.new()
var packer: ChannelPacker = ChannelPacker.new()
var baker: LodBaker = LodBaker.new()
var _runtime_page_cook_confirm: ConfirmationDialog
var _runtime_page_cook_result: AcceptDialog
var _runtime_page_cook_text: TextEdit
var _vt_rebake_timer: Timer
var _vt_rebake_terrain: Terrain3D
var _vt_rebake_result: AcceptDialog

# These are IDs and order must remain consistent with add_item and set_disabled calls.
enum {
	MENU_DIRECTORY_SETUP,
	MENU_REGION_LIGHTMAP,
	MENU_BAKE_CHANGED_VIRTUAL_TEXTURES,
	MENU_BAKE_VIRTUAL_TEXTURES,
	MENU_SPARSE_RASTER_STATS,
	MENU_HLOD,
	MENU_COOK_RUNTIME_DATASET,
	MENU_PACK_TEXTURES,
	MENU_CONVERT_STREAMING,
	MENU_SEPARATOR,
	MENU_BAKE_ARRAY_MESH,
	MENU_BAKE_OCCLUDER,
	MENU_SIMULATE_EROSION,
	MENU_AUTOSHADER_BAKER,
	MENU_FOLIAGE_GENERATOR,
	MENU_SHADER_QUALITY,
	MENU_SEPARATOR2,
	MENU_SET_UP_NAVIGATION,
	MENU_BAKE_NAV_MESH,
	MENU_SEPARATOR3,
	MENU_BRUSH_DIRECTORY,
	MENU_TERRAIN_SETTINGS,
}


func _enter_tree() -> void:
	name = "Terrain3DMenu"
	text = MENU_LABEL
	icon = TERRAIN_ICON
	tooltip_text = MENU_TOOLTIP
	alignment = HORIZONTAL_ALIGNMENT_LEFT
	icon_alignment = HORIZONTAL_ALIGNMENT_LEFT
	clip_text = false
	expand_icon = false
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	# The icon remains usable even when the 3D toolbar is crowded. The full label is
	# normally retained, while this floor prevents the button from collapsing to a
	# nearly zero-width strip.
	custom_minimum_size = Vector2(40.0, 0.0)

	directory_setup.plugin = plugin
	packer.plugin = plugin
	baker.plugin = plugin
	add_child(directory_setup)
	add_child(baker)

	var popup: PopupMenu = get_popup()
	popup.add_item("Data Directory…", MENU_DIRECTORY_SETUP)
	popup.add_item("Bake Region Lightmaps…", MENU_REGION_LIGHTMAP)
	popup.add_item("Bake Changed Virtual Textures", MENU_BAKE_CHANGED_VIRTUAL_TEXTURES)
	popup.add_item("Rebuild All Virtual Textures", MENU_BAKE_VIRTUAL_TEXTURES)
	popup.add_item("Sparse Raster Statistics…", MENU_SPARSE_RASTER_STATS)
	popup.add_item("Hierarchical LOD…", MENU_HLOD)
	popup.add_item("Cook Runtime Dataset…", MENU_COOK_RUNTIME_DATASET)
	popup.add_item("Channel Packer…", MENU_PACK_TEXTURES)
	popup.add_item("Convert Selection to Region Streaming…", MENU_CONVERT_STREAMING)
	popup.add_separator("", MENU_SEPARATOR)
	popup.add_item("Bake Terrain Mesh…", MENU_BAKE_ARRAY_MESH)
	popup.add_item("Bake Terrain Occluder…", MENU_BAKE_OCCLUDER)
	popup.add_item("Open Erosion Simulator…", MENU_SIMULATE_EROSION)
	popup.add_item("Open Editor Autoshader Baker…", MENU_AUTOSHADER_BAKER)
	popup.add_item("Foliage Generation…", MENU_FOLIAGE_GENERATOR)
	popup.add_item("Runtime Shader Quality…", MENU_SHADER_QUALITY)
	popup.add_item("Terrain Settings…", MENU_TERRAIN_SETTINGS)
	popup.add_separator("", MENU_SEPARATOR2)
	popup.add_item("Set Up Navigation…", MENU_SET_UP_NAVIGATION)
	popup.add_item("Bake Navigation Mesh…", MENU_BAKE_NAV_MESH)
	popup.add_separator("", MENU_SEPARATOR3)
	popup.add_item("Open Brush Directory…", MENU_BRUSH_DIRECTORY)

	_set_item_tooltip(popup, MENU_DIRECTORY_SETUP, "Choose the directory containing Terrain3D region resources.")
	_set_item_tooltip(popup, MENU_REGION_LIGHTMAP, "Bake independent lightmaps for every authored Terrain3D region in the active data directory.")
	_set_item_tooltip(popup, MENU_BAKE_CHANGED_VIRTUAL_TEXTURES, "Cook changed or missing fragments; reuse clean region caches and publish once.")
	_set_item_tooltip(popup, MENU_SPARSE_RASTER_STATS, "Inspect sparse terrain tile residency, raw bytes, and cooked bundle status.")
	_set_item_tooltip(popup, MENU_BAKE_VIRTUAL_TEXTURES, "Bypass caches, rebuild every VT material and MegaVT fragment, republish the preview, and save terrain3d_instances.t3dvt.")
	_set_item_tooltip(popup, MENU_HLOD, "Analyze, bake, validate, and preview Terrain3D hierarchical LOD proxies.")
	_set_item_tooltip(popup, MENU_COOK_RUNTIME_DATASET, "Build and validate the aggregate terrain_stream.t3sp and .t3pm package from authored .res regions.")
	_set_item_tooltip(popup, MENU_PACK_TEXTURES, "Pack albedo/height and normal/roughness textures.")
	_set_item_tooltip(popup, MENU_CONVERT_STREAMING, "Convert selected scene objects into streaming region descriptors.")
	_set_item_tooltip(popup, MENU_BAKE_ARRAY_MESH, "Create a MeshInstance3D from terrain geometry.")
	_set_item_tooltip(popup, MENU_BAKE_OCCLUDER, "Create a coarse OccluderInstance3D from terrain geometry.")
	_set_item_tooltip(popup, MENU_SIMULATE_EROSION, "Open the resizable Terrain3D erosion workspace.")
	_set_item_tooltip(popup, MENU_AUTOSHADER_BAKER, "Author expanded terrain material rules and bake them into control maps.")
	_set_item_tooltip(popup, MENU_FOLIAGE_GENERATOR, "Explicitly bake material-driven foliage into existing streamed instance records.")
	_set_item_tooltip(popup, MENU_SHADER_QUALITY, "Open the existing runtime terrain and ocean shader quality panel.")
	_set_item_tooltip(popup, MENU_TERRAIN_SETTINGS, "Open the Terrain3D streaming, bake, viewport, and diagnostic settings window.")
	_set_item_tooltip(popup, MENU_SET_UP_NAVIGATION, "Create and configure a NavigationRegion3D for the selected terrain.")
	_set_item_tooltip(popup, MENU_BAKE_NAV_MESH, "Rebuild navigation geometry for the active terrain or navigation region.")
	_set_item_tooltip(popup, MENU_BRUSH_DIRECTORY, "Open the optional custom EXR brush folder. It is created on demand; built-in brushes require no files.")

	popup.id_pressed.connect(_on_menu_pressed)
	about_to_popup.connect(_on_menu_about_to_popup)


func _set_item_tooltip(p_popup: PopupMenu, p_id: int, p_tooltip: String) -> void:
	var index: int = p_popup.get_item_index(p_id)
	if index >= 0:
		p_popup.set_item_tooltip(index, p_tooltip)


func open_menu() -> void:
	# This is also used by the Editor > Tools fallback entry. Do not depend on the
	# toolbar having enough width to receive a mouse click.
	show_popup()


func _on_menu_pressed(p_id: int) -> void:
	match p_id:
		MENU_DIRECTORY_SETUP:
			directory_setup.directory_setup_popup()
		MENU_REGION_LIGHTMAP:
			plugin.open_region_lightmap_baker()
		MENU_BAKE_VIRTUAL_TEXTURES:
			_bake_virtual_textures(true)
		MENU_BAKE_CHANGED_VIRTUAL_TEXTURES:
			_bake_virtual_textures(false)
		MENU_SPARSE_RASTER_STATS:
			_show_sparse_raster_stats()
		MENU_HLOD:
			plugin.open_hlod_panel()
		MENU_COOK_RUNTIME_DATASET:
			_open_runtime_page_cook_confirmation()
		MENU_PACK_TEXTURES:
			packer.pack_textures_popup()
		MENU_CONVERT_STREAMING:
			plugin.convert_selected_to_region_streaming()
		MENU_BAKE_ARRAY_MESH:
			baker.bake_mesh_popup()
		MENU_BAKE_OCCLUDER:
			baker.bake_occluder_popup()
		MENU_SIMULATE_EROSION:
			plugin.open_erosion_simulator()
		MENU_AUTOSHADER_BAKER:
			plugin.open_autoshader_baker()
		MENU_FOLIAGE_GENERATOR:
			plugin.open_foliage_generator()
		MENU_SHADER_QUALITY:
			plugin.open_shader_performance()
		MENU_TERRAIN_SETTINGS:
			plugin.open_terrain_settings()
		MENU_SET_UP_NAVIGATION:
			baker.set_up_navigation_popup()
		MENU_BAKE_NAV_MESH:
			baker.bake_nav_mesh()
		MENU_BRUSH_DIRECTORY:
			var brush_path: String = ProjectSettings.globalize_path("res://addons/terrain_3d/brushes")
			DirAccess.make_dir_recursive_absolute(brush_path)
			OS.shell_show_in_file_manager(brush_path)


func _show_sparse_raster_stats() -> void:
	if not is_instance_valid(plugin) or not is_instance_valid(plugin.terrain):
		return
	var data: Terrain3DData = plugin.terrain.data
	if not is_instance_valid(data):
		return
	var stats: Dictionary = data.get_sparse_raster_stats()
	var lines: PackedStringArray = []
	# Startup completion only proves that resources were published. Include the
	# native pass and World counters so a blank viewport can be diagnosed here.
	var rd_stats: Dictionary = plugin.terrain.get_rd_renderer_diagnostics()
	var world: Dictionary = rd_stats.get("world_renderer", {})
	var baselines: Dictionary = rd_stats.get("uwr_p00_draw_baseline", {})
	var opaque: Dictionary = baselines.get("opaque", {})
	lines.append("--- Forward+ World rendering ---")
	lines.append("render_enabled: %s" % str(rd_stats.get("render_enabled", false)))
	lines.append("renderer_failure_latched: %s" % str(rd_stats.get("renderer_failure_latched", false)))
	lines.append("forward_pass_callbacks: %d" % int(rd_stats.get("forward_pass_callbacks", 0)))
	lines.append("bridge_ready: %s" % str(rd_stats.get("forward_plus_bridge_ready", false)))
	lines.append("callback_registered: %s" % str(rd_stats.get("forward_plus_callback_registered", false)))
	lines.append("native_opaque_callbacks: %d" % int(rd_stats.get("opaque_pass_callbacks", 0)))
	lines.append("world_ready: %s" % str(world.get("ready", false)))
	lines.append("world_publication_generation: %d" % int(world.get("publication_generation", 0)))
	lines.append("world_visibility_candidates: %d" % int(world.get("visibility_candidates", 0)))
	lines.append("world_visibility_prepares: %d" % int(world.get("visibility_prepares", 0)))
	lines.append("world_raster_prepares: %d" % int(world.get("raster_prepares", 0)))
	lines.append("world_opaque_submits: %d" % int(world.get("draw_calls_opaque", 0)))
	lines.append("world_opaque_draws: %d" % int(opaque.get("world_draws", 0)))
	lines.append("terrain_prepare_failures: %d" % int(rd_stats.get("terrain_recoverable_prepare_failures", 0)))
	lines.append("world_prepare_failures: %d" % int(world.get("prepare_failures", 0)))
	lines.append("world_visibility_failures: %d" % int(world.get("visibility_failures", 0)))
	lines.append("world_prepare_failure_pass: %s" % str(rd_stats.get("world_prepare_failure_pass", "unknown")))
	lines.append("world_prepare_failure_reason: %s" % str(rd_stats.get("world_prepare_failure_reason", "unknown")))
	lines.append("shared_vt_class: %s (0=none, 1=preview, 2=production)" % str(rd_stats.get("shared_resource_class", -1)))
	for vt_key: String in ["shared_vt_resources_ready", "shared_vt_cpu_ready", "shared_vt_gpu_storage_ready", "shared_vt_gpu_executable_ready", "shared_vt_initial_published", "shared_vt_runtime_initialized", "shared_vt_warm_package_pending", "shared_vt_resource_rebuild_required", "shared_vt_preview_readback_pending"]:
		lines.append("%s: %s" % [vt_key, str(rd_stats.get(vt_key, "unknown"))])
	lines.append("clipmap_resource_ready: %s" % str(rd_stats.get("clipmap_resource_ready", false)))
	lines.append("clipmap_source_last_error: %s" % str(rd_stats.get("clipmap_source_last_error", "")))
	lines.append("clipmap_forward_prepare_failure_reason: %s" % str(rd_stats.get("clipmap_forward_prepare_failure_reason", "unknown")))
	lines.append("cpu_clipmap_patch_count: %d" % int(rd_stats.get("cpu_clipmap_patch_count", 0)))
	lines.append("last_render_error: %s" % str(rd_stats.get("last_render_error", "")))
	lines.append("initialization_error: %s" % str(rd_stats.get("initialization_error", "")))
	lines.append("")
	lines.append("--- Sparse raster storage ---")
	var keys: Array = stats.keys()
	keys.sort()
	for key: Variant in keys:
		lines.append("%s: %s" % [str(key), str(stats[key])])
	# Keep the GPU's physical and logical footprints visible together. An
	# occupied far-color slot is not a 4096-square full-world allocation.
	var gpu: Dictionary = data.get_gpu_region_cache_stats()
	lines.append("")
	lines.append("--- GPU far-color residency ---")
	for key: String in ["far_color_physical_regions", "far_color_fine_page_capacity", "far_color_coarse_regions", "far_color_physical_atlas_side", "far_color_physical_atlas_height", "far_color_physical_gpu_bytes", "far_color_logical_full_gpu_bytes", "far_color_layout_generation", "far_color_fine_admissions", "far_color_fine_evictions", "far_color_fine_pending"]:
		if gpu.has(key):
			lines.append("%s: %s" % [key, str(gpu[key])])
	_show_runtime_page_result("Sparse Terrain Raster Storage", "\n".join(lines))


func _bake_virtual_textures(rebuild_all: bool) -> void:
	if not is_instance_valid(plugin) or not is_instance_valid(plugin.terrain):
		push_error("Select a Terrain3D node before baking virtual textures")
		return
	if is_instance_valid(_vt_rebake_timer):
		_vt_rebake_timer.stop()
	_vt_rebake_terrain = plugin.terrain
	var result: Dictionary = (_vt_rebake_terrain.force_rebake_virtual_textures() if rebuild_all else _vt_rebake_terrain.bake_changed_virtual_textures())
	_show_vt_rebake_result(result, true)
	if _vt_rebake_timer == null:
		_vt_rebake_timer = Timer.new()
		_vt_rebake_timer.wait_time = 0.5
		_vt_rebake_timer.timeout.connect(_poll_vt_rebake)
		add_child(_vt_rebake_timer)
	if String(result.get("state", "failed")) not in ["saved", "failed"]:
		_vt_rebake_timer.start()


func _poll_vt_rebake() -> void:
	if not is_instance_valid(_vt_rebake_terrain):
		_vt_rebake_timer.stop()
		return
	var result: Dictionary = _vt_rebake_terrain.get_force_rebake_virtual_textures_status()
	_show_vt_rebake_result(result)
	var composed_state: String = String(result.get("composed_bake", "idle"))
	if String(result.get("state", "")) in ["saved", "failed"] and not bool(result.get("composed_bake_pending", false)) and composed_state not in ["queued", "working", "queued_after_source_bake"]:
		_vt_rebake_timer.stop()


func _show_vt_rebake_result(result: Dictionary, open_dialog: bool = false) -> void:
	var lines: PackedStringArray = PackedStringArray([
		"Status: %s" % String(result.get("state", "queued")).to_upper(),
		"Bake request ID / completed ID: %d / %d" % [int(result.get("request_serial", 0)), int(result.get("completion_serial", 0))],
		"Current stage: %s" % String(result.get("phase", "unknown")),
		"Terrain VT topology ready: %s" % str(result.get("megavt_ready", false)),
		"Cooked MegaVT regions: %d / %d" % [int(result.get("cooked_megavt_regions", 0)), int(result.get("forced_megavt_regions", 0))],
		"Region worker active: %s | total submitted: %d | retries: %d | failures: %d" % [str(result.get("worker_active", false)), int(result.get("worker_submitted", 0)), int(result.get("worker_retries", 0)), int(result.get("worker_failures", 0))],
		"GPU publication pending: %s | preview readback pending: %s" % [str(result.get("gpu_publication_pending", false)), str(result.get("preview_readback_pending", false))],
		"Forced material allocations: %d" % int(result.get("forced_material_allocations", 0)),
		"MegaVT regions to bake: %d" % int(result.get("forced_megavt_regions", 0)),
		"Package generation: %d" % int(result.get("package_generation", 0)),
		"Publication serial: %d" % int(result.get("publication_serial", 0)),
		"Output: %s" % String(result.get("output_path", "")),
		"Composed candidate: %s (reference only; viewport still uses live materials)" % String(result.get("composed_bake", "idle")),
		"Composed pages: %d | Reused: %d | Bytes: %d" % [int(result.get("composed_bake_pages", 0)), int(result.get("composed_bake_reused_pages", 0)), int(result.get("composed_bake_bytes", 0))],
		"Boundary-clamped preview pages: %d (requires seam parity before GPU use)" % int(result.get("composed_bake_boundary_clamped_pages", 0)),
		"Composed candidate path: %s" % String(result.get("composed_bake_path", "")),
		"Viewport backend: %s | Fallback: %s" % [String(result.get("composed_backend", "live")), String(result.get("composed_live_fallback_reason", ""))],
	])
	var error_text: String = String(result.get("error", ""))
	if not error_text.is_empty():
		lines.append("Error: %s" % error_text)
	var worker_error: String = String(result.get("worker_last_error", ""))
	if not worker_error.is_empty():
		lines.append("Region worker: %s" % worker_error)
	var composed_error: String = String(result.get("composed_bake_error", ""))
	if not composed_error.is_empty():
		lines.append("Composed candidate: %s" % composed_error)
	var publication_error: String = String(result.get("gpu_publication_error", ""))
	if not publication_error.is_empty():
		lines.append("GPU publication: %s" % publication_error)
	if not is_instance_valid(_vt_rebake_result):
		_vt_rebake_result = AcceptDialog.new()
		_vt_rebake_result.title = "Terrain3D Virtual Texture Rebuild"
		_vt_rebake_result.ok_button_text = "Close"
		_vt_rebake_result.min_size = Vector2i(600, 300)
		add_child(_vt_rebake_result)
	_vt_rebake_result.dialog_text = "\n".join(lines)
	# Polling updates progress without reopening a dismissed dialog or replacing
	# the results of a different menu action. Only a new bake click opens it.
	if open_dialog and not _vt_rebake_result.visible:
		_vt_rebake_result.popup_centered()


func open_runtime_cook_confirmation() -> void:
	_open_runtime_page_cook_confirmation()


func _open_runtime_page_cook_confirmation() -> void:
	if not is_instance_valid(plugin) or not is_instance_valid(plugin.terrain) or plugin.terrain.data == null:
		push_error("Select a Terrain3D node with loaded terrain data before cooking")
		return
	var inspection: Dictionary = plugin.terrain.data.inspect_gpu_region_pages()
	var discovered: int = int(inspection.get("discovered", 0))
	var current: int = int(inspection.get("current", 0))
	var missing: int = int(inspection.get("missing", 0))
	var pack_path: String = str(inspection.get("package_path", ""))
	if discovered <= 0:
		_show_runtime_page_result("Terrain Dataset Cook", "No authored regions were found in %s." % plugin.terrain.data_directory)
		return
	if not is_instance_valid(_runtime_page_cook_confirm):
		_runtime_page_cook_confirm = ConfirmationDialog.new()
		_runtime_page_cook_confirm.title = "Cook Terrain Runtime Dataset"
		_runtime_page_cook_confirm.cancel_button_text = "Cancel"
		_runtime_page_cook_confirm.min_size = Vector2i(600, 260)
		_runtime_page_cook_confirm.confirmed.connect(_cook_runtime_dataset)
		add_child(_runtime_page_cook_confirm)
	_runtime_page_cook_confirm.ok_button_text = "Cook / Validate %d Regions" % discovered
	_runtime_page_cook_confirm.dialog_text = "\n".join(PackedStringArray([
		"Source directory: %s" % plugin.terrain.data_directory,
		"Authored regions: %d" % discovered,
		"Usable packed pages: %d" % current,
		"Missing packed pages: %d" % missing,
		"Current pack ready: %s" % ("YES" if bool(inspection.get("package_ready", false)) else "NO"),
		"",
		"Rebuild authoritative region pages into the aggregate terrain_stream.t3sp and .t3pm package:",
		pack_path,
		"",
		"Source terrain3d*.res files remain the authoring source of truth.",
	]))
	_runtime_page_cook_confirm.popup_centered()


func _cook_runtime_dataset() -> void:
	if not is_instance_valid(plugin) or not is_instance_valid(plugin.terrain) or plugin.terrain.data == null:
		push_error("Terrain3D data became unavailable before runtime dataset cooking")
		return
	var result: Dictionary = plugin.terrain.data.cook_gpu_region_pages()
	var lines: PackedStringArray = PackedStringArray([
		"Status: %s" % ("COMPLETE" if bool(result.get("complete", false)) else "INCOMPLETE"),
		"Authored regions: %d" % int(result.get("discovered", 0)),
		"Baked packed pages: %d" % int(result.get("cooked", 0)),
		"Usable packed pages: %d" % int(result.get("usable", 0)),
		"Failed regions: %d" % int(result.get("failed", 0)),
		"Pack verified: %s" % ("YES" if bool(result.get("package_ready", false)) else "NO"),
		"Stream pack: %s" % str(result.get("package_path", "")),
		"Stream metadata: %s" % str(result.get("package_metadata_path", "")),
		"Far atlas: %s" % ("OK" if bool(result.get("far_atlas_cooked", false)) else "FAILED"),
		"World manifest: %s" % ("OK" if bool(result.get("world_manifest_cooked", false)) else "FAILED"),
	])
	var errors: PackedStringArray = result.get("errors", PackedStringArray())
	if not errors.is_empty():
		lines.append("")
		lines.append("Errors:")
		for error_text: String in errors:
			lines.append("- %s" % error_text)
	_show_runtime_page_result("Terrain Runtime Dataset Cook", "\n".join(lines))


func _show_runtime_page_result(dialog_title: String, dialog_text: String) -> void:
	if not is_instance_valid(_runtime_page_cook_result):
		_runtime_page_cook_result = AcceptDialog.new()
		_runtime_page_cook_result.min_size = Vector2i(560, 300)
		_runtime_page_cook_result.max_size = Vector2i(900, 600)
		_runtime_page_cook_result.wrap_controls = false
		_runtime_page_cook_result.ok_button_text = "Close"
		add_child(_runtime_page_cook_result)
		# AcceptDialog's built-in Label sizes the native window to its entire
		# contents. A TextEdit provides bounded, independently scrollable text.
		_runtime_page_cook_result.get_label().hide()
		_runtime_page_cook_text = TextEdit.new()
		_runtime_page_cook_text.editable = false
		_runtime_page_cook_text.context_menu_enabled = true
		_runtime_page_cook_text.custom_minimum_size = Vector2(520, 240)
		_runtime_page_cook_result.add_child(_runtime_page_cook_text)
	_runtime_page_cook_result.title = dialog_title
	_runtime_page_cook_text.text = dialog_text
	_runtime_page_cook_text.set_caret_line(0)
	if not _runtime_page_cook_result.visible:
		_runtime_page_cook_result.popup_centered_clamped(Vector2i(740, 420), 0.8)


func _on_menu_about_to_popup() -> void:
	var has_terrain: bool = is_instance_valid(plugin) and is_instance_valid(plugin.terrain)
	var popup: PopupMenu = get_popup()
	popup.set_item_disabled(MENU_DIRECTORY_SETUP, not has_terrain)
	popup.set_item_disabled(MENU_REGION_LIGHTMAP, not has_terrain)
	popup.set_item_disabled(MENU_BAKE_CHANGED_VIRTUAL_TEXTURES, not has_terrain)
	popup.set_item_disabled(MENU_BAKE_VIRTUAL_TEXTURES, not has_terrain)
	popup.set_item_disabled(MENU_SPARSE_RASTER_STATS, not has_terrain)
	popup.set_item_disabled(MENU_HLOD, not has_terrain)
	popup.set_item_disabled(MENU_COOK_RUNTIME_DATASET, not has_terrain)
	popup.set_item_disabled(MENU_PACK_TEXTURES, not has_terrain)
	popup.set_item_disabled(MENU_BAKE_ARRAY_MESH, not has_terrain)
	popup.set_item_disabled(MENU_BAKE_OCCLUDER, not has_terrain)
	popup.set_item_disabled(MENU_SIMULATE_EROSION, not has_terrain)
	popup.set_item_disabled(MENU_AUTOSHADER_BAKER, not has_terrain)
	popup.set_item_disabled(MENU_SHADER_QUALITY, not has_terrain)

	if has_terrain:
		var nav_regions: Array[NavigationRegion3D] = baker.find_terrain_nav_regions(plugin.terrain)
		popup.set_item_disabled(MENU_BAKE_NAV_MESH, nav_regions.is_empty())
		popup.set_item_disabled(MENU_SET_UP_NAVIGATION, not nav_regions.is_empty())
	elif is_instance_valid(plugin) and is_instance_valid(plugin.nav_region):
		var terrains: Array[Terrain3D] = baker.find_nav_region_terrains(plugin.nav_region)
		popup.set_item_disabled(MENU_BAKE_NAV_MESH, terrains.is_empty())
		popup.set_item_disabled(MENU_SET_UP_NAVIGATION, true)
	else:
		popup.set_item_disabled(MENU_BAKE_NAV_MESH, true)
		popup.set_item_disabled(MENU_SET_UP_NAVIGATION, true)
