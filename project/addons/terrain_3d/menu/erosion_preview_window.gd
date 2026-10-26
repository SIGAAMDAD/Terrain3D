# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Disposable 3D scene used to inspect an erosion job without modifying the edited scene.
@tool
extends Window

const EditorUI = preload("res://addons/terrain_3d/src/editor_ui_util.gd")
const EditorUtil = preload("res://addons/terrain_3d/src/editor_util.gd")
const DETAIL_VERTEX_BUDGETS: Array[int] = [75000, 150000, 300000]

var _job: Terrain3DErosionJob
class PreviewSample extends RefCounted:
	var location: Vector2i = Vector2i.ZERO
	var current: Image
	var initial: Image


var _terrain: Terrain3D
var _status_label: Label
var _stage_label: Label
var _step_label: Label
var _progress_bar: ProgressBar
var _pause_button: Button
var _step_button: Button
var _heatmap_toggle: CheckButton
var _detail_picker: OptionButton
var _viewport_container: SubViewportContainer
var _viewport: SubViewport
var _world_root: Node3D
var _mesh_instance: MeshInstance3D
var _camera: Camera3D
var _material: StandardMaterial3D
var _last_revision: int = -1
var _last_step: int = -1
var _last_locations_hash: int = 0
var _last_vertex_count: int = 0
var _camera_framed: bool = false
var _target: Vector3 = Vector3.ZERO
var _distance: float = 100.0
var _yaw: float = -0.7
var _pitch: float = 0.62
var _orbiting: bool = false
var _built: bool = false


func _ready() -> void:
	if _built:
		return
	_built = true
	title = "Erosion Simulation Preview — Temporary Scene"
	initial_position = Window.WINDOW_INITIAL_POSITION_CENTER_MAIN_WINDOW_SCREEN
	transient = true
	close_requested.connect(hide)
	_build_interface()
	EditorUI.configure_window(
		self,
		Vector2i(int(EditorUI.scaled(1120.0)), int(EditorUI.scaled(760.0))),
		Vector2i(int(EditorUI.scaled(760.0)), int(EditorUI.scaled(520.0)))
	)


func configure(p_terrain: Terrain3D, p_job: Terrain3DErosionJob) -> void:
	_terrain = p_terrain
	_job = p_job
	_last_revision = -1
	_last_step = -1
	_last_locations_hash = 0
	_camera_framed = false
	_update_controls()


func clear_job() -> void:
	_job = null
	_terrain = null
	_last_revision = -1
	_last_step = -1
	_clear_mesh()
	_update_controls()


func open_preview() -> void:
	if not _built:
		open_preview.call_deferred()
		return
	var popup_size: Vector2i = EditorUI.popup_size_for_screen(
		self,
		Vector2i(int(EditorUI.scaled(1120.0)), int(EditorUI.scaled(760.0))),
		Vector2i(int(EditorUI.scaled(760.0)), int(EditorUI.scaled(520.0)))
	)
	popup_centered(popup_size)
	refresh_from_job(true)


