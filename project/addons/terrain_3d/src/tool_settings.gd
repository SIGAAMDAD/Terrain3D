# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Tool settings bar for Terrain3D
@tool
extends PanelContainer

signal picking(type: int, callback: Callable)
signal setting_changed

enum Layout {
	HORIZONTAL,
	VERTICAL,
	GRID,
}

enum SettingType {
	CHECKBOX,
	COLOR_SELECT,
	DOUBLE_SLIDER,
	OPTION,
	PICKER,
	MULTI_PICKER,
	SLIDER,
	LABEL,
	TYPE_MAX,
}

const MultiPicker = preload("res://addons/terrain_3d/src/multi_picker.gd")
const EditorUI = preload("res://addons/terrain_3d/src/editor_ui_util.gd")
const DEFAULT_BRUSH: String = "circle0.exr"
const BRUSH_PATH: String = "res://addons/terrain_3d/brushes"
const ES_TOOL_SETTINGS: String = "terrain3d/tool_settings/"


# Every painter setting has user-facing documentation. Explicit per-call tooltips
# override this table for settings whose meaning depends on a specific mode.
const SETTING_TOOLTIPS: Dictionary = {
	"instructions": "Region editing instructions. Add creates a 128 m terrain region at the cursor; the inverse operation removes that region.",
	"size": "Brush diameter in world meters. Values below one terrain texel still target the nearest terrain sample.",
	"strength": "Brush influence per stamp. Pen pressure multiplies this value when a pressure-sensitive device is active.",
	"height": "Target world-space elevation used by Set Height. Painting blends existing heights toward this value.",
	"color": "Terrain color tint written to the color map. This does not change the selected terrain material.",
	"roughness": "Wetness/roughness channel target. Negative values are wetter, positive values are drier, and zero is neutral.",
	"enable_texture": "When enabled, Texture Paint/Spray changes material IDs. Disable it to edit only angle or scale metadata on matching texels.",
	"texture_filter": "Restrict Color or Wetness painting to terrain texels whose visible material matches the selected texture asset.",
	"margin": "Texture-filter blend margin. Positive values include more transition texels; negative values require the selected texture to be more dominant.",
	"slope": "Allowed terrain slope range in degrees. Samples outside this interval are left unchanged.",
	"enable_angle": "Write terrain material UV rotation while painting the selected texture.",
	"angle": "Material UV rotation in degrees, quantized to Terrain3D's 16 stored rotation steps.",
	"dynamic_angle": "Derive material UV rotation from brush-stroke direction instead of the fixed Angle value.",
	"enable_scale": "Write terrain material UV scale while painting the selected texture.",
	"scale": "Material UV scale offset, stored in Terrain3D's eight compact scale steps.",
	"gradient_points": "Two terrain points defining the start height, end height, and direction of the Slope tool.",
	"drawable": "Allow the Slope tool to follow brush motion after its two reference points are selected.",
	"landform_width": "Total cross-section width of the generated landform in world meters.",
	"landform_amplitude": "Primary vertical height or depth applied by landform modes that raise or cut terrain.",
	"landform_strength": "Blend strength of the landform result against the existing terrain.",
	"landform_falloff": "Controls how much of the landform width is used to transition back into surrounding terrain.",
	"landform_crest": "Width of the flatter central crest/core before the profile falls toward surrounding terrain.",
	"landform_terrace_step": "Vertical interval between generated terrace levels.",
	"landform_road_crown": "Height difference between the road/trail center and its edges; negative values create a shallow channel.",
	"landform_water_level": "Reference world elevation used by shoreline and water-relative landform operations.",
	"landform_shore_slope": "Target shoreline grade used to transition terrain toward the configured water level.",
	"landform_dune_spacing": "Approximate distance between repeated dune crests along the generated field.",
	"landform_repose": "Material angle of repose used by talus and thermal-relaxation operations.",
	"landform_iterations": "Number of relaxation/morphology passes. More passes increase authoring cost and the spatial extent of the result.",
	"landform_morphology": "Selects whether morphology expands peaks, fills pits, sharpens ridges/valleys, or cleans plateau surfaces.",
	"landform_path_spacing": "Distance between samples taken along the authored stroke. Lower values follow curves more closely and cost more authoring work.",
	"landform_noise": "Amount of procedural profile variation added to supported landforms.",
	"landform_noise_scale": "World-space size of procedural shape noise. Larger values produce broader variation.",
	"landform_seed": "Deterministic random seed for landform noise and procedural variation.",
	"landform_overlay_texture": "Terrain texture asset ID used as the optional overlay material for generated landforms.",
	"landform_material_blend": "Blend amount between the selected base terrain material and the configured overlay material.",
	"landform_mesh_depth": "Distance the procedural cliff upper cap extends back into the raised terrain.",
	"landform_mesh_irregularity": "Amount of large-scale positional variation applied to generated cliff geometry.",
	"landform_mesh_spacing": "Distance between generated procedural cliff samples along the stroke.",
	"landform_mesh_toe": "Distance the bottom of generated cliff geometry embeds into terrain to hide seams.",
	"landform_module_length": "Nominal world-space length used when placing reusable modular cliff face assets.",
	"landform_module_corner_angle": "Stroke-angle change above which a modular corner asset is preferred over a straight face module.",
	"landform_module_min_scale": "Minimum allowed longitudinal scale for modular cliff pieces when fitting them to the stroke.",
	"landform_module_max_scale": "Maximum allowed longitudinal scale for modular cliff pieces when fitting them to the stroke.",
	"landform_generate_collision": "Generate collision for procedural cliff geometry or enable collision ownership for placed modular cliff assets.",
	"instance_edit_mode": "Select and transform individual Ground Cover or Occluder instances with Godot's native 3D transform gizmo. Click once to select; Shift-click switches the active instance while a gizmo is selected.",
	"mesh_picker": "Pick an existing Terrain3D mesh instance from the viewport and select its mesh asset.",
	"height_offset": "World-space vertical offset applied to newly painted mesh instances.",
	"random_height": "Random vertical offset range added independently to each newly painted instance.",
	"fixed_scale": "Base scale applied to every newly painted mesh instance.",
	"random_scale": "Random scale variation applied around Fixed Scale for each new instance.",
	"fixed_spin": "Base yaw rotation applied to newly painted mesh instances.",
	"random_spin": "Random yaw variation added to Fixed Spin for each new instance.",
	"fixed_tilt": "Base tilt applied to newly painted mesh instances.",
	"random_tilt": "Random tilt variation added to Fixed Tilt for each new instance.",
	"align_to_normal": "Orient newly painted instances to follow the sampled terrain surface normal.",
	"vertex_color": "Base per-instance color written to newly painted terrain instances.",
	"random_hue": "Random hue variation applied to the per-instance color.",
	"random_darken": "Random brightness reduction applied to the per-instance color.",
	"on_collision": "Place instances against collision results rather than only Terrain3D height samples.",
	"raycast_height": "Vertical ray distance used when On Collision searches for a placement surface.",
	"invert": "Invert the selected additive/subtractive painter operation without holding Ctrl.",
	"auto_regions": "Automatically create missing terrain regions when Sculpt or Set Height paints into empty region slots.",
	"show_brush_texture": "Show the selected brush mask in the viewport decal while painting.",
	"align_to_view": "Rotate the brush mask to follow the editor camera yaw.",
	"brush_spin_speed": "Random rotation range applied to the brush mask for each stamp to reduce repeated brush patterns.",
	"gamma": "Raises brush-mask intensity to this gamma before painting. Lower values broaden soft influence; higher values concentrate it.",
}

