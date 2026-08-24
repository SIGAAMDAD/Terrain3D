# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#
# This is an example of using a particle shader with Terrain3D.
# To use it, add `Terrain3DParticles.tscn` to your scene and connect terrain to your Terrain3D node.
# This is a limited example for you to build on and make it your own.

@tool
extends Node3D

@export_category("Info")
## The maximum distance that particles will be drawn, which equals: cell_width * grid_width * 0.5.
## If using fade out effects like pixel alpha, use this value as the limit.
@export_custom(PROPERTY_HINT_NONE, "suffix:m", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY) var calculated_distance: float = 1.0:
	set(value):
		calculated_distance = float(cell_width * grid_width) * 0.5


## Displays current total particle count based on Cell Width and Instance Spacing
@export_custom(PROPERTY_HINT_NONE, "suffix:particles", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY)  var particle_count: int = 1:
	set(value):
		particle_count = amount * grid_width * grid_width


@export_storage var rows: int = 1
@export_storage var amount: int = 1:
	set(value):
		amount = value
		particle_count = value
		last_pos = Vector3.ZERO
		for p: GPUParticles3D in particle_nodes:
			p.amount = amount


#region settings
@export_category("Settings")
## Auto set if attached as a child of a Terrain3D node
@export var terrain: Terrain3D:
	set(value):
		terrain = value
		_create_grid()


## Distance between instances
@export_range(0.125, 2.0, 0.015625) var instance_spacing: float = 0.5:
	set(value):
		instance_spacing = clampf(roundf(value * 64.0) * 0.015625, 0.125, 2.0)
		rows = maxi(int(cell_width / instance_spacing), 1)
		amount = rows * rows
		_set_offsets()


## Width of an individual cell of the grid
@export_range(8.0, 256.0, 1.0) var cell_width: float = 32.0:
	set(value):
		cell_width = clampf(value, 8.0, 256.0)
		rows = maxi(int(cell_width / instance_spacing), 1)
		amount = rows * rows
		calculated_distance = 1.0
		# Have to update aabb
		if terrain and terrain.data:
			var height_range: Vector2 = terrain.data.get_height_range()
			var height: float = height_range[0] - height_range[1]
			var aabb: AABB = AABB()
			aabb.size = Vector3(cell_width, height, cell_width)
			aabb.position = aabb.size * -0.5
			aabb.position.y = height_range[1]
			for p: GPUParticles3D in particle_nodes:
				p.custom_aabb = aabb
		_set_offsets()


## Grid width. Must be odd.
## Higher values cull slightly better, draw further out.
@export_range(1, 15, 2) var grid_width: int = 9:
	set(value):
		grid_width = value
		particle_count = 1
		calculated_distance = 1.0
		_create_grid()


@export_range(1, 256, 1) var process_fixed_fps: int = 30:
	set(value):
		process_fixed_fps = maxi(value, 1)
		for p: GPUParticles3D in particle_nodes:
			p.fixed_fps = process_fixed_fps
			p.preprocess = 1.0 / float(process_fixed_fps)

@export var shadow_mode: GeometryInstance3D.ShadowCastingSetting = (
		GeometryInstance3D.ShadowCastingSetting.SHADOW_CASTING_SETTING_ON):
	set(value):
		shadow_mode = value
		for p: GPUParticles3D in particle_nodes:
			p.cast_shadow = value

## Access to process material parameters
@export var process_material: ShaderMaterial

## The mesh that each particle will render
@export var mesh: Mesh


## Override material for the particle mesh
@export_custom(
	PROPERTY_HINT_RESOURCE_TYPE,
	"BaseMaterial3D,ShaderMaterial") var mesh_material_override: Material:
	set(value):
		mesh_material_override = value
		for p: GPUParticles3D in particle_nodes:
			p.material_override = mesh_material_override

#endregion


var offsets: Array[Vector3]
var last_pos: Vector3 = Vector3.ZERO
var particle_nodes: Array[GPUParticles3D]


func _ready() -> void:
	if not terrain:
		var parent: Node = get_parent()
		if parent is Terrain3D:
			terrain = parent
	_create_grid()


func _notification(what: int) -> void:
	if what == NOTIFICATION_PREDELETE:
		_destroy_grid()


func _physics_process(delta: float) -> void:
	if terrain:
		var camera: Camera3D = terrain.get_camera()
		if camera:
			if last_pos.distance_squared_to(camera.global_position) > 1.0:
				var pos: Vector3 = camera.global_position.snapped(Vector3.ONE)
				_position_grid(pos)
				process_material.set_shader_parameter("camera_position", pos)
				last_pos = camera.global_position
		_update_process_parameters()
	else:
		set_physics_process(false)


