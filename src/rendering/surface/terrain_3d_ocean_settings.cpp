// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/surface/terrain_3d_ocean_settings.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>

#define T3D_OCEAN_SET_CLAMPED( methodSymbol, fieldName, minValue, maxValue )                                                               \
	void Terrain3DOceanSettings::methodSymbol( const real_t fValue ) {                                                                       \
		const float value = ( float )( CLAMP( fValue, ( real_t )( minValue ), ( real_t )( maxValue ) ) );                                     \
		if ( fieldName == value ) {                                                                                                           \
			return;                                                                                                                            \
		}                                                                                                                                     \
		fieldName = value;                                                                                                                    \
		emit_changed();                                                                                                                       \
	}

T3D_OCEAN_SET_CLAMPED( setSeaLevel, m_state.m_fSeaLevel, -50.0, 50.0 )
T3D_OCEAN_SET_CLAMPED( setVisibleDepth, m_state.m_fVisibleDepth, 0.001, 10000.0 )
T3D_OCEAN_SET_CLAMPED( setDepthCurve, m_state.m_fDepthCurve, 0.001, 5.0 )
T3D_OCEAN_SET_CLAMPED( setShoreBlend, m_state.m_fShoreBlend, 0.0, 10.0 )
T3D_OCEAN_SET_CLAMPED( setRefractionStrength, m_state.m_fRefractionStrength, 0.0, 0.2 )
T3D_OCEAN_SET_CLAMPED( setTimeScale, m_state.m_fTimeScale, 0.0, 64.0 )
T3D_OCEAN_SET_CLAMPED( setHeightScale, m_state.m_fHeightScale, 0.0, 16.0 )
T3D_OCEAN_SET_CLAMPED( setOceanScale, m_state.m_fOceanScale, 0.1, 16.0 )
T3D_OCEAN_SET_CLAMPED( setFresnelScale, m_state.m_fFresnelScale, 0.0, 1.0 )
T3D_OCEAN_SET_CLAMPED( setFresnelPower, m_state.m_fFresnelPower, 0.0, 10.0 )
T3D_OCEAN_SET_CLAMPED( setSpecular, m_state.m_fSpecular, 0.0, 10.0 )
T3D_OCEAN_SET_CLAMPED( setFoamHeight, m_state.m_fFoamHeight, 0.0, 5.0 )
T3D_OCEAN_SET_CLAMPED( setFoamTopIntensity, m_state.m_fFoamTopIntensity, 0.1, 100.0 )
T3D_OCEAN_SET_CLAMPED( setFoamDistance, m_state.m_fFoamDistance, 0.1, 16384.0 )
T3D_OCEAN_SET_CLAMPED( setFoamShoreIntensity, m_state.m_fFoamShoreIntensity, 0.1, 100.0 )
T3D_OCEAN_SET_CLAMPED( setFoamNoiseScale, m_state.m_fFoamNoiseScale, 0.1, 5.0 )
T3D_OCEAN_SET_CLAMPED( setHeightScattering, m_state.m_fHeightScattering, 0.0, 15.0 )
T3D_OCEAN_SET_CLAMPED( setHeightThreshold, m_state.m_fHeightThreshold, 0.0, 1.0 )
T3D_OCEAN_SET_CLAMPED( setHeightAngleThreshold, m_state.m_fHeightAngleThreshold, 0.0, 1.0 )
T3D_OCEAN_SET_CLAMPED( setFresnelScattering, m_state.m_fFresnelScattering, 0.0, 70.0 )
T3D_OCEAN_SET_CLAMPED( setDirectScattering, m_state.m_fDirectScattering, 0.0, 70.0 )

#undef T3D_OCEAN_SET_CLAMPED

void Terrain3DOceanSettings::setWaterColor( const Color &rColor ) {
	const Color current = getWaterColor();
	if ( current == rColor ) {
		return;
	}
	m_state.m_szWaterColor[0] = ( float )( rColor.r );
	m_state.m_szWaterColor[1] = ( float )( rColor.g );
	m_state.m_szWaterColor[2] = ( float )( rColor.b );
	m_state.m_szWaterColor[3] = ( float )( rColor.a );
	emit_changed();
}

