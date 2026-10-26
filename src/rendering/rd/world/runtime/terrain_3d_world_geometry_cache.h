// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_GEOMETRY_CACHE_H
#define TERRAIN3D_WORLD_GEOMETRY_CACHE_H

#include "rendering/rd/instances/terrain_3d_rd_mesh_cache.h"
#include "rendering/rd/world/runtime/terrain_3d_world_publication.h"

// Owner-thread cold geometry layout for the shared WORLD buffers. Active
// source revisions hold stable element offsets. Retired physical ranges enter
// per-stream free lists only after GPU-completion certification.
typedef struct t3dWorldResidentGeometrySpan_s
{
	uint64_t m_nSourceKey = 0u;
	uint64_t m_nContentSignature = 0u;
	uint32_t m_nPhysicalRecord = UINT32_MAX; // Alias sources share one physical WORLD geometry span.
	uint32_t m_nActiveSourceLeases = 0u; // Canonical physical record: one per active source LOD.
	uint64_t m_nRetirePublicationGeneration = 0u; // UINT64_MAX until the removing publication is committed.
	uint32_t m_nVertexBase = 0u;
	uint32_t m_nIndexBase = 0u;
	uint32_t m_nTriangleBase = 0u;
	uint32_t m_nClusterBase = 0u;
	uint32_t m_nVertexCount = 0u;
	uint32_t m_nIndexCount = 0u;
	uint32_t m_nTriangleCount = 0u;
	uint32_t m_nClusterCount = 0u;
    uint32_t m_nReservedVertexCount = 0u;
    uint32_t m_nReservedIndexCount = 0u;
    uint32_t m_nReservedTriangleCount = 0u;
    uint32_t m_nReservedClusterCount = 0u;
} t3dWorldResidentGeometrySpan_t;

// The CLUSTER stream also owns VT metadata at the identical element offset.
// A range is published to this free list only after GPU completion.
typedef struct t3dWorldGeometryFreeRange_s
{
    uint32_t m_nFirst = 0u;
    uint32_t m_nCount = 0u;
} t3dWorldGeometryFreeRange_t;

typedef struct t3dWorldGeometryRangeClaim_s
{
    uint32_t m_nStream = 0u;
    uint32_t m_nSlot = 0u;
    uint32_t m_nCount = 0u;
} t3dWorldGeometryRangeClaim_t;

// Logical source identity is independent of physical GPU geometry offsets.
// Slots are append-only within one cache epoch; a reset invalidates every handle.
typedef struct t3dWorldMeshSourceHandle_s
{
	uint32_t m_nSlot = UINT32_MAX;
	uint32_t m_nEpoch = 0u;
} t3dWorldMeshSourceHandle_t;

class Terrain3DWorldGeometryCache
{
private:
	Terrain3DWorldPublication m_geometry;
    mutable Terrain3DWorldGeometryLease m_frozenGeometry;
    mutable uint64_t m_nFrozenEpoch = 0u;
    mutable uint64_t m_nFrozenGeometryRevision = 0u;
    mutable uint64_t m_nFrozenMutationRevision = 0u;
	eastl::vector< t3dWorldResidentGeometrySpan_t, Terrain3DEastlAllocator > m_sources;
	eastl::vector< uint32_t, Terrain3DEastlAllocator > m_activeSources;
	eastl::vector< uint32_t, Terrain3DEastlAllocator > m_pendingActiveSources;
	eastl::vector< uint32_t, Terrain3DEastlAllocator > m_staticMaterialSlots;
	eastl::vector< uint32_t, Terrain3DEastlAllocator > m_pendingStaticMaterialSlots;
    // Four independent physical streams: vertices, cluster vertex indices,
    // triangle words, clusters + 1:1 VT metadata. No frame-path allocations.
    eastl::vector< t3dWorldGeometryFreeRange_t, Terrain3DEastlAllocator > m_freeRanges[4];
    eastl::vector< t3dWorldGeometryRangeClaim_t, Terrain3DEastlAllocator > m_pendingRangeClaims;
    eastl::vector< t3dWorldGeometryDirtyRange_t, Terrain3DEastlAllocator > m_dirtyRanges;
    uint64_t m_nMutationRevision = 0u;
    uint64_t m_nRecycledSpans = 0u;
    uint64_t m_nReusedGeometryBytes = 0u;
	uint32_t m_nActiveStaticSource = UINT32_MAX;
	uint64_t m_nCacheEpoch = 1u;
	uint64_t m_nGeometryRevision = 1u;
	uint64_t m_nMaterialRevision = 1u;
	uint64_t m_nCacheAdmissionFailures = 0u;
	uint64_t m_nDeduplicatedSources = 0u;
	uint64_t m_nDeduplicatedGpuBytes = 0u;
	uint64_t m_nSafeCompactionCount = 0u;
	uint64_t m_nReclaimedGeometryBytes = 0u;

