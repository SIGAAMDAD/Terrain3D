// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/terrain_3d_world_resources.h"
#include "rendering/rd/infrastructure/terrain_3d_gpu_upload_service.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"

#include <string.h>

using namespace godot;

static uint32_t t3dWorldClampBufferBytes( const uint64_t nBytes ) {
	if ( nBytes == 0u ) {
		return 16u;
	}
	return nBytes > UINT32_MAX ? 0u : ( uint32_t )nBytes;
}

uint32_t Terrain3DWorldResources::requiredBytes( const Terrain3DWorldPublication &rPublication, const t3dWorldBufferId_t eBuffer ) {
	switch ( eBuffer ) {
		case WORLD_BUFFER_VERTICES:
			return t3dWorldClampBufferBytes( rPublication.m_vertices.size() * sizeof( t3dWorldVertex_t ) );
		case WORLD_BUFFER_CLUSTER_VERTEX_INDICES:
			return t3dWorldClampBufferBytes( rPublication.m_clusterVertexIndices.size() * sizeof( uint32_t ) );
		case WORLD_BUFFER_TRIANGLES:
			return t3dWorldClampBufferBytes( rPublication.m_triangleWords.size() * sizeof( uint32_t ) );
		case WORLD_BUFFER_CLUSTERS:
			return t3dWorldClampBufferBytes( rPublication.m_clusters.size() * sizeof( t3dWorldCluster_t ) );
		case WORLD_BUFFER_CLUSTER_VT_METADATA:
			return t3dWorldClampBufferBytes( rPublication.m_clusterVtMetadata.size() * sizeof( t3dWorldClusterVtMetadata_t ) );
		case WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS:
			return t3dWorldClampBufferBytes( rPublication.m_terrainStripClusters.size() * sizeof( t3dWorldTerrainCluster_t ) );
		case WORLD_BUFFER_TERRAIN_STRIP_CLUSTER_BOUNDS:
			return t3dWorldClampBufferBytes( rPublication.m_terrainStripClusterBounds.size() * sizeof( t3dWorldTerrainClusterBounds_t ) );
		case WORLD_BUFFER_TRANSFORMS_CURRENT:
			return t3dWorldClampBufferBytes( rPublication.m_transforms.size() * sizeof( t3dWorldTransform_t ) );
		case WORLD_BUFFER_TRANSFORMS_PREVIOUS:
			return t3dWorldClampBufferBytes( rPublication.m_previousTransforms.size() * sizeof( t3dWorldTransform_t ) );
		case WORLD_BUFFER_INSTANCES:
			return t3dWorldClampBufferBytes( rPublication.m_instances.size() * sizeof( t3dWorldInstance_t ) );
		case WORLD_BUFFER_CELLS:
			return t3dWorldClampBufferBytes( rPublication.m_cells.size() * sizeof( t3dWorldCell_t ) );
		case WORLD_BUFFER_HLOD_RECORDS:
			return t3dWorldClampBufferBytes( rPublication.m_hlodRecords.size() * sizeof( t3dWorldHlodRecord_t ) );
		case WORLD_BUFFER_MATERIALS:
			return t3dWorldClampBufferBytes( rPublication.m_materials.size() * sizeof( t3dRenderingMaterialGpu_t ) );
		case WORLD_BUFFER_FRAME:
			return sizeof( t3dWorldFrame_t );
		case WORLD_BUFFER_VISIBILITY_CANDIDATES:
			return t3dWorldClampBufferBytes( rPublication.m_visibilityCandidates.size() * sizeof( t3dWorldVisibleItem_t ) );
		case WORLD_BUFFER_VISIBLE_ITEMS:
			return t3dWorldClampBufferBytes( rPublication.m_visibilityCandidates.size() * sizeof( t3dWorldVisibleItem_t ) );
		case WORLD_BUFFER_VISIBILITY_COUNTERS:
			return T3D_WORLD_VIS_COUNTER_COUNT * sizeof( uint32_t );
		case WORLD_BUFFER_INDIRECT:
			return T3D_WORLD_INDIRECT_BUFFER_SIZE_BYTES;
		default:
			return 0u;
	}
}