void Terrain3DOceanSettings::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "set_sea_level", "value" ), &Terrain3DOceanSettings::setSeaLevel );
	ClassDB::bind_method( D_METHOD( "get_sea_level" ), &Terrain3DOceanSettings::getSeaLevel );
	ClassDB::bind_method( D_METHOD( "set_water_color", "color" ), &Terrain3DOceanSettings::setWaterColor );
	ClassDB::bind_method( D_METHOD( "get_water_color" ), &Terrain3DOceanSettings::getWaterColor );
	ClassDB::bind_method( D_METHOD( "set_visible_depth", "value" ), &Terrain3DOceanSettings::setVisibleDepth );
	ClassDB::bind_method( D_METHOD( "get_visible_depth" ), &Terrain3DOceanSettings::getVisibleDepth );
	ClassDB::bind_method( D_METHOD( "set_depth_curve", "value" ), &Terrain3DOceanSettings::setDepthCurve );
	ClassDB::bind_method( D_METHOD( "get_depth_curve" ), &Terrain3DOceanSettings::getDepthCurve );
	ClassDB::bind_method( D_METHOD( "set_shore_blend", "value" ), &Terrain3DOceanSettings::setShoreBlend );
	ClassDB::bind_method( D_METHOD( "get_shore_blend" ), &Terrain3DOceanSettings::getShoreBlend );
	ClassDB::bind_method( D_METHOD( "set_refraction_strength", "value" ), &Terrain3DOceanSettings::setRefractionStrength );
	ClassDB::bind_method( D_METHOD( "get_refraction_strength" ), &Terrain3DOceanSettings::getRefractionStrength );
	ClassDB::bind_method( D_METHOD( "set_time_scale", "value" ), &Terrain3DOceanSettings::setTimeScale );
	ClassDB::bind_method( D_METHOD( "get_time_scale" ), &Terrain3DOceanSettings::getTimeScale );
	ClassDB::bind_method( D_METHOD( "set_height_scale", "value" ), &Terrain3DOceanSettings::setHeightScale );
	ClassDB::bind_method( D_METHOD( "get_height_scale" ), &Terrain3DOceanSettings::getHeightScale );
	ClassDB::bind_method( D_METHOD( "set_ocean_scale", "value" ), &Terrain3DOceanSettings::setOceanScale );
	ClassDB::bind_method( D_METHOD( "get_ocean_scale" ), &Terrain3DOceanSettings::getOceanScale );
	ClassDB::bind_method( D_METHOD( "set_fresnel_scale", "value" ), &Terrain3DOceanSettings::setFresnelScale );
	ClassDB::bind_method( D_METHOD( "get_fresnel_scale" ), &Terrain3DOceanSettings::getFresnelScale );
	ClassDB::bind_method( D_METHOD( "set_fresnel_power", "value" ), &Terrain3DOceanSettings::setFresnelPower );
	ClassDB::bind_method( D_METHOD( "get_fresnel_power" ), &Terrain3DOceanSettings::getFresnelPower );
	ClassDB::bind_method( D_METHOD( "set_specular", "value" ), &Terrain3DOceanSettings::setSpecular );
	ClassDB::bind_method( D_METHOD( "get_specular" ), &Terrain3DOceanSettings::getSpecular );
	ClassDB::bind_method( D_METHOD( "set_foam_height", "value" ), &Terrain3DOceanSettings::setFoamHeight );
	ClassDB::bind_method( D_METHOD( "get_foam_height" ), &Terrain3DOceanSettings::getFoamHeight );
	ClassDB::bind_method( D_METHOD( "set_foam_top_intensity", "value" ), &Terrain3DOceanSettings::setFoamTopIntensity );
	ClassDB::bind_method( D_METHOD( "get_foam_top_intensity" ), &Terrain3DOceanSettings::getFoamTopIntensity );
	ClassDB::bind_method( D_METHOD( "set_foam_distance", "value" ), &Terrain3DOceanSettings::setFoamDistance );
	ClassDB::bind_method( D_METHOD( "get_foam_distance" ), &Terrain3DOceanSettings::getFoamDistance );
	ClassDB::bind_method( D_METHOD( "set_foam_shore_intensity", "value" ), &Terrain3DOceanSettings::setFoamShoreIntensity );
	ClassDB::bind_method( D_METHOD( "get_foam_shore_intensity" ), &Terrain3DOceanSettings::getFoamShoreIntensity );
	ClassDB::bind_method( D_METHOD( "set_foam_noise_scale", "value" ), &Terrain3DOceanSettings::setFoamNoiseScale );
	ClassDB::bind_method( D_METHOD( "get_foam_noise_scale" ), &Terrain3DOceanSettings::getFoamNoiseScale );
	ClassDB::bind_method( D_METHOD( "set_height_scattering", "value" ), &Terrain3DOceanSettings::setHeightScattering );
	ClassDB::bind_method( D_METHOD( "get_height_scattering" ), &Terrain3DOceanSettings::getHeightScattering );
	ClassDB::bind_method( D_METHOD( "set_height_threshold", "value" ), &Terrain3DOceanSettings::setHeightThreshold );
	ClassDB::bind_method( D_METHOD( "get_height_threshold" ), &Terrain3DOceanSettings::getHeightThreshold );
	ClassDB::bind_method( D_METHOD( "set_height_angle_threshold", "value" ), &Terrain3DOceanSettings::setHeightAngleThreshold );
	ClassDB::bind_method( D_METHOD( "get_height_angle_threshold" ), &Terrain3DOceanSettings::getHeightAngleThreshold );
	ClassDB::bind_method( D_METHOD( "set_fresnel_scattering", "value" ), &Terrain3DOceanSettings::setFresnelScattering );
	ClassDB::bind_method( D_METHOD( "get_fresnel_scattering" ), &Terrain3DOceanSettings::getFresnelScattering );
	ClassDB::bind_method( D_METHOD( "set_direct_scattering", "value" ), &Terrain3DOceanSettings::setDirectScattering );
	ClassDB::bind_method( D_METHOD( "get_direct_scattering" ), &Terrain3DOceanSettings::getDirectScattering );

	ADD_GROUP( "Water", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "sea_level", PROPERTY_HINT_RANGE, "-50.0,50.0,0.001" ), "set_sea_level", "get_sea_level" );
	ADD_PROPERTY( PropertyInfo( Variant::COLOR, "water_color" ), "set_water_color", "get_water_color" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "visible_depth", PROPERTY_HINT_RANGE, "0.001,10000.0,0.1" ), "set_visible_depth", "get_visible_depth" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "depth_curve", PROPERTY_HINT_RANGE, "0.001,5.0,0.001" ), "set_depth_curve", "get_depth_curve" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "shore_blend", PROPERTY_HINT_RANGE, "0.0,10.0,0.001" ), "set_shore_blend", "get_shore_blend" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "refraction_strength", PROPERTY_HINT_RANGE, "0.0,0.2,0.001" ), "set_refraction_strength", "get_refraction_strength" );

	ADD_GROUP( "Waves", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "time_scale", PROPERTY_HINT_RANGE, "0.0,64.0,0.01" ), "set_time_scale", "get_time_scale" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "height_scale", PROPERTY_HINT_RANGE, "0.0,16.0,0.01" ), "set_height_scale", "get_height_scale" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "ocean_scale", PROPERTY_HINT_RANGE, "0.1,16.0,0.01" ), "set_ocean_scale", "get_ocean_scale" );

	ADD_GROUP( "Reflections", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "fresnel_scale", PROPERTY_HINT_RANGE, "0.0,1.0,0.001" ), "set_fresnel_scale", "get_fresnel_scale" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "fresnel_power", PROPERTY_HINT_RANGE, "0.0,10.0,0.01" ), "set_fresnel_power", "get_fresnel_power" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "specular", PROPERTY_HINT_RANGE, "0.0,10.0,0.01" ), "set_specular", "get_specular" );

	ADD_GROUP( "Foam", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "foam_height", PROPERTY_HINT_RANGE, "0.0,5.0,0.001" ), "set_foam_height", "get_foam_height" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "foam_top_intensity", PROPERTY_HINT_RANGE, "0.1,100.0,0.01" ), "set_foam_top_intensity", "get_foam_top_intensity" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "foam_distance", PROPERTY_HINT_RANGE, "0.1,16384.0,0.1" ), "set_foam_distance", "get_foam_distance" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "foam_shore_intensity", PROPERTY_HINT_RANGE, "0.1,100.0,0.01" ), "set_foam_shore_intensity", "get_foam_shore_intensity" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "foam_noise_scale", PROPERTY_HINT_RANGE, "0.1,5.0,0.001" ), "set_foam_noise_scale", "get_foam_noise_scale" );

	ADD_GROUP( "Light Scattering", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "height_scattering", PROPERTY_HINT_RANGE, "0.0,15.0,0.01" ), "set_height_scattering", "get_height_scattering" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "height_threshold", PROPERTY_HINT_RANGE, "0.0,1.0,0.001" ), "set_height_threshold", "get_height_threshold" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "height_angle_threshold", PROPERTY_HINT_RANGE, "0.0,1.0,0.001" ), "set_height_angle_threshold", "get_height_angle_threshold" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "fresnel_scattering", PROPERTY_HINT_RANGE, "0.0,70.0,0.01" ), "set_fresnel_scattering", "get_fresnel_scattering" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "direct_scattering", PROPERTY_HINT_RANGE, "0.0,70.0,0.01" ), "set_direct_scattering", "get_direct_scattering" );
}