	bool registerSourceCold( uint64_t nKey,
							 const t3dWorldResidentGeometrySpan_t &rInput,
							 const t3dWorldVertex_t *pVertices,
							 const uint32_t *pIndices,
							 const uint32_t *pTriangles,
							 const t3dWorldCluster_t *pClusters,
							 const t3dWorldClusterVTMetadata_t *pMetadata,
							 const t3dRenderingMaterialGpu_t *pMaterials,
							 const uint64_t *pMaterialKeys,
							 uint32_t nMaterialCount,
							 const t3dWorldResourceLimits_t &rLimits,
							 uint32_t &rRecordIndex );
	bool resolveMaterialCold( const t3dRenderingMaterialGpu_t &rMaterial,
							  uint64_t nStableKey, uint32_t nLimit, uint32_t &rSlot );

public:
	void bindRuntimeAllocator( const Terrain3DEastlAllocator &rAllocator );
	void reset( void );
	bool synchronizeCold( const t3dRdMeshSourceSnapshot_t &rSnapshot,
						 const Terrain3DWorldPublication *pStaticWorld,
						 uint64_t nStaticProducer,
						 const t3dWorldResourceLimits_t &rLimits );
	bool findActiveSpan( uint64_t nSourceKey, uint32_t &rFirstCluster, uint32_t &rClusterCount ) const;
	bool getActiveSourceHandle( uint64_t nSourceKey, t3dWorldMeshSourceHandle_t &rHandle ) const;
	bool resolveActiveSourceHandle( const t3dWorldMeshSourceHandle_t &rHandle,
		uint32_t &rFirstCluster, uint32_t &rClusterCount ) const;
	bool getStaticSpan( uint32_t &rFirstCluster, uint32_t &rClusterCount ) const;
	bool getStaticMaterialSlot( uint32_t nLocalMaterial, uint32_t &rSlot ) const;
	bool copyGeometryInto( Terrain3DWorldPublication &rPublication ) const;
	// Called after a successful owner-side WORLD publication excluding retired sources.
	void tagRetiredSourcesCold( uint64_t nPublicationGeneration );
	// Caller supplies ONLY a publication generation proven complete by an async GPU fence.
	// Rebuilds live physical spans in a fresh epoch; all old handles are invalidated.
	// Makes certified zero-lease geometry available to new admissions without
    // moving unrelated resident offsets or changing the cache epoch.
    bool releaseCompletedRetirementsCold( uint64_t nCompletedPublicationGeneration );
    bool compactAfterGpuCompletionCold( uint64_t nCompletedPublicationGeneration,
		const t3dWorldResourceLimits_t &rLimits );
	const Terrain3DWorldPublication &geometry( void ) const { return m_geometry; }
    const Terrain3DSharedWorldGeometry *getFrozenGeometryIdentity( void ) const { return m_frozenGeometry.get(); }
	uint64_t getCacheEpoch( void ) const { return m_nCacheEpoch; }
	uint64_t getGeometryRevision( void ) const { return m_nGeometryRevision; }
	uint64_t getMaterialRevision( void ) const { return m_nMaterialRevision; }
	uint64_t getAdmissionFailureCount( void ) const { return m_nCacheAdmissionFailures; }
	uint64_t getDeduplicatedSourceCount( void ) const { return m_nDeduplicatedSources; }
	uint64_t getDeduplicatedGpuBytes( void ) const { return m_nDeduplicatedGpuBytes; }
	uint64_t getSafeCompactionCount( void ) const { return m_nSafeCompactionCount; }
	uint64_t getReclaimedGeometryBytes( void ) const { return m_nReclaimedGeometryBytes; }
    uint64_t getRecycledSpanCount( void ) const { return m_nRecycledSpans; }
    uint64_t getReusedGeometryBytes( void ) const { return m_nReusedGeometryBytes; }
    uint64_t getMutationRevision( void ) const { return m_nMutationRevision; }
    uint64_t getFreeGeometryBytes( void ) const;
    uint32_t getFreeGeometryRangeCount( void ) const;
	uint32_t getActiveSourceCount( void ) const { return ( uint32_t ) m_activeSources.size() + ( m_nActiveStaticSource != UINT32_MAX ? 1u : 0u ); }
	uint32_t getTrackedSourceCount( void ) const { return ( uint32_t ) m_sources.size(); }
	uint32_t getPhysicalSourceLeaseCount( uint64_t nSourceKey ) const;
	uint64_t getQuarantinedGeometryBytes( void ) const;
	uint64_t getLiveGeometryBytes( void ) const;
	size_t getRetainedCpuBytes( void ) const;
};

#endif // TERRAIN3D_WORLD_GEOMETRY_CACHE_H
