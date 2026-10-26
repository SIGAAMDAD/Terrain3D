#include "diagnostics/core/terrain_3d_streaming_profiler.h"
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/resources/terrain_3d_world_resources.h"
#include "rendering/rd/vt/runtime/terrain_3d_virtual_texture.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer_batch.h"

#include <string.h>
#include <array>
#include <godot_cpp/templates/hashfuncs.hpp>

using namespace godot;

static uint32_t t3dWorldClampBufferBytes(
	const uint64_t nBytes ) {
	if ( nBytes == 0u ) {
		return 16u;
	}
	return nBytes > UINT32_MAX ? 0u : ( uint32_t ) nBytes;
}

struct WorldUploadTable
{
	const void *data;
	uint64_t	bytes;
};
static std::array< WorldUploadTable, WORLD_BUFFER_COUNT > worldPublicationTables(
	const Terrain3DWorldPublication &p ) {
	return { { { p.m_vertices.data(), uint64_t( p.m_vertices.size() ) * sizeof( *p.m_vertices.data() ) },
			   { p.m_clusterVertexIndices.data(),
				 uint64_t( p.m_clusterVertexIndices.size() ) * sizeof( *p.m_clusterVertexIndices.data() ) },
			   { p.m_triangleWords.data(), uint64_t( p.m_triangleWords.size() ) * sizeof( *p.m_triangleWords.data() ) },
			   { p.m_clusters.data(), uint64_t( p.m_clusters.size() ) * sizeof( *p.m_clusters.data() ) },
			   { p.m_clusterVtMetadata.data(),
				 uint64_t( p.m_clusterVtMetadata.size() ) * sizeof( *p.m_clusterVtMetadata.data() ) },
			   { p.m_terrainStripClusters.data(),
				 uint64_t( p.m_terrainStripClusters.size() ) * sizeof( *p.m_terrainStripClusters.data() ) },
			   { NULL, 0u },
			   { p.m_transforms.data(), uint64_t( p.m_transforms.size() ) * sizeof( *p.m_transforms.data() ) },
			   { p.m_previousTransforms.data(),
				 uint64_t( p.m_previousTransforms.size() ) * sizeof( *p.m_previousTransforms.data() ) },
			   { p.m_instances.data(), uint64_t( p.m_instances.size() ) * sizeof( *p.m_instances.data() ) },
			   { p.m_cells.data(), uint64_t( p.m_cells.size() ) * sizeof( *p.m_cells.data() ) },
			   { p.m_hlodRecords.data(), uint64_t( p.m_hlodRecords.size() ) * sizeof( *p.m_hlodRecords.data() ) },
			   { p.m_materials.data(), uint64_t( p.m_materials.size() ) * sizeof( *p.m_materials.data() ) },
			   { NULL, sizeof( t3dWorldFrame_t ) },
			   { p.m_visibilityCandidates.data(),
				 uint64_t( p.m_visibilityCandidates.size() ) * sizeof( t3dWorldVisibleItem_t ) },
			   { NULL, uint64_t( p.m_visibilityCandidates.size() ) * sizeof( t3dWorldVisibleItem_t ) },
			   { NULL, T3D_WORLD_VIS_COUNTER_COUNT * sizeof( uint32_t ) },
			   { NULL, T3D_WORLD_INDIRECT_BUFFER_SIZE_BYTES } } };
}

static constexpr uint32_t worldBufferStrides[WORLD_BUFFER_COUNT] = { 4u,
																	 4u,
																	 4u,
																	 sizeof( t3dWorldCluster_t ),
																	 sizeof( t3dWorldClusterVTMetadata_t ),
																	 sizeof( t3dWorldTerrainCluster_t ),
																	 sizeof( t3dWorldTerrainClusterBounds_t ),
																	 sizeof( t3dWorldTransform_t ),
																	 sizeof( t3dWorldTransform_t ),
																	 sizeof( t3dWorldInstance_t ),
																	 sizeof( t3dWorldCell_t ),
																	 sizeof( t3dWorldHlodRecord_t ),
																	 sizeof( t3dRenderingMaterialGpu_t ),
																	 16u,
																	 sizeof( t3dWorldVisibleItem_t ),
																	 sizeof( t3dWorldVisibleItem_t ),
																	 4u,
																	 16u };

