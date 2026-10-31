// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/utility/terrain_3d_object_bytes.h"
#include "rendering/rd/core/terrain_3d_render_publication_builder.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "core/utility/terrain_3d_generation.h"

#include "assets/catalog/terrain_3d_assets.h"
#include "core/utility/logger.h"
#include "data/model/terrain_3d_data.h"
#include "data/model/terrain_3d_region.h"
#include "data/formats/terrain_3d_half_buffer.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"
#include "instances/physics/terrain_3d_instance_occlusion.h"
#include "instances/runtime/terrain_3d_instancer.h"
#include "lighting/runtime/terrain_3d_region_lighting.h"
#include "memory/core/terrain_3d_memory.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/instances/terrain_3d_rd_mesh_cache.h"
#include "rendering/surface/terrain_3d_ocean_settings.h"
#include "rendering/geometry/terrain_3d_mesher.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <math.h>
#include <string.h>

static Terrain3DHLODStreamer *t3dRdGetInstanceHlodStreamer(
	Terrain3DOwner *const pTerrain ) {
	return pTerrain ? pTerrain->hlodObject() : NULL;
}

static size_t t3dRdPublicationGrowthTarget(
	const size_t nRequired,
	const size_t nCurrentCapacity ) {
	if ( nRequired == 0u ) {
		return nCurrentCapacity;
	}
	const size_t required_headroom = nRequired / 2u + 16u;
	const size_t by_required = nRequired > SIZE_MAX - required_headroom ? SIZE_MAX : nRequired + required_headroom;
	const size_t current_growth = nCurrentCapacity / 2u + 16u;
	const size_t by_current =
		nCurrentCapacity > SIZE_MAX - current_growth ? SIZE_MAX : nCurrentCapacity + current_growth;
	return MAX( nRequired, MAX( by_required, by_current ) );
}
static float t3dWorldUnpackSnorm16(
	const uint16_t nValue ) {
	const int16_t value = ( int16_t ) nValue;
	return MAX( -1.0f, ( float ) ( value ) / 32767.0f );
}

static t3dWorldTransform_t t3dWorldDecodeInstanceTransform(
	const t3dRdInstanceRecord_t &rRecord ) {
	t3dWorldTransform_t result{};
	float				x = t3dWorldUnpackSnorm16( ( uint16_t ) ( rRecord.m_nQuaternion01 & 0xffffu ) );
	float				y = t3dWorldUnpackSnorm16( ( uint16_t ) ( rRecord.m_nQuaternion01 >> 16u ) );
	float				z = t3dWorldUnpackSnorm16( ( uint16_t ) ( rRecord.m_nQuaternion23 & 0xffffu ) );
	float				w = t3dWorldUnpackSnorm16( ( uint16_t ) ( rRecord.m_nQuaternion23 >> 16u ) );
	const float			q_len = sqrtf( x * x + y * y + z * z + w * w );
	if ( q_len > 0.000001f ) {
		const float inv = 1.0f / q_len;
		x *= inv;
		y *= inv;
		z *= inv;
		w *= inv;
	} else {
		x = y = z = 0.0f;
		w = 1.0f;
	}
	const float sx = t3dHalfDecode( ( uint16_t ) ( rRecord.m_nScale01 & 0xffffu ) );
	const float sy = t3dHalfDecode( ( uint16_t ) ( rRecord.m_nScale01 >> 16u ) );
	const float sz = t3dHalfDecode( ( uint16_t ) ( rRecord.m_nScale2Flags & 0xffffu ) );
	const float xx = x * x, yy = y * y, zz = z * z;
	const float xy = x * y, xz = x * z, yz = y * z;
	const float wx = w * x, wy = w * y, wz = w * z;
	result.m_fRow0[0] = ( 1.0f - 2.0f * ( yy + zz ) ) * sx;
	result.m_fRow0[1] = ( 2.0f * ( xy - wz ) ) * sy;
	result.m_fRow0[2] = ( 2.0f * ( xz + wy ) ) * sz;
	result.m_fRow0[3] = rRecord.m_szPosition[0];
	result.m_fRow1[0] = ( 2.0f * ( xy + wz ) ) * sx;
	result.m_fRow1[1] = ( 1.0f - 2.0f * ( xx + zz ) ) * sy;
	result.m_fRow1[2] = ( 2.0f * ( yz - wx ) ) * sz;
	result.m_fRow1[3] = rRecord.m_szPosition[1];
	result.m_fRow2[0] = ( 2.0f * ( xz - wy ) ) * sx;
	result.m_fRow2[1] = ( 2.0f * ( yz + wx ) ) * sy;
	result.m_fRow2[2] = ( 1.0f - 2.0f * ( xx + yy ) ) * sz;
	result.m_fRow2[3] = rRecord.m_szPosition[2];
	return result;
}

static uint32_t t3dWorldSourceId(
	const uint64_t nSourceKey ) {
	return ( uint32_t ) ( nSourceKey ) ^ ( uint32_t ) ( nSourceKey >> 32u );
}

static void t3dWorldAccumulateCapacity(
	size_t		&rValue,
	const size_t nAdd ) {
	rValue = nAdd > SIZE_MAX - rValue ? SIZE_MAX : rValue + nAdd;
}

static size_t t3dWorldTerrainStripClusterCountForPart(
	const t3dClipmapStripPart_t &rPart ) {
	if ( rPart.m_nCellWidth == 0u || rPart.m_nCellHeight == 0u ) {
		return 0u;
	}
	size_t count = 0u;
	for ( uint32_t x = 0u; x < rPart.m_nCellWidth; ) {
		const uint32_t width = MIN( T3D_WORLD_TERRAIN_STRIP_MAX_CELL_WIDTH, ( uint32_t ) rPart.m_nCellWidth - x );
		const uint32_t vertices_per_run = width * 2u + 4u;
		const uint32_t rows_per_cluster = MAX( 1u, T3D_WORLD_TERRAIN_STRIP_RASTER_VERTEX_COUNT / vertices_per_run );
		count += ( ( size_t ) rPart.m_nCellHeight + rows_per_cluster - 1u ) / rows_per_cluster;
		x += width;
	}
	return count;
}

static size_t t3dWorldTerrainStripClusterCountForMesh(
	const Terrain3DClipmapSource &rMesher,
	const t3dMeshType_t			  type ) {
	size_t							   count = 0u;
	const t3dClipmapStripPart_t *const pParts = rMesher.getClipmapStripParts( type );
	const uint16_t					   part_count = rMesher.getClipmapStripPartCount( type );
	for ( uint16_t i = 0u; i < part_count; ++i ) {
		t3dWorldAccumulateCapacity( count, t3dWorldTerrainStripClusterCountForPart( pParts[i] ) );
	}
	return count;
}

static bool t3dWorldAppendTerrainStripClusters(
	Terrain3DWorldPublication	 &rWorld,
	const Terrain3DClipmapSource &rMesher,
	const t3dRdPatch_t			 &rPatch,
	const uint32_t				  nPatchId ) {
	const t3dMeshType_t				   type = rPatch.m_nLod == 0u ? LOD0_MESH : OUTER_RING_MESH;
	const t3dClipmapStripPart_t *const pParts = rMesher.getClipmapStripParts( type );
	const uint16_t					   part_count = rMesher.getClipmapStripPartCount( type );
	if ( !pParts || part_count == 0u || rPatch.m_nLod > UINT16_MAX || rPatch.m_nTier > 3u || rPatch.m_nStitchX > 3u ||
		 rPatch.m_nStitchZ > 3u ) {
		return false;
	}

	for ( uint16_t iPart = 0u; iPart < part_count; ++iPart ) {
		const t3dClipmapStripPart_t &part = pParts[iPart];
		for ( uint32_t x_offset = 0u; x_offset < part.m_nCellWidth; ) {
			const uint32_t width =
				MIN( T3D_WORLD_TERRAIN_STRIP_MAX_CELL_WIDTH, ( uint32_t ) part.m_nCellWidth - x_offset );
			const uint32_t vertices_per_run = width * 2u + 4u;
			const uint32_t rows_per_cluster = MAX( 1u, T3D_WORLD_TERRAIN_STRIP_RASTER_VERTEX_COUNT / vertices_per_run );
			for ( uint32_t row_base = 0u; row_base < part.m_nCellHeight; ) {
				const uint32_t row_count = MIN( rows_per_cluster, ( uint32_t ) part.m_nCellHeight - row_base );
				const uint32_t actual_vertex_count = row_count * vertices_per_run;
				const int32_t  grid_x = ( int32_t ) part.m_nOriginX + ( int32_t ) x_offset;
				const int32_t  grid_z = ( int32_t ) part.m_nOriginZ + ( int32_t ) row_base;
				if ( grid_x < INT16_MIN || grid_x > INT16_MAX || grid_z < INT16_MIN || grid_z > INT16_MAX ||
					 width > UINT16_MAX || row_count > UINT16_MAX || actual_vertex_count == 0u ||
					 actual_vertex_count > T3D_WORLD_TERRAIN_STRIP_RASTER_VERTEX_COUNT ||
					 actual_vertex_count > UINT16_MAX || part.m_nPartCode >= T3D_CLIPMAP_PART_CODE_COUNT ) {
					return false;
				}

				t3dWorldTerrainCluster_t cluster{};
				cluster.m_nPatchId = nPatchId;
				cluster.m_nPackedGridOrigin = t3dWorldPackTerrainGridOrigin( ( int16_t ) grid_x, ( int16_t ) grid_z );
				cluster.m_nPackedGridExtent =
					t3dWorldPackTerrainGridExtent( ( uint16_t ) width, ( uint16_t ) row_count );
				cluster.m_nPackedVertexCountLod =
					t3dWorldPackTerrainVertexCountLod( ( uint16_t ) actual_vertex_count, ( uint16_t ) rPatch.m_nLod );
				const bool serpentine = ( part.m_nFlags & T3D_CLIPMAP_STRIP_FLAG_SERPENTINE_ROWS ) != 0u;
				cluster.m_nFlags = t3dWorldPackTerrainStripState( ( uint16_t ) ( rPatch.m_nFlags & 0xffffu ),
																  ( uint8_t ) rPatch.m_nTier,
																  ( uint8_t ) rPatch.m_nStitchX,
																  ( uint8_t ) rPatch.m_nStitchZ,
																  ( uint8_t ) part.m_nPartCode,
																  serpentine,
																  serpentine && ( ( row_base & 1u ) != 0u ) );
				rWorld.m_terrainStripClusters.push_back( cluster );

				++rWorld.m_nTerrainStripClusterCount;
				row_base += row_count;
			}
			x_offset += width;
		}
	}
	return true;
}

uint32_t Terrain3DRenderPublicationBuilder::selectWritablePublicationBank(
	const uint32_t nReaderBank,
	const uint32_t nPendingBank ) const {
	for ( uint32_t bank = 0u; bank < T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT; ++bank ) {
		if ( bank != nReaderBank && bank != nPendingBank ) {
			return bank;
		}
	}
	return T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
}