func refresh_from_job(p_force: bool = false) -> void:
	if not _built or not is_instance_valid(_job):
		_update_controls()
		return
	var state: int = _job.get_state()
	var revision: int = int(_job.get_preview_revision())
	var completed: bool = (
		state == Terrain3DErosionJob.STATE_COMPLETED
		or state == Terrain3DErosionJob.STATE_APPLIED
	)
	if not p_force and revision == _last_revision and not completed:
		_update_controls()
		return
	if not completed and revision <= 0:
		_status_label.text = "Preparing the simulation snapshot..."
		_update_controls()
		return

	var locations: Array[Vector2i] = []
	for value: Vector2i in _job.get_region_locations():
		locations.push_back(value)
	if locations.is_empty():
		_status_label.text = "The erosion job contains no terrain regions."
		_clear_mesh()
		_update_controls()
		return

	var location_hash: int = hash(locations)
	var locations_changed: bool = location_hash != _last_locations_hash
	var vertex_budget: int = DETAIL_VERTEX_BUDGETS[_detail_picker.selected]
	var simulation_side: int = maxi(2, int(_job.get_simulation_region_size()))
	var side: int = clampi(
		int(floorf(sqrt(float(vertex_budget) / float(maxi(1, locations.size()))))),
		2,
		simulation_side
	)
	if side < 2:
		_status_label.text = "Preview detail is too low for the current region count."
		return

	var samples: Array[PreviewSample] = []
	var minimum_height: float = INF
	var maximum_height: float = -INF
	var maximum_absolute_delta: float = 0.000001
	for location: Vector2i in locations:
		var current: Image = _job.get_preview_region_height_map(location, false)
		var initial: Image = _job.get_initial_region_height_map(location, false)
		if not current or current.is_empty() or not initial or initial.is_empty():
			continue
		if current.get_width() != side or current.get_height() != side:
			current = current.duplicate()
			current.resize(side, side, Image.INTERPOLATE_BILINEAR)
		if initial.get_width() != side or initial.get_height() != side:
			initial = initial.duplicate()
			initial.resize(side, side, Image.INTERPOLATE_BILINEAR)
		for y: int in side:
			for x: int in side:
				var current_height: float = current.get_pixel(x, y).r
				var initial_height: float = initial.get_pixel(x, y).r
				minimum_height = minf(minimum_height, current_height)
				maximum_height = maxf(maximum_height, current_height)
				maximum_absolute_delta = maxf(
					maximum_absolute_delta,
					absf(current_height - initial_height)
				)
		var sample := PreviewSample.new()
		sample.location = location
		sample.current = current
		sample.initial = initial
		samples.push_back(sample)

	if samples.is_empty():
		_status_label.text = "No published height snapshot is available yet."
		_update_controls()
		return

	_build_combined_mesh(samples, side, minimum_height, maximum_height, maximum_absolute_delta)
	_last_revision = revision
	_last_step = int(_job.get_preview_step())
	_last_locations_hash = location_hash
	if not _camera_framed or locations_changed:
		_frame_camera()
	_update_controls()


