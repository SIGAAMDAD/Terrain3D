// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/runtime/terrain_3d_world_geometry_cache.h"

#include "core/utility/terrain_3d_hash_bytes.h"
#include "core/utility/terrain_3d_generation.h"

#include <string.h>
#include <new>
#include <EASTL/sort.h>

static bool t3dWorldSourceRangeValid( const uint32_t nBase, const uint32_t nCount, const size_t nSize )
{
	return nBase <= nSize && nCount <= nSize - nBase;
}

static bool t3dWorldDeriveSourceSpan( const t3dRdMeshSourceSnapshot_t &rSnapshot,
									const t3dRdMeshSourceLod_t &rLod,
									t3dWorldResidentGeometrySpan_t &rSpan )
{
	if ( !t3dWorldSourceRangeValid( rLod.m_nFirstSurface, rLod.m_nSurfaceCount, rSnapshot.m_surfaces.size() ) ) {
		return false;
	}
	uint32_t nVertexEnd = 0u, nIndexEnd = 0u, nTriangleEnd = 0u, nClusterEnd = 0u;
	rSpan.m_nVertexBase = rSpan.m_nIndexBase = rSpan.m_nTriangleBase = rSpan.m_nClusterBase = UINT32_MAX;
	for ( uint32_t i = 0u; i < rLod.m_nSurfaceCount; ++i ) {
		const t3dRdMeshSourceSurface_t &rSurface = rSnapshot.m_surfaces[rLod.m_nFirstSurface + i];
		if ( rSurface.m_nSourceKey != rLod.m_nSourceKey ||
			 !t3dWorldSourceRangeValid( rSurface.m_nWorldVertexOffset, rSurface.m_nWorldVertexCount,
										   rSnapshot.m_worldVertices.size() ) ||
			 !t3dWorldSourceRangeValid( rSurface.m_nWorldClusterOffset, rSurface.m_nWorldClusterCount,
										   rSnapshot.m_worldClusters.size() ) ) {
			return false;
		}
		if ( !rSurface.m_nWorldClusterCount ) {
			continue;
		}
		rSpan.m_nVertexBase = MIN( rSpan.m_nVertexBase, rSurface.m_nWorldVertexOffset );
		rSpan.m_nClusterBase = MIN( rSpan.m_nClusterBase, rSurface.m_nWorldClusterOffset );
		nVertexEnd = MAX( nVertexEnd, rSurface.m_nWorldVertexOffset + rSurface.m_nWorldVertexCount );
		nClusterEnd = MAX( nClusterEnd, rSurface.m_nWorldClusterOffset + rSurface.m_nWorldClusterCount );
		for ( uint32_t c = 0u; c < rSurface.m_nWorldClusterCount; ++c ) {
			const t3dWorldCluster_t &rCluster = rSnapshot.m_worldClusters[rSurface.m_nWorldClusterOffset + c];
			const uint32_t nIndexCount = t3dWorldUnpackU16Low( rCluster.m_nPackedCounts );
			if ( !t3dWorldSourceRangeValid( rCluster.m_nVertexIndexOffset, nIndexCount,
											   rSnapshot.m_worldClusterVertexIndices.size() ) ||
				 !t3dWorldSourceRangeValid( rCluster.m_nTriangleOffset, T3D_WORLD_CLUSTER_MAX_TRIANGLES,
											   rSnapshot.m_worldTriangleWords.size() ) ) {
				return false;
			}
			rSpan.m_nIndexBase = MIN( rSpan.m_nIndexBase, rCluster.m_nVertexIndexOffset );
			rSpan.m_nTriangleBase = MIN( rSpan.m_nTriangleBase, rCluster.m_nTriangleOffset );
			nIndexEnd = MAX( nIndexEnd, rCluster.m_nVertexIndexOffset + nIndexCount );
			nTriangleEnd = MAX( nTriangleEnd, rCluster.m_nTriangleOffset + T3D_WORLD_CLUSTER_MAX_TRIANGLES );
		}
	}
	if ( rSpan.m_nClusterBase == UINT32_MAX ) {
		rSpan = {};
		return true; // Occlusion-only sources have no WORLD raster geometry.
	}
	if ( rSpan.m_nIndexBase == UINT32_MAX || rSpan.m_nTriangleBase == UINT32_MAX ||
		 nClusterEnd > rSnapshot.m_worldClusterVtMetadata.size() ) {
		return false;
	}
	rSpan.m_nVertexCount = nVertexEnd - rSpan.m_nVertexBase;
	rSpan.m_nIndexCount = nIndexEnd - rSpan.m_nIndexBase;
	rSpan.m_nTriangleCount = nTriangleEnd - rSpan.m_nTriangleBase;
	rSpan.m_nClusterCount = nClusterEnd - rSpan.m_nClusterBase;
	return true;
}

// Hashes identify candidates; canonical byte comparison proves an alias is
// physically interchangeable. This is cold-only (source admission/revision).
static bool t3dWorldGeometryEqualsPhysical( const Terrain3DWorldPublication &rGeometry,
	const t3dWorldResidentGeometrySpan_t &rPhysical,
	const t3dWorldResidentGeometrySpan_t &rInput,
	const t3dWorldVertex_t *pVertices, const uint32_t *pIndices,
	const uint32_t *pTriangles, const t3dWorldCluster_t *pClusters,
	const t3dWorldClusterVTMetadata_t *pMetadata,
	const t3dRenderingMaterialGpu_t *pMaterials, const uint64_t *pMaterialKeys,
	const uint32_t nMaterialCount )
{
	if ( rPhysical.m_nVertexCount != rInput.m_nVertexCount ||
		rPhysical.m_nIndexCount != rInput.m_nIndexCount ||
		rPhysical.m_nTriangleCount != rInput.m_nTriangleCount ||
		rPhysical.m_nClusterCount != rInput.m_nClusterCount ||
		memcmp( &rGeometry.m_vertices[rPhysical.m_nVertexBase], pVertices + rInput.m_nVertexBase,
			uint64_t( rInput.m_nVertexCount ) * sizeof( t3dWorldVertex_t ) ) != 0 ||
		memcmp( &rGeometry.m_triangleWords[rPhysical.m_nTriangleBase], pTriangles + rInput.m_nTriangleBase,
			uint64_t( rInput.m_nTriangleCount ) * sizeof( uint32_t ) ) != 0 ||
		memcmp( &rGeometry.m_clusterVtMetadata[rPhysical.m_nClusterBase], pMetadata + rInput.m_nClusterBase,
			uint64_t( rInput.m_nClusterCount ) * sizeof( t3dWorldClusterVTMetadata_t ) ) != 0 ) {
		return false;
	}
	for ( uint32_t i = 0u; i < rInput.m_nIndexCount; ++i ) {
		if ( rGeometry.m_clusterVertexIndices[rPhysical.m_nIndexBase + i] - rPhysical.m_nVertexBase !=
			pIndices[rInput.m_nIndexBase + i] - rInput.m_nVertexBase ) {
			return false;
		}
	}
	for ( uint32_t i = 0u; i < rInput.m_nClusterCount; ++i ) {
		t3dWorldCluster_t input = pClusters[rInput.m_nClusterBase + i];
		t3dWorldCluster_t physical = rGeometry.m_clusters[rPhysical.m_nClusterBase + i];
		if ( input.m_nVertexIndexOffset - rInput.m_nIndexBase !=
			physical.m_nVertexIndexOffset - rPhysical.m_nIndexBase ||
			input.m_nTriangleOffset - rInput.m_nTriangleBase !=
			physical.m_nTriangleOffset - rPhysical.m_nTriangleBase ) {
			return false;
		}
		if ( input.m_nMaterial != T3D_WORLD_INVALID_ID || physical.m_nMaterial != T3D_WORLD_INVALID_ID ) {
			if ( input.m_nMaterial >= nMaterialCount || physical.m_nMaterial >= rGeometry.m_materials.size() ||
				!pMaterials ||
				memcmp( &pMaterials[input.m_nMaterial], &rGeometry.m_materials[physical.m_nMaterial],
					sizeof( t3dRenderingMaterialGpu_t ) ) != 0 ||
				( pMaterialKeys ? pMaterialKeys[input.m_nMaterial] : 0u ) !=
					rGeometry.m_instanceMaterialStableKeys[physical.m_nMaterial] ) {
				return false;
			}
		}
		input.m_nVertexIndexOffset -= rInput.m_nIndexBase;
		physical.m_nVertexIndexOffset -= rPhysical.m_nIndexBase;
		input.m_nTriangleOffset -= rInput.m_nTriangleBase;
		physical.m_nTriangleOffset -= rPhysical.m_nTriangleBase;
		input.m_nMaterial = physical.m_nMaterial = 0u;
		input.m_nSourceId = physical.m_nSourceId = 0u;
		if ( memcmp( &input, &physical, sizeof( input ) ) != 0 ) {
			return false;
		}
	}
	return true;
}

