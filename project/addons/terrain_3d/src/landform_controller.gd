# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Semantic landform stroke controller for Terrain3D.
extends Node

const ToolSettings = preload("res://addons/terrain_3d/src/tool_settings.gd")

const MIN_PATH_POINTS: int = 2
const LANDFORM_GENERATOR_VERSION: int = 3
const FACE_MODULE_SLOTS: int = 4
const CORNER_MODULE_SLOTS: int = 2
const END_MODULE_SLOTS: int = 2
const MODE_NAMES: PackedStringArray = [
	"Ridge", "Valley", "Scarp", "Basin", "Plateau", "Terrace", "Road", "Pass",
	"Talus", "Hydraulic Channel", "Thermal Relaxation", "Dune", "Shoreline",
	"Morphology", "Occlusion"
]

var plugin: T3DEditorPluginBase
var asset_dock: T3DAssetDock
var tool_settings: ToolSettings
var baker: T3DLandformBaker = T3DLandformBaker.new()
var path: PackedVector3Array = PackedVector3Array()
var drawing: bool = false
var preview_instance: MeshInstance3D
var preview_mesh: ImmediateMesh
var preview_material: StandardMaterial3D


func begin_stroke(p_position: Vector3) -> void:
	cancel_stroke()
	path = PackedVector3Array([p_position])
	drawing = true
	_ensure_preview()
	_update_preview()


func append_stroke(p_position: Vector3) -> void:
	if not drawing:
		return
	var minimum_spacing: float = maxf(
		plugin.terrain.get_vertex_spacing(),
		float(tool_settings.get_setting("landform_path_spacing"))
	)
	if path.is_empty() or path[path.size() - 1].distance_to(p_position) >= minimum_spacing:
		path.push_back(p_position)
		_update_preview()


func finish_stroke(p_position: Vector3) -> bool:
	if not drawing:
		return false
	append_stroke(p_position)
	drawing = false
	_destroy_preview()
	if path.size() < MIN_PATH_POINTS:
		path.clear()
		return false
	var applied: bool = _apply_path(path)
	path.clear()
	return applied


func cancel_stroke() -> void:
	drawing = false
	path.clear()
	_destroy_preview()


func _apply_path(p_path: PackedVector3Array) -> bool:
	if not plugin or not plugin.terrain or not tool_settings:
		return false
	var settings: T3DLandformSettings = _build_settings()
	var landform_id: int = _create_landform_id(plugin.terrain, p_path, settings)
	var result: T3DLandformNativeResult = T3DLandformNativeResult.from_apply_value(
		baker.apply(plugin.terrain, p_path, settings.to_dictionary()))
	if not result.ok:
		push_warning("Terrain3D landform: %s" % (result.error if not result.error.is_empty() else "Operation failed."))
		return false

	var placements: T3DLandformPlacements = result.placements
	if not placements.is_empty():
		if result.modules_ok:
			_add_module_placements(plugin.terrain, landform_id, placements)
		else:
			push_warning("Terrain3D modular cliff: %s" % (
				result.modules_error if not result.modules_error.is_empty() else "No modules were generated."))

	var generated_landform: Node3D
	var stores_landform_source: bool = settings.generate_mesh and settings.mode in [
		T3DLandformBaker.SCARP, T3DLandformBaker.OCCLUSION
	]
	if result.mesh or result.collision_mesh or not placements.is_empty() or stores_landform_source:
		generated_landform = _create_generated_landform(
			result.mesh,
			result.collision_mesh,
			settings,
			p_path,
			landform_id,
			result.module_count,
			placements
		)
		_add_generated_landform(plugin.terrain, generated_landform)

	var mode_name: String = _mode_name(settings.mode)
	var undo: EditorUndoRedoManager = plugin.get_undo_redo()
	undo.create_action("Terrain3D %s" % mode_name, UndoRedo.MERGE_DISABLE, plugin.terrain)
	undo.add_undo_method(baker, &"restore", plugin.terrain, result.before, result.edited_area)
	undo.add_do_method(baker, &"restore", plugin.terrain, result.after, result.edited_area)
	if not placements.is_empty():
		undo.add_undo_method(self, &"_remove_module_placements", plugin.terrain, landform_id)
		undo.add_do_method(self, &"_add_module_placements", plugin.terrain, landform_id, placements)
	if generated_landform:
		undo.add_undo_method(self, &"_remove_generated_landform", plugin.terrain, generated_landform)
		undo.add_do_method(self, &"_add_generated_landform", plugin.terrain, generated_landform)
		undo.add_do_reference(generated_landform)
	undo.commit_action(false)
	return true


