# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Toolbar for Terrain3D
extends VFlowContainer

signal tool_changed(p_tool: int, p_operation: int)

const ICON_REGION_ADD: String = "res://addons/terrain_3d/icons/region_add.svg"
const ICON_REGION_REMOVE: String = "res://addons/terrain_3d/icons/region_remove.svg"
const ICON_HEIGHT_ADD: String = "res://addons/terrain_3d/icons/height_add.svg"
const ICON_HEIGHT_SUB: String = "res://addons/terrain_3d/icons/height_sub.svg"
const ICON_HEIGHT_FLAT: String = "res://addons/terrain_3d/icons/height_flat.svg"
const ICON_HEIGHT_SLOPE: String = "res://addons/terrain_3d/icons/height_slope.svg"
const ICON_HEIGHT_SMOOTH: String = "res://addons/terrain_3d/icons/height_smooth.svg"
const ICON_PAINT_TEXTURE: String = "res://addons/terrain_3d/icons/texture_paint.svg"
const ICON_SPRAY_TEXTURE: String = "res://addons/terrain_3d/icons/texture_spray.svg"
const ICON_COLOR: String = "res://addons/terrain_3d/icons/color_paint.svg"
const ICON_WETNESS: String = "res://addons/terrain_3d/icons/wetness.svg"
const ICON_AUTOSHADER: String = "res://addons/terrain_3d/icons/autoshader.svg"
const ICON_HOLES: String = "res://addons/terrain_3d/icons/holes.svg"
const ICON_NAVIGATION: String = "res://addons/terrain_3d/icons/navigation.svg"
const ICON_INSTANCER: String = "res://addons/terrain_3d/icons/multimesh.svg"
const ICON_LANDFORM: String = "res://addons/terrain_3d/icons/landform.svg"

var add_tool_group: ButtonGroup = ButtonGroup.new()
var sub_tool_group: ButtonGroup = ButtonGroup.new()
var buttons: Dictionary[StringName, Button] = {}
var plugin: Terrain3DEditorPluginBase


func _init() -> void:
	set_custom_minimum_size(Vector2(20, 0))


