# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Focused editor for Terrain3DTextureAsset erosion modifiers and transition targets.
@tool
class_name Terrain3DErosionTextureProfiles
extends VBoxContainer

const EditorUI = preload("res://addons/terrain_3d/src/editor_ui_util.gd")
const SettingMetadata = preload("res://addons/terrain_3d/src/setting_metadata.gd")

signal profiles_changed

const PROFILE_PRESET_NAMES: PackedStringArray = ["Neutral", "Hard Rock", "Weathered Rock", "Gravel", "Sand", "Silt / Clay", "Topsoil", "Calcrete / Hardpan"]
const PROFILE_PRESET_NEUTRAL: Dictionary[String, float] = {
	"hardness": -1.0,
	"hydraulic": 1.0,
	"wind": 1.0,
	"wave": 1.0,
	"weathering": 1.0,
	"oxidation": 1.0,
	"infiltration": 1.0,
	"traffic": 1.0,
	"cohesion": 0.0,
	"grain": 0.5,
}
const PROFILE_PRESET_HARD_ROCK: Dictionary[String, float] = {
	"hardness": 0.88,
	"hydraulic": 0.28,
	"wind": 0.12,
	"wave": 0.55,
	"weathering": 0.72,
	"oxidation": 0.9,
	"infiltration": 0.22,
	"traffic": 0.18,
	"cohesion": 0.94,
	"grain": 0.9,
}
const PROFILE_PRESET_WEATHERED_ROCK: Dictionary[String, float] = {
	"hardness": 0.56,
	"hydraulic": 0.72,
	"wind": 0.42,
	"wave": 0.9,
	"weathering": 1.35,
	"oxidation": 1.2,
	"infiltration": 0.55,
	"traffic": 0.55,
	"cohesion": 0.68,
	"grain": 0.78,
}
const PROFILE_PRESET_GRAVEL: Dictionary[String, float] = {
	"hardness": 0.22,
	"hydraulic": 1.05,
	"wind": 0.62,
	"wave": 1.0,
	"weathering": 0.55,
	"oxidation": 0.45,
	"infiltration": 1.65,
	"traffic": 1.05,
	"cohesion": 0.22,
	"grain": 0.72,
}
const PROFILE_PRESET_SAND: Dictionary[String, float] = {
	"hardness": 0.04,
	"hydraulic": 0.78,
	"wind": 1.85,
	"wave": 1.25,
	"weathering": 0.15,
	"oxidation": 0.08,
	"infiltration": 2.0,
	"traffic": 1.35,
	"cohesion": 0.06,
	"grain": 0.55,
}
const PROFILE_PRESET_SILT_CLAY: Dictionary[String, float] = {
	"hardness": 0.08,
	"hydraulic": 1.5,
	"wind": 1.0,
	"wave": 1.2,
	"weathering": 0.85,
	"oxidation": 0.45,
	"infiltration": 0.35,
	"traffic": 1.8,
	"cohesion": 0.64,
	"grain": 0.12,
}
const PROFILE_PRESET_TOPSOIL: Dictionary[String, float] = {
	"hardness": 0.1,
	"hydraulic": 1.35,
	"wind": 0.82,
	"wave": 1.0,
	"weathering": 1.4,
	"oxidation": 0.55,
	"infiltration": 1.25,
	"traffic": 1.7,
	"cohesion": 0.42,
	"grain": 0.28,
}
const PROFILE_PRESET_CALCRETE_HARDPAN: Dictionary[String, float] = {
	"hardness": 0.74,
	"hydraulic": 0.42,
	"wind": 0.2,
	"wave": 0.65,
	"weathering": 0.48,
	"oxidation": 0.35,
	"infiltration": 0.16,
	"traffic": 0.38,
	"cohesion": 0.86,
	"grain": 0.62,
}

class CopiedProfile extends RefCounted:
	var enabled: bool = false
	var hardness: float = 0.0
	var hydraulic: float = 0.0
	var wind: float = 0.0
	var wave: float = 0.0
	var weathering: float = 0.0
	var oxidation: float = 0.0
	var salt: float = 0.0
	var wet_dry: float = 0.0
	var infiltration: float = 0.0
	var traffic: float = 0.0
	var cohesion: float = 0.0
	var grain: float = 0.0
	var exposed: int = -1
	var weathered: int = -1
	var oxidized: int = -1
	var coarse: int = -1
	var fine: int = -1
	var salt_transition: int = -1
	var pavement: int = -1
	var varnished: int = -1


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
		title.add_theme_font_size_override("font_size", 15)
		body.add_child(title)


