// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_PUBLICATION_H
#define TERRAIN3D_WORLD_PUBLICATION_H

#include "Terrain3DTypes.h"

#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "rendering/rd/vt/core/terrain_3d_vt_contract.h"
#include "rendering/rd/world/core/terrain_3d_world_contract.h"

#include <EASTL/vector.h>
#include <atomic>
#include <stddef.h>
#include <stdint.h>

// Immutable publication-local journal entry. The generation is compared with the
// render-thread's last successfully uploaded WORLD geometry revision. Every
// overwritten range is re-uploaded even when the overall buffer size is stable.
typedef struct t3dWorldGeometryDirtyRange_s
{
    uint64_t m_nRevision = 0u;
    uint32_t m_nBuffer = 0u;
    uint32_t m_nFirstByte = 0u;
    uint32_t m_nByteCount = 0u;
} t3dWorldGeometryDirtyRange_t;

// One logical opaque-world instance. Mesh/HLOD/static geometry all publish this
// record; geometry kind is data and never selects a graphics submission lane.

// Coarse visibility cell shared by ordinary instances, HLOD proxies, and
// imported static-world instances.

// Explicit HLOD publication metadata. P09 consumes this instead of rebuilding
// HLOD identity from renderer-specific draw packets.

// One immutable, reference-counted payload per WORLD geometry revision.
// Publication-bank readers keep an independent lease across the owner thread's
// next admission/recycle. No engine or render-thread RID is owned here.
class Terrain3DWorldPublication;

class Terrain3DSharedWorldGeometry
{
private:
    std::atomic< uint32_t > m_nReferences{ 1u };

public:
    eastl::vector< t3dWorldVertex_t, Terrain3DEastlAllocator > m_vertices;
    eastl::vector< uint32_t, Terrain3DEastlAllocator > m_clusterVertexIndices;
    eastl::vector< uint32_t, Terrain3DEastlAllocator > m_triangleWords;
    eastl::vector< t3dWorldCluster_t, Terrain3DEastlAllocator > m_clusters;
    eastl::vector< t3dWorldClusterVTMetadata_t, Terrain3DEastlAllocator > m_clusterVtMetadata;

    void retain( void ) { m_nReferences.fetch_add( 1u, std::memory_order_relaxed ); }
    void release( void )
    {
        if ( m_nReferences.fetch_sub( 1u, std::memory_order_acq_rel ) == 1u ) {
            delete this;
        }
    }
    void bindRuntimeAllocator( const Terrain3DEastlAllocator &rAllocator );
    bool copyCold( const Terrain3DWorldPublication &rSource );
    size_t getRetainedCpuBytes( void ) const;
};

// Manual intrusive lease, intentionally no STL smart-pointer or frame allocations.
class Terrain3DWorldGeometryLease
{
private:
    Terrain3DSharedWorldGeometry *m_pGeometry = NULL;

public:
    Terrain3DWorldGeometryLease( void ) = default;
    Terrain3DWorldGeometryLease( const Terrain3DWorldGeometryLease &rOther ) : m_pGeometry( rOther.m_pGeometry )
    {
        if ( m_pGeometry ) { m_pGeometry->retain(); }
    }
    Terrain3DWorldGeometryLease( Terrain3DWorldGeometryLease &&rOther ) noexcept : m_pGeometry( rOther.m_pGeometry )
    {
        rOther.m_pGeometry = NULL;
    }
    ~Terrain3DWorldGeometryLease( void ) { reset(); }
    Terrain3DWorldGeometryLease &operator=( const Terrain3DWorldGeometryLease &rOther )
    {
        bind( rOther.m_pGeometry );
        return *this;
    }
    Terrain3DWorldGeometryLease &operator=( Terrain3DWorldGeometryLease &&rOther ) noexcept
    {
        if ( this != &rOther ) {
            reset();
            m_pGeometry = rOther.m_pGeometry;
            rOther.m_pGeometry = NULL;
        }
        return *this;
    }
    void bind( Terrain3DSharedWorldGeometry *pGeometry )
    {
        if ( pGeometry ) { pGeometry->retain(); }
        reset();
        m_pGeometry = pGeometry;
    }
    void adopt( Terrain3DSharedWorldGeometry *pGeometry )
    {
        reset();
        m_pGeometry = pGeometry;
    }
    void reset( void )
    {
        if ( m_pGeometry ) { m_pGeometry->release(); m_pGeometry = NULL; }
    }
    Terrain3DSharedWorldGeometry *get( void ) const { return m_pGeometry; }
};