func _ready() -> void:
	add_tool_group.pressed.connect(_on_tool_selected)
	sub_tool_group.pressed.connect(_on_tool_selected)

	add_tool_button(
		Terrain3DEditor.REGION,
		"Add Region (E)",
		Terrain3DEditor.ADD,
		ICON_REGION_ADD,
		"Remove Region",
		Terrain3DEditor.SUBTRACT,
		ICON_REGION_REMOVE
	)

	add_child(HSeparator.new())

	add_tool_button(
		Terrain3DEditor.SCULPT,
		"Raise (R)",
		Terrain3DEditor.ADD,
		ICON_HEIGHT_ADD,
		"Lower (R)",
		Terrain3DEditor.SUBTRACT,
		ICON_HEIGHT_SUB
	)

	add_tool_button(
		Terrain3DEditor.SCULPT,
		"Smooth (Shift)",
		Terrain3DEditor.AVERAGE,
		ICON_HEIGHT_SMOOTH,
		"",
		Terrain3DEditor.OP_MAX,
		""
	)

	add_tool_button(
		Terrain3DEditor.HEIGHT,
		"Height (H)",
		Terrain3DEditor.ADD,
		ICON_HEIGHT_FLAT,
		"Height (H)",
		Terrain3DEditor.SUBTRACT,
		ICON_HEIGHT_FLAT
	)

	add_tool_button(
		Terrain3DEditor.SCULPT,
		"Slope (S)",
		Terrain3DEditor.GRADIENT,
		ICON_HEIGHT_SLOPE,
		"",
		Terrain3DEditor.OP_MAX,
		""
	)

	add_tool_button(
		Terrain3DEditor.LANDFORM,
		"Landform Stroke (L)",
		Terrain3DEditor.REPLACE,
		ICON_LANDFORM,
		"",
		Terrain3DEditor.OP_MAX,
		""
	)

	add_child(HSeparator.new())

	add_tool_button(
		Terrain3DEditor.TEXTURE,
		"Paint Texture (B)",
		Terrain3DEditor.REPLACE,
		ICON_PAINT_TEXTURE,
		"",
		Terrain3DEditor.OP_MAX,
		""
	)

	add_tool_button(
		Terrain3DEditor.TEXTURE,
		"Spray Texture (V)",
		Terrain3DEditor.ADD,
		ICON_SPRAY_TEXTURE,
		"",
		Terrain3DEditor.OP_MAX,
		""
	)

	add_tool_button(
		Terrain3DEditor.AUTOSHADER,
		"Enable Material Rules (A)",
		Terrain3DEditor.ADD,
		ICON_AUTOSHADER,
		"Protect from Material Rules (A)",
		Terrain3DEditor.SUBTRACT,
		""
	)

	add_child(HSeparator.new())

	add_tool_button(
		Terrain3DEditor.COLOR,
		"Paint Color (C)",
		Terrain3DEditor.ADD,
		ICON_COLOR,
		"Remove Color (C)",
		Terrain3DEditor.SUBTRACT,
		""
	)

	add_tool_button(
		Terrain3DEditor.ROUGHNESS,
		"Paint Wetness (W)",
		Terrain3DEditor.ADD,
		ICON_WETNESS,
		"Remove Wetness (W)",
		Terrain3DEditor.SUBTRACT,
		""
	)

	add_child(HSeparator.new())

	add_tool_button(
		Terrain3DEditor.HOLES,
		"Add Holes (X)",
		Terrain3DEditor.ADD,
		ICON_HOLES,
		"Remove Holes (X)",
		Terrain3DEditor.SUBTRACT,
		""
	)

	add_tool_button(
		Terrain3DEditor.NAVIGATION,
		"Paint Navigable Area (N)",
		Terrain3DEditor.ADD,
		ICON_NAVIGATION,
		"Remove Navigable Area (N)",
		Terrain3DEditor.SUBTRACT,
		""
	)

	add_tool_button(
		Terrain3DEditor.INSTANCER,
		"Instance Meshes (I)",
		Terrain3DEditor.ADD,
		ICON_INSTANCER,
		"Remove Meshes (I)",
		Terrain3DEditor.SUBTRACT,
		""
	)

	# Select first button
	var buttons: Array[BaseButton] = add_tool_group.get_buttons()
	buttons[0].set_pressed(true)
	show_add_buttons(true)


func _get_tool_help(p_tool: int, p_operation: int) -> String:
	match p_tool:
		Terrain3DEditor.REGION:
			return "Create or remove one 128 m Terrain3D region at the cursor."
		Terrain3DEditor.SCULPT:
			match p_operation:
				Terrain3DEditor.AVERAGE:
					return "Smooth height differences inside the brush mask. Transparent brush pixels do not modify terrain."
				Terrain3DEditor.GRADIENT:
					return "Fit terrain to the slope defined by two picked reference points."
				Terrain3DEditor.SUBTRACT:
					return "Lower terrain using brush alpha, strength, and pen pressure."
				_:
					return "Raise terrain using brush alpha, strength, and pen pressure."
		Terrain3DEditor.HEIGHT:
			return "Blend terrain toward an explicit world elevation sampled or entered in Tool Settings."
		Terrain3DEditor.TEXTURE:
			return "Paint terrain material IDs and optional UV rotation/scale metadata. Spray blends the selected material into existing material pairs."
		Terrain3DEditor.AUTOSHADER:
			return "Enable or protect texels from Terrain3D material-rule/AutoShader ownership."
		Terrain3DEditor.COLOR:
			return "Paint authored terrain tint while preserving wetness stored in the color-map alpha channel."
		Terrain3DEditor.ROUGHNESS:
			return "Paint the terrain wetness/roughness channel without changing authored RGB tint."
		Terrain3DEditor.HOLES:
			return "Mark terrain control texels as holes or restore solid terrain."
		Terrain3DEditor.NAVIGATION:
			return "Paint or erase the Terrain3D navigation mask used by navigation baking."
		Terrain3DEditor.INSTANCER:
			return "Paint or remove the selected Terrain3D mesh asset using the instance variation settings."
		Terrain3DEditor.LANDFORM:
			return "Author semantic ridge, valley, cliff, basin, road, dune, shoreline, morphology, or occlusion strokes."
	return "Terrain3D editor painter tool."