var plugin: Terrain3DEditorPluginBase
var terrain: Terrain3D
var inspector_embedded: bool = false
var _texture_picker: OptionButton
var _profile_preset_picker: OptionButton
var _enabled_toggle: CheckButton
var _physical_controls: Dictionary[String, SpinBox] = {}
var _transition_controls: Dictionary[String, OptionButton] = {}
var _status_label: Label
var _copy_buffer: CopiedProfile
var _syncing: bool = false
var _selected_texture_id: int = -1
var _asset_foldable: FoldableContainer
var _asset_inspector: EditorInspector


func _get_terrain() -> Terrain3D:
	if is_instance_valid(terrain):
		return terrain
	if is_instance_valid(plugin):
		var plugin_terrain: Terrain3D = plugin.terrain
		if is_instance_valid(plugin_terrain):
			return plugin_terrain
	return null


func _enter_tree() -> void:
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	_build_panel()
	refresh()


func popup() -> void:
	refresh()


func refresh() -> void:
	if not is_instance_valid(_texture_picker):
		return
	var previous_id: int = _selected_texture_id
	_refresh_texture_list()
	if _texture_picker.item_count == 0:
		_selected_texture_id = -1
		_status_label.text = "No Terrain3D texture assets are available."
		return
	var selected_index: int = 0
	for index: int in _texture_picker.item_count:
		if _texture_picker.get_item_id(index) == previous_id:
			selected_index = index
			break
	_texture_picker.select(selected_index)
	_load_selected_texture(selected_index)