static uint64_t worldReservedBufferBytes(
	uint32_t						id,
	const t3dWorldResourceLimits_t &limits ) {
	const uint32_t counts[WORLD_BUFFER_COUNT] = { limits.m_nMaxSourceVertices,
												  limits.m_nMaxClusterVertexIndices,
												  limits.m_nMaxTriangleSlots,
												  limits.m_nMaxClusters,
												  limits.m_nMaxClusters,
												  limits.m_nMaxTerrainClusters,
												  0u,
												  limits.m_nMaxTransforms,
												  limits.m_nMaxTransforms,
												  limits.m_nMaxVisibleItems,
												  limits.m_nMaxVisibleItems,
												  limits.m_nMaxVisibleItems,
												  limits.m_nMaxMaterials,
												  0u,
												  limits.m_nMaxVisibleItems,
												  limits.m_nMaxVisibleItems,
												  0u,
												  0u };
	return uint64_t( counts[id] ) *
		   ( id == WORLD_BUFFER_VERTICES ? sizeof( t3dWorldVertex_t ) : worldBufferStrides[id] );
}

uint32_t Terrain3DWorldResources::estimateSharedColdArenaBytes(
	const t3dWorldResourceLimits_t &limits ) {
	if ( !limits.m_nMaxSourceVertices || !limits.m_nMaxClusters ) {
		return Terrain3DRdContext::COLD_ARENA_BYTES;
	}
	uint32_t cursor = 0u;
	for ( uint32_t i = 0u; i < WORLD_BUFFER_COUNT; ++i ) {
		const t3dWorldBufferId_t id = static_cast< t3dWorldBufferId_t >( i );
		if ( id == WORLD_BUFFER_INDIRECT || arenaFor( id ) != Terrain3DRdContext::COLD ) {
			continue;
		}
		const uint64_t reserved = worldReservedBufferBytes( i, limits );
		if ( reserved > UINT32_MAX ) {
			return 0u;
		}
		const uint32_t bytes = static_cast< uint32_t >( MAX( uint64_t( 16u ), reserved ) );
		uint32_t	   offset = 0u;
		if ( !t3dRdGpuBufferSection( cursor, bytes, 16u, worldBufferStrides[i], 0u, offset ) ) {
			return 0u;
		}
	}
	// Include the clipmap COLD allocation and alignment headroom. The physical
	// backing is created once; stream publications reuse these fixed spans.
	static constexpr uint64_t CLIPMAP_AND_ALIGNMENT_HEADROOM = 8ull * 1024ull * 1024ull;
	static constexpr uint64_t MIB = 1024ull * 1024ull;
	const uint64_t			  total = MAX( 16ull * MIB, uint64_t( cursor ) + CLIPMAP_AND_ALIGNMENT_HEADROOM );
	const uint64_t			  rounded = ( ( total + MIB - 1u ) / MIB ) * MIB;
	return rounded <= UINT32_MAX ? static_cast< uint32_t >( rounded ) : 0u;
}

uint64_t Terrain3DWorldResources::estimateColdStagingBytes(
	const t3dWorldResourceLimits_t &limits ) {
	const uint64_t materials = MAX( uint64_t( 16u ), worldReservedBufferBytes( WORLD_BUFFER_MATERIALS, limits ) );
	if ( materials > UINT32_MAX ) {
		return UINT64_MAX;
	}
	return materials + MAX( uint64_t( TRANSFER_BYTES ), materials );
}

static uint64_t worldUploadSignature(
	const void *data,
	uint32_t	bytes ) {
	if ( !bytes ) {
		return 0u;
	}
	return ( uint64_t( hash_murmur3_buffer( data, int( bytes ), 0x9747b28cu ) ) << 32u ) |
		   hash_murmur3_buffer( data, int( bytes ), 0x85ebca6bu );
}