bool Terrain3DRenderPublicationBuilder::hasInstancePublicationCapacity(
	const t3dPublicationServices_t &rServices ) const {
	Terrain3DInstancer *const	 instancer = rServices.m_pTerrain ? rServices.m_pTerrain->instancerObject() : NULL;
	Terrain3DHLODStreamer *const hlod_streamer =
		t3dRdGetInstanceHlodStreamer( rServices.m_pTerrain ? rServices.m_pTerrain : NULL );
	const size_t regular_instances = instancer ? instancer->getRdSnapshotInstanceCapacity() : 0u;
	const size_t regular_cells = instancer ? instancer->getRdSnapshotCellCapacity() : 0u;
	const size_t hlod_instances = hlod_streamer ? hlod_streamer->getRdPublicationInstanceCapacity() : 0u;
    const size_t hlod_cells = hlod_streamer ? hlod_streamer->getRdPublicationCellCapacity() : 0u;
    const size_t hlod_sources = hlod_streamer ? hlod_streamer->getRdPublicationSourceCapacity() : 0u;
	const size_t combined_instances = regular_instances + hlod_instances;
	const size_t combined_cells = regular_cells + hlod_cells;
	if ( m_cachedRegularInstanceScene.m_instances.capacity() < regular_instances ||
		 m_cachedRegularInstanceScene.m_cells.capacity() < regular_cells ||
		 m_cachedRegularInstanceScene.m_dirtyRanges.capacity() < regular_cells ||
		 m_cachedRegularInstanceScene.m_dirtyCells.capacity() < regular_cells ||
		 m_cachedHlodInstanceScene.m_instances.capacity() < hlod_instances ||
		 m_cachedHlodInstanceScene.m_cells.capacity() < hlod_cells ||
		 m_cachedHlodInstanceScene.m_dirtyRanges.capacity() < hlod_cells ||
		 m_cachedHlodInstanceScene.m_dirtyCells.capacity() < hlod_cells ||
		 m_cachedInstanceScene.m_instances.capacity() < combined_instances ||
		 m_cachedInstanceScene.m_cells.capacity() < combined_cells ||
		 m_cachedInstanceScene.m_dirtyRanges.capacity() < combined_cells ||
		 m_cachedInstanceScene.m_dirtyCells.capacity() < combined_cells ||
		 m_cachedHlodMeshSources.capacity() < hlod_sources ) {
		return false;
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceSceneBanks ); iBank++ ) {
		const t3dRdInstanceSceneSnapshot_t &snapshot = ( m_instanceSceneBanks )[iBank].m_snapshot;
		if ( snapshot.m_instances.capacity() < combined_instances || snapshot.m_cells.capacity() < combined_cells ||
			 snapshot.m_dirtyRanges.capacity() < combined_cells || snapshot.m_dirtyCells.capacity() < combined_cells ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DRenderPublicationBuilder::hasInstanceMeshBankCapacity(
	void ) const {
	if ( m_meshSourceIndex.capacity() < m_cachedInstanceMeshes.m_lods.size() ) {
		return false;
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceMeshBanks ); iBank++ ) {
		const t3dRdMeshSourceSnapshot_t &snapshot = ( m_instanceMeshBanks )[iBank].m_snapshot;
		if ( snapshot.m_vertexChunkHashes.capacity() < m_cachedInstanceMeshes.m_vertexChunkHashes.size() ||
			 snapshot.m_index32ChunkHashes.capacity() < m_cachedInstanceMeshes.m_index32ChunkHashes.size() ||
			 snapshot.m_surfaces.capacity() < m_cachedInstanceMeshes.m_surfaces.size() ||
			 snapshot.m_lods.capacity() < m_cachedInstanceMeshes.m_lods.size() ||
			 snapshot.m_materials.capacity() < m_cachedInstanceMeshes.m_materials.size() ||
			 snapshot.m_vtMaterials.capacity() < m_cachedInstanceMeshes.m_vtMaterials.size() ||
			 snapshot.m_materialKeys.capacity() < m_cachedInstanceMeshes.m_materialKeys.size() ||
			 snapshot.m_vtStableKeys.capacity() < m_cachedInstanceMeshes.m_vtStableKeys.size() ||
			 snapshot.m_worldVertices.capacity() < m_cachedInstanceMeshes.m_worldVertices.size() ||
			 snapshot.m_worldClusterVertexIndices.capacity() <
				 m_cachedInstanceMeshes.m_worldClusterVertexIndices.size() ||
			 snapshot.m_worldTriangleWords.capacity() < m_cachedInstanceMeshes.m_worldTriangleWords.size() ||
			 snapshot.m_worldClusters.capacity() < m_cachedInstanceMeshes.m_worldClusters.size() ||
			 snapshot.m_worldClusterVtMetadata.capacity() < m_cachedInstanceMeshes.m_worldClusterVtMetadata.size() ) {
			return false;
		}
	}
	return true;
}

void Terrain3DRenderPublicationBuilder::recordCapacityMiss(
	const char *const pszOwner,
	const size_t	  nRequired,
	const size_t	  nCapacity,
	const bool		  bMeshBank ) {
	++m_nCapacityMissCount;
	++m_nOwnerGrowthDeniedCount;
	const bool already_pending = bMeshBank ? m_bInstanceMeshBankCapacityPending : m_bInstancePublicationCapacityPending;
	if ( bMeshBank ) {
		m_bInstanceMeshBankCapacityPending = true;
	} else {
		m_bInstancePublicationCapacityPending = true;
	}
	if ( !already_pending ) {
		const bool editor_growth = Engine::get_singleton() != NULL && Engine::get_singleton()->is_editor_hint();
		if ( editor_growth ) {
			LOG( DEBUG,
				 pszOwner,
				 " owner capacity growth; required=",
				 nRequired,
				 ", capacity=",
				 nCapacity,
				 ". Servicing cold reserve." );
		} else {
			LOG( WARN,
				 pszOwner,
				 " owner capacity miss; required=",
				 nRequired,
				 ", capacity=",
				 nCapacity,
				 ". Cold capacity rebuild queued." );
		}
	}
}

bool Terrain3DRenderPublicationBuilder::ensureInstancePublicationStorage(
	const t3dPublicationServices_t &rServices ) {
	if ( !m_bColdCapacityServiceActive ) {
		++m_nOwnerGrowthDeniedCount;
		LOG( ERROR, "RD publication owner backing growth requested outside serviceColdCapacity()" );
		return false;
	}
	Terrain3DInstancer *const	 instancer = rServices.m_pTerrain ? rServices.m_pTerrain->instancerObject() : NULL;
	Terrain3DHLODStreamer *const hlod_streamer =
		t3dRdGetInstanceHlodStreamer( rServices.m_pTerrain ? rServices.m_pTerrain : NULL );
	const size_t regular_instances = instancer ? instancer->getRdSnapshotInstanceCapacity() : 0u;
	const size_t regular_cells = instancer ? instancer->getRdSnapshotCellCapacity() : 0u;
	const size_t hlod_instances = hlod_streamer ? hlod_streamer->getRdPublicationInstanceCapacity() : 0u;
    const size_t hlod_cells = hlod_streamer ? hlod_streamer->getRdPublicationCellCapacity() : 0u;
    const size_t hlod_sources = hlod_streamer ? hlod_streamer->getRdPublicationSourceCapacity() : 0u;
	const size_t combined_instances = regular_instances + hlod_instances;
	const size_t combined_cells = regular_cells + hlod_cells;

	const bool needs_rebuild = m_cachedRegularInstanceScene.m_instances.capacity() < regular_instances ||
							   m_cachedRegularInstanceScene.m_cells.capacity() < regular_cells ||
							   m_cachedRegularInstanceScene.m_dirtyRanges.capacity() < regular_cells ||
							   m_cachedRegularInstanceScene.m_dirtyCells.capacity() < regular_cells ||
							   m_cachedHlodInstanceScene.m_instances.capacity() < hlod_instances ||
							   m_cachedHlodInstanceScene.m_cells.capacity() < hlod_cells ||
							   m_cachedHlodInstanceScene.m_dirtyRanges.capacity() < hlod_cells ||
							   m_cachedHlodInstanceScene.m_dirtyCells.capacity() < hlod_cells ||
							   m_cachedInstanceScene.m_instances.capacity() < combined_instances ||
							   m_cachedInstanceScene.m_cells.capacity() < combined_cells ||
							   m_cachedInstanceScene.m_dirtyRanges.capacity() < combined_cells ||
							   m_cachedInstanceScene.m_dirtyCells.capacity() < combined_cells ||
							   m_cachedHlodMeshSources.capacity() < hlod_sources;
	bool bank_needs_rebuild = false;
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceSceneBanks ); iBank++ ) {
		const t3dRdInstanceSceneSnapshot_t &snapshot = ( m_instanceSceneBanks )[iBank].m_snapshot;
		bank_needs_rebuild = bank_needs_rebuild || snapshot.m_instances.capacity() < combined_instances ||
							 snapshot.m_cells.capacity() < combined_cells ||
							 snapshot.m_dirtyRanges.capacity() < combined_cells ||
							 snapshot.m_dirtyCells.capacity() < combined_cells;
	}
	if ( !needs_rebuild && !bank_needs_rebuild ) {
		return true;
	}

	if ( !m_cachedRegularInstanceScene.reserveCold( regular_instances, regular_cells ) ||
		 !m_cachedHlodInstanceScene.reserveCold( hlod_instances, hlod_cells ) ||
		 !m_cachedInstanceScene.reserveCold( combined_instances, combined_cells ) ) {
		return false;
	}
	if ( m_cachedHlodMeshSources.capacity() < hlod_sources ) {
		if ( !t3dContainerReserveCold( m_cachedHlodMeshSources, hlod_sources ) ) {
			return false;
		}
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceSceneBanks ); iBank++ ) {
		if ( !( m_instanceSceneBanks )[iBank].m_snapshot.reserveCold( combined_instances, combined_cells ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DRenderPublicationBuilder::ensureInstanceMeshBankStorage(
	void ) {
	if ( !m_bColdCapacityServiceActive ) {
		++m_nOwnerGrowthDeniedCount;
		LOG( ERROR, "RD mesh-bank owner backing growth requested outside serviceColdCapacity()" );
		return false;
	}
	if ( !m_meshSourceIndex.reserveCold( m_cachedInstanceMeshes.m_lods.size() ) ) {
		return false;
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceMeshBanks ); iBank++ ) {
		t3dRdInstanceMeshBank_t	  &bank = ( m_instanceMeshBanks )[iBank];
		t3dRdMeshSourceSnapshot_t &snapshot = bank.m_snapshot;
		if ( snapshot.m_vertexChunkHashes.capacity() < m_cachedInstanceMeshes.m_vertexChunkHashes.size() ) {
			if ( !t3dContainerReserveCold(
					 snapshot.m_vertexChunkHashes,
					 t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_vertexChunkHashes.size(),
												   snapshot.m_vertexChunkHashes.capacity() ) ) ) {
				return false;
			}
		}
		if ( snapshot.m_index32ChunkHashes.capacity() < m_cachedInstanceMeshes.m_index32ChunkHashes.size() ) {
			if ( !t3dContainerReserveCold(
					 snapshot.m_index32ChunkHashes,
					 t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_index32ChunkHashes.size(),
												   snapshot.m_index32ChunkHashes.capacity() ) ) ) {
				return false;
			}
		}
		if ( snapshot.m_surfaces.capacity() < m_cachedInstanceMeshes.m_surfaces.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_surfaces,
										   t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_surfaces.size(),
																		 snapshot.m_surfaces.capacity() ) ) ) {
				return false;
			}
		}
		if ( snapshot.m_lods.capacity() < m_cachedInstanceMeshes.m_lods.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_lods,
										   t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_lods.size(),
																		 snapshot.m_lods.capacity() ) ) ) {
				return false;
			}
		}
		if ( snapshot.m_materials.capacity() < m_cachedInstanceMeshes.m_materials.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_materials,
										   t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_materials.size(),
																		 snapshot.m_materials.capacity() ) ) ) {
				return false;
			}
		}
		if ( snapshot.m_vtMaterials.capacity() < m_cachedInstanceMeshes.m_vtMaterials.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_vtMaterials,
										   t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_vtMaterials.size(),
																		 snapshot.m_vtMaterials.capacity() ) ) ) {
				return false;
			}
		}
		if ( snapshot.m_materialKeys.capacity() < m_cachedInstanceMeshes.m_materialKeys.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_materialKeys,
										   t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_materialKeys.size(),
																		 snapshot.m_materialKeys.capacity() ) ) ) {
				return false;
			}
		}
		if ( snapshot.m_vtStableKeys.capacity() < m_cachedInstanceMeshes.m_vtStableKeys.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_vtStableKeys,
										   t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_vtStableKeys.size(),
																		 snapshot.m_vtStableKeys.capacity() ) ) ) {
				return false;
			}
		}
		if ( snapshot.m_worldVertices.capacity() < m_cachedInstanceMeshes.m_worldVertices.size() &&
			 !t3dContainerReserveCold( snapshot.m_worldVertices,
									   t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_worldVertices.size(),
																	 snapshot.m_worldVertices.capacity() ) ) ) {
			return false;
		}
		if ( snapshot.m_worldClusterVertexIndices.capacity() <
				 m_cachedInstanceMeshes.m_worldClusterVertexIndices.size() &&
			 !t3dContainerReserveCold(
				 snapshot.m_worldClusterVertexIndices,
				 t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_worldClusterVertexIndices.size(),
											   snapshot.m_worldClusterVertexIndices.capacity() ) ) ) {
			return false;
		}
		if ( snapshot.m_worldTriangleWords.capacity() < m_cachedInstanceMeshes.m_worldTriangleWords.size() &&
			 !t3dContainerReserveCold( snapshot.m_worldTriangleWords,
									   t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_worldTriangleWords.size(),
																	 snapshot.m_worldTriangleWords.capacity() ) ) ) {
			return false;
		}
		if ( snapshot.m_worldClusters.capacity() < m_cachedInstanceMeshes.m_worldClusters.size() &&
			 !t3dContainerReserveCold( snapshot.m_worldClusters,
									   t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_worldClusters.size(),
																	 snapshot.m_worldClusters.capacity() ) ) ) {
			return false;
		}
		if ( snapshot.m_worldClusterVtMetadata.capacity() < m_cachedInstanceMeshes.m_worldClusterVtMetadata.size() &&
			 !t3dContainerReserveCold(
				 snapshot.m_worldClusterVtMetadata,
				 t3dRdPublicationGrowthTarget( m_cachedInstanceMeshes.m_worldClusterVtMetadata.size(),
											   snapshot.m_worldClusterVtMetadata.capacity() ) ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DRenderPublicationBuilder::hasWorldPublicationCapacity(
	const t3dWorldPublicationCapacity_t &rRequired ) const {
	for ( size_t i = 0u; i < T3D_ARRAY_COUNT( m_worldPublicationBanks ); ++i ) {
		if ( !m_worldPublicationBanks[i].m_publication.hasCapacity( rRequired ) ) {
			return false;
		}
	}
	return true;
}

#define T3D_WORLD_CAP_MAX( field )                                                                                     \
	m_worldPublicationRequired.field = MAX( m_worldPublicationRequired.field, rRequired.field )

void Terrain3DRenderPublicationBuilder::requestWorldPublicationCapacity(
	const t3dWorldPublicationCapacity_t &rRequired ) {
	T3D_WORLD_CAP_MAX( m_nVertices );
	T3D_WORLD_CAP_MAX( m_nClusterVertexIndices );
	T3D_WORLD_CAP_MAX( m_nTriangleWords );
	T3D_WORLD_CAP_MAX( m_nClusters );
	T3D_WORLD_CAP_MAX( m_nClusterVtMetadata );
	T3D_WORLD_CAP_MAX( m_nTerrainStripClusters );
	T3D_WORLD_CAP_MAX( m_nTransforms );
	T3D_WORLD_CAP_MAX( m_nInstances );
	T3D_WORLD_CAP_MAX( m_nCells );
	T3D_WORLD_CAP_MAX( m_nHlodRecords );
	T3D_WORLD_CAP_MAX( m_nMaterials );
	T3D_WORLD_CAP_MAX( m_nVisibilityCandidates );
#undef T3D_WORLD_CAP_MAX
	m_bWorldPublicationCapacityPending = true;
}

bool Terrain3DRenderPublicationBuilder::ensureWorldPublicationStorage(
	void ) {
	if ( !m_bColdCapacityServiceActive ) {
		++m_nOwnerGrowthDeniedCount;
		return false;
	}
	for ( size_t i = 0u; i < T3D_ARRAY_COUNT( m_worldPublicationBanks ); ++i ) {
		if ( !m_worldPublicationBanks[i].m_publication.reserveCold( m_worldPublicationRequired ) ) {
			return false;
		}
	}
	return true;
}

void Terrain3DRenderPublicationBuilder::serviceColdCapacity(
	const t3dPublicationServices_t &rServices ) {
	const bool publication_needed =
		m_bInstancePublicationCapacityPending || !hasInstancePublicationCapacity( rServices );
	// Mesh-bank requirements are not known until the immutable source snapshot
	// has been built. Keep the pending bit armed until that discovery exists;
	// otherwise initialization clears the request against an empty snapshot and
	// the first real publication observes capacity=0.
	const bool mesh_bank_needed =
		m_bCachedInstanceMeshesValid && ( m_bInstanceMeshBankCapacityPending || !hasInstanceMeshBankCapacity() );
	const bool world_needed =
		m_bWorldPublicationCapacityPending && !hasWorldPublicationCapacity( m_worldPublicationRequired );
	if ( !publication_needed && !mesh_bank_needed && !world_needed ) {
		return;
	}
	Terrain3DColdPhaseScope phase_scope;
	m_bColdCapacityServiceActive = true;
	++m_nColdCapacityRebuildCount;
	bool success = true;
	if ( publication_needed ) {
		if ( ensureInstancePublicationStorage( rServices ) ) {
			m_bInstancePublicationCapacityPending = false;
		} else {
			m_bInstancePublicationCapacityPending = true;
			success = false;
		}
	}
	if ( mesh_bank_needed ) {
		if ( ensureInstanceMeshBankStorage() ) {
			m_bInstanceMeshBankCapacityPending = false;
		} else {
			m_bInstanceMeshBankCapacityPending = true;
			success = false;
		}
	}
	if ( world_needed ) {
		if ( ensureWorldPublicationStorage() ) {
			m_bWorldPublicationCapacityPending = false;
		} else {
			m_bWorldPublicationCapacityPending = true;
			success = false;
		}
	}
	m_bColdCapacityServiceActive = false;
	if ( !success ) {
		rServices.m_host.reportPublicationError( "RD publication owner could not establish requested cold capacity" );
	}
}

void Terrain3DRenderPublicationBuilder::publishPatchState(
	const t3dPublicationServices_t &rServices,
	t3dRdFrameSnapshot_t		   &snapshot ) {
	snapshot.m_nMeshSize = ( uint32_t ) ( rServices.m_pTerrain->getMeshSize() );
	snapshot.m_fVertexSpacing = ( float ) ( s_fRegionVertexSpacing );
	snapshot.m_targetPosition = rServices.m_pTerrain->getClipmapTargetPosition();
	snapshot.m_fSubdiv = ( float ) ( 1 << rServices.m_pTerrain->getTessellationLevel() );
	snapshot.m_fHeightMin = s_fRegionHeightMin;
	snapshot.m_fHeightRange = s_fRegionHeightRange;
	snapshot.m_fDisplacementScale = ( float ) ( rServices.m_pTerrain->getDisplacementScale() );
	snapshot.m_bDisplacementEnabled = rServices.m_pTerrain->getTessellationLevel() > 0;
	Camera3D *publication_camera = NULL;
	if ( Engine::get_singleton()->is_editor_hint() ) {
		EditorInterface *const editor = EditorInterface::get_singleton();
		SubViewport *const	   viewport = editor ? editor->get_editor_viewport_3d( 0 ) : NULL;
		publication_camera = viewport ? viewport->get_camera_3d() : NULL;
	} else if ( rServices.m_pTerrain->sceneNode().get_viewport() ) {
		publication_camera = rServices.m_pTerrain->sceneNode().get_viewport()->get_camera_3d();
	}
	snapshot.m_nCameraCullMask = publication_camera ? publication_camera->get_cull_mask() : UINT32_MAX;
	snapshot.m_nRenderLayers = rServices.m_pTerrain->getRenderLayers();
	snapshot.m_nEditorExecutionMode = ( uint32_t ) ( rServices.m_pTerrain->getEditorExecutionMode() );
	snapshot.m_bEditorAuthoringMode = rServices.m_pTerrain->isEditorAuthoringMode();
	snapshot.m_bDominantSunOnly = rServices.m_pTerrain->isDominantSunOnly();
	snapshot.m_szInstanceTextureMipBias[0] = rServices.m_pTerrain->getClutterTextureMipBias();
	snapshot.m_szInstanceTextureMipBias[1] = rServices.m_pTerrain->getGroundCoverTextureMipBias();
	snapshot.m_szInstanceTextureMipBias[2] = rServices.m_pTerrain->getOccluderTextureMipBias();
	snapshot.m_szInstanceTextureMipBias[3] = ( float ) rServices.m_pTerrain->getInstanceTextureFilterOverrides();
	snapshot.m_nShadowLodCount = rServices.m_pTerrain->getShadowLodCount();
	snapshot.m_nNearShadowLodCount = rServices.m_pTerrain->getNearShadowLodCount();
	snapshot.m_nMiddleShadowLodCount = rServices.m_pTerrain->getMiddleShadowLodCount();
	snapshot.m_nFarShadowLodCount = rServices.m_pTerrain->getFarShadowLodCount();
	snapshot.m_bCastShadows = rServices.m_pTerrain->getCastShadows() != GeometryInstance3D::SHADOW_CASTING_SETTING_OFF;
	snapshot.m_bNearShadowEnabled = rServices.m_pTerrain->getNearShadowEnabled();
	snapshot.m_bMiddleShadowEnabled = rServices.m_pTerrain->getMiddleShadowEnabled();
	snapshot.m_bFarShadowEnabled = rServices.m_pTerrain->getFarShadowEnabled();
	snapshot.m_nPatchCount =
		( uint32_t ) ( rServices.m_pTerrain->getMesher()->fillRdPatches( snapshot.m_szPatches,
																		 T3D_MAX_CLIPMAP_LEVELS ) );
	const float displacement_padding = snapshot.m_bDisplacementEnabled ? abs( snapshot.m_fDisplacementScale ) : 0.0f;
	const float inv_vertex_spacing = 1.0f / MAX( snapshot.m_fVertexSpacing, 1e-6f );
	const t3dRdPatch_t *previous_by_lod[T3D_MAX_CLIPMAP_LEVELS]{};
	for ( uint32_t previous = 0u; previous < m_nPreviousPatchCount; ++previous ) {
		const t3dRdPatch_t &previous_patch = m_previousPatches[previous];
		if ( previous_patch.m_nLod < T3D_MAX_CLIPMAP_LEVELS ) {
			previous_by_lod[previous_patch.m_nLod] = &previous_patch;
		}
	}
	for ( uint32_t i = 0u; i < snapshot.m_nPatchCount; ++i ) {
		t3dRdPatch_t &patch = snapshot.m_szPatches[i];
		patch.m_fBoundsMinY = snapshot.m_fHeightMin - displacement_padding;
		patch.m_fBoundsMaxY = snapshot.m_fHeightMin + snapshot.m_fHeightRange + displacement_padding;
		patch.m_fBoundsHalfExtent =
			( float ) ( snapshot.m_nMeshSize * 4u + 12u ) * patch.m_fScale + displacement_padding;
		patch.m_fPreviousOriginX = patch.m_fOriginX;
		patch.m_fPreviousOriginZ = patch.m_fOriginZ;
		patch.m_fPreviousScale = patch.m_fScale;
		if ( patch.m_nLod < T3D_MAX_CLIPMAP_LEVELS && previous_by_lod[patch.m_nLod] ) {
			const t3dRdPatch_t &previous_patch = *previous_by_lod[patch.m_nLod];
			patch.m_fPreviousOriginX = previous_patch.m_fOriginX;
			patch.m_fPreviousOriginZ = previous_patch.m_fOriginZ;
			patch.m_fPreviousScale = previous_patch.m_fScale;
		}
		// padding_0/1 and high flag bits are invariant per-patch vertex hot-path constants.
		patch.m_fPadding0 = 1.0f / MAX( patch.m_fScale, 1e-6f );
		patch.m_fPadding1 = MAX( 1.0f, patch.m_fScale * inv_vertex_spacing );
		const int	   origin_scale_x = ( int ) Math::round( patch.m_fOriginX * patch.m_fPadding0 );
		const int	   origin_scale_z = ( int ) Math::round( patch.m_fOriginZ * patch.m_fPadding0 );
		const uint32_t phase_x = ( uint32_t ) ( ( origin_scale_x % 4 + 4 ) % 4 );
		const uint32_t phase_z = ( uint32_t ) ( ( origin_scale_z % 4 + 4 ) % 4 );
		patch.m_nFlags = ( patch.m_nFlags & ~0x000f0000u ) | ( phase_x << 16u ) | ( phase_z << 18u );
	}
	const uint64_t patch_bytes_hash =
		snapshot.m_nPatchCount > 0u
			? t3dHashPublicationBytes( snapshot.m_szPatches,
									   ( size_t ) snapshot.m_nPatchCount * sizeof( t3dRdPatch_t ) )
			: 0u;
	const uint64_t patch_fingerprint_words[2] = { patch_bytes_hash, snapshot.m_nPatchCount };
	const uint64_t patch_fingerprint =
		t3dHashPublicationBytes( patch_fingerprint_words, sizeof( patch_fingerprint_words ) );
	if ( !m_bPatchStateInitialized || patch_fingerprint != m_nLastPatchFingerprint ) {
		m_nPatchGeneration = t3dNextGeneration( m_nPatchGeneration );
		m_nLastPatchFingerprint = patch_fingerprint;
		m_bPatchStateInitialized = true;
	}
	snapshot.m_nGeneration = m_nPatchGeneration;
	for ( uint32_t i = 0u; i < snapshot.m_nPatchCount; ++i ) {
		m_previousPatches[i] = snapshot.m_szPatches[i];
	}
	m_nPreviousPatchCount = snapshot.m_nPatchCount;
}

void Terrain3DRenderPublicationBuilder::publishLightingState(
	const t3dPublicationServices_t &rServices,
	t3dRdFrameSnapshot_t		   &snapshot ) const {
	DirectionalLight3D *const light = Object::cast_to< DirectionalLight3D >( rServices.m_pTerrain->getLightTarget() );
	if ( light ) {
		snapshot.m_lightColor = light->get_color() * light->get_param( DirectionalLight3D::PARAM_ENERGY );
		snapshot.m_lightDirection = light->get_global_basis().get_column( 2 ).normalized();
	}
	const Ref< World3D >	 world = rServices.m_pTerrain->sceneNode().get_world_3d();
	const Ref< Environment > environment = world.is_valid() ? world->get_environment() : Ref< Environment >();
	if ( environment.is_valid() ) {
		if ( environment->is_fog_enabled() ) {
			snapshot.m_fogColor = environment->get_fog_light_color() * environment->get_fog_light_energy();
			snapshot.m_fFogDensity = environment->get_fog_density();
			snapshot.m_fFogStart = environment->get_fog_depth_begin();
			snapshot.m_fFogEnd = environment->get_fog_depth_end();
			snapshot.m_fFogCurve = environment->get_fog_depth_curve();
			snapshot.m_nFogMode = ( uint32_t ) ( environment->get_fog_mode() );
		}
	}
}

bool Terrain3DRenderPublicationBuilder::publishOceanState(
	const t3dPublicationServices_t		 &rServices,
	t3dRdFrameSnapshotSlot_t			 &publication,
	t3dRdFrameSnapshot_t				 &snapshot,
	const t3dRdFrameSnapshotSlot_t *const pPending ) {
	const uint64_t ocean_source_generation = rServices.m_pTerrain->getOceanPublicationGeneration();
	if ( !m_bOceanStaticStateInitialized || ocean_source_generation != m_nOceanStaticSourceGeneration ) {
		t3dClearTrivialObjectBytes( m_cachedOceanStaticState );
		t3dRdOceanState_t &static_ocean = m_cachedOceanStaticState;
		static_ocean.m_nEnabled =
			rServices.m_pTerrain->isOceanEnabled() && rServices.m_pTerrain->getOceanMesher() ? 1u : 0u;
		if ( static_ocean.m_nEnabled != 0u ) {
			static_ocean.m_nRenderLayers = rServices.m_pTerrain->getOceanRenderLayers();
			static_ocean.m_nCastShadows =
				rServices.m_pTerrain->getOceanCastShadows() != GeometryInstance3D::SHADOW_CASTING_SETTING_OFF ? 1u : 0u;
			static_ocean.m_nVertexIterations =
				( uint32_t ) ( MAX( rServices.m_pTerrain->getOceanVertexWaveIterations(), 0 ) );
			static_ocean.m_nFragmentIterations =
				( uint32_t ) ( MAX( rServices.m_pTerrain->getOceanFragmentWaveIterations(), 0 ) );
			static_ocean.m_nRefractionMode = ( uint32_t ) ( rServices.m_pTerrain->getOceanRefractionMode() );
			static_ocean.m_nFoamMode = ( uint32_t ) ( rServices.m_pTerrain->getOceanFoamMode() );
			static_ocean.m_nScatteringMode = ( uint32_t ) ( rServices.m_pTerrain->getOceanScatteringMode() );
			static_ocean.m_nShadingMode = ( uint32_t ) ( rServices.m_pTerrain->getOceanShadingMode() );
			static_ocean.m_fMeshSize = ( float ) ( rServices.m_pTerrain->getOceanMeshSize() );
			static_ocean.m_fVertexSpacing = ( float ) ( rServices.m_pTerrain->getOceanVertexSpacing() );
			static_ocean.m_fSubdiv = ( float ) ( 1 << rServices.m_pTerrain->getOceanTessellationLevel() );
			static_ocean.m_fCullMargin = ( float ) ( rServices.m_pTerrain->getOceanCullMargin() );
			const Ref< Terrain3DOceanSettings > ocean_settings = rServices.m_pTerrain->oceanSettingsResource();
			const t3dOceanSettingsState_t		defaults;
			const t3dOceanSettingsState_t &settings = ocean_settings.is_valid() ? ocean_settings->getState() : defaults;
			static_ocean.m_fSeaLevel = settings.m_fSeaLevel;
			memcpy( static_ocean.m_szWaterColor, settings.m_szWaterColor, sizeof( static_ocean.m_szWaterColor ) );
			static_ocean.m_fVisibleDepth = settings.m_fVisibleDepth;
			static_ocean.m_fDepthCurve = settings.m_fDepthCurve;
			static_ocean.m_fShoreBlend = settings.m_fShoreBlend;
			static_ocean.m_fRefractionStrength = settings.m_fRefractionStrength;
			static_ocean.m_fTimeScale = settings.m_fTimeScale;
			static_ocean.m_fHeightScale = settings.m_fHeightScale;
			static_ocean.m_fOceanScale = settings.m_fOceanScale;
			static_ocean.m_fFresnelScale = settings.m_fFresnelScale;
			static_ocean.m_fFresnelPower = settings.m_fFresnelPower;
			static_ocean.m_fSpecular = settings.m_fSpecular;
			static_ocean.m_fFoamHeight = settings.m_fFoamHeight;
			static_ocean.m_fFoamTopIntensity = settings.m_fFoamTopIntensity;
			static_ocean.m_fFoamDistance = settings.m_fFoamDistance;
			static_ocean.m_fFoamShoreIntensity = settings.m_fFoamShoreIntensity;
			static_ocean.m_fFoamNoiseScale = settings.m_fFoamNoiseScale;
			static_ocean.m_fHeightScattering = settings.m_fHeightScattering;
			static_ocean.m_fHeightThreshold = settings.m_fHeightThreshold;
			static_ocean.m_fHeightAngleThreshold = settings.m_fHeightAngleThreshold;
			static_ocean.m_fFresnelScattering = settings.m_fFresnelScattering;
			static_ocean.m_fDirectScattering = settings.m_fDirectScattering;
		}
		m_nOceanStaticSourceGeneration = ocean_source_generation;
		m_bOceanStaticStateInitialized = true;
		++m_nOceanStaticStateBuildCount;
	}
	t3dRdOceanState_t &ocean = snapshot.m_ocean;
	ocean = m_cachedOceanStaticState;
	if ( ocean.m_nEnabled != 0u ) {
		ocean.m_nPatchCount =
			( uint32_t ) ( rServices.m_pTerrain->getOceanMesher()->fillRdPatches( ocean.m_szPatches,
																				  T3D_MAX_CLIPMAP_LEVELS ) );
		ocean.m_fTimeSeconds =
			Time::get_singleton() ? ( float ) ( Time::get_singleton()->get_ticks_msec() * 0.001 ) : 0.0f;
		const Vector3 target = rServices.m_pTerrain->getClipmapTargetPosition();
		ocean.m_fTargetX = ( float ) ( target.x );
		ocean.m_fTargetY = ( float ) ( target.y );
		ocean.m_fTargetZ = ( float ) ( target.z );
		ocean.m_nGeneration = 0u;
		ocean.m_nGeneration = t3dHashPublicationBytes( &ocean, sizeof( ocean ) );
	}
	if ( m_bCachedOceanStripValid ) {
		if ( m_nCurrentOceanStripBank >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ||
			 m_oceanStripBanks[m_nCurrentOceanStripBank].m_nGeneration != m_cachedOceanStrip.m_nGeneration ) {
			const uint32_t pending_bank = pPending ? pPending->m_nOceanStripBank : T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
			const uint32_t bank =
				selectWritablePublicationBank( m_nOceanStripReaderBank.load( T3D_MEMORY_ORDER_ACQUIRE ), pending_bank );
			if ( bank >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ) {
				LOG( ERROR, "No writable RD ocean strip publication bank" );
				return false;
			}
			m_oceanStripBanks[bank].m_snapshot = m_cachedOceanStrip;
			m_oceanStripBanks[bank].m_nGeneration = m_cachedOceanStrip.m_nGeneration;
			m_nCurrentOceanStripBank = bank;
		}
		publication.m_nOceanStripBank = m_nCurrentOceanStripBank;
	}
	return true;
}

bool Terrain3DRenderPublicationBuilder::commitSuperTerrainCandidateCold(
    Terrain3DStPublicationImage &rImage, uint64_t nExpectedRegionTopology )
{
    if ( !rImage.valid() || nExpectedRegionTopology == 0u )
    {
        return false;
    }
    if ( m_stCommitted.valid() && m_stCommitted.signature() == rImage.signature() &&
         m_nStExpectedTopology == nExpectedRegionTopology )
    {
        return true;
    }
    m_stCommitted.swap( rImage );
    m_nStExpectedTopology = nExpectedRegionTopology;
    m_nStCommitEpoch = t3dNextGeneration( m_nStCommitEpoch );
    return true;
}

bool Terrain3DRenderPublicationBuilder::publishRegionState(
	const t3dPublicationServices_t		 &rServices,
	t3dRdFrameSnapshotSlot_t			 &publication,
	t3dRdFrameSnapshot_t				 &snapshot,
	const t3dRdFrameSnapshotSlot_t *const pPending ) {
	Terrain3DData *const		 data = rServices.m_pTerrain->dataObject();
	const Ref< Terrain3DAssets > assets = rServices.m_pTerrain->assetsResource();
	const uint64_t				 region_topology_generation = data->getRegionTopologyGenerationInternal();
	const uint64_t				 physical_region_map_generation = data->getPhysicalRegionMapGenerationInternal();
	const RID					 height_texture_rd = data->getHeightMapsRid();
	const bool					 height_texture_ready = height_texture_rd.is_valid();
	const int64_t				 region_map_count = T3D_REGION_SLOTS;
	const uint16_t *const		 pPhysicalRegionMap = data->getPhysicalRegionMapDataInternal();
	const uint16_t *const		 pLogicalRegionMap = data->getLogicalRegionMapDataInternal();
	if ( !pPhysicalRegionMap || !pLogicalRegionMap ) {
		LOG( ERROR, "Physical/logical region map storage unavailable" );
		return false;
	}
	const bool bTerrainMegaVTReady = rServices.m_terrainMegaVT.isReady();
	if ( bTerrainMegaVTReady &&
		 !rServices.m_terrainMegaVT.syncRegionPresenceHot( pLogicalRegionMap, region_topology_generation ) ) {
		LOG( ERROR, "Terrain MegaVT failed to synchronize stable region allocation identities" );
		return false;
	}
	if ( bTerrainMegaVTReady && region_topology_generation != m_nTerrainMegaVTPersistenceTopologyGeneration ) {
		rServices.m_host.importPublicationMegaVTPersistence();
		m_nTerrainMegaVTPersistenceTopologyGeneration = region_topology_generation;
	}
	const uint64_t nTerrainMegaVTPublicationGeneration = rServices.m_terrainMegaVT.getPublicationGeneration();
	const uint64_t region_lighting_source_generation = Terrain3DRegionLighting::getGlobalPublicationGeneration();
	const bool	   bRegionLightingSourceDirty = !m_bRegionMapInitialized ||
											region_lighting_source_generation != m_nRegionLightingSourceGeneration ||
											region_topology_generation != m_nRegionTopologyGeneration;
	uint64_t region_lighting_signature = m_nRegionLightingSignature;
	if ( bRegionLightingSourceDirty ) {
		region_lighting_signature = 1469598103934665603ull;
		for ( int64_t i = 0; i < region_map_count; ++i ) {
			const int source_index = Terrain3DData::getRegionMapIndex(
				Vector2i( ( int ) ( i & T3D_REGION_MAP_MASK ) - T3D_REGION_MAP_HALF,
						  ( int ) ( i >> T3D_REGION_MAP_SHIFT ) - T3D_REGION_MAP_HALF ) );
			if ( source_index < 0 || pLogicalRegionMap[source_index] == 0u ) {
				continue;
			}
			const Vector2i		   location( ( int ) ( i & T3D_REGION_MAP_MASK ) - T3D_REGION_MAP_HALF,
									 ( int ) ( i >> T3D_REGION_MAP_SHIFT ) - T3D_REGION_MAP_HALF );
			Terrain3DRegion *const pRegion = data->getRegionPtr( location );
			uint64_t			   value =
				( uint64_t ) ( ( uint32_t ) ( location.x ) ) | ( ( uint64_t ) ( ( uint32_t ) ( location.y ) ) << 32u );
			if ( pRegion && !pRegion->isDeleted() ) {
				const Ref< Terrain3DRegionLighting > lighting = pRegion->getLighting();
				if ( lighting.is_valid() ) {
					value ^= ( uint64_t ) ( CLAMP( lighting->getShadowmaskMode(), 0, 2 ) + 1 ) << 8u;
					value ^= ( uint64_t ) ( lighting->hasTerrainShadowMask() ? 1u : 0u ) << 16u;
					value ^= ( uint64_t ) ( lighting->getBakeSignature() );
				}
			}
			region_lighting_signature ^= value;
			region_lighting_signature *= 1099511628211ull;
		}
		m_nRegionLightingSourceGeneration = region_lighting_source_generation;
		++m_nRegionLightingScanCount;
	}
	const bool bRegionLightingChanged =
		!m_bRegionMapInitialized ||
		( bRegionLightingSourceDirty && region_lighting_signature != m_nRegionLightingSignature );
	if ( bRegionLightingChanged && !data->refreshTerrainShadowMasksInternal() ) {
		LOG( ERROR, "Unable to refresh Terrain3D regional shadowmask residency after lighting change" );
		return false;
	}
    const uint64_t vt_package_publication_generation = rServices.m_virtualTexture.getPackagePublicationGeneration();
	const bool	   region_map_changed =
		!m_bRegionMapInitialized ||
		physical_region_map_generation != m_nPhysicalRegionMapGeneration ||
		region_topology_generation != m_nRegionTopologyGeneration || bRegionLightingChanged ||
		height_texture_ready != m_bRegionMapHeightTextureReady || bTerrainMegaVTReady != m_bTerrainMegaVTReady ||
		nTerrainMegaVTPublicationGeneration != m_nTerrainMegaVTPublicationGeneration ||
        vt_package_publication_generation != m_nVTPackagePublicationGeneration ||
        m_nStCommitEpoch != m_nStRegionMapEpoch;
	if ( region_map_changed ) {
		const uint32_t pending_bank = pPending ? pPending->m_nRegionMapBank : T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
		const uint32_t bank =
			selectWritablePublicationBank( m_nRegionMapReaderBank.load( T3D_MEMORY_ORDER_ACQUIRE ), pending_bank );
		if ( bank >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ) {
			LOG( ERROR, "No writable RD region-map publication bank" );
			return false;
		}
		const int64_t required_bytes = region_map_count * ( int64_t ) ( sizeof( t3dRdTerrainRegionGpu_t ) );
		if ( required_bytes > m_regionMapBanks[bank].m_bytes.size() ) {
			LOG( ERROR, "RD region-map publication exceeded fixed bank capacity" );
			return false;
		}
		uint8_t *const pPackedRegionMap = m_regionMapBanks[bank].m_bytes.ptrw();
		for ( int64_t i = 0; i < region_map_count; ++i ) {
			const Vector2i map_location( ( int ) ( i & T3D_REGION_MAP_MASK ) - T3D_REGION_MAP_HALF,
										 ( int ) ( i >> T3D_REGION_MAP_SHIFT ) - T3D_REGION_MAP_HALF );
			const int	   source_index = Terrain3DData::getRegionMapIndex( map_location );
			// A physical layer is meaningful only while the corresponding Near height
			// array exists. During cache rebuild/startup publish zero residency so the
			// shader takes its Middle/Far/background path instead of indexing a missing
			// texture array. Logical presence remains published independently below.
			const uint32_t nPhysicalLayer =
				height_texture_ready && source_index >= 0 ? pPhysicalRegionMap[source_index] : 0u;
			const uint32_t nLogicalPresent =
				source_index >= 0 && pLogicalRegionMap[source_index] != 0u ? 0x80000000u : 0u;
			uint32_t nShadowmaskMode = 0u;
			if ( nLogicalPresent != 0u ) {
				const Vector2i		   location( ( int ) ( i & T3D_REGION_MAP_MASK ) - T3D_REGION_MAP_HALF,
										 ( int ) ( i >> T3D_REGION_MAP_SHIFT ) - T3D_REGION_MAP_HALF );
				Terrain3DRegion *const pRegion = data->getRegionPtr( location );
				if ( pRegion && !pRegion->isDeleted() ) {
					const Ref< Terrain3DRegionLighting > lighting = pRegion->getLighting();
					if ( lighting.is_valid() && lighting->hasTerrainShadowMask() ) {
						nShadowmaskMode = ( uint32_t ) ( CLAMP( lighting->getShadowmaskMode(), 0, 2 ) );
					}
				}
			}
			t3dRdTerrainRegionGpu_t gpu_region = {};
			// Physical-layer and shadowmask ownership remain in the existing record.
			// Retired bits 18..29 no longer encode a separate far-color GPU slot.
			gpu_region.m_nPackedPhysicalFlags =
				nPhysicalLayer | ( nShadowmaskMode << 16u ) | nLogicalPresent;
			gpu_region.m_nMegaVTAllocation = T3D_VT_INVALID_ALLOCATION;
            gpu_region.m_nVhmRootIndex = 0xffffffffu;
            gpu_region.m_nMaskRootIndex = 0xffffffffu;
            gpu_region.m_nComposedVTAllocation = T3D_VT_INVALID_ALLOCATION;
            gpu_region.m_nComposedVTGeneration = 0u;
            gpu_region.m_nRegionStateFlags = 0u; // Transitional FAR V1 shader alias.
			if ( nLogicalPresent != 0u && bTerrainMegaVTReady ) {
				const t3dTerrainMegaVTRegion_t *const pMegaRegion =
					rServices.m_terrainMegaVT.getRegion( ( uint32_t ) ( source_index ) );
				if ( pMegaRegion ) {
					gpu_region.m_nMegaVTAllocation = pMegaRegion->m_nAllocationId;
					gpu_region.m_nMegaVTGeneration = pMegaRegion->m_nGeneration;
					gpu_region.m_nMegaVTFlags = pMegaRegion->m_nFlags;
					memcpy( gpu_region.m_vMegaVTWorldToUv,
							pMegaRegion->m_vWorldToUv,
							sizeof( gpu_region.m_vMegaVTWorldToUv ) );
				}
			}
            if ( nLogicalPresent != 0u && source_index >= 0 &&
                 m_stCommitted.valid() && m_nStExpectedTopology == region_topology_generation )
            {
                const t3dStRegionRecord_t &stRegion = m_stCommitted.regions()[ (uint32_t) i ];
                uint32_t currentComposed = T3D_VT_INVALID_ALLOCATION;
                uint32_t currentGeneration = 0u;
                if ( t3dStRegionReady( &stRegion ) &&
                     rServices.m_virtualTexture.getComposedRegionAllocationCold(
                         (uint32_t) source_index, currentComposed, currentGeneration ) &&
                     currentComposed == stRegion.m_nComposedVTAllocation &&
                     currentGeneration == stRegion.m_nComposedVTGeneration )
                {
                    gpu_region.m_nComposedVTAllocation = currentComposed;
                    gpu_region.m_nComposedVTGeneration = currentGeneration;
                    gpu_region.m_nVhmRootIndex = stRegion.m_nVhmRootIndex;
                    gpu_region.m_nVhmGeneration = stRegion.m_nVhmGeneration;
                    gpu_region.m_nMaskRootIndex = stRegion.m_nMaskRootIndex;
                    gpu_region.m_nMaskGeneration = stRegion.m_nMaskGeneration;
                    gpu_region.m_nPublicationGeneration = stRegion.m_nPublicationGeneration;
                    gpu_region.m_nRegionStateFlags = stRegion.m_nRegionStateFlags;
                }
            }
			memcpy( pPackedRegionMap + i * ( int64_t ) ( sizeof( t3dRdTerrainRegionGpu_t ) ),
					&gpu_region,
					sizeof( gpu_region ) );
		}
		m_nRegionTopologyGeneration = region_topology_generation;
		m_nPhysicalRegionMapGeneration = physical_region_map_generation;
		m_nRegionLightingSignature = region_lighting_signature;
		m_bRegionMapHeightTextureReady = height_texture_ready;
		m_nTerrainMegaVTPublicationGeneration = nTerrainMegaVTPublicationGeneration;
        m_nVTPackagePublicationGeneration = vt_package_publication_generation;
        m_nStRegionMapEpoch = m_nStCommitEpoch;
		m_bTerrainMegaVTReady = bTerrainMegaVTReady;
		m_nRegionMapGeneration = t3dNextGeneration( m_nRegionMapGeneration );
		m_regionMapBanks[bank].m_nGeneration = m_nRegionMapGeneration;
		m_nCurrentRegionMapBank = bank;
		m_bRegionMapInitialized = true;
	}
	publication.m_nRegionMapBank = m_nCurrentRegionMapBank;
	snapshot.m_nRegionMapGeneration = m_nRegionMapGeneration;
	const uint64_t region_cache_sources[2] = { ( uint64_t ) ( data->getActiveStorageGeneration() ),
											   rServices.m_pTerrain->getRegionStreamingGeneration() };
	const uint64_t region_cache_source_generation =
		t3dHashPublicationBytes( region_cache_sources, sizeof( region_cache_sources ) );
	const bool bRegionCacheChanged =
		!m_bRegionCacheInitialized || region_cache_source_generation != m_nLastRegionCacheSourceGeneration;
	if ( bRegionCacheChanged ) {
		m_nRegionCacheGeneration = t3dNextGeneration( m_nRegionCacheGeneration );
		m_nLastRegionCacheSourceGeneration = region_cache_source_generation;
		m_bRegionCacheInitialized = true;
	}
	snapshot.m_nRegionCacheGeneration = m_nRegionCacheGeneration;

	t3dRdTerrainBindings_t &resources = publication.m_bindings;
	resources.m_hHeightTextureRd = height_texture_rd;
	resources.m_hDerivedTextureRd = data->getDerivedMapsRid();
	resources.m_hControlTextureRd = data->getControlMapsRid();
	resources.m_hColorTextureRd = data->getColorMapsRid();
	resources.m_hMiddleCompositeTextureRd = RID();
	resources.m_hTerrainShadowMaskTextureRd = data->getTerrainShadowMasksRid();
	resources.m_hMiddleHeightTextureRd = data->getMiddleHeightMapsRid();
	resources.m_hMiddleHeightRegionMapTextureRd = data->getMiddleHeightRegionMapRid();
	resources.m_hAlbedoTextureRd = assets.is_valid() ? assets->getAlbedoArrayRid() : RID();
	resources.m_hFarAlbedoTextureRd = assets.is_valid() ? assets->getFarAlbedoArrayRid() : RID();
	resources.m_hNormalTextureRd = assets.is_valid() ? assets->getNormalArrayRid() : RID();
	resources.m_hSurfaceTextureRd = assets.is_valid() ? assets->getSurfaceArrayRid() : RID();
	resources.m_hMaterialDisplacementTextureRd = assets.is_valid() ? assets->getDisplacementArrayRid() : RID();
	resources.m_hDisplacementTextureRd =
		snapshot.m_bDisplacementEnabled ? rServices.m_pTerrain->getComputeDisplacementTextureRid() : RID();
	resources.m_hWorldNoiseTextureRd = rServices.m_materialStateBuilder.getWorldNoiseRid();
	resources.m_hMacroNoiseTextureRd = rServices.m_materialStateBuilder.getMacroNoiseRid();
	resources.m_hEditorBrushTextureRd = rServices.m_materialStateBuilder.getEditorBrushRid();
	if ( !resources.m_hDisplacementTextureRd.is_valid() ) {
		snapshot.m_bDisplacementEnabled = false;
	}

	const bool bTerrainTexturesChanged =
		!m_bPublishedResourcesInitialized || bRegionCacheChanged ||
		resources.m_hHeightTextureRd != m_lastPublishedResources.m_hHeightTextureRd ||
		resources.m_hDerivedTextureRd != m_lastPublishedResources.m_hDerivedTextureRd ||
		resources.m_hControlTextureRd != m_lastPublishedResources.m_hControlTextureRd ||
		resources.m_hColorTextureRd != m_lastPublishedResources.m_hColorTextureRd ||
		resources.m_hMiddleCompositeTextureRd != m_lastPublishedResources.m_hMiddleCompositeTextureRd ||
		resources.m_hTerrainShadowMaskTextureRd != m_lastPublishedResources.m_hTerrainShadowMaskTextureRd ||
		resources.m_hMiddleHeightTextureRd != m_lastPublishedResources.m_hMiddleHeightTextureRd ||
		resources.m_hMiddleHeightRegionMapTextureRd != m_lastPublishedResources.m_hMiddleHeightRegionMapTextureRd;
	if ( bTerrainTexturesChanged ) {
		m_nTerrainTextureGeneration = t3dNextGeneration( m_nTerrainTextureGeneration );
	}
	const bool bMaterialTexturesChanged =
		!m_bPublishedResourcesInitialized ||
		resources.m_hAlbedoTextureRd != m_lastPublishedResources.m_hAlbedoTextureRd ||
		resources.m_hFarAlbedoTextureRd != m_lastPublishedResources.m_hFarAlbedoTextureRd ||
		resources.m_hNormalTextureRd != m_lastPublishedResources.m_hNormalTextureRd ||
		resources.m_hSurfaceTextureRd != m_lastPublishedResources.m_hSurfaceTextureRd ||
		resources.m_hMaterialDisplacementTextureRd != m_lastPublishedResources.m_hMaterialDisplacementTextureRd ||
		resources.m_hDisplacementTextureRd != m_lastPublishedResources.m_hDisplacementTextureRd ||
		resources.m_hWorldNoiseTextureRd != m_lastPublishedResources.m_hWorldNoiseTextureRd ||
		resources.m_hMacroNoiseTextureRd != m_lastPublishedResources.m_hMacroNoiseTextureRd ||
		resources.m_hEditorBrushTextureRd != m_lastPublishedResources.m_hEditorBrushTextureRd;
	if ( bMaterialTexturesChanged ) {
		m_nMaterialTextureGeneration = t3dNextGeneration( m_nMaterialTextureGeneration );
	}
	m_lastPublishedResources = resources;
	m_bPublishedResourcesInitialized = true;
	snapshot.m_nTerrainTextureGeneration = m_nTerrainTextureGeneration;
	snapshot.m_nMaterialTextureGeneration = m_nMaterialTextureGeneration;
	return true;
}

bool Terrain3DRenderPublicationBuilder::publishInstanceState(
	const t3dPublicationServices_t		 &rServices,
	t3dRdFrameSnapshotSlot_t			 &publication,
	const t3dRdFrameSnapshotSlot_t *const pPending ) {
	const Ref< Terrain3DAssets > instance_assets = rServices.m_pTerrain->assetsResource();
	Terrain3DHLODStreamer *const hlod_streamer =
		t3dRdGetInstanceHlodStreamer( rServices.m_pTerrain ? rServices.m_pTerrain : NULL );
	const uint64_t hlod_generation = hlod_streamer ? hlod_streamer->getRdPublicationGeneration() : 0u;
	const bool	   hlod_changed = !m_bCachedHlodInstanceSceneValid || hlod_generation != m_nHlodSceneGeneration;
	if ( hlod_changed ) {
		m_cachedHlodMeshSources.clear();
		if ( hlod_streamer ) {
			if ( !hlod_streamer->fillRdMeshSources( m_cachedHlodMeshSources ) ||
				 !hlod_streamer->buildRdInstanceSnapshot( m_cachedHlodInstanceScene ) ) {
				rServices.m_host.reportPublicationError( "Fixed RD HLOD publication capacity was exceeded" );
				return false;
			}
		} else {
			m_cachedHlodInstanceScene.m_instances.reset();
			m_cachedHlodInstanceScene.m_cells.reset();
			m_cachedHlodInstanceScene.m_dirtyRanges.reset();
			m_cachedHlodInstanceScene.m_dirtyCells.reset();
			m_cachedHlodInstanceScene.m_bInstanceLayoutChanged = true;
			m_cachedHlodInstanceScene.m_bTopologyChanged = true;
			m_cachedHlodInstanceScene.m_nGeneration = 0u;
		}
		m_bCachedHlodInstanceSceneValid = true;
		m_nHlodSceneGeneration = hlod_generation;
	}

	const uint64_t mesh_source_generations[2] = {
		instance_assets.is_valid() ? instance_assets->getMeshSourceGeneration() : 0u,
		hlod_generation };
	const uint64_t mesh_source_generation =
		t3dHashPublicationBytes( mesh_source_generations, sizeof( mesh_source_generations ) );
	if ( !m_bCachedInstanceMeshesValid || mesh_source_generation != m_nInstanceMeshSourceGeneration ) {
		Terrain3DColdPhaseScope cold_phase_scope;
		m_nInstanceMeshGeneration = t3dNextGeneration( m_nInstanceMeshGeneration );
		String	   mesh_error;
		const bool built = Terrain3DRdMeshCache::buildSourceSnapshot( instance_assets,
																	  m_cachedHlodMeshSources.data(),
																	  m_cachedHlodMeshSources.size(),
																	  m_cachedInstanceMeshes,
																	  mesh_source_generation,
																	  m_nInstanceMeshGeneration,
																	  &mesh_error );
		if ( !built ) {
			rServices.m_host.reportPublicationError( "Failed to publish RD instance/HLOD meshes: " + mesh_error );
			return false;
		}
		// Discovery is complete even if the publication bank still needs a cold
		// reserve. Preserve this immutable source image so the cold service can
		// size directly from it without rebuilding the mesh source a second time.
		m_bCachedInstanceMeshesValid = true;
		m_nInstanceMeshSourceGeneration = mesh_source_generation;
		++m_nMeshSourceBuildCount;
		if ( !hasInstanceMeshBankCapacity() ) {
			const size_t required =
				m_cachedInstanceMeshes.m_surfaces.size() + m_cachedInstanceMeshes.m_lods.size() +
				m_cachedInstanceMeshes.m_materials.size() + m_cachedInstanceMeshes.m_vtMaterials.size() +
				m_cachedInstanceMeshes.m_materialKeys.size() + m_cachedInstanceMeshes.m_vtStableKeys.size();
			const size_t capacity = m_instanceMeshBanks[0].m_snapshot.m_surfaces.capacity() +
									m_instanceMeshBanks[0].m_snapshot.m_lods.capacity() +
									m_instanceMeshBanks[0].m_snapshot.m_materials.capacity() +
									m_instanceMeshBanks[0].m_snapshot.m_vtMaterials.capacity() +
									m_instanceMeshBanks[0].m_snapshot.m_materialKeys.capacity() +
									m_instanceMeshBanks[0].m_snapshot.m_vtStableKeys.capacity();
			recordCapacityMiss( "RD instance mesh bank", required, capacity, true );
			return false;
		}
	}

	if ( m_meshSourceIndex.getSourceGeneration() != m_cachedInstanceMeshes.m_nGeneration &&
		 !m_meshSourceIndex.rebuildCold( m_cachedInstanceMeshes ) ) {
		rServices.m_host.reportPublicationError( "RD mesh source index has invalid spans or insufficient capacity" );
		return false;
	}

	Terrain3DInstancer *const instancer = rServices.m_pTerrain->instancerObject();
	const uint64_t			  instance_generation = instancer ? instancer->getRdPublicationGeneration() : 0u;
	const uint64_t			  occlusion_generation =
		   rServices.m_pTerrain ? rServices.m_pTerrain->instanceOcclusionPublicationGeneration() : 0u;
	const bool regular_changed = !m_bCachedRegularInstanceSceneValid ||
								 instance_generation != m_nInstanceSceneGeneration ||
								 occlusion_generation != m_nInstanceOcclusionGeneration;
	if ( regular_changed ) {
		if ( instancer ) {
			if ( !instancer->buildRdInstanceSnapshot( m_cachedRegularInstanceScene ) ) {
				rServices.m_host.reportPublicationError( "Fixed RD instance publication capacity was exceeded" );
				return false;
			}
		} else {
			m_cachedRegularInstanceScene.m_instances.reset();
			m_cachedRegularInstanceScene.m_cells.reset();
			m_cachedRegularInstanceScene.m_dirtyRanges.reset();
			m_cachedRegularInstanceScene.m_dirtyCells.reset();
			m_cachedRegularInstanceScene.m_bInstanceLayoutChanged = true;
			m_cachedRegularInstanceScene.m_bTopologyChanged = true;
			m_cachedRegularInstanceScene.m_nGeneration = 0u;
		}
		m_bCachedRegularInstanceSceneValid = true;
		m_nInstanceSceneGeneration = instance_generation;
		m_nInstanceOcclusionGeneration = occlusion_generation;
	}
	if ( !m_bCachedInstanceSceneValid || regular_changed || hlod_changed ) {
		t3dRdInstanceSceneSnapshot_t &combined = m_cachedInstanceScene;
		const size_t				  combined_instance_count =
			m_cachedRegularInstanceScene.m_instances.size() + m_cachedHlodInstanceScene.m_instances.size();
		const size_t combined_cell_count =
			m_cachedRegularInstanceScene.m_cells.size() + m_cachedHlodInstanceScene.m_cells.size();
		if ( combined_instance_count > combined.m_instances.capacity() ||
			 combined_cell_count > combined.m_cells.capacity() ) {
			rServices.m_host.reportPublicationError( "Fixed RD combined instance publication capacity was exceeded" );
			return false;
		}
		bool layout_changed =
			!m_bCachedInstanceSceneValid || hlod_changed || m_cachedRegularInstanceScene.m_bInstanceLayoutChanged;
		if ( combined.m_instances.size() != combined_instance_count ||
			 combined.m_cells.size() != combined_cell_count ) {
			layout_changed = true;
		}
		combined.m_dirtyRanges.reset();
		combined.m_dirtyCells.reset();
		combined.m_bInstanceLayoutChanged = layout_changed;
		combined.m_bTopologyChanged =
			!m_bCachedInstanceSceneValid || hlod_changed || m_cachedRegularInstanceScene.m_bTopologyChanged;
		m_nCombinedInstanceSceneGeneration = t3dNextGeneration( m_nCombinedInstanceSceneGeneration );
		combined.m_nGeneration = m_nCombinedInstanceSceneGeneration;

		if ( layout_changed ) {
			combined.m_instances.reset();
			if ( !combined.m_instances.append( m_cachedRegularInstanceScene.m_instances.data(),
											   m_cachedRegularInstanceScene.m_instances.size() ) ) {
				return false;
			}
			if ( !combined.m_instances.append( m_cachedHlodInstanceScene.m_instances.data(),
											   m_cachedHlodInstanceScene.m_instances.size() ) ) {
				return false;
			}
		} else {
			if ( !combined.m_instances.resizeUninitialized( combined_instance_count ) ) {
				return false;
			}
			for ( size_t iRange = 0u; iRange < m_cachedRegularInstanceScene.m_dirtyRanges.size(); ++iRange ) {
				const t3dRdInstanceDirtyRange_t &range = m_cachedRegularInstanceScene.m_dirtyRanges[iRange];
				if ( range.m_nInstanceCount == 0u ) {
					continue;
				}
				memcpy( combined.m_instances.data() + range.m_nFirstInstance,
						m_cachedRegularInstanceScene.m_instances.data() + range.m_nFirstInstance,
						( size_t ) ( range.m_nInstanceCount ) * sizeof( t3dRdInstanceRecord_t ) );
				if ( !combined.m_dirtyRanges.pushBack( range ) ) {
					return false;
				}
			}
		}

		combined.m_cells.reset();
		if ( !combined.m_cells.append( m_cachedRegularInstanceScene.m_cells.data(),
									   m_cachedRegularInstanceScene.m_cells.size() ) ) {
			return false;
		}
		if ( !layout_changed && !combined.m_dirtyCells.append( m_cachedRegularInstanceScene.m_dirtyCells.data(),
															   m_cachedRegularInstanceScene.m_dirtyCells.size() ) ) {
			return false;
		}
		{
			const uint32_t instance_offset = ( uint32_t ) ( m_cachedRegularInstanceScene.m_instances.size() );
			for ( size_t iCell = 0u; iCell < ( size_t ) ( ( m_cachedHlodInstanceScene.m_cells ).size() ); iCell++ ) {
				const t3dRdInstanceCell_t &source_cell = ( m_cachedHlodInstanceScene.m_cells )[iCell];
				t3dRdInstanceCell_t		   cell = source_cell;
				cell.m_nFirstInstance += instance_offset;
				if ( !combined.m_cells.pushBack( cell ) ) {
					return false;
				}
			}
		}
		m_bCachedInstanceSceneValid = true;
	}
	if ( m_bCachedInstanceMeshesValid ) {
		if ( m_nCurrentInstanceMeshBank >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ||
			 m_instanceMeshBanks[m_nCurrentInstanceMeshBank].m_nGeneration != m_cachedInstanceMeshes.m_nGeneration ) {
			const uint32_t pending_bank =
				pPending ? pPending->m_nInstanceMeshBank : T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
			const uint32_t bank =
				selectWritablePublicationBank( m_nInstanceMeshReaderBank.load( T3D_MEMORY_ORDER_ACQUIRE ),
											   pending_bank );
			if ( bank >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ) {
				LOG( ERROR, "No writable RD instance-mesh publication bank" );
				return false;
			}
			if ( !m_instanceMeshBanks[bank].m_snapshot.copyFrom( m_cachedInstanceMeshes ) ) {
				return false;
			}
			m_instanceMeshBanks[bank].m_nGeneration = m_cachedInstanceMeshes.m_nGeneration;
			m_nCurrentInstanceMeshBank = bank;
		}
		publication.m_nInstanceMeshBank = m_nCurrentInstanceMeshBank;
	}
	if ( m_bCachedInstanceSceneValid ) {
		if ( m_nCurrentInstanceSceneBank >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ||
			 m_instanceSceneBanks[m_nCurrentInstanceSceneBank].m_nGeneration != m_cachedInstanceScene.m_nGeneration ) {
			const uint32_t pending_bank =
				pPending ? pPending->m_nInstanceSceneBank : T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
			const uint32_t bank =
				selectWritablePublicationBank( m_nInstanceSceneReaderBank.load( T3D_MEMORY_ORDER_ACQUIRE ),
											   pending_bank );
			if ( bank >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ) {
				LOG( ERROR, "No writable RD instance-scene publication bank" );
				return false;
			}
			if ( !m_instanceSceneBanks[bank].m_snapshot.copyIncrementalFrom( m_cachedInstanceScene ) ) {
				return false;
			}
			m_instanceSceneBanks[bank].m_nGeneration = m_cachedInstanceScene.m_nGeneration;
			m_nCurrentInstanceSceneBank = bank;
		}
		publication.m_nInstanceSceneBank = m_nCurrentInstanceSceneBank;
	}
	return true;
}

bool Terrain3DRenderPublicationBuilder::publishWorldState(
	const t3dPublicationServices_t		 &rServices,
	t3dRdFrameSnapshotSlot_t			 &rPublication,
	const t3dRdFrameSnapshot_t			 &rSnapshot,
	const t3dRdFrameSnapshotSlot_t *const pPending ) {
	const Terrain3DClipmapSource *const pMesher = rServices.m_pTerrain ? rServices.m_pTerrain->getMesher() : NULL;
	if ( !pMesher || !m_bCachedInstanceMeshesValid || !m_bCachedInstanceSceneValid ) {
		return false;
	}

	const Terrain3DStaticSceneReadLease	   world_lease = rServices.m_staticScene.read();
	const t3dStaticScenePublication_t	  &static_pub = world_lease.publication();
	const Terrain3DWorldPublication *const pStaticWorld = static_pub.bValid ? static_pub.pWorld : NULL;

	// Fast retained-publication rejection before any capacity walk or instance scan.
	// Dynamic patch motion is intentionally absent from this signature.
	size_t terrain_strip_cluster_count = 0u;
	for ( uint32_t iPatch = 0u; iPatch < rSnapshot.m_nPatchCount; ++iPatch ) {
		const t3dMeshType_t type = rSnapshot.m_szPatches[iPatch].m_nLod == 0u ? LOD0_MESH : OUTER_RING_MESH;
		t3dWorldAccumulateCapacity( terrain_strip_cluster_count,
									t3dWorldTerrainStripClusterCountForMesh( *pMesher, type ) );
	}
	const uint64_t static_parts[5] = { pStaticWorld ? pStaticWorld->m_nSourceSignature : 0u,
									   static_pub.nGeneration,
									   static_pub.nCellCount,
									   static_pub.nInstanceCount,
									   static_pub.nMaterialCount };
	const uint64_t static_payload_signature = t3dHashPublicationBytes( static_parts, sizeof( static_parts ) );
	const uint64_t terrain_topology_words[5] = { rSnapshot.m_nMeshSize,
												 rSnapshot.m_nPatchCount,
												 ( uint64_t ) terrain_strip_cluster_count,
												 rSnapshot.m_nRenderLayers,
												 rSnapshot.m_bCastShadows ? 1u : 0u };
	const uint64_t terrain_topology_signature =
		t3dHashPublicationBytes( terrain_topology_words, sizeof( terrain_topology_words ) );
	const uint64_t signature_words[7] = { m_cachedInstanceMeshes.m_nGeneration,
										  m_cachedInstanceScene.m_nGeneration,
										  rSnapshot.m_nMaterialGeneration,
										  static_payload_signature,
										  terrain_topology_signature,
										  static_pub.nInstanceCount,
										  static_pub.nMaterialCount };
	const uint64_t source_signature = t3dHashPublicationBytes( signature_words, sizeof( signature_words ) );
	if ( m_nCurrentWorldPublicationBank < T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT &&
		 m_worldPublicationBanks[m_nCurrentWorldPublicationBank].m_publication.m_nSourceSignature ==
			 source_signature ) {
		rPublication.m_nWorldPublicationBank = m_nCurrentWorldPublicationBank;
		return true;
	}

	// Cold-only geometry admission. Existing source revisions keep stable WORLD
	// offsets even when an earlier HLOD source leaves the active scene.
	{
		Terrain3DColdPhaseScope cold_phase_scope;
		if ( !m_worldGeometryCache.synchronizeCold( m_cachedInstanceMeshes, pStaticWorld,
				world_lease.producerId(), rServices.m_pTerrain->getWorldResourceLimitsInternal() ) ) {
			rServices.m_host.reportPublicationError( "WORLD stable geometry cache admission failed" );
			return false;
		}
	}
	const Terrain3DWorldPublication &cached_geometry = m_worldGeometryCache.geometry();
	t3dWorldPublicationCapacity_t required{};
	required.m_nVertices = cached_geometry.m_vertices.size();
	required.m_nClusterVertexIndices = cached_geometry.m_clusterVertexIndices.size();
	required.m_nTriangleWords = cached_geometry.m_triangleWords.size();
	required.m_nClusters = cached_geometry.m_clusters.size();
	required.m_nClusterVtMetadata = cached_geometry.m_clusterVtMetadata.size();
	required.m_nMaterials = cached_geometry.m_materials.size();
	required.m_nInstances = m_cachedInstanceScene.m_instances.size();
	required.m_nCells = m_cachedInstanceScene.m_cells.size();
	required.m_nTransforms = required.m_nInstances;
	required.m_nHlodRecords = m_cachedHlodInstanceScene.m_cells.size();
	required.m_nTerrainStripClusters = terrain_strip_cluster_count;
	required.m_nVisibilityCandidates = required.m_nTerrainStripClusters;
	for ( size_t i = 0u; i < m_cachedInstanceScene.m_cells.size(); ++i ) {
		uint32_t cluster_offset = 0u, cluster_count = 0u;
		if ( m_worldGeometryCache.findActiveSpan(
										  m_cachedInstanceScene.m_cells[i].m_nMeshSourceKey,
										  cluster_offset,
										  cluster_count ) ) {
			t3dWorldAccumulateCapacity( required.m_nVisibilityCandidates,
										( size_t ) m_cachedInstanceScene.m_cells[i].m_nInstanceCount *
											( size_t ) cluster_count );
		}
	}
	if ( pStaticWorld ) {
		t3dWorldAccumulateCapacity( required.m_nTransforms, pStaticWorld->m_transforms.size() );
		t3dWorldAccumulateCapacity( required.m_nInstances, pStaticWorld->m_instances.size() );
		t3dWorldAccumulateCapacity( required.m_nCells, pStaticWorld->m_cells.size() );
		t3dWorldAccumulateCapacity( required.m_nHlodRecords, pStaticWorld->m_hlodRecords.size() );
		for ( size_t i = 0u; i < pStaticWorld->m_instances.size(); ++i ) {
			t3dWorldAccumulateCapacity( required.m_nVisibilityCandidates,
										pStaticWorld->m_instances[i].m_nClusterCount );
		}
	}

	if ( !hasWorldPublicationCapacity( required ) ) {
		requestWorldPublicationCapacity( required );
		return false;
	}

	const Terrain3DWorldPublication *const pPreviousWorld =
		m_nCurrentWorldPublicationBank < T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT
			? &m_worldPublicationBanks[m_nCurrentWorldPublicationBank].m_publication
			: NULL;
	const bool stable_instance_layout = pPreviousWorld && !m_cachedInstanceScene.m_bInstanceLayoutChanged;

	const uint32_t pending_bank = pPending ? pPending->m_nWorldPublicationBank : T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	const uint32_t bank_index =
		selectWritablePublicationBank( m_nWorldPublicationReaderBank.load( T3D_MEMORY_ORDER_ACQUIRE ), pending_bank );
	if ( bank_index >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ) {
		return false;
	}
	Terrain3DWorldPublication &world = m_worldPublicationBanks[bank_index].m_publication;
	const uint64_t mesh_payload_parts[3] = { m_worldGeometryCache.getCacheEpoch(),
													m_worldGeometryCache.getGeometryRevision(),
													m_worldGeometryCache.getMaterialRevision() };
	const uint64_t mesh_payload_signature = t3dHashPublicationBytes( mesh_payload_parts, sizeof( mesh_payload_parts ) );
	const bool reuse_mesh_payload = world.m_nMeshPayloadSignature == mesh_payload_signature && world.m_nGeneration != 0u;
	world.clearForBuild( true );
	if ( reuse_mesh_payload ) {
		++m_nWorldMeshPayloadReuseCount;
	}
	if ( !m_worldGeometryCache.copyGeometryInto( world ) ) {
		return false;
	}
	world.m_nSourceSignature = source_signature;
	world.m_materials.insert( world.m_materials.end(), cached_geometry.m_materials.begin(),
										 cached_geometry.m_materials.end() );
	world.m_instanceMaterialStableKeys.insert( world.m_instanceMaterialStableKeys.end(),
		cached_geometry.m_instanceMaterialStableKeys.begin(), cached_geometry.m_instanceMaterialStableKeys.end() );
	for ( size_t i = 0u; i < m_cachedInstanceMeshes.m_lods.size(); ++i ) {
		uint32_t offset = 0u, count = 0u;
		if ( !m_worldGeometryCache.findActiveSpan( m_cachedInstanceMeshes.m_lods[i].m_nSourceKey, offset, count ) ) {
			continue;
		}
		if ( ( m_cachedInstanceMeshes.m_lods[i].m_nSourceKey >> 60u ) == 2u ) {
			world.m_nHlodClusterCount += count;
		} else {
			world.m_nInstanceClusterCount += count;
		}
	}

	for ( size_t iCell = 0u; iCell < m_cachedInstanceScene.m_cells.size(); ++iCell ) {
		const t3dRdInstanceCell_t &source_cell = m_cachedInstanceScene.m_cells[iCell];
		uint32_t				   cluster_offset = 0u, cluster_count = 0u;
		if ( !m_worldGeometryCache.findActiveSpan(
										   source_cell.m_nMeshSourceKey,
										   cluster_offset,
										   cluster_count ) ) {
			continue;
		}
		const uint32_t first_instance = ( uint32_t ) world.m_instances.size();
		const uint32_t world_cell_id = ( uint32_t ) world.m_cells.size();
		for ( uint32_t local = 0u; local < source_cell.m_nInstanceCount; ++local ) {
			const uint32_t source_index = source_cell.m_nFirstInstance + local;
			if ( source_index >= m_cachedInstanceScene.m_instances.size() ) {
				break;
			}
			const t3dWorldTransform_t transform =
				t3dWorldDecodeInstanceTransform( m_cachedInstanceScene.m_instances[source_index] );
			const uint32_t		transform_index = ( uint32_t ) world.m_transforms.size();
			t3dWorldTransform_t previous_transform = transform;
			const uint32_t		previous_instance_index = ( uint32_t ) world.m_instances.size();
			if ( stable_instance_layout && bank_index != m_nCurrentWorldPublicationBank &&
				 previous_instance_index < pPreviousWorld->m_instances.size() ) {
				const t3dWorldInstance_t &previous_instance = pPreviousWorld->m_instances[previous_instance_index];
				if ( previous_instance.m_nTransformIndex < pPreviousWorld->m_transforms.size() &&
					 previous_instance.m_nGeometryKind == ( ( source_cell.m_nMeshSourceKey >> 60u ) == 2u
																? T3D_WORLD_GEOMETRY_HLOD_CLUSTER
																: T3D_WORLD_GEOMETRY_MESH_CLUSTER ) ) {
					previous_transform = pPreviousWorld->m_transforms[previous_instance.m_nTransformIndex];
				}
			}
			world.m_transforms.push_back( transform );
			world.m_previousTransforms.push_back( previous_transform );
			t3dWorldInstance_t instance{};
			instance.m_nClusterOffset = cluster_offset;
			instance.m_nClusterCount = cluster_count;
			instance.m_nTransformIndex = transform_index;
			instance.m_nPreviousTransformIndex = transform_index;
			instance.m_nVisibilityLayers = source_cell.m_nVisibilityLayers;
			instance.m_nFlags = source_cell.m_nFlags;
			instance.m_nGeometryKind = ( source_cell.m_nMeshSourceKey >> 60u ) == 2u ? T3D_WORLD_GEOMETRY_HLOD_CLUSTER
																					 : T3D_WORLD_GEOMETRY_MESH_CLUSTER;
			instance.m_nSourceId = t3dWorldSourceId( source_cell.m_nMeshSourceKey );
			instance.m_nMaterialOverride = T3D_WORLD_INVALID_ID;
			instance.m_nCellId = world_cell_id;
			instance.m_fLodBegin = source_cell.m_fLodBegin;
			instance.m_fLodEnd = source_cell.m_fLodEnd;
			instance.m_fFadeMargin = source_cell.m_fFadeMargin;
			world.m_instances.push_back( instance );
		}
		const Vector3  center = source_cell.m_bounds.get_center();
		const Vector3  extent = source_cell.m_bounds.size * 0.5f;
		t3dWorldCell_t cell{};
		cell.m_fCenter[0] = ( float ) center.x;
		cell.m_fCenter[1] = ( float ) center.y;
		cell.m_fCenter[2] = ( float ) center.z;
		cell.m_fRadius = ( float ) extent.length();
		cell.m_nFirstInstance = first_instance;
		cell.m_nInstanceCount = ( uint32_t ) world.m_instances.size() - first_instance;
		cell.m_nVisibilityLayers = source_cell.m_nVisibilityLayers;
		cell.m_nFlags = source_cell.m_nFlags;
		world.m_cells.push_back( cell );
		if ( ( source_cell.m_nMeshSourceKey >> 60u ) == 2u ) {
			t3dWorldHlodRecord_t hlod{};
			hlod.m_nFirstInstance = first_instance;
			hlod.m_nInstanceCount = cell.m_nInstanceCount;
			hlod.m_nClusterOffset = cluster_offset;
			hlod.m_nClusterCount = cluster_count;
			hlod.m_nProxySourceId = t3dWorldSourceId( source_cell.m_nMeshSourceKey );
			hlod.m_nFlags = source_cell.m_nFlags;
			world.m_hlodRecords.push_back( hlod );
		}
	}

	if ( pStaticWorld ) {
		uint32_t cluster_base = 0u, static_cluster_count = 0u;
		if ( !pStaticWorld->m_clusters.empty() &&
			 !m_worldGeometryCache.getStaticSpan( cluster_base, static_cluster_count ) ) {
			return false;
		}
		const uint32_t transform_base = ( uint32_t ) world.m_transforms.size();
		const uint32_t instance_base = ( uint32_t ) world.m_instances.size();
		const uint32_t cell_base = ( uint32_t ) world.m_cells.size();

		world.m_transforms.insert( world.m_transforms.end(),
								   pStaticWorld->m_transforms.begin(),
								   pStaticWorld->m_transforms.end() );
		world.m_previousTransforms.insert( world.m_previousTransforms.end(),
										   pStaticWorld->m_previousTransforms.begin(),
										   pStaticWorld->m_previousTransforms.end() );
		for ( size_t i = 0u; i < pStaticWorld->m_instances.size(); ++i ) {
			t3dWorldInstance_t instance = pStaticWorld->m_instances[i];
			instance.m_nClusterOffset += cluster_base;
			instance.m_nTransformIndex += transform_base;
			instance.m_nPreviousTransformIndex += transform_base;
			if ( instance.m_nMaterialOverride != T3D_WORLD_INVALID_ID &&
				 !m_worldGeometryCache.getStaticMaterialSlot( instance.m_nMaterialOverride,
														instance.m_nMaterialOverride ) ) {
				return false;
			}
			if ( instance.m_nCellId != T3D_WORLD_INVALID_ID )
				instance.m_nCellId += cell_base;
			world.m_instances.push_back( instance );
		}
		for ( size_t i = 0u; i < pStaticWorld->m_cells.size(); ++i ) {
			t3dWorldCell_t cell = pStaticWorld->m_cells[i];
			cell.m_nFirstInstance += instance_base;
			world.m_cells.push_back( cell );
		}
		for ( size_t i = 0u; i < pStaticWorld->m_hlodRecords.size(); ++i ) {
			t3dWorldHlodRecord_t hlod = pStaticWorld->m_hlodRecords[i];
			hlod.m_nFirstInstance += instance_base;
			hlod.m_nClusterOffset += cluster_base;
			world.m_hlodRecords.push_back( hlod );
		}
		world.m_nStaticClusterCount += pStaticWorld->m_nStaticClusterCount;
	}

	for ( uint32_t iPatch = 0u; iPatch < rSnapshot.m_nPatchCount; ++iPatch ) {
		const t3dRdPatch_t &patch = rSnapshot.m_szPatches[iPatch];
		if ( !t3dWorldAppendTerrainStripClusters( world, *pMesher, patch, iPatch ) ) {
			return false;
		}
	}

	// P09: immutable candidate stream. This is publication-time expansion only;
	// no per-frame CPU visible-object loop survives into unified visibility.
	for ( uint32_t iInstance = 0u; iInstance < ( uint32_t ) world.m_instances.size(); ++iInstance ) {
		const t3dWorldInstance_t &instance = world.m_instances[iInstance];
		for ( uint32_t local = 0u; local < instance.m_nClusterCount; ++local ) {
			const uint32_t cluster_id = instance.m_nClusterOffset + local;
			if ( cluster_id >= world.clusters().size() ) {
				break;
			}
			t3dWorldVisibleItem_t candidate{};
			candidate.m_nClusterOrTerrainId = cluster_id;
			candidate.m_nInstanceOrPatchId = iInstance;
			candidate.m_nGeometryKind = instance.m_nGeometryKind;
			candidate.m_nFlags = instance.m_nFlags;
			world.m_visibilityCandidates.push_back( candidate );
		}
	}
	for ( uint32_t iTerrain = 0u; iTerrain < ( uint32_t ) world.m_terrainStripClusters.size(); ++iTerrain ) {
		t3dWorldVisibleItem_t candidate{};
		candidate.m_nClusterOrTerrainId = iTerrain;
		candidate.m_nInstanceOrPatchId =
			world.m_terrainStripClusters[iTerrain].m_nPatchId | ( rSnapshot.m_bCastShadows ? 0x80000000u : 0u );
		candidate.m_nGeometryKind = T3D_WORLD_GEOMETRY_TERRAIN;
		candidate.m_nFlags = rSnapshot.m_nRenderLayers;
		world.m_visibilityCandidates.push_back( candidate );
	}

	m_nWorldPublicationGeneration = t3dNextGeneration( m_nWorldPublicationGeneration );
	m_nWorldGeometryGeneration = m_worldGeometryCache.getGeometryRevision();
	m_nWorldMaterialGeneration = m_worldGeometryCache.getMaterialRevision();
	world.m_nGeneration = m_nWorldPublicationGeneration;
	m_worldGeometryCache.tagRetiredSourcesCold( world.m_nGeneration );
	world.m_nGeometryGeneration = m_nWorldGeometryGeneration;
	world.m_nMaterialGeneration = m_nWorldMaterialGeneration;
	world.m_nInstanceGeneration = m_cachedInstanceScene.m_nGeneration;
	world.m_nHlodSourceGeneration = m_nHlodSceneGeneration;
	world.m_nTerrainGeneration = terrain_topology_signature;
	world.m_nStaticGeneration = static_payload_signature;
	world.m_nMeshPayloadSignature = mesh_payload_signature;
	world.m_nStaticSceneProducerId = pStaticWorld ? world_lease.producerId() : 0u;
	world.m_nStaticScenePublicationGeneration = pStaticWorld ? static_pub.nGeneration : 0u;
	m_worldPublicationBanks[bank_index].m_nGeneration = world.m_nGeneration;
	m_nCurrentWorldPublicationBank = bank_index;
	rPublication.m_nWorldPublicationBank = bank_index;
	return true;
}

namespace
{
    static int32_t t3dEnvironmentRegionSlot( const Vector2i &rRegion ) {
        const int32_t x = rRegion.x + T3D_REGION_MAP_HALF;
        const int32_t z = rRegion.y + T3D_REGION_MAP_HALF;
        if ( x < 0 || z < 0 || x >= T3D_REGION_MAP_SIZE || z >= T3D_REGION_MAP_SIZE ) {
            return -1;
        }
        return z * T3D_REGION_MAP_SIZE + x;
    }

    static uint16_t t3dEnvironmentQuantize( float fValue ) {
        return ( uint16_t ) ( CLAMP( fValue, 0.0f, 1.0f ) * 65535.0f + 0.5f );
    }

    static uint32_t t3dEnvironmentChecksum( const uint8_t *pBytes, uint32_t nBytes ) {
        uint32_t result = 2166136261u;
        for ( uint32_t i = 0u; i < nBytes; ++i ) {
            result = ( result ^ pBytes[i] ) * 16777619u;
        }
        return result;
    }

    static void t3dEnvironmentPutU32( uint8_t *pOut, uint32_t nValue ) {
        for ( uint32_t i = 0u; i < 4u; ++i ) {
            pOut[i] = ( uint8_t ) ( nValue >> ( 8u * i ) );
        }
    }

    static uint32_t t3dEnvironmentGetU32( const uint8_t *pIn ) {
        return ( uint32_t ) pIn[0] | ( ( uint32_t ) pIn[1] << 8u ) |
               ( ( uint32_t ) pIn[2] << 16u ) | ( ( uint32_t ) pIn[3] << 24u );
    }
}

void Terrain3DRenderPublicationBuilder::clearEnvironmentRegionClimateOwner() {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnOwnerThread(), VOID );
#endif
    memset( m_szEnvironmentRegionClimate, 0, sizeof( m_szEnvironmentRegionClimate ) );
    m_nEnvironmentPendingCount = 0u;
    ++m_nEnvironmentRegionClimateEpoch;
    ++m_nEnvironmentRegionOverrideEpoch;
}

bool Terrain3DRenderPublicationBuilder::setEnvironmentRegionClimateOwner(
    const Vector2i &rRegion, const Vector3 &rClimate ) {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnOwnerThread(), false );
#endif
    const int32_t slot = t3dEnvironmentRegionSlot( rRegion );
    if ( slot < 0 || !isfinite( ( double ) rClimate.x ) || !isfinite( ( double ) rClimate.y ) ||
         !isfinite( ( double ) rClimate.z ) ) {
        return false;
    }
    t3dEnvironmentRegionClimate_t &cell = m_szEnvironmentRegionClimate[slot];
    const t3dEnvironmentRegionClimate_t next{
        t3dEnvironmentQuantize( ( float ) rClimate.x ),
        t3dEnvironmentQuantize( ( float ) rClimate.y ),
        t3dEnvironmentQuantize( ( float ) rClimate.z ), 1u };
    if ( memcmp( &cell, &next, sizeof( next ) ) != 0 ) {
        cell = next;
        ++m_nEnvironmentRegionClimateEpoch;
        ++m_nEnvironmentRegionOverrideEpoch;
    }
    return true;
}

