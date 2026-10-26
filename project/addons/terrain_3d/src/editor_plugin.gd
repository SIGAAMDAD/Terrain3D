# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Editor Plugin for Terrain3D
@tool
extends "res://addons/terrain_3d/src/editor_plugin_base.gd"


# Includes
const Terrain3DUI = preload("res://addons/terrain_3d/src/ui.gd")
const VtDemandExportPlugin = preload("res://addons/terrain_3d/src/vt_demand_export_plugin.gd")
# Export-preflight parsing is deferred until the user explicitly requests it.
# A broken export checker must not disable the painting sidebar at startup.
const RUNTIME_EXPORT_PREFLIGHT_PATH: String = "res://addons/terrain_3d/src/runtime_export_preflight.gd"
const InstanceTransformProxy = preload("res://addons/terrain_3d/src/instance_transform_proxy.gd")
const ASSET_DOCK: PackedScene = preload("res://addons/terrain_3d/src/asset_dock.tscn")
const ErosionInspectorPlugin = preload("res://addons/terrain_3d/menu/erosion_inspector_plugin.gd")
const ErosionSimulator = preload("res://addons/terrain_3d/menu/erosion_simulator.gd")
const EditorUI = preload("res://addons/terrain_3d/src/editor_ui_util.gd")
const RegionNodeInspectorPlugin = preload("res://addons/terrain_3d/tools/region_node_inspector_plugin.gd")
const RegionWorkbenchTerrainInspectorPlugin = preload("res://addons/terrain_3d/tools/region_workbench_terrain_inspector_plugin.gd")
# Optional Inspector documentation must not be a hard parser dependency of
# the primary terrain toolbar. Resolve it after core controls are registered.
const TERRAIN_PROPERTY_HELP_PATH: String = "res://addons/terrain_3d/src/inspector/terrain_inspector_property_help.gd"
const TERRAIN_RESOURCE_HELP_PATH: String = "res://addons/terrain_3d/src/inspector/terrain_resource_help_inspector_plugin.gd"
const TERRAIN_SETTINGS_WINDOW_PATH: String = "res://addons/terrain_3d/src/inspector/terrain_settings_window.gd"
const ShaderPerformancePanel = preload("res://addons/terrain_3d/src/shader_performance_panel.gd")
const AutoShaderPanel = preload("res://addons/terrain_3d/src/autoshader_panel.gd")
const StreamingConverter = preload("res://addons/terrain_3d/tools/streaming_converter.gd")
const RegionLightmapBaker = preload("res://addons/terrain_3d/tools/region_lightmap_baker.gd")
const RegionLightmapPanel = preload("res://addons/terrain_3d/tools/region_lightmap_panel.gd")
const HLODBaker = preload("res://addons/terrain_3d/tools/hlod_baker.gd")
const HLODPanel = preload("res://addons/terrain_3d/tools/hlod_panel.gd")
const FoliagePanel = preload("res://addons/terrain_3d/tools/foliage_panel.gd")
const TOOL_MENU_OPEN: String = "Terrain3D: Open Tools Menu"
const TOOL_MENU_EXPORT_PREFLIGHT: String = "Terrain3D: Validate Runtime Export"
const EXPECTED_NATIVE_API_REVISION: int = 2026091003
const EXPECTED_NATIVE_BUILD_ID: String = "mesh-derived-instance-physics-occlusion-p13-2-rd-capacity-align-2026-09-16"
const EXPECTED_NATIVE_BUILD_FLAVOR: String = "editor"

# Editor Plugin
var ui: Terrain3DUI
var asset_dock: Terrain3DAssetDock
var erosion_inspector_plugin: ErosionInspectorPlugin
var region_node_inspector_plugin: RegionNodeInspectorPlugin
var region_workbench_inspector_plugin: EditorInspectorPlugin
var property_help_inspector_plugin: EditorInspectorPlugin
var resource_help_inspector_plugin: EditorInspectorPlugin
var terrain_settings_window: Window
var erosion_window: Window
var erosion_panel: ErosionSimulator
var shader_performance_panel: ShaderPerformancePanel
var autoshader_panel: AutoShaderPanel
var streaming_converter: StreamingConverter
var region_lightmap_baker: RegionLightmapBaker
var region_lightmap_panel: RegionLightmapPanel
var hlod_baker: HLODBaker
var hlod_panel: HLODPanel
var vt_demand_export_plugin: EditorExportPlugin
var foliage_panel: FoliagePanel

class ErosionSettingsCacheEntry extends RefCounted:
	var terrain_ref: WeakRef
	var settings: Terrain3DErosionSettings

	func _init(p_terrain: Terrain3D, p_settings: Terrain3DErosionSettings) -> void:
		terrain_ref = weakref(p_terrain)
		settings = p_settings

var erosion_settings_cache: Dictionary[int, ErosionSettingsCacheEntry] = {}

# Terrain
var _last_terrain: Terrain3D

# Input
var _last_modifiers: int = 0
var _use_meta: bool = false
var _native_contract_error_reported: bool = false
var _native_editor_initialized: bool = false
var _instance_edit_mode: bool = false
var _instance_edit_proxy: Node3D


func _init() -> void:
	if debug:
		print("Terrain3DEditorPlugin: _init")
	if OS.get_name() == "macOS":
		_use_meta = true


# Poll on the editor owner thread; RD completion callbacks never touch scene
# objects or map Images. The native method is a cheap no-op without GPU work.
func _process(_delta: float) -> void:
	if _native_editor_initialized and is_instance_valid(editor):
		editor.poll_editor_brush_commit()