bool Terrain3DWorldResources::ensureCapacityCold(
	const Terrain3DWorldPublication &publication,
	const t3dWorldResourceLimits_t	&limits ) {
	if ( !m_pContext ) {
		m_pContext = &Terrain3DRdContext::getShared();
	}
	if ( !m_pContext->initializeSharedArenas() ) {
		return false;
	}
	const auto &strides = worldBufferStrides;
	const auto	tables = worldPublicationTables( publication );
	if ( isInitialized() ) {
		for ( uint32_t i = 0u; i < WORLD_BUFFER_COUNT; ++i ) {
			const uint32_t bytes = t3dWorldClampBufferBytes( tables[i].bytes );
			if ( !bytes || bytes > getCapacityBytes( static_cast< t3dWorldBufferId_t >( i ) ) ) {
				return false;
			}
		}
		return true; // Frozen publication capacities: never grow during streaming.
	}
	uint32_t offsets[WORLD_BUFFER_COUNT]{}, capacities[WORLD_BUFFER_COUNT]{},
		totals[Terrain3DRdContext::SHARED_ARENA_COUNT]{};
	for ( uint32_t i = 0u; i < WORLD_BUFFER_COUNT; ++i ) {
		const auto	   id = static_cast< t3dWorldBufferId_t >( i );
		const uint64_t reserved_bytes = worldReservedBufferBytes( i, limits );
		capacities[i] = t3dWorldClampBufferBytes( MAX( tables[i].bytes, reserved_bytes ) );
		const Arena arena = id == WORLD_BUFFER_INDIRECT ? Terrain3DRdContext::SCRATCH : arenaFor( id );
		if ( !capacities[i] ||
			 !t3dRdGpuBufferSection( totals[arena], capacities[i], 16u, strides[i], 0u, offsets[i] ) ) {
			return false;
		}
	}
	// Reserve profile maxima for geometry and streaming records alike. A later
	// publication can fill these spans without replacing the shared backing.
	if ( totals[Terrain3DRdContext::COLD] > m_pContext->arena( Terrain3DRdContext::COLD ).capacityBytes() ||
		 totals[Terrain3DRdContext::SCRATCH] > m_pContext->arena( Terrain3DRdContext::SCRATCH ).capacityBytes() ) {
		return false;
	}
	const uint32_t material_bytes = capacities[WORLD_BUFFER_MATERIALS];
	const uint32_t transfer_bytes = MAX( TRANSFER_BYTES, material_bytes );
	if ( !m_shadowMemory.resize( material_bytes ) || m_materialShadow.resize( material_bytes ) != OK ||
		 !m_transferMemory.resize( transfer_bytes ) || m_transfer.resize( transfer_bytes ) != OK ) {
		return false;
	}
	memset( m_materialShadow.ptrw(), 0, material_bytes );
	if ( !m_pContext->reserveArenaSpan( Terrain3DRdContext::COLD,
										totals[Terrain3DRdContext::COLD],
										192u,
										m_coldSpan ) ||
		 !m_pContext->reserveArenaSpan( Terrain3DRdContext::SCRATCH,
										totals[Terrain3DRdContext::SCRATCH],
										16u,
										m_scratchSpan ) ) {
		return false;
	}
	for ( uint32_t i = 0u; i < WORLD_BUFFER_COUNT; ++i ) {
		const auto id = static_cast< t3dWorldBufferId_t >( i );
		if ( id == WORLD_BUFFER_FRAME ) {
			continue;
		}
		const uint32_t base = id == WORLD_BUFFER_INDIRECT || arenaFor( id ) == Terrain3DRdContext::SCRATCH
								  ? m_scratchSpan.m_nOffsetBytes
								  : m_coldSpan.m_nOffsetBytes;
		if ( !bufferFor( id ).slice( base + offsets[i],
									 capacities[i],
									 16u,
									 id == WORLD_BUFFER_INDIRECT ? m_fixedIndirectSlice : m_sections[i] ) ) {
			return false;
		}
	}
	m_sections[WORLD_BUFFER_FRAME].m_nCapacityBytes = sizeof( t3dWorldFrame_t );
	m_pFixedIndirectBuffer = &m_pContext->arena( Terrain3DRdContext::SCRATCH );
	++m_nResourceGeneration;
	++m_nStagingReallocations;
	m_nGpuBufferBytes = totals[Terrain3DRdContext::COLD] + totals[Terrain3DRdContext::SCRATCH];
	return true;
}