Terrain3DRdContext::MemoryCategory Terrain3DWorldResources::memoryCategory( const t3dWorldBufferId_t eBuffer ) {
	switch ( eBuffer ) {
		case WORLD_BUFFER_VERTICES:
			return Terrain3DRdContext::MEMORY_MESH_VERTEX_ARENA;
		case WORLD_BUFFER_CLUSTER_VERTEX_INDICES:
		case WORLD_BUFFER_TRIANGLES:
		case WORLD_BUFFER_CLUSTERS:
		case WORLD_BUFFER_CLUSTER_VT_METADATA:
		case WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS:
		case WORLD_BUFFER_TERRAIN_STRIP_CLUSTER_BOUNDS:
			return Terrain3DRdContext::MEMORY_MESH_INDEX_ARENA;
		case WORLD_BUFFER_TRANSFORMS_CURRENT:
		case WORLD_BUFFER_TRANSFORMS_PREVIOUS:
		case WORLD_BUFFER_INSTANCES:
		case WORLD_BUFFER_CELLS:
			return Terrain3DRdContext::MEMORY_INSTANCE;
		case WORLD_BUFFER_HLOD_RECORDS:
			return Terrain3DRdContext::MEMORY_HLOD;
		case WORLD_BUFFER_MATERIALS:
			return Terrain3DRdContext::MEMORY_MATERIAL_TABLE;
		case WORLD_BUFFER_FRAME:
		case WORLD_BUFFER_VISIBILITY_CANDIDATES:
		case WORLD_BUFFER_VISIBLE_ITEMS:
		case WORLD_BUFFER_VISIBILITY_COUNTERS:
			return Terrain3DRdContext::MEMORY_STORAGE;
		case WORLD_BUFFER_INDIRECT:
			return Terrain3DRdContext::MEMORY_INDIRECT;
		default:
			return Terrain3DRdContext::MEMORY_STORAGE;
	}
}

const char *Terrain3DWorldResources::bufferName( const t3dWorldBufferId_t eBuffer ) {
	static const char *const names[WORLD_BUFFER_COUNT] = { "WorldRuntime.Vertices",
		"WorldRuntime.ClusterVertexIndices",
		"WorldRuntime.Triangles",
		"WorldRuntime.Clusters",
		"WorldRuntime.ClusterVtMetadata",
		"WorldRuntime.TerrainStripClusters",
		"WorldRuntime.TerrainStripClusterBounds",
		"WorldRuntime.TransformsCurrent",
		"WorldRuntime.TransformsPrevious",
		"WorldRuntime.Instances",
		"WorldRuntime.Cells",
		"WorldRuntime.HLOD",
		"WorldRuntime.Materials",
		"WorldRuntime.Frame",
		"WorldRuntime.VisibilityCandidates",
		"WorldRuntime.VisibleItems",
		"WorldRuntime.VisibilityCounters",
		"WorldRuntime.Indirect" };
	return eBuffer < WORLD_BUFFER_COUNT ? names[eBuffer] : "WorldRuntime.Invalid";
}

bool Terrain3DWorldResources::ensureScratchCold( const t3dWorldBufferId_t eBuffer, const uint32_t nRequiredBytes ) {
	PackedByteArray &scratch = m_uploadScratch[eBuffer];
	if ( scratch.size() >= ( int64_t )nRequiredBytes ) {
		return true;
	}
	scratch.resize( nRequiredBytes );
	return scratch.size() >= ( int64_t )nRequiredBytes;
}