void Terrain3DWorldGeometryCache::bindRuntimeAllocator( const Terrain3DEastlAllocator &rAllocator )
{
	m_geometry.bindRuntimeAllocator( rAllocator );
	m_sources.set_allocator( rAllocator );
	m_activeSources.set_allocator( rAllocator );
	m_pendingActiveSources.set_allocator( rAllocator );
	m_staticMaterialSlots.set_allocator( rAllocator );
	m_pendingStaticMaterialSlots.set_allocator( rAllocator );
    for ( uint32_t i = 0u; i < 4u; ++i ) { m_freeRanges[i].set_allocator( rAllocator ); }
    m_pendingRangeClaims.set_allocator( rAllocator );
    m_dirtyRanges.set_allocator( rAllocator );
}

void Terrain3DWorldGeometryCache::reset( void )
{
    m_frozenGeometry.reset();
    m_nFrozenEpoch = m_nFrozenGeometryRevision = m_nFrozenMutationRevision = 0u;
	m_geometry.clearForBuild();
	m_sources.clear();
	m_activeSources.clear();
	m_pendingActiveSources.clear();
	m_staticMaterialSlots.clear();
	m_pendingStaticMaterialSlots.clear();
    for ( uint32_t i = 0u; i < 4u; ++i ) { m_freeRanges[i].clear(); }
    m_pendingRangeClaims.clear();
    m_dirtyRanges.clear();
    m_nMutationRevision = 0u;
    m_nRecycledSpans = 0u;
    m_nReusedGeometryBytes = 0u;
	m_nActiveStaticSource = UINT32_MAX;
	m_nCacheEpoch = t3dNextGeneration( m_nCacheEpoch );
	m_nGeometryRevision = t3dNextGeneration( m_nGeometryRevision );
	m_nMaterialRevision = t3dNextGeneration( m_nMaterialRevision );
	m_nDeduplicatedSources = 0u;
	m_nDeduplicatedGpuBytes = 0u;
	m_nSafeCompactionCount = 0u;
	m_nReclaimedGeometryBytes = 0u;
}

bool Terrain3DWorldGeometryCache::resolveMaterialCold( const t3dRenderingMaterialGpu_t &rMaterial,
													const uint64_t nStableKey,
													const uint32_t nLimit, uint32_t &rSlot )
{
	for ( uint32_t i = 0u; i < m_geometry.m_materials.size(); ++i ) {
		if ( m_geometry.m_instanceMaterialStableKeys[i] == nStableKey &&
			 memcmp( &m_geometry.m_materials[i], &rMaterial, sizeof( rMaterial ) ) == 0 ) {
			rSlot = i;
			return true;
		}
	}
	if ( m_geometry.m_materials.size() >= nLimit || m_geometry.m_materials.size() >= UINT32_MAX ||
		 !t3dContainerReserveCold( m_geometry.m_materials, m_geometry.m_materials.size() + 1u ) ||
		 !t3dContainerReserveCold( m_geometry.m_instanceMaterialStableKeys,
											 m_geometry.m_instanceMaterialStableKeys.size() + 1u ) ) {
		return false;
	}
	rSlot = ( uint32_t ) m_geometry.m_materials.size();
	m_geometry.m_materials.push_back( rMaterial );
	m_geometry.m_instanceMaterialStableKeys.push_back( nStableKey );
	m_nMaterialRevision = t3dNextGeneration( m_nMaterialRevision );
	return true;
}