func _build_panel() -> void:
	var root: VBoxContainer = self
	root.add_theme_constant_override("separation", 8)

	var intro: Label = Label.new()
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	intro.text = (
		"These modifiers are read from the texture IDs already painted in the Terrain3D control map. "
		+ "A transition target is optional: without one, erosion can reshape terrain while leaving that material unchanged."
	)
	root.add_child(intro)

	var texture_row: HBoxContainer = HBoxContainer.new()
	texture_row.add_theme_constant_override("separation", 8)
	root.add_child(texture_row)
	var texture_label: Label = Label.new()
	texture_label.text = "Texture"
	EditorUI.configure_form_label(texture_label, 100.0)
	texture_row.add_child(texture_label)
	_texture_picker = OptionButton.new()
	_texture_picker.tooltip_text = "Terrain texture asset whose erosion response and material-transition targets are being edited."
	EditorUI.configure_field_control(_texture_picker, 260.0)
	_texture_picker.item_selected.connect(_load_selected_texture)
	texture_row.add_child(_texture_picker)

	var texture_actions: HFlowContainer = HFlowContainer.new()
	texture_actions.add_theme_constant_override("h_separation", 8)
	texture_actions.add_theme_constant_override("v_separation", 4)
	root.add_child(texture_actions)

	var inspect_button: Button = Button.new()
	inspect_button.text = "Full Asset"
	inspect_button.tooltip_text = "Toggle the complete texture asset editor below."
	inspect_button.pressed.connect(_inspect_selected_asset)
	texture_actions.add_child(inspect_button)

	var refresh_button: Button = Button.new()
	refresh_button.text = "Refresh Assets"
	refresh_button.tooltip_text = "Reload the Terrain3D texture list while preserving the current selection when possible."
	refresh_button.pressed.connect(refresh)
	texture_actions.add_child(refresh_button)

	var body: VBoxContainer = VBoxContainer.new()
	body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	body.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	body.add_theme_constant_override("separation", 10)
	if inspector_embedded:
		root.add_child(body)
	else:
		var scroll: ScrollContainer = ScrollContainer.new()
		EditorUI.configure_scroll_container(scroll)
		root.add_child(scroll)
		scroll.add_child(body)

	var profile_row: HFlowContainer = HFlowContainer.new()
	profile_row.add_theme_constant_override("h_separation", 8)
	profile_row.add_theme_constant_override("v_separation", 4)
	body.add_child(profile_row)
	var profile_label: Label = Label.new()
	profile_label.text = "Physical preset"
	EditorUI.configure_form_label(profile_label, 150.0)
	profile_row.add_child(profile_label)
	_profile_preset_picker = OptionButton.new()
	_profile_preset_picker.tooltip_text = "Physical erosion-response starting point for the selected texture. Applying a preset changes the physical modifiers but preserves transition targets."
	for preset_name: String in PROFILE_PRESET_NAMES:
		_profile_preset_picker.add_item(preset_name)
	EditorUI.configure_field_control(_profile_preset_picker, 220.0)
	profile_row.add_child(_profile_preset_picker)
	var apply_preset_button: Button = Button.new()
	apply_preset_button.text = "Apply Preset"
	apply_preset_button.tooltip_text = "Changes physical modifiers only. Transition targets are preserved."
	apply_preset_button.pressed.connect(_apply_profile_preset)
	profile_row.add_child(apply_preset_button)

	_enabled_toggle = CheckButton.new()
	_enabled_toggle.text = "Use this texture's erosion profile"
	_enabled_toggle.tooltip_text = "Disabled textures remain present but use neutral physical modifiers and do not provide transition sources."
	_enabled_toggle.toggled.connect(_on_enabled_toggled)
	body.add_child(_enabled_toggle)

	var physical_panel: PanelParts = _make_panel("Physical Response")
	body.add_child(physical_panel.panel)
	var physical_grid: GridContainer = GridContainer.new()
	EditorUI.configure_form_grid(physical_grid)
	physical_panel.body.add_child(physical_grid)
	_add_physical_control(
		physical_grid,
		"hardness",
		"Hardness override",
		-1.0,
		1.0,
		0.01,
		"-1 inherits the scenario's base hardness; 1 is maximally resistant."
	)
	_add_physical_control(
		physical_grid,
		"hydraulic",
		"Hydraulic erodibility",
		0.0,
		8.0,
		0.01,
		"Multiplier for flowing-water erosion."
	)
	_add_physical_control(
		physical_grid,
		"wind",
		"Wind erodibility",
		0.0,
		8.0,
		0.01,
		"Multiplier for abrasion and wind transport."
	)
	_add_physical_control(
		physical_grid,
		"wave",
		"Wave erodibility",
		0.0,
		8.0,
		0.01,
		"Multiplier for coastline erosion."
	)
	_add_physical_control(
		physical_grid,
		"weathering",
		"Weathering susceptibility",
		0.0,
		8.0,
		0.01,
		"Multiplier for physical, chemical, and freeze-thaw breakdown."
	)
	_add_physical_control(
		physical_grid,
		"oxidation",
		"Oxidation susceptibility",
		0.0,
		8.0,
		0.01,
		"Multiplier for oxidation accumulation and weakening."
	)
	_add_physical_control(
		physical_grid,
		"salt",
		"Salt-weathering susceptibility",
		0.0,
		8.0,
		0.01,
		"Multiplier for crystallization damage during saline wet-dry cycles."
	)
	_add_physical_control(
		physical_grid,
		"wet_dry",
		"Wet-dry susceptibility",
		0.0,
		8.0,
		0.01,
		"Multiplier for slaking and shrink-swell breakdown during moisture-state changes."
	)
	_add_physical_control(
		physical_grid,
		"infiltration",
		"Infiltration multiplier",
		0.0,
		8.0,
		0.01,
		"Changes how much rainfall enters the material instead of becoming runoff."
	)
	_add_physical_control(
		physical_grid,
		"traffic",
		"Traffic erodibility",
		0.0,
		8.0,
		0.01,
		"Multiplier for masked foot-traffic incision."
	)
	_add_physical_control(
		physical_grid,
		"cohesion",
		"Cohesion",
		0.0,
		1.0,
		0.01,
		"Resistance to loose-material entrainment and transport."
	)
	_add_physical_control(
		physical_grid,
		"grain",
		"Grain size",
		0.0,
		1.0,
		0.01,
		"0 is fine material; 1 is coarse. Used to choose fine versus coarse deposition transitions."
	)

	var transition_panel: PanelParts = _make_panel("Control-Map Transitions")
	body.add_child(transition_panel.panel)
	var transition_help: Label = Label.new()
	transition_help.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	transition_help.text = (
		"Targets are texture asset IDs. The simulator changes a control-map texel only when the corresponding process "
		+ "passes its threshold and the texel's current dominant texture defines that transition."
	)
	transition_panel.body.add_child(transition_help)
	var transition_grid: GridContainer = GridContainer.new()
	EditorUI.configure_form_grid(transition_grid)
	transition_panel.body.add_child(transition_grid)
	_add_transition_control(transition_grid, "exposed", "Exposed / stripped surface")
	_add_transition_control(transition_grid, "weathered", "Weathered surface")
	_add_transition_control(transition_grid, "oxidized", "Oxidized surface")
	_add_transition_control(transition_grid, "coarse", "Coarse deposition")
	_add_transition_control(transition_grid, "fine", "Fine deposition")
	_add_transition_control(transition_grid, "salt", "Salt-weathered surface")
	_add_transition_control(transition_grid, "pavement", "Desert pavement")
	_add_transition_control(transition_grid, "varnished", "Mature desert varnish")

	var actions: HFlowContainer = HFlowContainer.new()
	actions.add_theme_constant_override("h_separation", 8)
	actions.add_theme_constant_override("v_separation", 6)
	body.add_child(actions)
	var copy_button: Button = Button.new()
	copy_button.text = "Copy Profile"
	copy_button.pressed.connect(_copy_profile)
	actions.add_child(copy_button)
	var paste_button: Button = Button.new()
	paste_button.text = "Paste Profile"
	paste_button.tooltip_text = "Copies physical modifiers and transition targets to the selected texture."
	paste_button.pressed.connect(_paste_profile)
	actions.add_child(paste_button)
	var clear_transitions_button: Button = Button.new()
	clear_transitions_button.text = "Clear Transitions"
	clear_transitions_button.tooltip_text = "Remove all erosion-driven material transition targets from the selected texture while preserving its physical response values."
	clear_transitions_button.pressed.connect(_clear_transitions)
	actions.add_child(clear_transitions_button)
	var reset_button: Button = Button.new()
	reset_button.text = "Reset to Neutral"
	reset_button.tooltip_text = "Restore neutral erosion-response values and clear the selected texture's transition overrides."
	reset_button.pressed.connect(_reset_selected_profile)
	actions.add_child(reset_button)

	_status_label = Label.new()
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.add_child(_status_label)

	_asset_foldable = FoldableContainer.new()
	_asset_foldable.title = "Complete Texture Asset"
	_asset_foldable.folded = true
	_asset_foldable.tooltip_text = "Edit the selected Terrain3DTextureAsset without leaving the Terrain3D Inspector."
	body.add_child(_asset_foldable)

	_asset_inspector = EditorInspector.create_default_inspector()
	if is_instance_valid(_asset_inspector):
		_asset_inspector.custom_minimum_size.y = 360.0
		_asset_inspector.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_asset_inspector.property_edited.connect(_on_asset_property_edited)
		_asset_foldable.add_child(_asset_inspector)
	else:
		var inspector_error: Label = Label.new()
		inspector_error.text = "The embedded texture-asset inspector could not be created."
		inspector_error.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		_asset_foldable.add_child(inspector_error)

	_apply_accessible_control_layout(root)