bool Terrain3DWorldResources::uploadBufferWarm(
	const t3dWorldBufferId_t id,
	const void				*data,
	const uint32_t			 bytes ) {
	if ( id >= WORLD_BUFFER_COUNT || !m_pContext || ( bytes && !data ) ) {
		return false;
	}
	const bool	publication = arenaFor( id ) == Terrain3DRdContext::COLD && id != WORLD_BUFFER_INDIRECT;
	const auto &section = id == WORLD_BUFFER_INDIRECT ? m_fixedIndirectSlice : m_sections[id];
	if ( bytes > section.m_nCapacityBytes || bytes > INT32_MAX ) {
		return false;
	}
	const uint64_t signature = publication ? worldUploadSignature( data, bytes ) : 0u;
	if ( publication && m_bPublicationUploadCached[id] && section.m_nSizeBytes == bytes &&
		 m_sectionSignatures[id] == signature &&
		 ( id != WORLD_BUFFER_MATERIALS || !bytes || !memcmp( m_materialShadow.ptr(), data, bytes ) ) ) {
		m_nSkippedPublicationUploadBytes += bytes;
		return true;
	}
	auto	  *buffer = id == WORLD_BUFFER_INDIRECT ? m_pFixedIndirectBuffer : &bufferFor( id );
	const auto policy = publication					  ? T3D_GPU_PUBLICATION
						: id == WORLD_BUFFER_FRAME	  ? T3D_GPU_HOT_FRAME
						: id == WORLD_BUFFER_INDIRECT ? T3D_GPU_INDIRECT
													  : T3D_GPU_SCRATCH;
	if ( publication ) {
		m_bPublicationUploadCached[id] = false;
		// Material resolution uses the transfer image as working storage. Keep its
		// queued source separate, since earlier geometry writes reuse that image.
		if ( id == WORLD_BUFFER_MATERIALS && bytes ) {
			memcpy( m_materialShadow.ptrw(), data, bytes );
			data = m_materialShadow.ptr();
		}
	}
	if ( publication && m_pPublicationBatch ) {
		if ( !m_pPublicationBatch
				  ->queueRangeWrite( *buffer, section.m_nOffsetBytes, data, bytes, m_transfer, policy ) ) {
			return false;
		}
		buffer->setSliceUsedBytes( m_sections[id], bytes );
		m_sectionSignatures[id] = signature;
		m_dirtySections[id] = true;
		return true;
	}
	if ( !buffer || !buffer->writeSliceTracked( *m_pContext,
												section,
												0u,
												data,
												bytes,
												m_transfer,
												policy,
												T3D_RD_CONTEXT_OWNER_WORLD ) ) {
		return false;
	}
	buffer->setSliceUsedBytes( id == WORLD_BUFFER_INDIRECT ? m_fixedIndirectSlice : m_sections[id], bytes );
	if ( publication ) {
		m_sectionSignatures[id] = signature;
		m_bPublicationUploadCached[id] = true;
	}
	m_nUploadBytes += bytes;
	return true;
}