bool Terrain3DWorldGeometryCache::registerSourceCold( const uint64_t nKey,
											 const t3dWorldResidentGeometrySpan_t &rInput,
											 const t3dWorldVertex_t *pVertices,
											 const uint32_t *pIndices,
											 const uint32_t *pTriangles,
											 const t3dWorldCluster_t *pClusters,
											 const t3dWorldClusterVTMetadata_t *pMetadata,
											 const t3dRenderingMaterialGpu_t *pMaterials,
											 const uint64_t *pMaterialKeys,
											 const uint32_t nMaterialCount,
											 const t3dWorldResourceLimits_t &rLimits,
											 uint32_t &rRecordIndex )
{
	if ( !nKey || !rInput.m_nClusterCount || !pVertices || !pIndices || !pTriangles || !pClusters || !pMetadata ) {
		return false;
	}
	uint64_t nSignature = 1469598103934665603ull;
	const uint32_t szCounts[4] = { rInput.m_nVertexCount, rInput.m_nIndexCount,
										  rInput.m_nTriangleCount, rInput.m_nClusterCount };
	nSignature = t3dHashFnv1a64( szCounts, sizeof( szCounts ), nSignature );
	nSignature = t3dHashFnv1a64( pVertices + rInput.m_nVertexBase,
									  uint64_t( rInput.m_nVertexCount ) * sizeof( t3dWorldVertex_t ), nSignature );
	for ( uint32_t i = 0u; i < rInput.m_nIndexCount; ++i ) {
		const uint32_t nVertexIndex = pIndices[rInput.m_nIndexBase + i];
		if ( nVertexIndex < rInput.m_nVertexBase ||
			 nVertexIndex - rInput.m_nVertexBase >= rInput.m_nVertexCount ) {
			return false;
		}
		nSignature = t3dHashFnv1a64LittleU32( nSignature, nVertexIndex - rInput.m_nVertexBase );
	}
	nSignature = t3dHashFnv1a64( pTriangles + rInput.m_nTriangleBase,
									  uint64_t( rInput.m_nTriangleCount ) * sizeof( uint32_t ), nSignature );
	for ( uint32_t i = 0u; i < rInput.m_nClusterCount; ++i ) {
		t3dWorldCluster_t cluster = pClusters[rInput.m_nClusterBase + i];
		if ( cluster.m_nVertexIndexOffset < rInput.m_nIndexBase ||
			 cluster.m_nTriangleOffset < rInput.m_nTriangleBase ||
			 !t3dWorldSourceRangeValid( cluster.m_nVertexIndexOffset - rInput.m_nIndexBase,
									  t3dWorldUnpackU16Low( cluster.m_nPackedCounts ), rInput.m_nIndexCount ) ||
			 !t3dWorldSourceRangeValid( cluster.m_nTriangleOffset - rInput.m_nTriangleBase,
									  T3D_WORLD_CLUSTER_MAX_TRIANGLES, rInput.m_nTriangleCount ) ) {
			return false;
		}
		cluster.m_nVertexIndexOffset -= rInput.m_nIndexBase;
		cluster.m_nTriangleOffset -= rInput.m_nTriangleBase;
		if ( cluster.m_nMaterial != T3D_WORLD_INVALID_ID ) {
			if ( cluster.m_nMaterial >= nMaterialCount || !pMaterials ) {
				return false;
			}
			const uint32_t nMaterial = cluster.m_nMaterial;
			const uint64_t nKeyPart = pMaterialKeys ? pMaterialKeys[nMaterial] : 0u;
			nSignature = t3dHashFnv1a64( &nKeyPart, sizeof( nKeyPart ), nSignature );
			nSignature = t3dHashFnv1a64( &pMaterials[nMaterial], sizeof( pMaterials[nMaterial] ), nSignature );
			cluster.m_nMaterial = 0u; // Material table placement is intentionally absent from identity.
		}
		// Source identifiers live in instance records; they do not affect the WORLD
		// geometry payload. Excluding them permits distinct GLB/HLOD source keys to
		// share one physical allocation for identical geometry and materials.
		cluster.m_nSourceId = 0u;
		nSignature = t3dHashFnv1a64( &cluster, sizeof( cluster ), nSignature );
	}
	nSignature = t3dHashFnv1a64( pMetadata + rInput.m_nClusterBase,
									  uint64_t( rInput.m_nClusterCount ) * sizeof( t3dWorldClusterVTMetadata_t ), nSignature );
	uint32_t nMatchingPhysical = UINT32_MAX;
	for ( uint32_t i = 0u; i < m_sources.size(); ++i ) {
		const t3dWorldResidentGeometrySpan_t &existing = m_sources[i];
		if ( existing.m_nPhysicalRecord == UINT32_MAX || existing.m_nContentSignature != nSignature ||
			 existing.m_nVertexCount != rInput.m_nVertexCount ||
			 existing.m_nIndexCount != rInput.m_nIndexCount ||
			 existing.m_nTriangleCount != rInput.m_nTriangleCount ||
			 existing.m_nClusterCount != rInput.m_nClusterCount ) {
			continue;
		}
		if ( existing.m_nSourceKey == nKey ) {
			rRecordIndex = i;
			return true;
		}
		// The canonical physical record may itself be referenced by several aliases.
		const uint32_t nPhysical = existing.m_nPhysicalRecord;
		if ( nPhysical < m_sources.size() && t3dWorldGeometryEqualsPhysical( m_geometry,
			m_sources[nPhysical], rInput, pVertices, pIndices, pTriangles, pClusters,
			pMetadata, pMaterials, pMaterialKeys, nMaterialCount ) ) {
			nMatchingPhysical = nPhysical;
		}
	}
	if ( nMatchingPhysical != UINT32_MAX ) {
		if ( m_sources.size() >= UINT32_MAX ||
			 !t3dContainerReserveCold( m_sources, m_sources.size() + 1u ) ) {
			return false;
		}
		t3dWorldResidentGeometrySpan_t alias = m_sources[nMatchingPhysical];
		alias.m_nSourceKey = nKey;
		alias.m_nContentSignature = nSignature;
		alias.m_nPhysicalRecord = nMatchingPhysical;
		rRecordIndex = ( uint32_t ) m_sources.size();
		m_sources.push_back( alias );
		++m_nDeduplicatedSources;
		m_nDeduplicatedGpuBytes += uint64_t( alias.m_nVertexCount ) * sizeof( t3dWorldVertex_t ) +
			uint64_t( alias.m_nIndexCount + alias.m_nTriangleCount ) * sizeof( uint32_t ) +
			uint64_t( alias.m_nClusterCount ) * ( sizeof( t3dWorldCluster_t ) + sizeof( t3dWorldClusterVTMetadata_t ) );
		return true;
	}

    // Best-fit each physical stream independently. A free range can be
    // claimed more than once in one cold transaction; claims advance its
    // uncommitted prefix. Live offsets never change.
    const Terrain3DWorldPublication &g = m_geometry;
    const uint32_t counts[4] = { rInput.m_nVertexCount, rInput.m_nIndexCount,
        rInput.m_nTriangleCount, rInput.m_nClusterCount };
    const uint32_t sizes[4] = { ( uint32_t ) g.m_vertices.size(),
        ( uint32_t ) g.m_clusterVertexIndices.size(), ( uint32_t ) g.m_triangleWords.size(),
        ( uint32_t ) g.m_clusters.size() };
    const uint32_t limits[4] = { rLimits.m_nMaxSourceVertices,
        rLimits.m_nMaxClusterVertexIndices, rLimits.m_nMaxTriangleSlots,
        rLimits.m_nMaxClusters };
    uint32_t bases[4] = {};
    uint32_t slots[4] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    uint32_t nNewSize[4] = {};
    bool bReused = false;
    for ( uint32_t stream = 0u; stream < 4u; ++stream ) {
        uint32_t nBestWaste = UINT32_MAX;
        for ( uint32_t slot = 0u; slot < m_freeRanges[stream].size(); ++slot ) {
            const t3dWorldGeometryFreeRange_t &free = m_freeRanges[stream][slot];
            uint32_t nClaimed = 0u;
            for ( uint32_t p = 0u; p < m_pendingRangeClaims.size(); ++p ) {
                const t3dWorldGeometryRangeClaim_t &claim = m_pendingRangeClaims[p];
                if ( claim.m_nStream == stream && claim.m_nSlot == slot ) {
                    nClaimed += claim.m_nCount;
                }
            }
            if ( nClaimed > free.m_nCount || free.m_nCount - nClaimed < counts[stream] ) {
                continue;
            }
            const uint32_t nWaste = free.m_nCount - nClaimed - counts[stream];
            if ( nWaste < nBestWaste ) {
                nBestWaste = nWaste;
                slots[stream] = slot;
                bases[stream] = free.m_nFirst + nClaimed;
            }
        }
        if ( slots[stream] == UINT32_MAX ) {
            if ( uint64_t( sizes[stream] ) + counts[stream] > limits[stream] ) {
                return false;
            }
            bases[stream] = sizes[stream];
            nNewSize[stream] = sizes[stream] + counts[stream];
        } else {
            bReused = true;
            nNewSize[stream] = sizes[stream];
        }
    }
    // Reserve every mutable container before overwriting recyclable bytes.
    // A rejected transaction can leave changed bytes in unowned/free ranges,
    // but it cannot alter any live allocation, source handle or GPU address.
    if ( m_sources.size() >= UINT32_MAX ||
        !t3dContainerReserveCold( m_sources, m_sources.size() + 1u ) ||
        !t3dContainerReserveCold( m_pendingRangeClaims, m_pendingRangeClaims.size() + 4u ) ||
        ( bReused && !t3dContainerReserveCold( m_dirtyRanges, m_dirtyRanges.size() + 5u ) ) ||
        !t3dContainerReserveCold( m_geometry.m_vertices, nNewSize[0] ) ||
        !t3dContainerReserveCold( m_geometry.m_clusterVertexIndices, nNewSize[1] ) ||
        !t3dContainerReserveCold( m_geometry.m_triangleWords, nNewSize[2] ) ||
        !t3dContainerReserveCold( m_geometry.m_clusters, nNewSize[3] ) ||
        !t3dContainerReserveCold( m_geometry.m_clusterVtMetadata, nNewSize[3] ) ) {
        return false;
    }
    for ( uint32_t i = 0u; i < rInput.m_nClusterCount; ++i ) {
        const uint32_t nLocal = pClusters[rInput.m_nClusterBase + i].m_nMaterial;
        if ( nLocal != T3D_WORLD_INVALID_ID ) {
            uint32_t nSlot = UINT32_MAX;
            if ( nLocal >= nMaterialCount || !pMaterials ||
                !resolveMaterialCold( pMaterials[nLocal],
                    pMaterialKeys ? pMaterialKeys[nLocal] : 0u,
                    rLimits.m_nMaxMaterials, nSlot ) ) {
                return false;
            }
        }
    }
    m_geometry.m_vertices.resize( nNewSize[0] );
    m_geometry.m_clusterVertexIndices.resize( nNewSize[1] );
    m_geometry.m_triangleWords.resize( nNewSize[2] );
    m_geometry.m_clusters.resize( nNewSize[3] );
    m_geometry.m_clusterVtMetadata.resize( nNewSize[3] );
    t3dWorldResidentGeometrySpan_t span{};
    span.m_nSourceKey = nKey;
    span.m_nContentSignature = nSignature;
    span.m_nPhysicalRecord = ( uint32_t ) m_sources.size();
    span.m_nVertexBase = bases[0];
    span.m_nIndexBase = bases[1];
    span.m_nTriangleBase = bases[2];
    span.m_nClusterBase = bases[3];
    span.m_nVertexCount = span.m_nReservedVertexCount = counts[0];
    span.m_nIndexCount = span.m_nReservedIndexCount = counts[1];
    span.m_nTriangleCount = span.m_nReservedTriangleCount = counts[2];
    span.m_nClusterCount = span.m_nReservedClusterCount = counts[3];
    memcpy( &m_geometry.m_vertices[bases[0]], pVertices + rInput.m_nVertexBase,
        uint64_t( counts[0] ) * sizeof( t3dWorldVertex_t ) );
    for ( uint32_t i = 0u; i < counts[1]; ++i ) {
        m_geometry.m_clusterVertexIndices[bases[1] + i] = bases[0] +
            pIndices[rInput.m_nIndexBase + i] - rInput.m_nVertexBase;
    }
    memcpy( &m_geometry.m_triangleWords[bases[2]], pTriangles + rInput.m_nTriangleBase,
        uint64_t( counts[2] ) * sizeof( uint32_t ) );
    for ( uint32_t i = 0u; i < counts[3]; ++i ) {
        t3dWorldCluster_t cluster = pClusters[rInput.m_nClusterBase + i];
        cluster.m_nVertexIndexOffset = bases[1] + cluster.m_nVertexIndexOffset - rInput.m_nIndexBase;
        cluster.m_nTriangleOffset = bases[2] + cluster.m_nTriangleOffset - rInput.m_nTriangleBase;
        if ( cluster.m_nMaterial != T3D_WORLD_INVALID_ID ) {
            uint32_t nSlot = UINT32_MAX;
            const uint32_t nLocal = cluster.m_nMaterial;
            resolveMaterialCold( pMaterials[nLocal], pMaterialKeys ? pMaterialKeys[nLocal] : 0u,
                rLimits.m_nMaxMaterials, nSlot );
            cluster.m_nMaterial = nSlot;
        }
        m_geometry.m_clusters[bases[3] + i] = cluster;
    }
    memcpy( &m_geometry.m_clusterVtMetadata[bases[3]], pMetadata + rInput.m_nClusterBase,
        uint64_t( counts[3] ) * sizeof( t3dWorldClusterVTMetadata_t ) );
    for ( uint32_t stream = 0u; stream < 4u; ++stream ) {
        if ( slots[stream] != UINT32_MAX && counts[stream] != 0u ) {
            m_pendingRangeClaims.push_back( { stream, slots[stream], counts[stream] } );
        }
    }
    if ( bReused ) {
        const uint64_t nChange = t3dNextGeneration( m_nMutationRevision );
        const uint32_t strides[5] = { sizeof( t3dWorldVertex_t ), sizeof( uint32_t ),
            sizeof( uint32_t ), sizeof( t3dWorldCluster_t ), sizeof( t3dWorldClusterVTMetadata_t ) };
        for ( uint32_t stream = 0u; stream < 4u; ++stream ) {
            if ( slots[stream] == UINT32_MAX || counts[stream] == 0u ) { continue; }
            m_dirtyRanges.push_back( { nChange, stream,
                bases[stream] * strides[stream], counts[stream] * strides[stream] } );
            if ( stream == 3u ) {
                m_dirtyRanges.push_back( { nChange, 4u,
                    bases[stream] * strides[4], counts[stream] * strides[4] } );
            }
            const uint32_t nBytes = counts[stream] * strides[stream];
            m_nReusedGeometryBytes += nBytes;
            if ( stream == 3u ) { m_nReusedGeometryBytes += counts[stream] * strides[4]; }
        }
        m_nMutationRevision = nChange;
        ++m_nRecycledSpans;
    }
    rRecordIndex = span.m_nPhysicalRecord;
    m_sources.push_back( span );
    m_nGeometryRevision = t3dNextGeneration( m_nGeometryRevision );
    return true;
}

