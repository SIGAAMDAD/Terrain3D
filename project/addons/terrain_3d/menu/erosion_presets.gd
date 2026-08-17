# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Curated Terrain3D erosion presets. Presets are starting points, not calibrated geology.
@tool
extends RefCounted

const PRESET_NAMES: PackedStringArray = [
	"Gentle Preview",
	"Balanced Natural",
	"Humid Rivers",
	"Arid Wind",
	"Alpine Freeze-Thaw",
	"Coastal Shelf",
	"Long-Term Weathering",
	"Alluvial Gravel Desert",
	"Dune Sea",
	"Salt Playa",
	"Cold Continental Desert",
	"Desert Badlands",
	"Foot Traffic",
]
const PRESET_DESCRIPTIONS: PackedStringArray = [
	"Low-risk weathering and water flow for checking scale and masks.",
	"General-purpose rainfall, weathering, oxidation, and restrained deposition.",
	"High runoff, channel incision, floodplain deposition, and chemical weathering.",
	"Sparse storms, strong directional transport, sheltered deposition, and oxidation.",
	"Rock breakdown, talus creep, runoff, and repeated freezing around exposed slopes.",
	"Wave undercutting, beach smoothing, nearshore deposition, and wet weathering.",
	"Slow geomorphic aging with stronger oxidation and material transition output.",
	"Rare violent storms, sorted sediment, debris-rich washes, alluvial fans, and gravel-armored plains.",
	"Sand-dominant wind transport with lee deposition, angle-of-repose avalanching, and rare storm reworking.",
	"Temporary flooding, fine settling, extreme evaporation, salt crystallization, and deflation at basin margins.",
	"Seasonal snow storage and melt pulses, freeze-thaw breakdown, talus, sparse runoff, and wind reworking.",
	"Soft layered substrate, wet-dry slaking, dense rills, debris flows, and resistant bedrock benches.",
	"Mask-driven trail incision, widening, compaction, smoothing, and runoff.",
]
const PRESET_0_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 100.0,
	&"global_intensity": 0.45,
}
const PRESET_0_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 48,
	&"resolution_scale": 8,
}
const PRESET_0_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"weathering_enabled": true,
	&"oxidation_enabled": true,
	&"wind_enabled": false,
	&"wave_enabled": false,
	&"traffic_enabled": false,
	&"material_output_enabled": false,
}
const PRESET_1_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 1000.0,
	&"global_intensity": 1.0,
	&"rainfall_m_per_year": 0.002,
	&"channel_incision_strength": 0.75,
	&"standing_water_deposition_rate": 0.08,
}
const PRESET_1_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 128,
	&"resolution_scale": 4,
}
const PRESET_1_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"weathering_enabled": true,
	&"oxidation_enabled": true,
	&"wind_enabled": false,
	&"wave_enabled": false,
	&"traffic_enabled": false,
}
const PRESET_2_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 2500.0,
	&"rainfall_m_per_year": 0.012,
	&"rainfall_variation": 0.45,
	&"evaporation_fraction": 0.08,
	&"infiltration_fraction": 0.12,
	&"flow_rate": 0.9,
	&"sediment_capacity": 2.2,
	&"hydraulic_erosion_m_per_year": 0.00035,
	&"hydraulic_deposition_rate": 0.42,
	&"storm_frequency": 0.45,
	&"storm_intensity": 2.5,
	&"channel_incision_strength": 3.0,
	&"standing_water_deposition_rate": 0.25,
	&"chemical_weathering_m_per_year": 8e-05,
	&"chemical_moisture_exponent": 1.5,
}
const PRESET_2_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 256,
	&"resolution_scale": 4,
}
const PRESET_2_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"weathering_enabled": true,
	&"oxidation_enabled": true,
	&"wind_enabled": false,
	&"wave_enabled": false,
}
const PRESET_3_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 5000.0,
	&"rainfall_m_per_year": 0.00035,
	&"evaporation_fraction": 0.78,
	&"storm_frequency": 0.08,
	&"storm_intensity": 7.0,
	&"physical_weathering_m_per_year": 8e-05,
	&"chemical_weathering_m_per_year": 5e-06,
	&"oxidation_rate_per_year": 0.0002,
	&"wind_strength": 2.5,
	&"wind_variation": 0.35,
	&"wind_erosion_m_per_year": 0.00012,
	&"wind_transport_rate": 0.62,
	&"wind_deposition_rate": 0.55,
	&"wind_shelter_strength": 0.8,
	&"wind_threshold": 0.1,
	&"wind_deposition_slope_bias": 1.25,
}
const PRESET_3_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 256,
	&"resolution_scale": 4,
	&"wind_transport_distance_cells": 5,
}
const PRESET_3_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"weathering_enabled": true,
	&"oxidation_enabled": true,
	&"wind_enabled": true,
	&"wave_enabled": false,
}
const PRESET_4_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 3500.0,
	&"rainfall_m_per_year": 0.004,
	&"evaporation_fraction": 0.12,
	&"freeze_thaw_strength": 1.8,
	&"physical_weathering_m_per_year": 0.00018,
	&"chemical_weathering_m_per_year": 1e-05,
	&"weathering_slope_bias": 2.0,
	&"talus_angle_degrees": 34.0,
	&"thermal_creep_rate": 0.48,
	&"base_hardness": 0.82,
	&"wind_strength": 1.2,
	&"wind_erosion_m_per_year": 2.5e-05,
}
const PRESET_4_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 256,
	&"resolution_scale": 4,
}
const PRESET_4_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"weathering_enabled": true,
	&"oxidation_enabled": false,
	&"wind_enabled": true,
}
const PRESET_5_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 1800.0,
	&"rainfall_m_per_year": 0.004,
	&"wave_strength": 1.4,
	&"wave_erosion_m_per_year": 0.0015,
	&"wave_deposition_rate": 0.68,
	&"coastline_band_m": 18.0,
	&"wave_fetch_distance_m": 1024.0,
	&"wave_undercut_bias": 2.25,
	&"beach_smoothing": 0.45,
}
const PRESET_5_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 192,
	&"resolution_scale": 4,
}
const PRESET_5_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"weathering_enabled": true,
	&"oxidation_enabled": true,
	&"wind_enabled": false,
	&"wave_enabled": true,
}
const PRESET_6_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 10000.0,
	&"global_intensity": 0.8,
	&"physical_weathering_m_per_year": 7.5e-05,
	&"chemical_weathering_m_per_year": 3.5e-05,
	&"oxidation_rate_per_year": 0.00028,
	&"oxidation_weakening": 0.32,
	&"material_transition_strength": 0.85,
	&"material_transition_threshold": 0.12,
	&"material_transition_depth_m": 0.35,
}
const PRESET_6_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 384,
	&"resolution_scale": 4,
}
const PRESET_6_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"weathering_enabled": true,
	&"oxidation_enabled": true,
	&"wind_enabled": false,
	&"wave_enabled": false,
	&"material_output_enabled": true,
}
const PRESET_7_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 12000.0,
	&"rainfall_m_per_year": 0.00045,
	&"evaporation_fraction": 0.82,
	&"infiltration_fraction": 0.08,
	&"storm_frequency": 0.06,
	&"storm_intensity": 12.0,
	&"channel_incision_strength": 3.5,
	&"alluvial_fan_strength": 1.25,
	&"alluvial_fan_spread": 0.55,
	&"debris_flow_rate": 0.45,
	&"wind_strength": 1.1,
}
const PRESET_7_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 384,
	&"resolution_scale": 4,
	&"debris_flow_runout_cells": 7,
}
const PRESET_7_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"weathering_enabled": true,
	&"oxidation_enabled": true,
	&"sediment_sorting_enabled": true,
	&"alluvial_fan_enabled": true,
	&"debris_flow_enabled": true,
	&"desert_pavement_enabled": true,
	&"wind_enabled": true,
	&"wet_dry_enabled": true,
	&"material_output_enabled": true,
}
const PRESET_8_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 6000.0,
	&"rainfall_m_per_year": 0.00012,
	&"storm_frequency": 0.025,
	&"storm_intensity": 8.0,
	&"wind_strength": 3.2,
	&"wind_transport_rate": 0.8,
	&"wind_deposition_rate": 0.72,
	&"physical_weathering_m_per_year": 0.00015,
	&"sand_repose_angle_degrees": 33.0,
	&"dune_avalanche_rate": 0.5,
}
const PRESET_8_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 384,
	&"resolution_scale": 2,
	&"wind_transport_distance_cells": 7,
	&"dune_relaxation_iterations": 4,
}
const PRESET_8_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"wind_enabled": true,
	&"weathering_enabled": true,
	&"dune_enabled": true,
	&"desert_pavement_enabled": false,
	&"sediment_sorting_enabled": true,
}
const PRESET_9_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 9000.0,
	&"rainfall_m_per_year": 0.0003,
	&"storm_frequency": 0.05,
	&"storm_intensity": 9.0,
	&"evaporation_fraction": 0.92,
	&"standing_water_deposition_rate": 0.75,
	&"fine_settling_rate": 0.22,
	&"background_salinity": 0.8,
	&"salt_crystallization_strength": 1.7,
	&"wind_strength": 1.4,
}
const PRESET_9_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 320,
	&"resolution_scale": 4,
}
const PRESET_9_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"salt_weathering_enabled": true,
	&"wet_dry_enabled": true,
	&"wind_enabled": true,
	&"desert_pavement_enabled": true,
	&"material_output_enabled": true,
}
const PRESET_10_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 8000.0,
	&"rainfall_m_per_year": 0.0012,
	&"evaporation_fraction": 0.48,
	&"snowfall_fraction": 0.75,
	&"snow_melt_fraction_per_step": 0.35,
	&"freeze_thaw_strength": 1.8,
	&"thermal_creep_rate": 0.42,
	&"wind_strength": 1.6,
}
const PRESET_10_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 384,
	&"resolution_scale": 4,
}
const PRESET_10_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"snowmelt_enabled": true,
	&"weathering_enabled": true,
	&"wind_enabled": true,
	&"sediment_sorting_enabled": true,
	&"alluvial_fan_enabled": true,
	&"wet_dry_enabled": true,
}
const PRESET_11_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 4500.0,
	&"rainfall_m_per_year": 0.0008,
	&"storm_frequency": 0.12,
	&"storm_intensity": 8.0,
	&"channel_incision_strength": 4.5,
	&"hydraulic_erosion_m_per_year": 0.0005,
	&"wet_dry_weathering_m_per_cycle": 0.002,
	&"debris_flow_rate": 0.5,
	&"surface_layer_depth_m": 6.0,
	&"bedrock_hardness": 0.9,
}
const PRESET_11_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 320,
	&"resolution_scale": 2,
}
const PRESET_11_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"wet_dry_enabled": true,
	&"debris_flow_enabled": true,
	&"sediment_sorting_enabled": true,
	&"stratigraphy_enabled": true,
	&"material_output_enabled": true,
}
const PRESET_12_FLOATS: Dictionary[StringName, float] = {
	&"simulation_years": 80.0,
	&"traffic_erosion_m_per_year": 0.0008,
	&"traffic_compaction": 0.85,
	&"traffic_smoothing": 0.3,
	&"traffic_runoff_multiplier": 2.0,
	&"traffic_threshold": 0.15,
}
const PRESET_12_INTS: Dictionary[StringName, int] = {
	&"simulation_steps": 96,
	&"resolution_scale": 2,
	&"traffic_widening_cells": 2,
}
const PRESET_12_BOOLS: Dictionary[StringName, bool] = {
	&"hydraulic_enabled": true,
	&"weathering_enabled": false,
	&"oxidation_enabled": false,
	&"wind_enabled": false,
	&"wave_enabled": false,
	&"traffic_enabled": true,
}