# Add settings flags
const NONE: int = 0x0
const ALLOW_LARGER: int = 0x1
const ALLOW_SMALLER: int = 0x2
const ALLOW_OUT_OF_BOUNDS: int = 0x3 # LARGER|SMALLER
const NO_LABEL: int = 0x4
const ADD_SEPARATOR: int = 0x8 # Add a vertical line before this entry
const ADD_SPACER: int = 0x10 # Add a space before this entry
const NO_SAVE: int = 0x20 # Don't save this in EditorSettings

var plugin: Terrain3DEditorPluginBase
var asset_dock: Terrain3DAssetDock
var select_brush_button: Button
var selected_brush_imgs: Array[Resource] = []
var main_list: HBoxContainer
var settings_scroll: ScrollContainer
var context_title: Label
var context_help: Label
var context_action: Button
var _context_action_callback: Callable
var advanced_list: VBoxContainer
var height_list: VBoxContainer
var scale_list: VBoxContainer
var rotation_list: VBoxContainer
var color_list: VBoxContainer
var collision_list: VBoxContainer
var landform_shape_list: VBoxContainer
var landform_surface_list: VBoxContainer
var landform_mesh_list: VBoxContainer
var settings: Dictionary[StringName, Object] = {}


func _ready() -> void:
	# Remove old editor settings, newer first so oldest can be removed
	for setting: String in ["jitter", "lift_floor", "flatten_peaks", "lift_flatten", "automatic_regions",
			"show_cursor_while_painting", "crosshair_threshold"]:
		plugin.erase_setting(ES_TOOL_SETTINGS + setting)

	# Stable one-row settings layout. Horizontal scrolling is deliberate: controls no
	# longer jump between rows as the editor or bottom panel changes width.
	custom_minimum_size.y = EditorUI.scaled(92.0)
	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 4)
	add_child(root, true)

	var context_row := HBoxContainer.new()
	context_row.add_theme_constant_override("separation", 10)
	root.add_child(context_row)
	context_title = Label.new()
	context_title.text = "Terrain Tool"
	context_title.add_theme_font_size_override("font_size", int(EditorUI.scaled(14.0)))
	context_row.add_child(context_title)
	context_help = Label.new()
	context_help.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	context_help.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	context_help.modulate.a = 0.72
	context_row.add_child(context_help)
	context_action = Button.new()
	context_action.hide()
	EditorUI.configure_action_button(context_action)
	context_action.pressed.connect(_on_context_action_pressed)
	context_row.add_child(context_action)

	settings_scroll = ScrollContainer.new()
	settings_scroll.name = "SettingsScroll"
	settings_scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	settings_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	settings_scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	settings_scroll.gui_input.connect(_on_settings_scroll_input)
	root.add_child(settings_scroll)

	main_list = HBoxContainer.new()
	main_list.add_theme_constant_override("separation", 4)
	main_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	settings_scroll.add_child(main_list, true)

	add_brushes(main_list)

	add_setting({ "name":"instructions", "label":"Click the terrain to add a region. CTRL+Click to remove. Or select another tool on the left.",
		"type":SettingType.LABEL, "list":main_list, "flags":NO_LABEL|NO_SAVE })

	add_setting({ "name":"size", "type":SettingType.SLIDER, "list":main_list, "default":20, "unit":"m",
							"range":Vector3(0.1, 200, 1), "flags":ALLOW_LARGER|ADD_SPACER })

	add_setting({ "name":"strength", "type":SettingType.SLIDER, "list":main_list, "default":33,
							"unit":"%", "range":Vector3(1, 100, 1), "flags":ALLOW_LARGER })

	add_setting({ "name":"height", "type":SettingType.SLIDER, "list":main_list, "default":20,
							"unit":"m", "range":Vector3(-500, 500, 0.1), "flags":ALLOW_OUT_OF_BOUNDS })
	add_setting({ "name":"height_picker", "type":SettingType.PICKER, "list":main_list, "default":Terrain3DEditor.HEIGHT,
							"flags":NO_LABEL, "tooltip":"Pick Height from the terrain." })

	add_setting({ "name":"color", "type":SettingType.COLOR_SELECT, "list":main_list,
							"default":Color.WHITE, "flags":ADD_SEPARATOR })
	add_setting({ "name":"color_picker", "type":SettingType.PICKER, "list":main_list, "default":Terrain3DEditor.COLOR,
							"flags":NO_LABEL, "tooltip":"Pick Color from the terrain." })

	add_setting({ "name":"roughness", "type":SettingType.SLIDER, "list":main_list, "default":-50,
							"unit":"%", "range":Vector3(-100, 100, 1), "flags":ADD_SEPARATOR })
	add_setting({ "name":"roughness_picker", "type":SettingType.PICKER, "list":main_list, "default":Terrain3DEditor.ROUGHNESS,
							"flags":NO_LABEL, "tooltip":"Pick Wetness from the terrain." })

	add_setting({ "name":"enable_texture", "label":"Texture", "type":SettingType.CHECKBOX,
							"list":main_list, "default":true, "flags":ADD_SEPARATOR })

	add_setting({ "name":"texture_picker", "type":SettingType.PICKER, "list":main_list, "default":Terrain3DEditor.TEXTURE,
							"flags":NO_LABEL, "tooltip":"Pick Texture from the terrain." })

	add_setting({ "name":"texture_filter", "label":"Texture Filter", "type":SettingType.CHECKBOX,
							"list":main_list, "default":false, "flags":ADD_SEPARATOR })

	add_setting({ "name":"margin", "type":SettingType.SLIDER, "list":main_list, "default":0,
							"unit":"", "range":Vector3(-50, 50, 1), "flags":ALLOW_OUT_OF_BOUNDS })

	# Slope painting filter
	add_setting({ "name":"slope", "type":SettingType.DOUBLE_SLIDER, "list":main_list, "default":Vector2(0, 90),
							"unit":"°", "range":Vector3(0, 90, 1), "flags":ADD_SEPARATOR })

	add_setting({ "name":"enable_angle", "label":"Angle", "type":SettingType.CHECKBOX,
							"list":main_list, "default":true, "flags":ADD_SEPARATOR })
	add_setting({ "name":"angle", "type":SettingType.SLIDER, "list":main_list, "default":0,
							"unit":"%", "range":Vector3(0, 337.5, 22.5), "flags":NO_LABEL })
	add_setting({ "name":"angle_picker", "type":SettingType.PICKER, "list":main_list, "default":Terrain3DEditor.ANGLE,
							"flags":NO_LABEL, "tooltip":"Pick Angle from the terrain." })
	add_setting({ "name":"dynamic_angle", "label":"Dynamic", "type":SettingType.CHECKBOX,
							"list":main_list, "default":false, "flags":ADD_SPACER })

	add_setting({ "name":"enable_scale", "label":"Scale", "type":SettingType.CHECKBOX,
							"list":main_list, "default":true, "flags":ADD_SEPARATOR })
	add_setting({ "name":"scale", "label":"±", "type":SettingType.SLIDER, "list":main_list, "default":0,
							"unit":"%", "range":Vector3(-60, 80, 20), "flags":NO_LABEL })
	add_setting({ "name":"scale_picker", "type":SettingType.PICKER, "list":main_list, "default":Terrain3DEditor.SCALE,
							"flags":NO_LABEL, "tooltip":"Pick Scale from the terrain." })

	## Slope sculpting brush
	add_setting({ "name":"gradient_points", "type":SettingType.MULTI_PICKER, "label":"Points",
							"list":main_list, "default":Terrain3DEditor.SCULPT, "flags":ADD_SEPARATOR })
	add_setting({ "name":"drawable", "type":SettingType.CHECKBOX, "list":main_list, "default":false,
							"flags":ADD_SEPARATOR })
	settings["drawable"].toggled.connect(_on_drawable_toggled)


	## Semantic landform authoring
	add_setting({ "name":"landform_mode", "label":"Landform", "type":SettingType.OPTION,
							"list":main_list, "default":Terrain3DLandformBaker.RIDGE,
							"items":PackedStringArray(["Ridge", "Valley", "Scarp / Cliff", "Basin", "Plateau / Mesa",
								"Terrace", "Road / Trail", "Mountain Pass", "Talus / Debris", "Hydraulic Channel",
								"Thermal Relaxation", "Dune", "Shoreline", "Morphology", "Occlusion"]),
							"flags":ADD_SEPARATOR,
							"tooltip":"Draw a stroke directly on the terrain. Release to commit the selected landform." })
	add_setting({ "name":"landform_width", "label":"Width", "type":SettingType.SLIDER, "list":main_list,
							"default":48, "unit":"m", "range":Vector3(2, 512, 1), "flags":ALLOW_LARGER })
	add_setting({ "name":"landform_amplitude", "label":"Height / Depth", "type":SettingType.SLIDER,
							"list":main_list, "default":24, "unit":"m", "range":Vector3(0, 300, 0.5),
							"flags":ALLOW_LARGER })
	add_setting({ "name":"landform_strength", "label":"Strength", "type":SettingType.SLIDER,
							"list":main_list, "default":100, "unit":"%", "range":Vector3(1, 100, 1) })

	landform_shape_list = create_submenu(main_list, "Shape", Layout.VERTICAL)
	add_setting({ "name":"landform_falloff", "label":"Edge Falloff", "type":SettingType.SLIDER,
							"list":landform_shape_list, "default":65, "unit":"%", "range":Vector3(1, 100, 1) })
	add_setting({ "name":"landform_crest", "label":"Flat Core / Crest", "type":SettingType.SLIDER,
							"list":landform_shape_list, "default":18, "unit":"%", "range":Vector3(0, 95, 1) })
	add_setting({ "name":"landform_asymmetry", "label":"Profile Bias", "type":SettingType.SLIDER,
							"list":landform_shape_list, "default":0, "unit":"%", "range":Vector3(-100, 100, 1),
							"flags":ALLOW_OUT_OF_BOUNDS,
							"tooltip":"Moves the strongest part of the profile toward one side without changing the selected raised side." })
	add_setting({ "name":"landform_cliff_side", "label":"Raised Side", "type":SettingType.OPTION,
							"list":landform_shape_list, "default":1,
							"items":PackedStringArray(["Left of Stroke", "Right of Stroke"]),
							"tooltip":"Chooses the raised side for a scarp and the visible cliff-face side for an occlusion mesh." })
	add_setting({ "name":"landform_terrace_step", "label":"Terrace Interval", "type":SettingType.SLIDER,
							"list":landform_shape_list, "default":4, "unit":"m", "range":Vector3(0.1, 64, 0.1),
							"flags":ALLOW_LARGER })
	add_setting({ "name":"landform_road_crown", "label":"Road Crown", "type":SettingType.SLIDER,
							"list":landform_shape_list, "default":0.15, "unit":"m", "range":Vector3(-4, 4, 0.05),
							"flags":ALLOW_OUT_OF_BOUNDS })
	add_setting({ "name":"landform_water_level", "label":"Water Level", "type":SettingType.SLIDER,
							"list":landform_shape_list, "default":0, "unit":"m", "range":Vector3(-384, 768, 0.25),
							"flags":ALLOW_OUT_OF_BOUNDS })
	add_setting({ "name":"landform_shore_slope", "label":"Shore Grade", "type":SettingType.SLIDER,
							"list":landform_shape_list, "default":8, "unit":"%", "range":Vector3(-100, 100, 1),
							"flags":ALLOW_OUT_OF_BOUNDS })
	add_setting({ "name":"landform_dune_spacing", "label":"Dune Spacing", "type":SettingType.SLIDER,
							"list":landform_shape_list, "default":28, "unit":"m", "range":Vector3(2, 256, 1),
							"flags":ALLOW_LARGER })
	add_setting({ "name":"landform_repose", "label":"Angle of Repose", "type":SettingType.SLIDER,
							"list":landform_shape_list, "default":36, "unit":"°", "range":Vector3(1, 89, 1) })
	add_setting({ "name":"landform_iterations", "label":"Relaxation Passes", "type":SettingType.SLIDER,
							"list":landform_shape_list, "default":4, "range":Vector3(1, 32, 1) })
	add_setting({ "name":"landform_morphology", "label":"Morphology", "type":SettingType.OPTION,
							"list":landform_shape_list, "default":Terrain3DLandformBaker.MORPH_EXPAND_PEAKS,
							"items":PackedStringArray(["Expand Peaks", "Fill Pits", "Sharpen Ridges", "Sharpen Valleys", "Clean Plateaus"]) })
	add_setting({ "name":"landform_path_spacing", "label":"Stroke Sampling", "type":SettingType.SLIDER,
							"list":landform_shape_list, "default":2, "unit":"m", "range":Vector3(0.25, 32, 0.25),
							"flags":ALLOW_OUT_OF_BOUNDS })
	add_setting({ "name":"landform_basin_floor_width", "label":"Flat Floor Width", "type":SettingType.SLIDER,
							"list":landform_shape_list, "default":35, "unit":"%", "range":Vector3(0, 90, 1),
							"tooltip":"Width of the depositional floor as a percentage of the basin radius. The remaining width becomes the basin wall and rim transition." })
	add_setting({ "name":"landform_basin_create_regions", "label":"Extend Into Empty Regions",
							"type":SettingType.CHECKBOX, "list":landform_shape_list, "default":false,
							"tooltip":"Allows Basin mode to create new blank Terrain3D regions. Disabled by default so a wide basin cannot silently expand or overwrite the region layout." })

	landform_surface_list = create_submenu(main_list, "Surface", Layout.VERTICAL)
	add_setting({ "name":"landform_noise", "label":"Shape Noise", "type":SettingType.SLIDER,
							"list":landform_surface_list, "default":12, "unit":"%", "range":Vector3(0, 100, 1) })
	add_setting({ "name":"landform_noise_scale", "label":"Noise Scale", "type":SettingType.SLIDER,
							"list":landform_surface_list, "default":24, "unit":"m", "range":Vector3(0.25, 512, 0.25),
							"flags":ALLOW_LARGER })
	add_setting({ "name":"landform_seed", "label":"Seed", "type":SettingType.SLIDER,
							"list":landform_surface_list, "default":1337, "range":Vector3(0, 999999, 1),
							"flags":ALLOW_LARGER })
	add_setting({ "name":"landform_paint_material", "label":"Paint Selected Texture", "type":SettingType.CHECKBOX,
							"list":landform_surface_list, "default":true,
							"tooltip":"Writes the selected texture asset into the same pixels modified by the landform." })
	add_setting({ "name":"landform_overlay_texture", "label":"Overlay Texture ID", "type":SettingType.SLIDER,
							"list":landform_surface_list, "default":0, "range":Vector3(0, 31, 1) })
	add_setting({ "name":"landform_material_blend", "label":"Overlay Blend", "type":SettingType.SLIDER,
							"list":landform_surface_list, "default":0, "unit":"%", "range":Vector3(0, 100, 1) })

	landform_mesh_list = create_submenu(main_list, "Cliff Mesh", Layout.VERTICAL)
	add_setting({ "name":"landform_generate_mesh", "label":"Generate Cliff Geometry", "type":SettingType.CHECKBOX,
							"list":landform_mesh_list, "default":true,
							"tooltip":"Builds terrain-only, reusable modular, or unique procedural cliff geometry from the stroke." })
	add_setting({ "name":"landform_geometry_mode", "label":"Geometry Mode", "type":SettingType.OPTION,
							"list":landform_mesh_list, "default":Terrain3DLandformBaker.GEOMETRY_PROCEDURAL_UNIQUE,
							"items":PackedStringArray(["Terrain Only", "Modular Instanced", "Procedural Unique"]),
							"tooltip":"Modular mode places reusable Terrain3D mesh assets. Procedural mode creates one hero ribbon mesh." })
	add_setting({ "name":"landform_mesh_depth", "label":"Upper Cap Depth", "type":SettingType.SLIDER,
							"list":landform_mesh_list, "default":5, "unit":"m", "range":Vector3(0.1, 64, 0.1),
							"flags":ALLOW_LARGER })
	add_setting({ "name":"landform_mesh_irregularity", "label":"Mesh Irregularity", "type":SettingType.SLIDER,
							"list":landform_mesh_list, "default":28, "unit":"%", "range":Vector3(0, 100, 1) })
	add_setting({ "name":"landform_mesh_spacing", "label":"Along-Stroke Spacing", "type":SettingType.SLIDER,
							"list":landform_mesh_list, "default":3, "unit":"m", "range":Vector3(0.25, 32, 0.25),
							"flags":ALLOW_OUT_OF_BOUNDS })
	add_setting({ "name":"landform_mesh_lip", "label":"Upper Overlap", "type":SettingType.SLIDER,
							"list":landform_mesh_list, "default":1.25, "unit":"m", "range":Vector3(0, 20, 0.05),
							"flags":ALLOW_LARGER,
							"tooltip":"Extends the top cap into the raised terrain. It does not lift the mesh above the terrain." })
	add_setting({ "name":"landform_mesh_toe", "label":"Lower Embed", "type":SettingType.SLIDER,
							"list":landform_mesh_list, "default":1.5, "unit":"m", "range":Vector3(0, 20, 0.05),
							"flags":ALLOW_LARGER })
	add_setting({ "name":"landform_module_length", "label":"Module Length", "type":SettingType.SLIDER,
							"list":landform_mesh_list, "default":8, "unit":"m", "range":Vector3(1, 64, 0.5),
							"flags":ALLOW_LARGER })
	add_setting({ "name":"landform_module_corner_angle", "label":"Corner Threshold", "type":SettingType.SLIDER,
							"list":landform_mesh_list, "default":18, "unit":"°", "range":Vector3(0, 90, 1) })
	add_setting({ "name":"landform_module_min_scale", "label":"Minimum Module Scale", "type":SettingType.SLIDER,
							"list":landform_mesh_list, "default":65, "unit":"%", "range":Vector3(5, 1000, 1),
							"flags":ALLOW_LARGER })
	add_setting({ "name":"landform_module_max_scale", "label":"Maximum Module Scale", "type":SettingType.SLIDER,
							"list":landform_mesh_list, "default":175, "unit":"%", "range":Vector3(5, 1000, 1),
							"flags":ALLOW_LARGER })
	for index: int in range(4):
		add_setting({ "name":"landform_module_mesh_%d" % index, "label":"Face Mesh ID %d" % (index + 1),
							"type":SettingType.SLIDER, "list":landform_mesh_list, "default":-1,
							"range":Vector3(-1, 255, 1), "flags":ALLOW_OUT_OF_BOUNDS,
							"tooltip":"Terrain3D mesh asset ID. Author +X along the cliff, +Y up, and +Z into the raised terrain. Use -1 to leave the slot empty." })
	for index: int in range(2):
		add_setting({ "name":"landform_corner_mesh_%d" % index, "label":"Corner Mesh ID %d" % (index + 1),
							"type":SettingType.SLIDER, "list":landform_mesh_list, "default":-1,
							"range":Vector3(-1, 255, 1), "flags":ALLOW_OUT_OF_BOUNDS,
							"tooltip":"Optional corner asset using the same +X/+Y/+Z orientation as face modules." })
	for index: int in range(2):
		add_setting({ "name":"landform_end_mesh_%d" % index, "label":"End Mesh ID %d" % (index + 1),
							"type":SettingType.SLIDER, "list":landform_mesh_list, "default":-1,
							"range":Vector3(-1, 255, 1), "flags":ALLOW_OUT_OF_BOUNDS,
							"tooltip":"Optional terminator asset using the same +X/+Y/+Z orientation as face modules." })
	add_setting({ "name":"landform_collision_spacing", "label":"Collision Spacing", "type":SettingType.SLIDER,
							"list":landform_mesh_list, "default":8, "unit":"m", "range":Vector3(1, 64, 0.5),
							"flags":ALLOW_LARGER,
							"tooltip":"Sampling interval for the simplified collision ribbon." })
	add_setting({ "name":"landform_generate_collision", "label":"Generate Collision", "type":SettingType.CHECKBOX,
							"list":landform_mesh_list, "default":true })

	## Instancer
	add_setting({ "name":"instance_edit_mode", "label":"Edit Instances", "type":SettingType.CHECKBOX, "list":main_list,
							"default":false, "flags":ADD_SEPARATOR })
	add_setting({ "name":"mesh_picker", "type":SettingType.PICKER, "list":main_list,
							"default":Terrain3DEditor.INSTANCER, "flags":NO_LABEL|ADD_SEPARATOR,
							"tooltip":"Pick a Mesh asset from the terrain, within an instancer cell. (See overlays.)" })

	height_list = create_submenu(main_list, "Height", Layout.VERTICAL)
	add_setting({ "name":"height_offset", "type":SettingType.SLIDER, "list":height_list, "default":0,
							"unit":"m", "range":Vector3(-10, 10, 0.05), "flags":ALLOW_OUT_OF_BOUNDS })
	add_setting({ "name":"random_height", "label":"Random Height ±", "type":SettingType.SLIDER, "list":height_list,
							"default":0, "unit":"m", "range":Vector3(0, 10, 0.05), "flags":ALLOW_OUT_OF_BOUNDS })

	scale_list = create_submenu(main_list, "Scale", Layout.VERTICAL)
	add_setting({ "name":"fixed_scale", "type":SettingType.SLIDER, "list":scale_list, "default":100,
							"unit":"%", "range":Vector3(1, 1000, 1), "flags":ALLOW_OUT_OF_BOUNDS })
	add_setting({ "name":"random_scale", "label":"Random Scale ±", "type":SettingType.SLIDER, "list":scale_list,
							"default":20, "unit":"%", "range":Vector3(0, 99, 1), "flags":ALLOW_OUT_OF_BOUNDS })

	rotation_list = create_submenu(main_list, "Rotation", Layout.VERTICAL)
	add_setting({ "name":"fixed_spin", "label":"Fixed Spin (Around Y)", "type":SettingType.SLIDER, "list":rotation_list,
							"default":0, "unit":"°", "range":Vector3(0, 360, 1) })
	add_setting({ "name":"random_spin", "type":SettingType.SLIDER, "list":rotation_list, "default":360,
							"unit":"°", "range":Vector3(0, 360, 1) })
	add_setting({ "name":"fixed_tilt", "label":"Fixed Tilt", "type":SettingType.SLIDER, "list":rotation_list,
							"default":0, "unit":"°", "range":Vector3(-85, 85, 1), "flags":ALLOW_OUT_OF_BOUNDS })
	add_setting({ "name":"random_tilt", "label":"Random Tilt ±", "type":SettingType.SLIDER, "list":rotation_list,
							"default":10, "unit":"°", "range":Vector3(0, 85, 1), "flags":ALLOW_OUT_OF_BOUNDS })
	add_setting({ "name":"align_to_normal", "type":SettingType.CHECKBOX, "list":rotation_list, "default":false })

	color_list = create_submenu(main_list, "Color", Layout.VERTICAL)
	add_setting({ "name":"vertex_color", "type":SettingType.COLOR_SELECT, "list":color_list,
							"default":Color.WHITE })
	add_setting({ "name":"random_hue", "label":"Random Hue Shift ±", "type":SettingType.SLIDER,
							"list":color_list, "default":0, "unit":"°", "range":Vector3(0, 360, 1) })
	add_setting({ "name":"random_darken", "type":SettingType.SLIDER, "list":color_list, "default":50,
							"unit":"%", "range":Vector3(0, 100, 1) })
	collision_list = create_submenu(main_list, "Collision", Layout.VERTICAL)
	add_setting({ "name":"on_collision", "label":"On Collision", "type":SettingType.CHECKBOX, "list":collision_list,
							"default":true })
	add_setting({ "name":"raycast_height", "label":"Raycast Height", "type":SettingType.SLIDER,
							"list":collision_list, "default":10, "unit":"m", "range":Vector3(0, 200, .25) })

	if DisplayServer.is_touchscreen_available():
		add_setting({ "name":"invert", "label":"Invert", "type":SettingType.CHECKBOX, "list":main_list, "default":false, "flags":ADD_SEPARATOR })

	var spacer: Control = Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	main_list.add_child(spacer, true)

	## Advanced Settings Menu
	advanced_list = create_submenu(main_list, "", Layout.VERTICAL, false)
	add_setting({ "name":"auto_regions", "label":"Add regions while sculpting", "type":SettingType.CHECKBOX,
							"list":advanced_list, "default":true })
	advanced_list.add_child(HSeparator.new(), true)
	add_setting({ "name":"show_brush_texture", "type":SettingType.CHECKBOX, "list":advanced_list, "default":true })
	add_setting({ "name":"align_to_view", "type":SettingType.CHECKBOX, "list":advanced_list, "default":true })
	add_setting({ "name":"brush_spin_speed", "type":SettingType.SLIDER, "list":advanced_list, "default":50,
							"unit":"%", "range":Vector3(0, 100, 1) })
	add_setting({ "name":"gamma", "type":SettingType.SLIDER, "list":advanced_list, "default":1.0,
							"unit":"γ", "range":Vector3(0.1, 2.0, 0.01) })