func _apply_accessible_control_layout(p_root: Node) -> void:
	for child: Node in p_root.find_children("*", "", true, false):
		if child is BaseButton or child is LineEdit or child is SpinBox or child is OptionButton:
			EditorUI.configure_compact_control(child as Control)


func _make_panel(p_title: String) -> PanelParts:
	return PanelParts.new(p_title)


func _add_physical_control(
	p_parent: GridContainer,
	p_key: String,
	p_label: String,
	p_min: float,
	p_max: float,
	p_step: float,
	p_tooltip: String
) -> void:
	var label: Label = Label.new()
	label.text = p_label
	label.tooltip_text = p_tooltip
	EditorUI.configure_form_label(label)
	p_parent.add_child(label)
	var spin: SpinBox = SpinBox.new()
	spin.min_value = p_min
	spin.max_value = p_max
	spin.step = p_step
	spin.allow_greater = false
	spin.tooltip_text = p_tooltip
	EditorUI.configure_field_control(spin, 180.0)
	spin.value_changed.connect(_on_physical_value_changed.bind(p_key))
	_physical_controls[p_key] = spin
	p_parent.add_child(spin)


func _add_transition_control(p_parent: GridContainer, p_key: String, p_label: String) -> void:
	var label: Label = Label.new()
	label.text = p_label
	var tooltip: String = SettingMetadata.tooltip("Terrain3DTextureAsset", StringName(p_key))
	label.tooltip_text = tooltip
	EditorUI.configure_form_label(label)
	p_parent.add_child(label)
	var picker: OptionButton = OptionButton.new()
	picker.tooltip_text = tooltip
	EditorUI.configure_field_control(picker, 220.0)
	picker.item_selected.connect(_on_transition_selected.bind(p_key))
	_transition_controls[p_key] = picker
	p_parent.add_child(picker)


