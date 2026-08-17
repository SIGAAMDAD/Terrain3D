# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# One ordered editor-time Terrain3D material classification rule.
@tool
class_name Terrain3DAutoShaderRule
extends Resource


enum CurveMode {
	LINEAR,
	SMOOTHSTEP,
	SMOOTHERSTEP,
}

@export_category("Rule")
@export var enabled: bool = true:
	set(value):
		if enabled == value:
			return
		enabled = value
		emit_changed()
@export var rule_name: String = "Material rule":
	set(value):
		if rule_name == value:
			return
		rule_name = value
		emit_changed()
@export_range(-1024, 1024, 1) var priority: int = 0:
	set(value):
		if priority == value:
			return
		priority = value
		emit_changed()
@export_range(0, 31, 1) var base_texture: int = 0:
	set(value):
		if base_texture == value:
			return
		base_texture = value
		emit_changed()
@export_range(0, 31, 1) var overlay_texture: int = 0:
	set(value):
		if overlay_texture == value:
			return
		overlay_texture = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var blend: float = 0.0:
	set(value):
		if blend == value:
			return
		blend = value
		emit_changed()
@export var overwrite_manual: bool = false:
	set(value):
		if overwrite_manual == value:
			return
		overwrite_manual = value
		emit_changed()
@export_enum("Linear", "Smoothstep", "Smootherstep") var curve_mode: int = CurveMode.SMOOTHSTEP:
	set(value):
		if curve_mode == value:
			return
		curve_mode = value
		emit_changed()
@export var custom_curve: Curve:
	set(value):
		if custom_curve == value:
			return
		_disconnect_custom_curve()
		custom_curve = value
		_connect_custom_curve()
		emit_changed()

@export_category("Slope")
@export var use_slope: bool = false:
	set(value):
		if use_slope == value:
			return
		use_slope = value
		emit_changed()
@export_range(0.0, 90.0, 0.1) var slope_min: float = 0.0:
	set(value):
		if slope_min == value:
			return
		slope_min = value
		emit_changed()
@export_range(0.0, 90.0, 0.1) var slope_max: float = 90.0:
	set(value):
		if slope_max == value:
			return
		slope_max = value
		emit_changed()
@export_range(0.0, 90.0, 0.1) var slope_falloff: float = 0.0:
	set(value):
		if slope_falloff == value:
			return
		slope_falloff = value
		emit_changed()

@export_category("Elevation")
@export var use_height: bool = false:
	set(value):
		if use_height == value:
			return
		use_height = value
		emit_changed()
@export_range(-100000.0, 100000.0, 0.1, "or_less", "or_greater", "suffix:m") var height_min: float = -384.0:
	set(value):
		if height_min == value:
			return
		height_min = value
		emit_changed()
@export_range(-100000.0, 100000.0, 0.1, "or_less", "or_greater", "suffix:m") var height_max: float = 768.0:
	set(value):
		if height_max == value:
			return
		height_max = value
		emit_changed()
@export_range(0.0, 100000.0, 0.1, "or_greater", "suffix:m") var height_falloff: float = 0.0:
	set(value):
		if height_falloff == value:
			return
		height_falloff = value
		emit_changed()

@export_category("Surface Shape")
@export var use_curvature: bool = false:
	set(value):
		if use_curvature == value:
			return
		use_curvature = value
		emit_changed()
@export_range(-1.0, 1.0, 0.001) var curvature_min: float = -1.0:
	set(value):
		if curvature_min == value:
			return
		curvature_min = value
		emit_changed()
@export_range(-1.0, 1.0, 0.001) var curvature_max: float = 1.0:
	set(value):
		if curvature_max == value:
			return
		curvature_max = value
		emit_changed()
@export_range(0.0, 2.0, 0.001) var curvature_falloff: float = 0.0:
	set(value):
		if curvature_falloff == value:
			return
		curvature_falloff = value
		emit_changed()
@export var use_concavity: bool = false:
	set(value):
		if use_concavity == value:
			return
		use_concavity = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var concavity_min: float = 0.0:
	set(value):
		if concavity_min == value:
			return
		concavity_min = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var concavity_max: float = 1.0:
	set(value):
		if concavity_max == value:
			return
		concavity_max = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var concavity_falloff: float = 0.0:
	set(value):
		if concavity_falloff == value:
			return
		concavity_falloff = value
		emit_changed()

