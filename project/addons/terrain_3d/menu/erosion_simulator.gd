# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Terrain3D erosion simulation editor workflow.
@tool
extends VBoxContainer

const ErosionPresets = preload("res://addons/terrain_3d/menu/erosion_presets.gd")
const ErosionProfileEditor = preload("res://addons/terrain_3d/menu/erosion_texture_profiles.gd")
const ErosionPreviewWindow = preload("res://addons/terrain_3d/menu/erosion_preview_window.gd")
const EditorUI = preload("res://addons/terrain_3d/src/editor_ui_util.gd")
const SettingMetadata = preload("res://addons/terrain_3d/src/setting_metadata.gd")
const ESTIMATED_BYTES_PER_CELL: float = 176.0
const CUSTOM_PRESET_INDEX: int = 1000
const ES_EROSION_PANEL_FOLDED: String = "terrain3d/erosion/panel_folded"
const QUALITY_NAMES: PackedStringArray = ["Draft", "Preview", "Balanced", "High", "Full Resolution"]
const QUALITY_SCALES: PackedInt32Array = [16, 8, 4, 2, 1]
const QUALITY_STEPS: PackedInt32Array = [32, 64, 128, 256, 512]
const OUTPUT_NAMES: PackedStringArray = [
	"Height Delta",
	"Flow Accumulation",
	"Total Suspended Sediment",
	"Coarse Sediment",
	"Sand Sediment",
	"Fine Sediment",
	"Loose Surface Material",
	"Oxidation",
	"Weathering",
	"Hydraulic Deposition",
	"Alluvial Fan Deposition",
	"Debris-Flow Deposition",
	"Wind Deposition",
	"Dune Deposition",
	"Wave Deposition",
	"Salt Weathering",
	"Wet-Dry Cycles",
	"Exposure Age",
	"Snowpack",
	"Desert Pavement",
	"Stratigraphic Erosion Depth",
	"Traffic",
	"Material Transition",
]
const OUTPUT_IDS: PackedInt32Array = [
	Terrain3DErosionJob.OUTPUT_HEIGHT_DELTA,
	Terrain3DErosionJob.OUTPUT_FLOW_ACCUMULATION,
	Terrain3DErosionJob.OUTPUT_SEDIMENT,
	Terrain3DErosionJob.OUTPUT_COARSE_SEDIMENT,
	Terrain3DErosionJob.OUTPUT_SAND_SEDIMENT,
	Terrain3DErosionJob.OUTPUT_FINE_SEDIMENT,
	Terrain3DErosionJob.OUTPUT_LOOSE_MATERIAL,
	Terrain3DErosionJob.OUTPUT_OXIDATION,
	Terrain3DErosionJob.OUTPUT_WEATHERING,
	Terrain3DErosionJob.OUTPUT_HYDRAULIC_DEPOSITION,
	Terrain3DErosionJob.OUTPUT_ALLUVIAL_DEPOSITION,
	Terrain3DErosionJob.OUTPUT_DEBRIS_FLOW_DEPOSITION,
	Terrain3DErosionJob.OUTPUT_WIND_DEPOSITION,
	Terrain3DErosionJob.OUTPUT_DUNE_DEPOSITION,
	Terrain3DErosionJob.OUTPUT_WAVE_DEPOSITION,
	Terrain3DErosionJob.OUTPUT_SALT_WEATHERING,
	Terrain3DErosionJob.OUTPUT_WET_DRY_CYCLES,
	Terrain3DErosionJob.OUTPUT_EXPOSURE_AGE,
	Terrain3DErosionJob.OUTPUT_SNOWPACK,
	Terrain3DErosionJob.OUTPUT_DESERT_PAVEMENT,
	Terrain3DErosionJob.OUTPUT_STRATIGRAPHY,
	Terrain3DErosionJob.OUTPUT_TRAFFIC,
	Terrain3DErosionJob.OUTPUT_MATERIAL_ID,
]

class ColorizedOutput extends RefCounted:
	var image: Image
	var minimum: float = 0.0
	var maximum: float = 0.0


class PanelParts:
	var panel: PanelContainer
	var body: VBoxContainer

	func _init(p_title: String) -> void:
		panel = PanelContainer.new()
		panel.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
		var margin: MarginContainer = MarginContainer.new()
		margin.add_theme_constant_override("margin_left", 10)
		margin.add_theme_constant_override("margin_top", 8)
		margin.add_theme_constant_override("margin_right", 10)
		margin.add_theme_constant_override("margin_bottom", 8)
		panel.add_child(margin)
		body = VBoxContainer.new()
		body.add_theme_constant_override("separation", 6)
		margin.add_child(body)
		var title: Label = Label.new()
		title.text = p_title
		title.add_theme_font_size_override("font_size", 16)
		body.add_child(title)


var plugin: Terrain3DEditorPluginBase
var terrain: Terrain3D

var _section: FoldableContainer
var _content_root: VBoxContainer
var _terrain_label: Label
var _tabs: TabContainer
var _settings_picker: EditorResourcePicker
var _preset_picker: OptionButton
var _preset_description: Label
var _quality_picker: OptionButton
var _years_spin: SpinBox
var _intensity_spin: SpinBox
var _seed_spin: SpinBox
var _process_toggles: Dictionary[String, CheckButton] = {}
var _warning_label: RichTextLabel
var _workload_label: Label
var _run_preview_button: Button
var _run_button: Button
var _cancel_button: Button
var _apply_button: Button
var _progress_bar: ProgressBar
var _stage_label: Label
var _live_preview_toggle: CheckButton
var _preview_interval_spin: SpinBox
var _preview_start_paused_toggle: CheckButton
var _open_preview_scene_button: Button
var _pause_preview_button: Button
var _step_preview_button: Button
var _simulation_step_label: Label
var _preview_window: ErosionPreviewWindow
var _last_preview_revision: int = -1
var _preview_refresh_accumulator: float = 0.0
var _summary_label: RichTextLabel
var _region_picker: OptionButton
var _output_picker: OptionButton
var _full_resolution_toggle: CheckButton
var _preview_texture: TextureRect
var _preview_range_label: Label
var _export_button: Button
var _copy_stats_button: Button
var _save_dialog: EditorFileDialog
var _export_dialog: EditorFileDialog
var _apply_confirmation: ConfirmationDialog
var _thread: Thread
var _job: Terrain3DErosionJob
var _settings: Terrain3DErosionSettings
var _active_run_settings: Terrain3DErosionSettings
var _thread_joined: bool = true
var _job_is_preview: bool = false
var _syncing_controls: bool = false
var _last_preview_image: Image
var _last_raw_output_image: Image
var _profile_editor: Terrain3DErosionTextureProfiles
var _advanced_foldable: FoldableContainer
var _advanced_inspector: EditorInspector


func _enter_tree() -> void:
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	if is_instance_valid(plugin):
		_settings = plugin.get_erosion_settings(_get_terrain())
	if not is_instance_valid(_settings):
		_settings = Terrain3DErosionSettings.new()
	_build_inspector_panel()
	_sync_quick_controls_from_settings()
	set_process(false)


func _exit_tree() -> void:
	_cancel_and_join()
	if is_instance_valid(_preview_window):
		_preview_window.queue_free()
	_preview_window = null


func is_editor_ready() -> bool:
	return is_instance_valid(_section)


func _get_terrain() -> Terrain3D:
	if is_instance_valid(terrain):
		return terrain
	if is_instance_valid(plugin):
		var plugin_terrain: Terrain3D = plugin.terrain
		if is_instance_valid(plugin_terrain):
			return plugin_terrain
	return null


func _store_settings() -> void:
	if is_instance_valid(plugin):
		plugin.set_erosion_settings(_get_terrain(), _settings)


