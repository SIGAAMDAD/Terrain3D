// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "integration/godot/terrain_3d_binding.h"
#include "instances/runtime/terrain_3d_world_instance_3d.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void Terrain3DWorldInstance3D::_bind_methods( void ) {

	ADD_GROUP( "World", "world_" );
	T3D_BIND_PROPERTY( Terrain3DWorldInstance3D, setImportProfile, getImportProfile, "profile",
	                   PropertyInfo( Variant::OBJECT, "world_import_profile", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DWorldImportProfile" ), "set_import_profile", "get_import_profile" );
	T3D_BIND_PROPERTY( Terrain3DWorldInstance3D, setWorldMaterial, getWorldMaterial, "material",
	                   PropertyInfo( Variant::OBJECT, "world_material", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DWorldMaterial" ), "set_world_material", "get_world_material" );
	T3D_BIND_PROPERTY( Terrain3DWorldInstance3D, setTierOverride, getTierOverride, "tier",
	                   PropertyInfo( Variant::INT, "world_tier_override", PROPERTY_HINT_ENUM, "Auto,World,Clutter,Foliage" ), "set_tier_override", "get_tier_override" );
	T3D_BIND_PROPERTY( Terrain3DWorldInstance3D, setForceNative, isForceNative, "enabled",
	                   PropertyInfo( Variant::BOOL, "world_force_native" ), "set_force_native", "is_force_native" );
	T3D_BIND_PROPERTY( Terrain3DWorldInstance3D, setDynamicInstance, isDynamicInstance, "enabled",
	                   PropertyInfo( Variant::BOOL, "world_dynamic_instance" ), "set_dynamic_instance", "is_dynamic_instance" );
}

void Terrain3DWorldInstance3D::setImportProfile( const Ref<Terrain3DWorldImportProfile> &rProfile ) {
	m_importProfile = rProfile;
}

void Terrain3DWorldInstance3D::setWorldMaterial( const Ref<Terrain3DWorldMaterial> &rMaterial ) {
	m_worldMaterial = rMaterial;
}

void Terrain3DWorldInstance3D::setForceNative( const bool bEnabled ) {
	m_bForceNative = bEnabled;
}

void Terrain3DWorldInstance3D::setDynamicInstance( const bool bEnabled ) {
	m_bDynamicInstance = bEnabled;
}

void Terrain3DWorldInstance3D::setTierOverride( const int32_t iTier ) {
	m_iTierOverride = iTier >= 0 && iTier <= ( int32_t )( T3D_WORLD_IMPORT_FOLIAGE ) ? iTier : ( int32_t )( T3D_WORLD_IMPORT_AUTO );
}
