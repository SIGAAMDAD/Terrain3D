// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_EROSION_SETTINGS_CLASS_H
#define TERRAIN3D_EROSION_SETTINGS_CLASS_H

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"
#include "authoring/erosion/terrain_3d_erosion_config.h"

#include <godot_cpp/classes/curve.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource.hpp>


class Terrain3DErosionSettings : public Resource {
	GDCLASS( Terrain3DErosionSettings, Resource );
	CLASS_NAME();

public:
private:
	t3dErosionConfigSnapshot_t m_config;
	Ref<Image> m_simulationMask;
	Ref<Image> m_salinityMask;
	Ref<Image> m_trafficMask;
	Ref<Image> m_surfaceLayerDepthMap;
	Ref<Image> m_bedrockHardnessMap;

	// Curves are sampled on the main thread before simulation starts.
	Ref<Curve> m_masterIntensityCurve;
	Ref<Curve> m_rainfallCurve;
	Ref<Curve> m_weatheringCurve;
	Ref<Curve> m_oxidationCurve;
	Ref<Curve> m_windCurve;
	Ref<Curve> m_waveCurve;
	Ref<Curve> m_trafficCurve;
	Ref<Curve> m_temperatureCurve;
	Ref<Curve> m_seaLevelCurve;
	Ref<Curve> m_snowmeltCurve;
	Ref<Curve> m_saltWeatheringCurve;
	Ref<Curve> m_debrisFlowCurve;

	static Ref<Curve> makeFlatCurve( float fValue, float fMin = 0.0f, float fMax = 2.0f );
	void setCurve( Ref<Curve> &rTargetCurve, const Ref<Curve> &rSourceCurve );

public:
	Terrain3DErosionSettings( void );
	~Terrain3DErosionSettings( void ) = default;

	EA_FORCE_INLINE const t3dErosionConfigSnapshot_t &getConfigSnapshot( void ) const {
		return m_config;
	}

	// One ordered property manifest supplies accessors, setter policies and bindings.
#define T3D_EROSION_ARG_CLAMPED( type ) type
#define T3D_EROSION_ARG_VALUE( type ) type
#define T3D_EROSION_ARG_TOGGLE( type ) type
#define T3D_EROSION_ARG_CUSTOM_VALUE( type ) type
#define T3D_EROSION_ARG_REF( type ) const type &
#define T3D_EROSION_ARG_CURVE( type ) const type &
#define T3D_EROSION_ARG_CUSTOM_REF( type ) const type &
#define T3D_EROSION_GROUP( name, prefix )
#define T3D_EROSION_PROPERTY( policy, type, symbol, name, field, minimum, maximum, variant, hint, hint_string ) \
	void set##symbol( T3D_EROSION_ARG_##policy( type ) value ); \
	EA_FORCE_INLINE type get##symbol( void ) const { return field; }
#include "authoring/erosion/terrain_3d_erosion_properties.inc"
#undef T3D_EROSION_PROPERTY
#undef T3D_EROSION_GROUP
#undef T3D_EROSION_ARG_CUSTOM_REF
#undef T3D_EROSION_ARG_CURVE
#undef T3D_EROSION_ARG_REF
#undef T3D_EROSION_ARG_CUSTOM_VALUE
#undef T3D_EROSION_ARG_TOGGLE
#undef T3D_EROSION_ARG_VALUE
#undef T3D_EROSION_ARG_CLAMPED

protected:
	void _validate_property( PropertyInfo &rProperty ) const;
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dBoundaryMode_t );
#endif // TERRAIN3D_EROSION_SETTINGS_CLASS_H