func _build_inspector_panel() -> void:
	_section = FoldableContainer.new()
	_section.title = "Terrain Erosion"
	_section.tooltip_text = "Simulate erosion for this Terrain3D node without leaving the Inspector."
	_section.folded = bool(plugin.get_setting(ES_EROSION_PANEL_FOLDED, false)) if is_instance_valid(plugin) else false
	_section.folding_changed.connect(_on_section_folding_changed)
	_section.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_section.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	add_child(_section)

	var margin: MarginContainer = MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 6)
	margin.add_theme_constant_override("margin_top", 6)
	margin.add_theme_constant_override("margin_right", 6)
	margin.add_theme_constant_override("margin_bottom", 8)
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_section.add_child(margin)

	_content_root = VBoxContainer.new()
	_content_root.add_theme_constant_override("separation", 8)
	_content_root.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_content_root.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	margin.add_child(_content_root)

	_build_context_header(_content_root)
	_build_resource_toolbar(_content_root)

	_tabs = TabContainer.new()
	_tabs.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_tabs.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	_content_root.add_child(_tabs)
	_build_setup_tab()
	_profile_editor = ErosionProfileEditor.new() as Terrain3DErosionTextureProfiles
	_profile_editor.plugin = plugin
	_profile_editor.terrain = _get_terrain()
	_profile_editor.inspector_embedded = true
	_profile_editor.name = "Materials"
	_profile_editor.profiles_changed.connect(_on_texture_profiles_changed)
	_tabs.add_child(_profile_editor)
	_build_results_tab()

	var action_row: HFlowContainer = HFlowContainer.new()
	action_row.alignment = FlowContainer.ALIGNMENT_END
	action_row.add_theme_constant_override("h_separation", 8)
	action_row.add_theme_constant_override("v_separation", 4)
	_content_root.add_child(action_row)

	_run_preview_button = Button.new()
	_run_preview_button.text = "Draft Preview"
	_run_preview_button.tooltip_text = "Runs a temporary coarse simulation. Preview results cannot be applied."
	_run_preview_button.pressed.connect(_start_preview_simulation)
	action_row.add_child(_run_preview_button)

	_run_button = Button.new()
	_run_button.text = "Run Final"
	_run_button.tooltip_text = "Runs using the selected settings and quality."
	_run_button.pressed.connect(_start_final_simulation)
	action_row.add_child(_run_button)

	_cancel_button = Button.new()
	_cancel_button.text = "Cancel"
	_cancel_button.pressed.connect(_cancel_simulation)
	action_row.add_child(_cancel_button)

	_apply_button = Button.new()
	_apply_button.text = "Apply..."
	_apply_button.tooltip_text = "Applies the completed result as one Terrain3D undo operation."
	_apply_button.pressed.connect(_request_apply_simulation)
	action_row.add_child(_apply_button)

	_build_file_dialogs()
	_apply_accessible_control_sizes(_content_root)


func _on_section_folding_changed(p_is_folded: bool) -> void:
	if is_instance_valid(plugin):
		plugin.set_setting(ES_EROSION_PANEL_FOLDED, p_is_folded)


func _apply_accessible_control_sizes(p_root: Node) -> void:
	for child: Node in p_root.find_children("*", "", true, false):
		if child is BaseButton or child is LineEdit or child is SpinBox or child is OptionButton:
			EditorUI.configure_compact_control(child as Control)


func _build_context_header(p_root: VBoxContainer) -> void:
	_terrain_label = Label.new()
	_terrain_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_terrain_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	p_root.add_child(_terrain_label)

	var actions: HFlowContainer = HFlowContainer.new()
	actions.add_theme_constant_override("h_separation", 8)
	actions.add_theme_constant_override("v_separation", 4)
	p_root.add_child(actions)

	var refresh_button: Button = Button.new()
	refresh_button.text = "Refresh"
	refresh_button.tooltip_text = "Refresh terrain, texture-profile, validation, and workload information."
	refresh_button.pressed.connect(_refresh_editor_context)
	actions.add_child(refresh_button)

	var help_button: Button = Button.new()
	help_button.text = "Workflow Help"
	help_button.tooltip_text = "Show the recommended non-destructive erosion workflow."
	help_button.pressed.connect(_show_workflow_help)
	actions.add_child(help_button)


func _build_resource_toolbar(p_root: VBoxContainer) -> void:
	var resource_row: HBoxContainer = HBoxContainer.new()
	resource_row.add_theme_constant_override("separation", 6)
	p_root.add_child(resource_row)

	var settings_label: Label = Label.new()
	settings_label.text = "Settings"
	EditorUI.configure_form_label(settings_label, 100.0)
	resource_row.add_child(settings_label)

	_settings_picker = EditorResourcePicker.new()
	_settings_picker.base_type = "Terrain3DErosionSettings"
	_settings_picker.tooltip_text = "Terrain3DErosionSettings resource edited and used by this erosion workflow. Every advanced property in the embedded Inspector carries the generated operational setting documentation."
	_settings_picker.edited_resource = _settings
	EditorUI.configure_field_control(_settings_picker, 260.0)
	_settings_picker.resource_changed.connect(_on_settings_changed)
	_settings_picker.resource_selected.connect(_on_settings_selected)
	resource_row.add_child(_settings_picker)

	var actions: HFlowContainer = HFlowContainer.new()
	actions.add_theme_constant_override("h_separation", 6)
	actions.add_theme_constant_override("v_separation", 4)
	p_root.add_child(actions)

	var new_button: Button = Button.new()
	new_button.text = "New"
	new_button.tooltip_text = "Create a fresh settings resource."
	new_button.pressed.connect(_new_settings)
	actions.add_child(new_button)

	var duplicate_button: Button = Button.new()
	duplicate_button.text = "Duplicate"
	duplicate_button.tooltip_text = "Duplicate the current settings and all curves."
	duplicate_button.pressed.connect(_duplicate_settings)
	actions.add_child(duplicate_button)

	var save_button: Button = Button.new()
	save_button.text = "Save As..."
	save_button.tooltip_text = "Save these erosion settings as a reusable .tres resource."
	save_button.pressed.connect(_save_settings_as)
	actions.add_child(save_button)

	var inspect_button: Button = Button.new()
	inspect_button.text = "Advanced Settings"
	inspect_button.tooltip_text = "Open every simulation parameter and timeline curve in the Inspector."
	inspect_button.pressed.connect(_inspect_settings)
	actions.add_child(inspect_button)


