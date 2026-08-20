# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Typed lazy-load contract for the runtime profiler window.
@tool
class_name T3DRuntimeProfilerWindowBase
extends Window


func set_terrain(_p_terrain: T3DTerrain) -> void:
	pass


func open_for_terrain(_p_terrain: T3DTerrain) -> void:
	pass


func refresh_now() -> void:
	pass
