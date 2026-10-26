# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Shared titled panel shell used by Terrain3D editor tools.
@tool
extends RefCounted

var panel: PanelContainer
var body: VBoxContainer


func _init(p_title: String, p_title_font_size: int = 16) -> void:
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
	title.add_theme_font_size_override("font_size", p_title_font_size)
	body.add_child(title)