@export_category("Aspect")
@export var use_aspect: bool = false:
	set(value):
		if use_aspect == value:
			return
		use_aspect = value
		emit_changed()
@export_range(0.0, 360.0, 0.1, "suffix:deg") var aspect_center: float = 0.0:
	set(value):
		if aspect_center == value:
			return
		aspect_center = value
		emit_changed()
@export_range(0.0, 360.0, 0.1, "suffix:deg") var aspect_width: float = 360.0:
	set(value):
		if aspect_width == value:
			return
		aspect_width = value
		emit_changed()
@export_range(0.0, 180.0, 0.1, "suffix:deg") var aspect_falloff: float = 0.0:
	set(value):
		if aspect_falloff == value:
			return
		aspect_falloff = value
		emit_changed()

@export_category("Hydrology and Erosion")
@export var use_flow: bool = false:
	set(value):
		if use_flow == value:
			return
		use_flow = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var flow_min: float = 0.0:
	set(value):
		if flow_min == value:
			return
		flow_min = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var flow_max: float = 1.0:
	set(value):
		if flow_max == value:
			return
		flow_max = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var flow_falloff: float = 0.0:
	set(value):
		if flow_falloff == value:
			return
		flow_falloff = value
		emit_changed()
@export var use_deposition: bool = false:
	set(value):
		if use_deposition == value:
			return
		use_deposition = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var deposition_min: float = 0.0:
	set(value):
		if deposition_min == value:
			return
		deposition_min = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var deposition_max: float = 1.0:
	set(value):
		if deposition_max == value:
			return
		deposition_max = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var deposition_falloff: float = 0.0:
	set(value):
		if deposition_falloff == value:
			return
		deposition_falloff = value
		emit_changed()
@export var use_water_distance: bool = false:
	set(value):
		if use_water_distance == value:
			return
		use_water_distance = value
		emit_changed()
@export_range(0.0, 100000.0, 0.1, "or_greater", "suffix:m") var water_distance_min: float = 0.0:
	set(value):
		if water_distance_min == value:
			return
		water_distance_min = value
		emit_changed()
@export_range(0.0, 100000.0, 0.1, "or_greater", "suffix:m") var water_distance_max: float = 100000.0:
	set(value):
		if water_distance_max == value:
			return
		water_distance_max = value
		emit_changed()
@export_range(0.0, 100000.0, 0.1, "or_greater", "suffix:m") var water_distance_falloff: float = 0.0:
	set(value):
		if water_distance_falloff == value:
			return
		water_distance_falloff = value
		emit_changed()

@export_category("Masks and Breakup")
@export var use_noise: bool = false:
	set(value):
		if use_noise == value:
			return
		use_noise = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var noise_min: float = 0.0:
	set(value):
		if noise_min == value:
			return
		noise_min = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var noise_max: float = 1.0:
	set(value):
		if noise_max == value:
			return
		noise_max = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var noise_falloff: float = 0.0:
	set(value):
		if noise_falloff == value:
			return
		noise_falloff = value
		emit_changed()
@export var use_biome: bool = false:
	set(value):
		if use_biome == value:
			return
		use_biome = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var biome_min: float = 0.0:
	set(value):
		if biome_min == value:
			return
		biome_min = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var biome_max: float = 1.0:
	set(value):
		if biome_max == value:
			return
		biome_max = value
		emit_changed()
@export_range(0.0, 1.0, 0.001) var biome_falloff: float = 0.0:
	set(value):
		if biome_falloff == value:
			return
		biome_falloff = value
		emit_changed()

@export_category("Existing Material Filter")
@export var use_existing_material: bool = false:
	set(value):
		if use_existing_material == value:
			return
		use_existing_material = value
		emit_changed()
@export_range(-1, 31, 1) var existing_base_texture: int = -1:
	set(value):
		if existing_base_texture == value:
			return
		existing_base_texture = value
		emit_changed()