func _build_setup_tab() -> void:
	var setup: VBoxContainer = VBoxContainer.new()
	setup.name = "Simulation"
	setup.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	setup.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	setup.add_theme_constant_override("separation", 10)
	_tabs.add_child(setup)

	var intro: Label = Label.new()
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	intro.text = (
		"Choose a production preset, adjust the common controls, then run a draft preview. "
		+ "Advanced parameters, masks, material transitions, and timeline curves remain available "
		+ "through the settings resource."
	)
	setup.add_child(intro)

	var preset_panel: PanelParts = _make_panel("Starting Point")
	setup.add_child(preset_panel.panel)
	var preset_grid: GridContainer = GridContainer.new()
	EditorUI.configure_form_grid(preset_grid)
	preset_panel.body.add_child(preset_grid)

	preset_grid.add_child(_make_field_label("Preset"))
	_preset_picker = OptionButton.new()
	for preset_name: String in ErosionPresets.get_names():
		_preset_picker.add_item(preset_name)
	_preset_picker.add_separator()
	_preset_picker.add_item("Custom", CUSTOM_PRESET_INDEX)
	_preset_picker.tooltip_text = "Starting parameter set for Terrain3DErosionSettings. Selecting a preset writes the corresponding simulation values; Custom preserves the current edited values."
	_preset_picker.item_selected.connect(_on_preset_selected)
	EditorUI.configure_field_control(_preset_picker, 280.0)
	preset_grid.add_child(_preset_picker)

	preset_grid.add_child(_make_field_label("Quality"))
	_quality_picker = OptionButton.new()
	for index: int in QUALITY_NAMES.size():
		_quality_picker.add_item(
			"%s — 1:%d, %d steps" % [QUALITY_NAMES[index], QUALITY_SCALES[index], QUALITY_STEPS[index]]
		)
	_quality_picker.add_item("Custom")
	_quality_picker.tooltip_text = "Simulation resolution/work preset. Coarser ratios process fewer cells and steps; Full Resolution uses source terrain density and the highest listed step count."
	_quality_picker.item_selected.connect(_on_quality_selected)
	EditorUI.configure_field_control(_quality_picker, 280.0)
	preset_grid.add_child(_quality_picker)

	_preset_description = Label.new()
	_preset_description.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_preset_description.text = ErosionPresets.get_description(0)
	preset_panel.body.add_child(_preset_description)

	var common_panel: PanelParts = _make_panel("Common Controls")
	setup.add_child(common_panel.panel)
	var common_grid: GridContainer = GridContainer.new()
	EditorUI.configure_form_grid(common_grid)
	common_panel.body.add_child(common_grid)

	common_grid.add_child(_make_field_label("Simulated years"))
	_years_spin = _make_spin(0.001, 1000000000.0, 1.0, true)
	_years_spin.tooltip_text = "Elapsed modeled time. Rates and curves are integrated over this duration."
	_years_spin.value_changed.connect(_on_years_changed)
	common_grid.add_child(_years_spin)

	common_grid.add_child(_make_field_label("Global intensity"))
	_intensity_spin = _make_spin(0.0, 10.0, 0.05, false)
	_intensity_spin.tooltip_text = "Scales all active processes without destroying their relative balance."
	_intensity_spin.value_changed.connect(_on_intensity_changed)
	common_grid.add_child(_intensity_spin)

	common_grid.add_child(_make_field_label("Deterministic seed"))
	var seed_row: HBoxContainer = HBoxContainer.new()
	seed_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	seed_row.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	_seed_spin = _make_spin(1.0, 2147483647.0, 1.0, true)
	_seed_spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_seed_spin.tooltip_text = SettingMetadata.tooltip("Terrain3DErosionSettings", &"seed")
	_seed_spin.value_changed.connect(_on_seed_changed)
	seed_row.add_child(_seed_spin)
	var randomize_button: Button = Button.new()
	randomize_button.text = "Randomize"
	randomize_button.tooltip_text = "Choose a new deterministic erosion seed. Reusing the same seed with identical settings reproduces the stochastic inputs."
	EditorUI.configure_action_button(randomize_button)
	randomize_button.pressed.connect(_randomize_seed)
	seed_row.add_child(randomize_button)
	common_grid.add_child(seed_row)

	var process_panel: PanelParts = _make_panel("Processes and Outputs")
	setup.add_child(process_panel.panel)
	var process_grid: GridContainer = GridContainer.new()
	process_grid.columns = 2
	process_grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	process_grid.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	process_grid.add_theme_constant_override("h_separation", int(EditorUI.scaled(16.0)))
	process_grid.add_theme_constant_override("v_separation", int(EditorUI.scaled(6.0)))
	process_panel.body.add_child(process_grid)
	_add_process_toggle(process_grid, "hydraulic_enabled", "Water Flow")
	_add_process_toggle(process_grid, "weathering_enabled", "Weathering")
	_add_process_toggle(process_grid, "oxidation_enabled", "Oxidation")
	_add_process_toggle(process_grid, "wind_enabled", "Wind")
	_add_process_toggle(process_grid, "wave_enabled", "Waves")
	_add_process_toggle(process_grid, "traffic_enabled", "Foot Traffic")
	_add_process_toggle(process_grid, "material_output_enabled", "Material Transitions")
	_add_process_toggle(process_grid, "sediment_sorting_enabled", "Sorted Sediment")
	_add_process_toggle(process_grid, "alluvial_fan_enabled", "Alluvial Fans")
	_add_process_toggle(process_grid, "debris_flow_enabled", "Debris Flows")
	_add_process_toggle(process_grid, "dune_enabled", "Dune Avalanching")
	_add_process_toggle(process_grid, "desert_pavement_enabled", "Desert Pavement")
	_add_process_toggle(process_grid, "salt_weathering_enabled", "Salt Weathering")
	_add_process_toggle(process_grid, "wet_dry_enabled", "Wet-Dry Cycling")
	_add_process_toggle(process_grid, "exposure_age_enabled", "Exposure / Varnish")
	_add_process_toggle(process_grid, "snowmelt_enabled", "Snowmelt")
	_add_process_toggle(process_grid, "stratigraphy_enabled", "Layered Substrate")
	_add_process_toggle(process_grid, "oxidation_color_enabled", "Oxidation Tint")

	var profile_button: Button = Button.new()
	profile_button.text = "Open Material Profiles"
	profile_button.tooltip_text = "Open the embedded material-profile editor."
	profile_button.pressed.connect(_open_texture_profiles)
	process_panel.body.add_child(profile_button)

	var scope_panel: PanelParts = _make_panel("Scope Shortcuts")
	setup.add_child(scope_panel.panel)
	var scope_row: HFlowContainer = HFlowContainer.new()
	scope_row.add_theme_constant_override("h_separation", 8)
	scope_row.add_theme_constant_override("v_separation", 4)
	scope_panel.body.add_child(scope_row)
	var use_mask_bounds_button: Button = Button.new()
	use_mask_bounds_button.text = "Fit Simulation Mask to Terrain"
	use_mask_bounds_button.tooltip_text = "Sets the simulation mask world rectangle to the active terrain bounds."
	use_mask_bounds_button.pressed.connect(_fit_simulation_mask_to_terrain)
	scope_row.add_child(use_mask_bounds_button)
	var use_traffic_bounds_button: Button = Button.new()
	use_traffic_bounds_button.text = "Fit Traffic Mask to Terrain"
	use_traffic_bounds_button.tooltip_text = "Sets the traffic mask world rectangle to the active terrain bounds."
	use_traffic_bounds_button.pressed.connect(_fit_traffic_mask_to_terrain)
	scope_row.add_child(use_traffic_bounds_button)
	var use_desert_bounds_button: Button = Button.new()
	use_desert_bounds_button.text = "Fit Desert Input Maps to Terrain"
	use_desert_bounds_button.tooltip_text = (
		"Sets salinity, surface-layer depth, and bedrock-hardness map rectangles "
		+ "to the active terrain bounds."
	)
	use_desert_bounds_button.pressed.connect(_fit_desert_input_maps_to_terrain)
	scope_row.add_child(use_desert_bounds_button)
	var clear_scope_button: Button = Button.new()
	clear_scope_button.text = "Clear Spatial Limits"
	clear_scope_button.tooltip_text = "Disables altitude limiting and clears edge fading. It does not remove assigned masks."
	clear_scope_button.pressed.connect(_clear_spatial_limits)
	scope_row.add_child(clear_scope_button)

	_warning_label = RichTextLabel.new()
	_warning_label.bbcode_enabled = true
	_warning_label.fit_content = true
	_warning_label.custom_minimum_size.y = 68.0
	setup.add_child(_warning_label)

	_workload_label = Label.new()
	_workload_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	setup.add_child(_workload_label)

	_advanced_foldable = FoldableContainer.new()
	_advanced_foldable.title = "Advanced Erosion Settings and Curves"
	_advanced_foldable.folded = true
	_advanced_foldable.tooltip_text = "Edit the complete Terrain3DErosionSettings resource inline."
	setup.add_child(_advanced_foldable)

	_advanced_inspector = EditorInspector.create_default_inspector()
	if is_instance_valid(_advanced_inspector):
		_advanced_inspector.custom_minimum_size.y = EditorUI.scaled(360.0)
		_advanced_inspector.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_advanced_inspector.edit(_settings)
		_advanced_inspector.property_edited.connect(_on_advanced_property_edited)
		_advanced_foldable.add_child(_advanced_inspector)
	else:
		var inspector_error: Label = Label.new()
		inspector_error.text = "The embedded settings inspector could not be created."
		inspector_error.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		_advanced_foldable.add_child(inspector_error)