Vector3 Terrain3DRenderPublicationBuilder::getEnvironmentRegionClimateOwner( const Vector2i &rRegion ) const {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnOwnerThread(), Vector3() );
#endif
    const int32_t slot = t3dEnvironmentRegionSlot( rRegion );
    if ( slot < 0 || m_szEnvironmentRegionClimate[slot].m_nFlags == 0u ) {
        return Vector3();
    }
    const t3dEnvironmentRegionClimate_t &cell = m_szEnvironmentRegionClimate[slot];
    return Vector3( ( real_t ) cell.m_nWetness / 65535.0f,
                    ( real_t ) cell.m_nSnow / 65535.0f,
                    ( real_t ) cell.m_nGroundCover / 65535.0f );
}

PackedByteArray Terrain3DRenderPublicationBuilder::exportEnvironmentRegionClimateOwner() const {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnOwnerThread(), PackedByteArray() );
#endif
    const uint32_t header = 16u;
    const uint32_t payload = T3D_REGION_SLOTS * 8u;
    PackedByteArray bytes;
    bytes.resize( header + payload );
    uint8_t *const pOut = bytes.ptrw();
    pOut[0] = 'T'; pOut[1] = '3'; pOut[2] = 'E'; pOut[3] = '7';
    t3dEnvironmentPutU32( pOut + 4, 1u );
    t3dEnvironmentPutU32( pOut + 8, T3D_REGION_SLOTS );
    for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) {
        const t3dEnvironmentRegionClimate_t &cell = m_szEnvironmentRegionClimate[i];
        const uint16_t channels[4] = { cell.m_nWetness, cell.m_nSnow, cell.m_nGroundCover, cell.m_nFlags };
        for ( uint32_t c = 0u; c < 4u; ++c ) {
            const uint32_t offset = header + i * 8u + c * 2u;
            pOut[offset] = ( uint8_t ) channels[c];
            pOut[offset + 1u] = ( uint8_t ) ( channels[c] >> 8u );
        }
    }
    t3dEnvironmentPutU32( pOut + 12, t3dEnvironmentChecksum( pOut + header, payload ) );
    return bytes;
}