func _create_grid() -> void:
	_destroy_grid()
	if not terrain:
		return
	set_physics_process(true)
	_set_offsets()
	var hr: Vector2 = terrain.data.get_height_range()
	var height: float = hr.x - hr.y
	var aabb: AABB = AABB()
	aabb.size = Vector3(cell_width, height, cell_width)
	aabb.position = aabb.size * -0.5
	aabb.position.y = hr.y
	var half_grid: int = grid_width / 2
	# Iterating the array like this allows identifying grid position, in case setting
	# different mesh or materials is desired for LODs etc.
	for x: int in range(-half_grid, half_grid + 1):
		for z: int in range(-half_grid, half_grid + 1):
			#var ring: int = maxi(maxi(absi(x), absi(z)), 0)
			var particle_node := GPUParticles3D.new()
			particle_node.lifetime = 600.0
			particle_node.amount = amount
			particle_node.explosiveness = 1.0
			particle_node.amount_ratio = 1.0
			particle_node.process_material = process_material
			particle_node.draw_pass_1 = mesh
			particle_node.speed_scale = 1.0
			particle_node.custom_aabb = aabb
			particle_node.cast_shadow = shadow_mode
			particle_node.fixed_fps = process_fixed_fps
			# This prevent minor grid alignment errors when the camera is moving very fast
			particle_node.preprocess = 1.0 / float(process_fixed_fps)
			if mesh_material_override:
				particle_node.material_override = mesh_material_override
			particle_node.use_fixed_seed = true
			if (x > -half_grid and z > -half_grid): # Use the same seed across all nodes
				particle_node.seed = particle_nodes[0].seed
			self.add_child(particle_node)
			particle_node.emitting = true
			particle_nodes.push_back(particle_node)
	last_pos = Vector3.ZERO


func _set_offsets() -> void:
	var half_grid: int = grid_width / 2
	offsets.clear()
	for x: int in range(-half_grid, half_grid + 1):
		for z: int in range(-half_grid, half_grid + 1):
			var offset := Vector3(
				float(x * rows) * instance_spacing,
				0.0,
				float(z * rows) * instance_spacing
			)
			offsets.append(offset)


func _destroy_grid() -> void:
	for node: GPUParticles3D in particle_nodes:
		if is_instance_valid(node):
			node.queue_free()
	particle_nodes.clear()


func _position_grid(pos: Vector3) -> void:
	for i: int in particle_nodes.size():
		var node := particle_nodes[i] as GPUParticles3D
		var snap: Vector3 = Vector3(pos.x, 0, pos.z).snapped(Vector3.ONE) + offsets[i]
		node.global_position = (snap / instance_spacing).round() * instance_spacing
		node.reset_physics_interpolation()
		node.restart(true) # keep the same seed.


func _update_process_parameters() -> void:
	if process_material:
		if terrain:
			process_material.set_shader_parameter("_background_mode", terrain.material.world_background)
			if terrain.material.world_background > 0:
				process_material.set_shader_parameter("ground_level", terrain.material.get_shader_param("ground_level"))
				process_material.set_shader_parameter("region_blend", terrain.material.get_shader_param("region_blend"))
			process_material.set_shader_parameter("_vertex_spacing", terrain.vertex_spacing)
			process_material.set_shader_parameter("_vertex_density", 1.0 / terrain.vertex_spacing)
			process_material.set_shader_parameter("_region_size", terrain.region_size)
			process_material.set_shader_parameter("_region_texel_size", 1.0 / terrain.region_size)
			process_material.set_shader_parameter("_region_map_size", 32)
			process_material.set_shader_parameter("_region_map", terrain.data.get_region_map())
			process_material.set_shader_parameter("_region_locations", terrain.data.get_region_locations())
			process_material.set_shader_parameter("_height_maps", terrain.data.get_height_maps_rid())
			process_material.set_shader_parameter("_control_maps", terrain.data.get_control_maps_rid())
			process_material.set_shader_parameter("_color_maps", terrain.data.get_color_maps_rid())
			process_material.set_shader_parameter("_control_maps_implicit", terrain.data.are_control_maps_implicit())
			process_material.set_shader_parameter("_color_maps_implicit", terrain.data.are_color_maps_implicit())
			process_material.set_shader_parameter("instance_spacing", instance_spacing)
			process_material.set_shader_parameter("instance_rows", rows)
			process_material.set_shader_parameter("max_dist", calculated_distance)