func _build_results_tab() -> void:
	var results: VBoxContainer = VBoxContainer.new()
	results.name = "Results"
	results.add_theme_constant_override("separation", 8)
	_tabs.add_child(results)

	var status_panel: PanelContainer = PanelContainer.new()
	status_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	status_panel.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	results.add_child(status_panel)
	var status_margin: MarginContainer = MarginContainer.new()
	status_margin.add_theme_constant_override("margin_left", 10)
	status_margin.add_theme_constant_override("margin_top", 8)
	status_margin.add_theme_constant_override("margin_right", 10)
	status_margin.add_theme_constant_override("margin_bottom", 8)
	status_panel.add_child(status_margin)
	var status_box: VBoxContainer = VBoxContainer.new()
	status_box.add_theme_constant_override("separation", 5)
	status_margin.add_child(status_box)

	_stage_label = Label.new()
	_stage_label.text = "Ready"
	_stage_label.add_theme_font_size_override("font_size", int(EditorUI.scaled(15.0)))
	_stage_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	status_box.add_child(_stage_label)

	_progress_bar = ProgressBar.new()
	_progress_bar.min_value = 0.0
	_progress_bar.max_value = 100.0
	_progress_bar.value = 0.0
	_progress_bar.show_percentage = true
	EditorUI.configure_compact_control(_progress_bar)
	status_box.add_child(_progress_bar)

	var scene_preview_section: FoldableContainer = FoldableContainer.new()
	scene_preview_section.title = "Live 3D Preview — Temporary 3D Simulation Scene"
	scene_preview_section.folded = false
	results.add_child(scene_preview_section)
	var scene_preview_box: VBoxContainer = VBoxContainer.new()
	scene_preview_box.add_theme_constant_override("separation", 6)
	scene_preview_section.add_child(scene_preview_box)

	var preview_explanation: Label = Label.new()
	preview_explanation.text = (
		"Inspect the complete simulated terrain in a disposable scene. "
		+ "Nothing is written to the authored terrain until Apply is confirmed."
	)
	preview_explanation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	scene_preview_box.add_child(preview_explanation)

	var preview_options: GridContainer = GridContainer.new()
	EditorUI.configure_form_grid(preview_options)
	scene_preview_box.add_child(preview_options)
	preview_options.add_child(_make_field_label("Live snapshots"))
	_live_preview_toggle = CheckButton.new()
	_live_preview_toggle.text = "Publish while simulating"
	_live_preview_toggle.tooltip_text = (
		"Retains one extra float height grid and publishes it at the selected step interval."
	)
	_live_preview_toggle.button_pressed = true
	_live_preview_toggle.toggled.connect(_on_live_preview_toggled)
	EditorUI.configure_field_control(_live_preview_toggle, 220.0)
	preview_options.add_child(_live_preview_toggle)

	preview_options.add_child(_make_field_label("Snapshot interval"))
	_preview_interval_spin = _make_spin(1.0, 256.0, 1.0, false)
	_preview_interval_spin.value = 4.0
	_preview_interval_spin.tooltip_text = "Number of completed simulation steps between live-preview publications. Higher values reduce editor preview overhead; lower values update the temporary mesh more often."
	_preview_interval_spin.suffix = " steps"
	_preview_interval_spin.value_changed.connect(_on_preview_interval_changed)
	preview_options.add_child(_preview_interval_spin)

	preview_options.add_child(_make_field_label("Stepped start"))
	_preview_start_paused_toggle = CheckButton.new()
	_preview_start_paused_toggle.text = "Pause before the first step"
	_preview_start_paused_toggle.tooltip_text = (
		"Starts the worker at step zero so Advance One Step can inspect the simulation deterministically."
	)
	_preview_start_paused_toggle.toggled.connect(_on_preview_start_paused_toggled)
	EditorUI.configure_field_control(_preview_start_paused_toggle, 220.0)
	preview_options.add_child(_preview_start_paused_toggle)

	var preview_actions: HFlowContainer = HFlowContainer.new()
	preview_actions.add_theme_constant_override("h_separation", 6)
	preview_actions.add_theme_constant_override("v_separation", 4)
	scene_preview_box.add_child(preview_actions)
	_open_preview_scene_button = Button.new()
	_open_preview_scene_button.text = "Open Temporary 3D Preview"
	_open_preview_scene_button.tooltip_text = "Open an isolated scene containing the complete simulated terrain."
	_open_preview_scene_button.pressed.connect(_open_temporary_preview_scene)
	EditorUI.configure_action_button(_open_preview_scene_button)
	preview_actions.add_child(_open_preview_scene_button)
	_pause_preview_button = Button.new()
	_pause_preview_button.text = "Pause"
	_pause_preview_button.tooltip_text = "Pause at the next completed simulation step."
	_pause_preview_button.pressed.connect(_toggle_preview_pause)
	EditorUI.configure_action_button(_pause_preview_button)
	preview_actions.add_child(_pause_preview_button)
	_step_preview_button = Button.new()
	_step_preview_button.text = "Advance One Step"
	_step_preview_button.tooltip_text = "Pause the worker and execute exactly one additional simulation step."
	_step_preview_button.pressed.connect(_advance_preview_step)
	EditorUI.configure_action_button(_step_preview_button)
	preview_actions.add_child(_step_preview_button)

	_simulation_step_label = Label.new()
	_simulation_step_label.text = "No simulation snapshot available."
	_simulation_step_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	scene_preview_box.add_child(_simulation_step_label)

	var summary_section: FoldableContainer = FoldableContainer.new()
	summary_section.title = "Simulation Summary"
	summary_section.folded = false
	results.add_child(summary_section)
	var summary_box: VBoxContainer = VBoxContainer.new()
	summary_box.add_theme_constant_override("separation", 6)
	summary_section.add_child(summary_box)
	_summary_label = RichTextLabel.new()
	_summary_label.bbcode_enabled = true
	_summary_label.fit_content = true
	_summary_label.custom_minimum_size.y = 120.0
	_summary_label.text = _idle_summary()
	summary_box.add_child(_summary_label)

	_copy_stats_button = Button.new()
	_copy_stats_button.text = "Copy Statistics"
	_copy_stats_button.pressed.connect(_copy_statistics)
	summary_box.add_child(_copy_stats_button)

	var preview_section: FoldableContainer = FoldableContainer.new()
	preview_section.title = "Diagnostic Map"
	preview_section.folded = true
	results.add_child(preview_section)
	var preview_box: VBoxContainer = VBoxContainer.new()
	preview_box.add_theme_constant_override("separation", 6)
	preview_section.add_child(preview_box)

	var preview_controls: HFlowContainer = HFlowContainer.new()
	preview_controls.add_theme_constant_override("h_separation", 6)
	preview_controls.add_theme_constant_override("v_separation", 4)
	preview_box.add_child(preview_controls)
	_region_picker = OptionButton.new()
	_region_picker.tooltip_text = "Simulation region to inspect."
	_region_picker.item_selected.connect(_on_preview_selection_changed)
	preview_controls.add_child(_region_picker)
	_output_picker = OptionButton.new()
	for index: int in OUTPUT_NAMES.size():
		_output_picker.add_item(OUTPUT_NAMES[index], OUTPUT_IDS[index])
	_output_picker.tooltip_text = "Diagnostic erosion output shown for the selected region. This changes only the Results preview/export channel and does not alter the simulation."
	_output_picker.item_selected.connect(_on_preview_selection_changed)
	preview_controls.add_child(_output_picker)
	_full_resolution_toggle = CheckButton.new()
	_full_resolution_toggle.text = "Full-resolution map"
	_full_resolution_toggle.tooltip_text = "Interpolates the diagnostic map to source resolution. This can be slower."
	_full_resolution_toggle.toggled.connect(_on_preview_resolution_toggled)
	preview_controls.add_child(_full_resolution_toggle)

	_preview_texture = TextureRect.new()
	_preview_texture.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_preview_texture.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	_preview_texture.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	_preview_texture.custom_minimum_size = Vector2(220.0, 180.0)
	_preview_texture.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	preview_box.add_child(_preview_texture)

	_preview_range_label = Label.new()
	_preview_range_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_preview_range_label.text = "No diagnostic map available."
	preview_box.add_child(_preview_range_label)

	_export_button = Button.new()
	_export_button.text = "Export Selected Map..."
	_export_button.tooltip_text = "EXR preserves numeric values; PNG exports the colorized preview."
	_export_button.pressed.connect(_export_selected_map)
	preview_box.add_child(_export_button)


func _build_file_dialogs() -> void:
	_save_dialog = EditorFileDialog.new()
	_save_dialog.title = "Save Erosion Settings"
	_save_dialog.access = FileDialog.ACCESS_RESOURCES
	_save_dialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE
	_save_dialog.filters = PackedStringArray(["*.tres ; Terrain3D erosion settings"])
	_save_dialog.file_selected.connect(_on_save_settings_selected)
	add_child(_save_dialog)
	EditorUI.configure_file_dialog(_save_dialog)

	_export_dialog = EditorFileDialog.new()
	_export_dialog.title = "Export Erosion Diagnostic Map"
	_export_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_export_dialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE
	_export_dialog.filters = PackedStringArray(
		[
			"*.exr ; OpenEXR numeric map",
			"*.png ; Colorized PNG preview",
		]
	)
	_export_dialog.file_selected.connect(_on_export_path_selected)
	add_child(_export_dialog)
	EditorUI.configure_file_dialog(_export_dialog)

	_apply_confirmation = ConfirmationDialog.new()
	_apply_confirmation.title = "Apply Erosion Result"
	_apply_confirmation.ok_button_text = "Apply as Undoable Edit"
	_apply_confirmation.min_size = Vector2i(int(EditorUI.scaled(480.0)), int(EditorUI.scaled(260.0)))
	_apply_confirmation.confirmed.connect(_apply_simulation_confirmed)
	add_child(_apply_confirmation)


func _make_panel(p_title: String) -> PanelParts:
	return PanelParts.new(p_title)


func _make_field_label(p_text: String) -> Label:
	var label: Label = Label.new()
	label.text = p_text
	EditorUI.configure_form_label(label, 180.0)
	return label


func _make_spin(p_min: float, p_max: float, p_step: float, p_allow_greater: bool) -> SpinBox:
	var spin: SpinBox = SpinBox.new()
	spin.min_value = p_min
	spin.max_value = p_max
	spin.step = p_step
	spin.allow_greater = p_allow_greater
	EditorUI.configure_field_control(spin, 180.0)
	return spin


func _add_process_toggle(p_parent: GridContainer, p_property: String, p_text: String) -> void:
	var toggle: CheckButton = CheckButton.new()
	toggle.text = p_text
	toggle.tooltip_text = SettingMetadata.tooltip("Terrain3DErosionSettings", StringName(p_property))
	EditorUI.configure_compact_control(toggle)
	toggle.toggled.connect(_on_process_toggled.bind(p_property))
	_process_toggles[p_property] = toggle
	p_parent.add_child(toggle)


func _open_texture_profiles() -> void:
	if _profile_editor:
		_profile_editor.refresh()
		_tabs.current_tab = 1


func _on_texture_profiles_changed() -> void:
	_invalidate_completed_job(
		"Texture erosion profiles changed. Run the simulation again before applying."
	)
	_update_controls()


func _on_settings_selected(p_resource: Resource, p_inspect: bool) -> void:
	if p_resource is Terrain3DErosionSettings:
		_settings = p_resource
		_store_settings()
		if is_instance_valid(_advanced_inspector):
			_advanced_inspector.edit(_settings)
		if p_inspect:
			_advanced_foldable.folded = false
		_sync_quick_controls_from_settings()


func _on_settings_changed(p_resource: Resource) -> void:
	if p_resource is Terrain3DErosionSettings:
		_settings = p_resource
	else:
		_settings = Terrain3DErosionSettings.new()
		_settings_picker.edited_resource = _settings
	_store_settings()
	if is_instance_valid(_advanced_inspector):
		_advanced_inspector.edit(_settings)
	_sync_quick_controls_from_settings()
	_invalidate_completed_job("Settings changed. Run the simulation again before applying.")


func _new_settings() -> void:
	if _is_running():
		return
	_settings = Terrain3DErosionSettings.new()
	_settings_picker.edited_resource = _settings
	_store_settings()
	if is_instance_valid(_advanced_inspector):
		_advanced_inspector.edit(_settings)
	_sync_quick_controls_from_settings()
	_invalidate_completed_job("Created new settings.")


