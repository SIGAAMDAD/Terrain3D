# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Typed lazy-load contract for the editor execution-mode menu.
@tool
class_name T3DExecutionModeMenuBase
extends MenuButton

var plugin: T3DEditorPluginBase


func set_terrain(_p_terrain: T3DTerrain) -> void:
	pass