func _enter_tree() -> void:
	if debug:
		print("Terrain3DEditorPlugin: _enter_tree")
	# No UI, editor objects or export hooks may be constructed for a template DLL.
	# This check runs before any authoring component is initialized.
	if not _validate_native_editor_build():
		return
	editor = Terrain3DEditor.new()
	_native_editor_initialized = true
	setup_editor_settings()
	vt_demand_export_plugin = VtDemandExportPlugin.new()
	add_export_plugin(vt_demand_export_plugin)
	ui = Terrain3DUI.new()
	ui.plugin = self
	add_child(ui)

	asset_dock = ASSET_DOCK.instantiate() as Terrain3DAssetDock
	asset_dock.initialize(self)
	ui.set_asset_dock(asset_dock)
	ui.toolbar.tool_changed.connect(asset_dock._on_tool_changed)

	# The Inspector stays lightweight. Authoring jobs live in a persistent window.
	erosion_inspector_plugin = ErosionInspectorPlugin.new()
	erosion_inspector_plugin.plugin = self
	add_inspector_plugin(erosion_inspector_plugin)
	region_node_inspector_plugin = RegionNodeInspectorPlugin.new()
	region_node_inspector_plugin.plugin = self
	add_inspector_plugin(region_node_inspector_plugin)
	region_workbench_inspector_plugin = RegionWorkbenchTerrainInspectorPlugin.new()
	add_inspector_plugin(region_workbench_inspector_plugin)
	# A syntax/import error in optional Inspector help must not prevent terrain
	# painting tools from being constructed. The native Inspector still works.
	property_help_inspector_plugin = _load_optional_inspector(TERRAIN_PROPERTY_HELP_PATH)
	if is_instance_valid(property_help_inspector_plugin):
		add_inspector_plugin(property_help_inspector_plugin)
	resource_help_inspector_plugin = _load_optional_inspector(TERRAIN_RESOURCE_HELP_PATH)
	if is_instance_valid(resource_help_inspector_plugin):
		add_inspector_plugin(resource_help_inspector_plugin)

	shader_performance_panel = ShaderPerformancePanel.new()
	shader_performance_panel.plugin = self
	add_child(shader_performance_panel)
	autoshader_panel = AutoShaderPanel.new()
	autoshader_panel.plugin = self
	add_child(autoshader_panel)
	streaming_converter = StreamingConverter.new()
	streaming_converter.plugin = self
	add_child(streaming_converter)
	region_lightmap_baker = RegionLightmapBaker.new()
	region_lightmap_baker.plugin = self
	add_child(region_lightmap_baker)
	region_lightmap_panel = RegionLightmapPanel.new()
	region_lightmap_panel.visible = false
	region_lightmap_panel.plugin = self
	region_lightmap_panel.baker = region_lightmap_baker
	add_child(region_lightmap_panel)
	hlod_baker = HLODBaker.new()
	hlod_panel = HLODPanel.new()
	hlod_panel.visible = false
	hlod_panel.plugin = self
	hlod_panel.baker = hlod_baker
	add_child(hlod_panel)

	foliage_panel = FoliagePanel.new()
	foliage_panel.plugin = self
	add_child(foliage_panel)

	# Other authoring actions are available from Terrain3D > Tools.
	add_tool_menu_item(TOOL_MENU_OPEN, _open_terrain_tools_menu)
	add_tool_menu_item(TOOL_MENU_EXPORT_PREFLIGHT, _validate_runtime_export)
	_connect_editor_signals()
	# Enabling the plugin while Terrain3D is already selected does not always
	# replay _edit(). Reconcile once after the editor has entered the tree.
	_sync_tools_after_selection.call_deferred()


func _load_optional_inspector(p_path: String) -> EditorInspectorPlugin:
	var script: Script = load(p_path) as Script
	if script == null or not script.can_instantiate():
		push_warning("Terrain3D optional Inspector documentation disabled: %s" % p_path)
		return null
	var instance: EditorInspectorPlugin = script.new() as EditorInspectorPlugin
	if not is_instance_valid(instance):
		push_warning("Terrain3D optional Inspector documentation has invalid type: %s" % p_path)
	return instance


func _exit_tree() -> void:
	if debug:
		print("Terrain3DEditorPlugin: _exit_tree")
	if not _native_editor_initialized:
		return
	_native_editor_initialized = false
	_disconnect_editor_signals()
	if vt_demand_export_plugin != null:
		remove_export_plugin(vt_demand_export_plugin)
		vt_demand_export_plugin = null
	_detach_current_terrain()
	_last_terrain = null
	remove_tool_menu_item(TOOL_MENU_OPEN)
	remove_tool_menu_item(TOOL_MENU_EXPORT_PREFLIGHT)
	# Release the long-lived erosion panel only when the addon is disabled.
	if is_instance_valid(erosion_window):
		erosion_window.queue_free()
		erosion_window = null
		erosion_panel = null
	if is_instance_valid(terrain_settings_window):
		terrain_settings_window.queue_free()
		terrain_settings_window = null
	if resource_help_inspector_plugin:
		remove_inspector_plugin(resource_help_inspector_plugin)
		resource_help_inspector_plugin = null
	if property_help_inspector_plugin:
		remove_inspector_plugin(property_help_inspector_plugin)
		property_help_inspector_plugin = null
	if region_workbench_inspector_plugin:
		remove_inspector_plugin(region_workbench_inspector_plugin)
		region_workbench_inspector_plugin = null
	if region_node_inspector_plugin:
		remove_inspector_plugin(region_node_inspector_plugin)
		region_node_inspector_plugin = null
	if erosion_inspector_plugin:
		remove_inspector_plugin(erosion_inspector_plugin)
		erosion_inspector_plugin = null
	erosion_settings_cache.clear()
	if is_instance_valid(autoshader_panel):
		autoshader_panel.shutdown()
		autoshader_panel.queue_free()
		autoshader_panel = null
	if is_instance_valid(shader_performance_panel):
		shader_performance_panel.queue_free()
		shader_performance_panel = null
	if is_instance_valid(streaming_converter):
		streaming_converter.queue_free()
		streaming_converter = null
	if is_instance_valid(region_lightmap_panel):
		region_lightmap_panel.queue_free()
		region_lightmap_panel = null
	if is_instance_valid(region_lightmap_baker):
		region_lightmap_baker.queue_free()
		region_lightmap_baker = null
	if is_instance_valid(foliage_panel):
		foliage_panel.queue_free()
		foliage_panel = null
	if is_instance_valid(hlod_panel):
		hlod_panel.queue_free()
		hlod_panel = null
	hlod_baker = null
	if is_instance_valid(ui) and is_instance_valid(ui.toolbar) and is_instance_valid(asset_dock):
		if ui.toolbar.tool_changed.is_connected(asset_dock._on_tool_changed):
			ui.toolbar.tool_changed.disconnect(asset_dock._on_tool_changed)
	if is_instance_valid(asset_dock):
		asset_dock.remove_dock(true)
		asset_dock = null
	if is_instance_valid(ui):
		ui.queue_free()
		ui = null
	if is_instance_valid(editor):
		editor.free()
		editor = null


func _connect_editor_signals() -> void:
	# Connect only after all editor helpers are initialized so scene/inspector callbacks
	# cannot observe a partially constructed plugin. Terrain3D deliberately does not
	# subscribe to editor-window focus changes.
	if not scene_changed.is_connected(_on_scene_changed):
		scene_changed.connect(_on_scene_changed)
	var selection: EditorSelection = EditorInterface.get_selection()
	if is_instance_valid(selection) and not selection.selection_changed.is_connected(_on_editor_selection_changed):
		selection.selection_changed.connect(_on_editor_selection_changed)
	# Retained for AssetDock focus handoff only. No focus-entered callback is
	# connected here; Terrain3D has no work to perform merely because Godot regained focus.
	godot_editor_window = EditorInterface.get_base_control().get_window()
	var inspector := EditorInterface.get_inspector()
	if is_instance_valid(inspector) and not inspector.mouse_entered.is_connected(_on_inspector_mouse_entered):
		inspector.mouse_entered.connect(_on_inspector_mouse_entered)


