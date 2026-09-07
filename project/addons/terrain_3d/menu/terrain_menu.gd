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
var _runtime_page_migrate_confirm: ConfirmationDialog
var _runtime_page_cook_result: AcceptDialog

# These are IDs and order must remain consistent with add_item and set_disabled calls.
enum {
	MENU_DIRECTORY_SETUP,
	MENU_REGION_LIGHTMAP,
	MENU_HLOD,
	MENU_COOK_RUNTIME_DATASET,
	MENU_MIGRATE_RUNTIME_PAGES,
	MENU_PACK_TEXTURES,
	MENU_SEPARATOR,
	MENU_BAKE_ARRAY_MESH,
	MENU_BAKE_OCCLUDER,
	MENU_SIMULATE_EROSION,
	MENU_AUTOSHADER_BAKER,
	MENU_SHADER_QUALITY,
	MENU_SEPARATOR2,
	MENU_SET_UP_NAVIGATION,
	MENU_BAKE_NAV_MESH,
	MENU_SEPARATOR3,
	MENU_BRUSH_DIRECTORY,
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
	popup.add_item("Hierarchical LOD…", MENU_HLOD)
	popup.add_item("Cook Runtime Dataset…", MENU_COOK_RUNTIME_DATASET)
	popup.add_item("Migrate Runtime Pages…", MENU_MIGRATE_RUNTIME_PAGES)
	popup.add_item("Channel Packer…", MENU_PACK_TEXTURES)
	popup.add_separator("", MENU_SEPARATOR)
	popup.add_item("Bake Terrain Mesh…", MENU_BAKE_ARRAY_MESH)
	popup.add_item("Bake Terrain Occluder…", MENU_BAKE_OCCLUDER)
	popup.add_item("Open Erosion Simulator…", MENU_SIMULATE_EROSION)
	popup.add_item("Open Editor Autoshader Baker…", MENU_AUTOSHADER_BAKER)
	popup.add_item("Runtime Shader Quality…", MENU_SHADER_QUALITY)
	popup.add_separator("", MENU_SEPARATOR2)
	popup.add_item("Set Up Navigation…", MENU_SET_UP_NAVIGATION)
	popup.add_item("Bake Navigation Mesh…", MENU_BAKE_NAV_MESH)
	popup.add_separator("", MENU_SEPARATOR3)
	popup.add_item("Open Brush Directory…", MENU_BRUSH_DIRECTORY)

