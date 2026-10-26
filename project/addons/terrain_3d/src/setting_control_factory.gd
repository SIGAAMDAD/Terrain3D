# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Shared metadata-driven editor controls for Terrain3D custom setting surfaces.
@tool
extends RefCounted

const SettingMetadata = preload("res://addons/terrain_3d/src/setting_metadata.gd")


static func is_supported(p_descriptor: Dictionary) -> bool:
	var type: int = int(p_descriptor.get("variant_type", TYPE_NIL))
	return type == TYPE_BOOL or type == TYPE_INT or type == TYPE_FLOAT


static func supported_groups(p_class_name: String, p_group_prefixes: Array = []) -> Array[Dictionary]:
	var result: Array[Dictionary] = []
	var descriptors: Array[Dictionary] = SettingMetadata.descriptors_for_class(p_class_name, p_group_prefixes)
	for group: Dictionary in SettingMetadata.group_descriptors(descriptors):
		var supported: Array[Dictionary] = []
		for descriptor: Dictionary in group.get("settings", []):
			if is_supported(descriptor):
				supported.append(descriptor)
		if not supported.is_empty():
			result.append({"group_path": str(group.get("group_path", "General")), "settings": supported})
	return result


static func group_is_advanced(p_descriptors: Array[Dictionary]) -> bool:
	var advanced_count: int = 0
	for descriptor: Dictionary in p_descriptors:
		if str(descriptor.get("visibility", "basic")) == "advanced":
			advanced_count += 1
	return advanced_count > 0 and advanced_count * 2 >= p_descriptors.size()


static func create_control(p_descriptor: Dictionary, p_changed: Callable) -> Control:
	var type: int = int(p_descriptor.get("variant_type", TYPE_NIL))
	var hint: int = int(p_descriptor.get("hint", PROPERTY_HINT_NONE))
	var hint_string: String = str(p_descriptor.get("hint_string", ""))
	var tooltip: String = str(p_descriptor.get("tooltip", ""))
	if type == TYPE_BOOL:
		var check: CheckBox = CheckBox.new()
		check.tooltip_text = tooltip
		check.text = "Enabled"
		check.toggled.connect(func(p_value: bool) -> void: p_changed.call(p_value))
		return check
	if type == TYPE_INT and hint == PROPERTY_HINT_ENUM:
		var option: OptionButton = OptionButton.new()
		option.tooltip_text = tooltip
		_populate_enum(option, hint_string)
		option.item_selected.connect(func(p_index: int) -> void: p_changed.call(option.get_item_id(p_index)))
		return option
	if type == TYPE_INT or type == TYPE_FLOAT:
		var spin: SpinBox = SpinBox.new()
		spin.tooltip_text = tooltip
		_configure_spin(spin, type, hint, hint_string)
		spin.value_changed.connect(func(p_value: float) -> void:
			p_changed.call(int(p_value) if type == TYPE_INT else p_value)
		)
		return spin
	return null


static func sync_control(p_control: Control, p_descriptor: Dictionary, p_value: Variant) -> void:
	if p_control is CheckBox:
		(p_control as CheckBox).button_pressed = bool(p_value)
	elif p_control is OptionButton:
		var option: OptionButton = p_control as OptionButton
		var index: int = option.get_item_index(int(p_value))
		if index >= 0:
			option.select(index)
	elif p_control is SpinBox:
		(p_control as SpinBox).value = float(p_value)


static func mutation_label(p_descriptor: Dictionary) -> String:
	return SettingMetadata.mutation_mode_label(str(p_descriptor.get("mutation_mode", "serialized_configuration")))


static func tooltip_with_mode(p_descriptor: Dictionary) -> String:
	var tooltip: String = str(p_descriptor.get("tooltip", ""))
	var mode: String = mutation_label(p_descriptor)
	if tooltip.is_empty():
		return "Edit mode: %s" % mode
	return "%s\nEdit mode: %s" % [tooltip, mode]


static func _populate_enum(p_option: OptionButton, p_hint_string: String) -> void:
	var entries: PackedStringArray = p_hint_string.split(",", false)
	for index: int in range(entries.size()):
		var token: String = entries[index].strip_edges()
		var label: String = token
		var item_id: int = index
		var separator: int = token.rfind(":")
		if separator > 0:
			var candidate: String = token.substr(separator + 1).strip_edges()
			if candidate.is_valid_int():
				label = token.substr(0, separator).strip_edges()
				item_id = int(candidate)
		p_option.add_item(label, item_id)


static func _configure_spin(p_spin: SpinBox, p_type: int, p_hint: int, p_hint_string: String) -> void:
	p_spin.allow_greater = true
	p_spin.allow_lesser = true
	p_spin.min_value = -1000000000.0
	p_spin.max_value = 1000000000.0
	p_spin.step = 1.0 if p_type == TYPE_INT else 0.01
	if p_hint != PROPERTY_HINT_RANGE:
		return
	var parts: PackedStringArray = p_hint_string.split(",", false)
	if parts.size() >= 2:
		p_spin.min_value = float(parts[0].strip_edges())
		p_spin.max_value = float(parts[1].strip_edges())
		p_spin.allow_lesser = false
		p_spin.allow_greater = false
	if parts.size() >= 3:
		p_spin.step = float(parts[2].strip_edges())
	for index: int in range(3, parts.size()):
		var token: String = parts[index].strip_edges()
		if token == "or_less":
			p_spin.allow_lesser = true
		elif token == "or_greater":
			p_spin.allow_greater = true
		elif token.begins_with("suffix:"):
			p_spin.suffix = " " + token.trim_prefix("suffix:").strip_edges()