func _build_settings() -> T3DLandformSettings:
	var settings := T3DLandformSettings.new()
	settings.mode = int(tool_settings.get_setting("landform_mode"))
	settings.morphology_mode = int(tool_settings.get_setting("landform_morphology"))
	settings.width = float(tool_settings.get_setting("landform_width"))
	settings.amplitude = float(tool_settings.get_setting("landform_amplitude"))
	settings.strength = float(tool_settings.get_setting("landform_strength")) * 0.01
	settings.falloff = float(tool_settings.get_setting("landform_falloff")) * 0.01
	settings.crest_width = float(tool_settings.get_setting("landform_crest")) * 0.01
	settings.asymmetry = float(tool_settings.get_setting("landform_asymmetry")) * 0.01
	settings.cliff_side = int(tool_settings.get_setting("landform_cliff_side"))
	settings.noise = float(tool_settings.get_setting("landform_noise")) * 0.01
	settings.noise_scale = float(tool_settings.get_setting("landform_noise_scale"))
	settings.terrace_step = float(tool_settings.get_setting("landform_terrace_step"))
	settings.road_crown = float(tool_settings.get_setting("landform_road_crown"))
	settings.water_level = float(tool_settings.get_setting("landform_water_level"))
	settings.shore_slope = float(tool_settings.get_setting("landform_shore_slope")) * 0.01
	settings.basin_floor_width = float(tool_settings.get_setting("landform_basin_floor_width")) * 0.01
	settings.basin_create_regions = bool(tool_settings.get_setting("landform_basin_create_regions"))
	settings.dune_spacing = float(tool_settings.get_setting("landform_dune_spacing"))
	settings.angle_of_repose = float(tool_settings.get_setting("landform_repose"))
	settings.iterations = int(tool_settings.get_setting("landform_iterations"))
	settings.seed = int(tool_settings.get_setting("landform_seed"))
	settings.auto_regions = bool(tool_settings.get_setting("auto_regions"))
	settings.paint_material = bool(tool_settings.get_setting("landform_paint_material"))
	settings.base_texture = _selected_texture_id()
	settings.overlay_texture = int(tool_settings.get_setting("landform_overlay_texture"))
	settings.material_blend = float(tool_settings.get_setting("landform_material_blend")) * 0.01
	settings.generate_mesh = bool(tool_settings.get_setting("landform_generate_mesh"))
	settings.geometry_mode = int(tool_settings.get_setting("landform_geometry_mode"))
	settings.mesh_depth = float(tool_settings.get_setting("landform_mesh_depth"))
	settings.mesh_irregularity = float(tool_settings.get_setting("landform_mesh_irregularity")) * 0.01
	settings.mesh_sample_spacing = float(tool_settings.get_setting("landform_mesh_spacing"))
	settings.mesh_lip = float(tool_settings.get_setting("landform_mesh_lip"))
	settings.mesh_toe = float(tool_settings.get_setting("landform_mesh_toe"))
	settings.module_length = float(tool_settings.get_setting("landform_module_length"))
	settings.module_corner_angle = float(tool_settings.get_setting("landform_module_corner_angle"))
	settings.module_min_scale = float(tool_settings.get_setting("landform_module_min_scale")) * 0.01
	settings.module_max_scale = float(tool_settings.get_setting("landform_module_max_scale")) * 0.01
	settings.module_mesh_ids = _collect_mesh_ids("landform_module_mesh_", FACE_MODULE_SLOTS)
	settings.corner_mesh_ids = _collect_mesh_ids("landform_corner_mesh_", CORNER_MODULE_SLOTS)
	settings.end_mesh_ids = _collect_mesh_ids("landform_end_mesh_", END_MODULE_SLOTS)
	settings.collision_spacing = float(tool_settings.get_setting("landform_collision_spacing"))
	settings.generate_collision = bool(tool_settings.get_setting("landform_generate_collision"))
	return settings


