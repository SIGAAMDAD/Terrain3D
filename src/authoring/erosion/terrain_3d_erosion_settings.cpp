// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "integration/godot/terrain_3d_binding.h"
#include "authoring/erosion/terrain_3d_erosion_settings.h"

#include <godot_cpp/core/class_db.hpp>

static inline int32_t t3dErosionSettingsSanitizeResolutionScale( int32_t nValue ) {
	if ( nValue <= 1 ) {
		return 1;
	}
	if ( nValue <= 2 ) {
		return 2;
	}
	if ( nValue <= 4 ) {
		return 4;
	}
	if ( nValue <= 8 ) {
		return 8;
	}
	return 16;
}

Ref<Curve> Terrain3DErosionSettings::makeFlatCurve( const float fValue, const float fMin, const float fMax ) {
	Ref<Curve> pCurve;
	pCurve.instantiate();
	pCurve->set_min_value( fMin );
	pCurve->set_max_value( fMax );
	pCurve->add_point( Vector2( 0.0f, fValue ) );
	pCurve->add_point( Vector2( 1.0f, fValue ) );
	return pCurve;
}

void Terrain3DErosionSettings::setCurve( Ref<Curve> &rTargetCurve, const Ref<Curve> &rSourceCurve ) {
	if ( rTargetCurve == rSourceCurve ) {
		return;
	}
	rTargetCurve = rSourceCurve;
	emit_changed();
}

Terrain3DErosionSettings::Terrain3DErosionSettings( void ) {
	m_masterIntensityCurve = makeFlatCurve( 1.0f );
	m_rainfallCurve = makeFlatCurve( 1.0f );
	m_weatheringCurve = makeFlatCurve( 1.0f );
	m_oxidationCurve = makeFlatCurve( 1.0f );
	m_windCurve = makeFlatCurve( 1.0f );
	m_waveCurve = makeFlatCurve( 1.0f );
	m_trafficCurve = makeFlatCurve( 1.0f );
	m_temperatureCurve = makeFlatCurve( 0.5f, -1.0f, 1.0f );
	m_seaLevelCurve = makeFlatCurve( 0.0f, -1000000.0f, 1000000.0f );
	m_snowmeltCurve = makeFlatCurve( 1.0f );
	m_saltWeatheringCurve = makeFlatCurve( 1.0f );
	m_debrisFlowCurve = makeFlatCurve( 1.0f );
}

#define SET_CLAMPED( methodName, methodSymbol, field_name, value_type, min_value, max_value )                                              \
	void Terrain3DErosionSettings::methodSymbol( value_type valueArg ) {                                                                   \
		value_type value = CLAMP( valueArg, value_type( min_value ), value_type( max_value ) );                                            \
		if ( field_name == value ) {                                                                                                       \
			return;                                                                                                                        \
		}                                                                                                                                  \
		field_name = value;                                                                                                                \
		emit_changed();                                                                                                                    \
	}

#define SET_VALUE( methodName, methodSymbol, field_name, value_type )                                                                      \
	void Terrain3DErosionSettings::methodSymbol( value_type valueArg ) {                                                                   \
		if ( field_name == valueArg ) {                                                                                                    \
			return;                                                                                                                        \
		}                                                                                                                                  \
		field_name = valueArg;                                                                                                             \
		emit_changed();                                                                                                                    \
	}

#define SET_REF_VALUE( methodName, methodSymbol, field_name, value_type )                                                                  \
	void Terrain3DErosionSettings::methodSymbol( const value_type &valueArg ) {                                                            \
		if ( field_name == valueArg ) {                                                                                                    \
			return;                                                                                                                        \
		}                                                                                                                                  \
		field_name = valueArg;                                                                                                             \
		emit_changed();                                                                                                                    \
	}

#define SET_INSPECTOR_TOGGLE( methodName, methodSymbol, field_name )                                                                       \
	void Terrain3DErosionSettings::methodSymbol( bool bValueArg ) {                                                                        \
		if ( field_name == bValueArg ) {                                                                                                   \
			return;                                                                                                                        \
		}                                                                                                                                  \
		field_name = bValueArg;                                                                                                            \
		emit_changed();                                                                                                                    \
		notify_property_list_changed();                                                                                                    \
	}

// Normalization rules remain explicit rather than becoming assignment policies.
void Terrain3DErosionSettings::setResolutionScale( const int32_t nValue ) {
	const int32_t value = t3dErosionSettingsSanitizeResolutionScale( nValue );
	if ( m_config.m_nResolutionScale != value ) {
		m_config.m_nResolutionScale = value;
		emit_changed();
	}
}