bool Terrain3DWorldGeometryCache::synchronizeCold( const t3dRdMeshSourceSnapshot_t &rSnapshot,
												 const Terrain3DWorldPublication *pStaticWorld,
												 const uint64_t nStaticProducer,
												 const t3dWorldResourceLimits_t &rLimits )
{
	const size_t nOldVertices = m_geometry.m_vertices.size();
	const size_t nOldIndices = m_geometry.m_clusterVertexIndices.size();
	const size_t nOldTriangles = m_geometry.m_triangleWords.size();
	const size_t nOldClusters = m_geometry.m_clusters.size();
	const size_t nOldMaterials = m_geometry.m_materials.size();
	const size_t nOldSources = m_sources.size();
    const size_t nOldDirtyRanges = m_dirtyRanges.size();
    const uint64_t nOldMutationRevision = m_nMutationRevision;
    const uint64_t nOldRecycledSpans = m_nRecycledSpans;
    const uint64_t nOldReusedGeometryBytes = m_nReusedGeometryBytes;
    m_pendingRangeClaims.clear();
	const uint64_t nOldGeometryRevision = m_nGeometryRevision;
	const uint64_t nOldMaterialRevision = m_nMaterialRevision;
	const uint64_t nOldDeduplicatedSources = m_nDeduplicatedSources;
	const uint64_t nOldDeduplicatedGpuBytes = m_nDeduplicatedGpuBytes;
	m_pendingActiveSources.clear();
	m_pendingStaticMaterialSlots.clear();
	uint32_t nNewStaticIndex = UINT32_MAX;
	bool bOk = t3dContainerReserveCold( m_pendingActiveSources, rSnapshot.m_lods.size() ) &&
										  t3dContainerReserveCold( m_pendingStaticMaterialSlots,
													 pStaticWorld ? pStaticWorld->m_materials.size() : 0u );
	for ( size_t i = 0u; bOk && i < rSnapshot.m_lods.size(); ++i ) {
		const t3dRdMeshSourceLod_t &lod = rSnapshot.m_lods[i];
		t3dWorldResidentGeometrySpan_t view{};
		bOk = t3dWorldDeriveSourceSpan( rSnapshot, lod, view );
		if ( !bOk || !view.m_nClusterCount ) {
			continue;
		}
		uint32_t nRecord = UINT32_MAX;
		bOk = registerSourceCold( lod.m_nSourceKey, view,
			rSnapshot.m_worldVertices.data(), rSnapshot.m_worldClusterVertexIndices.data(),
			rSnapshot.m_worldTriangleWords.data(), rSnapshot.m_worldClusters.data(),
			rSnapshot.m_worldClusterVtMetadata.data(), rSnapshot.m_vtMaterials.data(),
			rSnapshot.m_vtStableKeys.data(), ( uint32_t ) rSnapshot.m_vtMaterials.size(), rLimits, nRecord );
		if ( bOk ) {
			m_pendingActiveSources.push_back( nRecord );
		}
	}
	if ( bOk && pStaticWorld ) {
		// A static-scene publication owns one cooked bundle. Transform-only edits
		// reuse it because source identity hashes canonical geometry, not instances.
		const uint64_t nStaticKey = 0x4000000000000000ull | ( nStaticProducer & 0x0fffffffffffffffull );
		if ( !pStaticWorld->m_clusters.empty() ) {
			t3dWorldResidentGeometrySpan_t view{};
			view.m_nVertexCount = ( uint32_t ) pStaticWorld->m_vertices.size();
			view.m_nIndexCount = ( uint32_t ) pStaticWorld->m_clusterVertexIndices.size();
			view.m_nTriangleCount = ( uint32_t ) pStaticWorld->m_triangleWords.size();
			view.m_nClusterCount = ( uint32_t ) pStaticWorld->m_clusters.size();
			bOk = registerSourceCold( nStaticKey, view, pStaticWorld->m_vertices.data(),
				pStaticWorld->m_clusterVertexIndices.data(), pStaticWorld->m_triangleWords.data(),
				pStaticWorld->m_clusters.data(), pStaticWorld->m_clusterVtMetadata.data(),
				pStaticWorld->m_materials.data(), NULL, ( uint32_t ) pStaticWorld->m_materials.size(),
				rLimits, nNewStaticIndex );
		}
		for ( size_t i = 0u; bOk && i < pStaticWorld->m_materials.size(); ++i ) {
			uint32_t nSlot = UINT32_MAX;
			bOk = resolveMaterialCold( pStaticWorld->m_materials[i], 0u,
											 rLimits.m_nMaxMaterials, nSlot );
			if ( bOk ) {
				m_pendingStaticMaterialSlots.push_back( nSlot );
			}
		}
	}
	if ( !bOk ) {
		m_geometry.m_vertices.resize( nOldVertices );
		m_geometry.m_clusterVertexIndices.resize( nOldIndices );
		m_geometry.m_triangleWords.resize( nOldTriangles );
		m_geometry.m_clusters.resize( nOldClusters );
		m_geometry.m_clusterVtMetadata.resize( nOldClusters );
		m_geometry.m_materials.resize( nOldMaterials );
		m_geometry.m_instanceMaterialStableKeys.resize( nOldMaterials );
		m_sources.resize( nOldSources );
        m_dirtyRanges.resize( nOldDirtyRanges );
        m_nMutationRevision = nOldMutationRevision;
        m_nRecycledSpans = nOldRecycledSpans;
        m_nReusedGeometryBytes = nOldReusedGeometryBytes;
        m_pendingRangeClaims.clear();
		m_nGeometryRevision = nOldGeometryRevision;
		m_nMaterialRevision = nOldMaterialRevision;
		m_nDeduplicatedSources = nOldDeduplicatedSources;
		m_nDeduplicatedGpuBytes = nOldDeduplicatedGpuBytes;
		m_pendingActiveSources.clear();
		m_pendingStaticMaterialSlots.clear();
		++m_nCacheAdmissionFailures;
		return false;
	}
    // Commit all claims atomically. Each claim consumed an independent stream
    // prefix; unclaimed suffixes remain recyclable and may coalesce later.
    for ( uint32_t stream = 0u; stream < 4u; ++stream ) {
        eastl::vector< t3dWorldGeometryFreeRange_t, Terrain3DEastlAllocator > &free = m_freeRanges[stream];
        for ( size_t slot = free.size(); slot > 0u; --slot ) {
            uint32_t nConsumed = 0u;
            for ( uint32_t p = 0u; p < m_pendingRangeClaims.size(); ++p ) {
                const t3dWorldGeometryRangeClaim_t &claim = m_pendingRangeClaims[p];
                if ( claim.m_nStream == stream && claim.m_nSlot == slot - 1u ) {
                    nConsumed += claim.m_nCount;
                }
            }
            if ( nConsumed == 0u ) { continue; }
            free[slot - 1u].m_nFirst += nConsumed;
            free[slot - 1u].m_nCount -= nConsumed;
            if ( free[slot - 1u].m_nCount == 0u ) {
                free.erase( free.begin() + slot - 1u );
            }
        }
    }
    m_pendingRangeClaims.clear();
    // Keep only the most recent overwrite for each physical byte-range start.
    // The latest source completely owns that tuple; earlier revisions can no
    // longer be referenced after the certified retirement and source switch.
    // This bounds the journal by distinct recycled physical slots rather than
    // by the total number of load/unload cycles.
    if ( m_dirtyRanges.size() != nOldDirtyRanges ) {
    eastl::sort( m_dirtyRanges.begin(), m_dirtyRanges.end(),
        []( const t3dWorldGeometryDirtyRange_t &a, const t3dWorldGeometryDirtyRange_t &b ) {
            if ( a.m_nBuffer != b.m_nBuffer ) { return a.m_nBuffer < b.m_nBuffer; }
            if ( a.m_nFirstByte != b.m_nFirstByte ) { return a.m_nFirstByte < b.m_nFirstByte; }
            return a.m_nRevision > b.m_nRevision;
        } );
    size_t nWrite = 0u;
    for ( size_t i = 0u; i < m_dirtyRanges.size(); ++i ) {
        if ( nWrite && m_dirtyRanges[nWrite - 1u].m_nBuffer == m_dirtyRanges[i].m_nBuffer &&
             m_dirtyRanges[nWrite - 1u].m_nFirstByte == m_dirtyRanges[i].m_nFirstByte ) { continue; }
        if ( nWrite != i ) { m_dirtyRanges[nWrite] = m_dirtyRanges[i]; }
        ++nWrite;
    }
    m_dirtyRanges.resize( nWrite );
    }
	eastl::sort( m_pendingActiveSources.begin(), m_pendingActiveSources.end(),
		[this]( const uint32_t a, const uint32_t b ) {
			return m_sources[a].m_nSourceKey < m_sources[b].m_nSourceKey;
		} );
	m_activeSources.swap( m_pendingActiveSources );
	m_staticMaterialSlots.swap( m_pendingStaticMaterialSlots );
	m_nActiveStaticSource = nNewStaticIndex;
	// Commit logical leases only after successful transactional geometry admission.
	// Physical allocations are independently quarantined until GPU-safe retirement.
	for ( uint32_t i = 0u; i < m_sources.size(); ++i ) {
		if ( m_sources[i].m_nPhysicalRecord == i && m_sources[i].m_nActiveSourceLeases != 0u ) {
            m_sources[i].m_nRetirePublicationGeneration = UINT64_MAX;
        }
        m_sources[i].m_nActiveSourceLeases = 0u;
	}
	for ( uint32_t i = 0u; i < m_activeSources.size(); ++i ) {
		const uint32_t nPhysical = m_sources[m_activeSources[i]].m_nPhysicalRecord;
		++m_sources[nPhysical].m_nActiveSourceLeases;
        m_sources[nPhysical].m_nRetirePublicationGeneration = 0u;
	}
	if ( m_nActiveStaticSource != UINT32_MAX ) {
		const uint32_t nPhysical = m_sources[m_nActiveStaticSource].m_nPhysicalRecord;
		++m_sources[nPhysical].m_nActiveSourceLeases;
        m_sources[nPhysical].m_nRetirePublicationGeneration = 0u;
	}
    for ( uint32_t i = 0u; i < m_sources.size(); ++i ) {
        if ( m_sources[i].m_nPhysicalRecord == i && m_sources[i].m_nActiveSourceLeases == 0u &&
             m_sources[i].m_nRetirePublicationGeneration == 0u ) {
            m_sources[i].m_nRetirePublicationGeneration = UINT64_MAX;
        }
    }
	return true;
}