@export_range(-1, 31, 1) var existing_overlay_texture: int = -1:
	set(value):
		if existing_overlay_texture == value:
			return
		existing_overlay_texture = value
		emit_changed()


func sync_subresource_connections() -> void:
	_disconnect_custom_curve()
	_connect_custom_curve()


func _connect_custom_curve() -> void:
	if is_instance_valid(custom_curve) and not custom_curve.changed.is_connected(_on_custom_curve_changed):
		custom_curve.changed.connect(_on_custom_curve_changed)


func _disconnect_custom_curve() -> void:
	if is_instance_valid(custom_curve) and custom_curve.changed.is_connected(_on_custom_curve_changed):
		custom_curve.changed.disconnect(_on_custom_curve_changed)


func _on_custom_curve_changed() -> void:
	emit_changed()


func get_display_name() -> String:
	return rule_name if not rule_name.is_empty() else "Material rule"


func get_output_summary() -> String:
	if base_texture == overlay_texture or blend <= 0.001:
		return "Texture %d" % base_texture
	return "%d → %d at %d%%" % [base_texture, overlay_texture, roundi(blend * 100.0)]


func get_condition_summary() -> String:
	var conditions := PackedStringArray()
	if use_slope:
		conditions.append("slope %.0f–%.0f°" % [slope_min, slope_max])
	if use_height:
		conditions.append("height %.0f–%.0f m" % [height_min, height_max])
	if use_curvature:
		conditions.append("curvature")
	if use_concavity:
		conditions.append("concavity")
	if use_aspect:
		conditions.append("aspect %.0f° ± %.0f°" % [aspect_center, aspect_width * 0.5])
	if use_flow:
		conditions.append("flow")
	if use_deposition:
		conditions.append("deposition")
	if use_water_distance:
		conditions.append("water distance")
	if use_noise:
		conditions.append("noise")
	if use_biome:
		conditions.append("biome mask")
	if use_existing_material:
		conditions.append("existing material")
	return ", ".join(conditions) if not conditions.is_empty() else "fallback"


func to_bake_dictionary() -> Dictionary[String, Variant]:
	var curve_lut := PackedFloat32Array()
	if is_instance_valid(custom_curve):
		curve_lut.resize(64)
		for index: int in curve_lut.size():
			curve_lut[index] = custom_curve.sample_baked(float(index) / float(curve_lut.size() - 1))
	return {
		"name": get_display_name(),
		"enabled": enabled,
		"priority": priority,
		"base_texture": base_texture,
		"overlay_texture": overlay_texture,
		"blend": blend,
		"overwrite_manual": overwrite_manual,
		"curve_mode": int(curve_mode),
		"curve_lut": curve_lut,
		"use_slope": use_slope,
		"slope_range": Vector2(slope_min, slope_max),
		"slope_falloff": slope_falloff,
		"use_height": use_height,
		"height_range": Vector2(height_min, height_max),
		"height_falloff": height_falloff,
		"use_curvature": use_curvature,
		"curvature_range": Vector2(curvature_min, curvature_max),
		"curvature_falloff": curvature_falloff,
		"use_concavity": use_concavity,
		"concavity_range": Vector2(concavity_min, concavity_max),
		"concavity_falloff": concavity_falloff,
		"use_aspect": use_aspect,
		"aspect_center": aspect_center,
		"aspect_width": aspect_width,
		"aspect_falloff": aspect_falloff,
		"use_flow": use_flow,
		"flow_range": Vector2(flow_min, flow_max),
		"flow_falloff": flow_falloff,
		"use_deposition": use_deposition,
		"deposition_range": Vector2(deposition_min, deposition_max),
		"deposition_falloff": deposition_falloff,
		"use_water_distance": use_water_distance,
		"water_distance_range": Vector2(water_distance_min, water_distance_max),
		"water_distance_falloff": water_distance_falloff,
		"use_noise": use_noise,
		"noise_range": Vector2(noise_min, noise_max),
		"noise_falloff": noise_falloff,
		"use_biome": use_biome,
		"biome_range": Vector2(biome_min, biome_max),
		"biome_falloff": biome_falloff,
		"use_existing_material": use_existing_material,
		"existing_base": existing_base_texture,
		"existing_overlay": existing_overlay_texture,
	}