bool Terrain3DRenderPublicationBuilder::importEnvironmentRegionClimateOwner( const PackedByteArray &rData ) {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnOwnerThread(), false );
#endif
    const uint32_t header = 16u;
    const uint32_t payload = T3D_REGION_SLOTS * 8u;
    if ( rData.size() != header + payload ) {
        return false;
    }
    const uint8_t *const pIn = rData.ptr();
    if ( pIn[0] != 'T' || pIn[1] != '3' || pIn[2] != 'E' || pIn[3] != '7' ||
         t3dEnvironmentGetU32( pIn + 4 ) != 1u ||
         t3dEnvironmentGetU32( pIn + 8 ) != T3D_REGION_SLOTS ||
         t3dEnvironmentGetU32( pIn + 12 ) != t3dEnvironmentChecksum( pIn + header, payload ) ) {
        return false;
    }
    t3dEnvironmentRegionClimate_t restored[T3D_REGION_SLOTS]{};
    for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) {
        uint16_t channels[4]{};
        for ( uint32_t c = 0u; c < 4u; ++c ) {
            const uint32_t offset = header + i * 8u + c * 2u;
            channels[c] = ( uint16_t ) ( pIn[offset] | ( ( uint16_t ) pIn[offset + 1u] << 8u ) );
        }
        if ( channels[3] > 1u ) {
            return false;
        }
        restored[i] = { channels[0], channels[1], channels[2], channels[3] };
    }
    memcpy( m_szEnvironmentRegionClimate, restored, sizeof( restored ) );
    ++m_nEnvironmentRegionClimateEpoch;
    ++m_nEnvironmentRegionOverrideEpoch;
    return true;
}