func _duplicate_settings() -> void:
	if _is_running() or not _settings:
		return
	_settings = _settings.duplicate(true) as Terrain3DErosionSettings
	_settings_picker.edited_resource = _settings
	_store_settings()
	if is_instance_valid(_advanced_inspector):
		_advanced_inspector.edit(_settings)
	_sync_quick_controls_from_settings()
	_invalidate_completed_job("Duplicated settings. The copy has not been saved.")


func _save_settings_as() -> void:
	if not _settings:
		return
	_save_dialog.current_file = "erosion_settings.tres"
	_save_dialog.popup_centered_ratio(0.65)


func _on_save_settings_selected(p_path: String) -> void:
	var path: String = p_path
	if path.get_extension().is_empty():
		path += ".tres"
	var error: Error = ResourceSaver.save(_settings, path)
	if error != OK:
		_stage_label.text = "Settings save failed"
		_summary_label.text = "[color=red]Could not save settings: error %d[/color]" % error
		return
	_settings_picker.edited_resource = _settings
	_stage_label.text = "Settings saved"
	_summary_label.text = "Saved [code]%s[/code]." % path


func _inspect_settings() -> void:
	if not _settings:
		return
	if is_instance_valid(_advanced_inspector):
		_advanced_inspector.edit(_settings)
	_advanced_foldable.folded = not _advanced_foldable.folded


func _on_advanced_property_edited(_property: String) -> void:
	if _syncing_controls:
		return
	_sync_quick_controls_from_settings()
	_invalidate_completed_job(
		"Advanced settings changed. Run the simulation again before applying."
	)


func _on_preset_selected(p_index: int) -> void:
	if _syncing_controls or p_index >= ErosionPresets.PRESET_NAMES.size():
		return
	ErosionPresets.apply(_settings, p_index)
	_preset_description.text = ErosionPresets.get_description(p_index)
	_sync_quick_controls_from_settings(false)
	_invalidate_completed_job("Preset changed. Run the simulation again before applying.")
	_update_controls()


func _on_quality_selected(p_index: int) -> void:
	if _syncing_controls or p_index >= QUALITY_NAMES.size():
		return
	_settings.resolution_scale = QUALITY_SCALES[p_index]
	_settings.simulation_steps = QUALITY_STEPS[p_index]
	_settings.auto_adjust_resolution = true
	_mark_settings_modified()


func _on_years_changed(p_value: float) -> void:
	if not _syncing_controls:
		_settings.simulation_years = p_value
		_mark_settings_modified()


func _on_intensity_changed(p_value: float) -> void:
	if not _syncing_controls:
		_settings.global_intensity = p_value
		_mark_settings_modified()


func _on_seed_changed(p_value: float) -> void:
	if not _syncing_controls:
		_settings.seed = int(p_value)
		_mark_settings_modified()


func _randomize_seed() -> void:
	var randomized_seed: int = int(Time.get_ticks_usec() & 0x7fffffff)
	if randomized_seed <= 0:
		randomized_seed = 1
	_seed_spin.value = randomized_seed


func _on_process_toggled(p_enabled: bool, p_property: String) -> void:
	if _syncing_controls:
		return
	_settings.set(p_property, p_enabled)
	_mark_settings_modified()


func _fit_simulation_mask_to_terrain() -> void:
	_settings.simulation_mask_world_rect = _get_terrain_world_rect()
	_mark_settings_modified()


func _fit_traffic_mask_to_terrain() -> void:
	_settings.traffic_world_rect = _get_terrain_world_rect()
	_mark_settings_modified()


func _fit_desert_input_maps_to_terrain() -> void:
	var bounds: Rect2 = _get_terrain_world_rect()
	_settings.salinity_world_rect = bounds
	_settings.surface_layer_depth_world_rect = bounds
	_settings.bedrock_hardness_world_rect = bounds
	_mark_settings_modified()


func _clear_spatial_limits() -> void:
	_settings.height_range_enabled = false
	_settings.edge_fade_distance_m = 0.0
	_settings.invert_simulation_mask = false
	_mark_settings_modified()


func _mark_settings_modified() -> void:
	if _settings:
		_settings.emit_changed()
	_select_custom_preset()
	_invalidate_completed_job("Settings changed. Run the simulation again before applying.")
	_update_controls()


func _sync_quick_controls_from_settings(p_reset_preset: bool = true) -> void:
	if not _settings or not is_instance_valid(_years_spin):
		return
	_syncing_controls = true
	_years_spin.value = _settings.simulation_years
	_intensity_spin.value = _settings.global_intensity
	_seed_spin.value = _settings.seed
	if is_instance_valid(_live_preview_toggle):
		_live_preview_toggle.button_pressed = bool(_settings.live_preview_enabled)
		_preview_interval_spin.value = float(_settings.live_preview_interval_steps)
		_preview_start_paused_toggle.button_pressed = bool(_settings.live_preview_start_paused)
	for property_name: String in _process_toggles:
		_process_toggles[property_name].button_pressed = bool(_settings.get(property_name))
	var quality_index: int = QUALITY_NAMES.size()
	for index: int in QUALITY_NAMES.size():
		if (
			int(_settings.resolution_scale) == QUALITY_SCALES[index]
			and int(_settings.simulation_steps) == QUALITY_STEPS[index]
		):
			quality_index = index
			break
	_quality_picker.select(quality_index)
	if p_reset_preset:
		_select_custom_preset()
	_syncing_controls = false
	_update_controls()


func _select_custom_preset() -> void:
	if not is_instance_valid(_preset_picker):
		return
	_syncing_controls = true
	_preset_picker.select(_preset_picker.item_count - 1)
	_preset_description.text = "Custom settings. Use Advanced Settings for masks, rates, transitions, and curves."
	_syncing_controls = false


func _start_preview_simulation() -> void:
	_start_simulation(true)


func _start_final_simulation() -> void:
	_start_simulation(false)


func _start_simulation(p_preview: bool) -> void:
	var active_terrain: Terrain3D = _get_terrain()
	if _is_running() or not is_instance_valid(active_terrain) or not _settings:
		return
	var blocking_errors: Array[String] = _collect_blocking_errors(_settings)
	if not blocking_errors.is_empty():
		_stage_label.text = "Settings need attention"
		_summary_label.text = "[color=red]%s[/color]" % "\n".join(blocking_errors)
		_tabs.current_tab = 0
		_update_controls()
		return

	_job_is_preview = p_preview
	_active_run_settings = _settings.duplicate(true) as Terrain3DErosionSettings
	_active_run_settings.live_preview_enabled = _live_preview_toggle.button_pressed
	_active_run_settings.live_preview_interval_steps = int(_preview_interval_spin.value)
	_active_run_settings.live_preview_start_paused = _preview_start_paused_toggle.button_pressed
	if p_preview:
		_active_run_settings.resolution_scale = maxi(8, int(_active_run_settings.resolution_scale))
		_active_run_settings.simulation_steps = mini(64, int(_active_run_settings.simulation_steps))
		_active_run_settings.auto_adjust_resolution = true
		_active_run_settings.memory_budget_mb = mini(512, int(_active_run_settings.memory_budget_mb))

	_tabs.current_tab = 2
	_stage_label.text = "Creating terrain snapshot..."
	_progress_bar.value = 0.0
	_summary_label.text = (
		"Capturing immutable RF source bytes on the editor thread. Simulation buffers and downsampling are built on the worker."
		+ (
			" Draft preview quality is forced to 1:8 or coarser and at most 64 steps."
			if p_preview
			else ""
		)
	)
	_clear_result_preview()
	_job = active_terrain.data.create_erosion_job(_active_run_settings)
	if not _job or _job.get_state() == Terrain3DErosionJob.STATE_FAILED:
		var message: String = "Failed to create erosion job."
		if _job:
			message = _job.get_error_message()
		_stage_label.text = "Snapshot failed"
		_summary_label.text = "[color=red]%s[/color]" % message
		_update_controls()
		return

	_last_preview_revision = -1
	_preview_refresh_accumulator = 1.0
	_ensure_preview_window()
	_preview_window.configure(active_terrain, _job)
	if _active_run_settings.live_preview_start_paused:
		_preview_window.open_preview()

	_thread = Thread.new()
	_thread_joined = false
	var error: Error = _thread.start(_job.run)
	if error != OK:
		_thread_joined = true
		_thread = null
		_stage_label.text = "Worker launch failed"
		_summary_label.text = (
			"[color=red]Could not start erosion worker thread: error %d[/color]" % error
		)
		_update_controls()
		return

	set_process(true)
	_update_controls()


func _process(p_delta: float) -> void:
	if not _job:
		set_process(false)
		return
	_progress_bar.value = _job.get_progress() * 100.0
	_stage_label.text = _job.get_stage_name()
	_update_live_preview_state(p_delta)
	if _job.is_complete():
		_join_thread()
		set_process(false)
		var result_text: String = _format_statistics(_job.get_statistics())
		if _job_is_preview and _job.get_state() == Terrain3DErosionJob.STATE_COMPLETED:
			result_text += "\n\n[color=yellow][b]Draft preview:[/b] This coarse result is inspection-only and cannot be applied.[/color]"
		_summary_label.text = result_text
		if is_instance_valid(_preview_window):
			_preview_window.refresh_from_job(true)
		_populate_result_preview()
		_update_controls()