	_set_item_tooltip(popup, MENU_DIRECTORY_SETUP, "Choose the directory containing Terrain3D region resources.")
	_set_item_tooltip(popup, MENU_REGION_LIGHTMAP, "Bake independent lightmaps for every authored Terrain3D region in the active data directory.")
	_set_item_tooltip(popup, MENU_HLOD, "Analyze, bake, validate, and preview Terrain3D hierarchical LOD proxies.")
	_set_item_tooltip(popup, MENU_COOK_RUNTIME_DATASET, "Compile and reload-validate every authored terrain region as a .t3dpage runtime page, then rebuild the far atlas.")
	_set_item_tooltip(popup, MENU_MIGRATE_RUNTIME_PAGES, "Scan only runtime-page headers and regenerate legacy .t3dpage files from authoritative region resources. Current pages are left untouched.")
	_set_item_tooltip(popup, MENU_PACK_TEXTURES, "Pack albedo/height and normal/roughness textures.")
	_set_item_tooltip(popup, MENU_BAKE_ARRAY_MESH, "Create a MeshInstance3D from terrain geometry.")
	_set_item_tooltip(popup, MENU_BAKE_OCCLUDER, "Create a coarse OccluderInstance3D from terrain geometry.")
	_set_item_tooltip(popup, MENU_SIMULATE_EROSION, "Open the erosion workflow in the Terrain3D Inspector section.")
	_set_item_tooltip(popup, MENU_AUTOSHADER_BAKER, "Author expanded terrain material rules and bake them into control maps.")
	_set_item_tooltip(popup, MENU_SHADER_QUALITY, "Open the existing runtime terrain and ocean shader quality panel.")
	_set_item_tooltip(popup, MENU_SET_UP_NAVIGATION, "Create and configure a NavigationRegion3D for the selected terrain.")
	_set_item_tooltip(popup, MENU_BAKE_NAV_MESH, "Rebuild navigation geometry for the active terrain or navigation region.")
	_set_item_tooltip(popup, MENU_BRUSH_DIRECTORY, "Open the folder containing Terrain3D brush images.")

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
		MENU_HLOD:
			plugin.open_hlod_panel()
		MENU_COOK_RUNTIME_DATASET:
			_open_runtime_page_cook_confirmation()
		MENU_MIGRATE_RUNTIME_PAGES:
			_open_runtime_page_migrate_confirmation()
		MENU_PACK_TEXTURES:
			packer.pack_textures_popup()
		MENU_BAKE_ARRAY_MESH:
			baker.bake_mesh_popup()
		MENU_BAKE_OCCLUDER:
			baker.bake_occluder_popup()
		MENU_SIMULATE_EROSION:
			plugin.open_erosion_simulator()
		MENU_AUTOSHADER_BAKER:
			plugin.open_autoshader_baker()
		MENU_SHADER_QUALITY:
			plugin.open_shader_performance()
		MENU_SET_UP_NAVIGATION:
			baker.set_up_navigation_popup()
		MENU_BAKE_NAV_MESH:
			baker.bake_nav_mesh()
		MENU_BRUSH_DIRECTORY:
			OS.shell_show_in_file_manager(
				ProjectSettings.globalize_path("res://addons/terrain_3d/brushes")
			)


func _open_runtime_page_migrate_confirmation() -> void:
	if not is_instance_valid(plugin) or not is_instance_valid(plugin.terrain) or plugin.terrain.data == null:
		push_error("Select a Terrain3D node with loaded terrain data before migrating runtime pages")
		return
	var inspection: Dictionary = plugin.terrain.data.inspect_gpu_region_page_migration()
	if bool(inspection.get("preflight_failed", false)):
		var preflight_lines: PackedStringArray = PackedStringArray([
			"Terrain3D could not build a safe runtime-page migration set.",
			"",
		])
		var preflight_errors: PackedStringArray = inspection.get("errors", PackedStringArray())
		for error_text: String in preflight_errors:
			preflight_lines.append("• %s" % error_text)
		_show_runtime_page_result("Runtime Page Migration Needs Attention", "\n".join(preflight_lines))
		return
	var legacy: int = int(inspection.get("legacy", 0))
	var future: int = int(inspection.get("future", 0))
	var invalid: int = int(inspection.get("invalid", 0))
	if legacy == 0:
		_show_runtime_page_result("Runtime Page Migration", "\n".join(PackedStringArray([
			"Current page format: v%d" % int(inspection.get("current_version", 0)),
			"Current pages: %d" % int(inspection.get("current", 0)),
			"Legacy pages: 0",
			"Missing pages: %d" % int(inspection.get("missing", 0)),
			"Invalid pages: %d" % invalid,
			"Future-version pages: %d" % future,
			"",
			"No format migration is required.",
		])))
		return
	if future > 0:
		_show_runtime_page_result("Runtime Page Migration Blocked", "\n".join(PackedStringArray([
			"Legacy pages ready to migrate: %d" % legacy,
			"Future-version pages: %d" % future,
			"",
			"A newer page format is present. This build will not write any pages into that dataset.",
		])))
		return
	if not is_instance_valid(_runtime_page_migrate_confirm):
		_runtime_page_migrate_confirm = ConfirmationDialog.new()
		_runtime_page_migrate_confirm.title = "Migrate Terrain Runtime Pages"
		_runtime_page_migrate_confirm.cancel_button_text = "Cancel"
		_runtime_page_migrate_confirm.min_size = Vector2i(600, 250)
		_runtime_page_migrate_confirm.confirmed.connect(_migrate_runtime_pages)
		add_child(_runtime_page_migrate_confirm)
	_runtime_page_migrate_confirm.ok_button_text = "Migrate %d Page%s" % [legacy, "" if legacy == 1 else "s"]
	_runtime_page_migrate_confirm.dialog_text = "\n".join(PackedStringArray([
		"Target format: v%d" % int(inspection.get("current_version", 0)),
		"Current pages left untouched: %d" % int(inspection.get("current", 0)),
		"Legacy pages to regenerate: %d" % legacy,
		"Missing pages left untouched: %d" % int(inspection.get("missing", 0)),
		"Invalid pages left untouched: %d" % invalid,
		"",
		"Migration regenerates only legacy .t3dpage files from the authoritative terrain3d*.res sources. Each page is published atomically and reload-validated before the old page is discarded.",
	]))
	_runtime_page_migrate_confirm.popup_centered()


func _migrate_runtime_pages() -> void:
	if not is_instance_valid(plugin) or not is_instance_valid(plugin.terrain) or plugin.terrain.data == null:
		push_error("Terrain3D data became unavailable before runtime-page migration started")
		return
	var result: Dictionary = plugin.terrain.data.migrate_gpu_region_pages(false)
	var lines: PackedStringArray = PackedStringArray([
		"Migration status: %s" % ("COMPLETE" if bool(result.get("migration_complete", false)) else "INCOMPLETE"),
		"Dataset complete: %s" % ("YES" if bool(result.get("complete", false)) else "NO"),
		"Target page format: v%d" % int(result.get("current_version", 0)),
		"Migrated: %d / %d" % [int(result.get("migrated", 0)), int(result.get("legacy_candidates", 0))],
		"Current / unchanged: %d" % int(result.get("unchanged", 0)),
		"Skipped: %d  (missing %d · invalid %d)" % [
			int(result.get("skipped", 0)),
			int(result.get("missing_skipped", 0)),
			int(result.get("invalid_skipped", 0)),
		],
		"Failures: %d" % int(result.get("failed", 0)),
		"World manifest republished: %s" % ("YES" if bool(result.get("manifest_published", false)) else "NO"),
	])
	var errors: PackedStringArray = result.get("errors", PackedStringArray())
	if not errors.is_empty():
		lines.append("")
		lines.append("Errors:")
		for error_text: String in errors:
			lines.append("• %s" % error_text)
	_show_runtime_page_result("Terrain Runtime Page Migration", "\n".join(lines))


func _open_runtime_page_cook_confirmation() -> void:
	if not is_instance_valid(plugin) or not is_instance_valid(plugin.terrain) or plugin.terrain.data == null:
		push_error("Select a Terrain3D node with loaded terrain data before cooking runtime pages")
		return
	var inspection: Dictionary = plugin.terrain.data.inspect_gpu_region_pages()
	if not bool(inspection.get("complete", false)) and inspection.has("error"):
		push_error("Unable to inspect Terrain3D runtime pages: %s" % String(inspection.get("error", "unknown error")))
		return
	if bool(inspection.get("preflight_failed", false)):
		var preflight_lines: PackedStringArray = PackedStringArray([
			"Terrain3D could not build a safe runtime-page bake set from the configured data directory.",
			"",
		])
		var preflight_errors: PackedStringArray = inspection.get("errors", PackedStringArray())
		for error_text: String in preflight_errors:
			preflight_lines.append("• %s" % error_text)
		_show_runtime_page_result("Runtime Dataset Needs Attention", "\n".join(preflight_lines))
		return
	var discovered: int = int(inspection.get("discovered", 0))
	var needs_cook: int = int(inspection.get("needs_cook", 0))
	var future: int = int(inspection.get("future", 0))
	var orphan_count: int = int(inspection.get("orphan_count", 0))
	if future > 0:
		_show_runtime_page_result("Runtime Dataset Cook Blocked", "\n".join(PackedStringArray([
			"Future-version runtime pages: %d" % future,
			"",
			"This build will not overwrite pages produced by a newer runtime-page format.",
		])))
		return
	if not is_instance_valid(_runtime_page_cook_confirm):
		_runtime_page_cook_confirm = ConfirmationDialog.new()
		_runtime_page_cook_confirm.title = "Cook Terrain Runtime Dataset"
		_runtime_page_cook_confirm.cancel_button_text = "Cancel"
		_runtime_page_cook_confirm.min_size = Vector2i(600, 260)
		_runtime_page_cook_confirm.confirmed.connect(_cook_runtime_dataset)
		add_child(_runtime_page_cook_confirm)
	_runtime_page_cook_confirm.ok_button_text = "Cook %d Page%s" % [needs_cook, "" if needs_cook == 1 else "s"] if needs_cook > 0 else "Validate / Rebuild Dataset"
	_runtime_page_cook_confirm.dialog_text = "\n".join(PackedStringArray([
		"Source of truth: %s" % String(inspection.get("data_directory", plugin.terrain.data_directory)),
		"Authored regions: %d" % discovered,
		"Current pages: %d" % int(inspection.get("current", 0)),
		"Need cooking: %d  (missing %d · legacy %d · stale %d · incompatible %d · invalid %d · unavailable %d)" % [
			needs_cook, int(inspection.get("missing", 0)), int(inspection.get("legacy", 0)),
			int(inspection.get("stale", 0)), int(inspection.get("incompatible", 0)),
			int(inspection.get("invalid", 0)), int(inspection.get("source_unavailable", 0))],
		"Orphan pages to remove: %d" % orphan_count,
		"",
		"Cooking validates/rebuilds .t3dpage files against every terrain3d*.res file in this directory, then rebuilds the whole-world far atlas.",
	]))
	_runtime_page_cook_confirm.popup_centered()


func _cook_runtime_dataset() -> void:
	if not is_instance_valid(plugin) or not is_instance_valid(plugin.terrain) or plugin.terrain.data == null:
		push_error("Terrain3D data became unavailable before runtime-page cooking started")
		return
	var result: Dictionary = plugin.terrain.data.cook_gpu_region_pages()
	var complete: bool = bool(result.get("complete", false))
	var lines: PackedStringArray = PackedStringArray([
		"Status: %s" % ("COMPLETE" if complete else "INCOMPLETE"),
		"Authored regions discovered: %d" % int(result.get("discovered", 0)),
		"Cooked now: %d" % int(result.get("cooked", 0)),
		"Legacy pages replaced: %d" % int(result.get("legacy_recooked", 0)),
		"Stale pages replaced: %d" % int(result.get("stale_recooked", 0)),
		"Current-version incompatible pages replaced: %d" % int(result.get("incompatible_recooked", 0)),
		"Invalid pages replaced: %d" % int(result.get("invalid_recooked", 0)),
		"Validated unchanged: %d" % int(result.get("unchanged", 0)),
		"Usable pages: %d" % int(result.get("usable", 0)),
		"Failures: %d" % int(result.get("failed", 0)),
		"Unavailable source entries: %d" % int(result.get("skipped", 0)),
		"Orphan pages removed: %d" % int(result.get("orphan_removed", 0)),
		"Orphan removal failures: %d" % int(result.get("orphan_failed", 0)),
		"Far atlas: %s" % ("OK" if bool(result.get("far_atlas_cooked", false)) else "FAILED"),
	])
	var errors: PackedStringArray = result.get("errors", PackedStringArray())
	if not errors.is_empty():
		lines.append("")
		lines.append("Errors:")
		for error_text in errors:
			lines.append("• %s" % error_text)
	_show_runtime_page_result("Terrain Runtime Dataset Cook", "\n".join(lines))


func _show_runtime_page_result(dialog_title: String, dialog_text: String) -> void:
	if not is_instance_valid(_runtime_page_cook_result):
		_runtime_page_cook_result = AcceptDialog.new()
		_runtime_page_cook_result.min_size = Vector2i(600, 300)
		add_child(_runtime_page_cook_result)
	_runtime_page_cook_result.title = dialog_title
	_runtime_page_cook_result.dialog_text = dialog_text
	_runtime_page_cook_result.popup_centered()


func _on_menu_about_to_popup() -> void:
	var has_terrain: bool = is_instance_valid(plugin) and is_instance_valid(plugin.terrain)
	var popup: PopupMenu = get_popup()
	popup.set_item_disabled(MENU_DIRECTORY_SETUP, not has_terrain)
	popup.set_item_disabled(MENU_REGION_LIGHTMAP, not has_terrain)
	popup.set_item_disabled(MENU_HLOD, not has_terrain)
	popup.set_item_disabled(MENU_COOK_RUNTIME_DATASET, not has_terrain)
	popup.set_item_disabled(MENU_MIGRATE_RUNTIME_PAGES, not has_terrain)
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