func _refresh_texture_list() -> void:
	_texture_picker.clear()
	var active_terrain: Terrain3D = _get_terrain()
	if not is_instance_valid(active_terrain) or not active_terrain.assets:
		return
	var assets: Terrain3DAssets = active_terrain.assets
	for texture_id: int in assets.get_texture_count():
		var asset: Terrain3DTextureAsset = assets.get_texture_asset(texture_id)
		if not asset:
			continue
		var display_name: String = asset.name if not asset.name.is_empty() else "Texture %d" % texture_id
		_texture_picker.add_item("%02d — %s" % [texture_id, display_name], texture_id)
	for picker: OptionButton in _transition_controls.values():
		_refresh_transition_picker(picker)


func _refresh_transition_picker(p_picker: OptionButton) -> void:
	p_picker.clear()
	p_picker.add_item("No transition")
	p_picker.set_item_metadata(0, -1)
	var active_terrain: Terrain3D = _get_terrain()
	if not is_instance_valid(active_terrain) or not active_terrain.assets:
		return
	var assets: Terrain3DAssets = active_terrain.assets
	for texture_id: int in assets.get_texture_count():
		var asset: Terrain3DTextureAsset = assets.get_texture_asset(texture_id)
		if not asset:
			continue
		var display_name: String = asset.name if not asset.name.is_empty() else "Texture %d" % texture_id
		p_picker.add_item("%02d — %s" % [texture_id, display_name])
		p_picker.set_item_metadata(p_picker.item_count - 1, texture_id)


func _load_selected_texture(p_index: int) -> void:
	if _syncing or p_index < 0 or p_index >= _texture_picker.item_count:
		return
	_selected_texture_id = _texture_picker.get_item_id(p_index)
	var asset: Terrain3DTextureAsset = _get_selected_asset()
	if not asset:
		return
	_syncing = true
	_enabled_toggle.button_pressed = asset.erosion_enabled
	_physical_controls["hardness"].value = asset.erosion_hardness
	_physical_controls["hydraulic"].value = asset.erosion_hydraulic_erodibility
	_physical_controls["wind"].value = asset.erosion_wind_erodibility
	_physical_controls["wave"].value = asset.erosion_wave_erodibility
	_physical_controls["weathering"].value = asset.erosion_weathering_susceptibility
	_physical_controls["oxidation"].value = asset.erosion_oxidation_susceptibility
	_physical_controls["salt"].value = asset.erosion_salt_susceptibility
	_physical_controls["wet_dry"].value = asset.erosion_wet_dry_susceptibility
	_physical_controls["infiltration"].value = asset.erosion_infiltration_multiplier
	_physical_controls["traffic"].value = asset.erosion_traffic_erodibility
	_physical_controls["cohesion"].value = asset.erosion_cohesion
	_physical_controls["grain"].value = asset.erosion_grain_size
	_select_transition_value(_transition_controls["exposed"], asset.erosion_exposed_texture_id)
	_select_transition_value(_transition_controls["weathered"], asset.erosion_weathered_texture_id)
	_select_transition_value(_transition_controls["oxidized"], asset.erosion_oxidized_texture_id)
	_select_transition_value(_transition_controls["coarse"], asset.erosion_coarse_deposit_texture_id)
	_select_transition_value(_transition_controls["fine"], asset.erosion_fine_deposit_texture_id)
	_select_transition_value(_transition_controls["salt"], asset.erosion_salt_weathered_texture_id)
	_select_transition_value(_transition_controls["pavement"], asset.erosion_pavement_texture_id)
	_select_transition_value(_transition_controls["varnished"], asset.erosion_varnished_texture_id)
	_update_transition_target_availability()
	_syncing = false
	_update_status(asset)
	if is_instance_valid(_asset_inspector):
		_asset_inspector.edit(asset)