func _build_interface() -> void:
	var margin: MarginContainer = MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 8)
	margin.add_theme_constant_override("margin_top", 8)
	margin.add_theme_constant_override("margin_right", 8)
	margin.add_theme_constant_override("margin_bottom", 8)
	add_child(margin)

	var split: HSplitContainer = HSplitContainer.new()
	split.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	split.size_flags_vertical = Control.SIZE_EXPAND_FILL
	split.split_offset = int(EditorUI.scaled(292.0))
	margin.add_child(split)

	var sidebar_panel: PanelContainer = PanelContainer.new()
	sidebar_panel.custom_minimum_size.x = EditorUI.scaled(270.0)
	sidebar_panel.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	sidebar_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	split.add_child(sidebar_panel)

	var sidebar_margin: MarginContainer = MarginContainer.new()
	sidebar_margin.add_theme_constant_override("margin_left", 10)
	sidebar_margin.add_theme_constant_override("margin_top", 10)
	sidebar_margin.add_theme_constant_override("margin_right", 10)
	sidebar_margin.add_theme_constant_override("margin_bottom", 10)
	sidebar_panel.add_child(sidebar_margin)

	var sidebar: VBoxContainer = VBoxContainer.new()
	sidebar.add_theme_constant_override("separation", 8)
	sidebar.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	sidebar.size_flags_vertical = Control.SIZE_EXPAND_FILL
	sidebar_margin.add_child(sidebar)

	var title_label: Label = Label.new()
	title_label.text = "Simulation Playback"
	title_label.add_theme_font_size_override("font_size", int(EditorUI.scaled(16.0)))
	sidebar.add_child(title_label)

	_stage_label = Label.new()
	_stage_label.text = "No active simulation"
	_stage_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	_stage_label.tooltip_text = "Current native erosion stage."
	sidebar.add_child(_stage_label)

	_step_label = Label.new()
	_step_label.text = "Step 0 / 0"
	_step_label.tooltip_text = "The latest completed step published by the worker."
	sidebar.add_child(_step_label)

	_progress_bar = ProgressBar.new()
	_progress_bar.min_value = 0.0
	_progress_bar.max_value = 100.0
	_progress_bar.value = 0.0
	_progress_bar.show_percentage = true
	EditorUI.configure_compact_control(_progress_bar)
	sidebar.add_child(_progress_bar)

	var transport_label: Label = Label.new()
	transport_label.text = "Playback"
	transport_label.add_theme_font_size_override("font_size", int(EditorUI.scaled(13.0)))
	sidebar.add_child(transport_label)

	var transport: HBoxContainer = HBoxContainer.new()
	transport.add_theme_constant_override("separation", 6)
	sidebar.add_child(transport)

	_pause_button = Button.new()
	_pause_button.text = "Pause"
	_pause_button.tooltip_text = "Pause at the next completed simulation step. Shortcut: Space."
	_pause_button.pressed.connect(_toggle_pause)
	_pause_button.custom_minimum_size.x = EditorUI.scaled(82.0)
	EditorUI.configure_action_button(_pause_button)
	transport.add_child(_pause_button)

	_step_button = Button.new()
	_step_button.text = "Advance One Step"
	_step_button.tooltip_text = "Pause and execute exactly one additional step. Shortcut: Period."
	_step_button.pressed.connect(_advance_one_step)
	_step_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	EditorUI.configure_action_button(_step_button)
	transport.add_child(_step_button)

	var scene_actions: HBoxContainer = HBoxContainer.new()
	scene_actions.add_theme_constant_override("separation", 6)
	sidebar.add_child(scene_actions)

	var refresh_button: Button = Button.new()
	refresh_button.text = "Refresh"
	refresh_button.tooltip_text = "Rebuild the temporary mesh from the newest published snapshot."
	refresh_button.pressed.connect(refresh_from_job.bind(true))
	refresh_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	EditorUI.configure_action_button(refresh_button)
	scene_actions.add_child(refresh_button)

	var frame_button: Button = Button.new()
	frame_button.text = "Frame"
	frame_button.tooltip_text = "Frame the full simulated terrain. Shortcut: F."
	frame_button.pressed.connect(_frame_camera)
	frame_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	EditorUI.configure_action_button(frame_button)
	scene_actions.add_child(frame_button)

	var display_label: Label = Label.new()
	display_label.text = "Display"
	display_label.add_theme_font_size_override("font_size", int(EditorUI.scaled(13.0)))
	sidebar.add_child(display_label)

	var display_grid: GridContainer = GridContainer.new()
	EditorUI.configure_form_grid(display_grid)
	display_grid.add_theme_constant_override("h_separation", int(EditorUI.scaled(8.0)))
	sidebar.add_child(display_grid)

	var mode_label: Label = Label.new()
	mode_label.text = "Change view"
	EditorUI.configure_form_label(mode_label, 90.0)
	display_grid.add_child(mode_label)

	_heatmap_toggle = CheckButton.new()
	_heatmap_toggle.text = "Show erosion/deposition"
	_heatmap_toggle.tooltip_text = "Blue indicates lowering; orange indicates deposition."
	_heatmap_toggle.button_pressed = true
	_heatmap_toggle.toggled.connect(_on_display_mode_changed)
	EditorUI.configure_field_control(_heatmap_toggle, 140.0)
	display_grid.add_child(_heatmap_toggle)

	var detail_label: Label = Label.new()
	detail_label.text = "Mesh detail"
	EditorUI.configure_form_label(detail_label, 90.0)
	display_grid.add_child(detail_label)

	_detail_picker = OptionButton.new()
	_detail_picker.tooltip_text = "Limits temporary preview vertices across the complete terrain."
	_detail_picker.add_item("Low — 75k")
	_detail_picker.add_item("Balanced — 150k")
	_detail_picker.add_item("High — 300k")
	_detail_picker.select(1)
	_detail_picker.item_selected.connect(_on_detail_changed)
	EditorUI.configure_field_control(_detail_picker, 140.0)
	display_grid.add_child(_detail_picker)

	var spacer: Control = Control.new()
	spacer.size_flags_vertical = Control.SIZE_EXPAND_FILL
	sidebar.add_child(spacer)

	_status_label = Label.new()
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status_label.text = "Run an erosion simulation with live preview enabled."
	_status_label.tooltip_text = "Snapshot state and temporary mesh statistics."
	sidebar.add_child(_status_label)

	var input_hint: Label = Label.new()
	input_hint.text = "Orbit: left-drag  •  Zoom: wheel  •  Frame: F"
	input_hint.modulate = Color(0.75, 0.75, 0.75)
	input_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	sidebar.add_child(input_hint)

	var viewport_panel: PanelContainer = PanelContainer.new()
	viewport_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	viewport_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	split.add_child(viewport_panel)

	var viewport_margin: MarginContainer = MarginContainer.new()
	viewport_margin.add_theme_constant_override("margin_left", 4)
	viewport_margin.add_theme_constant_override("margin_top", 4)
	viewport_margin.add_theme_constant_override("margin_right", 4)
	viewport_margin.add_theme_constant_override("margin_bottom", 4)
	viewport_panel.add_child(viewport_margin)

	_viewport_container = SubViewportContainer.new()
	_viewport_container.stretch = true
	_viewport_container.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_viewport_container.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_viewport_container.custom_minimum_size = Vector2(420.0, 360.0)
	_viewport_container.gui_input.connect(_on_viewport_gui_input)
	viewport_margin.add_child(_viewport_container)

	_viewport = SubViewport.new()
	_viewport.own_world_3d = true
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_viewport.msaa_3d = Viewport.MSAA_4X
	_viewport_container.add_child(_viewport)

	_world_root = Node3D.new()
	_world_root.name = "TemporaryErosionPreview"
	_viewport.add_child(_world_root)

	var environment_node: WorldEnvironment = WorldEnvironment.new()
	var environment: Environment = Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.055, 0.06, 0.07)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.62, 0.66, 0.72)
	environment.ambient_light_energy = 0.55
	environment_node.environment = environment
	_world_root.add_child(environment_node)

	var key_light: DirectionalLight3D = DirectionalLight3D.new()
	key_light.rotation_degrees = Vector3(-48.0, -35.0, 0.0)
	key_light.light_energy = 1.35
	key_light.shadow_enabled = true
	_world_root.add_child(key_light)

	var fill_light: DirectionalLight3D = DirectionalLight3D.new()
	fill_light.rotation_degrees = Vector3(-25.0, 145.0, 0.0)
	fill_light.light_energy = 0.32
	fill_light.shadow_enabled = false
	_world_root.add_child(fill_light)

	_mesh_instance = MeshInstance3D.new()
	_mesh_instance.name = "SimulatedTerrain"
	_world_root.add_child(_mesh_instance)

	_material = StandardMaterial3D.new()
	_material.vertex_color_use_as_albedo = true
	_material.roughness = 0.92
	_material.metallic = 0.0
	_material.cull_mode = BaseMaterial3D.CULL_BACK
	_mesh_instance.material_override = _material

	_camera = Camera3D.new()
	_camera.current = true
	_camera.fov = 52.0
	_world_root.add_child(_camera)
	_update_camera()