func _ensure_preview_window() -> void:
	if is_instance_valid(_preview_window):
		return
	_preview_window = ErosionPreviewWindow.new()
	add_child(_preview_window)


func _open_temporary_preview_scene() -> void:
	if not _job:
		return
	_ensure_preview_window()
	_preview_window.configure(_get_terrain(), _job)
	_preview_window.open_preview()


func _toggle_preview_pause() -> void:
	if not _job or _job.get_state() != Terrain3DErosionJob.STATE_RUNNING:
		return
	_job.set_preview_paused(not _job.is_preview_paused())
	_update_live_preview_state(1.0)
	_update_controls()


func _advance_preview_step() -> void:
	if not _job or _job.get_state() != Terrain3DErosionJob.STATE_RUNNING:
		return
	_job.request_preview_step()
	_update_live_preview_state(1.0)
	_update_controls()


func _update_live_preview_state(p_delta: float = 0.0) -> void:
	if not _job:
		if is_instance_valid(_simulation_step_label):
			_simulation_step_label.text = "No simulation snapshot available."
		return
	var completed: bool = _job.is_complete()
	var step: int = int(_job.get_completed_steps()) if completed else int(_job.get_preview_step())
	var total: int = maxi(1, int(_job.get_simulation_steps()))
	var waiting_text: String = " — paused at a step boundary" if _job.is_preview_waiting() else ""
	_simulation_step_label.text = (
		"Published step %d / %d%s. Open the temporary scene to inspect the complete terrain."
		% [step, total, waiting_text]
	)
	_preview_refresh_accumulator += maxf(0.0, p_delta)
	var revision: int = int(_job.get_preview_revision())
	if (
		is_instance_valid(_preview_window)
		and _preview_window.visible
		and revision != _last_preview_revision
		and _preview_refresh_accumulator >= 0.2
	):
		_last_preview_revision = revision
		_preview_refresh_accumulator = 0.0
		_preview_window.refresh_from_job(false)


func _on_live_preview_toggled(p_enabled: bool) -> void:
	if _syncing_controls or not _settings:
		return
	_settings.live_preview_enabled = p_enabled
	_store_settings()
	_update_controls()


func _on_preview_interval_changed(p_value: float) -> void:
	if _syncing_controls or not _settings:
		return
	_settings.live_preview_interval_steps = int(p_value)
	_store_settings()


func _on_preview_start_paused_toggled(p_enabled: bool) -> void:
	if _syncing_controls or not _settings:
		return
	_settings.live_preview_start_paused = p_enabled
	_store_settings()


func _cancel_simulation() -> void:
	if _job and _job.get_state() == Terrain3DErosionJob.STATE_RUNNING:
		_job.cancel()
		_stage_label.text = "Cancellation requested..."


func _request_apply_simulation() -> void:
	if not _can_apply_job():
		return
	var stats: Dictionary[String, Variant] = _job.get_statistics()
	_apply_confirmation.dialog_text = (
		(
			"Apply this simulation to %d terrain regions?\n\n"
			+ "Maximum lowering: %.3f m\nMaximum raising: %.3f m\n"
			+ "Material/control-map output: %s\n\n"
			+ "Terrain3D will record one undo operation for all affected regions."
		)
		% [
			stats.get("region_count", 0),
			stats.get("maximum_lowering_m", 0.0),
			stats.get("maximum_raising_m", 0.0),
			(
				"enabled"
				if _active_run_settings and _active_run_settings.material_output_enabled
				else "disabled"
			),
		]
	)
	_apply_confirmation.popup_centered(Vector2i(int(EditorUI.scaled(520.0)), int(EditorUI.scaled(300.0))))


func _apply_simulation_confirmed() -> void:
	var active_terrain: Terrain3D = _get_terrain()
	if not _can_apply_job() or not is_instance_valid(active_terrain):
		return
	if not _job.is_source_unchanged(active_terrain.data):
		_stage_label.text = "Cannot apply stale result"
		_summary_label.text = (
			"[color=red]Terrain height, control maps, texture profiles, or auto-shader settings changed "
			+ "after the simulation snapshot. Run the simulation again.[/color]"
		)
		_update_controls()
		return

	var regions: Array[Terrain3DRegion] = []
	for location: Vector2i in _job.get_region_locations():
		var region: Terrain3DRegion = active_terrain.data.get_region(location)
		if not region:
			_stage_label.text = "Cannot apply result"
			_summary_label.text = "[color=red]A simulated terrain region no longer exists. Run the simulation again.[/color]"
			return
		regions.push_back(region)

	var terrain_editor: Terrain3DEditor
	if is_instance_valid(plugin):
		terrain_editor = plugin.editor
	if not is_instance_valid(terrain_editor):
		_stage_label.text = "Cannot apply result"
		_summary_label.text = "[color=red]Terrain3D editor transaction service is unavailable.[/color]"
		return
	terrain_editor.start_operation(Vector3.ZERO)
	for region: Terrain3DRegion in regions:
		terrain_editor.backup_region_full(region)
	var error: Error = _job.apply(active_terrain.data)
	terrain_editor.stop_operation()

	if error != OK:
		_stage_label.text = "Apply failed"
		_summary_label.text = "[color=red]%s (error %d)[/color]" % [_job.get_error_message(), error]
	else:
		_stage_label.text = "Applied"
		_summary_label.text = (
			_format_statistics(_job.get_statistics())
			+ "\n\n[b]Applied through Terrain3D's undo system.[/b]"
		)
	_update_controls()


func _can_apply_job() -> bool:
	return (
		_job != null
		and _job.get_state() == Terrain3DErosionJob.STATE_COMPLETED
		and not _job_is_preview
	)


func _populate_result_preview() -> void:
	_region_picker.clear()
	if not _job or _job.get_state() != Terrain3DErosionJob.STATE_COMPLETED:
		_clear_result_preview()
		return
	for location: Vector2i in _job.get_region_locations():
		_region_picker.add_item("Region (%d, %d)" % [location.x, location.y])
		_region_picker.set_item_metadata(_region_picker.item_count - 1, location)
	if _region_picker.item_count > 0:
		_region_picker.select(0)
		_refresh_result_preview()


func _on_preview_selection_changed(_index: int) -> void:
	_refresh_result_preview()


func _on_preview_resolution_toggled(_enabled: bool) -> void:
	_refresh_result_preview()


func _refresh_result_preview() -> void:
	if (
		not _job
		or _job.get_state() != Terrain3DErosionJob.STATE_COMPLETED
		or _region_picker.item_count == 0
	):
		_clear_result_preview()
		return
	var location: Vector2i = _region_picker.get_selected_metadata()
	var output_id: int = _output_picker.get_item_id(_output_picker.selected)
	_last_raw_output_image = _job.get_region_output_map(
		location, output_id, _full_resolution_toggle.button_pressed
	)
	if not _last_raw_output_image or _last_raw_output_image.is_empty():
		_clear_result_preview()
		return
	var preview_source: Image = _make_preview_source(_last_raw_output_image)
	var preview_result: ColorizedOutput = _colorize_output(preview_source, output_id)
	_last_preview_image = preview_result.image
	_preview_texture.texture = ImageTexture.create_from_image(_last_preview_image)
	_preview_range_label.text = (
		"Range: %.6g to %.6g | source %d × %d | display %d × %d"
		% [
			preview_result.minimum,
			preview_result.maximum,
			_last_raw_output_image.get_width(),
			_last_raw_output_image.get_height(),
			_last_preview_image.get_width(),
			_last_preview_image.get_height(),
		]
	)


func _make_preview_source(p_source: Image) -> Image:
	const MAX_PREVIEW_SIDE := 1024
	if p_source.get_width() <= MAX_PREVIEW_SIDE and p_source.get_height() <= MAX_PREVIEW_SIDE:
		return p_source
	var preview_source: Image = p_source.duplicate()
	var scale: float = minf(
		float(MAX_PREVIEW_SIDE) / float(preview_source.get_width()),
		float(MAX_PREVIEW_SIDE) / float(preview_source.get_height())
	)
	preview_source.resize(
		maxi(1, int(roundf(preview_source.get_width() * scale))),
		maxi(1, int(roundf(preview_source.get_height() * scale))),
		Image.INTERPOLATE_BILINEAR
	)
	return preview_source


