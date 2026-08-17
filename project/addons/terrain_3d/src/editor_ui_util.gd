# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Shared editor UI sizing and popup placement helpers for Terrain3D.
@tool
extends RefCounted

const BASE_CONTROL_HEIGHT: float = 34.0
const BASE_ICON_BUTTON_SIZE: float = 38.0
const BASE_TOOLBAR_WIDTH_COMPACT: float = 48.0
const BASE_TOOLBAR_WIDTH_EXPANDED: float = 188.0
const BASE_FORM_LABEL_WIDTH: float = 220.0
const BASE_FORM_FIELD_WIDTH: float = 240.0
const SCREEN_MARGIN: int = 18


static func editor_scale() -> float:
	return maxf(1.0, EditorInterface.get_editor_scale())


static func scaled(p_value: float) -> float:
	return p_value * editor_scale()


static func control_height() -> float:
	return scaled(BASE_CONTROL_HEIGHT)


static func icon_button_size() -> Vector2:
	var side: float = scaled(BASE_ICON_BUTTON_SIZE)
	return Vector2(side, side)


static func set_button_editor_icon(p_button: Button, p_icon_name: String) -> void:
	p_button.icon = EditorInterface.get_base_control().get_theme_icon(p_icon_name, "EditorIcons")


static func configure_icon_button(p_button: BaseButton, p_tooltip: String = "") -> void:
	p_button.custom_minimum_size = icon_button_size()
	p_button.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	p_button.focus_mode = Control.FOCUS_ALL
	p_button.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
	if not p_tooltip.is_empty():
		p_button.tooltip_text = p_tooltip


static func configure_compact_control(p_control: Control) -> void:
	p_control.custom_minimum_size.y = control_height()
	p_control.size_flags_vertical = Control.SIZE_SHRINK_CENTER


static func configure_action_button(p_button: BaseButton) -> void:
	configure_compact_control(p_button)
	p_button.focus_mode = Control.FOCUS_ALL
	p_button.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND


static func configure_field_control(p_control: Control, p_minimum_width: float = BASE_FORM_FIELD_WIDTH) -> void:
	p_control.custom_minimum_size.x = scaled(p_minimum_width)
	configure_compact_control(p_control)
	p_control.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	p_control.size_flags_stretch_ratio = 0.58


static func configure_form_label(p_label: Label, p_minimum_width: float = BASE_FORM_LABEL_WIDTH) -> void:
	p_label.custom_minimum_size.x = scaled(p_minimum_width)
	p_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	p_label.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	p_label.size_flags_stretch_ratio = 0.42
	p_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	p_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER


static func configure_form_grid(p_grid: GridContainer) -> void:
	p_grid.columns = 2
	p_grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	p_grid.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	p_grid.add_theme_constant_override("h_separation", int(scaled(18.0)))
	p_grid.add_theme_constant_override("v_separation", int(scaled(8.0)))


static func configure_scroll_container(p_scroll: ScrollContainer) -> void:
	p_scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	p_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	p_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	p_scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO


static func usable_screen_rect(p_context: Node) -> Rect2i:
	var screen: int = DisplayServer.window_get_current_screen()
	var host_window: Window
	if p_context is Window:
		host_window = p_context as Window
	elif p_context is Control:
		host_window = (p_context as Control).get_window()
	if is_instance_valid(host_window):
		screen = host_window.current_screen
	return DisplayServer.screen_get_usable_rect(screen)


static func available_popup_size(p_context: Node) -> Vector2i:
	var usable: Rect2i = usable_screen_rect(p_context)
	return Vector2i(
		maxi(1, usable.size.x - SCREEN_MARGIN * 2),
		maxi(1, usable.size.y - SCREEN_MARGIN * 2)
	)


static func minimum_size_for_screen(p_context: Node, p_minimum: Vector2i) -> Vector2i:
	var available: Vector2i = available_popup_size(p_context)
	return Vector2i(
		mini(p_minimum.x, available.x),
		mini(p_minimum.y, available.y)
	)


static func popup_size_for_screen(
	p_context: Node,
	p_preferred: Vector2i,
	p_minimum: Vector2i = Vector2i(420, 320)
) -> Vector2i:
	var maximum: Vector2i = available_popup_size(p_context)
	var minimum: Vector2i = minimum_size_for_screen(p_context, p_minimum)
	return Vector2i(
		clampi(p_preferred.x, minimum.x, maximum.x),
		clampi(p_preferred.y, minimum.y, maximum.y)
	)


static func configure_window(
	p_window: Window,
	p_preferred: Vector2i,
	p_minimum: Vector2i = Vector2i(520, 400)
) -> void:
	p_window.min_size = minimum_size_for_screen(p_window, p_minimum)
	p_window.size = popup_size_for_screen(p_window, p_preferred, p_minimum)
	p_window.move_to_center()


static func configure_file_dialog(
	p_dialog: Window,
	p_preferred: Vector2i = Vector2i(760, 560),
	p_minimum: Vector2i = Vector2i(520, 420)
) -> void:
	p_dialog.min_size = minimum_size_for_screen(p_dialog, p_minimum)
	p_dialog.size = popup_size_for_screen(p_dialog, p_preferred, p_minimum)


static func position_popup_near_control(
	p_popup: Popup,
	p_anchor: Control,
	p_prefer_above: bool = true
) -> void:
	var viewport: Viewport = p_anchor.get_viewport()
	var embedded: bool = is_instance_valid(viewport) and viewport.gui_embed_subwindows
	var bounds: Rect2i
	var anchor_rect: Rect2
	if embedded:
		var host_window: Window = p_anchor.get_window()
		bounds = Rect2i(Vector2i.ZERO, host_window.size)
		anchor_rect = p_anchor.get_global_rect()
	else:
		bounds = usable_screen_rect(p_anchor)
		anchor_rect = Rect2(p_anchor.get_screen_position(), p_anchor.size)

	var popup_size: Vector2i = p_popup.size
	var x: int = int(anchor_rect.position.x)
	var y_above: int = int(anchor_rect.position.y) - popup_size.y - 4
	var y_below: int = int(anchor_rect.end.y) + 4
	var y: int = y_above if p_prefer_above and y_above >= bounds.position.y else y_below
	if y + popup_size.y > bounds.end.y:
		y = y_above

	var minimum_x: int = bounds.position.x + SCREEN_MARGIN
	var minimum_y: int = bounds.position.y + SCREEN_MARGIN
	var maximum_x: int = maxi(minimum_x, bounds.end.x - popup_size.x - SCREEN_MARGIN)
	var maximum_y: int = maxi(minimum_y, bounds.end.y - popup_size.y - SCREEN_MARGIN)
	p_popup.position = Vector2i(
		clampi(x, minimum_x, maximum_x),
		clampi(y, minimum_y, maximum_y)
	)