bool Terrain3DWorldResources::uploadResolvedMaterialsWarm(
	const Terrain3DWorldPublication &rPublication,
	const Terrain3DVirtualTexture	&rVirtualTexture ) {
	const size_t   nMaterialCount = rPublication.m_materials.size();
	const size_t   nInstanceMaterialCount = rPublication.m_instanceMaterialStableKeys.size();
	const uint64_t nBytes64 = nMaterialCount * sizeof( t3dRenderingMaterialGpu_t );
	if ( nInstanceMaterialCount > nMaterialCount || nBytes64 > UINT32_MAX ) {
		return false;
	}
	if ( nInstanceMaterialCount == 0u ) {
		return uploadBufferWarm( WORLD_BUFFER_MATERIALS, rPublication.m_materials.data(), ( uint32_t ) nBytes64 );
	}
	const uint32_t	 nBytes = ( uint32_t ) nBytes64;
	PackedByteArray &scratch = m_transfer;
	if ( !m_pContext || scratch.size() < ( int64_t ) nBytes || nBytes > getCapacityBytes( WORLD_BUFFER_MATERIALS ) ) {
		return false;
	}
	bool bChanged =
		!m_bPublicationUploadCached[WORLD_BUFFER_MATERIALS] || getUsedBytes( WORLD_BUFFER_MATERIALS ) != nBytes;
	uint8_t *const				 pBytes = scratch.ptrw();
	const bool					 bV3 = rVirtualTexture.getDatabaseKind() == T3D_VT_RUNTIME_DATABASE_PACK_V3;
	const uint32_t				 nAllocationCount = rVirtualTexture.getActiveAllocationCount();
	const uint32_t				 nPageCount = rVirtualTexture.getActiveVirtualPageCount();
	const t3dVTResidencyState_t &residency = rVirtualTexture.get_residency();
	for ( size_t i = 0u; i < nMaterialCount; ++i ) {
		t3dRenderingMaterialGpu_t material = rPublication.m_materials[i];
		if ( i < nInstanceMaterialCount ) {
			uint32_t nAllocation = T3D_VT_INVALID_ALLOCATION;
			if ( !bV3 ||
				 !rVirtualTexture.findMaterialAllocationCold( rPublication.m_instanceMaterialStableKeys[i],
															  nAllocation ) ||
				 nAllocation >= nAllocationCount || !residency.bInitialized || residency.Buffers.pAllocations == NULL ||
				 nAllocation >= residency.nAllocationCount ) {
				nAllocation = T3D_VT_INVALID_ALLOCATION;
			} else {
				const t3dVTAllocation_t &allocation = residency.Buffers.pAllocations[nAllocation];
				if ( allocation.m_nBaseVirtualPage >= nPageCount ||
					 allocation.m_nPageCount > nPageCount - allocation.m_nBaseVirtualPage ) {
					nAllocation = T3D_VT_INVALID_ALLOCATION;
				}
			}
			material.m_nVtAllocation = nAllocation;
			material.m_nOverlayVtAllocation = T3D_VT_INVALID_ALLOCATION;
			if ( nAllocation != T3D_VT_INVALID_ALLOCATION ) {
				material.m_nFlags |= T3D_VT_MATERIAL_V3_LAYOUT;
			} else {
				material.m_nFlags &= ~T3D_VT_MATERIAL_V3_LAYOUT;
			}
		}
		uint8_t *const pRecord = pBytes + i * sizeof( material );
		if ( !m_bPublicationUploadCached[WORLD_BUFFER_MATERIALS] ||
			 memcmp( m_materialShadow.ptr() + i * sizeof( material ), &material, sizeof( material ) ) != 0 ) {
			bChanged = true;
		}
		memcpy( pRecord, &material, sizeof( material ) );
	}
	if ( !bChanged ) {
		m_nSkippedPublicationUploadBytes += nBytes;
		return true;
	}
	return uploadBufferWarm( WORLD_BUFFER_MATERIALS, scratch.ptr(), nBytes );
}

bool Terrain3DWorldResources::getResolvedMaterialHot(
	const uint32_t			   nMaterial,
	t3dRenderingMaterialGpu_t &rMaterial ) const {
	const uint64_t nOffset = ( uint64_t ) nMaterial * sizeof( t3dRenderingMaterialGpu_t );
	if ( !m_bPublicationUploadCached[WORLD_BUFFER_MATERIALS] ||
		 nOffset + sizeof( t3dRenderingMaterialGpu_t ) > getUsedBytes( WORLD_BUFFER_MATERIALS ) ) {
		return false;
	}
	memcpy( &rMaterial, m_materialShadow.ptr() + nOffset, sizeof( rMaterial ) );
	return true;
}

uint64_t Terrain3DWorldResources::estimatePublicationUploadBytes(
	const Terrain3DWorldPublication &rPublication ) {
	const auto tables = worldPublicationTables( rPublication );
	uint64_t   bytes = 0u;
	for ( uint32_t i = 0u; i < WORLD_BUFFER_COUNT; ++i ) {
		if ( i != WORLD_BUFFER_VISIBLE_ITEMS ) {
			bytes += t3dWorldClampBufferBytes( tables[i].bytes );
		}
	}
	return bytes;
}