func _collect_mesh_ids(p_prefix: String, p_count: int) -> PackedInt32Array:
	var result := PackedInt32Array()
	for index: int in range(p_count):
		var mesh_id: int = int(tool_settings.get_setting("%s%d" % [p_prefix, index]))
		if mesh_id >= 0:
			result.push_back(mesh_id)
	return result


func _selected_texture_id() -> int:
	if not plugin or not asset_dock or not asset_dock.texture_list:
		return 0
	return maxi(asset_dock.texture_list.get_selected_asset_id(), 0)


func regenerate_landform(p_landform: Node3D, p_use_current_settings: bool = true) -> Node3D:
	if not is_instance_valid(p_landform) or not bool(p_landform.get_meta(&"terrain3d_generated_landform", false)):
		push_warning("Terrain3D landform: select a generated landform root or one of its children.")
		return null
	var terrain: T3DTerrain = p_landform.get_parent() as T3DTerrain
	if not terrain:
		push_warning("Terrain3D landform: the generated landform is no longer parented to Terrain3D.")
		return null
	var source_path: PackedVector3Array = _stored_landform_path(p_landform)
	if source_path.size() < MIN_PATH_POINTS:
		push_warning("Terrain3D landform: the stored source path is incomplete.")
		return null

	var old_settings: T3DLandformSettings = T3DLandformSettings.from_value(
		p_landform.get_meta(&"terrain3d_landform_settings", {}))
	old_settings.mode = int(p_landform.get_meta(&"terrain3d_landform_mode", old_settings.mode))
	var settings: T3DLandformSettings = _build_settings() if p_use_current_settings and tool_settings \
		else old_settings.duplicate_settings()
	settings.mode = old_settings.mode
	settings.generate_mesh = true
	var landform_id: int = int(p_landform.get_meta(&"terrain3d_landform_id", 0))
	if landform_id <= 0:
		push_warning("Terrain3D landform: the selected node has no valid ownership ID.")
		return null

	var placements := T3DLandformPlacements.new()
	var module_count: int = 0
	var mesh: ArrayMesh
	match settings.geometry_mode:
		T3DLandformBaker.GEOMETRY_MODULAR_INSTANCED:
			var modules: T3DLandformNativeResult = T3DLandformNativeResult.from_modules_value(
				baker.build_cliff_modules(terrain, source_path, settings.to_dictionary()))
			if not modules.modules_ok:
				push_warning("Terrain3D modular cliff: %s" % (
					modules.modules_error if not modules.modules_error.is_empty() else "No modules were generated."))
				return null
			placements = modules.placements
			module_count = modules.module_count
		T3DLandformBaker.GEOMETRY_PROCEDURAL_UNIQUE:
			mesh = baker.build_cliff_mesh(terrain, source_path, settings.to_dictionary())
			if not mesh:
				push_warning("Terrain3D landform: procedural cliff regeneration produced no mesh.")
				return null

	var collision_mesh: ArrayMesh
	if settings.generate_collision and settings.geometry_mode != T3DLandformBaker.GEOMETRY_TERRAIN_ONLY:
		collision_mesh = baker.build_cliff_collision_mesh(terrain, source_path, settings.to_dictionary())

	var old_placements: T3DLandformPlacements = T3DLandformPlacements.from_value(
		p_landform.get_meta(&"terrain3d_landform_module_placements", {}))
	if old_placements.is_empty() and int(p_landform.get_meta(&"terrain3d_landform_module_count", 0)) > 0:
		var old_modules: T3DLandformNativeResult = T3DLandformNativeResult.from_modules_value(
			baker.build_cliff_modules(terrain, source_path, old_settings.to_dictionary()))
		if old_modules.modules_ok:
			old_placements = old_modules.placements

	var replacement: Node3D = _create_generated_landform(
		mesh, collision_mesh, settings, source_path, landform_id, module_count, placements
	)
	_replace_generated_landform(terrain, p_landform, replacement, landform_id, placements)

	var undo: EditorUndoRedoManager = plugin.get_undo_redo()
	undo.create_action("Terrain3D Regenerate Landform", UndoRedo.MERGE_DISABLE, terrain)
	undo.add_undo_method(
		self, &"_replace_generated_landform", terrain, replacement, p_landform, landform_id, old_placements
	)
	undo.add_do_method(
		self, &"_replace_generated_landform", terrain, p_landform, replacement, landform_id, placements
	)
	undo.add_do_reference(replacement)
	undo.add_undo_reference(p_landform)
	undo.commit_action(false)
	return replacement