func _select_transition_value(p_picker: OptionButton, p_texture_id: int) -> void:
	for index: int in p_picker.item_count:
		if int(p_picker.get_item_metadata(index)) == p_texture_id:
			p_picker.select(index)
			return
	p_picker.select(0)


func _update_transition_target_availability() -> void:
	for picker: OptionButton in _transition_controls.values():
		for index: int in picker.item_count:
			var target_id: int = int(picker.get_item_metadata(index))
			picker.set_item_disabled(index, target_id == _selected_texture_id)


func _get_selected_asset() -> Terrain3DTextureAsset:
	var active_terrain: Terrain3D = _get_terrain()
	if (
		_selected_texture_id < 0
		or not is_instance_valid(active_terrain)
		or not active_terrain.assets
	):
		return null
	return active_terrain.assets.get_texture_asset(_selected_texture_id)


func _on_enabled_toggled(p_enabled: bool) -> void:
	if _syncing:
		return
	var asset: Terrain3DTextureAsset = _get_selected_asset()
	if asset:
		asset.erosion_enabled = p_enabled
		_commit_asset_change(asset)


func _on_physical_value_changed(p_value: float, p_key: String) -> void:
	if _syncing:
		return
	var asset: Terrain3DTextureAsset = _get_selected_asset()
	if not asset:
		return
	match p_key:
		"hardness":
			asset.erosion_hardness = p_value
		"hydraulic":
			asset.erosion_hydraulic_erodibility = p_value
		"wind":
			asset.erosion_wind_erodibility = p_value
		"wave":
			asset.erosion_wave_erodibility = p_value
		"weathering":
			asset.erosion_weathering_susceptibility = p_value
		"oxidation":
			asset.erosion_oxidation_susceptibility = p_value
		"salt":
			asset.erosion_salt_susceptibility = p_value
		"wet_dry":
			asset.erosion_wet_dry_susceptibility = p_value
		"infiltration":
			asset.erosion_infiltration_multiplier = p_value
		"traffic":
			asset.erosion_traffic_erodibility = p_value
		"cohesion":
			asset.erosion_cohesion = p_value
		"grain":
			asset.erosion_grain_size = p_value
	_commit_asset_change(asset)


func _on_transition_selected(p_index: int, p_key: String) -> void:
	if _syncing:
		return
	var asset: Terrain3DTextureAsset = _get_selected_asset()
	if not asset:
		return
	var picker := _transition_controls[p_key] as OptionButton
	var target_id: int = int(picker.get_item_metadata(p_index))
	if target_id == _selected_texture_id:
		_status_label.text = "A texture cannot transition to itself."
		_load_selected_texture(_texture_picker.selected)
		return
	match p_key:
		"exposed":
			asset.erosion_exposed_texture_id = target_id
		"weathered":
			asset.erosion_weathered_texture_id = target_id
		"oxidized":
			asset.erosion_oxidized_texture_id = target_id
		"coarse":
			asset.erosion_coarse_deposit_texture_id = target_id
		"fine":
			asset.erosion_fine_deposit_texture_id = target_id
		"salt":
			asset.erosion_salt_weathered_texture_id = target_id
		"pavement":
			asset.erosion_pavement_texture_id = target_id
		"varnished":
			asset.erosion_varnished_texture_id = target_id
	_commit_asset_change(asset)


func _profile_preset_values(p_index: int) -> Dictionary[String, float]:
	match p_index:
		0:
			return PROFILE_PRESET_NEUTRAL
		1:
			return PROFILE_PRESET_HARD_ROCK
		2:
			return PROFILE_PRESET_WEATHERED_ROCK
		3:
			return PROFILE_PRESET_GRAVEL
		4:
			return PROFILE_PRESET_SAND
		5:
			return PROFILE_PRESET_SILT_CLAY
		6:
			return PROFILE_PRESET_TOPSOIL
		7:
			return PROFILE_PRESET_CALCRETE_HARDPAN
	return PROFILE_PRESET_NEUTRAL