func _colorize_output(p_source: Image, p_output_id: int) -> ColorizedOutput:
	var width: int = p_source.get_width()
	var height: int = p_source.get_height()
	var minimum: float = INF
	var maximum: float = -INF
	for y: int in height:
		for x: int in width:
			var value: float = p_source.get_pixel(x, y).r
			minimum = minf(minimum, value)
			maximum = maxf(maximum, value)
	if not is_finite(minimum) or not is_finite(maximum):
		minimum = 0.0
		maximum = 0.0
	var preview: Image = Image.create(width, height, false, Image.FORMAT_RGBA8)
	var value_range: float = maxf(maximum - minimum, 0.0000001)
	var absolute_range: float = maxf(maxf(absf(minimum), absf(maximum)), 0.0000001)
	for y: int in height:
		for x: int in width:
			var value: float = p_source.get_pixel(x, y).r
			var color: Color
			if p_output_id == Terrain3DErosionJob.OUTPUT_HEIGHT_DELTA:
				var normalized: float = clampf(value / absolute_range, -1.0, 1.0)
				color = Color(0.12, 0.12, 0.12)
				if normalized < 0.0:
					color = Color(0.08, 0.32, 0.9).lerp(Color(0.12, 0.12, 0.12), 1.0 + normalized)
				elif normalized > 0.0:
					color = Color(0.12, 0.12, 0.12).lerp(Color(0.95, 0.48, 0.08), normalized)
			elif p_output_id == Terrain3DErosionJob.OUTPUT_MATERIAL_ID:
				var material_id: int = int(roundf(value))
				color = Color.from_hsv(fposmod(float(material_id) * 0.113, 1.0), 0.72, 0.92)
			else:
				var normalized: float = clampf((value - minimum) / value_range, 0.0, 1.0)
				color = Color(normalized, normalized, normalized, 1.0)
			preview.set_pixel(x, y, color)
	var result := ColorizedOutput.new()
	result.image = preview
	result.minimum = minimum
	result.maximum = maximum
	return result


func _export_selected_map() -> void:
	if not _last_raw_output_image or _last_raw_output_image.is_empty():
		return
	var output_name: String = OUTPUT_NAMES[_output_picker.selected].to_snake_case()
	_export_dialog.current_file = "%s.exr" % output_name
	_export_dialog.popup_centered_ratio(0.7)


func _on_export_path_selected(p_path: String) -> void:
	if not _last_raw_output_image or _last_raw_output_image.is_empty():
		return
	var extension: String = p_path.get_extension().to_lower()
	var error: Error = ERR_INVALID_PARAMETER
	if extension == "exr":
		error = _last_raw_output_image.save_exr(p_path, false)
	elif extension == "png" and _last_preview_image:
		error = _last_preview_image.save_png(p_path)
	else:
		_stage_label.text = "Export failed"
		_summary_label.text = "[color=red]Use .exr for numeric data or .png for the colorized preview.[/color]"
		return
	if error != OK:
		_stage_label.text = "Export failed"
		_summary_label.text = "[color=red]Could not export map: error %d[/color]" % error
	else:
		_stage_label.text = "Diagnostic map exported"


func _copy_statistics() -> void:
	if is_instance_valid(_summary_label):
		DisplayServer.clipboard_set(_summary_label.get_parsed_text())
		_stage_label.text = "Statistics copied"


func _clear_result_preview() -> void:
	_last_raw_output_image = null
	_last_preview_image = null
	if is_instance_valid(_preview_texture):
		_preview_texture.texture = null
	if is_instance_valid(_preview_range_label):
		_preview_range_label.text = "No diagnostic map available."


func _collect_blocking_errors(p_settings: Terrain3DErosionSettings) -> Array[String]:
	var errors: Array[String] = []
	var active_terrain: Terrain3D = _get_terrain()
	if not is_instance_valid(active_terrain):
		errors.push_back("The inspected Terrain3D node is no longer available.")
	elif not is_instance_valid(active_terrain.data):
		errors.push_back("The Terrain3D node has no data resource.")
	elif active_terrain.data.get_region_count() <= 0:
		errors.push_back("The Terrain3D node has no active regions to erode.")
	if (
		not p_settings.apply_height
		and not p_settings.material_output_enabled
		and not p_settings.oxidation_color_enabled
	):
		errors.push_back("Enable Height Output, Material Transitions, or Oxidation Tint. The current run would apply nothing.")
	if (
		p_settings.traffic_enabled
		and (not p_settings.traffic_mask or p_settings.traffic_mask.is_empty())
	):
		errors.push_back("Foot Traffic is enabled but no traffic mask is assigned.")
	if (
		p_settings.height_range_enabled
		and p_settings.minimum_height_m > p_settings.maximum_height_m
	):
		errors.push_back("Minimum height is greater than maximum height.")
	if p_settings.simulation_mask and p_settings.simulation_mask.is_empty():
		errors.push_back("The assigned simulation mask is empty. Clear it or assign a valid image.")
	if (
		p_settings.salt_weathering_enabled
		and p_settings.salinity_mask
		and p_settings.salinity_mask.is_empty()
	):
		errors.push_back("Salt Weathering is enabled but the assigned salinity mask is empty.")
	if p_settings.stratigraphy_enabled:
		if p_settings.surface_layer_depth_map and p_settings.surface_layer_depth_map.is_empty():
			errors.push_back("The assigned surface-layer depth map is empty.")
		if p_settings.bedrock_hardness_map and p_settings.bedrock_hardness_map.is_empty():
			errors.push_back("The assigned bedrock-hardness map is empty.")
	if p_settings.wet_dry_enabled and p_settings.dry_threshold >= p_settings.wet_threshold:
		errors.push_back("Wet-Dry Cycling requires Dry Threshold to be lower than Wet Threshold.")
	if (
		p_settings.snowmelt_enabled
		and p_settings.snow_melt_temperature <= p_settings.snow_accumulation_temperature
	):
		errors.push_back("Snowmelt Temperature must be greater than Snow Accumulation Temperature.")
	if p_settings.wind_enabled and p_settings.wind_direction.length_squared() <= 0.000001:
		errors.push_back("Wind Erosion requires a non-zero Wind Direction.")
	if p_settings.wave_enabled and p_settings.wave_direction.length_squared() <= 0.000001:
		errors.push_back("Wave Erosion requires a non-zero Wave Direction.")
	if p_settings.material_output_enabled and _count_texture_transitions() == 0:
		errors.push_back("Material Transitions is enabled, but no texture asset defines an erosion transition target.")
	return errors


func _count_texture_transitions() -> int:
	var active_terrain: Terrain3D = _get_terrain()
	if not is_instance_valid(active_terrain) or not active_terrain.assets:
		return 0
	var count: int = 0
	var assets: Terrain3DAssets = active_terrain.assets
	for texture_id: int in assets.get_texture_count():
		var asset: Terrain3DTextureAsset = assets.get_texture_asset(texture_id)
		if not asset or not asset.erosion_enabled:
			continue
		for target_id: int in [
			asset.erosion_exposed_texture_id,
			asset.erosion_weathered_texture_id,
			asset.erosion_oxidized_texture_id,
			asset.erosion_coarse_deposit_texture_id,
			asset.erosion_fine_deposit_texture_id,
			asset.erosion_salt_weathered_texture_id,
			asset.erosion_pavement_texture_id,
			asset.erosion_varnished_texture_id,
		]:
			if target_id >= 0:
				count += 1
	return count


func _collect_advisories() -> Array[String]:
	var warnings: Array[String] = []
	if not _settings:
		return warnings
	if (
		not _settings.hydraulic_enabled
		and not _settings.weathering_enabled
		and not _settings.wind_enabled
		and not _settings.wave_enabled
		and not _settings.traffic_enabled
		and not _settings.salt_weathering_enabled
		and not _settings.wet_dry_enabled
	):
		warnings.push_back("No geomorphic process is enabled. Oxidation alone can affect material/color output but will not meaningfully reshape terrain.")
	if _settings.wave_enabled and is_zero_approx(_settings.coastline_band_m):
		warnings.push_back("Wave erosion is enabled with an effectively zero coastline band.")
	if _settings.alluvial_fan_enabled and not _settings.hydraulic_enabled:
		warnings.push_back(
			"Alluvial Fans require Hydraulic Erosion; the fan stage will have no sediment supply."
		)
	if _settings.debris_flow_enabled and not _settings.hydraulic_enabled:
		warnings.push_back("Debris Flows are most useful with Hydraulic Erosion enabled to create water and suspended load.")
	if _settings.dune_enabled and not _settings.wind_enabled:
		warnings.push_back("Dune Avalanching can relax existing sand, but Wind Erosion is needed for active dune migration.")
	if (
		_settings.salt_weathering_enabled
		and _settings.background_salinity <= 0.0
		and not _settings.salinity_mask
	):
		warnings.push_back("Salt Weathering has zero background salinity and no salinity mask, so it will do nothing.")
	if _settings.stratigraphy_enabled and _settings.surface_layer_depth_m <= 0.0:
		warnings.push_back("Layered Substrate uses a zero surface-layer depth; bedrock hardness will apply immediately.")
	if _settings.material_output_enabled:
		warnings.push_back("Material output only converts a painted texture when that texture asset defines the qualifying transition.")
	if _settings.resolution_scale >= 8:
		warnings.push_back("Coarse resolution is appropriate for broad drainage and aging, but it will not carve narrow channels or trails accurately.")
	if _settings.simulation_years > 100000.0:
		warnings.push_back("Very long runs are authoring extrapolations. Rate values are not calibrated geological constants.")
	return warnings


