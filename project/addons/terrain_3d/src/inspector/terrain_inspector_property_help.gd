# Terrain3D property help. This plugin is deliberately read-only: Godot retains
# every native property editor, setter, editor undo action and visibility rule.
@tool
extends EditorInspectorPlugin

const SettingMetadata = preload("res://addons/terrain_3d/src/setting_metadata.gd")
const MAX_RESULTS: int = 20


func _can_handle(p_object: Object) -> bool:
	return p_object is Terrain3D


func _parse_begin(p_object: Object) -> void:
	var native_class: String = "Terrain3D"
	var entries: Array[Dictionary] = []
	var group_name: String = "General"
	var subgroup_name: String = ""
	# Use the selected object's effective property list. Hidden compatibility
	# fields and dynamically unavailable options must not appear in help.
	for info: Dictionary in p_object.get_property_list():
		var usage: int = int(info.get("usage", 0))
		if usage & PROPERTY_USAGE_GROUP:
			group_name = str(info.get("name", "General"))
			subgroup_name = ""
			continue
		if usage & PROPERTY_USAGE_SUBGROUP:
			subgroup_name = str(info.get("name", ""))
			continue
		if not (usage & PROPERTY_USAGE_EDITOR):
			continue
		var property_name: String = str(info.get("name", ""))
		var record: Dictionary = SettingMetadata.get_record(native_class, StringName(property_name))
		if record.is_empty():
			continue
		var section: String = group_name
		if not subgroup_name.is_empty():
			section += " / " + subgroup_name
		entries.append({
			"property": property_name,
			"label": str(record.get("display_name", property_name.capitalize())),
			"description": str(record.get("tooltip", "")),
			"section": section,
			"read_only": bool(usage & PROPERTY_USAGE_READ_ONLY),
		})

	if entries.is_empty():
		return

	var foldable: FoldableContainer = FoldableContainer.new()
	foldable.title = "Property Help (%d settings)" % entries.size()
	foldable.tooltip_text = "Search native Terrain3D settings by name, purpose, or Inspector section. Does not edit settings."
	foldable.folded = true
	foldable.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var content: VBoxContainer = VBoxContainer.new()
	content.add_theme_constant_override("separation", 5)
	foldable.add_child(content)

	var search: LineEdit = LineEdit.new()
	search.placeholder_text = "Search a property, unit, or feature..."
	search.clear_button_enabled = true
	search.tooltip_text = "Filter the currently visible native properties; no property values are changed."
	content.add_child(search)

	var results: RichTextLabel = RichTextLabel.new()
	results.bbcode_enabled = false
	results.scroll_active = true
	results.custom_minimum_size.y = 188.0
	results.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	content.add_child(results)
	search.text_changed.connect(func(p_text: String) -> void: _update_results(results, entries, p_text))
	_update_results(results, entries, "")
	add_custom_control(foldable)


func _update_results(p_results: RichTextLabel, p_entries: Array[Dictionary], p_query: String) -> void:
	var query: String = p_query.strip_edges().to_lower()
	if query.is_empty():
		p_results.text = "Search for a setting to see what it changes, its units, and any relevant dependencies.\nExample searches: page budget, shadows, streaming, LOD, displacement."
		return
	var lines: PackedStringArray = PackedStringArray()
	var matches: int = 0
	for entry: Dictionary in p_entries:
		var title: String = str(entry.get("label", ""))
		var property_name: String = str(entry.get("property", ""))
		var section: String = str(entry.get("section", ""))
		var description: String = str(entry.get("description", ""))
		var searchable: String = "%s %s %s %s" % [title, property_name, section, description]
		if not searchable.to_lower().contains(query):
			continue
		matches += 1
		if matches <= MAX_RESULTS:
			var status: String = " [read-only]" if bool(entry.get("read_only", false)) else ""
			# Use plain text, not BBCode created from metadata strings.
			lines.append("%s (%s)%s\n%s\n%s" % [title, property_name, status, section, description])
	if matches == 0:
		p_results.text = "No matching visible properties."
	elif matches > MAX_RESULTS:
		p_results.text = "\n\n".join(lines) + "\n\nShowing first %d of %d matches. Refine your search." % [MAX_RESULTS, matches]
	else:
		p_results.text = "\n\n".join(lines)