func _apply_profile_preset() -> void:
	var asset: Terrain3DTextureAsset = _get_selected_asset()
	if not asset:
		return
	var values: Dictionary[String, float] = _profile_preset_values(_profile_preset_picker.selected)
	var weathering: float = float(values.get("weathering", 1.0))
	asset.erosion_hardness = float(values.get("hardness", -1.0))
	asset.erosion_hydraulic_erodibility = float(values.get("hydraulic", 1.0))
	asset.erosion_wind_erodibility = float(values.get("wind", 1.0))
	asset.erosion_wave_erodibility = float(values.get("wave", 1.0))
	asset.erosion_weathering_susceptibility = weathering
	asset.erosion_oxidation_susceptibility = float(values.get("oxidation", 1.0))
	asset.erosion_salt_susceptibility = float(values.get("salt", weathering))
	asset.erosion_wet_dry_susceptibility = float(values.get("wet_dry", weathering))
	asset.erosion_infiltration_multiplier = float(values.get("infiltration", 1.0))
	asset.erosion_traffic_erodibility = float(values.get("traffic", 1.0))
	asset.erosion_cohesion = float(values.get("cohesion", 0.0))
	asset.erosion_grain_size = float(values.get("grain", 0.5))
	_commit_asset_change(asset)
	_load_selected_texture(_texture_picker.selected)


func _copy_profile() -> void:
	var asset: Terrain3DTextureAsset = _get_selected_asset()
	if not asset:
		return
	_copy_buffer = CopiedProfile.new()
	_copy_buffer.enabled = asset.erosion_enabled
	_copy_buffer.hardness = asset.erosion_hardness
	_copy_buffer.hydraulic = asset.erosion_hydraulic_erodibility
	_copy_buffer.wind = asset.erosion_wind_erodibility
	_copy_buffer.wave = asset.erosion_wave_erodibility
	_copy_buffer.weathering = asset.erosion_weathering_susceptibility
	_copy_buffer.oxidation = asset.erosion_oxidation_susceptibility
	_copy_buffer.salt = asset.erosion_salt_susceptibility
	_copy_buffer.wet_dry = asset.erosion_wet_dry_susceptibility
	_copy_buffer.infiltration = asset.erosion_infiltration_multiplier
	_copy_buffer.traffic = asset.erosion_traffic_erodibility
	_copy_buffer.cohesion = asset.erosion_cohesion
	_copy_buffer.grain = asset.erosion_grain_size
	_copy_buffer.exposed = asset.erosion_exposed_texture_id
	_copy_buffer.weathered = asset.erosion_weathered_texture_id
	_copy_buffer.oxidized = asset.erosion_oxidized_texture_id
	_copy_buffer.coarse = asset.erosion_coarse_deposit_texture_id
	_copy_buffer.fine = asset.erosion_fine_deposit_texture_id
	_copy_buffer.salt_transition = asset.erosion_salt_weathered_texture_id
	_copy_buffer.pavement = asset.erosion_pavement_texture_id
	_copy_buffer.varnished = asset.erosion_varnished_texture_id
	_status_label.text = "Copied erosion profile from texture %d." % _selected_texture_id