func _stored_landform_path(p_landform: Node3D) -> PackedVector3Array:
	var value: Variant = p_landform.get_meta(&"terrain3d_landform_path", PackedVector3Array())
	return value if value is PackedVector3Array else PackedVector3Array()


func _replace_generated_landform(
	p_terrain: T3DTerrain,
	p_remove: Node3D,
	p_add: Node3D,
	p_landform_id: int,
	p_placements: T3DLandformPlacements
) -> void:
	_remove_generated_landform(p_terrain, p_remove)
	if p_placements.is_empty():
		_remove_module_placements(p_terrain, p_landform_id)
	else:
		_add_module_placements(p_terrain, p_landform_id, p_placements)
	_add_generated_landform(p_terrain, p_add)


func _create_landform_id(p_terrain: T3DTerrain, p_path: PackedVector3Array,
		p_settings: T3DLandformSettings) -> int:
	var candidate: int = int(hash([
		Time.get_unix_time_from_system(),
		Time.get_ticks_usec(),
		p_path,
		p_settings.seed
	]) & 0x7fffffff)
	candidate = maxi(candidate, 1)
	while _landform_id_exists(p_terrain, candidate):
		candidate = 1 if candidate >= 0x7ffffffe else candidate + 1
	return candidate


func _landform_id_exists(p_terrain: T3DTerrain, p_landform_id: int) -> bool:
	for child: Node in p_terrain.get_children():
		if int(child.get_meta(&"terrain3d_landform_id", 0)) == p_landform_id:
			return true
	return false


func _create_generated_landform(
	p_mesh: ArrayMesh,
	p_collision_mesh: ArrayMesh,
	p_settings: T3DLandformSettings,
	p_path: PackedVector3Array,
	p_landform_id: int,
	p_module_count: int,
	p_placements: T3DLandformPlacements
) -> Node3D:
	var root := Node3D.new()
	root.name = "Terrain3D_%s_Landform" % _mode_name(p_settings.mode)
	root.set_meta(&"terrain3d_generated_landform", true)
	root.set_meta(&"terrain3d_landform_id", p_landform_id)
	root.set_meta(&"terrain3d_landform_mode", p_settings.mode)
	root.set_meta(&"terrain3d_landform_geometry_mode", p_settings.geometry_mode)
	root.set_meta(&"terrain3d_landform_generator_version", LANDFORM_GENERATOR_VERSION)
	root.set_meta(&"terrain3d_landform_settings", p_settings.to_dictionary())
	root.set_meta(&"terrain3d_landform_path", p_path.duplicate())
	root.set_meta(&"terrain3d_landform_module_count", p_module_count)
	root.set_meta(&"terrain3d_landform_module_placements", p_placements.to_dictionary())

	if p_mesh:
		var instance := MeshInstance3D.new()
		instance.name = "Visual"
		instance.mesh = p_mesh
		instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
		root.add_child(instance, true)

	if p_collision_mesh:
		var shape: Shape3D = p_collision_mesh.create_trimesh_shape()
		if shape:
			var body := StaticBody3D.new()
			body.name = "Collision"
			var collision := CollisionShape3D.new()
			collision.name = "Shape"
			collision.shape = shape
			body.add_child(collision, true)
			root.add_child(body, true)
	return root


func _add_generated_landform(p_terrain: T3DTerrain, p_instance: Node3D) -> void:
	if not is_instance_valid(p_terrain) or not is_instance_valid(p_instance):
		return
	if p_instance.get_parent() != p_terrain:
		p_terrain.add_child(p_instance, true)
	var edited_root: Node = EditorInterface.get_edited_scene_root()
	if edited_root:
		_set_owner_recursive(p_instance, edited_root)


