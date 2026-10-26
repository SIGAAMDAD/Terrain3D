// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_GEOMETRY_H
#define TERRAIN3D_WORLD_GEOMETRY_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "rendering/rd/world/core/terrain_3d_world_contract.h"

#include <stdint.h>

static_assert( sizeof( t3dWorldVertex_t ) == 32, "t3dWorldVertex_t ABI mismatch" );
static_assert( sizeof( t3dWorldPackedClusterTriangle_t ) == 4, "t3dWorldPackedClusterTriangle_t ABI mismatch" );
static_assert( sizeof( t3dWorldMeshRecord_t ) == 32, "t3dWorldMeshRecord_t ABI mismatch" );
static_assert( sizeof( t3dWorldMeshLodRecord_t ) == 32, "t3dWorldMeshLodRecord_t ABI mismatch" );
static_assert( sizeof( t3dWorldClusterRecord_t ) == 64, "t3dWorldClusterRecord_t ABI mismatch" );

void t3d_world_geometry_database_initialize_cold( t3dWorldGeometryDatabase_t			  &rDatabase,
												  const t3dWorldGeometryDatabaseBuffers_t &pBuffers );

t3dWorldGeometryCookResult_t t3d_world_geometry_append_mesh_cold( t3dWorldGeometryDatabase_t		 &rDatabase,
																  const t3dWorldGeometrySourceMesh_t &pSource,
																  t3dWorldGeometryCookedMeshSpan_t	 &rSpan,
																  t3dWorldGeometryCookStats_t		 &rStats );

uint64_t t3d_world_geometry_compute_signature_reference_debug( const t3dWorldGeometryDatabase_t &pDatabase );

bool t3d_world_geometry_validate_database_reference_debug( const t3dWorldGeometryDatabase_t &pDatabase );

bool t3d_world_geometry_validate_source_reconstruction_reference_debug( const t3dWorldGeometryDatabase_t   &pDatabase,
																		const t3dWorldGeometrySourceMesh_t &pSource,
																		const t3dWorldGeometryCookedMeshSpan_t &pSpan );

bool t3d_world_geometry_reconstruct_triangle_reference_debug( const t3dWorldGeometryDatabase_t &pDatabase,
															  uint32_t							pClusterId,
															  uint32_t							pTriangleIndex,
															  uint32_t						   &rGlobalVertex0,
															  uint32_t						   &rGlobalVertex1,
															  uint32_t						   &rGlobalVertex2 );

uint32_t t3d_world_geometry_pack_local_triangle( uint32_t pLocal0, uint32_t pLocal1, uint32_t pLocal2 );
void	 t3d_world_geometry_unpack_local_triangle( uint32_t	 pPacked,
												   uint32_t &rLocal0,
												   uint32_t &rLocal1,
												   uint32_t &rLocal2 );

float t3d_world_geometry_unpack_texel_density( uint32_t pPacked );
float t3d_world_geometry_cluster_fill_ratio( const t3dWorldGeometryCookStats_t &pStats );

#endif // TERRAIN3D_WORLD_GEOMETRY_H