func _paste_profile() -> void:
	var asset: Terrain3DTextureAsset = _get_selected_asset()
	if not asset or _copy_buffer == null:
		_status_label.text = "Copy a profile first."
		return
	asset.erosion_enabled = _copy_buffer.enabled
	asset.erosion_hardness = _copy_buffer.hardness
	asset.erosion_hydraulic_erodibility = _copy_buffer.hydraulic
	asset.erosion_wind_erodibility = _copy_buffer.wind
	asset.erosion_wave_erodibility = _copy_buffer.wave
	asset.erosion_weathering_susceptibility = _copy_buffer.weathering
	asset.erosion_oxidation_susceptibility = _copy_buffer.oxidation
	asset.erosion_salt_susceptibility = _copy_buffer.salt
	asset.erosion_wet_dry_susceptibility = _copy_buffer.wet_dry
	asset.erosion_infiltration_multiplier = _copy_buffer.infiltration
	asset.erosion_traffic_erodibility = _copy_buffer.traffic
	asset.erosion_cohesion = _copy_buffer.cohesion
	asset.erosion_grain_size = _copy_buffer.grain
	asset.erosion_exposed_texture_id = _copy_buffer.exposed
	asset.erosion_weathered_texture_id = _copy_buffer.weathered
	asset.erosion_oxidized_texture_id = _copy_buffer.oxidized
	asset.erosion_coarse_deposit_texture_id = _copy_buffer.coarse
	asset.erosion_fine_deposit_texture_id = _copy_buffer.fine
	asset.erosion_salt_weathered_texture_id = _copy_buffer.salt_transition
	asset.erosion_pavement_texture_id = _copy_buffer.pavement
	asset.erosion_varnished_texture_id = _copy_buffer.varnished
	_commit_asset_change(asset)
	_load_selected_texture(_texture_picker.selected)


func _clear_transitions() -> void:
	var asset: Terrain3DTextureAsset = _get_selected_asset()
	if not asset:
		return
	asset.erosion_exposed_texture_id = -1
	asset.erosion_weathered_texture_id = -1
	asset.erosion_oxidized_texture_id = -1
	asset.erosion_coarse_deposit_texture_id = -1
	asset.erosion_fine_deposit_texture_id = -1
	asset.erosion_salt_weathered_texture_id = -1
	asset.erosion_pavement_texture_id = -1
	asset.erosion_varnished_texture_id = -1
	_commit_asset_change(asset)
	_load_selected_texture(_texture_picker.selected)


func _reset_selected_profile() -> void:
	_profile_preset_picker.select(0)
	_apply_profile_preset()
	_clear_transitions()


func _inspect_selected_asset() -> void:
	var asset: Terrain3DTextureAsset = _get_selected_asset()
	if not asset:
		return
	if is_instance_valid(_asset_inspector):
		_asset_inspector.edit(asset)
	_asset_foldable.folded = not _asset_foldable.folded


func _on_asset_property_edited(_property: String) -> void:
	var asset: Terrain3DTextureAsset = _get_selected_asset()
	if not asset:
		return
	_load_selected_texture(_texture_picker.selected)
	_commit_asset_change(asset)


func _commit_asset_change(p_asset: Terrain3DTextureAsset) -> void:
	p_asset.emit_changed()
	var active_terrain: Terrain3D = _get_terrain()
	if is_instance_valid(active_terrain) and active_terrain.assets:
		active_terrain.assets.emit_changed()
	profiles_changed.emit()
	_update_status(p_asset)


func _update_status(p_asset: Terrain3DTextureAsset) -> void:
	var transitions: int = 0
	for target_id: int in [
		p_asset.erosion_exposed_texture_id,
		p_asset.erosion_weathered_texture_id,
		p_asset.erosion_oxidized_texture_id,
		p_asset.erosion_coarse_deposit_texture_id,
		p_asset.erosion_fine_deposit_texture_id,
		p_asset.erosion_salt_weathered_texture_id,
		p_asset.erosion_pavement_texture_id,
		p_asset.erosion_varnished_texture_id,
	]:
		if target_id >= 0:
			transitions += 1
	var has_self_transition: bool = false
	for target_id: int in [
		p_asset.erosion_exposed_texture_id,
		p_asset.erosion_weathered_texture_id,
		p_asset.erosion_oxidized_texture_id,
		p_asset.erosion_coarse_deposit_texture_id,
		p_asset.erosion_fine_deposit_texture_id,
		p_asset.erosion_salt_weathered_texture_id,
		p_asset.erosion_pavement_texture_id,
		p_asset.erosion_varnished_texture_id,
	]:
		if target_id == _selected_texture_id:
			has_self_transition = true
			break
	if has_self_transition:
		_status_label.text = (
			"Texture %d contains a self-transition. Choose another target or clear the transition."
			% _selected_texture_id
		)
		return
	_status_label.text = (
		"Texture %d uses %d configured transition%s. Physical modifiers affect erosion even when no transition is assigned."
		% [_selected_texture_id, transitions, "" if transitions == 1 else "s"]
	)
