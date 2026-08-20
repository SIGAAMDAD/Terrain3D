# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends Control

signal cell_selected(cell: T3DRuntimeValueMap)

var grid: T3DRuntimeValueMap = T3DRuntimeValueMap.new()
var selected_location: Vector2i = Vector2i(2147483647, 2147483647)


func _ready() -> void:
	custom_minimum_size = Vector2(520.0, 520.0)
	mouse_filter = Control.MOUSE_FILTER_STOP


func set_grid(p_grid: T3DRuntimeValueMap) -> void:
	grid = p_grid
	queue_redraw()


func _gui_input(p_event: InputEvent) -> void:
	if not (p_event is InputEventMouseButton):
		return
	var mouse_event: InputEventMouseButton = p_event
	if mouse_event.button_index != MOUSE_BUTTON_LEFT or not mouse_event.pressed:
		return
	var cells: Array[T3DRuntimeValueMap] = grid.map_array("cells")
	var radius: int = grid.int_value("radius")
	var diameter: int = radius * 2 + 1
	if cells.is_empty() or diameter <= 0:
		return
	var cell_size: float = minf(size.x, size.y) / float(diameter)
	var origin: Vector2 = (size - Vector2(cell_size * diameter, cell_size * diameter)) * 0.5
	var local: Vector2 = mouse_event.position - origin
	var x: int = int(floorf(local.x / cell_size))
	var y: int = int(floorf(local.y / cell_size))
	if x < 0 or y < 0 or x >= diameter or y >= diameter:
		return
	var index: int = y * diameter + x
	if index < 0 or index >= cells.size():
		return
	var cell: T3DRuntimeValueMap = cells[index]
	selected_location = cell.vector2i_value("location")
	cell_selected.emit(cell)
	queue_redraw()


func _draw() -> void:
	var background: Color = get_theme_color("dark_color_2", "Editor")
	draw_rect(Rect2(Vector2.ZERO, size), background)
	var cells: Array[T3DRuntimeValueMap] = grid.map_array("cells")
	var radius: int = grid.int_value("radius")
	var diameter: int = radius * 2 + 1
	if cells.is_empty() or diameter <= 0:
		return
	var cell_size: float = minf(size.x, size.y) / float(diameter)
	var origin: Vector2 = (size - Vector2(cell_size * diameter, cell_size * diameter)) * 0.5
	for index: int in range(cells.size()):
		var cell: T3DRuntimeValueMap = cells[index]
		var x: int = index % diameter
		var y: int = index / diameter
		var rect: Rect2 = Rect2(origin + Vector2(x, y) * cell_size, Vector2.ONE * cell_size)
		_draw_cell(rect, cell)

	var center_rect: Rect2 = Rect2(origin + Vector2(radius, radius) * cell_size, Vector2.ONE * cell_size)
	draw_rect(center_rect.grow(-2.0), Color(1.0, 1.0, 1.0, 0.9), false, 2.0)
	var motion: Vector2i = grid.vector2i_value("motion_direction")
	if motion != Vector2i():
		var center: Vector2 = center_rect.get_center()
		var end: Vector2 = center + Vector2(motion.x, motion.y) * cell_size * 1.5
		draw_line(center, end, Color(1.0, 0.9, 0.2), 3.0)


func _draw_cell(p_rect: Rect2, p_cell: T3DRuntimeValueMap) -> void:
	var authored: bool = p_cell.bool_value("active")
	var streaming_state: int = p_cell.int_value("streaming_state")
	var predicted: bool = p_cell.bool_value("predicted")
	var gpu_resident: bool = p_cell.bool_value("gpu_resident")
	var prepared: bool = p_cell.bool_value("prepared")
	var prepare_pending: bool = p_cell.bool_value("prepare_pending")
	var slot_state: int = p_cell.int_value("gpu_slot_state")
	var fill: Color = Color(0.10, 0.10, 0.10, 1.0)
	if authored:
		fill = Color(0.18, 0.18, 0.18, 1.0)
	if authored:
		match streaming_state:
			3: # ACTIVE
				fill = Color(0.52, 0.20, 0.18, 1.0)
			2: # HOT
				fill = Color(0.48, 0.34, 0.12, 1.0)
			1: # COLD
				fill = Color(0.18, 0.28, 0.48, 1.0)
			_: # UNLOADED authored region
				fill = Color(0.16, 0.16, 0.16, 1.0)
	draw_rect(p_rect.grow(-1.0), fill)
	draw_rect(p_rect.grow(-1.0), Color(0.0, 0.0, 0.0, 0.75), false, 1.0)
	if gpu_resident:
		draw_rect(p_rect.grow(-4.0), Color(0.15, 0.70, 0.30, 0.75), false, 2.0)
	if p_cell.bool_value("middle_height_resident"):
		draw_line(p_rect.position + Vector2(2.0, p_rect.size.y - 3.0), p_rect.position + Vector2(p_rect.size.x - 2.0, p_rect.size.y - 3.0), Color(0.85, 0.40, 1.0), 2.0)
	if p_cell.bool_value("cpu_resident"):
		draw_line(p_rect.position + Vector2(2.0, 3.0), p_rect.position + Vector2(p_rect.size.x - 2.0, 3.0), Color(0.25, 0.75, 1.0), 2.0)
	if p_cell.bool_value("instance_resident"):
		draw_circle(p_rect.position + Vector2(p_rect.size.x - 5.0, 5.0), 2.5, Color(1.0, 0.55, 0.15))
	if p_cell.bool_value("far_height_dirty") or p_cell.bool_value("far_color_dirty"):
		draw_circle(p_rect.position + Vector2(5.0, p_rect.size.y - 5.0), 2.5, Color(1.0, 0.95, 0.15))
	if prepare_pending:
		draw_circle(p_rect.get_center(), 3.0, Color(0.85, 0.35, 1.0))
	elif prepared:
		draw_circle(p_rect.get_center(), 3.0, Color(0.65, 0.30, 0.80))
	if slot_state == 4:
		draw_rect(p_rect.grow(-3.0), Color(0.70, 0.70, 0.70, 0.9), false, 1.0)
	if predicted:
		draw_rect(p_rect.grow(-2.0), Color(0.25, 0.95, 0.95, 0.95), false, 2.0)
	if p_cell.vector2i_value("location") == selected_location:
		draw_rect(p_rect.grow(-2.0), Color(1.0, 1.0, 1.0), false, 2.0)
