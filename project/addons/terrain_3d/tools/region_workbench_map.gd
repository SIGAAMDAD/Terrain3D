@tool
class_name T3DRegionWorkbenchMap
extends Control

signal selection_changed(locations)
signal center_requested(location)

const BASE_CELL_SIZE: float = 24.0
const MIN_ZOOM: float = 0.15
const MAX_ZOOM: float = 3.0

var _available: Dictionary[Vector2i, bool] = {}
var _selected: Dictionary[Vector2i, bool] = {}
var _minimum: Vector2i = Vector2i.ZERO
var _maximum: Vector2i = Vector2i.ZERO
var _center: Vector2i = Vector2i.ZERO
var _zoom: float = 1.0
var _pan: Vector2 = Vector2.ZERO
var _dragging: bool = false
var _drag_select_value: bool = true
var _last_drag_location: Vector2i = Vector2i(999999, 999999)


func _ready() -> void:
	custom_minimum_size = Vector2(340, 280)
	mouse_filter = Control.MOUSE_FILTER_STOP
	clip_contents = true
	tooltip_text = "Spatial region selector. Left click or drag toggles authored regions, right click moves the checkout center, and the mouse wheel zooms."


func set_regions(locations: Array[Vector2i]) -> void:
	_available.clear()
	if locations.is_empty():
		_minimum = Vector2i.ZERO
		_maximum = Vector2i.ZERO
		queue_redraw()
		return
	_minimum = locations[0]
	_maximum = locations[0]
	for location: Vector2i in locations:
		_available[location] = true
		_minimum.x = mini(_minimum.x, location.x)
		_minimum.y = mini(_minimum.y, location.y)
		_maximum.x = maxi(_maximum.x, location.x)
		_maximum.y = maxi(_maximum.y, location.y)
	_fit_to_bounds()
	queue_redraw()


func set_selection(locations: Array[Vector2i]) -> void:
	_selected.clear()
	for location: Vector2i in locations:
		if _available.has(location):
			_selected[location] = true
	queue_redraw()


func set_center(location: Vector2i) -> void:
	_center = location
	queue_redraw()


func _draw() -> void:
	var bg: Color = get_theme_color("dark_color_2", "Editor") if has_theme_color("dark_color_2", "Editor") else Color(0.08, 0.08, 0.08, 1.0)
	draw_rect(Rect2(Vector2.ZERO, size), bg, true)
	if _available.is_empty():
		return
	var authored: Color = Color(0.34, 0.37, 0.41, 1.0)
	var selected_color: Color = get_theme_color("accent_color", "Editor") if has_theme_color("accent_color", "Editor") else Color(0.2, 0.55, 0.9, 1.0)
	var outline: Color = Color(0.08, 0.08, 0.08, 0.95)
	for location: Vector2i in _available.keys():
		var rect: Rect2 = _location_rect(location)
		if not rect.intersects(Rect2(Vector2.ZERO, size)):
			continue
		draw_rect(rect.grow(-1.0), selected_color if _selected.has(location) else authored, true)
		draw_rect(rect, outline, false, 1.0)
	var center_rect: Rect2 = _location_rect(_center)
	if center_rect.intersects(Rect2(Vector2.ZERO, size)):
		draw_rect(center_rect.grow(-2.0), Color(1.0, 0.75, 0.2, 1.0), false, 2.0)


func _gui_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var mouse := event as InputEventMouseButton
		if mouse.button_index == MOUSE_BUTTON_WHEEL_UP and mouse.pressed:
			_zoom_at(mouse.position, 1.15)
			accept_event()
			return
		if mouse.button_index == MOUSE_BUTTON_WHEEL_DOWN and mouse.pressed:
			_zoom_at(mouse.position, 1.0 / 1.15)
			accept_event()
			return
		if mouse.button_index == MOUSE_BUTTON_LEFT:
			_dragging = mouse.pressed
			if mouse.pressed:
				var location: Vector2i = _position_to_location(mouse.position)
				if _available.has(location):
					_drag_select_value = not _selected.has(location)
					_set_selected(location, _drag_select_value)
					_last_drag_location = location
			else:
				_last_drag_location = Vector2i(999999, 999999)
			accept_event()
			return
		if mouse.button_index == MOUSE_BUTTON_RIGHT and mouse.pressed:
			var location: Vector2i = _position_to_location(mouse.position)
			_center = location
			center_requested.emit(location)
			queue_redraw()
			accept_event()
			return
	if event is InputEventMouseMotion and _dragging:
		var motion := event as InputEventMouseMotion
		var location: Vector2i = _position_to_location(motion.position)
		if location != _last_drag_location and _available.has(location):
			_set_selected(location, _drag_select_value)
			_last_drag_location = location
		accept_event()


func _set_selected(location: Vector2i, enabled: bool) -> void:
	if enabled:
		_selected[location] = true
	else:
		_selected.erase(location)
	var values: Array[Vector2i] = []
	for value: Vector2i in _selected.keys():
		values.push_back(value)
	values.sort_custom(func(a: Vector2i, b: Vector2i) -> bool: return a.y < b.y or (a.y == b.y and a.x < b.x))
	selection_changed.emit(values)
	queue_redraw()


func _fit_to_bounds() -> void:
	var cells := Vector2(float(_maximum.x - _minimum.x + 1), float(_maximum.y - _minimum.y + 1))
	if cells.x <= 0.0 or cells.y <= 0.0:
		return
	var target := Vector2(maxf(size.x, 340.0), maxf(size.y, 280.0)) - Vector2(24.0, 24.0)
	_zoom = clampf(minf(target.x / (cells.x * BASE_CELL_SIZE), target.y / (cells.y * BASE_CELL_SIZE)), MIN_ZOOM, 1.35)
	_pan = Vector2.ZERO


func _location_rect(location: Vector2i) -> Rect2:
	var cell: float = BASE_CELL_SIZE * _zoom
	var grid_size := Vector2(float(_maximum.x - _minimum.x + 1), float(_maximum.y - _minimum.y + 1)) * cell
	var origin: Vector2 = (size - grid_size) * 0.5 + _pan
	var offset := Vector2(float(location.x - _minimum.x), float(location.y - _minimum.y)) * cell
	return Rect2(origin + offset, Vector2(cell, cell))


func _position_to_location(position: Vector2) -> Vector2i:
	var cell: float = BASE_CELL_SIZE * _zoom
	var grid_size := Vector2(float(_maximum.x - _minimum.x + 1), float(_maximum.y - _minimum.y + 1)) * cell
	var origin: Vector2 = (size - grid_size) * 0.5 + _pan
	var local: Vector2 = (position - origin) / cell
	return _minimum + Vector2i(floori(local.x), floori(local.y))


func _zoom_at(position: Vector2, factor: float) -> void:
	var before: Vector2i = _position_to_location(position)
	_zoom = clampf(_zoom * factor, MIN_ZOOM, MAX_ZOOM)
	var after_rect: Rect2 = _location_rect(before)
	_pan += position - after_rect.get_center()
	queue_redraw()
