# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends Control

signal sample_selected(index: int)

var samples: Array[T3DRuntimeValueMap] = []
var selected_index: int = -1
var frame_threshold_usec: float = 2000.0


func _ready() -> void:
	custom_minimum_size = Vector2(640.0, 220.0)
	mouse_filter = Control.MOUSE_FILTER_STOP


func set_samples(p_samples: Array[T3DRuntimeValueMap]) -> void:
	samples = p_samples
	if selected_index >= samples.size():
		selected_index = samples.size() - 1
	queue_redraw()


func select_latest() -> void:
	selected_index = samples.size() - 1
	queue_redraw()


func _gui_input(p_event: InputEvent) -> void:
	if not (p_event is InputEventMouseButton):
		return
	var mouse_event: InputEventMouseButton = p_event
	if mouse_event.button_index == MOUSE_BUTTON_LEFT and mouse_event.pressed:
		if samples.is_empty() or size.x <= 1.0:
			return
		selected_index = clampi(int(roundf(mouse_event.position.x / size.x * float(samples.size() - 1))), 0, samples.size() - 1)
		sample_selected.emit(selected_index)
		queue_redraw()


func _draw() -> void:
	var background: Color = get_theme_color("dark_color_2", "Editor")
	var grid_color: Color = get_theme_color("dark_color_3", "Editor")
	var text_color: Color = get_theme_color("font_color", "Label")
	draw_rect(Rect2(Vector2.ZERO, size), background)
	if samples.is_empty():
		draw_string(get_theme_default_font(), Vector2(12.0, 24.0), "No profiler history yet", HORIZONTAL_ALIGNMENT_LEFT, -1.0, 14, text_color)
		return

	var maximum_usec: float = maxf(frame_threshold_usec, 1.0)
	for sample: T3DRuntimeValueMap in samples:
		maximum_usec = maxf(maximum_usec, sample.float_value("physics_total_usec"))
	maximum_usec *= 1.1

	for line_index: int in range(1, 5):
		var y: float = size.y * float(line_index) / 5.0
		draw_line(Vector2(0.0, y), Vector2(size.x, y), grid_color, 1.0)

	var threshold_y: float = size.y - frame_threshold_usec / maximum_usec * size.y
	draw_line(Vector2(0.0, threshold_y), Vector2(size.x, threshold_y), Color(0.95, 0.55, 0.2, 0.9), 1.0)
	draw_string(get_theme_default_font(), Vector2(8.0, maxf(14.0, threshold_y - 3.0)), "%.2f ms trigger" % (frame_threshold_usec / 1000.0), HORIZONTAL_ALIGNMENT_LEFT, -1.0, 12, text_color)

	_draw_series("physics_total_usec", maximum_usec, Color(0.95, 0.85, 0.35), 2.0)
	_draw_series("data_residency_usec", maximum_usec, Color(0.35, 0.75, 1.0), 1.5)
	_draw_series("clipmap_usec", maximum_usec, Color(0.55, 1.0, 0.55), 1.5)
	_draw_series("instancer_usec", maximum_usec, Color(1.0, 0.55, 0.8), 1.5)
	_draw_event_markers(maximum_usec)

	if selected_index >= 0 and selected_index < samples.size():
		var selected_x: float = _sample_x(selected_index)
		draw_line(Vector2(selected_x, 0.0), Vector2(selected_x, size.y), Color(1.0, 1.0, 1.0, 0.8), 1.0)


func _draw_series(p_key: String, p_maximum_usec: float, p_color: Color, p_width: float) -> void:
	if samples.size() < 2:
		return
	var points: PackedVector2Array = PackedVector2Array()
	points.resize(samples.size())
	for index: int in range(samples.size()):
		var sample: T3DRuntimeValueMap = samples[index]
		var value: float = sample.float_value(p_key)
		points[index] = Vector2(_sample_x(index), size.y - value / p_maximum_usec * size.y)
	draw_polyline(points, p_color, p_width, true)


func _draw_event_markers(p_maximum_usec: float) -> void:
	for index: int in range(samples.size()):
		var sample: T3DRuntimeValueMap = samples[index]
		var events: PackedStringArray = sample.string_array("events")
		var stalls: PackedStringArray = sample.string_array("stall_reasons")
		if events.is_empty() and stalls.is_empty():
			continue
		var value: float = sample.float_value("physics_total_usec")
		var point: Vector2 = Vector2(_sample_x(index), size.y - value / p_maximum_usec * size.y)
		var marker_color: Color = Color(1.0, 0.3, 0.3) if not stalls.is_empty() else Color(0.8, 0.55, 1.0)
		draw_circle(point, 2.5, marker_color)


func _sample_x(p_index: int) -> float:
	if samples.size() <= 1:
		return 0.0
	return float(p_index) / float(samples.size() - 1) * size.x