func create_submenu(p_parent: Control, p_button_name: String, p_layout: Layout, _p_hover_pop: bool = true) -> Container:
	var menu_button := Button.new()
	menu_button.toggle_mode = true
	EditorUI.configure_action_button(menu_button)
	if p_button_name.is_empty():
		menu_button.icon = get_theme_icon("GuiTabMenuHl", "EditorIcons")
		menu_button.tooltip_text = "Advanced brush settings"
	else:
		menu_button.text = p_button_name
		menu_button.tooltip_text = "Open %s settings" % p_button_name
	menu_button.toggled.connect(_on_show_submenu.bind(menu_button))

	var submenu := PopupPanel.new()
	submenu.popup_hide.connect(menu_button.set_pressed.bind(false))
	submenu.add_to_group("terrain3d_submenus")
	submenu.unresizable = false

	var margin := MarginContainer.new()
	for side: StringName in [&"margin_left", &"margin_top", &"margin_right", &"margin_bottom"]:
		margin.add_theme_constant_override(side, int(EditorUI.scaled(10.0)))
	submenu.add_child(margin)

	var content_root: Control = margin
	var scroll: ScrollContainer
	if p_layout == Layout.GRID:
		scroll = ScrollContainer.new()
		scroll.custom_minimum_size = Vector2(EditorUI.scaled(460.0), EditorUI.scaled(380.0))
		EditorUI.configure_scroll_container(scroll)
		margin.add_child(scroll)
		content_root = scroll

	var sublist: Container
	match p_layout:
		Layout.GRID:
			sublist = GridContainer.new()
			(sublist as GridContainer).columns = 5
		Layout.VERTICAL:
			sublist = VBoxContainer.new()
		Layout.HORIZONTAL, _:
			sublist = HBoxContainer.new()
	sublist.add_theme_constant_override("separation", int(EditorUI.scaled(5.0)))
	sublist.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	content_root.add_child(sublist, true)

	p_parent.add_child(menu_button, true)
	menu_button.add_child(submenu, true)
	sublist.set_meta("terrain3d_menu_button", menu_button)
	sublist.set_meta("terrain3d_popup", submenu)
	return sublist