func _remove_generated_landform(p_terrain: T3DTerrain, p_instance: Node3D) -> void:
	if is_instance_valid(p_terrain) and is_instance_valid(p_instance) and p_instance.get_parent() == p_terrain:
		p_terrain.remove_child(p_instance)


func _add_module_placements(p_terrain: T3DTerrain, p_landform_id: int,
		p_placements: T3DLandformPlacements) -> void:
	if not is_instance_valid(p_terrain) or p_landform_id <= 0 or p_placements.is_empty():
		return
	var instancer: T3DInstancer = p_terrain.get_instancer()
	if not instancer:
		return
	instancer.remove_owned_instances(p_landform_id, false, false)
	for group: T3DLandformPlacements.PlacementGroup in p_placements.groups:
		instancer.add_owned_transforms(
			group.mesh_id, group.transforms, p_landform_id, PackedColorArray(), false, false)
	instancer.update_mmis()


func _remove_module_placements(p_terrain: T3DTerrain, p_landform_id: int) -> void:
	if not is_instance_valid(p_terrain) or p_landform_id <= 0:
		return
	var instancer: T3DInstancer = p_terrain.get_instancer()
	if instancer:
		instancer.remove_owned_instances(p_landform_id, true, false)


func _set_owner_recursive(p_node: Node, p_owner: Node) -> void:
	p_node.owner = p_owner
	for child: Node in p_node.get_children():
		_set_owner_recursive(child, p_owner)


func _ensure_preview() -> void:
	if preview_instance or not plugin or not plugin.terrain:
		return
	preview_mesh = ImmediateMesh.new()
	preview_material = StandardMaterial3D.new()
	preview_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	preview_material.albedo_color = Color(1.0, 0.58, 0.12, 1.0)
	preview_material.no_depth_test = true
	preview_instance = MeshInstance3D.new()
	preview_instance.name = "Terrain3D_LandformStrokePreview"
	preview_instance.mesh = preview_mesh
	preview_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	plugin.terrain.add_child(preview_instance)


func _update_preview() -> void:
	if not preview_mesh:
		return
	preview_mesh.clear_surfaces()
	if path.size() < 2:
		return
	var lift: float = maxf(plugin.terrain.get_vertex_spacing() * 0.2, 0.05)
	preview_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP, preview_material)
	for point: Vector3 in path:
		preview_mesh.surface_add_vertex(point + Vector3.UP * lift)
	preview_mesh.surface_end()

	if int(tool_settings.get_setting("landform_mode")) != T3DLandformBaker.SCARP:
		return
	var side: float = -1.0 if int(tool_settings.get_setting("landform_cliff_side")) == 0 else 1.0
	var indicator_length: float = maxf(float(tool_settings.get_setting("landform_width")) * 0.22, 2.0)
	preview_mesh.surface_begin(Mesh.PRIMITIVE_LINES, preview_material)
	for index: int in range(0, path.size(), maxi(int(path.size() / 8.0), 1)):
		var previous: Vector3 = path[maxi(index - 1, 0)]
		var next: Vector3 = path[mini(index + 1, path.size() - 1)]
		var tangent := Vector2(next.x - previous.x, next.z - previous.z).normalized()
		if tangent.is_zero_approx():
			continue
		var raised_normal := Vector2(-tangent.y, tangent.x) * side
		var start: Vector3 = path[index] + Vector3.UP * lift
		var finish := start + Vector3(raised_normal.x, 0.0, raised_normal.y) * indicator_length
		preview_mesh.surface_add_vertex(start)
		preview_mesh.surface_add_vertex(finish)
	preview_mesh.surface_end()


func _destroy_preview() -> void:
	if preview_instance and is_instance_valid(preview_instance):
		preview_instance.queue_free()
	preview_instance = null
	preview_mesh = null
	preview_material = null


func _mode_name(p_mode: int) -> String:
	return MODE_NAMES[p_mode] if p_mode >= 0 and p_mode < MODE_NAMES.size() else "Landform"