class Terrain3DWorldPublication
{
private:
    Terrain3DWorldGeometryLease m_sharedGeometry;
    bool m_bUseSharedGeometry = false;

public:
    void enableSharedGeometryConsumerCold( void ) { m_bUseSharedGeometry = true; }
    bool usesSharedGeometry( void ) const { return m_bUseSharedGeometry; }
    void attachSharedGeometryCold( const Terrain3DWorldGeometryLease &rGeometry );
    const eastl::vector< t3dWorldVertex_t, Terrain3DEastlAllocator > &vertices( void ) const
    { return m_sharedGeometry.get() ? m_sharedGeometry.get()->m_vertices : m_vertices; }
    const eastl::vector< uint32_t, Terrain3DEastlAllocator > &clusterVertexIndices( void ) const
    { return m_sharedGeometry.get() ? m_sharedGeometry.get()->m_clusterVertexIndices : m_clusterVertexIndices; }
    const eastl::vector< uint32_t, Terrain3DEastlAllocator > &triangleWords( void ) const
    { return m_sharedGeometry.get() ? m_sharedGeometry.get()->m_triangleWords : m_triangleWords; }
    const eastl::vector< t3dWorldCluster_t, Terrain3DEastlAllocator > &clusters( void ) const
    { return m_sharedGeometry.get() ? m_sharedGeometry.get()->m_clusters : m_clusters; }
    const eastl::vector< t3dWorldClusterVTMetadata_t, Terrain3DEastlAllocator > &clusterVtMetadata( void ) const
    { return m_sharedGeometry.get() ? m_sharedGeometry.get()->m_clusterVtMetadata : m_clusterVtMetadata; }
    uint64_t getSharedGeometryRetainedCpuBytes( void ) const
    { return m_sharedGeometry.get() ? m_sharedGeometry.get()->getRetainedCpuBytes() : 0u; }
    const Terrain3DSharedWorldGeometry *getSharedGeometryIdentity( void ) const
    { return m_sharedGeometry.get(); }

	uint64_t m_nSourceSignature = 0u;
	uint64_t m_nGeneration = 0u;
	uint64_t m_nGeometryGeneration = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nInstanceGeneration = 0u;
	// CPU-only HLOD source generation acknowledged after the exact WORLD GPU upload.
	uint64_t m_nHlodSourceGeneration = 0u;
	uint64_t m_nTerrainGeneration = 0u;
	uint64_t m_nStaticGeneration = 0u;
	// CPU bank-local identity for unchanged mesh topology payload.
	uint64_t m_nMeshPayloadSignature = 0u;
	uint64_t m_nStableGeometryEpoch = 0u;
    uint64_t m_nGeometryMutationRevision = 0u;
    eastl::vector< t3dWorldGeometryDirtyRange_t, Terrain3DEastlAllocator > m_geometryDirtyRanges;
	// CPU-only provenance for the static-scene visibility handoff.
	uint64_t m_nStaticSceneProducerId = 0u;
	uint64_t m_nStaticScenePublicationGeneration = 0u;
	uint32_t m_nInstanceClusterCount = 0u;
	uint32_t m_nHlodClusterCount = 0u;
	uint32_t m_nStaticClusterCount = 0u;
	uint32_t m_nTerrainStripClusterCount = 0u;

	eastl::vector< t3dWorldVertex_t, Terrain3DEastlAllocator >			  m_vertices;
	eastl::vector< uint32_t, Terrain3DEastlAllocator >					  m_clusterVertexIndices;
	eastl::vector< uint32_t, Terrain3DEastlAllocator >					  m_triangleWords;
	eastl::vector< t3dWorldCluster_t, Terrain3DEastlAllocator >			  m_clusters;
	eastl::vector< t3dWorldClusterVTMetadata_t, Terrain3DEastlAllocator > m_clusterVtMetadata;
	eastl::vector< t3dWorldTerrainCluster_t, Terrain3DEastlAllocator >	  m_terrainStripClusters;
	eastl::vector< t3dWorldTransform_t, Terrain3DEastlAllocator >		  m_transforms;
	eastl::vector< t3dWorldTransform_t, Terrain3DEastlAllocator >		  m_previousTransforms;
	eastl::vector< t3dWorldInstance_t, Terrain3DEastlAllocator >		  m_instances;
	eastl::vector< t3dWorldCell_t, Terrain3DEastlAllocator >			  m_cells;
	eastl::vector< t3dWorldHlodRecord_t, Terrain3DEastlAllocator >		  m_hlodRecords;
	eastl::vector< t3dRenderingMaterialGpu_t, Terrain3DEastlAllocator >	  m_materials;
	// CPU-only keys for the instance-material prefix; resolve against the live VT package.
	eastl::vector< uint64_t, Terrain3DEastlAllocator >				m_instanceMaterialStableKeys;
	eastl::vector< t3dWorldVisibleItem_t, Terrain3DEastlAllocator > m_visibilityCandidates;

	void   bindRuntimeAllocator( const Terrain3DEastlAllocator &rAllocator );
	bool   hasCapacity( const t3dWorldPublicationCapacity_t &rRequired ) const;
	bool   reserveCold( const t3dWorldPublicationCapacity_t &rRequired );
	void   clearForBuild( bool bRetainMeshPayload = false );
	bool   copyFrom( const Terrain3DWorldPublication &rOther );
	size_t getRetainedCpuBytes( void ) const;
};

static_assert( sizeof( t3dWorldInstance_t ) == 64u, "World instance publication ABI changed" );
static_assert( sizeof( t3dWorldCell_t ) == 32u, "World cell publication ABI changed" );
static_assert( sizeof( t3dWorldHlodRecord_t ) == 32u, "World HLOD publication ABI changed" );
static_assert( sizeof( t3dWorldTerrainClusterBounds_t ) == 16u, "World terrain cluster bounds ABI changed" );

#endif // TERRAIN3D_WORLD_PUBLICATION_H