func _disconnect_editor_signals() -> void:
	# Disconnect before teardown so queued editor callbacks cannot access objects
	# that are being removed below.
	if scene_changed.is_connected(_on_scene_changed):
		scene_changed.disconnect(_on_scene_changed)
	var selection: EditorSelection = EditorInterface.get_selection()
	if is_instance_valid(selection) and selection.selection_changed.is_connected(_on_editor_selection_changed):
		selection.selection_changed.disconnect(_on_editor_selection_changed)
	var inspector := EditorInterface.get_inspector()
	if is_instance_valid(inspector) and inspector.mouse_entered.is_connected(_on_inspector_mouse_entered):
		inspector.mouse_entered.disconnect(_on_inspector_mouse_entered)
	godot_editor_window = null


func _on_editor_selection_changed() -> void:
	# Godot can invoke _make_visible and _edit in either order. Inspect the
	# settled scene selection rather than the transient callback state.
	_sync_tools_after_selection.call_deferred()


func _sync_tools_after_selection() -> void:
	if not _native_editor_initialized or not is_instance_valid(editor) or not is_instance_valid(ui) \
			or not is_instance_valid(ui.toolbar):
		return
	var selection: EditorSelection = EditorInterface.get_selection()
	if not is_instance_valid(selection):
		return
	for node: Node in selection.get_selected_nodes():
		if not node is Terrain3D:
			continue
		var selected_terrain: Terrain3D = node as Terrain3D
		if not is_instance_valid(selected_terrain) or not selected_terrain.is_inside_tree():
			continue
		var active_root: Node = EditorInterface.get_edited_scene_root()
		if not is_instance_valid(active_root) or (selected_terrain != active_root and not active_root.is_ancestor_of(selected_terrain)):
			continue
		# An authored Terrain3D node can be selected before its data resource is
		# ready. That must not hide the entire tool sidebar. The native binding
		# itself keeps the stricter data/lifetime checks.
		if is_terrain_valid(selected_terrain):
			if terrain != selected_terrain:
				_bind_terrain(selected_terrain)
			elif not ui.toolbar.visible:
				ui.set_visible(true)
		else:
			ui.set_visible(true)
		return
	if _is_instance_edit_proxy_selected() and is_terrain_valid():
		if not ui.toolbar.visible:
			ui.set_visible(true)
		return
	if is_instance_valid(nav_region) and is_terrain_valid(_last_terrain):
		ui.set_visible(true, true)
	else:
		ui.set_visible(false)


func get_erosion_settings(p_terrain: Terrain3D) -> Terrain3DErosionSettings:
	if not is_instance_valid(p_terrain):
		return Terrain3DErosionSettings.new()
	# Weak terrain ownership must not keep old scene settings alive indefinitely.
	for terrain_id: int in erosion_settings_cache.keys():
		var cached: ErosionSettingsCacheEntry = erosion_settings_cache[terrain_id]
		if not is_instance_valid(cached.terrain_ref.get_ref() if cached.terrain_ref else null):
			erosion_settings_cache.erase(terrain_id)
	var terrain_id := p_terrain.get_instance_id()
	if erosion_settings_cache.has(terrain_id):
		var entry: ErosionSettingsCacheEntry = erosion_settings_cache[terrain_id]
		var cached_terrain := entry.terrain_ref.get_ref() as Terrain3D if entry.terrain_ref else null
		if cached_terrain == p_terrain and is_instance_valid(entry.settings):
			return entry.settings
	var settings := Terrain3DErosionSettings.new()
	erosion_settings_cache[terrain_id] = ErosionSettingsCacheEntry.new(p_terrain, settings)
	return settings


func set_erosion_settings(p_terrain: Terrain3D, p_settings: Terrain3DErosionSettings) -> void:
	if not is_instance_valid(p_terrain) or not is_instance_valid(p_settings):
		return
	erosion_settings_cache[p_terrain.get_instance_id()] = ErosionSettingsCacheEntry.new(p_terrain, p_settings)


func open_foliage_generator() -> void:
	if is_instance_valid(foliage_panel):
		foliage_panel.open()


func open_erosion_simulator() -> void:
	var active_terrain: Terrain3D = get_terrain()
	if not is_instance_valid(active_terrain):
		push_warning("Select a Terrain3D node before opening erosion tools.")
		return
	if not is_instance_valid(erosion_window):
		_create_erosion_window(active_terrain)
	if not is_instance_valid(erosion_panel):
		return
	if not erosion_panel.bind_terrain(active_terrain):
		# An active job remains bound to the source terrain until it completes.
		push_warning("Finish or cancel the current erosion job before switching terrains.")
	var preferred: Vector2i = Vector2i(int(EditorUI.scaled(1040.0)), int(EditorUI.scaled(760.0)))
	var minimum: Vector2i = Vector2i(int(EditorUI.scaled(680.0)), int(EditorUI.scaled(460.0)))
	if erosion_window.visible:
		erosion_window.grab_focus()
	else:
		erosion_window.popup_centered(EditorUI.popup_size_for_screen(erosion_window, preferred, minimum))


func _create_erosion_window(p_terrain: Terrain3D) -> void:
	erosion_window = Window.new()
	erosion_window.title = "Terrain3D — Erosion"
	erosion_window.initial_position = Window.WINDOW_INITIAL_POSITION_CENTER_MAIN_WINDOW_SCREEN
	erosion_window.transient = true
	erosion_window.exclusive = false
	erosion_window.visible = false
	erosion_window.close_requested.connect(erosion_window.hide)
	erosion_window.min_size = Vector2i(int(EditorUI.scaled(680.0)), int(EditorUI.scaled(460.0)))

	var margin: MarginContainer = MarginContainer.new()
	for side: StringName in [&"margin_left", &"margin_top", &"margin_right", &"margin_bottom"]:
		margin.add_theme_constant_override(side, int(EditorUI.scaled(10.0)))
	erosion_window.add_child(margin)
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)

	var layout: VBoxContainer = VBoxContainer.new()
	layout.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	layout.size_flags_vertical = Control.SIZE_EXPAND_FILL
	margin.add_child(layout)

	var scroll: ScrollContainer = ScrollContainer.new()
	EditorUI.configure_scroll_container(scroll)
	layout.add_child(scroll)

	var footer: HFlowContainer = HFlowContainer.new()
	footer.alignment = FlowContainer.ALIGNMENT_END
	footer.add_theme_constant_override("h_separation", int(EditorUI.scaled(8.0)))
	footer.add_theme_constant_override("v_separation", int(EditorUI.scaled(4.0)))
	layout.add_child(HSeparator.new())
	layout.add_child(footer)

	erosion_panel = ErosionSimulator.new()
	erosion_panel.plugin = self
	erosion_panel.terrain = p_terrain
	erosion_panel.action_host = footer
	erosion_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(erosion_panel)
	add_child(erosion_window)


