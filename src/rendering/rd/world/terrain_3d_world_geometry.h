// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_GEOMETRY_H
#define TERRAIN3D_WORLD_GEOMETRY_H

#include "rendering/rd/world/terrain_3d_world_contract.h"

#include <stdint.h>

struct T3DWorldPackedClusterTriangle {
	uint32_t nLocalIndices;
};

struct T3DWorldMeshRecord {
	uint32_t nClusterOffset;
	uint32_t nClusterCount;
	uint32_t nLodOffset;
	uint32_t nLodCount;
	uint32_t nMaterialBase;
	uint32_t nFlags;
	uint32_t nReserved0;
	uint32_t nReserved1;
};

struct T3DWorldMeshLodRecord {
	uint32_t nClusterOffset;
	uint32_t nClusterCount;
	float fMinDistance;
	float fMaxDistance;
	float fScreenError;
	uint32_t nFlags;
	uint32_t nReserved0;
	uint32_t nReserved1;
};

struct T3DWorldClusterRecord {
	float vCenter[3];
	float fRadius;
	float vConeAxis[3];
	float fConeCutoff;
	uint32_t nVertexIndexOffset;
	uint32_t nTriangleOffset;
	uint16_t nVertexCount;
	uint16_t nTriangleCount;
	uint16_t nLod;
	uint16_t nTier;
	uint32_t nMaterialId;
	uint32_t nPackedUvMin;
	uint32_t nPackedUvMax;
	uint32_t nTexelDensity;
};

static_assert( sizeof( t3dWorldVertex_t ) == 32, "t3dWorldVertex_t ABI mismatch" );
static_assert( sizeof( T3DWorldPackedClusterTriangle ) == 4, "T3DWorldPackedClusterTriangle ABI mismatch" );
static_assert( sizeof( T3DWorldMeshRecord ) == 32, "T3DWorldMeshRecord ABI mismatch" );
static_assert( sizeof( T3DWorldMeshLodRecord ) == 32, "T3DWorldMeshLodRecord ABI mismatch" );
static_assert( sizeof( T3DWorldClusterRecord ) == 64, "T3DWorldClusterRecord ABI mismatch" );

static const uint32_t T3D_WORLD_GEOMETRY_SIGNATURE_VERSION = 1u;

enum T3DWorldGeometryCookResult : uint32_t {
	T3D_WORLD_GEOMETRY_COOK_OK = 0,
	T3D_WORLD_GEOMETRY_COOK_INVALID_INPUT,
	T3D_WORLD_GEOMETRY_COOK_CAPACITY_EXCEEDED,
	T3D_WORLD_GEOMETRY_COOK_NUMERIC_OVERFLOW,
	T3D_WORLD_GEOMETRY_COOK_INTERNAL_ERROR
};

struct T3DWorldGeometrySourceLod {
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

struct T3DWorldGeometrySourceMesh {
	const t3dWorldVertex_t *pVertices;
	const T3DWorldGeometrySourceLod *pLods;
	uint32_t nVertexCount;
	uint32_t nLodCount;
	uint32_t nMaterialBase;
	uint32_t nFlags;
};

struct T3DWorldGeometryDatabaseBuffers {
	t3dWorldVertex_t *pVertexHeap;
	uint32_t nVertexCapacity;
	uint32_t *pClusterVertexIndexHeap;
	uint32_t nClusterVertexIndexCapacity;
	T3DWorldPackedClusterTriangle *pTriangleHeap;
	uint32_t nTriangleCapacity;
	T3DWorldMeshRecord *pMeshRecords;
	uint32_t nMeshCapacity;
	T3DWorldMeshLodRecord *pLodRecords;
	uint32_t nLodCapacity;
	T3DWorldClusterRecord *pClusterRecords;
	t3dWorldCluster_t *pWorldClusterRecords;
	t3dWorldClusterVtMetadata_t *pWorldClusterVtMetadata;
	uint32_t nClusterCapacity;
};

struct T3DWorldGeometryDatabase {
	T3DWorldGeometryDatabaseBuffers Buffers;
	uint32_t nVertexCount;
	uint32_t nClusterVertexIndexCount;
	uint32_t nTriangleCount;
	uint32_t nMeshCount;
	uint32_t nLodCount;
	uint32_t nClusterCount;
	uint64_t nContentSignature;
};

struct T3DWorldGeometryCookedMeshSpan {
	uint32_t nMeshId;
	uint32_t nVertexOffset;
	uint32_t nVertexCount;
	uint32_t nLodOffset;
	uint32_t nLodCount;
	uint32_t nClusterOffset;
	uint32_t nClusterCount;
	uint32_t nReserved;
};

struct T3DWorldGeometryCookStats {
	uint64_t nSourceTriangleCount;
	uint64_t nClusterTriangleSlotCount;
	uint64_t nClusterVertexReferenceCount;
	uint32_t nClusterCount;
	uint32_t nMinimumClusterTriangleCount;
	uint32_t nMaximumClusterTriangleCount;
	uint32_t nReserved;
};

void t3d_world_geometry_database_initialize_cold( T3DWorldGeometryDatabase &rDatabase, const T3DWorldGeometryDatabaseBuffers &pBuffers );

T3DWorldGeometryCookResult t3d_world_geometry_append_mesh_cold( T3DWorldGeometryDatabase &rDatabase,
        const T3DWorldGeometrySourceMesh &pSource,
        T3DWorldGeometryCookedMeshSpan &rSpan,
        T3DWorldGeometryCookStats &rStats );

uint64_t t3d_world_geometry_compute_signature_reference_debug( const T3DWorldGeometryDatabase &pDatabase );

bool t3d_world_geometry_validate_database_reference_debug( const T3DWorldGeometryDatabase &pDatabase );

bool t3d_world_geometry_validate_source_reconstruction_reference_debug(
        const T3DWorldGeometryDatabase &pDatabase, const T3DWorldGeometrySourceMesh &pSource, const T3DWorldGeometryCookedMeshSpan &pSpan );

bool t3d_world_geometry_reconstruct_triangle_reference_debug( const T3DWorldGeometryDatabase &pDatabase,
        uint32_t pClusterId,
        uint32_t pTriangleIndex,
        uint32_t &rGlobalVertex0,
        uint32_t &rGlobalVertex1,
        uint32_t &rGlobalVertex2 );

uint32_t t3d_world_geometry_pack_local_triangle( uint32_t pLocal0, uint32_t pLocal1, uint32_t pLocal2 );
void t3d_world_geometry_unpack_local_triangle( uint32_t pPacked, uint32_t &rLocal0, uint32_t &rLocal1, uint32_t &rLocal2 );

float t3d_world_geometry_unpack_texel_density( uint32_t pPacked );
float t3d_world_geometry_cluster_fill_ratio( const T3DWorldGeometryCookStats &pStats );

#endif // TERRAIN3D_WORLD_GEOMETRY_H