func _on_show_submenu(p_toggled: bool, p_button: Button) -> void:
	var popup: PopupPanel = p_button.get_child(0) as PopupPanel
	if not p_toggled:
		popup.hide()
		return

	get_tree().call_group("terrain3d_submenus", "hide")
	p_button.set_pressed_no_signal(true)
	popup.popup()
	await get_tree().process_frame
	var popup_minimum := Vector2i(int(EditorUI.scaled(260.0)), int(EditorUI.scaled(160.0)))
	popup.size = EditorUI.popup_size_for_screen(popup, popup.size, popup_minimum)
	EditorUI.position_popup_near_control(popup, p_button, true)


# Brush masks are optional user assets. Keep one compact procedural brush available
# even when the external EXR library is deliberately excluded from the addon.
func _create_default_brush() -> Image:
	const SIZE: int = 256
	var image: Image = Image.create_empty(SIZE, SIZE, false, Image.FORMAT_RF)
	for y: int in range(SIZE):
		var fy: float = (float(y) + 0.5) * 2.0 / float(SIZE) - 1.0
		for x: int in range(SIZE):
			var fx: float = (float(x) + 0.5) * 2.0 / float(SIZE) - 1.0
			var radius: float = sqrt(fx * fx + fy * fy)
			var fade: float = clampf((1.0 - radius) / 0.30, 0.0, 1.0)
			var alpha: float = fade * fade * (3.0 - 2.0 * fade)
			image.set_pixel(x, y, Color(alpha, 0.0, 0.0, 1.0))
	return image


