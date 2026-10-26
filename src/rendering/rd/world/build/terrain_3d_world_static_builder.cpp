// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/build/terrain_3d_world_static_builder.h"

#include <stddef.h>
#include <stdint.h>

static uint64_t t3dWorldStaticHashBytes( uint64_t hash, const void *const pData, const size_t nBytes ) {
	const uint8_t *const bytes = ( const uint8_t * )( pData );
	for ( size_t i = 0u; i < nBytes; ++i ) {
		hash ^= ( uint64_t )bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

bool Terrain3DWorldStaticBuilder::buildCold( const t3dWorldStaticBuildSource_t &rSource, Terrain3DWorldPublication &rTarget ) {
	if ( !rSource.m_pGeometry || ( rSource.m_nTransformCount != 0u && !rSource.m_pTransforms )
	        || ( rSource.m_nInstanceCount != 0u && !rSource.m_pInstances ) || ( rSource.m_nCellCount != 0u && !rSource.m_pCells )
	        || ( rSource.m_nMaterialCount != 0u && !rSource.m_pMaterials ) || rSource.m_nTransformCount != rSource.m_nInstanceCount ) {
		return false;
	}
	const t3dWorldGeometryDatabase_t &geometry = *rSource.m_pGeometry;
	if ( ( geometry.nVertexCount != 0u && !geometry.Buffers.pVertexHeap )
	        || ( geometry.nClusterVertexIndexCount != 0u && !geometry.Buffers.pClusterVertexIndexHeap )
	        || ( geometry.nTriangleCount != 0u && !geometry.Buffers.pTriangleHeap )
	        || ( geometry.nClusterCount != 0u && !geometry.Buffers.pWorldClusterRecords ) ) {
		return false;
	}

	size_t visibility_count = 0u;
	for ( uint32_t i = 0u; i < rSource.m_nInstanceCount; ++i ) {
		const t3dWorldInstance_t &instance = rSource.m_pInstances[i];
		if ( instance.m_nClusterOffset > geometry.nClusterCount || instance.m_nClusterCount > geometry.nClusterCount - instance.m_nClusterOffset
		        || instance.m_nTransformIndex >= rSource.m_nTransformCount || instance.m_nPreviousTransformIndex >= rSource.m_nTransformCount
		        || ( instance.m_nCellId != T3D_WORLD_INVALID_ID && instance.m_nCellId >= rSource.m_nCellCount )
		        || ( instance.m_nMaterialOverride != T3D_WORLD_INVALID_ID && instance.m_nMaterialOverride >= rSource.m_nMaterialCount ) ) {
			return false;
		}
		if ( visibility_count > SIZE_MAX - instance.m_nClusterCount ) {
			return false;
		}
		visibility_count += instance.m_nClusterCount;
	}
	for ( uint32_t i = 0u; i < rSource.m_nCellCount; ++i ) {
		const t3dWorldCell_t &cell = rSource.m_pCells[i];
		if ( cell.m_nFirstInstance > rSource.m_nInstanceCount || cell.m_nInstanceCount > rSource.m_nInstanceCount - cell.m_nFirstInstance ) {
			return false;
		}
	}
	for ( uint32_t i = 0u; i < geometry.nClusterCount; ++i ) {
		const uint32_t material = geometry.Buffers.pWorldClusterRecords[i].m_nMaterial;
		if ( material != T3D_WORLD_INVALID_ID && material >= rSource.m_nMaterialCount ) {
			return false;
		}
	}

	const t3dWorldPublicationCapacity_t required = {
		geometry.nVertexCount,
		geometry.nClusterVertexIndexCount,
		geometry.nTriangleCount,
		geometry.nClusterCount,
		geometry.nClusterCount,
		0u,
		0u,
		rSource.m_nTransformCount,
		rSource.m_nInstanceCount,
		rSource.m_nCellCount,
		0u,
		rSource.m_nMaterialCount,
		visibility_count
	};
	if ( !rTarget.reserveCold( required ) ) {
		return false;
	}

	rTarget.clearForBuild();
	for ( uint32_t i = 0u; i < geometry.nVertexCount; ++i ) rTarget.m_vertices.push_back( geometry.Buffers.pVertexHeap[i] );
	for ( uint32_t i = 0u; i < geometry.nClusterVertexIndexCount; ++i ) rTarget.m_clusterVertexIndices.push_back( geometry.Buffers.pClusterVertexIndexHeap[i] );
	for ( uint32_t i = 0u; i < geometry.nTriangleCount; ++i ) rTarget.m_triangleWords.push_back( geometry.Buffers.pTriangleHeap[i].nLocalIndices );
	for ( uint32_t i = 0u; i < geometry.nClusterCount; ++i ) {
		t3dWorldCluster_t cluster = geometry.Buffers.pWorldClusterRecords[i];
		cluster.m_nPackedLodGeometryKind = ( cluster.m_nPackedLodGeometryKind & 0xffffu ) | ( T3D_WORLD_GEOMETRY_STATIC_CLUSTER << 16u );
		rTarget.m_clusters.push_back( cluster );
		rTarget.m_clusterVtMetadata.push_back( geometry.Buffers.pWorldClusterVtMetadata ? geometry.Buffers.pWorldClusterVtMetadata[i] : t3dWorldClusterVTMetadata_t{} );
	}
	for ( uint32_t i = 0u; i < rSource.m_nMaterialCount; ++i ) rTarget.m_materials.push_back( rSource.m_pMaterials[i] );
	for ( uint32_t i = 0u; i < rSource.m_nTransformCount; ++i ) {
		rTarget.m_transforms.push_back( rSource.m_pTransforms[i] );
		rTarget.m_previousTransforms.push_back( rSource.m_pTransforms[i] );
	}
	for ( uint32_t i = 0u; i < rSource.m_nInstanceCount; ++i ) rTarget.m_instances.push_back( rSource.m_pInstances[i] );
	for ( uint32_t i = 0u; i < rSource.m_nCellCount; ++i ) rTarget.m_cells.push_back( rSource.m_pCells[i] );
	for ( uint32_t iInstance = 0u; iInstance < rSource.m_nInstanceCount; ++iInstance ) {
		const t3dWorldInstance_t &instance = rTarget.m_instances[iInstance];
		for ( uint32_t local = 0u; local < instance.m_nClusterCount; ++local ) {
			t3dWorldVisibleItem_t candidate{};
			candidate.m_nClusterOrTerrainId = instance.m_nClusterOffset + local;
			candidate.m_nInstanceOrPatchId = iInstance;
			candidate.m_nGeometryKind = T3D_WORLD_GEOMETRY_STATIC_CLUSTER;
			candidate.m_nFlags = instance.m_nFlags;
			rTarget.m_visibilityCandidates.push_back( candidate );
		}
	}
	rTarget.m_nStaticClusterCount = geometry.nClusterCount;

	uint64_t signature = 1469598103934665603ull;
	signature = t3dWorldStaticHashBytes( signature, &geometry.nContentSignature, sizeof( geometry.nContentSignature ) );
	const uint32_t counts[4] = { rSource.m_nTransformCount, rSource.m_nInstanceCount, rSource.m_nCellCount, rSource.m_nMaterialCount };
	signature = t3dWorldStaticHashBytes( signature, counts, sizeof( counts ) );
	if ( rSource.m_nTransformCount != 0u ) signature = t3dWorldStaticHashBytes( signature, rSource.m_pTransforms, sizeof( t3dWorldTransform_t ) * rSource.m_nTransformCount );
	if ( rSource.m_nInstanceCount != 0u ) signature = t3dWorldStaticHashBytes( signature, rSource.m_pInstances, sizeof( t3dWorldInstance_t ) * rSource.m_nInstanceCount );
	if ( rSource.m_nCellCount != 0u ) signature = t3dWorldStaticHashBytes( signature, rSource.m_pCells, sizeof( t3dWorldCell_t ) * rSource.m_nCellCount );
	if ( rSource.m_nMaterialCount != 0u ) signature = t3dWorldStaticHashBytes( signature, rSource.m_pMaterials, sizeof( t3dRenderingMaterialGpu_t ) * rSource.m_nMaterialCount );
	rTarget.m_nSourceSignature = signature == 0u ? 1u : signature;
	rTarget.m_nGeneration = 1u;
	rTarget.m_nGeometryGeneration = rTarget.m_nSourceSignature;
	rTarget.m_nMaterialGeneration = rTarget.m_nSourceSignature;
	rTarget.m_nInstanceGeneration = rTarget.m_nSourceSignature;
	rTarget.m_nStaticGeneration = rTarget.m_nSourceSignature;
	return true;
}