func open_shader_performance() -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before opening shader quality controls")
		return
	if is_instance_valid(shader_performance_panel):
		shader_performance_panel.open()


func open_autoshader_baker() -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before opening the autoshader baker")
		return
	if is_instance_valid(autoshader_panel):
		autoshader_panel.set_terrain(terrain)
		autoshader_panel.open()
	if is_instance_valid(foliage_panel):
		foliage_panel.set_terrain(terrain)


func convert_selected_to_region_streaming() -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before converting streamed objects")
		return
	if is_instance_valid(streaming_converter):
		streaming_converter.convert_selected()


func convert_selected_to_hlod_visual(min_level: int = 1) -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before converting HLOD visual scenes")
		return
	if is_instance_valid(streaming_converter):
		streaming_converter.convert_selected_as_hlod_visual(min_level)


func open_terrain_settings() -> void:
	var selected_terrain: Terrain3D = get_terrain()
	if not is_instance_valid(selected_terrain):
		push_warning("Select a Terrain3D node before opening Terrain Settings")
		return
	if not is_instance_valid(terrain_settings_window):
		# The optional window is loaded on demand, not during primary plugin startup.
		var window_script: Script = load(TERRAIN_SETTINGS_WINDOW_PATH) as Script
		if window_script == null or not window_script.can_instantiate():
			push_error("Terrain3D Settings window script could not be loaded")
			return
		terrain_settings_window = window_script.new() as Window
		if not is_instance_valid(terrain_settings_window):
			push_error("Terrain3D Settings window could not be instantiated")
			return
		terrain_settings_window.set("plugin", self)
		add_child(terrain_settings_window)
	terrain_settings_window.call("open_tab", "Streaming & VT", selected_terrain)


func open_runtime_dataset_cook_confirmation() -> void:
	if not is_instance_valid(ui) or not is_instance_valid(ui.terrain_menu):
		return
	ui.terrain_menu.open_runtime_cook_confirmation()


func open_region_lightmap_baker() -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before baking region lightmaps")
		return
	if is_instance_valid(region_lightmap_panel):
		region_lightmap_panel.open(terrain)


func open_hlod_panel() -> void:
	if not is_instance_valid(terrain):
		push_error("Select a Terrain3D node before opening HLOD tools")
		return
	if is_instance_valid(hlod_panel):
		hlod_panel.open(terrain)


func get_region_lightmap_baker() -> Node:
	return region_lightmap_baker



func change_tool(p_tool_name: String) -> void:
	if is_instance_valid(ui) and is_instance_valid(ui.toolbar):
		ui.toolbar.change_tool(p_tool_name)


func notify_ui_setting_changed() -> void:
	if is_instance_valid(ui):
		ui._on_setting_changed()


func _open_terrain_tools_menu() -> void:
	if not is_instance_valid(ui) or not is_instance_valid(ui.terrain_menu):
		return
	# Resource inspection detaches painting, but the last terrain remains a valid
	# target for an explicit request to reopen its tools.
	var active_terrain: Terrain3D = get_terrain()
	if is_instance_valid(active_terrain):
		_bind_terrain(active_terrain)
	EditorInterface.set_main_screen_editor("3D")
	ui.set_visible(is_instance_valid(terrain))
	ui.terrain_menu.show()
	ui.terrain_menu.open_menu.call_deferred()



func _on_inspector_mouse_entered() -> void:
	mouse_in_main = false


## EditorPlugin selection function call chain isn't consistent. Here's the map of calls:
## Assume we handle Terrain3D and NavigationRegion3D
# Click Terrain3D: 					_handles(Terrain3D), _edit(Terrain3D), _make_visible(true)
# Deselect:							_edit(null), _make_visible(false)
# Click other node:					_handles(OtherNode)
# Click NavRegion3D:				_handles(NavReg3D), _edit(NavReg3D), _make_visible(true)
# Click NavRegion3D, Terrain3D:		_handles(Terrain3D), _make_visible(true), _edit(Terrain3D)
# Click Terrain3D, NavRegion3D:		_handles(NavReg3D), _make_visible(true), _edit(NavReg3D)
func _handles(p_object: Object) -> bool:
	if p_object is Terrain3D:
		return true
	elif _is_instance_edit_proxy(p_object):
		return true
	elif p_object is NavigationRegion3D and is_instance_valid(_last_terrain):
		return true

	# Terrain3DObjects requires access to EditorUndoRedoManager. The only way to make sure it
	# always has it, is to pass it in here. _edit is NOT called if the node is cut and pasted.
	elif p_object is Terrain3DObjects:
		var objects := p_object as Terrain3DObjects
		objects.editor_setup(self)
	elif p_object is Node3D:
		var node := p_object as Node3D
		var parent := node.get_parent()
		if parent is Terrain3DObjects:
			(parent as Terrain3DObjects).editor_setup(self)

	return false


func _edit(p_object: Object) -> void:
	if !p_object:
		_clear()
		return

	if p_object is Terrain3D:
		_bind_terrain(p_object as Terrain3D)
	elif _is_instance_edit_proxy(p_object) and is_terrain_valid(_last_terrain):
		terrain = _last_terrain
		ui.set_visible(true)
	else:
		_clear()

	if is_terrain_valid(_last_terrain):
		if p_object is NavigationRegion3D:
			ui.set_visible(true, true)
			nav_region = p_object
		else:
			nav_region = null


func _validate_native_editor_build() -> bool:
	var flavor: String = Terrain3DUtil.get_native_build_flavor()
	var revision: int = Terrain3DUtil.get_native_api_revision()
	var build_id: String = Terrain3DUtil.get_native_build_id()
	if flavor == EXPECTED_NATIVE_BUILD_FLAVOR and revision == EXPECTED_NATIVE_API_REVISION \
			and build_id == EXPECTED_NATIVE_BUILD_ID and ClassDB.class_exists("Terrain3DEditor"):
		_native_contract_error_reported = false
		return true
	if not _native_contract_error_reported:
		push_error("Terrain3D editor native contract mismatch. Expected %s API %d (%s), loaded %s API %d (%s). Refusing to initialize authoring tools." % [EXPECTED_NATIVE_BUILD_FLAVOR, EXPECTED_NATIVE_API_REVISION, EXPECTED_NATIVE_BUILD_ID, flavor, revision, build_id])
		_native_contract_error_reported = true
	return false


