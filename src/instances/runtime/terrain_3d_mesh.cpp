// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "instances/runtime/terrain_3d_mesh.h"
#include "integration/godot/terrain_3d_binding.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>

using namespace godot;

void Terrain3DMesh::_bind_methods( void )
{
    ADD_GROUP( "Mesh Asset", "mesh_" );
    T3D_BIND_PROPERTY( Terrain3DMesh, setMeshAsset, getMeshAsset, "asset",
        PropertyInfo( Variant::OBJECT, "mesh_asset", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DMeshAsset" ),
        "set_mesh_asset", "get_mesh_asset" );
    T3D_BIND_PROPERTY( Terrain3DMesh, setPreviewLod, getPreviewLod, "preview_lod",
        PropertyInfo( Variant::INT, "mesh_preview_lod", PROPERTY_HINT_RANGE, "0,9,1" ),
        "set_preview_lod", "get_preview_lod" );
    ClassDB::bind_method( D_METHOD( "refresh_mesh" ), &Terrain3DMesh::refreshMesh );
    ClassDB::bind_method( D_METHOD( "get_available_lod_count" ), &Terrain3DMesh::getAvailableLodCount );
}

void Terrain3DMesh::setMeshAsset( const Ref< Terrain3DMeshAsset > &rAsset )
{
    const Callable refresh( this, StringName( "refresh_mesh" ) );
    if ( m_meshAsset.is_valid() && m_meshAsset->is_connected( "vt_material_changed", refresh ) )
    {
        m_meshAsset->disconnect( "vt_material_changed", refresh );
    }
    m_meshAsset = rAsset;
    if ( m_meshAsset.is_valid() && !m_meshAsset->is_connected( "vt_material_changed", refresh ) )
    {
        m_meshAsset->connect( "vt_material_changed", refresh );
    }
    refreshMesh();
}

Ref< Terrain3DMeshAsset > Terrain3DMesh::getMeshAsset( void ) const
{
    return m_meshAsset;
}

void Terrain3DMesh::setPreviewLod( int32_t iLod )
{
    const int32_t nLastAvailable = m_meshAsset.is_valid() ?
        MAX( 0, m_meshAsset->getLodCount() - 1 ) : T3D_MESH_MAX_LOD_COUNT - 1;
    const int32_t iClamped = CLAMP( iLod, 0, nLastAvailable );
    if ( m_iPreviewLod != iClamped )
    {
        m_iPreviewLod = iClamped;
        refreshMesh();
    }
}

int32_t Terrain3DMesh::getPreviewLod( void ) const
{
    return m_iPreviewLod;
}

void Terrain3DMesh::refreshMesh( void )
{
    if ( m_meshAsset.is_valid() && m_meshAsset->getLodCount() > 0 ) {
        m_iPreviewLod = MIN( m_iPreviewLod, m_meshAsset->getLodCount() - 1 );
    }
    const Ref< Mesh > mesh = m_meshAsset.is_valid() ? m_meshAsset->getMesh( m_iPreviewLod ) : Ref< Mesh >();
    set_mesh( mesh );
}

int32_t Terrain3DMesh::getAvailableLodCount( void ) const
{
    return m_meshAsset.is_valid() ? m_meshAsset->getLodCount() : 0;
}