void Terrain3DRenderPublicationBuilder::setEnvironmentForcingOwner(
    const t3dEnvironmentForcing_t &rForcing ) {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnOwnerThread(), VOID );
#endif
    m_environmentForcing = rForcing;
    if ( ( rForcing.m_nFlags & 1u ) == 0u || rForcing.m_fDeltaTime <= 0.0f ) {
        return;
    }
    const float dt = CLAMP( rForcing.m_fDeltaTime, 0.0f, 0.1f );
    bool dirty = false;
    for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) {
        t3dEnvironmentRegionClimate_t &cell = m_szEnvironmentRegionClimate[i];
        const t3dEnvironmentRegionClimate_t before = cell;
        cell.m_nWetness = t3dEnvironmentQuantize(
            ( float ) cell.m_nWetness / 65535.0f + dt * ( rForcing.m_fRainRate - rForcing.m_fDryRate ) );
        cell.m_nSnow = t3dEnvironmentQuantize(
            ( float ) cell.m_nSnow / 65535.0f + dt * ( rForcing.m_fSnowRate - rForcing.m_fDryRate * 0.3f ) );
        cell.m_nGroundCover = t3dEnvironmentQuantize(
            ( float ) cell.m_nGroundCover / 65535.0f - dt * 0.15f );
        cell.m_nFlags = 1u;
        dirty |= memcmp( &cell, &before, sizeof( cell ) ) != 0;
    }
    if ( dirty ) {
        ++m_nEnvironmentRegionClimateEpoch;
    }
}