void Terrain3DErosionSettings::setBoundaryMode( const t3dBoundaryMode_t eValue ) {
	const t3dBoundaryMode_t value =
	        ( t3dBoundaryMode_t )( CLAMP( ( int32_t )( eValue ), ( int32_t )( BOUNDARY_CLOSED ), ( int32_t )( BOUNDARY_OPEN ) ) );
	if ( m_config.m_boundaryMode != value ) {
		m_config.m_boundaryMode = value;
		emit_changed();
	}
}

void Terrain3DErosionSettings::setWindDirection( const Vector2 &vValue ) {
	const Vector2 value = vValue.length_squared() > CMP_EPSILON ? vValue.normalized() : Vector2( 1.0f, 0.0f );
	if ( !m_config.m_windDirection.is_equal_approx( value ) ) {
		m_config.m_windDirection = value;
		emit_changed();
	}
}

void Terrain3DErosionSettings::setWaveDirection( const Vector2 &vValue ) {
	const Vector2 value = vValue.length_squared() > CMP_EPSILON ? vValue.normalized() : Vector2( 1.0f, 0.0f );
	if ( !m_config.m_waveDirection.is_equal_approx( value ) ) {
		m_config.m_waveDirection = value;
		emit_changed();
	}
}

#define T3D_EROSION_SETTER_CLAMPED( type, symbol, name, field, minimum, maximum ) \
	SET_CLAMPED( name, set##symbol, field, type, minimum, maximum )
#define T3D_EROSION_SETTER_VALUE( type, symbol, name, field, minimum, maximum ) SET_VALUE( name, set##symbol, field, type )
#define T3D_EROSION_SETTER_REF( type, symbol, name, field, minimum, maximum ) SET_REF_VALUE( name, set##symbol, field, type )
#define T3D_EROSION_SETTER_TOGGLE( type, symbol, name, field, minimum, maximum ) SET_INSPECTOR_TOGGLE( name, set##symbol, field )
#define T3D_EROSION_SETTER_CURVE( type, symbol, name, field, minimum, maximum ) \
	void Terrain3DErosionSettings::set##symbol( const type &value ) { setCurve( field, value ); }
#define T3D_EROSION_SETTER_CUSTOM_VALUE( type, symbol, name, field, minimum, maximum )
#define T3D_EROSION_SETTER_CUSTOM_REF( type, symbol, name, field, minimum, maximum )
#define T3D_EROSION_GROUP( name, prefix )
#define T3D_EROSION_PROPERTY( policy, type, symbol, name, field, minimum, maximum, variant, hint, hint_string ) \
	T3D_EROSION_SETTER_##policy( type, symbol, name, field, minimum, maximum )
#include "authoring/erosion/terrain_3d_erosion_properties.inc"
#undef T3D_EROSION_PROPERTY
#undef T3D_EROSION_GROUP
#undef T3D_EROSION_SETTER_CUSTOM_REF
#undef T3D_EROSION_SETTER_CUSTOM_VALUE
#undef T3D_EROSION_SETTER_CURVE
#undef T3D_EROSION_SETTER_TOGGLE
#undef T3D_EROSION_SETTER_REF
#undef T3D_EROSION_SETTER_VALUE
#undef T3D_EROSION_SETTER_CLAMPED

#undef SET_INSPECTOR_TOGGLE
#undef SET_REF_VALUE
#undef SET_VALUE
#undef SET_CLAMPED

// Inspector dependencies are data, not a branch for every property.  Literal
// names keep the table independent of Godot object initialization order.
typedef struct t3dErosionPropertyRule_s {
	const char *m_pName;
	bool t3dErosionConfigSnapshot_t::*m_pEnabled;
} t3dErosionPropertyRule_t;

void Terrain3DErosionSettings::_validate_property( PropertyInfo &rProperty ) const {
	static const t3dErosionPropertyRule_t rules[] = {
		{ "height_output_strength", &t3dErosionConfigSnapshot_t::m_bApplyHeight },
		{ "minimum_height_m", &t3dErosionConfigSnapshot_t::m_bHeightRangeEnabled },
		{ "maximum_height_m", &t3dErosionConfigSnapshot_t::m_bHeightRangeEnabled },
		{ "height_range_falloff_m", &t3dErosionConfigSnapshot_t::m_bHeightRangeEnabled },
		{ "rainfall_m_per_year", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "rainfall_variation", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "evaporation_fraction", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "infiltration_fraction", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "flow_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "sediment_capacity", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "hydraulic_erosion_m_per_year", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "hydraulic_deposition_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "minimum_slope", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "storm_frequency", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "storm_intensity", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "channel_incision_strength", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "standing_water_deposition_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "sediment_sorting_enabled", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "coarse_transport_multiplier", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "sand_transport_multiplier", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "fine_transport_multiplier", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "coarse_settling_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "sand_settling_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "fine_settling_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "alluvial_fan_enabled", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "alluvial_fan_strength", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "alluvial_fan_slope_drop", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "alluvial_fan_spread", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "debris_flow_enabled", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "debris_flow_concentration_threshold", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "debris_flow_slope_threshold", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "debris_flow_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "debris_flow_deposition_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "debris_flow_runout_cells", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "snowmelt_enabled", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "snowfall_fraction", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "snow_accumulation_temperature", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "snow_melt_temperature", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "snow_melt_fraction_per_step", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "snow_sublimation_fraction", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "coarse_transport_multiplier", &t3dErosionConfigSnapshot_t::m_bSedimentSortingEnabled },
		{ "sand_transport_multiplier", &t3dErosionConfigSnapshot_t::m_bSedimentSortingEnabled },
		{ "fine_transport_multiplier", &t3dErosionConfigSnapshot_t::m_bSedimentSortingEnabled },
		{ "coarse_settling_rate", &t3dErosionConfigSnapshot_t::m_bSedimentSortingEnabled },
		{ "sand_settling_rate", &t3dErosionConfigSnapshot_t::m_bSedimentSortingEnabled },
		{ "fine_settling_rate", &t3dErosionConfigSnapshot_t::m_bSedimentSortingEnabled },
		{ "alluvial_fan_strength", &t3dErosionConfigSnapshot_t::m_bAlluvialFanEnabled },
		{ "alluvial_fan_slope_drop", &t3dErosionConfigSnapshot_t::m_bAlluvialFanEnabled },
		{ "alluvial_fan_spread", &t3dErosionConfigSnapshot_t::m_bAlluvialFanEnabled },
		{ "debris_flow_concentration_threshold", &t3dErosionConfigSnapshot_t::m_bDebrisFlowEnabled },
		{ "debris_flow_slope_threshold", &t3dErosionConfigSnapshot_t::m_bDebrisFlowEnabled },
		{ "debris_flow_rate", &t3dErosionConfigSnapshot_t::m_bDebrisFlowEnabled },
		{ "debris_flow_deposition_rate", &t3dErosionConfigSnapshot_t::m_bDebrisFlowEnabled },
		{ "debris_flow_runout_cells", &t3dErosionConfigSnapshot_t::m_bDebrisFlowEnabled },
		{ "snowfall_fraction", &t3dErosionConfigSnapshot_t::m_bSnowmeltEnabled },
		{ "snow_accumulation_temperature", &t3dErosionConfigSnapshot_t::m_bSnowmeltEnabled },
		{ "snow_melt_temperature", &t3dErosionConfigSnapshot_t::m_bSnowmeltEnabled },
		{ "snow_melt_fraction_per_step", &t3dErosionConfigSnapshot_t::m_bSnowmeltEnabled },
		{ "snow_sublimation_fraction", &t3dErosionConfigSnapshot_t::m_bSnowmeltEnabled },
		{ "physical_weathering_m_per_year", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "chemical_weathering_m_per_year", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "freeze_thaw_strength", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "talus_angle_degrees", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "thermal_creep_rate", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "weathering_slope_bias", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "chemical_moisture_exponent", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "freeze_point", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "wet_dry_weathering_m_per_cycle", &t3dErosionConfigSnapshot_t::m_bWetDryEnabled },
		{ "wet_threshold", &t3dErosionConfigSnapshot_t::m_bWetDryEnabled },
		{ "dry_threshold", &t3dErosionConfigSnapshot_t::m_bWetDryEnabled },
		{ "salinity_mask", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "salinity_world_rect", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "background_salinity", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "salt_weathering_m_per_year", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "salt_crystallization_strength", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "salt_evaporation_bias", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "salt_cohesion_loss", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "varnish_maturation_years", &t3dErosionConfigSnapshot_t::m_bExposureAgeEnabled },
		{ "exposure_reset_depth_m", &t3dErosionConfigSnapshot_t::m_bExposureAgeEnabled },
		{ "varnish_stability_slope", &t3dErosionConfigSnapshot_t::m_bExposureAgeEnabled },
		{ "varnish_oxidation_multiplier", &t3dErosionConfigSnapshot_t::m_bExposureAgeEnabled },
		{ "oxidation_rate_per_year", &t3dErosionConfigSnapshot_t::m_bOxidationEnabled },
		{ "oxidation_weakening", &t3dErosionConfigSnapshot_t::m_bOxidationEnabled },
		{ "sand_repose_angle_degrees", &t3dErosionConfigSnapshot_t::m_bDuneEnabled },
		{ "dune_avalanche_rate", &t3dErosionConfigSnapshot_t::m_bDuneEnabled },
		{ "dune_relaxation_iterations", &t3dErosionConfigSnapshot_t::m_bDuneEnabled },
		{ "dune_min_sand_depth_m", &t3dErosionConfigSnapshot_t::m_bDuneEnabled },
		{ "sea_level", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "tidal_range", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "wave_direction", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "wave_strength", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "wave_erosion_m_per_year", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "wave_deposition_rate", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "coastline_band_m", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "wave_fetch_distance_m", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "wave_undercut_bias", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "beach_smoothing", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "surface_layer_depth_m", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "surface_layer_depth_map", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "surface_layer_depth_world_rect", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "bedrock_hardness", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "bedrock_hardness_map", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "bedrock_hardness_world_rect", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "stratigraphy_transition_width_m", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "preserve_control_flags", &t3dErosionConfigSnapshot_t::m_bMaterialOutputEnabled },
		{ "resolve_auto_materials", &t3dErosionConfigSnapshot_t::m_bMaterialOutputEnabled },
		{ "material_transition_strength", &t3dErosionConfigSnapshot_t::m_bMaterialOutputEnabled },
		{ "material_transition_threshold", &t3dErosionConfigSnapshot_t::m_bMaterialOutputEnabled },
		{ "material_transition_depth_m", &t3dErosionConfigSnapshot_t::m_bMaterialOutputEnabled },
		{ "oxidation_tint", &t3dErosionConfigSnapshot_t::m_bOxidationColorEnabled },
		{ "oxidation_tint_strength", &t3dErosionConfigSnapshot_t::m_bOxidationColorEnabled },
		{ "rainfall_curve", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "weathering_curve", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "oxidation_curve", &t3dErosionConfigSnapshot_t::m_bOxidationEnabled },
		{ "wind_curve", &t3dErosionConfigSnapshot_t::m_bWindEnabled },
		{ "wave_curve", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "sea_level_curve", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "traffic_curve", &t3dErosionConfigSnapshot_t::m_bTrafficEnabled },
		{ "snowmelt_curve", &t3dErosionConfigSnapshot_t::m_bSnowmeltEnabled },
		{ "salt_weathering_curve", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "debris_flow_curve", &t3dErosionConfigSnapshot_t::m_bDebrisFlowEnabled },
	};
	const StringName name = rProperty.name;
	for ( size_t i = 0; i < sizeof( rules ) / sizeof( rules[0] ); ++i ) {
		const t3dErosionPropertyRule_t &rRule = rules[i];
		if ( !( m_config.*rRule.m_pEnabled ) && name == StringName( rRule.m_pName ) ) {
			rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
			return;
		}
	}

	// Prefix-gated groups exclude their own enable switch, as before.
	if ( !m_config.m_bWindEnabled && name != StringName( "wind_enabled" ) && name.begins_with( "wind_" ) ) {
		rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
	} else if ( !m_config.m_bDesertPavementEnabled && name.begins_with( "pavement_" ) ) {
		rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
	} else if ( !m_config.m_bTrafficEnabled && name != StringName( "traffic_enabled" ) && name.begins_with( "traffic_" ) ) {
		rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void Terrain3DErosionSettings::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( BOUNDARY_CLOSED );
	BIND_ENUM_CONSTANT( BOUNDARY_OPEN );

#define T3D_EROSION_GROUP( name, prefix ) ADD_GROUP( name, prefix );
#define T3D_EROSION_PROPERTY( policy, type, symbol, name, field, minimum, maximum, variant, hint, hint_string ) \
	T3D_BIND_PROPERTY( Terrain3DErosionSettings, set##symbol, get##symbol, "value", \
	        PropertyInfo( variant, #name, hint, hint_string ), "set_" #name, "get_" #name );
#include "authoring/erosion/terrain_3d_erosion_properties.inc"
#undef T3D_EROSION_PROPERTY
#undef T3D_EROSION_GROUP
}
