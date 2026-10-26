// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_GEOMETRY_INTERNAL_H
#define TERRAIN3D_RENDER_WORLD_GEOMETRY_INTERNAL_H

#include "rendering/rd/render_world/terrain_3d_render_world_geometry.h"

struct T3DRwGeometryDerivedCluster {
	float vCenter[3];
	float fRadius;
	float vConeAxis[3];
	float fConeCutoff;
	uint32_t nPackedUvMin;
	uint32_t nPackedUvMax;
	uint32_t nTexelDensity;
};

uint32_t t3d_rw_geometry_float_bits( float pValue );
bool t3d_rw_geometry_finite( float pValue );
float t3d_rw_geometry_length3( float pX, float pY, float pZ );
float t3d_rw_geometry_unpack_half( uint16_t nHalf );

bool t3d_rw_geometry_get_triangle_classification( const T3DGeometrySourceMesh &pSource,
        const T3DGeometrySourceLod &pLod,
        uint32_t pTriangle,
        uint32_t &rMaterialId,
        uint32_t &rTier );

bool t3d_rw_geometry_validate_source( const T3DGeometrySourceMesh &pSource );

bool t3d_rw_geometry_compute_derived_cluster( const T3DPrototypeVertex *pVertices,
        uint32_t pVertexCount,
        const uint32_t *pClusterVertexIndices,
        uint32_t pClusterVertexCount,
        const T3DPackedClusterTriangle *pTriangles,
        uint32_t pTriangleCount,
        T3DRwGeometryDerivedCluster &rDerived );

bool t3d_rw_geometry_database_buffers_valid( const T3DGeometryDatabaseBuffers &pBuffers );

#endif // TERRAIN3D_RENDER_WORLD_GEOMETRY_INTERNAL_H