uint32_t Terrain3DRenderPublicationBuilder::submitEnvironmentImpulsesOwner(
    const t3dEnvironmentImpulse_t *pEvents, const uint32_t nCount ) {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnOwnerThread(), 0u );
#endif
    if ( !pEvents || nCount == 0u ) {
        return 0u;
    }
    const uint32_t remaining = T3D_ENVIRONMENT_MAX_IMPULSES - m_nEnvironmentPendingCount;
    const uint32_t accepted = MIN( nCount, remaining );
    if ( accepted > 0u ) {
        memcpy( &m_szEnvironmentPending[m_nEnvironmentPendingCount], pEvents,
                accepted * sizeof( t3dEnvironmentImpulse_t ) );
        m_nEnvironmentPendingCount += accepted;
    }
    m_nEnvironmentImpulseDropped += nCount - accepted;
    return accepted;
}

void Terrain3DRenderPublicationBuilder::commitSnapshotPublication(
	const t3dPublicationServices_t &rServices,
	t3dRdFrameSnapshot_t		   &snapshot ) {
	uint64_t publication_generation = m_nNextSnapshotGeneration;
	if ( publication_generation == 0u || publication_generation > T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX ) {
		publication_generation = 1u;
	}
	m_nNextSnapshotGeneration =
		publication_generation == T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX ? 1u : publication_generation + 1u;
	snapshot.m_nPublicationGeneration = publication_generation;
	const uint64_t published_token = t3dRdMakeFrameSnapshotToken( m_nSnapshotWriteIndex, publication_generation, true );
	const uint64_t reclaimed_token = m_nSnapshotExchangeToken.exchange( published_token, T3D_MEMORY_ORDER_ACQ_REL );
	const uint32_t reclaimed_index = t3dRdFrameSnapshotTokenIndex( reclaimed_token );
	m_nPublishedSnapshotIndex.store( m_nSnapshotWriteIndex, T3D_MEMORY_ORDER_RELEASE );
	m_nPublishedSnapshotGeneration.store( publication_generation, T3D_MEMORY_ORDER_RELEASE );
	m_nSnapshotPublications.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	if ( reclaimed_index >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ) {
		rServices.m_host.failPublicationRenderer();
		return;
	}
	m_nSnapshotWriteIndex = reclaimed_index;
}