bool Terrain3DWorldGeometryCache::findActiveSpan( const uint64_t nSourceKey,
											 uint32_t &rFirstCluster, uint32_t &rClusterCount ) const
{
	size_t low = 0u;
	size_t high = m_activeSources.size();
	while ( low < high ) {
		const size_t middle = low + ( high - low ) / 2u;
		if ( m_sources[m_activeSources[middle]].m_nSourceKey < nSourceKey ) {
			low = middle + 1u;
		} else {
			high = middle;
		}
	}
	if ( low < m_activeSources.size() && m_sources[m_activeSources[low]].m_nSourceKey == nSourceKey ) {
		const t3dWorldResidentGeometrySpan_t &span = m_sources[m_activeSources[low]];
		rFirstCluster = span.m_nClusterBase;
		rClusterCount = span.m_nClusterCount;
		return true;
	}
	rFirstCluster = rClusterCount = 0u;
	return false;
}

bool Terrain3DWorldGeometryCache::getActiveSourceHandle( const uint64_t nSourceKey,
	t3dWorldMeshSourceHandle_t &rHandle ) const
{
	rHandle = {};
	size_t low = 0u, high = m_activeSources.size();
	while ( low < high ) {
		const size_t middle = low + ( high - low ) / 2u;
		if ( m_sources[m_activeSources[middle]].m_nSourceKey < nSourceKey ) {
			low = middle + 1u;
		} else {
			high = middle;
		}
	}
	if ( low >= m_activeSources.size() || m_sources[m_activeSources[low]].m_nSourceKey != nSourceKey ) {
		return false;
	}
	rHandle.m_nSlot = m_activeSources[low];
	rHandle.m_nEpoch = ( uint32_t ) m_nCacheEpoch;
	return true;
}

