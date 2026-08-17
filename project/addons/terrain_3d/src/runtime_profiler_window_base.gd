# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Typed lazy-load contract for the runtime profiler window.
@tool
class_name Terrain3DRuntimeProfilerWindowBase
extends Window


func set_terrain(_p_terrain: Terrain3D) -> void:
	pass


func open_for_terrain(_p_terrain: Terrain3D) -> void:
	pass


func refresh_now() -> void:
	pass
