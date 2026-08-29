// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "instances/terrain_3d_render_world_instance_3d.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void Terrain3DRenderWorldInstance3D::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "set_import_profile", "profile" ), &Terrain3DRenderWorldInstance3D::setImportProfile );
	ClassDB::bind_method( D_METHOD( "get_import_profile" ), &Terrain3DRenderWorldInstance3D::getImportProfile );
	ClassDB::bind_method( D_METHOD( "set_render_world_material", "material" ), &Terrain3DRenderWorldInstance3D::setRenderWorldMaterial );
	ClassDB::bind_method( D_METHOD( "get_render_world_material" ), &Terrain3DRenderWorldInstance3D::getRenderWorldMaterial );
	ClassDB::bind_method( D_METHOD( "set_force_native", "enabled" ), &Terrain3DRenderWorldInstance3D::setForceNative );
	ClassDB::bind_method( D_METHOD( "is_force_native" ), &Terrain3DRenderWorldInstance3D::isForceNative );
	ClassDB::bind_method( D_METHOD( "set_dynamic_instance", "enabled" ), &Terrain3DRenderWorldInstance3D::setDynamicInstance );
	ClassDB::bind_method( D_METHOD( "is_dynamic_instance" ), &Terrain3DRenderWorldInstance3D::isDynamicInstance );
	ClassDB::bind_method( D_METHOD( "set_tier_override", "tier" ), &Terrain3DRenderWorldInstance3D::setTierOverride );
	ClassDB::bind_method( D_METHOD( "get_tier_override" ), &Terrain3DRenderWorldInstance3D::getTierOverride );

	ADD_GROUP( "RenderWorld", "render_world_" );
	ADD_PROPERTY(
	        PropertyInfo(
	                Variant::OBJECT, "render_world_import_profile", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DRenderWorldImportProfile" ),
	        "set_import_profile",
	        "get_import_profile" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "render_world_material", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DRenderWorldMaterial" ),
	        "set_render_world_material",
	        "get_render_world_material" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "render_world_tier_override", PROPERTY_HINT_ENUM, "Auto,World,Clutter,Foliage" ),
	        "set_tier_override",
	        "get_tier_override" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "render_world_force_native" ), "set_force_native", "is_force_native" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "render_world_dynamic_instance" ), "set_dynamic_instance", "is_dynamic_instance" );
}

void Terrain3DRenderWorldInstance3D::setImportProfile( const Ref<Terrain3DRenderWorldImportProfile> &rProfile ) {
	m_importProfile = rProfile;
}

void Terrain3DRenderWorldInstance3D::setRenderWorldMaterial( const Ref<Terrain3DRenderWorldMaterial> &rMaterial ) {
	m_renderWorldMaterial = rMaterial;
}

void Terrain3DRenderWorldInstance3D::setForceNative( const bool bEnabled ) {
	m_bForceNative = bEnabled;
}

void Terrain3DRenderWorldInstance3D::setDynamicInstance( const bool bEnabled ) {
	m_bDynamicInstance = bEnabled;
}

void Terrain3DRenderWorldInstance3D::setTierOverride( const int32_t iTier ) {
	m_iTierOverride = iTier >= 0 && iTier <= ( int32_t )( T3D_RW_IMPORT_FOLIAGE ) ? iTier : ( int32_t )( T3D_RW_IMPORT_AUTO );
}
