// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_GEOMETRY_H
#define TERRAIN3D_RENDER_WORLD_GEOMETRY_H

#include "rendering/rd/render_world/terrain_3d_render_world_contract.h"

#include <stdint.h>

static const uint32_t T3D_RW_GEOMETRY_SIGNATURE_VERSION = 1u;

enum T3DGeometryCookResult : uint32_t {
	T3D_RW_GEOMETRY_COOK_OK = 0,
	T3D_RW_GEOMETRY_COOK_INVALID_INPUT,
	T3D_RW_GEOMETRY_COOK_CAPACITY_EXCEEDED,
	T3D_RW_GEOMETRY_COOK_NUMERIC_OVERFLOW,
	T3D_RW_GEOMETRY_COOK_INTERNAL_ERROR
};

struct T3DGeometrySourceLod {
	const uint32_t *pIndices;
	const uint32_t *pTriangleMaterialOffsets;
	const uint8_t *pTriangleTiers;
	uint32_t nIndexCount;
	uint32_t nDefaultTier;
	float fMinDistance;
	float fMaxDistance;
	float fScreenError;
	uint32_t nFlags;
};

struct T3DGeometrySourceMesh {
	const T3DPrototypeVertex *pVertices;
	const T3DGeometrySourceLod *pLods;
	uint32_t nVertexCount;
	uint32_t nLodCount;
	uint32_t nMaterialBase;
	uint32_t nFlags;
};

struct T3DGeometryDatabaseBuffers {
	T3DPrototypeVertex *pVertexHeap;
	uint32_t nVertexCapacity;
	uint32_t *pClusterVertexIndexHeap;
	uint32_t nClusterVertexIndexCapacity;
	T3DPackedClusterTriangle *pTriangleHeap;
	uint32_t nTriangleCapacity;
	T3DMeshRecord *pMeshRecords;
	uint32_t nMeshCapacity;
	T3DMeshLodRecord *pLodRecords;
	uint32_t nLodCapacity;
	T3DClusterRecord *pClusterRecords;
	t3dWorldCluster_t *pWorldClusterRecords;
	t3dWorldClusterVtMetadata_t *pWorldClusterVtMetadata;
	uint32_t nClusterCapacity;
};

struct T3DGeometryDatabase {
	T3DGeometryDatabaseBuffers Buffers;
	uint32_t nVertexCount;
	uint32_t nClusterVertexIndexCount;
	uint32_t nTriangleCount;
	uint32_t nMeshCount;
	uint32_t nLodCount;
	uint32_t nClusterCount;
	uint64_t nContentSignature;
};

struct T3DGeometryCookedMeshSpan {
	uint32_t nMeshId;
	uint32_t nVertexOffset;
	uint32_t nVertexCount;
	uint32_t nLodOffset;
	uint32_t nLodCount;
	uint32_t nClusterOffset;
	uint32_t nClusterCount;
	uint32_t nReserved;
};

struct T3DGeometryCookStats {
	uint64_t nSourceTriangleCount;
	uint64_t nClusterTriangleSlotCount;
	uint64_t nClusterVertexReferenceCount;
	uint32_t nClusterCount;
	uint32_t nMinimumClusterTriangleCount;
	uint32_t nMaximumClusterTriangleCount;
	uint32_t nReserved;
};

void t3d_render_world_geometry_database_initialize_cold( T3DGeometryDatabase &rDatabase, const T3DGeometryDatabaseBuffers &pBuffers );

T3DGeometryCookResult t3d_render_world_geometry_append_mesh_cold( T3DGeometryDatabase &rDatabase,
        const T3DGeometrySourceMesh &pSource,
        T3DGeometryCookedMeshSpan &rSpan,
        T3DGeometryCookStats &rStats );

uint64_t t3d_render_world_geometry_compute_signature_reference_debug( const T3DGeometryDatabase &pDatabase );

bool t3d_render_world_geometry_validate_database_reference_debug( const T3DGeometryDatabase &pDatabase );

bool t3d_render_world_geometry_validate_source_reconstruction_reference_debug(
        const T3DGeometryDatabase &pDatabase, const T3DGeometrySourceMesh &pSource, const T3DGeometryCookedMeshSpan &pSpan );

bool t3d_render_world_geometry_reconstruct_triangle_reference_debug( const T3DGeometryDatabase &pDatabase,
        uint32_t pClusterId,
        uint32_t pTriangleIndex,
        uint32_t &rGlobalVertex0,
        uint32_t &rGlobalVertex1,
        uint32_t &rGlobalVertex2 );

uint32_t t3d_render_world_geometry_pack_local_triangle( uint32_t pLocal0, uint32_t pLocal1, uint32_t pLocal2 );
void t3d_render_world_geometry_unpack_local_triangle( uint32_t pPacked, uint32_t &rLocal0, uint32_t &rLocal1, uint32_t &rLocal2 );

float t3d_render_world_geometry_unpack_texel_density( uint32_t pPacked );
float t3d_render_world_geometry_cluster_fill_ratio( const T3DGeometryCookStats &pStats );

#endif // TERRAIN3D_RENDER_WORLD_GEOMETRY_H