void Terrain3DRenderPublicationBuilder::publish(
	const t3dPublicationServices_t &rServices ) {
#ifdef DEBUG_ENABLED
	// RenderingServer::is_on_render_thread() can also be true on Godot's main
	// thread when the renderer is configured single-threaded. Publication is an
	// owner-thread operation, so assert the owner identity directly.
	ASSERT( Terrain3DRdContext::isOnOwnerThread(), VOID );
#endif
	if ( !rServices.m_pTerrain || !rServices.m_pTerrain->getMesher() || !rServices.m_pTerrain->dataObject() ) {
		return;
	}
	if ( m_nSnapshotWriteIndex >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ) {
		rServices.m_host.failPublicationRenderer();
		return;
	}
	if ( !hasInstancePublicationCapacity( rServices ) ) {
		Terrain3DInstancer *const	 instancer = rServices.m_pTerrain->instancerObject();
		Terrain3DHLODStreamer *const hlod_streamer =
			t3dRdGetInstanceHlodStreamer( rServices.m_pTerrain ? rServices.m_pTerrain : NULL );
		const size_t required = ( instancer ? instancer->getRdSnapshotInstanceCapacity() : 0u ) +
								( hlod_streamer ? hlod_streamer->getRdPublicationInstanceCapacity() : 0u );
		recordCapacityMiss( "RD instance publication", required, m_cachedInstanceScene.m_instances.capacity(), false );
		return;
	}
	// The exchange still owns frame slots exactly as before. Large immutable
	// payloads are addressed through fixed generation banks instead of cloned
	// into every reclaimed frame slot.
	t3dRdFrameSnapshotSlot_t &publication = m_snapshotSlots[m_nSnapshotWriteIndex];
	publication = t3dRdFrameSnapshotSlot_t{};
	t3dRdFrameSnapshot_t				 &snapshot = publication.m_snapshot;
	const uint64_t						  exchange_token = m_nSnapshotExchangeToken.load( T3D_MEMORY_ORDER_ACQUIRE );
	const uint32_t						  pending_slot = t3dRdFrameSnapshotTokenIsPublished( exchange_token )
															 ? t3dRdFrameSnapshotTokenIndex( exchange_token )
															 : T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	const t3dRdFrameSnapshotSlot_t *const pPending =
		pending_slot < T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ? &m_snapshotSlots[pending_slot] : NULL;
	publishPatchState( rServices, snapshot );
	if ( !rServices.m_materialStateBuilder.publishSnapshotState( rServices.m_materialSources, snapshot ) ) {
		return;
	}
	if ( snapshot.m_nTerrainMaterialBackend != T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED ) {
		rServices.m_host.reportPublicationError(
			String( "Retired terrain material backend rejected before publication: backend=" ) +
			String::num_uint64( snapshot.m_nTerrainMaterialBackend ) );
		return;
	}
	publishLightingState( rServices, snapshot );
	if ( !publishOceanState( rServices, publication, snapshot, pPending ) ) {
		return;
	}
	if ( !publishRegionState( rServices, publication, snapshot, pPending ) ) {
		return;
	}
	if ( !publishInstanceState( rServices, publication, pPending ) ) {
		return;
	}
	if ( !publishWorldState( rServices, publication, snapshot, pPending ) ) {
		return;
	}
	// Capture the complete generation tuple inside the same owner-side publication
	// transaction. The render thread must never re-read mutable producer counters.
	snapshot.m_generations.m_nRegionTopologyGeneration = m_nRegionTopologyGeneration;
	snapshot.m_generations.m_nPhysicalRegionMapGeneration = m_nPhysicalRegionMapGeneration;
	snapshot.m_generations.m_nPatchGeneration = m_nPatchGeneration;
	snapshot.m_generations.m_nMaterialGeneration = snapshot.m_nMaterialGeneration;
	snapshot.m_generations.m_nRegionLightingSignature = m_nRegionLightingSignature;
    // The cache is authoritative owner data. The render thread receives a
    // complete packed snapshot; no GPU readback or cooked region mutation.
    snapshot.m_nEnvironmentRegionClimateEpoch = m_nEnvironmentRegionClimateEpoch;
    snapshot.m_nEnvironmentRegionOverrideEpoch = m_nEnvironmentRegionOverrideEpoch;
    for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) {
        const t3dEnvironmentRegionClimate_t &cell = m_szEnvironmentRegionClimate[i];
        snapshot.m_szEnvironmentRegionClimate[i] =
            ( uint32_t ) ( cell.m_nWetness >> 8u ) |
            ( ( uint32_t ) ( cell.m_nSnow >> 8u ) << 8u ) |
            ( ( uint32_t ) ( cell.m_nGroundCover >> 8u ) << 16u ) |
            ( cell.m_nFlags != 0u ? 0xff000000u : 0u );
    }
    snapshot.m_environmentForcing = m_environmentForcing;
    snapshot.m_nEnvironmentImpulseCount = m_nEnvironmentPendingCount;
    if ( m_nEnvironmentPendingCount > 0u ) {
        ++m_nEnvironmentImpulseEpoch;
        if ( m_nEnvironmentImpulseEpoch == 0u ) {
            ++m_nEnvironmentImpulseEpoch;
        }
        memcpy( snapshot.m_szEnvironmentImpulses, m_szEnvironmentPending,
                m_nEnvironmentPendingCount * sizeof( t3dEnvironmentImpulse_t ) );
    }
    snapshot.m_nEnvironmentImpulseEpoch = m_nEnvironmentImpulseEpoch;
	commitSnapshotPublication( rServices, snapshot );
    m_nEnvironmentPendingCount = 0u;
}

const t3dRdFrameSnapshotSlot_t *Terrain3DRenderPublicationBuilder::acquireOnRenderThread(
	void ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), NULL );
#endif
	const t3dRdFrameSnapshotSlot_t *const current = m_bSnapshotRenderValid && m_nSnapshotRenderGeneration != 0u &&
															m_nSnapshotRenderIndex < T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT
														? &m_snapshotSlots[m_nSnapshotRenderIndex]
														: NULL;
	for ( ;; ) {
		uint64_t token = m_nSnapshotExchangeToken.load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( !t3dRdFrameSnapshotTokenIsPublished( token ) ) {
			if ( current ) {
				m_nSnapshotReuses.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			}
			return current;
		}
		const uint32_t candidate_index = t3dRdFrameSnapshotTokenIndex( token );
		const uint64_t candidate_generation = t3dRdFrameSnapshotTokenGeneration( token );
		if ( candidate_index >= T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT || candidate_generation == 0u ||
			 t3dRdFrameSnapshotGenerationIsStale( candidate_generation, m_nSnapshotRenderGeneration ) ) {
			recordStaleSnapshot( token );
			if ( current ) {
				m_nSnapshotReuses.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			}
			return current;
		}
		const uint64_t released_token =
			t3dRdMakeFrameSnapshotToken( m_nSnapshotRenderIndex, m_nSnapshotRenderGeneration, false );
		if ( !m_nSnapshotExchangeToken.compare_exchange_weak( token,
															  released_token,
															  T3D_MEMORY_ORDER_ACQ_REL,
															  T3D_MEMORY_ORDER_ACQUIRE ) ) {
			continue;
		}
		const uint64_t previous_generation = m_nSnapshotRenderGeneration;
		m_nSnapshotRenderIndex = candidate_index;
		m_nSnapshotRenderGeneration = candidate_generation;
		m_nConsumedSnapshotGeneration.store( candidate_generation, T3D_MEMORY_ORDER_RELEASE );
		m_nSnapshotConsumes.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		const uint64_t generation_distance =
			t3dRdFrameSnapshotGenerationDistance( candidate_generation, previous_generation );
		if ( previous_generation != 0u && generation_distance > 1u ) {
			m_nSkippedSnapshotGenerations.fetch_add( generation_distance - 1u, T3D_MEMORY_ORDER_RELAXED );
		}
		const t3dRdFrameSnapshotSlot_t &acquired = m_snapshotSlots[candidate_index];
		if ( acquired.m_snapshot.m_nPublicationGeneration != candidate_generation ) {
			m_bSnapshotRenderValid = false;
			recordStaleSnapshot( token );
			return NULL;
		}
		m_nRegionMapReaderBank.store( acquired.m_nRegionMapBank, T3D_MEMORY_ORDER_RELEASE );
		m_nInstanceMeshReaderBank.store( acquired.m_nInstanceMeshBank, T3D_MEMORY_ORDER_RELEASE );
		m_nInstanceSceneReaderBank.store( acquired.m_nInstanceSceneBank, T3D_MEMORY_ORDER_RELEASE );
		m_nOceanStripReaderBank.store( acquired.m_nOceanStripBank, T3D_MEMORY_ORDER_RELEASE );
		m_nWorldPublicationReaderBank.store( acquired.m_nWorldPublicationBank, T3D_MEMORY_ORDER_RELEASE );
		m_bSnapshotRenderValid = true;
		return &acquired;
	}
}