func _validate_native_contract(p_terrain: Terrain3D) -> bool:
	if not is_instance_valid(p_terrain):
		return false
	var revision: int = p_terrain.get_native_api_revision()
	var build_id: String = p_terrain.get_native_build_id()
	var flavor: String = p_terrain.get_native_build_flavor()
	if flavor != EXPECTED_NATIVE_BUILD_FLAVOR or revision != EXPECTED_NATIVE_API_REVISION or build_id != EXPECTED_NATIVE_BUILD_ID:
		if not _native_contract_error_reported:
			push_error("Terrain3D native/script contract mismatch. Expected %s API %d (%s), loaded %s API %d (%s). Refusing to bind the editor UI." % [EXPECTED_NATIVE_BUILD_FLAVOR, EXPECTED_NATIVE_API_REVISION, EXPECTED_NATIVE_BUILD_ID, flavor, revision, build_id])
			_native_contract_error_reported = true
		return false
	_native_contract_error_reported = false
	return true


func _bind_terrain(p_terrain: Terrain3D) -> void:
	if not _validate_native_contract(p_terrain):
		_detach_current_terrain()
		return
	if p_terrain == terrain:
		# Godot may hide the spatial controls while inspecting another object,
		# then call _edit() again for the same Terrain3D. Restoring the existing
		# binding must also restore its toolbar and tool settings.
		if is_instance_valid(ui):
			ui.set_visible(true)
		return
	_detach_current_terrain()
	terrain = p_terrain
	_last_terrain = terrain
	terrain.set_plugin(self)
	terrain.set_editor(editor)
	# Bind the editor camera as soon as the terrain becomes the active edited
	# object. Waiting for _forward_3d_gui_input() means the native renderer can
	# keep the startup origin-centered clipmap until the user first interacts with
	# the viewport.
	var main_viewport := EditorInterface.get_editor_viewport_3d(0)
	if is_instance_valid(main_viewport):
		var main_camera := main_viewport.get_camera_3d()
		if is_instance_valid(main_camera):
			terrain.set_camera(main_camera)
	debug = terrain.debug_level
	editor.set_terrain(terrain)
	terrain.set_meta("_edit_lock_", true)
	ui.set_visible(true)

	# Get alerted when a new asset list is loaded.
	if is_instance_valid(asset_dock):
		if not terrain.assets_changed.is_connected(asset_dock.update_assets):
			terrain.assets_changed.connect(asset_dock.update_assets)
		asset_dock.update_assets()
	if is_instance_valid(autoshader_panel):
		autoshader_panel.set_terrain(terrain)
	if is_instance_valid(foliage_panel):
		foliage_panel.set_terrain(terrain)
	if is_instance_valid(terrain_settings_window):
		terrain_settings_window.call("set_terrain", terrain)


func _make_visible(p_visible: bool) -> void:
	if debug:
		print("Terrain3DEditorPlugin: _make_visible(%s)" % p_visible)
	if not is_instance_valid(ui):
		return
	if not p_visible:
		if is_instance_valid(editor) and editor.is_operating():
			editor.cancel_operation()
		ui.cancel_landform_stroke()
		ui.clear_picking()
	if p_visible and (is_selected() or is_terrain_valid()):
		# _make_visible() can precede the editor selection update. The bound
		# Terrain3D is still authoritative for a positive visibility request.
		ui.set_visible(true)
		if is_instance_valid(asset_dock):
			asset_dock.update_dock()
	else:
		ui.set_visible(false)


func _clear() -> void:
	if is_instance_valid(ui):
		ui.cancel_landform_stroke()
	_detach_current_terrain()


func _detach_current_terrain() -> void:
	nav_region = null
	viewport = null
	_input_mode = 0
	_instance_edit_mode = false
	_destroy_instance_edit_proxy()
	if is_instance_valid(ui):
		ui.clear_picking()
	if is_instance_valid(editor):
		if editor.is_operating():
			editor.cancel_operation()
		editor.set_tool(int(Terrain3DEditor.TOOL_MAX))
		editor.set_operation(int(Terrain3DEditor.OP_MAX))
		editor.set_terrain(null)

	var previous := terrain
	if is_instance_valid(foliage_panel):
		foliage_panel.set_terrain(null)
	terrain = null
	if is_instance_valid(terrain_settings_window):
		terrain_settings_window.call("set_terrain", null)
	if is_instance_valid(autoshader_panel):
		autoshader_panel.set_terrain(null)
	if not is_instance_valid(previous):
		return

	if is_instance_valid(asset_dock) and previous.assets_changed.is_connected(asset_dock.update_assets):
		previous.assets_changed.disconnect(asset_dock.update_assets)
	previous.set_editor(null)
	previous.set_plugin(null)