func _append_brush_button(p_list: GridContainer, p_group: ButtonGroup, p_image: Image, p_name: String) -> Button:
	# Gallery icons use a small thumbnail; only the selected brush is sent to the painter.
	var thumbimg: Image = p_image.duplicate()
	if thumbimg.get_width() != 84 or thumbimg.get_height() != 84:
		thumbimg.resize(84, 84, Image.INTERPOLATE_CUBIC)
	thumbimg = Terrain3DUtil.black_to_alpha(thumbimg)
	thumbimg.convert(Image.FORMAT_LA8)
	var thumb_data: PackedByteArray = thumbimg.get_data()
	for pixel_index: int in range(0, thumb_data.size(), 2):
		var mask: float = float(thumb_data[pixel_index + 1]) / 255.0
		thumb_data[pixel_index] = 255
		thumb_data[pixel_index + 1] = int(round(pow(mask, 0.666) * 255.0))
	thumbimg.set_data(thumbimg.get_width(), thumbimg.get_height(), false, Image.FORMAT_LA8, thumb_data)

	var button: Button = Button.new()
	button.name = p_name
	button.set_custom_minimum_size(Vector2.ONE * EditorUI.scaled(84.0))
	button.set_button_icon(ImageTexture.create_from_image(thumbimg))
	button.set_meta("image", p_image)
	button.set_expand_icon(true)
	button.set_toggle_mode(true)
	button.set_button_group(p_group)
	button.mouse_entered.connect(_on_brush_hover.bind(true, button))
	button.mouse_exited.connect(_on_brush_hover.bind(false, button))
	button.tooltip_text = "Select the %s brush mask. Bright pixels apply more of the active painter operation." % p_name
	p_list.add_child(button, true)

	var label: Label = Label.new()
	button.add_child(label, true)
	label.text = p_name
	label.tooltip_text = button.tooltip_text
	label.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_WIDE)
	label.offset_top = -EditorUI.scaled(24.0)
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	label.add_theme_color_override("font_shadow_color", Color.BLACK)
	label.add_theme_constant_override("shadow_offset_x", 1)
	label.add_theme_constant_override("shadow_offset_y", 1)
	label.add_theme_font_size_override("font_size", int(EditorUI.scaled(11.0)))
	return button


