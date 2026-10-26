// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/runtime/terrain_3d_world_publication.h"

void Terrain3DWorldPublication::bindRuntimeAllocator( const Terrain3DEastlAllocator &rAllocator ) {
	m_vertices.set_allocator( rAllocator );
	m_clusterVertexIndices.set_allocator( rAllocator );
	m_triangleWords.set_allocator( rAllocator );
	m_clusters.set_allocator( rAllocator );
	m_clusterVtMetadata.set_allocator( rAllocator );
	m_terrainStripClusters.set_allocator( rAllocator );
	m_terrainStripClusterBounds.set_allocator( rAllocator );
	m_transforms.set_allocator( rAllocator );
	m_previousTransforms.set_allocator( rAllocator );
	m_instances.set_allocator( rAllocator );
	m_cells.set_allocator( rAllocator );
	m_hlodRecords.set_allocator( rAllocator );
	m_materials.set_allocator( rAllocator );
	m_instanceMaterialStableKeys.set_allocator( rAllocator );
	m_visibilityCandidates.set_allocator( rAllocator );
}

bool Terrain3DWorldPublication::hasCapacity( const t3dWorldPublicationCapacity_t &rRequired ) const {
	return m_vertices.capacity() >= rRequired.m_nVertices && m_clusterVertexIndices.capacity() >= rRequired.m_nClusterVertexIndices
	        && m_triangleWords.capacity() >= rRequired.m_nTriangleWords && m_clusters.capacity() >= rRequired.m_nClusters
	        && m_clusterVtMetadata.capacity() >= rRequired.m_nClusterVtMetadata
	        && m_terrainStripClusters.capacity() >= rRequired.m_nTerrainStripClusters
	        && m_terrainStripClusterBounds.capacity() >= rRequired.m_nTerrainStripClusterBounds
	        && m_transforms.capacity() >= rRequired.m_nTransforms
	        && m_previousTransforms.capacity() >= rRequired.m_nTransforms && m_instances.capacity() >= rRequired.m_nInstances
	        && m_cells.capacity() >= rRequired.m_nCells && m_hlodRecords.capacity() >= rRequired.m_nHlodRecords
	        && m_materials.capacity() >= rRequired.m_nMaterials
	        && m_instanceMaterialStableKeys.capacity() >= rRequired.m_nMaterials
	        && m_visibilityCandidates.capacity() >= rRequired.m_nVisibilityCandidates;
}

bool Terrain3DWorldPublication::reserveCold( const t3dWorldPublicationCapacity_t &rRequired ) {
	return t3dContainerReserveCold( m_vertices, rRequired.m_nVertices )
	        && t3dContainerReserveCold( m_clusterVertexIndices, rRequired.m_nClusterVertexIndices )
	        && t3dContainerReserveCold( m_triangleWords, rRequired.m_nTriangleWords )
	        && t3dContainerReserveCold( m_clusters, rRequired.m_nClusters )
	        && t3dContainerReserveCold( m_clusterVtMetadata, rRequired.m_nClusterVtMetadata )
	        && t3dContainerReserveCold( m_terrainStripClusters, rRequired.m_nTerrainStripClusters )
	        && t3dContainerReserveCold( m_terrainStripClusterBounds, rRequired.m_nTerrainStripClusterBounds )
	        && t3dContainerReserveCold( m_transforms, rRequired.m_nTransforms )
	        && t3dContainerReserveCold( m_previousTransforms, rRequired.m_nTransforms )
	        && t3dContainerReserveCold( m_instances, rRequired.m_nInstances ) && t3dContainerReserveCold( m_cells, rRequired.m_nCells )
	        && t3dContainerReserveCold( m_hlodRecords, rRequired.m_nHlodRecords )
	        && t3dContainerReserveCold( m_materials, rRequired.m_nMaterials )
	        && t3dContainerReserveCold( m_instanceMaterialStableKeys, rRequired.m_nMaterials )
	        && t3dContainerReserveCold( m_visibilityCandidates, rRequired.m_nVisibilityCandidates );
}

void Terrain3DWorldPublication::clearForBuild( void ) {
	m_nStaticSceneProducerId = 0u;
	m_nStaticScenePublicationGeneration = 0u;
	m_vertices.clear();
	m_clusterVertexIndices.clear();
	m_triangleWords.clear();
	m_clusters.clear();
	m_clusterVtMetadata.clear();
	m_terrainStripClusters.clear();
	m_terrainStripClusterBounds.clear();
	m_transforms.clear();
	m_previousTransforms.clear();
	m_instances.clear();
	m_cells.clear();
	m_hlodRecords.clear();
	m_materials.clear();
	m_instanceMaterialStableKeys.clear();
	m_visibilityCandidates.clear();
	m_nInstanceClusterCount = 0u;
	m_nHlodClusterCount = 0u;
	m_nStaticClusterCount = 0u;
	m_nTerrainStripClusterCount = 0u;
}