bool Terrain3DWorldResources::ensureBufferCold( const t3dWorldBufferId_t eBuffer, const uint32_t nRequiredBytes ) {
	static_assert( T3D_WORLD_INDIRECT_BUFFER_SIZE_BYTES
	                       <= Terrain3DRdContext::fixedIndirectCapacity( Terrain3DRdContext::FIXED_INDIRECT_WORLD ),
	        "World indirect commands exceed the shared fixed slice" );
	if ( eBuffer >= WORLD_BUFFER_COUNT || nRequiredBytes == 0u || !m_pContext || !m_pContext->getDevice() ) {
		return false;
	}
	if ( m_hBuffers[eBuffer].is_valid() && m_nCapacityBytes[eBuffer] >= nRequiredBytes ) {
		return ensureScratchCold( eBuffer, m_nCapacityBytes[eBuffer] );
	}
	if ( eBuffer == WORLD_BUFFER_INDIRECT ) {
		const uint32_t capacity = Terrain3DRdContext::fixedIndirectCapacity( Terrain3DRdContext::FIXED_INDIRECT_WORLD );
		if ( !ensureScratchCold( eBuffer, capacity ) ) {
			return false;
		}
		uint32_t offset = 0u;
		if ( !m_pContext->acquireFixedIndirect( Terrain3DRdContext::FIXED_INDIRECT_WORLD,
		             nRequiredBytes, m_hBuffers[eBuffer], offset ) ) {
			return false;
		}
		if ( offset != 0u ) {
			m_pContext->releaseFixedIndirect( Terrain3DRdContext::FIXED_INDIRECT_WORLD, m_hBuffers[eBuffer] );
			return false;
		}
		m_nCapacityBytes[eBuffer] = capacity;
		m_nGpuBufferBytes += m_nCapacityBytes[eBuffer];
		++m_nCapacityGrowthCount;
		++m_nResourceGeneration;
		return true;
	}
	const uint32_t nOldCapacity = m_nCapacityBytes[eBuffer];
	const uint64_t nGrowth = ( uint64_t )nRequiredBytes + ( uint64_t )nRequiredBytes / 2u + 256u;
	const uint32_t nCapacity = nGrowth > UINT32_MAX ? nRequiredBytes : MAX( nRequiredBytes, ( uint32_t )nGrowth );
	if ( nCapacity < nRequiredBytes ) {
		return false;
	}
	if ( !ensureScratchCold( eBuffer, nCapacity ) ) {
		return false;
	}
	const bool rebuilding = m_hBuffers[eBuffer].is_valid();
	const Terrain3DRdResourceService resources( *m_pContext, Terrain3DRdContext::OWNER_WORLD );
	const bool created_ok = resources.replaceBuffer( m_hBuffers[eBuffer], m_nCapacityBytes[eBuffer],
	        String::utf8( bufferName( eBuffer ) ), Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	        memoryCategory( eBuffer ), nCapacity );
	if ( !created_ok ) {
		return false;
	}
	if ( rebuilding ) {
		m_nGpuBufferBytes -= MIN( m_nGpuBufferBytes, ( uint64_t )nOldCapacity );
	}
	m_nGpuBufferBytes += nCapacity;
	++m_nCapacityGrowthCount;
	++m_nResourceGeneration;
	return true;
}