bool Terrain3DWorldGeometryCache::resolveActiveSourceHandle( const t3dWorldMeshSourceHandle_t &rHandle,
	uint32_t &rFirstCluster, uint32_t &rClusterCount ) const
{
	rFirstCluster = 0u;
	rClusterCount = 0u;
	if ( rHandle.m_nSlot >= m_sources.size() || rHandle.m_nEpoch != ( uint32_t ) m_nCacheEpoch ||
		rHandle.m_nSlot == UINT32_MAX ) {
		return false;
	}
	const t3dWorldResidentGeometrySpan_t &span = m_sources[rHandle.m_nSlot];
	if ( rHandle.m_nSlot != m_nActiveStaticSource ) {
		t3dWorldMeshSourceHandle_t active{};
		if ( !getActiveSourceHandle( span.m_nSourceKey, active ) || active.m_nSlot != rHandle.m_nSlot ) {
			return false;
		}
	}
	rFirstCluster = span.m_nClusterBase;
	rClusterCount = span.m_nClusterCount;
	return true;
}

bool Terrain3DWorldGeometryCache::getStaticSpan( uint32_t &rFirstCluster, uint32_t &rClusterCount ) const
{
	if ( m_nActiveStaticSource == UINT32_MAX ) {
		rFirstCluster = rClusterCount = 0u;
		return false;
	}
	const t3dWorldResidentGeometrySpan_t &span = m_sources[m_nActiveStaticSource];
	rFirstCluster = span.m_nClusterBase;
	rClusterCount = span.m_nClusterCount;
	return true;
}

bool Terrain3DWorldGeometryCache::getStaticMaterialSlot( const uint32_t nLocalMaterial, uint32_t &rSlot ) const
{
	if ( nLocalMaterial >= m_staticMaterialSlots.size() ) {
		return false;
	}
	rSlot = m_staticMaterialSlots[nLocalMaterial];
	return true;
}

bool Terrain3DWorldGeometryCache::copyGeometryInto( Terrain3DWorldPublication &rWorld ) const
{
    // Legacy standalone test/editor publications still receive a real vector copy.
    if ( !rWorld.usesSharedGeometry() ) {
        const bool bRetainPrefix = rWorld.m_nStableGeometryEpoch == m_nCacheEpoch &&
            rWorld.m_vertices.size() <= m_geometry.m_vertices.size() &&
            rWorld.m_clusterVertexIndices.size() <= m_geometry.m_clusterVertexIndices.size() &&
            rWorld.m_triangleWords.size() <= m_geometry.m_triangleWords.size() &&
            rWorld.m_clusters.size() <= m_geometry.m_clusters.size() &&
            rWorld.m_clusterVtMetadata.size() <= m_geometry.m_clusterVtMetadata.size();
        if ( !bRetainPrefix ) {
            rWorld.m_vertices.clear(); rWorld.m_clusterVertexIndices.clear();
            rWorld.m_triangleWords.clear(); rWorld.m_clusters.clear(); rWorld.m_clusterVtMetadata.clear();
        }
        if ( rWorld.m_vertices.capacity() < m_geometry.m_vertices.size() ||
             rWorld.m_clusterVertexIndices.capacity() < m_geometry.m_clusterVertexIndices.size() ||
             rWorld.m_triangleWords.capacity() < m_geometry.m_triangleWords.size() ||
             rWorld.m_clusters.capacity() < m_geometry.m_clusters.size() ||
             rWorld.m_clusterVtMetadata.capacity() < m_geometry.m_clusterVtMetadata.size() ) { return false; }
        rWorld.m_vertices.insert( rWorld.m_vertices.end(), m_geometry.m_vertices.begin() + rWorld.m_vertices.size(), m_geometry.m_vertices.end() );
        rWorld.m_clusterVertexIndices.insert( rWorld.m_clusterVertexIndices.end(),
            m_geometry.m_clusterVertexIndices.begin() + rWorld.m_clusterVertexIndices.size(), m_geometry.m_clusterVertexIndices.end() );
        rWorld.m_triangleWords.insert( rWorld.m_triangleWords.end(),
            m_geometry.m_triangleWords.begin() + rWorld.m_triangleWords.size(), m_geometry.m_triangleWords.end() );
        rWorld.m_clusters.insert( rWorld.m_clusters.end(), m_geometry.m_clusters.begin() + rWorld.m_clusters.size(), m_geometry.m_clusters.end() );
        rWorld.m_clusterVtMetadata.insert( rWorld.m_clusterVtMetadata.end(),
            m_geometry.m_clusterVtMetadata.begin() + rWorld.m_clusterVtMetadata.size(), m_geometry.m_clusterVtMetadata.end() );
        if ( bRetainPrefix && rWorld.m_nGeometryMutationRevision != m_nMutationRevision ) {
            const uint32_t strides[5] = { sizeof( t3dWorldVertex_t ), sizeof( uint32_t ), sizeof( uint32_t ),
                sizeof( t3dWorldCluster_t ), sizeof( t3dWorldClusterVTMetadata_t ) };
            void *dest[5] = { rWorld.m_vertices.data(), rWorld.m_clusterVertexIndices.data(),
                rWorld.m_triangleWords.data(), rWorld.m_clusters.data(), rWorld.m_clusterVtMetadata.data() };
            const void *src[5] = { m_geometry.m_vertices.data(), m_geometry.m_clusterVertexIndices.data(),
                m_geometry.m_triangleWords.data(), m_geometry.m_clusters.data(), m_geometry.m_clusterVtMetadata.data() };
            const uint32_t available[5] = { ( uint32_t ) rWorld.m_vertices.size(),
                ( uint32_t ) rWorld.m_clusterVertexIndices.size(), ( uint32_t ) rWorld.m_triangleWords.size(),
                ( uint32_t ) rWorld.m_clusters.size(), ( uint32_t ) rWorld.m_clusterVtMetadata.size() };
            for ( size_t i = 0u; i < m_dirtyRanges.size(); ++i ) {
                const t3dWorldGeometryDirtyRange_t &change = m_dirtyRanges[i];
                if ( change.m_nRevision <= rWorld.m_nGeometryMutationRevision || change.m_nBuffer >= 5u ) { continue; }
                const uint32_t nCapacity = available[change.m_nBuffer] * strides[change.m_nBuffer];
                if ( change.m_nFirstByte >= nCapacity ) { continue; }
                const uint32_t nBytes = MIN( change.m_nByteCount, nCapacity - change.m_nFirstByte );
                memcpy( ( uint8_t * ) dest[change.m_nBuffer] + change.m_nFirstByte,
                    ( const uint8_t * ) src[change.m_nBuffer] + change.m_nFirstByte, nBytes );
            }
        }
    } else {
        // Active-source membership can change without touching physical geometry.
        // Only an actual geometry mutation or cache reset needs a new frozen copy.
        if ( !m_frozenGeometry.get() || m_nFrozenEpoch != m_nCacheEpoch ||
             m_nFrozenMutationRevision != m_nMutationRevision ||
             m_frozenGeometry.get()->m_vertices.size() != m_geometry.m_vertices.size() ||
             m_frozenGeometry.get()->m_clusterVertexIndices.size() != m_geometry.m_clusterVertexIndices.size() ||
             m_frozenGeometry.get()->m_triangleWords.size() != m_geometry.m_triangleWords.size() ||
             m_frozenGeometry.get()->m_clusters.size() != m_geometry.m_clusters.size() ||
             m_frozenGeometry.get()->m_clusterVtMetadata.size() != m_geometry.m_clusterVtMetadata.size() ) {
            Terrain3DSharedWorldGeometry *pFrozen = new ( std::nothrow ) Terrain3DSharedWorldGeometry();
            if ( !pFrozen ) { return false; }
            pFrozen->bindRuntimeAllocator( m_geometry.m_vertices.get_allocator() );
            if ( !pFrozen->copyCold( m_geometry ) ) { pFrozen->release(); return false; }
            m_frozenGeometry.adopt( pFrozen );
            m_nFrozenEpoch = m_nCacheEpoch;
            m_nFrozenGeometryRevision = m_nGeometryRevision;
            m_nFrozenMutationRevision = m_nMutationRevision;
        }
        rWorld.attachSharedGeometryCold( m_frozenGeometry );
    }
    if ( !t3dContainerReserveCold( rWorld.m_geometryDirtyRanges, m_dirtyRanges.size() ) ) { return false; }
    rWorld.m_geometryDirtyRanges.assign( m_dirtyRanges.begin(), m_dirtyRanges.end() );
    rWorld.m_nGeometryMutationRevision = m_nMutationRevision;
    rWorld.m_nStableGeometryEpoch = m_nCacheEpoch;
    return true;
}

