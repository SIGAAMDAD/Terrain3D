// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_OCEAN_MATERIAL_PARAMS_H
#define TERRAIN3D_RD_OCEAN_MATERIAL_PARAMS_H

#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

inline Variant t3dRdOceanParam( const ShaderMaterial *const pMaterial, const char *const pszName ) {
	return pMaterial ? pMaterial->get_shader_parameter( StringName( pszName ) ) : Variant();
}

inline float t3dRdOceanFloatParam( const ShaderMaterial *const pMaterial, const char *const pszName, const float fDefault ) {
	const Variant value = t3dRdOceanParam( pMaterial, pszName );
	return value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT ? ( float )( value ) : fDefault;
}

inline Color t3dRdOceanColorParam( const ShaderMaterial *const pMaterial, const char *const pszName, const Color &rDefault ) {
	const Variant value = t3dRdOceanParam( pMaterial, pszName );
	if ( value.get_type() == Variant::COLOR ) {
		return ( Color )( value );
	}
	if ( value.get_type() == Variant::VECTOR3 ) {
		const Vector3 v = value;
		return Color( v.x, v.y, v.z, 1.0f );
	}
	return rDefault;
}

#endif // TERRAIN3D_RD_OCEAN_MATERIAL_PARAMS_H
