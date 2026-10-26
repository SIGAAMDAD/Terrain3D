// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_CLUSTER_COOKER_H
#define TERRAIN3D_WORLD_CLUSTER_COOKER_H

#include "rendering/rd/world/terrain_3d_world_contract.h"

#include <stdint.h>

enum t3dWorldClusterCookFlags_t : uint32_t {
	T3D_WORLD_CLUSTER_COOK_NONE = 0u,
	T3D_WORLD_CLUSTER_COOK_TWO_SIDED = 1u << 0u,
};

typedef struct t3dWorldClusterCookSource_s {
	const t3dWorldVertex_t *m_pVertices;
	const uint32_t *m_pIndices;
	uint32_t m_nVertexBase;
	uint32_t m_nVertexCount;
	uint32_t m_nIndexCount;
	uint32_t m_nMaterial;
	uint32_t m_nLod;
	uint32_t m_nGeometryKind;
	uint32_t m_nClusterFlags;
	uint32_t m_nCookFlags;
	uint32_t m_nSourceId;
} t3dWorldClusterCookSource_t;

typedef struct t3dWorldClusterCookBuffers_s {
	uint32_t *m_pClusterVertexIndices;
	uint32_t m_nClusterVertexIndexCapacity;
	uint32_t m_nClusterVertexIndexCount;
	uint32_t *m_pTriangleWords;
	uint32_t m_nTriangleWordCapacity;
	uint32_t m_nTriangleWordCount;
	t3dWorldCluster_t *m_pClusters;
	t3dWorldClusterVtMetadata_t *m_pVtMetadata;
	uint32_t m_nClusterCapacity;
	uint32_t m_nClusterCount;
} t3dWorldClusterCookBuffers_t;

typedef struct t3dWorldClusterCookStats_s {
	uint32_t m_nSourceTriangleCount;
	uint32_t m_nRasterTriangleCount;
	uint32_t m_nClusterCount;
	uint32_t m_nPaddedTriangleSlots;
} t3dWorldClusterCookStats_t;

bool t3dWorldCookMeshClustersCold( const t3dWorldClusterCookSource_t &rSource,
        t3dWorldClusterCookBuffers_t &rBuffers,
        t3dWorldMeshClusterSpan_t &rSpan,
        t3dWorldClusterCookStats_t &rStats );

#endif
