@tool
class_name T3DLandformSettings
extends RefCounted

var mode: int = 0
var morphology_mode: int = 0
var width: float = 32.0
var amplitude: float = 12.0
var strength: float = 1.0
var falloff: float = 0.65
var crest_width: float = 0.18
var asymmetry: float = 0.0
var cliff_side: int = 1
var noise: float = 0.12
var noise_scale: float = 24.0
var terrace_step: float = 4.0
var road_crown: float = 0.15
var water_level: float = 0.0
var shore_slope: float = 0.08
var basin_floor_width: float = 0.35
var basin_create_regions: bool = false
var dune_spacing: float = 28.0
var angle_of_repose: float = 36.0
var iterations: int = 4
@warning_ignore("shadowed_global_identifier")
var seed: int = 1337
var auto_regions: bool = true
var paint_material: bool = false
var base_texture: int = 0
var overlay_texture: int = 0
var material_blend: float = 0.0
var generate_mesh: bool = false
var geometry_mode: int = 2
var mesh_depth: float = 5.0
var mesh_irregularity: float = 0.28
var mesh_sample_spacing: float = 3.0
var mesh_lip: float = 1.25
var mesh_toe: float = 1.5
var module_length: float = 8.0
var module_corner_angle: float = 18.0
var module_min_scale: float = 0.65
var module_max_scale: float = 1.75
var module_mesh_ids: PackedInt32Array = PackedInt32Array()
var corner_mesh_ids: PackedInt32Array = PackedInt32Array()
var end_mesh_ids: PackedInt32Array = PackedInt32Array()
var collision_spacing: float = 8.0
var generate_collision: bool = true


static func from_value(p_value: Variant) -> T3DLandformSettings:
	if not p_value is Dictionary:
		return T3DLandformSettings.new()
	var values := T3DRuntimeValueMap.from_value(p_value)
	var settings := T3DLandformSettings.new()
	settings.mode = values.int_value("mode", settings.mode)
	settings.morphology_mode = values.int_value("morphology_mode", settings.morphology_mode)
	settings.width = values.float_value("width", settings.width)
	settings.amplitude = values.float_value("amplitude", settings.amplitude)
	settings.strength = values.float_value("strength", settings.strength)
	settings.falloff = values.float_value("falloff", settings.falloff)
	settings.crest_width = values.float_value("crest_width", settings.crest_width)
	settings.asymmetry = values.float_value("asymmetry", settings.asymmetry)
	settings.cliff_side = values.int_value("cliff_side", settings.cliff_side)
	settings.noise = values.float_value("noise", settings.noise)
	settings.noise_scale = values.float_value("noise_scale", settings.noise_scale)
	settings.terrace_step = values.float_value("terrace_step", settings.terrace_step)
	settings.road_crown = values.float_value("road_crown", settings.road_crown)
	settings.water_level = values.float_value("water_level", settings.water_level)
	settings.shore_slope = values.float_value("shore_slope", settings.shore_slope)
	settings.basin_floor_width = values.float_value("basin_floor_width", settings.basin_floor_width)
	settings.basin_create_regions = values.bool_value("basin_create_regions", settings.basin_create_regions)
	settings.dune_spacing = values.float_value("dune_spacing", settings.dune_spacing)
	settings.angle_of_repose = values.float_value("angle_of_repose", settings.angle_of_repose)
	settings.iterations = values.int_value("iterations", settings.iterations)
	settings.seed = values.int_value("seed", settings.seed)
	settings.auto_regions = values.bool_value("auto_regions", settings.auto_regions)
	settings.paint_material = values.bool_value("paint_material", settings.paint_material)
	settings.base_texture = values.int_value("base_texture", settings.base_texture)
	settings.overlay_texture = values.int_value("overlay_texture", settings.overlay_texture)
	settings.material_blend = values.float_value("material_blend", settings.material_blend)
	settings.generate_mesh = values.bool_value("generate_mesh", settings.generate_mesh)
	settings.geometry_mode = values.int_value("geometry_mode", settings.geometry_mode)
	settings.mesh_depth = values.float_value("mesh_depth", settings.mesh_depth)
	settings.mesh_irregularity = values.float_value("mesh_irregularity", settings.mesh_irregularity)
	settings.mesh_sample_spacing = values.float_value("mesh_sample_spacing", settings.mesh_sample_spacing)
	settings.mesh_lip = values.float_value("mesh_lip", settings.mesh_lip)
	settings.mesh_toe = values.float_value("mesh_toe", settings.mesh_toe)
	settings.module_length = values.float_value("module_length", settings.module_length)
	settings.module_corner_angle = values.float_value("module_corner_angle", settings.module_corner_angle)
	settings.module_min_scale = values.float_value("module_min_scale", settings.module_min_scale)
	settings.module_max_scale = values.float_value("module_max_scale", settings.module_max_scale)
	settings.module_mesh_ids = values.int32_array("module_mesh_ids")
	settings.corner_mesh_ids = values.int32_array("corner_mesh_ids")
	settings.end_mesh_ids = values.int32_array("end_mesh_ids")
	settings.collision_spacing = values.float_value("collision_spacing", settings.collision_spacing)
	settings.generate_collision = values.bool_value("generate_collision", settings.generate_collision)
	return settings


func duplicate_settings() -> T3DLandformSettings:
	return T3DLandformSettings.from_value(to_dictionary())


func to_dictionary() -> Dictionary[String, Variant]:
	return {
		"mode": mode,
		"morphology_mode": morphology_mode,
		"width": width,
		"amplitude": amplitude,
		"strength": strength,
		"falloff": falloff,
		"crest_width": crest_width,
		"asymmetry": asymmetry,
		"cliff_side": cliff_side,
		"noise": noise,
		"noise_scale": noise_scale,
		"terrace_step": terrace_step,
		"road_crown": road_crown,
		"water_level": water_level,
		"shore_slope": shore_slope,
		"basin_floor_width": basin_floor_width,
		"basin_create_regions": basin_create_regions,
		"dune_spacing": dune_spacing,
		"angle_of_repose": angle_of_repose,
		"iterations": iterations,
		"seed": seed,
		"auto_regions": auto_regions,
		"paint_material": paint_material,
		"base_texture": base_texture,
		"overlay_texture": overlay_texture,
		"material_blend": material_blend,
		"generate_mesh": generate_mesh,
		"geometry_mode": geometry_mode,
		"mesh_depth": mesh_depth,
		"mesh_irregularity": mesh_irregularity,
		"mesh_sample_spacing": mesh_sample_spacing,
		"mesh_lip": mesh_lip,
		"mesh_toe": mesh_toe,
		"module_length": module_length,
		"module_corner_angle": module_corner_angle,
		"module_min_scale": module_min_scale,
		"module_max_scale": module_max_scale,
		"module_mesh_ids": module_mesh_ids,
		"corner_mesh_ids": corner_mesh_ids,
		"end_mesh_ids": end_mesh_ids,
		"collision_spacing": collision_spacing,
		"generate_collision": generate_collision,
	}
