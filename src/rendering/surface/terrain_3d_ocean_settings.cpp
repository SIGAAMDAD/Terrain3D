// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "integration/godot/terrain_3d_binding.h"
#include "rendering/surface/terrain_3d_ocean_settings.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>

#define T3D_OCEAN_SET_CLAMPED( methodSymbol, fieldName, minValue, maxValue )                                           \
	void Terrain3DOceanSettings::methodSymbol( const real_t fValue ) {                                                 \
		const float value = ( float ) ( CLAMP( fValue, ( real_t ) ( minValue ), ( real_t ) ( maxValue ) ) );           \
		if ( fieldName == value ) {                                                                                    \
			return;                                                                                                    \
		}                                                                                                              \
		fieldName = value;                                                                                             \
		emit_changed();                                                                                                \
	}

T3D_OCEAN_SET_CLAMPED(
	setSeaLevel,
	m_state.m_fSeaLevel,
	-50.0,
	50.0 )
T3D_OCEAN_SET_CLAMPED(
	setVisibleDepth,
	m_state.m_fVisibleDepth,
	0.001,
	10000.0 )
T3D_OCEAN_SET_CLAMPED(
	setDepthCurve,
	m_state.m_fDepthCurve,
	0.001,
	5.0 )
T3D_OCEAN_SET_CLAMPED(
	setShoreBlend,
	m_state.m_fShoreBlend,
	0.0,
	10.0 )
T3D_OCEAN_SET_CLAMPED(
	setRefractionStrength,
	m_state.m_fRefractionStrength,
	0.0,
	0.2 )
T3D_OCEAN_SET_CLAMPED(
	setTimeScale,
	m_state.m_fTimeScale,
	0.0,
	64.0 )
T3D_OCEAN_SET_CLAMPED(
	setHeightScale,
	m_state.m_fHeightScale,
	0.0,
	16.0 )
T3D_OCEAN_SET_CLAMPED(
	setOceanScale,
	m_state.m_fOceanScale,
	0.1,
	16.0 )
T3D_OCEAN_SET_CLAMPED(
	setFresnelScale,
	m_state.m_fFresnelScale,
	0.0,
	1.0 )
T3D_OCEAN_SET_CLAMPED(
	setFresnelPower,
	m_state.m_fFresnelPower,
	0.0,
	10.0 )
T3D_OCEAN_SET_CLAMPED(
	setSpecular,
	m_state.m_fSpecular,
	0.0,
	10.0 )
T3D_OCEAN_SET_CLAMPED(
	setFoamHeight,
	m_state.m_fFoamHeight,
	0.0,
	5.0 )
T3D_OCEAN_SET_CLAMPED(
	setFoamTopIntensity,
	m_state.m_fFoamTopIntensity,
	0.1,
	100.0 )
T3D_OCEAN_SET_CLAMPED(
	setFoamDistance,
	m_state.m_fFoamDistance,
	0.1,
	16384.0 )
T3D_OCEAN_SET_CLAMPED(
	setFoamShoreIntensity,
	m_state.m_fFoamShoreIntensity,
	0.1,
	100.0 )
T3D_OCEAN_SET_CLAMPED(
	setFoamNoiseScale,
	m_state.m_fFoamNoiseScale,
	0.1,
	5.0 )
T3D_OCEAN_SET_CLAMPED(
	setHeightScattering,
	m_state.m_fHeightScattering,
	0.0,
	15.0 )
T3D_OCEAN_SET_CLAMPED(
	setHeightThreshold,
	m_state.m_fHeightThreshold,
	0.0,
	1.0 )
T3D_OCEAN_SET_CLAMPED(
	setHeightAngleThreshold,
	m_state.m_fHeightAngleThreshold,
	0.0,
	1.0 )
T3D_OCEAN_SET_CLAMPED(
	setFresnelScattering,
	m_state.m_fFresnelScattering,
	0.0,
	70.0 )
T3D_OCEAN_SET_CLAMPED(
	setDirectScattering,
	m_state.m_fDirectScattering,
	0.0,
	70.0 )

#undef T3D_OCEAN_SET_CLAMPED

void Terrain3DOceanSettings::setWaterColor(
	const Color &rColor ) {
	const Color current = getWaterColor();
	if ( current == rColor ) {
		return;
	}
	m_state.m_szWaterColor[0] = ( float ) ( rColor.r );
	m_state.m_szWaterColor[1] = ( float ) ( rColor.g );
	m_state.m_szWaterColor[2] = ( float ) ( rColor.b );
	m_state.m_szWaterColor[3] = ( float ) ( rColor.a );
	emit_changed();
}