void Terrain3DRenderPublicationBuilder::recordStaleSnapshot(
	const uint64_t nToken ) {
	if ( m_nLastStaleSnapshotToken.exchange( nToken, T3D_MEMORY_ORDER_ACQ_REL ) != nToken ) {
		m_nStaleSnapshots.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
}

void Terrain3DRenderPublicationBuilder::reset(
	void ) {
	m_nSnapshotExchangeToken.store( t3dRdMakeFrameSnapshotToken( 1u, 0u, false ), T3D_MEMORY_ORDER_RELEASE );
	m_nSnapshotWriteIndex = 2u;
	m_nSnapshotRenderIndex = 0u;
	m_nSnapshotRenderGeneration = 0u;
	m_bSnapshotRenderValid = false;
	m_nPublishedSnapshotIndex.store( T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX, T3D_MEMORY_ORDER_RELEASE );
	m_nPublishedSnapshotGeneration.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nConsumedSnapshotGeneration.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nLastStaleSnapshotToken.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	for ( size_t iSlot = 0u; iSlot < T3D_ARRAY_COUNT( m_snapshotSlots ); iSlot++ ) {
		t3dRdFrameSnapshotSlot_t &slot = ( m_snapshotSlots )[iSlot];
		slot = t3dRdFrameSnapshotSlot_t{};
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_regionMapBanks ); iBank++ ) {
		t3dRdRegionMapBank_t &bank = ( m_regionMapBanks )[iBank];
		bank = t3dRdRegionMapBank_t{};
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceMeshBanks ); iBank++ ) {
		t3dRdInstanceMeshBank_t &bank = ( m_instanceMeshBanks )[iBank];
		bank = t3dRdInstanceMeshBank_t{};
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceSceneBanks ); iBank++ ) {
		t3dRdInstanceSceneBank_t &bank = ( m_instanceSceneBanks )[iBank];
		bank = t3dRdInstanceSceneBank_t{};
	}
	for ( size_t iTopology = 0u; iTopology < T3D_ARRAY_COUNT( m_oceanStripBanks ); iTopology++ ) {
		t3dRdOceanStripBank_t &bank = ( m_oceanStripBanks )[iTopology];
		bank = t3dRdOceanStripBank_t{};
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_worldPublicationBanks ); ++iBank ) {
		m_worldPublicationBanks[iBank].m_nGeneration = 0u;
		m_worldPublicationBanks[iBank].m_publication.clearForBuild();
	}
	m_nRegionMapReaderBank.store( T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX, T3D_MEMORY_ORDER_RELEASE );
	m_nInstanceMeshReaderBank.store( T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX, T3D_MEMORY_ORDER_RELEASE );
	m_nInstanceSceneReaderBank.store( T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX, T3D_MEMORY_ORDER_RELEASE );
	m_nOceanStripReaderBank.store( T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX, T3D_MEMORY_ORDER_RELEASE );
	m_nWorldPublicationReaderBank.store( T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX, T3D_MEMORY_ORDER_RELEASE );
    Terrain3DStPublicationImage emptySuperTerrain;
    m_stCommitted.swap( emptySuperTerrain );
    m_nStExpectedTopology = UINT64_MAX;
    m_nStCommitEpoch = 0u;
    m_nStRegionMapEpoch = UINT64_MAX;
	m_nCurrentRegionMapBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	m_nCurrentInstanceMeshBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	m_nCurrentInstanceSceneBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	m_nCurrentOceanStripBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	m_nCurrentWorldPublicationBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	m_nRegionTopologyGeneration = 0u;
    m_nVTPackagePublicationGeneration = UINT64_MAX;
	m_nPhysicalRegionMapGeneration = 0u;
	m_nRegionLightingSignature = 0u;
	m_nRegionMapGeneration = 0u;
	m_nLastRegionCacheSourceGeneration = 0u;
	m_nRegionCacheGeneration = 0u;
	m_nLastPatchFingerprint = 0u;
	m_nPatchGeneration = 0u;
	m_nPreviousPatchCount = 0u;
	m_nRegionLightingSourceGeneration = 0u;
	m_nRegionLightingScanCount = 0u;
	m_nMeshSourceBuildCount = 0u;
	m_nOceanStaticStateBuildCount = 0u;
	m_cachedOceanStaticState = t3dRdOceanState_t{};
	m_nOceanStaticSourceGeneration = 0u;
	m_bOceanStaticStateInitialized = false;
	m_nTerrainTextureGeneration = 0u;
	m_nMaterialTextureGeneration = 0u;
	m_lastPublishedResources = t3dRdTerrainBindings_t{};
	m_bRegionMapInitialized = false;
	m_bRegionMapHeightTextureReady = false;
	m_bRegionCacheInitialized = false;
	m_bPatchStateInitialized = false;
	m_bPublishedResourcesInitialized = false;
	m_worldGeometryCache.reset();
}

bool Terrain3DRenderPublicationBuilder::bindRuntimeAllocator(
	Terrain3DMemoryZone *const		   pZone,
	const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone ) {
		return false;
	}
	const Terrain3DEastlAllocator allocator = Terrain3DEastlAllocator::zoneAllocator( pZone, ePolicy );
	m_cachedHlodMeshSources.set_allocator( allocator );
	m_meshSourceIndex.bindRuntimeAllocator( allocator );
	m_worldGeometryCache.bindRuntimeAllocator( allocator );
	m_cachedInstanceMeshes.bindRuntimeAllocator( allocator );
	m_cachedRegularInstanceScene.bindRuntimeAllocator( pZone, ePolicy );
	m_cachedHlodInstanceScene.bindRuntimeAllocator( pZone, ePolicy );
	m_cachedInstanceScene.bindRuntimeAllocator( pZone, ePolicy );
	for ( uint32_t i = 0u; i < T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT; ++i ) {
		m_instanceMeshBanks[i].m_snapshot.bindRuntimeAllocator( allocator );
		m_instanceSceneBanks[i].m_snapshot.bindRuntimeAllocator( pZone, ePolicy );
		m_worldPublicationBanks[i].m_publication.bindRuntimeAllocator( allocator );
        m_worldPublicationBanks[i].m_publication.enableSharedGeometryConsumerCold();
	}
	return true;
}

bool Terrain3DRenderPublicationBuilder::initializeFixedStorageCold(
	void ) {
	const int64_t nRegionMapBankBytes = ( int64_t ) ( T3D_REGION_SLOTS * sizeof( t3dRdTerrainRegionGpu_t ) );
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_regionMapBanks ); ++iBank ) {
		if ( m_regionMapBanks[iBank].m_bytes.size() != nRegionMapBankBytes ) {
			m_regionMapBanks[iBank].m_bytes.resize( nRegionMapBankBytes );
		}
		if ( m_regionMapBanks[iBank].m_bytes.size() != nRegionMapBankBytes ) {
			return false;
		}
	}
	return true;
}

void Terrain3DRenderPublicationBuilder::initializeOcean(
	const t3dPublicationServices_t &rServices ) {
	if ( !rServices.m_pTerrain || !rServices.m_pTerrain->getOceanMesher() ) {
		m_cachedOceanStrip = t3dRdOceanStripSnapshot_t{};
		m_bCachedOceanStripValid = false;
		return;
	}
	t3dRdOceanStripSnapshot_t topology;
	m_nOceanStripGeneration = t3dNextGeneration( m_nOceanStripGeneration );
	topology.m_nGeneration = m_nOceanStripGeneration;
	for ( uint32_t type = 0u; type < MESH_TYPE_COUNT; ++type ) {
		const t3dMeshType_t mesh_type = ( t3dMeshType_t ) ( type );
		const uint16_t		count = rServices.m_pTerrain->getOceanMesher()->getClipmapStripPartCount( mesh_type );
		if ( count == 0u || count > T3D_CLIPMAP_STRIP_PART_CAPACITY ) {
			m_cachedOceanStrip = t3dRdOceanStripSnapshot_t{};
			m_bCachedOceanStripValid = false;
			return;
		}
		topology.m_szStripPartCounts[type] = count;
		memcpy( topology.m_szStripParts[type],
				rServices.m_pTerrain->getOceanMesher()->getClipmapStripParts( mesh_type ),
				sizeof( t3dClipmapStripPart_t ) * count );
	}
	m_cachedOceanStrip = topology;
	m_bCachedOceanStripValid = true;
}

void Terrain3DRenderPublicationBuilder::clearCachedSources(
	void ) {
	m_meshSourceIndex.reset();
	m_worldGeometryCache.reset();
	m_cachedInstanceMeshes = t3dRdMeshSourceSnapshot_t{};
	m_bCachedInstanceMeshesValid = false;
	m_cachedRegularInstanceScene = t3dRdInstanceSceneSnapshot_t{};
	m_cachedHlodInstanceScene = t3dRdInstanceSceneSnapshot_t{};
	m_cachedInstanceScene = t3dRdInstanceSceneSnapshot_t{};
	m_bCachedRegularInstanceSceneValid = false;
	m_bCachedHlodInstanceSceneValid = false;
	m_bCachedInstanceSceneValid = false;
	m_cachedOceanStrip = t3dRdOceanStripSnapshot_t{};
	m_bCachedOceanStripValid = false;
	m_cachedHlodMeshSources.clear();
	m_nInstanceMeshSourceGeneration = 0u;
	m_nInstanceMeshGeneration = 0u;
	m_nInstanceSceneGeneration = 0u;
	m_nInstanceOcclusionGeneration = 0u;
	m_nHlodSceneGeneration = 0u;
	m_nCombinedInstanceSceneGeneration = 0u;
	m_nWorldPublicationGeneration = 0u;
	m_nWorldGeometryGeneration = 0u;
	m_nWorldMaterialGeneration = 0u;
	m_worldPublicationRequired = t3dWorldPublicationCapacity_t{};
	m_bWorldPublicationCapacityPending = true;
}

bool Terrain3DRenderPublicationBuilder::compactRetiredGlobalMeshesOwner(
    const uint64_t nCompletedPublicationGeneration, const t3dWorldResourceLimits_t &rLimits )
{
    // A fence covering the first prepared WORLD publication that excludes a
    // retired source also completes all earlier draws of that source. Later
    // publications cannot revive a zero-lease allocation without first
    // reactivating it, which prevents the cache from retiring it. Waiting for
    // the latest generation would starve reclamation during active streaming.
    if ( !nCompletedPublicationGeneration ) {
        return false;
    }
    Terrain3DColdPhaseScope cold_phase_scope;
    // Fence-certified retirement returns independent WORLD stream ranges without
    // relocating live sources or invalidating their published cluster offsets.
    if ( m_worldGeometryCache.releaseCompletedRetirementsCold( nCompletedPublicationGeneration ) ) {
        return true;
    }
    const uint64_t nRetiredBytes = m_worldGeometryCache.getQuarantinedGeometryBytes();
    const uint64_t nLiveBytes = m_worldGeometryCache.getLiveGeometryBytes();
    const Terrain3DWorldPublication &cached = m_worldGeometryCache.geometry();
    const bool bCapacityPressure =
        ( rLimits.m_nMaxSourceVertices && cached.m_vertices.size() * 4u >=
            uint64_t( rLimits.m_nMaxSourceVertices ) * 3u ) ||
        ( rLimits.m_nMaxClusterVertexIndices && cached.m_clusterVertexIndices.size() * 4u >=
            uint64_t( rLimits.m_nMaxClusterVertexIndices ) * 3u ) ||
        ( rLimits.m_nMaxTriangleSlots && cached.m_triangleWords.size() * 4u >=
            uint64_t( rLimits.m_nMaxTriangleSlots ) * 3u ) ||
        ( rLimits.m_nMaxClusters && cached.m_clusters.size() * 4u >=
            uint64_t( rLimits.m_nMaxClusters ) * 3u );
    // Avoid rebuilding all live geometry for small isolated unloads. Reclaim
    // immediately under capacity pressure; otherwise batch retired spans until
    // they exceed 4 MiB or half the retained geometry. No allocation in hot paths.
    if ( !nRetiredBytes ||
         ( !bCapacityPressure && nRetiredBytes < 4u * 1024u * 1024u &&
           nRetiredBytes < nLiveBytes ) ) {
        return false;
    }
    if ( !m_worldGeometryCache.compactAfterGpuCompletionCold( nCompletedPublicationGeneration, rLimits ) ) {
        return false;
    }
    // The compacted offsets are a new immutable layout. A new publication
    // must be built even when imported scene/terrain source signatures match.
    m_nCurrentWorldPublicationBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
    return true;
}

godot::Dictionary Terrain3DRenderPublicationBuilder::getGlobalMeshCacheDiagnosticsOwner( void ) const
{
	godot::Dictionary stats;
	stats["active_source_records"] = ( int64_t ) m_worldGeometryCache.getActiveSourceCount();
	stats["tracked_source_records"] = ( int64_t ) m_worldGeometryCache.getTrackedSourceCount();
	stats["deduplicated_sources_total"] = ( int64_t ) m_worldGeometryCache.getDeduplicatedSourceCount();
	stats["deduplicated_upload_bytes_total"] = ( int64_t ) m_worldGeometryCache.getDeduplicatedGpuBytes();
	stats["live_geometry_bytes"] = ( int64_t ) m_worldGeometryCache.getLiveGeometryBytes();
	stats["quarantined_geometry_bytes"] = ( int64_t ) m_worldGeometryCache.getQuarantinedGeometryBytes();
	stats["owner_retained_cpu_bytes"] = ( int64_t ) m_worldGeometryCache.getRetainedCpuBytes();
	stats["admission_failures_total"] = ( int64_t ) m_worldGeometryCache.getAdmissionFailureCount();
	stats["cache_epoch"] = ( int64_t ) m_worldGeometryCache.getCacheEpoch();
	stats["recyclable_geometry_bytes"] = ( int64_t ) m_worldGeometryCache.getFreeGeometryBytes();
    stats["recycled_physical_spans_total"] = ( int64_t ) m_worldGeometryCache.getRecycledSpanCount();
    stats["recyclable_physical_ranges"] = ( int64_t ) m_worldGeometryCache.getFreeGeometryRangeCount();
    stats["reused_geometry_bytes_total"] = ( int64_t ) m_worldGeometryCache.getReusedGeometryBytes();
    stats["gpu_safe_compactions"] = ( int64_t ) m_worldGeometryCache.getSafeCompactionCount();
	stats["reclaimed_geometry_bytes_total"] = ( int64_t ) m_worldGeometryCache.getReclaimedGeometryBytes();
	return stats;
}

size_t Terrain3DRenderPublicationBuilder::getRetainedCpuBytes(
	void ) const {
	size_t bytes = sizeof( m_snapshotSlots );
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_regionMapBanks ); ++iBank ) {
		bytes += m_regionMapBanks[iBank].m_bytes.size();
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceMeshBanks ); ++iBank ) {
		bytes += m_instanceMeshBanks[iBank].m_snapshot.getRetainedCpuBytes();
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceSceneBanks ); ++iBank ) {
		bytes += m_instanceSceneBanks[iBank].m_snapshot.getRetainedCpuBytes();
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_oceanStripBanks ); ++iBank ) {
		bytes += sizeof( m_oceanStripBanks[iBank].m_snapshot );
	}
    // Count historical frozen payloads still leased by render banks once each.
    // The current frozen payload is counted by the global cache below.
    const Terrain3DSharedWorldGeometry *pCurrentFrozen = m_worldGeometryCache.getFrozenGeometryIdentity();
    for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_worldPublicationBanks ); ++iBank ) {
        const Terrain3DWorldPublication &rBank = m_worldPublicationBanks[iBank].m_publication;
        bytes += rBank.getRetainedCpuBytes();
        const Terrain3DSharedWorldGeometry *pSnapshot = rBank.getSharedGeometryIdentity();
        if ( !pSnapshot || pSnapshot == pCurrentFrozen ) { continue; }
        bool bAlreadyCounted = false;
        for ( size_t iEarlier = 0u; iEarlier < iBank; ++iEarlier ) {
            if ( m_worldPublicationBanks[iEarlier].m_publication.getSharedGeometryIdentity() == pSnapshot ) {
                bAlreadyCounted = true;
                break;
            }
        }
        if ( !bAlreadyCounted ) { bytes += pSnapshot->getRetainedCpuBytes(); }
    }
    bytes += m_cachedInstanceMeshes.getRetainedCpuBytes();
	bytes += m_meshSourceIndex.getRetainedCpuBytes();
	bytes += m_worldGeometryCache.getRetainedCpuBytes();
	bytes += m_cachedRegularInstanceScene.getRetainedCpuBytes();
	bytes += m_cachedHlodInstanceScene.getRetainedCpuBytes();
	bytes += m_cachedInstanceScene.getRetainedCpuBytes();
	bytes += m_cachedHlodMeshSources.capacity() * sizeof( t3dRdExternalMeshSource_t );
	return bytes;
}
