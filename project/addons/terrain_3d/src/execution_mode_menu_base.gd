# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Typed lazy-load contract for the editor execution-mode menu.
@tool
class_name Terrain3DExecutionModeMenuBase
extends MenuButton

var plugin: Terrain3DEditorPluginBase


func set_terrain(_p_terrain: Terrain3D) -> void:
	pass