void Terrain3DWorldGeometryCache::tagRetiredSourcesCold( const uint64_t nPublicationGeneration )
{
    if ( nPublicationGeneration == 0u ) {
        return;
    }
    for ( uint32_t i = 0u; i < m_sources.size(); ++i ) {
        t3dWorldResidentGeometrySpan_t &span = m_sources[i];
        if ( span.m_nPhysicalRecord == i && span.m_nActiveSourceLeases == 0u &&
             span.m_nRetirePublicationGeneration == UINT64_MAX ) {
            span.m_nRetirePublicationGeneration = nPublicationGeneration;
        }
    }
}

bool Terrain3DWorldGeometryCache::releaseCompletedRetirementsCold(
    const uint64_t nCompletedPublicationGeneration )
{
    if ( !nCompletedPublicationGeneration ) { return false; }
    uint32_t nReady = 0u;
    for ( uint32_t i = 0u; i < m_sources.size(); ++i ) {
        const t3dWorldResidentGeometrySpan_t &span = m_sources[i];
        if ( span.m_nPhysicalRecord == i && span.m_nActiveSourceLeases == 0u &&
             span.m_nRetirePublicationGeneration != 0u &&
             span.m_nRetirePublicationGeneration != UINT64_MAX &&
             span.m_nRetirePublicationGeneration <= nCompletedPublicationGeneration ) {
            ++nReady;
        }
    }
    if ( !nReady ) { return false; }
    for ( uint32_t stream = 0u; stream < 4u; ++stream ) {
        if ( !t3dContainerReserveCold( m_freeRanges[stream], m_freeRanges[stream].size() + nReady ) ) {
            return false;
        }
    }
    for ( uint32_t i = 0u; i < m_sources.size(); ++i ) {
        t3dWorldResidentGeometrySpan_t &span = m_sources[i];
        if ( span.m_nPhysicalRecord != i || span.m_nActiveSourceLeases != 0u ||
             span.m_nRetirePublicationGeneration == 0u ||
             span.m_nRetirePublicationGeneration == UINT64_MAX ||
             span.m_nRetirePublicationGeneration > nCompletedPublicationGeneration ) { continue; }
        const uint32_t bases[4] = { span.m_nVertexBase, span.m_nIndexBase,
            span.m_nTriangleBase, span.m_nClusterBase };
        const uint32_t counts[4] = { span.m_nReservedVertexCount, span.m_nReservedIndexCount,
            span.m_nReservedTriangleCount, span.m_nReservedClusterCount };
        for ( uint32_t stream = 0u; stream < 4u; ++stream ) {
            if ( counts[stream] ) { m_freeRanges[stream].push_back( { bases[stream], counts[stream] } ); }
        }
        // Mark the canonical record as retired. Aliases are invalidated in a
        // second linear scan to avoid a quadratic walk for large HLOD unloads.
        span.m_nPhysicalRecord = UINT32_MAX;
    }
    for ( uint32_t stream = 0u; stream < 4u; ++stream ) {
        eastl::vector< t3dWorldGeometryFreeRange_t, Terrain3DEastlAllocator > &free = m_freeRanges[stream];
        eastl::sort( free.begin(), free.end(), []( const t3dWorldGeometryFreeRange_t &a,
            const t3dWorldGeometryFreeRange_t &b ) { return a.m_nFirst < b.m_nFirst; } );
        size_t nWrite = 0u;
        for ( size_t j = 0u; j < free.size(); ++j ) {
            if ( nWrite && uint64_t( free[nWrite - 1u].m_nFirst ) + free[nWrite - 1u].m_nCount ==
                    free[j].m_nFirst ) {
                free[nWrite - 1u].m_nCount += free[j].m_nCount;
            } else {
                if ( nWrite != j ) { free[nWrite] = free[j]; }
                ++nWrite;
            }
        }
        free.resize( nWrite );
    }
    for ( uint32_t i = 0u; i < m_sources.size(); ++i ) {
        const uint32_t nPhysical = m_sources[i].m_nPhysicalRecord;
        if ( nPhysical != UINT32_MAX && nPhysical < m_sources.size() &&
             m_sources[nPhysical].m_nPhysicalRecord == UINT32_MAX ) {
            m_sources[i].m_nPhysicalRecord = UINT32_MAX;
        }
    }
    return true;
}

