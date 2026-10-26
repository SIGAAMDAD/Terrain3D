// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_MESH_SOURCE_INDEX_H
#define TERRAIN3D_MESH_SOURCE_INDEX_H

#include "rendering/rd/instances/terrain_3d_rd_mesh_cache.h"

// Cold-built mapping from deduplicated mesh source identity to WORLD cluster
// spans. One index replaces O(cells * surfaces) scans during WORLD publication.
typedef struct t3dWorldMeshSourceSpan_s
{
	uint64_t m_nSourceKey = 0u;
	uint32_t m_nFirstCluster = 0u;
	uint32_t m_nClusterCount = 0u;
} t3dWorldMeshSourceSpan_t;

static_assert( sizeof( t3dWorldMeshSourceSpan_t ) == 16u, "WORLD source index ABI changed" );

class Terrain3DMeshSourceIndex
{
	eastl::vector< t3dWorldMeshSourceSpan_t, Terrain3DEastlAllocator > m_spans;
	uint64_t m_nSourceGeneration = 0u;

public:
	void bindRuntimeAllocator( const Terrain3DEastlAllocator &rAllocator );
	bool reserveCold( size_t nCount );
	bool rebuildCold( const t3dRdMeshSourceSnapshot_t &rSnapshot );
	void reset( void );
	bool findClusterSpan( uint64_t nSourceKey, uint32_t &rFirstCluster, uint32_t &rClusterCount ) const;
	EA_FORCE_INLINE size_t capacity( void ) const { return m_spans.capacity(); }
	EA_FORCE_INLINE size_t getRetainedCpuBytes( void ) const { return t3dContainerResidentBytes( m_spans ); }
	EA_FORCE_INLINE uint64_t getSourceGeneration( void ) const { return m_nSourceGeneration; }
};

#endif // TERRAIN3D_MESH_SOURCE_INDEX_H
