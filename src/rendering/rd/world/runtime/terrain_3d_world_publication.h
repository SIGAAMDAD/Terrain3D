// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_PUBLICATION_H
#define TERRAIN3D_WORLD_PUBLICATION_H

#include "Terrain3DTypes.h"

#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "rendering/rd/vt/core/terrain_3d_vt_contract.h"
#include "rendering/rd/world/core/terrain_3d_world_contract.h"

#include <EASTL/vector.h>
#include <stddef.h>
#include <stdint.h>

// One logical opaque-world instance. Mesh/HLOD/static geometry all publish this
// record; geometry kind is data and never selects a graphics submission lane.
// t3dWorldInstance_s centralized in Terrain3DTypes.h.


// Coarse visibility cell shared by ordinary instances, HLOD proxies, and
// imported static-world instances.
// t3dWorldCell_s centralized in Terrain3DTypes.h.


// Explicit HLOD publication metadata. P09 consumes this instead of rebuilding
// HLOD identity from renderer-specific draw packets.
// t3dWorldHlodRecord_s centralized in Terrain3DTypes.h.


// t3dWorldTerrainClusterBounds_s centralized in Terrain3DTypes.h.


// t3dWorldPublicationCapacity_s centralized in Terrain3DTypes.h.


class Terrain3DWorldPublication {
public:
	uint64_t m_nSourceSignature = 0u;
	uint64_t m_nGeneration = 0u;
	uint64_t m_nGeometryGeneration = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nInstanceGeneration = 0u;
	uint64_t m_nTerrainGeneration = 0u;
	uint64_t m_nStaticGeneration = 0u;
	// CPU-only provenance for the static-scene visibility handoff.
	uint64_t m_nStaticSceneProducerId = 0u;
	uint64_t m_nStaticScenePublicationGeneration = 0u;
	uint32_t m_nInstanceClusterCount = 0u;
	uint32_t m_nHlodClusterCount = 0u;
	uint32_t m_nStaticClusterCount = 0u;
	uint32_t m_nTerrainStripClusterCount = 0u;

	eastl::vector<t3dWorldVertex_t, Terrain3DEastlAllocator> m_vertices;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_clusterVertexIndices;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_triangleWords;
	eastl::vector<t3dWorldCluster_t, Terrain3DEastlAllocator> m_clusters;
	eastl::vector<t3dWorldClusterVTMetadata_t, Terrain3DEastlAllocator> m_clusterVtMetadata;
	eastl::vector<t3dWorldTerrainCluster_t, Terrain3DEastlAllocator> m_terrainStripClusters;
	eastl::vector<t3dWorldTerrainClusterBounds_t, Terrain3DEastlAllocator> m_terrainStripClusterBounds;
	eastl::vector<t3dWorldTransform_t, Terrain3DEastlAllocator> m_transforms;
	eastl::vector<t3dWorldTransform_t, Terrain3DEastlAllocator> m_previousTransforms;
	eastl::vector<t3dWorldInstance_t, Terrain3DEastlAllocator> m_instances;
	eastl::vector<t3dWorldCell_t, Terrain3DEastlAllocator> m_cells;
	eastl::vector<t3dWorldHlodRecord_t, Terrain3DEastlAllocator> m_hlodRecords;
	eastl::vector<t3dRenderingMaterialGpu_t, Terrain3DEastlAllocator> m_materials;
	// CPU-only keys for the instance-material prefix; resolve against the live VT package.
	eastl::vector<uint64_t, Terrain3DEastlAllocator> m_instanceMaterialStableKeys;
	eastl::vector<t3dWorldVisibleItem_t, Terrain3DEastlAllocator> m_visibilityCandidates;

	void bindRuntimeAllocator( const Terrain3DEastlAllocator &rAllocator );
	bool hasCapacity( const t3dWorldPublicationCapacity_t &rRequired ) const;
	bool reserveCold( const t3dWorldPublicationCapacity_t &rRequired );
	void clearForBuild( void );
	bool copyFrom( const Terrain3DWorldPublication &rOther );
	size_t getRetainedCpuBytes( void ) const;
};

static_assert( sizeof( t3dWorldInstance_t ) == 64u, "World instance publication ABI changed" );
static_assert( sizeof( t3dWorldCell_t ) == 32u, "World cell publication ABI changed" );
static_assert( sizeof( t3dWorldHlodRecord_t ) == 32u, "World HLOD publication ABI changed" );
static_assert( sizeof( t3dWorldTerrainClusterBounds_t ) == 16u, "World terrain cluster bounds ABI changed" );

#endif // TERRAIN3D_WORLD_PUBLICATION_H