func _forward_3d_gui_input(p_viewport_camera: Camera3D, p_event: InputEvent) -> AfterGUIInput:
	mouse_in_main = true
	if not is_terrain_valid():
		return AFTER_GUI_INPUT_PASS

	# Camera/navigation input can return PASS from _read_input(). Hand the active
	# viewport camera to Terrain3D before that early return so a terrain whose
	# physics snapping was disabled during startup can recover without a brush edit.
	terrain.set_camera(p_viewport_camera)

	var continue_input: AfterGUIInput = _read_input(p_event)
	if continue_input != AFTER_GUI_INPUT_CUSTOM:
		return continue_input
	## Setup active camera & viewport
	# Always update this for all inputs, as the mouse position can move without
	# necessarily being a InputEventMouseMotion object. get_intersection() also
	# returns the last frame position, and should be updated more frequently.

	# Detect if viewport is set to half_resolution
	# Structure is: Node3DEditorViewportContainer/Node3DEditorViewport(4)/SubViewportContainer/SubViewport/Camera3D
	viewport = p_viewport_camera.get_viewport() as SubViewport
	var viewport_container := viewport.get_parent() as SubViewportContainer
	var full_resolution: bool = not is_instance_valid(viewport_container) or viewport_container.stretch_shrink != 2

	## Get mouse location on terrain
	# Project 2D mouse position to 3D position and direction
	var vp_mouse_pos: Vector2 = viewport.get_mouse_position()
	var mouse_pos: Vector2 = vp_mouse_pos if full_resolution else vp_mouse_pos / 2
	var camera_pos: Vector3 = p_viewport_camera.project_ray_origin(mouse_pos)
	var camera_dir: Vector3 = p_viewport_camera.project_ray_normal(mouse_pos)

	if _instance_edit_mode:
		return _handle_instance_edit_input(p_event, camera_pos, camera_dir)

	# If region tool, grab mouse position without considering height
	if editor.get_tool() == Terrain3DEditor.REGION:
		var t: float = -Vector3(0, 1, 0).dot(camera_pos) / Vector3(0, 1, 0).dot(camera_dir)
		mouse_global_position = (camera_pos + t * camera_dir)
	else:
	#Else look for intersection with terrain
		var intersection_point: Vector3 = terrain.get_intersection(camera_pos, camera_dir, true)
		if intersection_point.z > 3.4e38 or is_nan(intersection_point.y): # max double or nan
			return AFTER_GUI_INPUT_PASS
		mouse_global_position = intersection_point

	ui.update_decal()

	## Handle mouse movement
	if p_event is InputEventMouseMotion:
		var mouse_motion := p_event as InputEventMouseMotion

		if _input_mode != -1: # Not cam rotation
			## Update region highlight
			var region_position: Vector2 = ( Vector2(mouse_global_position.x, mouse_global_position.z) \
				/ (terrain.get_region_size() * terrain.get_vertex_spacing()) ).floor()

			if _input_mode > 0 and editor.get_tool() == Terrain3DEditor.LANDFORM:
				ui.append_landform_stroke(mouse_global_position)
				return AFTER_GUI_INPUT_STOP

			if _input_mode > 0 and editor.is_operating():
				# Inject pressure - Relies on C++ set_brush_data() using same dictionary instance
				ui.brush_data["mouse_pressure"] = mouse_motion.pressure

				editor.operate(mouse_global_position, p_viewport_camera.rotation.y)
				return AFTER_GUI_INPUT_STOP

		return AFTER_GUI_INPUT_PASS

	if p_event is InputEventMouseButton and _input_mode > 0:
		if p_event.is_pressed():
			if editor.get_tool() == Terrain3DEditor.LANDFORM:
				ui.begin_landform_stroke(mouse_global_position)
				return AFTER_GUI_INPUT_STOP

			# If picking
			if ui.is_picking():
				ui.pick(mouse_global_position)
				if not ui.operation_builder or not ui.operation_builder.is_ready():
					return AFTER_GUI_INPUT_STOP

			if modifier_ctrl and editor.get_tool() == Terrain3DEditor.HEIGHT:
				if not terrain.data.prepare_sampling_area(mouse_global_position):
					return AFTER_GUI_INPUT_STOP
				var height: float = terrain.data.get_height(mouse_global_position)
				ui.brush_data["height"] = height
				ui.tool_settings.set_setting("height", height)

			# If adjusting regions
			if editor.get_tool() == Terrain3DEditor.REGION:
				# Skip regions that already exist or don't
				var has_region: bool = terrain.data.has_regionp(mouse_global_position)
				var op: int = editor.get_operation()
				if	( has_region and op == Terrain3DEditor.ADD) or \
					( not has_region and op == Terrain3DEditor.SUBTRACT ):
					return AFTER_GUI_INPUT_STOP

			# If an automatic operation is ready to go (e.g. gradient)
			if ui.operation_builder and ui.operation_builder.is_ready():
				ui.operation_builder.apply_operation(editor, mouse_global_position, p_viewport_camera.rotation.y)
				return AFTER_GUI_INPUT_STOP

			# Mouse clicked, start editing
			editor.start_operation(mouse_global_position)
			editor.operate(mouse_global_position, p_viewport_camera.rotation.y)
			return AFTER_GUI_INPUT_STOP

		# _input_apply released, save undo data
		elif editor.get_tool() == Terrain3DEditor.LANDFORM:
			ui.finish_landform_stroke(mouse_global_position)
			return AFTER_GUI_INPUT_STOP
		elif editor.is_operating():
			editor.stop_operation()
			return AFTER_GUI_INPUT_STOP

	return AFTER_GUI_INPUT_PASS


func _read_input(p_event: InputEvent = null) -> AfterGUIInput:
	## Determine if user is moving camera or applying.
	var mouse_button: InputEventMouseButton = p_event as InputEventMouseButton
	var released_button: int = MOUSE_BUTTON_NONE
	if mouse_button and not mouse_button.pressed:
		released_button = mouse_button.button_index

	if (
		Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
		or released_button == MOUSE_BUTTON_LEFT
	):
		_input_mode = 1
	else:
		_input_mode = 0

	match get_setting("editors/3d/navigation/navigation_scheme", 0):
		2, 1: # Modo, Maya
			if (
				Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)
				or (
					Input.is_key_pressed(KEY_ALT)
					and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
				)
			):
				_input_mode = -1
			if (
				released_button == MOUSE_BUTTON_RIGHT
				or (
					Input.is_key_pressed(KEY_ALT)
					and released_button == MOUSE_BUTTON_LEFT
				)
			):
				rmb_release_time = Time.get_ticks_msec()
		0, _: # Godot
			if (
				Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)
				or Input.is_mouse_button_pressed(MOUSE_BUTTON_MIDDLE)
			):
				_input_mode = -1
			if released_button in [MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE]:
				rmb_release_time = Time.get_ticks_msec()
	if _input_mode < 0:
		return AFTER_GUI_INPUT_PASS

	# The left-button release can occur outside the 3D viewport. Recover the
	# transaction as soon as input/focus returns so edited flags and undo state do
	# not leak into the next stroke. A real left release keeps _input_mode at 1 and
	# is finalized by _forward_3d_gui_input() below.
	if _input_mode == 0 and is_instance_valid(editor):
		if editor.is_operating() and is_terrain_valid():
			editor.stop_operation()
		if editor.get_tool() == Terrain3DEditor.LANDFORM and is_instance_valid(ui):
			ui.finish_landform_stroke(mouse_global_position)

	## Determine modifiers pressed.
	modifier_shift = Input.is_key_pressed(KEY_SHIFT)

	# Editor responds to modifier_ctrl so we must register touchscreen Invert.
	var ui_inverted: bool = is_instance_valid(ui) and ui.is_input_inverted()
	if _use_meta:
		modifier_ctrl = Input.is_key_pressed(KEY_META) or ui_inverted
	else:
		modifier_ctrl = Input.is_key_pressed(KEY_CTRL) or ui_inverted

	# Keybind enum: Alt, Space, Meta, Caps Lock.
	var alt_key: int
	match get_setting("terrain3d/config/alt_key_bind", 0):
		3: alt_key = KEY_CAPSLOCK
		2: alt_key = KEY_META
		1: alt_key = KEY_SPACE
		0, _: alt_key = KEY_ALT
	modifier_alt = Input.is_key_pressed(alt_key)
	var current_mods: int = (
		int(modifier_shift)
		| int(modifier_ctrl) << 1
		| int(modifier_alt) << 2
	)

	## Instancer edit mode delegates keyboard transform shortcuts to Godot's native
	## Node3D editor. Terrain3D must not steal W/E/S/etc. while the proxy gizmo is active.
	if _instance_edit_mode and p_event is InputEventKey:
		return AFTER_GUI_INPUT_PASS

	## Process hotkeys.
	var key_event: InputEventKey = p_event as InputEventKey
	if (
		key_event
		and current_mods == 0
		and key_event.pressed
		and consume_hotkey(key_event)
	):
		EditorInterface.get_editor_viewport_3d(0).set_input_as_handled()
		return AFTER_GUI_INPUT_STOP

	# Brush data is cleared on set_tool, or clicking textures in the asset dock.
	# Update modifiers if changed or missing.
	if _last_modifiers != current_mods or not ui.brush_data.has("modifier_shift"):
		_last_modifiers = current_mods
		ui.brush_data["modifier_shift"] = modifier_shift
		ui.brush_data["modifier_ctrl"] = modifier_ctrl
		ui.brush_data["modifier_alt"] = modifier_alt
		ui.set_active_operation()

	return AFTER_GUI_INPUT_CUSTOM