func _update_controls() -> void:
	if not is_instance_valid(_run_button):
		return
	var running: bool = _is_running()
	var blocking_errors: Array[String] = []
	if _settings:
		blocking_errors = _collect_blocking_errors(_settings)
	else:
		blocking_errors.push_back("No settings resource selected.")
	if is_instance_valid(_terrain_label):
		var active_terrain: Terrain3D = _get_terrain()
		if is_instance_valid(active_terrain) and is_instance_valid(active_terrain.data):
			_terrain_label.text = (
				"%s — %d regions — Inspector workflow"
				% [active_terrain.name, active_terrain.data.get_region_count()]
			)
		elif is_instance_valid(active_terrain):
			_terrain_label.text = "%s — no terrain data" % active_terrain.name
		else:
			_terrain_label.text = "No active Terrain3D node"
	_run_preview_button.disabled = running or _settings == null or not blocking_errors.is_empty()
	_run_button.disabled = running or _settings == null or not blocking_errors.is_empty()
	_cancel_button.disabled = not running
	_apply_button.disabled = not _can_apply_job()
	_settings_picker.editable = not running
	_preset_picker.disabled = running
	_quality_picker.disabled = running
	_years_spin.editable = not running
	_intensity_spin.editable = not running
	_seed_spin.editable = not running
	for toggle: CheckButton in _process_toggles.values():
		toggle.disabled = running
	_live_preview_toggle.disabled = running
	var live_configuration_enabled: bool = not running and _live_preview_toggle.button_pressed
	_preview_interval_spin.editable = live_configuration_enabled
	_preview_start_paused_toggle.disabled = not live_configuration_enabled
	_open_preview_scene_button.disabled = _job == null
	var live_running: bool = running and _job.is_live_preview_enabled()
	_pause_preview_button.disabled = not live_running
	_step_preview_button.disabled = not live_running
	_pause_preview_button.text = "Resume" if live_running and _job.is_preview_paused() else "Pause"
	_export_button.disabled = _last_raw_output_image == null
	_copy_stats_button.disabled = _job == null
	_update_advisory_text(blocking_errors)
	_update_workload_estimate()


func _update_advisory_text(p_blocking_errors: Array[String]) -> void:
	if not is_instance_valid(_warning_label):
		return
	var text: String = ""
	if not p_blocking_errors.is_empty():
		text += "[color=red][b]Cannot run[/b][/color]\n• %s" % "\n• ".join(p_blocking_errors)
	var advisories: Array[String] = _collect_advisories()
	if not advisories.is_empty():
		if not text.is_empty():
			text += "\n\n"
		text += "[color=yellow][b]Check before running[/b][/color]\n• %s" % "\n• ".join(advisories)
	if text.is_empty():
		text = "[color=green][b]Ready.[/b][/color] No blocking configuration problem detected."
	_warning_label.text = text


func _update_workload_estimate() -> void:
	var active_terrain: Terrain3D = _get_terrain()
	if (
		not is_instance_valid(_workload_label)
		or not is_instance_valid(active_terrain)
		or not is_instance_valid(active_terrain.data)
		or not _settings
	):
		return
	var region_count: int = active_terrain.data.get_region_count()
	var region_size: int = int(active_terrain.get_region_size())
	var scale: int = maxi(1, int(_settings.resolution_scale))
	var side: int = maxi(1, int(region_size / scale))
	var cells: int = region_count * side * side
	var bytes: float = float(cells) * ESTIMATED_BYTES_PER_CELL
	if is_instance_valid(_live_preview_toggle) and _live_preview_toggle.button_pressed:
		bytes += float(cells) * 4.0
	var memory_text: String = _format_bytes(bytes)
	var budget_text: String = "%d MiB budget" % int(_settings.memory_budget_mb)
	var risk: String = ""
	if bytes > float(_settings.memory_budget_mb) * 1024.0 * 1024.0:
		risk = " — auto-resolution will likely reduce quality"
	elif cells > 16000000:
		risk = " — heavy run"
	elif cells < 1000000:
		risk = " — light run"
	_workload_label.text = (
		(
			"Estimated working set: %s for %s cells across %d regions, %d steps (%s)%s. "
			+ "The actual solver may use less or auto-reduce resolution. Live preview includes one extra float height grid."
		)
		% [
			memory_text,
			_format_integer(cells),
			region_count,
			int(_settings.simulation_steps),
			budget_text,
			risk
		]
	)


func _get_terrain_world_rect() -> Rect2:
	var active_terrain: Terrain3D = _get_terrain()
	if not is_instance_valid(active_terrain):
		return Rect2()
	var locations: Array[Vector2i] = active_terrain.data.get_region_locations()
	if locations.is_empty():
		return Rect2()
	var region_world_size: float = (
		float(active_terrain.get_region_size()) * active_terrain.get_vertex_spacing()
	)
	var minimum: Vector2 = Vector2(INF, INF)
	var maximum: Vector2 = Vector2(-INF, -INF)
	for location: Vector2i in locations:
		var origin: Vector2 = Vector2(location.x, location.y) * region_world_size
		minimum.x = minf(minimum.x, origin.x)
		minimum.y = minf(minimum.y, origin.y)
		maximum.x = maxf(maximum.x, origin.x + region_world_size)
		maximum.y = maxf(maximum.y, origin.y + region_world_size)
	return Rect2(minimum, maximum - minimum)


func _format_bytes(p_bytes: float) -> String:
	if p_bytes >= 1024.0 * 1024.0 * 1024.0:
		return "%.2f GiB" % (p_bytes / (1024.0 * 1024.0 * 1024.0))
	if p_bytes >= 1024.0 * 1024.0:
		return "%.1f MiB" % (p_bytes / (1024.0 * 1024.0))
	return "%.1f KiB" % (p_bytes / 1024.0)


func _format_integer(p_value: int) -> String:
	var text: String = str(p_value)
	var result: String = ""
	while text.length() > 3:
		result = "," + text.right(3) + result
		text = text.left(text.length() - 3)
	return text + result


func _invalidate_completed_job(p_message: String) -> void:
	if _is_running():
		return
	if _job and _job.get_state() == Terrain3DErosionJob.STATE_COMPLETED:
		_job = null
		_active_run_settings = null
		_job_is_preview = false
		_last_preview_revision = -1
		if is_instance_valid(_preview_window):
			_preview_window.clear_job()
		_clear_result_preview()
		_stage_label.text = "Result invalidated"
		_summary_label.text = p_message


func _is_running() -> bool:
	return _job != null and _job.get_state() == Terrain3DErosionJob.STATE_RUNNING


func _refresh_editor_context() -> void:
	if _profile_editor:
		_profile_editor.refresh()
	_sync_quick_controls_from_settings(false)
	_update_controls()


func _show_workflow_help() -> void:
	_tabs.current_tab = 0
	_warning_label.text = (
		"[b]Safe workflow[/b]\n"
		+ "1. Start with Draft or Preview quality.\n"
		+ "2. Configure texture behavior in Materials.\n"
		+ "3. Inspect height delta, flow, and material-transition maps.\n"
		+ "4. Run Final only after the preview is credible.\n"
		+ "5. Apply through Terrain3D undo, then save the scene/data explicitly."
	)


func _cancel_and_join() -> void:
	if _is_running():
		_job.cancel()
	_join_thread()


func _join_thread() -> void:
	if _thread and _thread.is_started() and not _thread_joined:
		_thread.wait_to_finish()
		_thread_joined = true


func _idle_summary() -> String:
	return (
		"[b]Recommended workflow[/b]\n"
		+ "1. Pick a preset and fit any masks to terrain bounds.\n"
		+ "2. Run a draft preview and inspect diagnostic maps.\n"
		+ "3. Refine advanced rates, texture transitions, and timeline curves.\n"
		+ "4. Run final quality, review the reported height range, and apply through undo."
	)


func _format_statistics(p_stats: Dictionary[String, Variant]) -> String:
	if p_stats.is_empty():
		return ""
	var error_text: String = str(p_stats.get("error", ""))
	var text: String = "[b]Simulation result[/b]\n"
	text += (
		"Regions: %d | Effective scale: 1:%d | Cells: %s\n"
		% [
			p_stats.get("region_count", 0),
			p_stats.get("resolution_scale", 1),
			_format_integer(int(p_stats.get("cell_count", 0))),
		]
	)
	text += (
		"Completed steps: %d | Simulated years: %.3f\n"
		% [
			p_stats.get("completed_steps", 0),
			p_stats.get("simulation_years", 0.0),
		]
	)
	text += "Estimated eroded volume: %.3f m³\n" % p_stats.get("estimated_eroded_volume_m3", 0.0)
	text += (
		"Estimated deposited volume: %.3f m³\n" % p_stats.get("estimated_deposited_volume_m3", 0.0)
	)
	text += (
		"Maximum lowering: %.3f m | Maximum raising: %.3f m"
		% [
			p_stats.get("maximum_lowering_m", 0.0),
			p_stats.get("maximum_raising_m", 0.0),
		]
	)
	if not error_text.is_empty():
		text += "\n[color=red]%s[/color]" % error_text
	return text
