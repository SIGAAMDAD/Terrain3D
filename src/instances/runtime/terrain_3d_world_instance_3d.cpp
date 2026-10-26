// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "instances/runtime/terrain_3d_world_instance_3d.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void Terrain3DWorldInstance3D::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "set_import_profile", "profile" ), &Terrain3DWorldInstance3D::setImportProfile );
	ClassDB::bind_method( D_METHOD( "get_import_profile" ), &Terrain3DWorldInstance3D::getImportProfile );
	ClassDB::bind_method( D_METHOD( "set_world_material", "material" ), &Terrain3DWorldInstance3D::setWorldMaterial );
	ClassDB::bind_method( D_METHOD( "get_world_material" ), &Terrain3DWorldInstance3D::getWorldMaterial );
	ClassDB::bind_method( D_METHOD( "set_force_native", "enabled" ), &Terrain3DWorldInstance3D::setForceNative );
	ClassDB::bind_method( D_METHOD( "is_force_native" ), &Terrain3DWorldInstance3D::isForceNative );
	ClassDB::bind_method( D_METHOD( "set_dynamic_instance", "enabled" ), &Terrain3DWorldInstance3D::setDynamicInstance );
	ClassDB::bind_method( D_METHOD( "is_dynamic_instance" ), &Terrain3DWorldInstance3D::isDynamicInstance );
	ClassDB::bind_method( D_METHOD( "set_tier_override", "tier" ), &Terrain3DWorldInstance3D::setTierOverride );
	ClassDB::bind_method( D_METHOD( "get_tier_override" ), &Terrain3DWorldInstance3D::getTierOverride );

	ADD_GROUP( "World", "world_" );
	ADD_PROPERTY(
	        PropertyInfo(
	                Variant::OBJECT, "world_import_profile", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DWorldImportProfile" ),
	        "set_import_profile",
	        "get_import_profile" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "world_material", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DWorldMaterial" ),
	        "set_world_material",
	        "get_world_material" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "world_tier_override", PROPERTY_HINT_ENUM, "Auto,World,Clutter,Foliage" ),
	        "set_tier_override",
	        "get_tier_override" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "world_force_native" ), "set_force_native", "is_force_native" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "world_dynamic_instance" ), "set_dynamic_instance", "is_dynamic_instance" );
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