uint64_t Terrain3DWorldResources::estimateChangedPublicationUploadBytes(
	const Terrain3DWorldPublication &rPublication,
	const bool						 bResolveVtMaterials,
	uint32_t						&rChangedClusterCount ) const {
	rChangedClusterCount = 0u;
	if ( !isInitialized() ) {
		rChangedClusterCount =
			( uint32_t ) MIN( uint64_t( UINT32_MAX ),
							  uint64_t( rPublication.m_clusters.size() ) + rPublication.m_terrainStripClusters.size() );
		return estimatePublicationUploadBytes( rPublication );
	}
	const auto tables = worldPublicationTables( rPublication );
	uint64_t   changed_bytes = 0u;
	for ( uint32_t i = 0u; i <= WORLD_BUFFER_VISIBILITY_CANDIDATES; ++i ) {
		if ( i == WORLD_BUFFER_FRAME ) {
			continue;
		}
		const uint64_t byte_count = tables[i].bytes;
		if ( byte_count > UINT32_MAX ) {
			return UINT64_MAX;
		}
		const uint32_t bytes = ( uint32_t ) byte_count;
		const bool	   changed =
			( i == WORLD_BUFFER_MATERIALS && bResolveVtMaterials ) || !m_bPublicationUploadCached[i] ||
			m_sections[i].m_nSizeBytes != bytes ||
			m_sectionSignatures[i] != worldUploadSignature( tables[i].data, bytes ) ||
			( i == WORLD_BUFFER_MATERIALS && bytes && memcmp( m_materialShadow.ptr(), tables[i].data, bytes ) != 0 );
		if ( !changed ) {
			continue;
		}
		changed_bytes += bytes;
		if ( i == WORLD_BUFFER_CLUSTERS || i == WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS ) {
			rChangedClusterCount +=
				( uint32_t ) ( i == WORLD_BUFFER_CLUSTERS ? rPublication.m_clusters.size()
														  : rPublication.m_terrainStripClusters.size() );
		}
	}
	// Counter and indirect command initialization happens on every new publication.
	return changed_bytes + sizeof( uint32_t ) * T3D_WORLD_VIS_COUNTER_COUNT +
		   sizeof( t3dWorldDrawIndirectArgs_t ) * T3D_WORLD_INDIRECT_COMMAND_COUNT;
}