func _build_combined_mesh(
	p_samples: Array[PreviewSample],
	p_side: int,
	p_minimum_height: float,
	p_maximum_height: float,
	p_maximum_absolute_delta: float
) -> void:
	var region_count: int = p_samples.size()
	var vertices_per_region: int = p_side * p_side
	var indices_per_region: int = (p_side - 1) * (p_side - 1) * 6
	var vertices: PackedVector3Array = PackedVector3Array()
	var normals: PackedVector3Array = PackedVector3Array()
	var colors: PackedColorArray = PackedColorArray()
	var uvs: PackedVector2Array = PackedVector2Array()
	var indices: PackedInt32Array = PackedInt32Array()
	vertices.resize(region_count * vertices_per_region)
	normals.resize(region_count * vertices_per_region)
	colors.resize(region_count * vertices_per_region)
	uvs.resize(region_count * vertices_per_region)
	indices.resize(region_count * indices_per_region)

	var source_region_size: float = (
		float(_terrain.get_region_size()) if is_instance_valid(_terrain) else float(_job.get_simulation_region_size())
	)
	var vertex_spacing: float = (
		_terrain.get_vertex_spacing() if is_instance_valid(_terrain) else _job.get_preview_cell_size()
	)
	var region_world_size: float = source_region_size * vertex_spacing
	var sample_spacing: float = region_world_size / float(maxi(1, p_side - 1))
	var height_range: float = maxf(0.000001, p_maximum_height - p_minimum_height)
	var vertex_offset: int = 0
	var index_offset: int = 0
	var minimum_world: Vector3 = Vector3(INF, p_minimum_height, INF)
	var maximum_world: Vector3 = Vector3(-INF, p_maximum_height, -INF)

	for sample: PreviewSample in p_samples:
		var location: Vector2i = sample.location
		var current: Image = sample.current
		var initial: Image = sample.initial
		var region_origin: Vector3 = Vector3(
			float(location.x) * region_world_size,
			0.0,
			float(location.y) * region_world_size
		)
		minimum_world.x = minf(minimum_world.x, region_origin.x)
		minimum_world.z = minf(minimum_world.z, region_origin.z)
		maximum_world.x = maxf(maximum_world.x, region_origin.x + region_world_size)
		maximum_world.z = maxf(maximum_world.z, region_origin.z + region_world_size)

		for y: int in p_side:
			for x: int in p_side:
				var local_index: int = y * p_side + x
				var target_index: int = vertex_offset + local_index
				var height: float = current.get_pixel(x, y).r
				var left_height: float = current.get_pixel(maxi(0, x - 1), y).r
				var right_height: float = current.get_pixel(mini(p_side - 1, x + 1), y).r
				var top_height: float = current.get_pixel(x, maxi(0, y - 1)).r
				var bottom_height: float = current.get_pixel(x, mini(p_side - 1, y + 1)).r
				vertices[target_index] = region_origin + Vector3(
					float(x) * sample_spacing,
					height,
					float(y) * sample_spacing
				)
				normals[target_index] = Vector3(
					left_height - right_height,
					2.0 * sample_spacing,
					top_height - bottom_height
				).normalized()
				uvs[target_index] = Vector2(
					float(x) / float(maxi(1, p_side - 1)),
					float(y) / float(maxi(1, p_side - 1))
				)
				if _heatmap_toggle.button_pressed:
					var delta: float = height - initial.get_pixel(x, y).r
					colors[target_index] = _delta_color(delta / p_maximum_absolute_delta)
				else:
					colors[target_index] = _terrain_color((height - p_minimum_height) / height_range)

		for y: int in p_side - 1:
			for x: int in p_side - 1:
				var a: int = vertex_offset + y * p_side + x
				var b: int = a + 1
				var c: int = a + p_side
				var d: int = c + 1
				indices[index_offset] = a
				indices[index_offset + 1] = c
				indices[index_offset + 2] = b
				indices[index_offset + 3] = b
				indices[index_offset + 4] = c
				indices[index_offset + 5] = d
				index_offset += 6
		vertex_offset += vertices_per_region

	var arrays: Array[Variant] = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_COLOR] = colors
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh: ArrayMesh = ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	_mesh_instance.mesh = mesh
	_last_vertex_count = vertices.size()

	_target = Vector3(
		(minimum_world.x + maximum_world.x) * 0.5,
		(p_minimum_height + p_maximum_height) * 0.5,
		(minimum_world.z + maximum_world.z) * 0.5
	)
	var horizontal_span: float = maxf(
		maximum_world.x - minimum_world.x,
		maximum_world.z - minimum_world.z
	)
	_distance = maxf(10.0, horizontal_span * 1.15 + height_range * 0.35)
	_camera_framed = true
	_update_camera()