func add_brushes(p_parent: Control) -> void:
	var brush_list: GridContainer = create_submenu(p_parent, "Brush", Layout.GRID)
	brush_list.name = "BrushList"
	brush_list.columns = 5

	var brush_button_group: ButtonGroup = ButtonGroup.new()
	brush_button_group.pressed.connect(_on_brush_selected)
	var default_brush_btn: Button = _append_brush_button(
		brush_list, brush_button_group, _create_default_brush(), "Soft Circle")

	# A missing optional brush directory is a valid, asset-free installation.
	var dir: DirAccess = DirAccess.open(BRUSH_PATH)
	if dir:
		var filenames: PackedStringArray = dir.get_files()
		filenames.sort()
		for file_name: String in filenames:
			if not file_name.to_lower().ends_with(".exr"):
				continue
			var image: Image = Image.load_from_file(BRUSH_PATH.path_join(file_name))
			if image == null or image.is_empty():
				continue
			image.convert(Image.FORMAT_RF)
			var button: Button = _append_brush_button(
				brush_list, brush_button_group, image, file_name.get_basename().to_pascal_case())
			if file_name == DEFAULT_BRUSH:
				default_brush_btn = button

	settings["brush"] = brush_button_group
	default_brush_btn.set_pressed_no_signal(true)
	_generate_brush_texture(default_brush_btn)

	select_brush_button = brush_list.get_meta("terrain3d_menu_button") as Button
	select_brush_button.set_text("")
	select_brush_button.set_button_icon(default_brush_btn.get_button_icon())
	select_brush_button.set_custom_minimum_size(EditorUI.icon_button_size())
	select_brush_button.set_icon_alignment(HORIZONTAL_ALIGNMENT_CENTER)
	select_brush_button.set_expand_icon(true)
	select_brush_button.tooltip_text = "Brush: %s" % default_brush_btn.name


func _on_brush_hover(p_hovering: bool, p_button: Button) -> void:
	p_button.modulate = Color(1.12, 1.12, 1.12) if p_hovering else Color.WHITE


func _on_pick(p_type: int) -> void:
	if plugin.debug:
		print("Terrain3DToolSettings: _on_pick: emitting picking: ", p_type, ", ", _on_picked)
	emit_signal("picking", p_type, _on_picked)


func _on_picked(p_type: int, p_color: Color, p_global_position: Vector3) -> void:
	match p_type:
		Terrain3DEditor.HEIGHT:
			settings["height"].value = p_color.r if not is_nan(p_color.r) else 0.
		Terrain3DEditor.COLOR:
			settings["color"].color = p_color if not is_nan(p_color.r) else Color.WHITE
		Terrain3DEditor.ROUGHNESS:
			# This converts 0,1 to -100,100
			# It also quantizes explicitly so picked values matches painted values
			settings["roughness"].value = roundf(200. * float(int(p_color.a * 255.) / 255. - .5)) if not is_nan(p_color.r) else 0.
		Terrain3DEditor.ANGLE:
			settings["angle"].value = p_color.r
		Terrain3DEditor.SCALE:
			settings["scale"].value = p_color.r
		Terrain3DEditor.INSTANCER:
			if p_color.r < 0:
				return
			asset_dock.set_selected_by_asset_id(int(p_color.r), Terrain3DAssets.TYPE_MESH)
		Terrain3DEditor.TEXTURE:
			if p_color.r < 0:
				return
			asset_dock.set_selected_by_asset_id(int(p_color.r), Terrain3DAssets.TYPE_TEXTURE)
	_notify_setting_changed()


func _on_point_pick(p_type: int, p_name: String) -> void:
	assert(p_type == Terrain3DEditor.SCULPT)
	if plugin.debug:
		print("Terrain3DToolSettings: _on_pick: emitting picking: ", p_type, ", ", _on_point_picked)
	emit_signal("picking", p_type, _on_point_picked.bind(p_name))


func _on_point_picked(p_type: int, p_color: Color, p_global_position: Vector3, p_name: String) -> void:
	assert(p_type == Terrain3DEditor.SCULPT)
	var point: Vector3 = p_global_position
	point.y = p_color.r
	settings[p_name].add_point(point)
	_notify_setting_changed()