bool Terrain3DWorldResources::uploadPublicationWarm(
	const Terrain3DWorldPublication		&rPublication,
	const Terrain3DVirtualTexture *const pVirtualTexture ) {
	T3D_STREAM_PROFILE_SCOPE( T3D_SP_WORLD_BUFFER_UPLOAD );
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
		const uint32_t nActual =
			t3dWorldUnpackTerrainVertexCount( rPublication.m_terrainStripClusters[i].m_nPackedVertexCountLod );
		const uint32_t nBucket = t3dWorldTerrainStripBucketIndex( nActual );
		++m_nTerrainStripBucketCounts[nBucket];
		m_nTerrainStripUsefulVertices += nActual;
		m_nTerrainStripFixedSubmittedVertices += T3D_WORLD_TERRAIN_STRIP_RASTER_VERTEX_COUNT;
		m_nTerrainStripBucketSubmittedVertices += t3dWorldTerrainStripBucketVertexCount( nBucket );
	}
	t3dWorldDrawIndirectArgs_t indirect[T3D_WORLD_INDIRECT_COMMAND_COUNT]{};
	uint32_t				   nTerrainBase = t3dWorldTerrainVisibleBase();
	for ( uint32_t nBucket = 0u; nBucket < T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT; ++nBucket ) {
		indirect[nBucket] = { t3dWorldTerrainStripBucketVertexCount( nBucket ), 0u, 0u, nTerrainBase };
		nTerrainBase += m_nTerrainStripBucketCounts[nBucket];
	}
	indirect[T3D_WORLD_INDIRECT_NONTERRAIN_INDEX] = { T3D_WORLD_CLUSTER_RASTER_VERTEX_COUNT, 0u, 0u, nTerrainBase };
	uint32_t				  counters[T3D_WORLD_VIS_COUNTER_COUNT]{};
	const auto				  tables = worldPublicationTables( rPublication );
	Terrain3DRdGpuBufferBatch uploads( *m_pContext );
	memset( m_dirtySections, 0, sizeof( m_dirtySections ) );
	m_pPublicationBatch = &uploads;
	const bool staged = [&]() {
		for ( uint32_t i = 0u; i < WORLD_BUFFER_MATERIALS; ++i ) {
			if ( !uploadBufferWarm( static_cast< t3dWorldBufferId_t >( i ),
									tables[i].data,
									uint32_t( tables[i].bytes ) ) ) {
				return false;
			}
		}
		return ( pVirtualTexture ? uploadResolvedMaterialsWarm( rPublication, *pVirtualTexture )
								 : uploadBufferWarm( WORLD_BUFFER_MATERIALS,
													 rPublication.m_materials.data(),
													 uint32_t( rPublication.m_materials.size() *
															   sizeof( t3dRenderingMaterialGpu_t ) ) ) ) &&
			   uploadBufferWarm(
				   WORLD_BUFFER_VISIBILITY_CANDIDATES,
				   rPublication.m_visibilityCandidates.data(),
				   uint32_t( rPublication.m_visibilityCandidates.size() * sizeof( t3dWorldVisibleItem_t ) ) );
	}();
	m_pPublicationBatch = NULL;
	if ( !staged || !uploads.flush() ) {
		return false;
	}
	for ( uint32_t i = 0u; i < WORLD_BUFFER_INDIRECT; ++i ) {
		if ( m_dirtySections[i] ) {
			m_bPublicationUploadCached[i] = true;
			m_nUploadBytes += m_sections[i].m_nSizeBytes;
		}
	}
	if ( !uploadBufferWarm( WORLD_BUFFER_VISIBILITY_COUNTERS, counters, sizeof( counters ) ) ||
		 !uploadBufferWarm( WORLD_BUFFER_INDIRECT, indirect, sizeof( indirect ) ) ) {
		return false;
	}

	m_nUploadedPublicationGeneration = rPublication.m_nGeneration;
	return true;
}

bool Terrain3DWorldResources::uploadFrameHot(
	const t3dWorldFrame_t &frame ) {
	if ( !isInitialized() ) {
		return false;
	}
	m_hotIndex = m_pContext->hotPassIndex();
	uint32_t	bases[20]{};
	const auto &strides = worldBufferStrides;
	for ( uint32_t i = 0u; i < WORLD_BUFFER_COUNT; ++i ) {
		bases[i] = bufferOffset( static_cast< t3dWorldBufferId_t >( i ) ) / strides[i];
	}
	return m_pContext->writeHotRecord( offsetof( Terrain3DRdHotRecord, world ), &frame, sizeof( frame ) ) &&
		   m_pContext->writeHotRecord( offsetof( Terrain3DRdHotRecord, world_bases ), bases, sizeof( bases ) );
}

void Terrain3DWorldResources::shutdownCold() {
	if ( isInitialized() ) {
		// Keep fixed spans and CPU staging across pipeline/lane recovery. Physical
		// retirement and reuse belong to an explicit shared generation transition.
		m_nUploadedPublicationGeneration = 0u;
		return;
	}
	m_materialShadow.clear();
	m_transfer.clear();
	m_shadowMemory.reset();
	m_transferMemory.reset();
	memset( m_bPublicationUploadCached, 0, sizeof( m_bPublicationUploadCached ) );
	memset( m_sectionSignatures, 0, sizeof( m_sectionSignatures ) );
	for ( auto &section : m_sections ) {
		section = {};
	}
	m_coldSpan = {};
	m_scratchSpan = {};
	m_fixedIndirectSlice = {};
	m_pFixedIndirectBuffer = NULL;
	m_pContext = NULL;
	m_nGpuBufferBytes = m_nUploadedPublicationGeneration = m_nResourceGeneration = 0u;
	memset( m_nTerrainStripBucketCounts, 0, sizeof( m_nTerrainStripBucketCounts ) );
	m_nTerrainStripUsefulVertices = m_nTerrainStripFixedSubmittedVertices = m_nTerrainStripBucketSubmittedVertices = 0u;
}