static func get_names() -> PackedStringArray:
	return PRESET_NAMES


static func get_description(p_index: int) -> String:
	if p_index < 0 or p_index >= PRESET_NAMES.size():
		return "Custom settings."
	return PRESET_DESCRIPTIONS[p_index]


static func apply(p_settings: Terrain3DErosionSettings, p_index: int) -> void:
	if not p_settings or p_index < 0 or p_index >= PRESET_NAMES.size():
		return
	_reset_process_defaults(p_settings)
	match p_index:
		0:
			_apply_values(p_settings, PRESET_0_FLOATS, PRESET_0_INTS, PRESET_0_BOOLS)
		1:
			_apply_values(p_settings, PRESET_1_FLOATS, PRESET_1_INTS, PRESET_1_BOOLS)
		2:
			_apply_values(p_settings, PRESET_2_FLOATS, PRESET_2_INTS, PRESET_2_BOOLS)
		3:
			_apply_values(p_settings, PRESET_3_FLOATS, PRESET_3_INTS, PRESET_3_BOOLS)
		4:
			_apply_values(p_settings, PRESET_4_FLOATS, PRESET_4_INTS, PRESET_4_BOOLS)
		5:
			_apply_values(p_settings, PRESET_5_FLOATS, PRESET_5_INTS, PRESET_5_BOOLS)
		6:
			_apply_values(p_settings, PRESET_6_FLOATS, PRESET_6_INTS, PRESET_6_BOOLS)
		7:
			_apply_values(p_settings, PRESET_7_FLOATS, PRESET_7_INTS, PRESET_7_BOOLS)
		8:
			_apply_values(p_settings, PRESET_8_FLOATS, PRESET_8_INTS, PRESET_8_BOOLS)
		9:
			_apply_values(p_settings, PRESET_9_FLOATS, PRESET_9_INTS, PRESET_9_BOOLS)
		10:
			_apply_values(p_settings, PRESET_10_FLOATS, PRESET_10_INTS, PRESET_10_BOOLS)
		11:
			_apply_values(p_settings, PRESET_11_FLOATS, PRESET_11_INTS, PRESET_11_BOOLS)
		12:
			_apply_values(p_settings, PRESET_12_FLOATS, PRESET_12_INTS, PRESET_12_BOOLS)
	if PRESET_NAMES[p_index] == "Cold Continental Desert":
		p_settings.temperature_curve = _make_cold_desert_temperature_curve()
		p_settings.snowmelt_curve = _make_snowmelt_pulse_curve()
	p_settings.emit_changed()


