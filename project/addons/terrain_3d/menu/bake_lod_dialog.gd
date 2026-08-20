# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Bake LOD Dialog for Terrain3D
@tool
class_name T3DBakeLodDialog
extends ConfirmationDialog

var lod: int = 4
var description: String = "":
	set(p_value):
		description = p_value
		_sync_description_label()


func _ready() -> void:
	set_unparent_when_invisible(true)
	unresizable = true
	%LodBox.tooltip_text = "LOD level to bake. Higher values use a coarser terrain representation and reduce baked geometry/detail cost."
	%LodBox.value_changed.connect(_on_lod_box_value_changed)
	lod = get_selected_lod()
	_sync_description_label()


func _sync_description_label() -> void:
	var description_label := get_node_or_null("MarginContainer/VBoxContainer/DescriptionLabel") as Label
	if description_label:
		description_label.text = description


func get_selected_lod() -> int:
	if not is_node_ready():
		return clampi(lod, 0, 8)
	return clampi(roundi(%LodBox.value), 0, 8)


func _on_lod_box_value_changed(p_value: float) -> void:
	lod = clampi(roundi(p_value), 0, 8)