# Returns true if hotkey matches and operation triggered
func consume_hotkey(p_event: InputEventKey) -> bool:
	# Handle repeatable keys
	match p_event.keycode:
		KEY_BRACKETLEFT:
			ui.tool_settings.set_setting("size", ui.tool_settings.get_setting("size") - 1)
			return true
		KEY_BRACKETRIGHT:
			ui.tool_settings.set_setting("size", ui.tool_settings.get_setting("size") + 1)
			return true
		KEY_MINUS:
			ui.tool_settings.set_setting("strength", ui.tool_settings.get_setting("strength") - 1)
			return true
		KEY_EQUAL:
			ui.tool_settings.set_setting("strength", ui.tool_settings.get_setting("strength") + 1)
			return true

	if p_event.is_echo():
		return false

	# Handle non-repeatable keys
	match p_event.keycode:
		KEY_ESCAPE:
			if editor.get_tool() == Terrain3DEditor.LANDFORM:
				ui.cancel_landform_stroke()
				return true
			return false
		KEY_1, KEY_KP_1:
			terrain.material.set_show_region_grid(!terrain.material.get_show_region_grid())
		KEY_2, KEY_KP_2:
			terrain.label_distance = 4096.0 if is_zero_approx(terrain.label_distance) else 0.0
		KEY_3, KEY_KP_3:
			terrain.material.set_show_contours(!terrain.material.get_show_contours())
		KEY_4, KEY_KP_4:
			terrain.material.set_show_instancer_grid(!terrain.material.get_show_instancer_grid())
		KEY_5, KEY_KP_5:
			terrain.material.set_show_vertex_grid(!terrain.material.get_show_vertex_grid())
		KEY_E:
			ui.toolbar.change_tool("AddRegion")
		KEY_R:
			ui.toolbar.change_tool("Raise")
		KEY_H:
			ui.toolbar.change_tool("Height")
		KEY_S:
			ui.toolbar.change_tool("Slope")
		KEY_L:
			ui.toolbar.change_tool("LandformStroke")
		KEY_C:
			ui.toolbar.change_tool("PaintColor")
		KEY_N:
			ui.toolbar.change_tool("PaintNavigableArea")
		KEY_I:
			ui.toolbar.change_tool("InstanceMeshes")
		KEY_X:
			ui.toolbar.change_tool("AddHoles")
		KEY_W:
			ui.toolbar.change_tool("PaintWetness")
		KEY_B:
			ui.toolbar.change_tool("PaintTexture")
		KEY_V:
			ui.toolbar.change_tool("SprayTexture")
		KEY_A:
			ui.toolbar.change_tool("EnableMaterialRules")
		KEY_T:
			ui.tool_settings.inverse_slope_range()
		_:
			return false
	return true


func _on_scene_changed(scene_root: Node) -> void:
	if debug:
		print("Terrain3DEditorPlugin: _on_scene_changed: ", scene_root)
	if not scene_root:
		_clear()
		_last_terrain = null
		if is_instance_valid(asset_dock):
			asset_dock.update_assets()
		return

	# Scene tabs retain resources and sometimes nodes longer than the visible
	# viewport lifetime. Never carry the active editor binding into another
	# scene root; otherwise both Terrain3D nodes retain the same native editor
	# object and signal destinations.
	if is_instance_valid(terrain) and terrain != scene_root and not scene_root.is_ancestor_of(terrain):
		_clear()
	if is_instance_valid(_last_terrain) and _last_terrain != scene_root and \
			not scene_root.is_ancestor_of(_last_terrain):
		_last_terrain = null

	for node: Node in scene_root.find_children("", "Terrain3DObjects"):
		var objects := node as Terrain3DObjects
		if is_instance_valid(objects):
			objects.editor_setup(self)

	if is_instance_valid(asset_dock):
		asset_dock.update_assets()
	# Scene changes can retain a selection without emitting selection_changed.
	# Reconcile the UI after the new scene root is active.
	_sync_tools_after_selection.call_deferred()


func get_terrain() -> Terrain3D:
	if is_terrain_valid():
		return terrain
	elif is_instance_valid(_last_terrain) and is_terrain_valid(_last_terrain):
		return _last_terrain
	else:
		return null


func is_terrain_valid(p_terrain: Terrain3D = null) -> bool:
	var t: Terrain3D
	if p_terrain:
		t = p_terrain
	else:
		t = terrain
	var scene_root := EditorInterface.get_edited_scene_root()
	var belongs_to_active_scene := is_instance_valid(t) and is_instance_valid(scene_root) and \
		(t == scene_root or scene_root.is_ancestor_of(t))
	if belongs_to_active_scene and t.is_inside_tree() and t.data:
		return true
	return false


func is_selected() -> bool:
	var selected: Array[Node] = EditorInterface.get_selection().get_selected_nodes()
	for node: Node in selected:
		if ( is_instance_valid(_last_terrain) and node.get_instance_id() == _last_terrain.get_instance_id() ) or \
			node is Terrain3D or _is_instance_edit_proxy(node):
			return true
	return false


func select_terrain() -> void:
	if debug and is_selected():
		print("Terrain3DEditorPlugin: Terrain is selected, skipping")
	if is_instance_valid(_last_terrain) and is_terrain_valid(_last_terrain) and not is_selected():
		var es: EditorSelection = EditorInterface.get_selection()
		if debug:
			print("Terrain3DEditorPlugin: Clearing and reselecting terrain")
		es.clear()
		es.add_node(_last_terrain)


func set_instance_edit_mode(p_enabled: bool) -> void:
	var enabled := p_enabled and is_terrain_valid() and is_instance_valid(editor) and editor.get_tool() == Terrain3DEditor.INSTANCER
	if enabled == _instance_edit_mode:
		return
	_instance_edit_mode = enabled
	if not enabled:
		_clear_instance_edit_selection(true)


func _is_instance_edit_proxy(p_object: Object) -> bool:
	return is_instance_valid(_instance_edit_proxy) and p_object == _instance_edit_proxy


func _is_instance_edit_proxy_selected() -> bool:
	if not is_instance_valid(_instance_edit_proxy):
		return false
	return EditorInterface.get_selection().get_selected_nodes().has(_instance_edit_proxy)