func _format_tooltip(p_text: String, p_tool: int, p_operation: int) -> String:
	var help: String = _get_tool_help(p_tool, p_operation)
	return p_text + ("\n" + help if not help.is_empty() else "")


func add_tool_button(
		p_tool: int,
		p_add_text: String,
		p_add_op: int,
		p_add_icon: String,
		p_sub_text: String = "",
		p_sub_op: int = int(Terrain3DEditor.OP_MAX),
		p_sub_icon: String = "",
) -> void:
	# Additive button
	var button := Button.new()
	var name_str: String = p_add_text.get_slice('(', 0).to_pascal_case()
	button.set_name(name_str)
	button.set_meta("Tool", p_tool)
	button.set_meta("Operation", p_add_op)
	button.set_meta("ID", add_tool_group.get_buttons().size() + 1)
	button.set_tooltip_text(_format_tooltip(p_add_text, p_tool, p_add_op))
	button.set_button_icon(load(p_add_icon))
	button.set_flat(true)
	button.set_toggle_mode(true)
	button.set_h_size_flags(SIZE_SHRINK_END)
	button.set_button_group(add_tool_group)
	add_child(button, true)
	buttons[button.get_name()] = button

	# Subtractive button
	var button2: Button
	if not p_sub_text.is_empty():
		button2 = Button.new()
		name_str = p_sub_text.get_slice('(', 0).to_pascal_case()
		button2.set_name(name_str)
		button2.set_meta("Tool", p_tool)
		button2.set_meta("Operation", p_sub_op)
		button2.set_meta("ID", button.get_meta("ID"))
		button2.set_tooltip_text(_format_tooltip(p_sub_text, p_tool, p_sub_op))
		button2.set_button_icon(load(p_sub_icon if not p_sub_icon.is_empty() else p_add_icon))
		button2.set_flat(true)
		button2.set_toggle_mode(true)
		button2.set_h_size_flags(SIZE_SHRINK_END)
	else:
		button2 = button.duplicate()
	button2.set_button_group(sub_tool_group)
	add_child(button2, true)
	buttons[button2.get_name()] = button2


func get_button(p_name: String) -> Button:
	return buttons.get(p_name, null)


func show_add_buttons(p_enable: bool) -> void:
	for button: BaseButton in add_tool_group.get_buttons():
		button.visible = p_enable
	for button: BaseButton in sub_tool_group.get_buttons():
		button.visible = !p_enable


func _on_tool_selected(p_button: BaseButton) -> void:
	if plugin.debug:
		print("Terrain3DToolbar: _on_tool_selected: ", p_button)
	# Select same tool on negative bar
	var group: ButtonGroup = p_button.get_button_group()
	var change_group: ButtonGroup = add_tool_group if group == sub_tool_group else sub_tool_group
	var id: int = p_button.get_meta("ID", -2)
	for button: BaseButton in change_group.get_buttons():
		button.set_pressed_no_signal(button.get_meta("ID", -1) == id)
	if plugin.debug:
		print("Terrain3DToolbar: _on_tool_selected: emitting tool_changed, ",
			p_button.get_meta("Tool", Terrain3DEditor.TOOL_MAX), ", ",
			p_button.get_meta("Operation", Terrain3DEditor.OP_MAX))
	emit_signal("tool_changed", p_button.get_meta("Tool", Terrain3DEditor.TOOL_MAX), p_button.get_meta("Operation", Terrain3DEditor.OP_MAX))


func change_tool(p_name: String) -> void:
	var btn := get_button(p_name)
	if plugin.debug:
		print("Terrain3DToolbar: change_tool: ", p_name, ", pressed: ", btn and btn.button_pressed)
	if not is_instance_valid(btn):
		return

	# Programmatic state changes emit toggle state, not the user activation path
	# this toolbar uses to update Terrain3DEditor. Drive the group and handler
	# explicitly so hotkeys/asset clicks also resynchronize an already-selected tool.
	var group: ButtonGroup = btn.get_button_group()
	if is_instance_valid(group):
		for peer: BaseButton in group.get_buttons():
			peer.set_pressed_no_signal(peer == btn)
	else:
		btn.set_pressed_no_signal(true)
	_on_tool_selected(btn)