func _delta_color(p_normalized: float) -> Color:
	var value: float = clampf(p_normalized, -1.0, 1.0)
	var neutral: Color = Color(0.34, 0.35, 0.35)
	if value < 0.0:
		return neutral.lerp(Color(0.04, 0.24, 0.95), -value)
	return neutral.lerp(Color(1.0, 0.34, 0.035), value)


func _terrain_color(p_normalized: float) -> Color:
	var value: float = clampf(p_normalized, 0.0, 1.0)
	return Color(0.18, 0.20, 0.16).lerp(Color(0.62, 0.58, 0.48), value)


func _toggle_pause() -> void:
	if not is_instance_valid(_job):
		return
	_job.set_preview_paused(not _job.is_preview_paused())
	_update_controls()


func _advance_one_step() -> void:
	if not is_instance_valid(_job):
		return
	_job.request_preview_step()
	_update_controls()


func _on_display_mode_changed(_enabled: bool) -> void:
	refresh_from_job(true)


func _on_detail_changed(_index: int) -> void:
	refresh_from_job(true)


func _frame_camera() -> void:
	if not is_instance_valid(_mesh_instance) or not _mesh_instance.mesh:
		return
	var bounds: AABB = _mesh_instance.get_aabb()
	_target = bounds.get_center()
	_distance = maxf(10.0, bounds.size.length() * 0.8)
	_camera_framed = true
	_update_camera()