func add_setting(p_args: Dictionary[String, Variant]) -> void:
	var p_name: StringName = p_args.get("name", "")
	var p_label: String = p_args.get("label", "") # Optional replacement for name
	var p_type: SettingType = p_args.get("type", SettingType.TYPE_MAX)
	var p_list: Control = p_args.get("list") as Control
	var p_default: Variant = p_args.get("default")
	var p_suffix: String = p_args.get("unit", "")
	var p_range: Vector3 = p_args.get("range", Vector3(0, 0, 1))
	var p_minimum: float = p_range.x
	var p_maximum: float = p_range.y
	var p_step: float = p_range.z
	var p_flags: int = p_args.get("flags", NONE)
	var p_tooltip: String = p_args.get("tooltip", SETTING_TOOLTIPS.get(String(p_name), ""))
	var p_items: PackedStringArray = p_args.get("items", PackedStringArray())

	if p_name.is_empty() or p_type == SettingType.TYPE_MAX:
		return
	if p_tooltip.is_empty():
		push_error("Terrain3D painter setting '%s' has no UI documentation tooltip." % p_name)

	var container: HBoxContainer = HBoxContainer.new()
	container.custom_minimum_size.y = EditorUI.control_height()
	container.set_v_size_flags(SIZE_SHRINK_CENTER)
	var control: Control	# Houses the setting to be saved
	var pending_children: Array[Control]

	match p_type:
		SettingType.LABEL:
			var label := Label.new()
			label.set_text(p_label)
			label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
			label.size_flags_vertical = Control.SIZE_SHRINK_CENTER
			pending_children.push_back(label)
			control = label

		SettingType.CHECKBOX:
			var checkbox := CheckBox.new()
			EditorUI.configure_compact_control(checkbox)
			if !(p_flags & NO_SAVE):
				checkbox.set_pressed_no_signal(plugin.get_setting(ES_TOOL_SETTINGS + p_name, p_default))
				checkbox.toggled.connect( (
					func(value: bool, path: String) -> void:
						plugin.set_setting(path, value)
				).bind(ES_TOOL_SETTINGS + p_name) )
			else:
				checkbox.set_pressed_no_signal(p_default)
			checkbox.pressed.connect(_notify_setting_changed)
			pending_children.push_back(checkbox)
			control = checkbox

		SettingType.COLOR_SELECT:
			var picker := ColorPickerButton.new()
			picker.set_custom_minimum_size(Vector2(EditorUI.scaled(120.0), EditorUI.control_height()))
			picker.size_flags_vertical = Control.SIZE_SHRINK_CENTER
			picker.edit_alpha = false
			picker.get_picker().set_color_mode(ColorPicker.MODE_HSV)
			if !(p_flags & NO_SAVE):
				picker.set_pick_color(plugin.get_setting(ES_TOOL_SETTINGS + p_name, p_default))
				picker.color_changed.connect( (
					func(value: Color, path: String) -> void:
						plugin.set_setting(path, value)
				).bind(ES_TOOL_SETTINGS + p_name) )
			else:
				picker.set_pick_color(p_default)
			picker.color_changed.connect(func(_value: Color) -> void: _notify_setting_changed())
			pending_children.push_back(picker)
			control = picker

		SettingType.PICKER:
			var button := Button.new()
			button.set_v_size_flags(SIZE_SHRINK_CENTER)
			EditorUI.configure_icon_button(button)
			button.icon = get_theme_icon("ColorPick", "EditorIcons")
			button.pressed.connect(_on_pick.bind(p_default))
			pending_children.push_back(button)
			control = button

		SettingType.MULTI_PICKER:
			var multi_picker: MultiPicker = MultiPicker.new()
			multi_picker.pressed.connect(_on_point_pick.bind(p_default, p_name))
			multi_picker.value_changed.connect(func(_value: PackedVector3Array) -> void: _notify_setting_changed())
			pending_children.push_back(multi_picker)
			control = multi_picker

		SettingType.OPTION:
			var option := OptionButton.new()
			EditorUI.configure_compact_control(option)
			if p_items.is_empty():
				for i: int in int(p_maximum):
					option.add_item(str(i), i)
			else:
				for i: int in p_items.size():
					option.add_item(p_items[i], i)
			var saved_option: int = int(p_default)
			if not (p_flags & NO_SAVE):
				saved_option = int(plugin.get_setting(ES_TOOL_SETTINGS + p_name, p_default))
			option.select(clampi(saved_option, 0, maxi(option.item_count - 1, 0)))
			if not (p_flags & NO_SAVE):
				option.item_selected.connect(func(value: int) -> void:
					plugin.set_setting(ES_TOOL_SETTINGS + p_name, value)
				)
			option.item_selected.connect(func(_value: int) -> void:
				_notify_setting_changed()
			)
			pending_children.push_back(option)
			control = option

		SettingType.SLIDER:
			var spin_slider := EditorSpinSlider.new()
			spin_slider.set_flat(false)
			spin_slider.set_hide_slider(true)
			spin_slider.value_changed.connect(func(_value: float) -> void: _notify_setting_changed())
			spin_slider.set_max(p_maximum)
			spin_slider.set_min(p_minimum)
			spin_slider.set_step(p_step)
			spin_slider.set_suffix(p_suffix)
			spin_slider.set_v_size_flags(SIZE_SHRINK_CENTER)
			spin_slider.set_custom_minimum_size(Vector2(EditorUI.scaled(82.0), EditorUI.control_height()))

			var slider := HSlider.new()
			slider.share(spin_slider)
			if p_flags & ALLOW_LARGER:
				slider.set_allow_greater(true)
			if p_flags & ALLOW_SMALLER:
				slider.set_allow_lesser(true)
			slider.set_min(p_minimum)
			slider.set_max(p_maximum)
			slider.set_step(p_step)
			slider.set_v_size_flags(SIZE_SHRINK_CENTER)
			slider.set_custom_minimum_size(Vector2(EditorUI.scaled(112.0), EditorUI.scaled(12.0)))
			if p_flags & NO_SAVE:
				slider.set_value(float(p_default))
			else:
				slider.set_value(float(plugin.get_setting(ES_TOOL_SETTINGS + p_name, p_default)))
				slider.value_changed.connect((func(value: float, path: String) -> void:
					plugin.set_setting(path, value)
				).bind(ES_TOOL_SETTINGS + p_name))
			pending_children.push_back(slider)
			pending_children.push_back(spin_slider)
			control = spin_slider

		SettingType.DOUBLE_SLIDER:
			var label := Label.new()
			label.set_custom_minimum_size(Vector2(EditorUI.scaled(70.0), 0))
			label.set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER)
			var slider := DoubleSlider.new()
			slider.label = label
			slider.suffix = p_suffix
			slider.set_min(p_minimum)
			slider.set_max(p_maximum)
			slider.set_step(p_step)
			slider.set_v_size_flags(SIZE_SHRINK_CENTER)
			slider.set_custom_minimum_size(Vector2(EditorUI.scaled(112.0), EditorUI.scaled(12.0)))
			slider.value_changed.connect(func(_value: Vector2) -> void: _notify_setting_changed())
			if p_flags & NO_SAVE:
				slider.set_value(p_default as Vector2)
			else:
				slider.set_value(plugin.get_setting(ES_TOOL_SETTINGS + p_name, p_default) as Vector2)
				slider.value_changed.connect((func(value: Vector2, path: String) -> void:
					plugin.set_setting(path, value)
				).bind(ES_TOOL_SETTINGS + p_name))
			pending_children.push_back(slider)
			pending_children.push_back(label)
			control = slider

	control.name = p_name.to_pascal_case()
	if not p_tooltip.is_empty():
		control.tooltip_text = p_tooltip
		for documented_child: Control in pending_children:
			if documented_child.tooltip_text.is_empty():
				documented_child.tooltip_text = p_tooltip
	settings[p_name] = control

	# Setup button labels
	if not (p_flags & NO_LABEL):
		# Labels are actually buttons styled to look like labels
		var label := Button.new()
		EditorUI.configure_compact_control(label)
		label.set("theme_override_styles/normal", get_theme_stylebox("normal", "Label"))
		label.set("theme_override_styles/hover", get_theme_stylebox("normal", "Label"))
		label.set("theme_override_styles/pressed", get_theme_stylebox("normal", "Label"))
		label.set("theme_override_styles/focus", get_theme_stylebox("normal", "Label"))
		label.pressed.connect(_on_label_pressed.bind(p_name, p_default))
		label.tooltip_text = (p_tooltip + "\n" if not p_tooltip.is_empty() else "") + "Click the label to reset this setting."
		container.set_meta(&"terrain3d_setting_label", label)
		if p_label.is_empty():
			label.set_text(p_name.capitalize() + ": ")
		else:
			label.set_text(p_label.capitalize() + ": ")
		pending_children.push_front(label)

	# Add separators to front
	if p_flags & ADD_SEPARATOR:
		pending_children.push_front(VSeparator.new())
	if p_flags & ADD_SPACER:
		var spacer := Control.new()
		spacer.set_custom_minimum_size(Vector2(5, 0))
		pending_children.push_front(spacer)

	# Add all children to container and list
	for child: Control in pending_children:
		container.add_child(child, true)
	container.set_meta(&"terrain3d_setting_name", p_name)
	p_list.add_child(container, true)


func set_setting_label(p_setting: StringName, p_label: String) -> void:
	var object: Object = settings.get(p_setting)
	if not object is Control:
		return
	var row: Control = (object as Control).get_parent() as Control
	if not is_instance_valid(row):
		return
	var label: Button = row.get_meta(&"terrain3d_setting_label", null) as Button
	if is_instance_valid(label):
		label.text = p_label + ": "


func set_setting_tooltip(p_setting: StringName, p_tooltip: String) -> void:
	var object: Object = settings.get(p_setting)
	if not object is Control:
		return
	var control := object as Control
	control.tooltip_text = p_tooltip
	var row: Control = control.get_parent() as Control
	if not is_instance_valid(row):
		return
	for child: Node in row.find_children("*", "Control", true, false):
		var documented_child := child as Control
		if documented_child:
			documented_child.tooltip_text = p_tooltip
	var label: Button = row.get_meta(&"terrain3d_setting_label", null) as Button
	if is_instance_valid(label):
		label.tooltip_text = p_tooltip + "\nClick the label to reset this setting."


# If label button is pressed, reset value to default or toggle checkbox
func _on_label_pressed(p_name: String, p_default: Variant) -> void:
	var control: Control = settings.get(p_name) as Control
	if not control:
		return
	if control is CheckBox:
		var checkbox := control as CheckBox
		set_setting(p_name, not checkbox.button_pressed)
	elif p_default != null:
		set_setting(p_name, p_default)


