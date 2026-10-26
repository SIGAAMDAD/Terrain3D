// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "hlod/runtime/terrain_3d_hlod_mesh.h"
#include "integration/godot/terrain_3d_binding.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void Terrain3DHLODMesh::_bind_methods( void )
{
    ADD_GROUP( "HLOD", "hlod_" );
    T3D_BIND_PROPERTY( Terrain3DHLODMesh, setEnabledForBake, isEnabledForBake, "enabled_for_bake",
        PropertyInfo( Variant::BOOL, "hlod_enabled_for_bake" ),
        "set_enabled_for_bake", "is_enabled_for_bake" );
    T3D_BIND_PROPERTY( Terrain3DHLODMesh, setMinimumHlodLevel, getMinimumHlodLevel, "minimum_level",
        PropertyInfo( Variant::INT, "hlod_minimum_level", PROPERTY_HINT_RANGE, "1,6,1" ),
        "set_minimum_hlod_level", "get_minimum_hlod_level" );
    T3D_BIND_PROPERTY( Terrain3DHLODMesh, setLayerSettings, getLayerSettings, "settings",
        PropertyInfo( Variant::OBJECT, "hlod_layer_settings", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DHLODLayerSettings" ),
        "set_layer_settings", "get_layer_settings" );
}

void Terrain3DHLODMesh::setEnabledForBake( bool bEnabled )
{
    m_bEnabledForBake = bEnabled;
}

bool Terrain3DHLODMesh::isEnabledForBake( void ) const
{
    return m_bEnabledForBake;
}

void Terrain3DHLODMesh::setMinimumHlodLevel( int32_t nLevel )
{
    m_nMinimumHlodLevel = CLAMP( nLevel, 1, 6 );
}

int32_t Terrain3DHLODMesh::getMinimumHlodLevel( void ) const
{
    return m_nMinimumHlodLevel;
}

void Terrain3DHLODMesh::setLayerSettings( const godot::Ref< Terrain3DHLODLayerSettings > &rSettings )
{
    m_layerSettings = rSettings;
}

godot::Ref< Terrain3DHLODLayerSettings > Terrain3DHLODMesh::getLayerSettings( void ) const
{
    return m_layerSettings;
}
