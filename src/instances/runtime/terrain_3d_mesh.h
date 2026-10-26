// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_MESH_H
#define TERRAIN3D_MESH_H

#include "assets/resources/terrain_3d_mesh_asset.h"
#include "instances/runtime/terrain_3d_world_instance_3d.h"

// An authored static scene mesh. The inherited MeshInstance3D retains the
// Godot editor/fallback visual; WORLD takes ownership through its existing
// conversion registry after a successful native publication.
class Terrain3DMesh : public Terrain3DWorldInstance3D
{
    GDCLASS( Terrain3DMesh, Terrain3DWorldInstance3D );

private:
    godot::Ref< Terrain3DMeshAsset > m_meshAsset;
    int32_t m_iPreviewLod = 0;

protected:
    static void _bind_methods( void );

public:
    Terrain3DMesh( void ) = default;
    ~Terrain3DMesh( void ) = default;

    void setMeshAsset( const godot::Ref< Terrain3DMeshAsset > &rAsset );
    godot::Ref< Terrain3DMeshAsset > getMeshAsset( void ) const;
    void setPreviewLod( int32_t iLod );
    int32_t getPreviewLod( void ) const;
    void refreshMesh( void );
    int32_t getAvailableLodCount( void ) const;
};

#endif // TERRAIN3D_MESH_H