bool Terrain3DWorldGeometryCache::compactAfterGpuCompletionCold(
    const uint64_t nCompletedPublicationGeneration, const t3dWorldResourceLimits_t &rLimits )
{
    if ( nCompletedPublicationGeneration == 0u ) {
        return false;
    }
    // Do not reclaim *any* geometry unless every quarantined physical range has
    // been excluded from a WORLD publication that the GPU has finished reading.
    uint32_t nRetired = 0u;
    for ( uint32_t i = 0u; i < m_sources.size(); ++i ) {
        const t3dWorldResidentGeometrySpan_t &span = m_sources[i];
        if ( span.m_nPhysicalRecord != i || span.m_nActiveSourceLeases != 0u ) {
            continue;
        }
        if ( span.m_nRetirePublicationGeneration == 0u ||
             span.m_nRetirePublicationGeneration == UINT64_MAX ||
             span.m_nRetirePublicationGeneration > nCompletedPublicationGeneration ) {
            return false;
        }
        ++nRetired;
    }
    if ( !nRetired ) {
        return false;
    }
    Terrain3DWorldGeometryCache replacement;
    replacement.bindRuntimeAllocator( m_sources.get_allocator() );
    if ( !t3dContainerReserveCold( replacement.m_activeSources, m_activeSources.size() ) ||
         !t3dContainerReserveCold( replacement.m_staticMaterialSlots, m_staticMaterialSlots.size() ) ) {
        return false;
    }
    const Terrain3DWorldPublication &g = m_geometry;
    for ( uint32_t i = 0u; i < m_activeSources.size(); ++i ) {
        const t3dWorldResidentGeometrySpan_t &source = m_sources[m_activeSources[i]];
        const t3dWorldResidentGeometrySpan_t &physical = m_sources[source.m_nPhysicalRecord];
        uint32_t nNewRecord = UINT32_MAX;
        if ( !replacement.registerSourceCold( source.m_nSourceKey, physical,
            g.m_vertices.data(), g.m_clusterVertexIndices.data(), g.m_triangleWords.data(),
            g.m_clusters.data(), g.m_clusterVtMetadata.data(), g.m_materials.data(),
            g.m_instanceMaterialStableKeys.data(), ( uint32_t ) g.m_materials.size(),
            rLimits, nNewRecord ) ) {
            return false;
        }
        replacement.m_activeSources.push_back( nNewRecord );
    }
    if ( m_nActiveStaticSource != UINT32_MAX ) {
        const t3dWorldResidentGeometrySpan_t &source = m_sources[m_nActiveStaticSource];
        const t3dWorldResidentGeometrySpan_t &physical = m_sources[source.m_nPhysicalRecord];
        uint32_t nNewRecord = UINT32_MAX;
        if ( !replacement.registerSourceCold( source.m_nSourceKey, physical,
            g.m_vertices.data(), g.m_clusterVertexIndices.data(), g.m_triangleWords.data(),
            g.m_clusters.data(), g.m_clusterVtMetadata.data(), g.m_materials.data(),
            g.m_instanceMaterialStableKeys.data(), ( uint32_t ) g.m_materials.size(),
            rLimits, nNewRecord ) ) {
            return false;
        }
        replacement.m_nActiveStaticSource = nNewRecord;
    }
    for ( uint32_t i = 0u; i < m_staticMaterialSlots.size(); ++i ) {
        const uint32_t oldSlot = m_staticMaterialSlots[i];
        if ( oldSlot >= g.m_materials.size() ) {
            return false;
        }
        uint32_t nNewSlot = UINT32_MAX;
        if ( !replacement.resolveMaterialCold( g.m_materials[oldSlot],
            g.m_instanceMaterialStableKeys[oldSlot], rLimits.m_nMaxMaterials, nNewSlot ) ) {
            return false;
        }
        replacement.m_staticMaterialSlots.push_back( nNewSlot );
    }
    for ( uint32_t i = 0u; i < replacement.m_sources.size(); ++i ) {
        replacement.m_sources[i].m_nActiveSourceLeases = 0u;
    }
    for ( uint32_t i = 0u; i < replacement.m_activeSources.size(); ++i ) {
        ++replacement.m_sources[replacement.m_sources[replacement.m_activeSources[i]].m_nPhysicalRecord].m_nActiveSourceLeases;
    }
    if ( replacement.m_nActiveStaticSource != UINT32_MAX ) {
        ++replacement.m_sources[replacement.m_sources[replacement.m_nActiveStaticSource].m_nPhysicalRecord].m_nActiveSourceLeases;
    }
    const uint64_t nOldResidentBytes = uint64_t( g.m_vertices.size() ) * sizeof( t3dWorldVertex_t ) +
        uint64_t( g.m_clusterVertexIndices.size() + g.m_triangleWords.size() ) * sizeof( uint32_t ) +
        uint64_t( g.m_clusters.size() ) * ( sizeof( t3dWorldCluster_t ) + sizeof( t3dWorldClusterVTMetadata_t ) );
    const uint64_t nNewResidentBytes = uint64_t( replacement.m_geometry.m_vertices.size() ) * sizeof( t3dWorldVertex_t ) +
        uint64_t( replacement.m_geometry.m_clusterVertexIndices.size() + replacement.m_geometry.m_triangleWords.size() ) * sizeof( uint32_t ) +
        uint64_t( replacement.m_geometry.m_clusters.size() ) * ( sizeof( t3dWorldCluster_t ) + sizeof( t3dWorldClusterVTMetadata_t ) );
    // All allocations and recooks succeeded. Swap only now: failure preserves
    // the last resident snapshot and no published bank sees a partial migration.
    m_geometry.m_vertices.swap( replacement.m_geometry.m_vertices );
    m_geometry.m_clusterVertexIndices.swap( replacement.m_geometry.m_clusterVertexIndices );
    m_geometry.m_triangleWords.swap( replacement.m_geometry.m_triangleWords );
    m_geometry.m_clusters.swap( replacement.m_geometry.m_clusters );
    m_geometry.m_clusterVtMetadata.swap( replacement.m_geometry.m_clusterVtMetadata );
    m_geometry.m_materials.swap( replacement.m_geometry.m_materials );
    m_geometry.m_instanceMaterialStableKeys.swap( replacement.m_geometry.m_instanceMaterialStableKeys );
    m_sources.swap( replacement.m_sources );
    m_activeSources.swap( replacement.m_activeSources );
    m_staticMaterialSlots.swap( replacement.m_staticMaterialSlots );
    for ( uint32_t i = 0u; i < 4u; ++i ) { m_freeRanges[i].clear(); }
    m_pendingRangeClaims.clear();
    m_dirtyRanges.clear();
    m_nMutationRevision = 0u;
    m_nActiveStaticSource = replacement.m_nActiveStaticSource;
    m_nCacheEpoch = t3dNextGeneration( m_nCacheEpoch );
    m_nGeometryRevision = t3dNextGeneration( m_nGeometryRevision );
    m_nMaterialRevision = t3dNextGeneration( m_nMaterialRevision );
    ++m_nSafeCompactionCount;
    m_nReclaimedGeometryBytes += nOldResidentBytes > nNewResidentBytes ? nOldResidentBytes - nNewResidentBytes : 0u;
    return true;
}

static uint64_t t3dWorldGeometrySpanByteCount( const t3dWorldResidentGeometrySpan_t &rSpan )
{
	return uint64_t( rSpan.m_nVertexCount ) * sizeof( t3dWorldVertex_t ) +
		uint64_t( rSpan.m_nIndexCount + rSpan.m_nTriangleCount ) * sizeof( uint32_t ) +
		uint64_t( rSpan.m_nClusterCount ) * ( sizeof( t3dWorldCluster_t ) + sizeof( t3dWorldClusterVTMetadata_t ) );
}

uint64_t Terrain3DWorldGeometryCache::getFreeGeometryBytes( void ) const
{
    const uint32_t strides[4] = { sizeof( t3dWorldVertex_t ), sizeof( uint32_t ),
        sizeof( uint32_t ), sizeof( t3dWorldCluster_t ) + sizeof( t3dWorldClusterVTMetadata_t ) };
    uint64_t nBytes = 0u;
    for ( uint32_t stream = 0u; stream < 4u; ++stream ) {
        for ( uint32_t i = 0u; i < m_freeRanges[stream].size(); ++i ) {
            nBytes += uint64_t( m_freeRanges[stream][i].m_nCount ) * strides[stream];
        }
    }
    return nBytes;
}

uint32_t Terrain3DWorldGeometryCache::getFreeGeometryRangeCount( void ) const
{
    uint32_t nCount = 0u;
    for ( uint32_t i = 0u; i < 4u; ++i ) { nCount += ( uint32_t ) m_freeRanges[i].size(); }
    return nCount;
}

uint64_t Terrain3DWorldGeometryCache::getLiveGeometryBytes( void ) const
{
	uint64_t nBytes = 0u;
	for ( uint32_t i = 0u; i < m_sources.size(); ++i ) {
		if ( m_sources[i].m_nPhysicalRecord != i ) {
			continue;
		}
		if ( m_sources[i].m_nActiveSourceLeases != 0u ) {
			nBytes += t3dWorldGeometrySpanByteCount( m_sources[i] );
		}
	}
	return nBytes;
}

uint32_t Terrain3DWorldGeometryCache::getPhysicalSourceLeaseCount( const uint64_t nSourceKey ) const
{
	for ( size_t i = m_sources.size(); i > 0u; --i ) {
		if ( m_sources[i - 1u].m_nSourceKey == nSourceKey ) {
			const uint32_t nPhysical = m_sources[i - 1u].m_nPhysicalRecord;
			return nPhysical < m_sources.size() ? m_sources[nPhysical].m_nActiveSourceLeases : 0u;
		}
	}
	return 0u;
}

uint64_t Terrain3DWorldGeometryCache::getQuarantinedGeometryBytes( void ) const
{
	uint64_t nPhysicalBytes = 0u;
	for ( uint32_t i = 0u; i < m_sources.size(); ++i ) {
		if ( m_sources[i].m_nPhysicalRecord == i ) {
			nPhysicalBytes += t3dWorldGeometrySpanByteCount( m_sources[i] );
		}
	}
	return nPhysicalBytes - getLiveGeometryBytes();
}

size_t Terrain3DWorldGeometryCache::getRetainedCpuBytes( void ) const
{
    return m_geometry.getRetainedCpuBytes() +
        ( m_frozenGeometry.get() ? m_frozenGeometry.get()->getRetainedCpuBytes() : 0u ) +
        t3dContainerResidentBytes( m_sources ) +
		t3dContainerResidentBytes( m_activeSources ) + t3dContainerResidentBytes( m_pendingActiveSources ) +
		t3dContainerResidentBytes( m_staticMaterialSlots ) +
		t3dContainerResidentBytes( m_pendingStaticMaterialSlots ) +
        t3dContainerResidentBytes( m_freeRanges[0] ) + t3dContainerResidentBytes( m_freeRanges[1] ) +
        t3dContainerResidentBytes( m_freeRanges[2] ) + t3dContainerResidentBytes( m_freeRanges[3] ) +
        t3dContainerResidentBytes( m_pendingRangeClaims ) + t3dContainerResidentBytes( m_dirtyRanges );
}