func get_settings() -> Dictionary[StringName, Variant]:
	var dict: Dictionary[StringName, Variant] = {}
	for key: StringName in settings.keys():
		dict[key] = get_setting(key)
	return dict


func get_setting(p_setting: String) -> Variant:
	var object: Object = settings.get(p_setting)
	var value: Variant
	if object is Range:
		value = (object as Range).get_value()
	elif object is DoubleSlider:
		value = (object as DoubleSlider).get_value()
	elif object is ButtonGroup: # "brush"
		value = selected_brush_imgs
	elif object is CheckBox:
		value = (object as CheckBox).is_pressed()
	elif object is ColorPickerButton:
		value = (object as ColorPickerButton).color
	elif object is OptionButton:
		value = (object as OptionButton).get_selected_id()
	elif object is MultiPicker:
		value = (object as MultiPicker).get_points()
	if value == null:
		value = 0
	return value


func set_setting(p_setting: String, p_value: Variant) -> void:
	var object: Object = settings.get(p_setting)
	if object is DoubleSlider: # Expects p_value is Vector2
		(object as DoubleSlider).set_value(p_value)
	elif object is Range:
		(object as Range).set_value(float(p_value))
	elif object is ButtonGroup: # Expects p_value is Array [ "button name", boolean ]
		if p_value is Array and p_value.size() == 2 and \
				(p_value[0] is String or p_value[0] is StringName) and p_value[1] is bool:
			var target_name: StringName = StringName(p_value[0])
			var pressed: bool = bool(p_value[1])
			for button: BaseButton in (object as ButtonGroup).get_buttons():
				if button.name == target_name:
					button.button_pressed = pressed
	elif object is CheckBox:
		(object as CheckBox).button_pressed = bool(p_value)
	elif object is ColorPickerButton:
		(object as ColorPickerButton).color = p_value as Color
		plugin.set_setting(ES_TOOL_SETTINGS + p_setting, p_value) # Signal doesn't fire on CPB
	elif object is OptionButton:
		(object as OptionButton).select(clampi(int(p_value), 0, maxi((object as OptionButton).item_count - 1, 0)))
		plugin.set_setting(ES_TOOL_SETTINGS + p_setting, int(p_value))
	elif object is MultiPicker: # Expects p_value is PackedVector3Array
		(object as MultiPicker).points = p_value as PackedVector3Array
	_notify_setting_changed()


func set_setting_row_visible(p_setting: StringName, p_visible: bool) -> void:
	var object: Object = settings.get(p_setting)
	if object is Control:
		var row := (object as Control).get_parent() as Control
		if is_instance_valid(row):
			row.visible = p_visible


func show_settings(p_settings: PackedStringArray) -> void:
	for setting: StringName in settings.keys():
		var object: Object = settings[setting]
		if object is Control:
			var row := (object as Control).get_parent() as Control
			if not is_instance_valid(row):
				continue
			if setting in p_settings:
				row.show()
			else:
				row.hide()
	if select_brush_button:
		if not "brush" in p_settings:
			select_brush_button.hide()
		else:
			select_brush_button.show()


func _on_brush_selected(p_button: BaseButton) -> void:
	if not p_button is Button or p_button.get_parent().name != "BrushList":
		_notify_setting_changed()
		return
	var brush_button := p_button as Button
	_generate_brush_texture(brush_button)
	if select_brush_button:
		select_brush_button.set_button_icon(brush_button.get_button_icon())
		select_brush_button.tooltip_text = "Brush: %s" % brush_button.name
	var brush_list: Control = brush_button.get_parent()
	var popup: PopupPanel = brush_list.get_meta("terrain3d_popup", null) as PopupPanel
	if is_instance_valid(popup):
		popup.hide()
	_notify_setting_changed()


func _notify_setting_changed() -> void:
	if plugin.debug:
		print("Terrain3DToolSettings: emitting setting_changed")
	setting_changed.emit()


func _generate_brush_texture(p_btn: Button) -> void:
	if p_btn is Button:
		var img: Image = p_btn.get_meta("image") as Image
		# Preserve native mask resolution. Upscaling every brush to 1024 x 1024
		# needlessly creates CPU and GPU copies and does not add mask detail.
		var tex: ImageTexture = ImageTexture.create_from_image(img)
		selected_brush_imgs = [img, tex]


func _on_drawable_toggled(p_button_pressed: bool) -> void:
	if not p_button_pressed:
		settings["gradient_points"].clear()



# Counts digits of a number including negative sign, decimal points, and up to 3 decimals
func _count_digits(p_value: float) -> int:
	var count: int = 1
	for i: int in range(5, 0, -1):
		if absf(p_value) >= pow(10, i):
			count = i+1
			break
	if p_value - floorf(p_value) >= .1:
		count += 1 # For the decimal
		if p_value*10 - floorf(p_value*10.) >= .1:
			count += 1
			if p_value*100 - floorf(p_value*100.) >= .1:
				count += 1
				if p_value*1000 - floorf(p_value*1000.) >= .1:
					count += 1
	# Negative sign
	if p_value < 0:
		count += 1
	return count


func set_context(p_title: String, p_help: String) -> void:
	if is_instance_valid(context_title):
		context_title.text = p_title
	if is_instance_valid(context_title):
		context_title.tooltip_text = p_help
	if is_instance_valid(context_help):
		context_help.text = p_help
		context_help.tooltip_text = p_help


func set_context_action(p_text: String = "", p_tooltip: String = "", p_callback: Callable = Callable()) -> void:
	_context_action_callback = p_callback
	if not is_instance_valid(context_action):
		return
	context_action.text = p_text
	context_action.tooltip_text = p_tooltip
	context_action.visible = not p_text.is_empty() and p_callback.is_valid()


func _on_context_action_pressed() -> void:
	if _context_action_callback.is_valid():
		_context_action_callback.call()


func set_submenu_visible(p_list: Control, p_visible: bool) -> void:
	if not is_instance_valid(p_list):
		return
	var menu_button: Button = p_list.get_meta("terrain3d_menu_button", null) as Button
	if is_instance_valid(menu_button):
		menu_button.visible = p_visible
		if not p_visible:
			var popup: PopupPanel = p_list.get_meta("terrain3d_popup", null) as PopupPanel
			if is_instance_valid(popup):
				popup.hide()


func _on_settings_scroll_input(p_event: InputEvent) -> void:
	if not p_event is InputEventMouseButton:
		return
	var mouse_button := p_event as InputEventMouseButton
	if not mouse_button.pressed:
		return
	if mouse_button.button_index == MOUSE_BUTTON_WHEEL_UP:
		settings_scroll.scroll_horizontal -= int(EditorUI.scaled(64.0))
		settings_scroll.accept_event()
	elif mouse_button.button_index == MOUSE_BUTTON_WHEEL_DOWN:
		settings_scroll.scroll_horizontal += int(EditorUI.scaled(64.0))
		settings_scroll.accept_event()


func inverse_slope_range() -> void:
	var slope_range: Vector2 = get_setting("slope")
	if slope_range.y - slope_range.x > 89.99:
		return
	if slope_range.x == 0.0:
		slope_range = Vector2(slope_range.y, 90.0)
	elif slope_range.y == 90.0:
		slope_range = Vector2(0.0, slope_range.x)
	else:
		# If midpoint <= 45, inverse to 90, else to 0
		var midpoint: float = 0.5 * (slope_range.x + slope_range.y)
		if midpoint <= 45.0:
			slope_range = Vector2(slope_range.y, 90.0)
		else:
			slope_range = Vector2(0.0, slope_range.x)
	set_setting("slope", slope_range)