void Terrain3DOceanSettings::_bind_methods(
	void ) {
	ADD_GROUP( "Water", "" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setSeaLevel,
					   getSeaLevel,
					   "value",
					   PropertyInfo( Variant::FLOAT, "sea_level", PROPERTY_HINT_RANGE, "-50.0,50.0,0.001" ),
					   "set_sea_level",
					   "get_sea_level" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setWaterColor,
					   getWaterColor,
					   "color",
					   PropertyInfo( Variant::COLOR, "water_color" ),
					   "set_water_color",
					   "get_water_color" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setVisibleDepth,
					   getVisibleDepth,
					   "value",
					   PropertyInfo( Variant::FLOAT, "visible_depth", PROPERTY_HINT_RANGE, "0.001,10000.0,0.1" ),
					   "set_visible_depth",
					   "get_visible_depth" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setDepthCurve,
					   getDepthCurve,
					   "value",
					   PropertyInfo( Variant::FLOAT, "depth_curve", PROPERTY_HINT_RANGE, "0.001,5.0,0.001" ),
					   "set_depth_curve",
					   "get_depth_curve" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setShoreBlend,
					   getShoreBlend,
					   "value",
					   PropertyInfo( Variant::FLOAT, "shore_blend", PROPERTY_HINT_RANGE, "0.0,10.0,0.001" ),
					   "set_shore_blend",
					   "get_shore_blend" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setRefractionStrength,
					   getRefractionStrength,
					   "value",
					   PropertyInfo( Variant::FLOAT, "refraction_strength", PROPERTY_HINT_RANGE, "0.0,0.2,0.001" ),
					   "set_refraction_strength",
					   "get_refraction_strength" );

	ADD_GROUP( "Waves", "" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setTimeScale,
					   getTimeScale,
					   "value",
					   PropertyInfo( Variant::FLOAT, "time_scale", PROPERTY_HINT_RANGE, "0.0,64.0,0.01" ),
					   "set_time_scale",
					   "get_time_scale" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setHeightScale,
					   getHeightScale,
					   "value",
					   PropertyInfo( Variant::FLOAT, "height_scale", PROPERTY_HINT_RANGE, "0.0,16.0,0.01" ),
					   "set_height_scale",
					   "get_height_scale" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setOceanScale,
					   getOceanScale,
					   "value",
					   PropertyInfo( Variant::FLOAT, "ocean_scale", PROPERTY_HINT_RANGE, "0.1,16.0,0.01" ),
					   "set_ocean_scale",
					   "get_ocean_scale" );

	ADD_GROUP( "Reflections", "" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setFresnelScale,
					   getFresnelScale,
					   "value",
					   PropertyInfo( Variant::FLOAT, "fresnel_scale", PROPERTY_HINT_RANGE, "0.0,1.0,0.001" ),
					   "set_fresnel_scale",
					   "get_fresnel_scale" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setFresnelPower,
					   getFresnelPower,
					   "value",
					   PropertyInfo( Variant::FLOAT, "fresnel_power", PROPERTY_HINT_RANGE, "0.0,10.0,0.01" ),
					   "set_fresnel_power",
					   "get_fresnel_power" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setSpecular,
					   getSpecular,
					   "value",
					   PropertyInfo( Variant::FLOAT, "specular", PROPERTY_HINT_RANGE, "0.0,10.0,0.01" ),
					   "set_specular",
					   "get_specular" );

	ADD_GROUP( "Foam", "" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setFoamHeight,
					   getFoamHeight,
					   "value",
					   PropertyInfo( Variant::FLOAT, "foam_height", PROPERTY_HINT_RANGE, "0.0,5.0,0.001" ),
					   "set_foam_height",
					   "get_foam_height" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setFoamTopIntensity,
					   getFoamTopIntensity,
					   "value",
					   PropertyInfo( Variant::FLOAT, "foam_top_intensity", PROPERTY_HINT_RANGE, "0.1,100.0,0.01" ),
					   "set_foam_top_intensity",
					   "get_foam_top_intensity" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setFoamDistance,
					   getFoamDistance,
					   "value",
					   PropertyInfo( Variant::FLOAT, "foam_distance", PROPERTY_HINT_RANGE, "0.1,16384.0,0.1" ),
					   "set_foam_distance",
					   "get_foam_distance" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setFoamShoreIntensity,
					   getFoamShoreIntensity,
					   "value",
					   PropertyInfo( Variant::FLOAT, "foam_shore_intensity", PROPERTY_HINT_RANGE, "0.1,100.0,0.01" ),
					   "set_foam_shore_intensity",
					   "get_foam_shore_intensity" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setFoamNoiseScale,
					   getFoamNoiseScale,
					   "value",
					   PropertyInfo( Variant::FLOAT, "foam_noise_scale", PROPERTY_HINT_RANGE, "0.1,5.0,0.001" ),
					   "set_foam_noise_scale",
					   "get_foam_noise_scale" );

	ADD_GROUP( "Light Scattering", "" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setHeightScattering,
					   getHeightScattering,
					   "value",
					   PropertyInfo( Variant::FLOAT, "height_scattering", PROPERTY_HINT_RANGE, "0.0,15.0,0.01" ),
					   "set_height_scattering",
					   "get_height_scattering" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setHeightThreshold,
					   getHeightThreshold,
					   "value",
					   PropertyInfo( Variant::FLOAT, "height_threshold", PROPERTY_HINT_RANGE, "0.0,1.0,0.001" ),
					   "set_height_threshold",
					   "get_height_threshold" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setHeightAngleThreshold,
					   getHeightAngleThreshold,
					   "value",
					   PropertyInfo( Variant::FLOAT, "height_angle_threshold", PROPERTY_HINT_RANGE, "0.0,1.0,0.001" ),
					   "set_height_angle_threshold",
					   "get_height_angle_threshold" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setFresnelScattering,
					   getFresnelScattering,
					   "value",
					   PropertyInfo( Variant::FLOAT, "fresnel_scattering", PROPERTY_HINT_RANGE, "0.0,70.0,0.01" ),
					   "set_fresnel_scattering",
					   "get_fresnel_scattering" );
	T3D_BIND_PROPERTY( Terrain3DOceanSettings,
					   setDirectScattering,
					   getDirectScattering,
					   "value",
					   PropertyInfo( Variant::FLOAT, "direct_scattering", PROPERTY_HINT_RANGE, "0.0,70.0,0.01" ),
					   "set_direct_scattering",
					   "get_direct_scattering" );
}
