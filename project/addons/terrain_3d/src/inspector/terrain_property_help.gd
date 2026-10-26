# Copyright © 2023-2026 Terrain3D Contributors.
# Searchable documentation for native properties without replacing Godot's editors.
@tool
extends RefCounted

const SettingMetadata = preload("res://addons/terrain_3d/src/setting_metadata.gd")
const MAX_RESULTS: int = 30


static func create_panel(p_object: Object) -> Control:
	var fold: FoldableContainer = FoldableContainer.new()
	fold.title = "Property Help"
	fold.folded = true
	fold.tooltip_text = "Search descriptions of Inspector properties. Native property controls remain unchanged."
	var body: VBoxContainer = VBoxContainer.new()
	body.add_theme_constant_override("separation", 5)
	fold.add_child(body)
	var introduction: Label = Label.new()
	introduction.text = "Search the properties of this resource or node. Select a result for details."
	introduction.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.add_child(introduction)
	var search: LineEdit = LineEdit.new()
	search.placeholder_text = "Find a setting (name, unit, or behavior)..."
	search.clear_button_enabled = true
	body.add_child(search)
	var results: ItemList = ItemList.new()
	results.custom_minimum_size.y = 128
	results.select_mode = ItemList.SELECT_SINGLE
	body.add_child(results)
	var explanation: Label = Label.new()
	explanation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	explanation.custom_minimum_size.y = 44
	body.add_child(explanation)
	var available: Array[String] = []
	for property_info: Dictionary in p_object.get_property_list():
		var property_name: String = str(property_info.get("name", ""))
		var usage: int = int(property_info.get("usage", 0))
		if property_name.is_empty() or not (usage & PROPERTY_USAGE_EDITOR):
			continue
		if not SettingMetadata.tooltip(p_object.get_class(), StringName(property_name)).is_empty():
			available.append(property_name)
	available.sort()
	search.text_changed.connect(func(p_filter: String) -> void:
		populate_results(results, explanation, p_object.get_class(), available, p_filter))
	results.item_selected.connect(func(p_index: int) -> void:
		var key: String = str(results.get_item_metadata(p_index))
		explanation.text = SettingMetadata.tooltip(p_object.get_class(), StringName(key)))
	populate_results(results, explanation, p_object.get_class(), available, "")
	return fold


static func populate_results(p_results: ItemList, p_explanation: Label, p_class_name: String,
		p_available: Array[String], p_filter: String) -> void:
	p_results.clear()
	var filter_text: String = p_filter.strip_edges().to_lower()
	var found: int = 0
	for property_name: String in p_available:
		var help: String = SettingMetadata.tooltip(p_class_name, StringName(property_name))
		if not filter_text.is_empty() and not property_name.to_lower().contains(filter_text) and not help.to_lower().contains(filter_text):
			continue
		p_results.add_item(property_name.replace("_", " ").capitalize())
		p_results.set_item_metadata(p_results.item_count - 1, property_name)
		found += 1
		if found >= MAX_RESULTS:
			break
	p_explanation.text = "Select a property to read its description." if found > 0 else "No matching documented properties."
