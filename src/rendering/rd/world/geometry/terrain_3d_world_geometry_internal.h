// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_GEOMETRY_INTERNAL_H
#define TERRAIN3D_WORLD_GEOMETRY_INTERNAL_H

#include "Terrain3DTypes.h"

#include "rendering/rd/world/geometry/terrain_3d_world_geometry.h"

// t3dWorldGeometryDerivedCluster_s centralized in Terrain3DTypes.h.


uint32_t t3d_world_geometry_float_bits( float pValue );
bool t3d_world_geometry_finite( float pValue );
float t3d_world_geometry_length3( float pX, float pY, float pZ );

bool t3d_world_geometry_get_triangle_classification( const t3dWorldGeometrySourceMesh_t &pSource,
        const t3dWorldGeometrySourceLod_t &pLod,
        uint32_t pTriangle,
        uint32_t &rMaterialId,
        uint32_t &rTier );

bool t3d_world_geometry_validate_source( const t3dWorldGeometrySourceMesh_t &pSource );

bool t3d_world_geometry_compute_derived_cluster( const t3dWorldVertex_t *pVertices,
        uint32_t pVertexCount,
        const uint32_t *pClusterVertexIndices,
        uint32_t pClusterVertexCount,
        const t3dWorldPackedClusterTriangle_t *pTriangles,
        uint32_t pTriangleCount,
        t3dWorldGeometryDerivedCluster_t &rDerived );

bool t3d_world_geometry_database_buffers_valid( const t3dWorldGeometryDatabaseBuffers_t &pBuffers );

#endif // TERRAIN3D_WORLD_GEOMETRY_INTERNAL_H