static func _apply_values(
		p_settings: Terrain3DErosionSettings,
		p_floats: Dictionary[StringName, float],
		p_ints: Dictionary[StringName, int],
		p_bools: Dictionary[StringName, bool],
) -> void:
	for property_name: StringName in p_floats:
		p_settings.set(property_name, p_floats[property_name])
	for property_name: StringName in p_ints:
		p_settings.set(property_name, p_ints[property_name])
	for property_name: StringName in p_bools:
		p_settings.set(property_name, p_bools[property_name])


static func _make_cold_desert_temperature_curve() -> Curve:
	var curve := Curve.new()
	curve.min_value = -1.0
	curve.max_value = 1.0
	curve.add_point(Vector2(0.0, -0.55))
	curve.add_point(Vector2(0.2, -0.15))
	curve.add_point(Vector2(0.42, 0.65))
	curve.add_point(Vector2(0.68, 0.35))
	curve.add_point(Vector2(0.86, -0.1))
	curve.add_point(Vector2(1.0, -0.55))
	return curve


static func _make_snowmelt_pulse_curve() -> Curve:
	var curve := Curve.new()
	curve.min_value = 0.0
	curve.max_value = 2.0
	curve.add_point(Vector2(0.0, 0.15))
	curve.add_point(Vector2(0.25, 0.35))
	curve.add_point(Vector2(0.42, 1.8))
	curve.add_point(Vector2(0.58, 0.65))
	curve.add_point(Vector2(1.0, 0.15))
	return curve


static func _reset_process_defaults(p_settings: Terrain3DErosionSettings) -> void:
	var defaults := Terrain3DErosionSettings.new()
	for property: Dictionary[StringName, Variant] in defaults.get_property_list():
		var usage := int(property.get("usage", 0))
		if (usage & PROPERTY_USAGE_STORAGE) == 0:
			continue
		var property_name := StringName(property.name)
		var value: Variant = defaults.get(property_name)
		if value is Resource:
			value = value.duplicate(true)
		p_settings.set(property_name, value)
