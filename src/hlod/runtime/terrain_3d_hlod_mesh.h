// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_HLOD_MESH_H
#define TERRAIN3D_HLOD_MESH_H

#include <godot_cpp/classes/node3d.hpp>

// Editor-facing HLOD group. The node owns no runtime draw RID or baked GPU
// payload: the existing HLOD baker, streamer and WORLD cache own those stages.
class Terrain3DHLODMesh : public godot::Node3D
{
    GDCLASS( Terrain3DHLODMesh, godot::Node3D );

private:
    bool m_bEnabledForBake = true;
    int32_t m_nMinimumHlodLevel = 1;

protected:
    static void _bind_methods( void );

public:
    Terrain3DHLODMesh( void ) = default;
    ~Terrain3DHLODMesh( void ) = default;

    void setEnabledForBake( bool bEnabled );
    bool isEnabledForBake( void ) const;
    void setMinimumHlodLevel( int32_t nLevel );
    int32_t getMinimumHlodLevel( void ) const;
};

#endif // TERRAIN3D_HLOD_MESH_H