bool Terrain3DWorldPublication::copyFrom( const Terrain3DWorldPublication &rOther ) {
	if ( !hasCapacity( t3dWorldPublicationCapacity_t{ rOther.m_vertices.size(),
	             rOther.m_clusterVertexIndices.size(),
	             rOther.m_triangleWords.size(),
	             rOther.m_clusters.size(),
	             rOther.m_clusterVtMetadata.size(),
	             rOther.m_terrainStripClusters.size(),
	             rOther.m_terrainStripClusterBounds.size(),
	             rOther.m_transforms.size(),
	             rOther.m_instances.size(),
	             rOther.m_cells.size(),
	             rOther.m_hlodRecords.size(),
	             rOther.m_materials.size(),
	             rOther.m_visibilityCandidates.size() } ) ) {
		return false;
	}
	if ( !t3dVectorCopyWithinCapacity( m_vertices, rOther.m_vertices )
	        || !t3dVectorCopyWithinCapacity( m_clusterVertexIndices, rOther.m_clusterVertexIndices )
	        || !t3dVectorCopyWithinCapacity( m_triangleWords, rOther.m_triangleWords )
	        || !t3dVectorCopyWithinCapacity( m_clusters, rOther.m_clusters )
	        || !t3dVectorCopyWithinCapacity( m_clusterVtMetadata, rOther.m_clusterVtMetadata )
	        || !t3dVectorCopyWithinCapacity( m_terrainStripClusters, rOther.m_terrainStripClusters )
	        || !t3dVectorCopyWithinCapacity( m_terrainStripClusterBounds, rOther.m_terrainStripClusterBounds )
	        || !t3dVectorCopyWithinCapacity( m_transforms, rOther.m_transforms )
	        || !t3dVectorCopyWithinCapacity( m_previousTransforms, rOther.m_previousTransforms )
	        || !t3dVectorCopyWithinCapacity( m_instances, rOther.m_instances ) || !t3dVectorCopyWithinCapacity( m_cells, rOther.m_cells )
	        || !t3dVectorCopyWithinCapacity( m_hlodRecords, rOther.m_hlodRecords )
	        || !t3dVectorCopyWithinCapacity( m_materials, rOther.m_materials )
	        || !t3dVectorCopyWithinCapacity( m_instanceMaterialStableKeys, rOther.m_instanceMaterialStableKeys )
	        || !t3dVectorCopyWithinCapacity( m_visibilityCandidates, rOther.m_visibilityCandidates ) ) {
		return false;
	}
	m_nSourceSignature = rOther.m_nSourceSignature;
	m_nGeneration = rOther.m_nGeneration;
	m_nGeometryGeneration = rOther.m_nGeometryGeneration;
	m_nMaterialGeneration = rOther.m_nMaterialGeneration;
	m_nInstanceGeneration = rOther.m_nInstanceGeneration;
	m_nTerrainGeneration = rOther.m_nTerrainGeneration;
	m_nStaticGeneration = rOther.m_nStaticGeneration;
	m_nStaticSceneProducerId = rOther.m_nStaticSceneProducerId;
	m_nStaticScenePublicationGeneration = rOther.m_nStaticScenePublicationGeneration;
	m_nInstanceClusterCount = rOther.m_nInstanceClusterCount;
	m_nHlodClusterCount = rOther.m_nHlodClusterCount;
	m_nStaticClusterCount = rOther.m_nStaticClusterCount;
	m_nTerrainStripClusterCount = rOther.m_nTerrainStripClusterCount;
	return true;
}

size_t Terrain3DWorldPublication::getRetainedCpuBytes( void ) const {
	return m_vertices.capacity() * sizeof( t3dWorldVertex_t ) + m_clusterVertexIndices.capacity() * sizeof( uint32_t )
	        + m_triangleWords.capacity() * sizeof( uint32_t ) + m_clusters.capacity() * sizeof( t3dWorldCluster_t )
	        + m_clusterVtMetadata.capacity() * sizeof( t3dWorldClusterVTMetadata_t )
	        + m_terrainStripClusters.capacity() * sizeof( t3dWorldTerrainCluster_t )
	        + m_terrainStripClusterBounds.capacity() * sizeof( t3dWorldTerrainClusterBounds_t )
	        + ( m_transforms.capacity() + m_previousTransforms.capacity() ) * sizeof( t3dWorldTransform_t )
	        + m_instances.capacity() * sizeof( t3dWorldInstance_t ) + m_cells.capacity() * sizeof( t3dWorldCell_t )
	        + m_hlodRecords.capacity() * sizeof( t3dWorldHlodRecord_t ) + m_materials.capacity() * sizeof( t3dRenderingMaterialGpu_t )
	        + m_instanceMaterialStableKeys.capacity() * sizeof( uint64_t )
	        + m_visibilityCandidates.capacity() * sizeof( t3dWorldVisibleItem_t );
}
