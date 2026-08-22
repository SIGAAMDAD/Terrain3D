# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Typed lazy-load contract for the runtime profiler window.
@tool
@abstract
class_name Terrain3DRuntimeProfilerWindowBase
extends Window


@abstract func set_terrain(_p_terrain: Terrain3D) -> void

@abstract func open_for_terrain(_p_terrain: Terrain3D) -> void

@abstract func refresh_now() -> void