func _unhandled_key_input(p_event: InputEvent) -> void:
	if not visible or not (p_event is InputEventKey):
		return
	var key_event: InputEventKey = p_event as InputEventKey
	if not key_event.pressed or key_event.echo:
		return
	match key_event.keycode:
		KEY_SPACE:
			_toggle_pause()
			get_viewport().set_input_as_handled()
		KEY_PERIOD:
			_advance_one_step()
			get_viewport().set_input_as_handled()
		KEY_F:
			_frame_camera()
			get_viewport().set_input_as_handled()


func _on_viewport_gui_input(p_event: InputEvent) -> void:
	if p_event is InputEventMouseButton:
		var mouse_button: InputEventMouseButton = p_event as InputEventMouseButton
		if mouse_button.button_index == MOUSE_BUTTON_LEFT:
			_orbiting = mouse_button.pressed
			_viewport_container.accept_event()
		elif mouse_button.pressed and mouse_button.button_index == MOUSE_BUTTON_WHEEL_UP:
			_distance = maxf(2.0, _distance * 0.88)
			_update_camera()
			_viewport_container.accept_event()
		elif mouse_button.pressed and mouse_button.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			_distance *= 1.14
			_update_camera()
			_viewport_container.accept_event()
	elif p_event is InputEventMouseMotion and _orbiting:
		var motion: InputEventMouseMotion = p_event as InputEventMouseMotion
		_yaw -= motion.relative.x * 0.008
		_pitch = clampf(_pitch - motion.relative.y * 0.008, 0.08, 1.48)
		_update_camera()
		_viewport_container.accept_event()


func _update_camera() -> void:
	if not is_instance_valid(_camera):
		return
	var direction: Vector3 = Vector3(
		cos(_pitch) * sin(_yaw),
		sin(_pitch),
		cos(_pitch) * cos(_yaw)
	)
	_camera.global_position = _target + direction * _distance
	_camera.look_at(_target, Vector3.UP)


func _clear_mesh() -> void:
	if is_instance_valid(_mesh_instance):
		_mesh_instance.mesh = null
	_last_vertex_count = 0
	_camera_framed = false


func _update_controls() -> void:
	if not _built or not is_instance_valid(_status_label):
		return
	var valid_job: bool = is_instance_valid(_job)
	var running: bool = valid_job and _job.get_state() == Terrain3DErosionJob.STATE_RUNNING
	var live_enabled: bool = running and _job.is_live_preview_enabled()
	_pause_button.disabled = not live_enabled
	_step_button.disabled = not live_enabled
	_detail_picker.disabled = not valid_job
	_heatmap_toggle.disabled = not valid_job
	if valid_job:
		_pause_button.text = "Resume" if _job.is_preview_paused() else "Pause"
		var state: int = _job.get_state()
		var completed: bool = (
			state == Terrain3DErosionJob.STATE_COMPLETED
			or state == Terrain3DErosionJob.STATE_APPLIED
		)
		var step: int = int(_job.get_completed_steps()) if completed else int(_job.get_preview_step())
		var total: int = maxi(1, int(_job.get_simulation_steps()))
		var progress: float = clampf(_job.get_progress(), 0.0, 1.0)
		_stage_label.text = _job.get_stage_name()
		_step_label.text = "Step %d / %d%s" % [
			step,
			total,
			" — paused at boundary" if _job.is_preview_waiting() else "",
		]
		_progress_bar.value = progress * 100.0
		if state == Terrain3DErosionJob.STATE_RUNNING and _job.get_preview_revision() <= 0:
			_status_label.text = "Preparing the first terrain snapshot. The authored terrain remains untouched."
		elif state == Terrain3DErosionJob.STATE_RUNNING and not _job.is_live_preview_enabled():
			_status_label.text = "Live snapshots are disabled. The final simulated terrain will remain viewable after completion."
		else:
			_status_label.text = "%s vertices in the disposable preview mesh." % EditorUtil.format_integer(_last_vertex_count)
	else:
		_pause_button.text = "Pause"
		_stage_label.text = "No active simulation"
		_step_label.text = "Step 0 / 0"
		_progress_bar.value = 0.0
		_status_label.text = "No erosion job is attached to this temporary preview scene."