void Terrain3DWorldResources::releaseArenaSpansFinal(
	void ) {
	if ( m_pContext ) {
		m_pContext->releaseArenaSpanFinal( Terrain3DRdContext::COLD, m_coldSpan );
		m_pContext->releaseArenaSpanFinal( Terrain3DRdContext::SCRATCH, m_scratchSpan );
	}
	m_coldSpan = {};
	m_scratchSpan = {};
	m_fixedIndirectSlice = {};
	for ( auto &section : m_sections ) {
		section = {};
	}
	m_materialShadow.clear();
	m_transfer.clear();
	m_shadowMemory.reset();
	m_transferMemory.reset();
	m_pFixedIndirectBuffer = NULL;
	m_pContext = NULL;
	m_nGpuBufferBytes = 0u;
	m_nUploadedPublicationGeneration = 0u;
}

bool Terrain3DWorldResources::isInitialized() const {
	return m_pContext && m_pContext->arena( Terrain3DRdContext::COLD ).ownsSlice( m_coldSpan ) &&
		   m_pContext->arena( Terrain3DRdContext::SCRATCH ).ownsSlice( m_scratchSpan );
}

const RID &Terrain3DWorldResources::getBuffer(
	t3dWorldBufferId_t id ) const {
	static const RID invalid;
	return id < WORLD_BUFFER_COUNT && m_pContext ? bufferFor( id ).rid() : invalid;
}
uint32_t Terrain3DWorldResources::getCapacityBytes(
	t3dWorldBufferId_t id ) const {
	return id >= WORLD_BUFFER_COUNT		 ? 0u
		   : id == WORLD_BUFFER_INDIRECT ? m_fixedIndirectSlice.m_nCapacityBytes
										 : m_sections[id].m_nCapacityBytes;
}
uint32_t Terrain3DWorldResources::getUsedBytes(
	t3dWorldBufferId_t id ) const {
	return id >= WORLD_BUFFER_COUNT		 ? 0u
		   : id == WORLD_BUFFER_INDIRECT ? m_fixedIndirectSlice.m_nSizeBytes
										 : m_sections[id].m_nSizeBytes;
}

uint32_t Terrain3DWorldResources::getTerrainStripBucketCount(
	const uint32_t nBucket ) const {
	return nBucket < T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT ? m_nTerrainStripBucketCounts[nBucket] : 0u;
}
size_t Terrain3DWorldResources::getRetainedCpuBytes() const {
	return m_materialShadow.size() + m_transfer.size();
}

Dictionary Terrain3DWorldResources::getArenaDiagnostics() const {
	Dictionary result = m_pContext ? m_pContext->getSharedArenaDiagnostics() : Dictionary();
	result["borrowed_world_capacity_bytes"] = int64_t( m_nGpuBufferBytes );
	result["staging_capacity_bytes"] = int64_t( getRetainedCpuBytes() );
	result["staging_reallocations"] = int64_t( m_nStagingReallocations );
	const t3dWorldBufferId_t mesh_sections[3] = { WORLD_BUFFER_VERTICES,
												  WORLD_BUFFER_CLUSTER_VERTEX_INDICES,
												  WORLD_BUFFER_TRIANGLES };
	uint64_t				 mesh_capacity = 0u, mesh_used = 0u;
	for ( const t3dWorldBufferId_t id : mesh_sections ) {
		mesh_capacity += getCapacityBytes( id );
		mesh_used += getUsedBytes( id );
	}
	result["mesh_geometry_reserved_bytes"] = ( int64_t ) mesh_capacity;
	result["mesh_geometry_used_bytes"] = ( int64_t ) mesh_used;
	result["terrain_cluster_reserved_bytes"] = ( int64_t ) getCapacityBytes( WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS );
	result["terrain_cluster_used_bytes"] = ( int64_t ) getUsedBytes( WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS );
	result["terrain_bounds_reserved_bytes"] = ( int64_t ) getCapacityBytes( WORLD_BUFFER_TERRAIN_STRIP_CLUSTER_BOUNDS );
	return result;
}