func _ensure_instance_edit_proxy() -> Node3D:
	if is_instance_valid(_instance_edit_proxy):
		return _instance_edit_proxy
	if not is_terrain_valid():
		return null
	_instance_edit_proxy = InstanceTransformProxy.new()
	_instance_edit_proxy.name = "Terrain3D Instance"
	terrain.add_child(_instance_edit_proxy, false, INTERNAL_MODE_BACK)
	_instance_edit_proxy.transform_edited.connect(_on_instance_edit_proxy_transform_edited)
	return _instance_edit_proxy


func _destroy_instance_edit_proxy() -> void:
	if not is_instance_valid(_instance_edit_proxy):
		_instance_edit_proxy = null
		return
	if _instance_edit_proxy.transform_edited.is_connected(_on_instance_edit_proxy_transform_edited):
		_instance_edit_proxy.transform_edited.disconnect(_on_instance_edit_proxy_transform_edited)
	var selection := EditorInterface.get_selection()
	if selection.get_selected_nodes().has(_instance_edit_proxy):
		selection.clear()
	_instance_edit_proxy.queue_free()
	_instance_edit_proxy = null


func _clear_instance_edit_selection(p_reselect_terrain: bool = false) -> void:
	if is_instance_valid(_instance_edit_proxy):
		_instance_edit_proxy.clear_handle()
	var selection := EditorInterface.get_selection()
	if is_instance_valid(_instance_edit_proxy) and selection.get_selected_nodes().has(_instance_edit_proxy):
		selection.clear()
		if p_reselect_terrain and is_terrain_valid():
			selection.add_node(terrain)


func _select_editable_instance(p_handle: Dictionary) -> void:
	if p_handle.is_empty() or not bool(p_handle.get("valid", false)):
		return
	var proxy := _ensure_instance_edit_proxy()
	if not is_instance_valid(proxy):
		return
	proxy.bind_handle(p_handle)
	proxy.name = "Terrain3D Instance %d" % int(p_handle.get("mesh_id", -1))
	if is_instance_valid(asset_dock):
		asset_dock.set_selected_by_asset_id(int(p_handle.get("mesh_id", -1)), Terrain3DAssets.TYPE_MESH)
	var selection := EditorInterface.get_selection()
	selection.clear()
	selection.add_node(proxy)


func _handle_instance_edit_input(p_event: InputEvent, p_ray_origin: Vector3, p_ray_direction: Vector3) -> AfterGUIInput:
	if not is_instance_valid(terrain) or not is_instance_valid(terrain.instancer):
		return AFTER_GUI_INPUT_PASS
	if not (p_event is InputEventMouseButton):
		return AFTER_GUI_INPUT_PASS
	var mouse_button := p_event as InputEventMouseButton
	if mouse_button.button_index != MOUSE_BUTTON_LEFT or not mouse_button.pressed or _input_mode <= 0:
		return AFTER_GUI_INPUT_PASS

	# Once the native transform gizmo owns a selected proxy, normal clicks must pass
	# through to Godot so translate/rotate/scale handles work. Shift-click explicitly
	# asks Terrain3D to ray-pick a different editable instance.
	if _is_instance_edit_proxy_selected() and _instance_edit_proxy.has_handle() and not modifier_shift:
		return AFTER_GUI_INPUT_PASS

	var hit: Dictionary = terrain.instancer.pick_editable_instance(p_ray_origin, p_ray_direction, 4096.0)
	if hit.is_empty():
		return AFTER_GUI_INPUT_PASS
	# Handle identity includes the cell revision. A compacted/reinserted record may
	# reuse the same dense index, but a revision change must rebind the proxy.
	if is_instance_valid(_instance_edit_proxy) and _instance_edit_proxy.is_same_handle(hit):
		return AFTER_GUI_INPUT_PASS
	_select_editable_instance(hit)
	return AFTER_GUI_INPUT_STOP


func _on_instance_edit_proxy_transform_edited(p_global_transform: Transform3D) -> void:
	if not _instance_edit_mode or not is_terrain_valid() or not is_instance_valid(_instance_edit_proxy):
		return
	var handle: Dictionary = _instance_edit_proxy.get_handle()
	if handle.is_empty():
		return
	var updated: Dictionary = terrain.instancer.set_editable_instance_transform(handle, p_global_transform, false)
	if updated.is_empty():
		_instance_edit_proxy.restore_last_valid_transform()
		# Distinguish an ordinary rejected transform from a stale dense-index handle.
		# Native lookup is revision-checked, so an empty refresh means this selection
		# no longer identifies the record it originally bound to.
		var current: Dictionary = terrain.instancer.get_editable_instance(handle)
		if current.is_empty():
			_clear_instance_edit_selection(true)
		return
	_instance_edit_proxy.accept_handle(updated)


## Editor Settings


func setup_editor_settings() -> void:
	editor_settings = EditorInterface.get_editor_settings()
	if not editor_settings.has_setting("terrain3d/config/alt_key_bind"):
		editor_settings.set_setting("terrain3d/config/alt_key_bind", 0)
	var property_info: Dictionary[String, Variant] = {
		"name": "terrain3d/config/alt_key_bind",
		"type": TYPE_INT,
		"hint": PROPERTY_HINT_ENUM,
		"hint_string": "Alt,Space,Meta,Capslock"
	}
	editor_settings.add_property_info(property_info)


func set_setting(p_str: String, p_value: Variant) -> void:
	editor_settings.set_setting(p_str, p_value)


func get_setting(p_str: String, p_default: Variant) -> Variant:
	if editor_settings.has_setting(p_str):
		return editor_settings.get_setting(p_str)
	else:
		return p_default


func has_setting(p_str: String) -> bool:
	return editor_settings.has_setting(p_str)


func erase_setting(p_str: String) -> void:
	editor_settings.erase(p_str)


## Undo / Redo Functions


func create_undo_action(p_action_name: String) -> void:
	get_undo_redo().create_action(p_action_name, UndoRedo.MERGE_DISABLE, terrain)


func add_undo_method(p_method: Callable) -> void:
	var args: Array[Variant] = [ p_method.get_object(), p_method.get_method() ]
	args.append_array(p_method.get_bound_arguments())
	get_undo_redo().add_undo_method.callv(args)


func add_do_method(p_method: Callable) -> void:
	var args: Array[Variant] = [ p_method.get_object(), p_method.get_method() ]
	args.append_array(p_method.get_bound_arguments())
	get_undo_redo().add_do_method.callv(args)


func commit_action(p_execute: bool) -> void:
	get_undo_redo().commit_action(p_execute)


func _validate_runtime_export() -> void:
	var preflight: Script = load(RUNTIME_EXPORT_PREFLIGHT_PATH) as Script
	if preflight == null or not preflight.can_instantiate():
		push_error("Terrain3D runtime export preflight script could not be loaded")
		return
	var failures: Array[String] = preflight.call("validate_project")
	if failures.is_empty():
		print("[Terrain3D][P05] Runtime export preflight passed")
		return
	for failure: String in failures:
		push_error("[Terrain3D][P05] " + failure)