bool Terrain3DWorldResources::ensureCapacityCold( const Terrain3DWorldPublication &rPublication ) {
	if ( !m_pContext ) {
		m_pContext = &Terrain3DRdContext::getShared();
	}
	for ( uint32_t i = 0u; i < WORLD_BUFFER_COUNT; ++i ) {
		const t3dWorldBufferId_t eBuffer = ( t3dWorldBufferId_t )i;
		const uint32_t nRequired = requiredBytes( rPublication, eBuffer );
		if ( nRequired == 0u || !ensureBufferCold( eBuffer, nRequired ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DWorldResources::uploadBufferWarm( const t3dWorldBufferId_t eBuffer, const void *const pData, const uint32_t nBytes ) {
	m_nUsedBytes[eBuffer] = nBytes;
	if ( nBytes == 0u ) {
		return true;
	}
	if ( !pData || !m_pContext || !m_pContext->getDevice() || !m_hBuffers[eBuffer].is_valid() || nBytes > m_nCapacityBytes[eBuffer]
	        || m_uploadScratch[eBuffer].size() < ( int64_t )nBytes ) {
		return false;
	}
	const Terrain3DGpuBufferPolicy policy = eBuffer == WORLD_BUFFER_FRAME ? T3D_GPU_HOT_FRAME
	        : eBuffer == WORLD_BUFFER_INDIRECT ? T3D_GPU_INDIRECT
	        : eBuffer == WORLD_BUFFER_VISIBLE_ITEMS || eBuffer == WORLD_BUFFER_VISIBILITY_COUNTERS ? T3D_GPU_SCRATCH
	        : T3D_GPU_PUBLICATION;
	if ( !Terrain3DGpuUploadService( *m_pContext, Terrain3DRdContext::OWNER_WORLD, policy )
	                        .writeWhole( m_hBuffers[eBuffer],
	                                m_nCapacityBytes[eBuffer],
	                                pData,
	                                nBytes,
	                                m_uploadScratch[eBuffer] ) ) {
		return false;
	}
	m_nUploadBytes += nBytes;
	return true;
}

uint64_t Terrain3DWorldResources::estimatePublicationUploadBytes( const Terrain3DWorldPublication &rPublication ) {
	uint64_t nBytes = 0u;
	for ( uint32_t i = 0u; i < WORLD_BUFFER_COUNT; ++i ) {
		const t3dWorldBufferId_t eBuffer = ( t3dWorldBufferId_t )i;
		if ( eBuffer == WORLD_BUFFER_VISIBLE_ITEMS ) {
			continue;
		}
		nBytes += requiredBytes( rPublication, eBuffer );
	}
	return nBytes;
}

bool Terrain3DWorldResources::uploadPublicationWarm( const Terrain3DWorldPublication &rPublication ) {
	if ( rPublication.m_nGeneration != 0u && rPublication.m_nGeneration == m_nUploadedPublicationGeneration ) {
		return true;
	}
	if ( !isInitialized() ) {
		return false;
	}
	memset( m_nTerrainStripBucketCounts, 0, sizeof( m_nTerrainStripBucketCounts ) );
	m_nTerrainStripUsefulVertices = 0u;
	m_nTerrainStripFixedSubmittedVertices = 0u;
	m_nTerrainStripBucketSubmittedVertices = 0u;
	for ( size_t i = 0u; i < rPublication.m_terrainStripClusters.size(); ++i ) {
		const uint32_t nActual = t3dWorldUnpackTerrainVertexCount( rPublication.m_terrainStripClusters[i].m_nPackedVertexCountLod );
		const uint32_t nBucket = t3dWorldTerrainStripBucketIndex( nActual );
		++m_nTerrainStripBucketCounts[nBucket];
		m_nTerrainStripUsefulVertices += nActual;
		m_nTerrainStripFixedSubmittedVertices += T3D_WORLD_TERRAIN_STRIP_RASTER_VERTEX_COUNT;
		m_nTerrainStripBucketSubmittedVertices += t3dWorldTerrainStripBucketVertexCount( nBucket );
	}
	t3dWorldDrawIndirectArgs_t indirect[T3D_WORLD_INDIRECT_COMMAND_COUNT]{};
	uint32_t nTerrainBase = t3dWorldTerrainVisibleBase();
	for ( uint32_t nBucket = 0u; nBucket < T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT; ++nBucket ) {
		indirect[nBucket] = { t3dWorldTerrainStripBucketVertexCount( nBucket ), 0u, 0u, nTerrainBase };
		nTerrainBase += m_nTerrainStripBucketCounts[nBucket];
	}
	indirect[T3D_WORLD_INDIRECT_NONTERRAIN_INDEX] = { T3D_WORLD_CLUSTER_RASTER_VERTEX_COUNT, 0u, 0u, nTerrainBase };
	uint32_t counters[T3D_WORLD_VIS_COUNTER_COUNT]{};
	if ( !uploadBufferWarm( WORLD_BUFFER_VERTICES,
	             rPublication.m_vertices.empty() ? NULL : rPublication.m_vertices.data(),
	             ( uint32_t )( rPublication.m_vertices.size() * sizeof( t3dWorldVertex_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_CLUSTER_VERTEX_INDICES,
	                rPublication.m_clusterVertexIndices.empty() ? NULL : rPublication.m_clusterVertexIndices.data(),
	                ( uint32_t )( rPublication.m_clusterVertexIndices.size() * sizeof( uint32_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_TRIANGLES,
	                rPublication.m_triangleWords.empty() ? NULL : rPublication.m_triangleWords.data(),
	                ( uint32_t )( rPublication.m_triangleWords.size() * sizeof( uint32_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_CLUSTERS,
	                rPublication.m_clusters.empty() ? NULL : rPublication.m_clusters.data(),
	                ( uint32_t )( rPublication.m_clusters.size() * sizeof( t3dWorldCluster_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_CLUSTER_VT_METADATA,
	                rPublication.m_clusterVtMetadata.empty() ? NULL : rPublication.m_clusterVtMetadata.data(),
	                ( uint32_t )( rPublication.m_clusterVtMetadata.size() * sizeof( t3dWorldClusterVtMetadata_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS,
	                rPublication.m_terrainStripClusters.empty() ? NULL : rPublication.m_terrainStripClusters.data(),
	                ( uint32_t )( rPublication.m_terrainStripClusters.size() * sizeof( t3dWorldTerrainCluster_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_TERRAIN_STRIP_CLUSTER_BOUNDS,
	                rPublication.m_terrainStripClusterBounds.empty() ? NULL : rPublication.m_terrainStripClusterBounds.data(),
	                ( uint32_t )( rPublication.m_terrainStripClusterBounds.size() * sizeof( t3dWorldTerrainClusterBounds_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_TRANSFORMS_CURRENT,
	                rPublication.m_transforms.empty() ? NULL : rPublication.m_transforms.data(),
	                ( uint32_t )( rPublication.m_transforms.size() * sizeof( t3dWorldTransform_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_TRANSFORMS_PREVIOUS,
	                rPublication.m_previousTransforms.empty() ? NULL : rPublication.m_previousTransforms.data(),
	                ( uint32_t )( rPublication.m_previousTransforms.size() * sizeof( t3dWorldTransform_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_INSTANCES,
	                rPublication.m_instances.empty() ? NULL : rPublication.m_instances.data(),
	                ( uint32_t )( rPublication.m_instances.size() * sizeof( t3dWorldInstance_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_CELLS,
	                rPublication.m_cells.empty() ? NULL : rPublication.m_cells.data(),
	                ( uint32_t )( rPublication.m_cells.size() * sizeof( t3dWorldCell_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_HLOD_RECORDS,
	                rPublication.m_hlodRecords.empty() ? NULL : rPublication.m_hlodRecords.data(),
	                ( uint32_t )( rPublication.m_hlodRecords.size() * sizeof( t3dWorldHlodRecord_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_MATERIALS,
	                rPublication.m_materials.empty() ? NULL : rPublication.m_materials.data(),
	                ( uint32_t )( rPublication.m_materials.size() * sizeof( t3dRenderingMaterialGpu_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_FRAME, NULL, 0u )
	        || !uploadBufferWarm( WORLD_BUFFER_VISIBILITY_CANDIDATES,
	                rPublication.m_visibilityCandidates.empty() ? NULL : rPublication.m_visibilityCandidates.data(),
	                ( uint32_t )( rPublication.m_visibilityCandidates.size() * sizeof( t3dWorldVisibleItem_t ) ) )
	        || !uploadBufferWarm( WORLD_BUFFER_VISIBILITY_COUNTERS, counters, sizeof( counters ) )
	        || !uploadBufferWarm( WORLD_BUFFER_INDIRECT, indirect, sizeof( indirect ) ) ) {
		return false;
	}
	m_nUploadedPublicationGeneration = rPublication.m_nGeneration;
	return true;
}

bool Terrain3DWorldResources::uploadFrameHot( const t3dWorldFrame_t &rFrame ) {
	return uploadBufferWarm( WORLD_BUFFER_FRAME, &rFrame, sizeof( rFrame ) );
}

void Terrain3DWorldResources::shutdownCold( void ) {
	if ( m_pContext ) {
		for ( uint32_t i = 0u; i < WORLD_BUFFER_COUNT; ++i ) {
			if ( m_hBuffers[i].is_valid() ) {
				if ( i == WORLD_BUFFER_INDIRECT ) {
					m_pContext->releaseFixedIndirect( Terrain3DRdContext::FIXED_INDIRECT_WORLD, m_hBuffers[i] );
				} else {
					m_pContext->freeResource( m_hBuffers[i], m_nCapacityBytes[i] );
				}
			}
			m_nCapacityBytes[i] = 0u;
			m_nUsedBytes[i] = 0u;
			m_uploadScratch[i].clear();
		}
	}
	m_pContext = NULL;
	m_nGpuBufferBytes = 0u;
	m_nUploadedPublicationGeneration = 0u;
	m_nResourceGeneration = 0u;
	memset( m_nTerrainStripBucketCounts, 0, sizeof( m_nTerrainStripBucketCounts ) );
	m_nTerrainStripUsefulVertices = 0u;
	m_nTerrainStripFixedSubmittedVertices = 0u;
	m_nTerrainStripBucketSubmittedVertices = 0u;
}

bool Terrain3DWorldResources::isInitialized( void ) const {
	for ( uint32_t i = 0u; i < WORLD_BUFFER_COUNT; ++i ) {
		if ( !m_hBuffers[i].is_valid() ) {
			return false;
		}
	}
	return true;
}

const RID &Terrain3DWorldResources::getBuffer( const t3dWorldBufferId_t eBuffer ) const {
	static RID invalid;
	return eBuffer < WORLD_BUFFER_COUNT ? m_hBuffers[eBuffer] : invalid;
}
uint32_t Terrain3DWorldResources::getCapacityBytes( const t3dWorldBufferId_t eBuffer ) const {
	return eBuffer < WORLD_BUFFER_COUNT ? m_nCapacityBytes[eBuffer] : 0u;
}
uint32_t Terrain3DWorldResources::getUsedBytes( const t3dWorldBufferId_t eBuffer ) const {
	return eBuffer < WORLD_BUFFER_COUNT ? m_nUsedBytes[eBuffer] : 0u;
}
uint64_t Terrain3DWorldResources::getResourceGeneration( void ) const {
	return m_nResourceGeneration;
}
uint64_t Terrain3DWorldResources::getUploadedPublicationGeneration( void ) const {
	return m_nUploadedPublicationGeneration;
}
uint64_t Terrain3DWorldResources::getGpuBufferBytes( void ) const {
	return m_nGpuBufferBytes;
}
uint64_t Terrain3DWorldResources::getUploadBytes( void ) const {
	return m_nUploadBytes;
}
uint64_t Terrain3DWorldResources::getCapacityGrowthCount( void ) const {
	return m_nCapacityGrowthCount;
}
uint32_t Terrain3DWorldResources::getTerrainStripBucketCount( const uint32_t nBucket ) const {
	return nBucket < T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT ? m_nTerrainStripBucketCounts[nBucket] : 0u;
}
uint64_t Terrain3DWorldResources::getTerrainStripUsefulVertices( void ) const {
	return m_nTerrainStripUsefulVertices;
}
uint64_t Terrain3DWorldResources::getTerrainStripFixedSubmittedVertices( void ) const {
	return m_nTerrainStripFixedSubmittedVertices;
}
uint64_t Terrain3DWorldResources::getTerrainStripBucketSubmittedVertices( void ) const {
	return m_nTerrainStripBucketSubmittedVertices;
}
size_t Terrain3DWorldResources::getRetainedCpuBytes( void ) const {
	size_t nBytes = 0u;
	for ( uint32_t i = 0u; i < WORLD_BUFFER_COUNT; ++i ) {
		nBytes += ( size_t )m_uploadScratch[i].size();
	}
	return nBytes;
}
