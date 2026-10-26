// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "core/utility/logger.h"
#include "core/utility/terrain_3d_integration_helpers.h"
#include "core/utility/terrain_3d_util.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "eastl_config.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"
#include "lighting/policy/terrain_3d_lighting_policy.h"
#include "lighting/runtime/terrain_3d_lightmap_streamer.h"
#include "memory/arena/terrain_3d_linear_arena.h"
#include "memory/arena/terrain_3d_zone_allocator.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "simd/core/terrain_3d_parallel.h"
#include "streaming/runtime/terrain_3d_node_streamer.h"
#include "streaming/policy/terrain_3d_performance_profile.h"
#include "streaming/policy/terrain_3d_streaming_policy.h"
#include "terrain_3d.h"

#include <EASTL/numeric_limits.h>
#include <math.h>
#include <string.h>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

bool Terrain3D::bakeRegionSnapshotLess(
	const t3dBakeRegionSnapshot_t &rA,
	const t3dBakeRegionSnapshot_t &rB ) {
	if ( rA.m_location.y != rB.m_location.y ) {
		return rA.m_location.y < rB.m_location.y;
	}
	return rA.m_location.x < rB.m_location.x;
}

t3dBakeSnapshot_t Terrain3D::createBakeSnapshot(
	void ) const {
	t3dBakeSnapshot_t snapshot;
	snapshot.m_fVertexSpacing = s_fRegionVertexSpacing;
	eastl::fill_n( snapshot.m_szRegionLookup, T3D_REGION_SLOTS, ( int32_t ) ( -1 ) );

	const TypedArray< Vector2i > region_locations = m_pData->getRegionLocations();
	snapshot.m_regions.reserve( region_locations.size() );
	const int64_t expected_height_bytes =
		( int64_t ) ( T3D_REGION_SIZE ) * ( int64_t ) ( T3D_REGION_SIZE ) * ( int64_t ) ( sizeof( uint16_t ) );
	const int64_t expected_control_bytes =
		( int64_t ) ( T3D_REGION_SIZE ) * ( int64_t ) ( T3D_REGION_SIZE ) * ( int64_t ) ( sizeof( uint32_t ) );

	for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( region_locations ).size() ); iRegion++ ) {
		const Vector2i regionLocation = ( region_locations )[iRegion];
		if ( !m_pData->ensureCpuRegionResident( regionLocation ) ) {
			LOG( ERROR, "Skipping terrain region that could not be paged for bake snapshot: ", regionLocation );
			continue;
		}
		Terrain3DRegion *const pRegion = m_pData->getRegionPtr( regionLocation );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}

		const Ref< Image > pHeightMap = pRegion->getHeightMap();
		const Ref< Image > pControlMap = pRegion->getControlMap();
		if ( pHeightMap.is_null() || pHeightMap->get_format() != Image::FORMAT_R16 ||
			 pHeightMap->get_width() != T3D_REGION_SIZE || pHeightMap->get_height() != T3D_REGION_SIZE ||
			 ( pControlMap.is_valid() &&
			   ( pControlMap->get_format() != Image::FORMAT_RF || pControlMap->get_width() != T3D_REGION_SIZE ||
				 pControlMap->get_height() != T3D_REGION_SIZE ) ) ) {
			LOG( ERROR, "Skipping invalid terrain region during bake snapshot: ", regionLocation );
			continue;
		}

		t3dBakeRegionSnapshot_t region_snapshot;
		region_snapshot.m_location = regionLocation;
		region_snapshot.m_heightData = pHeightMap->get_data();
		if ( pControlMap.is_valid() ) {
			region_snapshot.m_controlData = pControlMap->get_data();
		}
		if ( region_snapshot.m_heightData.size() < expected_height_bytes ||
			 ( pControlMap.is_valid() && region_snapshot.m_controlData.size() < expected_control_bytes ) ) {
			LOG( ERROR, "Skipping terrain region with incomplete map data during bake snapshot: ", regionLocation );
			continue;
		}

		const int32_t map_index = Terrain3DData::getRegionMapIndex( regionLocation );
		if ( map_index < 0 ) {
			continue;
		}
		snapshot.m_regions.push_back( eastl::move( region_snapshot ) );
	}

	// Keep adjacent terrain regions adjacent in the snapshot vector. Whole-terrain
	// bake tasks iterate this order, so spatial sorting improves reuse of neighboring
	// height/control payloads and makes worker task construction deterministic even
	// when the resource's active-region order reflects edit history.
	eastl::sort( snapshot.m_regions.begin(), snapshot.m_regions.end(), bakeRegionSnapshotLess );
	for ( size_t region_index = 0; region_index < snapshot.m_regions.size(); ++region_index ) {
		const int32_t map_index = Terrain3DData::getRegionMapIndex( snapshot.m_regions[region_index].m_location );
		if ( map_index >= 0 ) {
			snapshot.m_szRegionLookup[map_index] = ( int32_t ) ( region_index );
		}
	}

	return snapshot;
}

void Terrain3D::runBakeTaskRange(
	void *const	 pContext,
	const size_t nBegin,
	const size_t nEnd ) {
	t3dBakeTaskContext_t *context = ( t3dBakeTaskContext_t * ) ( pContext );
	for ( size_t task_index = nBegin; task_index < nEnd; ++task_index ) {
		const t3dBakeRowTask_t &task = ( *context->m_pTasks )[task_index];
		t3dTriangleBuffer_t	   &buffer = ( *context->m_pBuffers )[task_index];
		for ( int32_t z = task.m_nZStart; z < task.m_nZEnd; z += context->m_nStep ) {
			for ( int32_t x = task.m_nXStart; x < task.m_nXEnd; x += context->m_nStep ) {
				context->m_pTerrain->generateTrianglePair( buffer,
														   { context->m_bGenerateUvs,
															 context->m_nLod,
															 context->m_filter,
															 context->m_bRequireNav,
															 x,
															 z,
															 context->m_pSnapshot } );
			}
		}
	}
}

Vector3 Terrain3D::getTriangleVertex(
	const t3dBakeSnapshot_t *pSnapshot,
	const int32_t			 nLod,
	const t3dHeightFilter_t	 filter,
	const int32_t			 iX,
	const int32_t			 iZ,
	const Vector3			&rPosition,
	uint32_t				&nControl ) const {
	if ( pSnapshot ) {
		return pSnapshot->getMeshVertex( nLod, filter, iX, iZ, nControl );
	}
	if ( !m_pData->prepareSamplingArea( rPosition, ( 1 << CLAMP( nLod, 0, 8 ) ) / 2 + 1 ) ) {
		nControl = UINT32_MAX;
		return Vector3( rPosition.x, NAN, rPosition.z );
	}
	const Vector3 vertex = m_pData->getMeshVertex( nLod, filter, rPosition );
	nControl = m_pData->getControl( rPosition );
	return vertex;
}

void Terrain3D::generateTriangles(
	PackedVector3Array		 &rVertices,
	PackedVector2Array *const pUvs,
	const int32_t			  nLod,
	const t3dHeightFilter_t	  filter,
	const bool				  bRequireNav,
	const AABB				 &rGlobalAabb ) const {
	ERR_FAIL_COND( m_pData == NULL );
	const int32_t						 step = 1 << CLAMP( nLod, 0, 8 );
	const bool							 generate_uvs = pUvs != NULL;
	eastl::vector< t3dTriangleBuffer_t > buffers;
	Terrain3DLinearArena				 bake_arena;
	Terrain3DArenaBuffer< Vector3 >		 bake_vertex_slab;
	Terrain3DArenaBuffer< Vector2 >		 bake_uv_slab;

	// Whole-terrain bakes are split into row chunks. This parallelizes even a single large region,
	// while keeping each worker's output isolated and preserving deterministic merge order.
	if ( !rGlobalAabb.has_volume() ) {
		const int32_t	  nRegionSize = ( int32_t ) T3D_REGION_SIZE;
		const int32_t	  rows_per_task = 16;
		t3dBakeSnapshot_t snapshot = createBakeSnapshot();
		for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( snapshot.m_regions ).size() ); iRegion++ ) {
			t3dBakeRegionSnapshot_t &region = ( snapshot.m_regions )[iRegion];
			region.m_pHeightBytes = region.m_heightData.ptr();
			region.m_pControlBytes = region.m_controlData.is_empty() ? NULL : region.m_controlData.ptr();
		}
		eastl::vector< t3dBakeRowTask_t > tasks;
		tasks.reserve( snapshot.m_regions.size() * MAX( 1, nRegionSize / ( step * rows_per_task ) ) );

		size_t total_output_capacity = 0u;
		for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( snapshot.m_regions ).size() ); iRegion++ ) {
			const t3dBakeRegionSnapshot_t &region = ( snapshot.m_regions )[iRegion];
			const Vector2i				   region_loc = region.m_location;
			const Vector2i				   region_pos = region_loc * nRegionSize;
			const int32_t				   region_z_end = region_pos.y + nRegionSize;
			for ( int32_t z = region_pos.y; z < region_z_end; z += step * rows_per_task ) {
				const int32_t z_end = MIN( z + step * rows_per_task, region_z_end );
				const size_t  column_count = ( size_t ) ( ( nRegionSize + step - 1 ) / step );
				const size_t  row_count = ( size_t ) ( ( z_end - z + step - 1 ) / step );
				ERR_FAIL_COND_MSG( column_count != 0u &&
									   row_count > eastl::numeric_limits< size_t >::max() / column_count,
								   "Terrain bake task dimensions overflow size_t." );
				const size_t quad_count = column_count * row_count;
				ERR_FAIL_COND_MSG( quad_count > eastl::numeric_limits< size_t >::max() / 6u,
								   "Terrain bake triangle capacity overflows size_t." );
				const size_t output_capacity = quad_count * 6u;
				// Isolate adjacent worker slabs on independent cacheline boundaries. The
				// merge uses each buffer's written size, so padding remains invisible.
				ERR_FAIL_COND_MSG( total_output_capacity > eastl::numeric_limits< size_t >::max() -
															   ( T3D_BAKE_TASK_ALIGNMENT_ELEMENTS - 1u ),
								   "Terrain bake task alignment overflows size_t." );
				total_output_capacity = TERRAIN3D_ALIGN_BAKE_TASK_OFFSET( total_output_capacity );
				ERR_FAIL_COND_MSG( output_capacity > eastl::numeric_limits< size_t >::max() - total_output_capacity,
								   "Terrain bake output capacity overflows size_t." );
				tasks.push_back(
					{ region_pos.x, region_pos.x + nRegionSize, z, z_end, total_output_capacity, output_capacity } );
				total_output_capacity += output_capacity;
			}
		}

		Terrain3DLinearArenaLayout bake_layout;
		const bool				   bLayoutValid = bake_layout.addArray< Vector3 >( total_output_capacity ) &&
								  ( !generate_uvs || bake_layout.addArray< Vector2 >( total_output_capacity ) );
		ERR_FAIL_COND_MSG( !bLayoutValid || !bake_layout.valid() || !bake_arena.configure( bake_layout.size() ),
						   "Terrain bake scratch arena allocation failed." );
		ERR_FAIL_COND_MSG( !bake_vertex_slab.allocateUninitialized( bake_arena, total_output_capacity ),
						   "Terrain bake vertex slab allocation did not match its precomputed layout." );
		if ( generate_uvs ) {
			ERR_FAIL_COND_MSG( !bake_uv_slab.allocateUninitialized( bake_arena, total_output_capacity ),
							   "Terrain bake UV slab allocation did not match its precomputed layout." );
		}
		ERR_FAIL_COND_MSG( bake_arena.used() != bake_layout.size(),
						   "Terrain bake scratch arena consumption did not match its precomputed layout." );
		buffers.resize( tasks.size() );
		for ( size_t task_index = 0; task_index < tasks.size(); ++task_index ) {
			const t3dBakeRowTask_t &task = tasks[task_index];
			buffers[task_index].bindExternal( bake_vertex_slab.data() + task.m_iOutputOffset,
											  generate_uvs ? bake_uv_slab.data() + task.m_iOutputOffset : NULL,
											  task.m_nOutputCapacity );
		}
		if ( !tasks.empty() ) {
#if defined( __EMSCRIPTEN__ ) && !defined( __EMSCRIPTEN_PTHREADS__ )
			const uint32_t worker_count = 1u;
#else
			const uint32_t hardware_threads =
				MAX( 1u, ( uint32_t ) ( MAX( 1, OS::get_singleton()->get_processor_count() ) ) );
			const uint32_t worker_count = MIN( MIN( hardware_threads, ( uint32_t ) tasks.size() ), 32u );
#endif
			const Terrain3DParallelExecutor executor{ ( int32_t ) ( worker_count ) };
			t3dBakeTaskContext_t			task_context;
			task_context.m_pTerrain = this;
			task_context.m_pSnapshot = &snapshot;
			task_context.m_pTasks = &tasks;
			task_context.m_pBuffers = &buffers;
			task_context.m_nStep = step;
			task_context.m_nLod = nLod;
			task_context.m_filter = filter;
			task_context.m_bGenerateUvs = generate_uvs;
			task_context.m_bRequireNav = bRequireNav;

			LOG( INFO,
				 "Generating terrain bake geometry with up to ",
				 worker_count,
				 " persistent worker tasks across ",
				 tasks.size(),
				 " chunks" );
			executor.run( tasks.size(), 1u, runBakeTaskRange, &task_context );
		}

		for ( size_t iBuffer = 0u; iBuffer < ( size_t ) ( ( buffers ).size() ); iBuffer++ ) {
			const t3dTriangleBuffer_t &buffer = ( buffers )[iBuffer];
			ERR_FAIL_COND_MSG(
				buffer.m_bOverflowed,
				"Terrain bake output exceeded its precomputed slab capacity. This indicates a bake sizing "
				"invariant failure." );
		}
	} else {
		// AABB generation is primarily used by runtime navigation baking. Keep it serial because these
		// requests are generally small and thread setup costs can exceed the geometry work.
		t3dTriangleBuffer_t buffer;
		const int32_t		z_start = ( int32_t ) Math::ceil( rGlobalAabb.position.z / s_fRegionVertexSpacing );
		const int32_t		z_end = ( int32_t ) Math::floor( rGlobalAabb.get_end().z / s_fRegionVertexSpacing ) + 1;
		const int32_t		x_start = ( int32_t ) Math::ceil( rGlobalAabb.position.x / s_fRegionVertexSpacing );
		const int32_t		x_end = ( int32_t ) Math::floor( rGlobalAabb.get_end().x / s_fRegionVertexSpacing ) + 1;
		const size_t		column_count = ( size_t ) ( MAX( 0, x_end - x_start + step - 1 ) / step );
		const size_t		row_count = ( size_t ) ( MAX( 0, z_end - z_start + step - 1 ) / step );
		buffer.reserve( column_count * row_count * 6u, generate_uvs );

		for ( int32_t z = z_start; z < z_end; z += step ) {
			for ( int32_t x = x_start; x < x_end; x += step ) {
				const Vector3 samplePosition( ( real_t ) ( x ) *s_fRegionVertexSpacing,
											  0.f,
											  ( real_t ) ( z ) *s_fRegionVertexSpacing );
				if ( !m_pData->prepareSamplingArea( samplePosition, step + 1 ) ) {
					continue;
				}
				const real_t fHeight = m_pData->getHeight( Vector3( ( real_t ) ( x ) *s_fRegionVertexSpacing,
																	0.f,
																	( real_t ) ( z ) *s_fRegionVertexSpacing ) );
				if ( fHeight >= rGlobalAabb.position.y && fHeight <= rGlobalAabb.get_end().y ) {
					generateTrianglePair( buffer, { generate_uvs, nLod, filter, bRequireNav, x, z, NULL } );
				}
			}
		}
		buffers.push_back( eastl::move( buffer ) );
	}

	size_t vertex_count = 0;
	size_t uv_count = 0;
	for ( size_t iBuffer = 0u; iBuffer < ( size_t ) ( ( buffers ).size() ); iBuffer++ ) {
		const t3dTriangleBuffer_t &buffer = ( buffers )[iBuffer];
		vertex_count += buffer.size();
		uv_count += buffer.uvSize();
	}
	ERR_FAIL_COND( generate_uvs && vertex_count != uv_count );
	ERR_FAIL_COND( vertex_count > ( size_t ) ( eastl::numeric_limits< int32_t >::max() ) );

	rVertices.resize( ( int64_t ) vertex_count );
	if ( vertex_count > 0 ) {
		Vector3 *const pVertexWrite = rVertices.ptrw();
		size_t		   vertex_offset = 0;
		for ( size_t iBuffer = 0u; iBuffer < ( size_t ) ( ( buffers ).size() ); iBuffer++ ) {
			const t3dTriangleBuffer_t &buffer = ( buffers )[iBuffer];
			if ( buffer.size() > 0u ) {
				memcpy( pVertexWrite + vertex_offset,
						buffer.vertexData(),
						sizeof( buffer.vertexData()[0] ) * ( size_t ) ( buffer.size() ) );
			}
			vertex_offset += buffer.size();
		}
	}

	if ( generate_uvs ) {
		pUvs->resize( ( int64_t ) uv_count );
		if ( uv_count > 0 ) {
			Vector2 *const pUvWrite = pUvs->ptrw();
			size_t		   uv_offset = 0;
			for ( size_t iBuffer = 0u; iBuffer < ( size_t ) ( ( buffers ).size() ); iBuffer++ ) {
				const t3dTriangleBuffer_t &buffer = ( buffers )[iBuffer];
				if ( buffer.uvSize() > 0u ) {
					memcpy( pUvWrite + uv_offset,
							buffer.uvData(),
							sizeof( buffer.uvData()[0] ) * ( size_t ) ( buffer.uvSize() ) );
				}
				uv_offset += buffer.uvSize();
			}
		}
	}
}

void Terrain3D::generateTrianglePair(
	t3dTriangleBuffer_t			   &rBuffer,
	const t3dTrianglePairRequest_t &rRequest ) const {
	const bool					   bGenerateUvs = rRequest.m_bGenerateUvs;
	const int32_t				   nLod = rRequest.m_nLod;
	const t3dHeightFilter_t		   filter = rRequest.m_filter;
	const bool					   bRequireNav = rRequest.m_bRequireNav;
	const int32_t				   x = rRequest.m_nX, z = rRequest.m_nZ;
	const t3dBakeSnapshot_t *const pSnapshot = rRequest.m_pSnapshot;
	const int32_t				   step = 1 << CLAMP( nLod, 0, 8 );
	const Vector3				   xz = Vector3( x, 0.0f, z ) * s_fRegionVertexSpacing;
	const Vector3				   xsz = Vector3( x + step, 0.0f, z ) * s_fRegionVertexSpacing;
	const Vector3				   xzs = Vector3( x, 0.0f, z + step ) * s_fRegionVertexSpacing;
	const Vector3				   xszs = Vector3( x + step, 0.0f, z + step ) * s_fRegionVertexSpacing;
	uint32_t					   ctrl1 = UINT32_MAX;
	uint32_t					   ctrl2 = UINT32_MAX;
	uint32_t					   ctrl3 = UINT32_MAX;
	uint32_t					   ctrl4 = UINT32_MAX;
	const Vector3				   v1 = getTriangleVertex( pSnapshot, nLod, filter, x, z, xz, ctrl1 );
	const bool					   bNan1 = isnan( v1.y );
	if ( bNan1 ) {
		return;
	}
	Vector3	   v2 = getTriangleVertex( pSnapshot, nLod, filter, x + step, z, xsz, ctrl2 );
	Vector3	   v3 = getTriangleVertex( pSnapshot, nLod, filter, x, z + step, xzs, ctrl3 );
	Vector3	   v4 = getTriangleVertex( pSnapshot, nLod, filter, x + step, z + step, xszs, ctrl4 );
	const bool bNan2 = isnan( v2.y );
	const bool bNan3 = isnan( v3.y );
	const bool bNan4 = isnan( v4.y );
	if ( bNan2 ) {
		v2.y = v1.y;
	}
	if ( bNan3 ) {
		v3.y = v1.y;
	}
	if ( bNan4 ) {
		if ( !bNan2 ) {
			v4.y = v2.y;
		} else if ( !bNan3 ) {
			v4.y = v3.y;
		} else {
			v4.y = v1.y;
		}
	}
	const bool bHole1 = ctrl1 != UINT32_MAX && t3dIsHole( ctrl1 );
	const bool bHole2 = ctrl2 != UINT32_MAX && t3dIsHole( ctrl2 );
	const bool bHole3 = ctrl3 != UINT32_MAX && t3dIsHole( ctrl3 );
	const bool bHole4 = ctrl4 != UINT32_MAX && t3dIsHole( ctrl4 );
	const bool bNav1 = ctrl1 != UINT32_MAX && t3dIsNav( ctrl1 );
	const bool bNav2 = ( ctrl2 != UINT32_MAX && t3dIsNav( ctrl2 ) ) || ( bNan2 && bNav1 );
	const bool bNav3 = ( ctrl3 != UINT32_MAX && t3dIsNav( ctrl3 ) ) || ( bNan3 && bNav1 );
	const bool bNav4 = ( ctrl4 != UINT32_MAX && t3dIsNav( ctrl4 ) ) || ( bNan4 && bNav1 );

	if ( !( bHole1 || bHole4 || bHole3 ) && ( !bRequireNav || ( bNav1 && bNav4 && bNav3 ) ) ) {
		rBuffer.appendTriangle( v1, v4, v3, bGenerateUvs );
	}
	if ( !( bHole1 || bHole2 || bHole4 ) && ( !bRequireNav || ( bNav1 && bNav2 && bNav4 ) ) ) {
		rBuffer.appendTriangle( v1, v2, v4, bGenerateUvs );
	}
}

Ref< Mesh > Terrain3D::bakeMesh(
	const int				nLod,
	const t3dHeightFilter_t filter ) const {
	LOG( INFO, "Baking mesh at lod: ", nLod, " with filter: ", filter );
	Ref< Mesh > pResult;
	ERR_FAIL_COND_V( m_pData == NULL, pResult );

	Ref< SurfaceTool > pSt;
	pSt.instantiate();
	pSt->begin( Mesh::PRIMITIVE_TRIANGLES );

	PackedVector3Array vertices;
	PackedVector2Array uvs;
	generateTriangles( vertices, &uvs, nLod, filter, false, AABB() );

	ERR_FAIL_COND_V( vertices.size() != uvs.size(), pResult );
	for ( int i = 0; i < vertices.size(); ++i ) {
		pSt->set_uv( uvs[i] );
		pSt->add_vertex( vertices[i] );
	}

	pSt->index();
	pSt->generate_normals();
	pSt->generate_tangents();
	pSt->optimize_indices_for_cache();
	pResult = pSt->commit();
	return pResult;
}

Ref< Mesh > Terrain3D::bakeRegionLightmapMesh(
	const Vector2i &rRegionLocation,
	const int		nLod,
	const int		nContextRadiusRegions,
	const real_t	fTexelSize ) const {
	return bakeLightmapClusterMesh( rRegionLocation, rRegionLocation, nLod, nContextRadiusRegions, fTexelSize );
}

Ref< Mesh > Terrain3D::bakeLightmapClusterMesh(
	const Vector2i &rRegionMin,
	const Vector2i &rRegionMax,
	const int		nLod,
	const int		nContextRadiusRegions,
	const real_t	fTexelSize ) const {
	Ref< Mesh > pResult;
	ERR_FAIL_COND_V( m_pData == NULL, pResult );
	ERR_FAIL_COND_V( fTexelSize <= 0.0f, pResult );

	const int lod = CLAMP( nLod, 0, 8 );
	const int context = CLAMP( nContextRadiusRegions, 0, 4 );
	const int nRegionSize = m_pData->getRegionSizeInternal();
	ERR_FAIL_COND_V( nRegionSize <= 0, pResult );
	const Vector2i ownerMin( MIN( rRegionMin.x, rRegionMax.x ), MIN( rRegionMin.y, rRegionMax.y ) );
	const Vector2i ownerMax( MAX( rRegionMin.x, rRegionMax.x ), MAX( rRegionMin.y, rRegionMax.y ) );
	const Vector2i min_region = ownerMin - Vector2i( context, context );
	const Vector2i max_region = ownerMax + Vector2i( context, context );
	const real_t   fMinX = ( real_t ) ( min_region.x * nRegionSize ) * s_fRegionVertexSpacing;
	const real_t   fMinZ = ( real_t ) ( min_region.y * nRegionSize ) * s_fRegionVertexSpacing;
	const real_t   fSpanX = ( real_t ) ( ( max_region.x - min_region.x + 1 ) * nRegionSize ) * s_fRegionVertexSpacing;
	const real_t   fSpanZ = ( real_t ) ( ( max_region.y - min_region.y + 1 ) * nRegionSize ) * s_fRegionVertexSpacing;
	const AABB	   bounds( Vector3( fMinX, -1000000.0f, fMinZ ), Vector3( fSpanX, 2000000.0f, fSpanZ ) );

	PackedVector3Array vertices;
	PackedVector2Array terrain_uvs;
	generateTriangles( vertices, &terrain_uvs, lod, HEIGHT_FILTER_NEAREST, false, bounds );
	ERR_FAIL_COND_V( vertices.is_empty() || vertices.size() != terrain_uvs.size(), pResult );

	Ref< SurfaceTool > pSt;
	pSt.instantiate();
	pSt->begin( Mesh::PRIMITIVE_TRIANGLES );
	for ( int i = 0; i < vertices.size(); ++i ) {
		const Vector3 vertex = vertices[i];
		pSt->set_uv( terrain_uvs[i] );
		pSt->set_uv2( Vector2( ( vertex.x - fMinX ) / fSpanX, ( vertex.z - fMinZ ) / fSpanZ ) );
		pSt->add_vertex( vertex );
	}
	pSt->index();
	pSt->generate_normals();
	pSt->generate_tangents();
	pSt->optimize_indices_for_cache();
	pResult = pSt->commit();
	if ( pResult.is_valid() ) {
		const int lightmap_width = CLAMP( ( int ) ( Math::ceil( fSpanX / fTexelSize ) ), 16, 16384 );
		const int lightmap_height = CLAMP( ( int ) ( Math::ceil( fSpanZ / fTexelSize ) ), 16, 16384 );
		pResult->set_lightmap_size_hint( Vector2i( lightmap_width, lightmap_height ) );
	}
	return pResult;
}

Ref< ArrayOccluder3D > Terrain3D::bakeOccluder(
	const int nLod ) const {
	LOG( INFO, "Baking occluder at lod: ", nLod );
	Ref< ArrayOccluder3D > pResult;
	ERR_FAIL_COND_V( m_pData == NULL, pResult );

	PackedVector3Array vertices;
	generateTriangles( vertices, NULL, nLod, HEIGHT_FILTER_MINIMUM, false, AABB() );
	ERR_FAIL_COND_V( vertices.is_empty(), pResult );

	Ref< SurfaceTool > pSt;
	pSt.instantiate();
	pSt->begin( Mesh::PRIMITIVE_TRIANGLES );
	for ( int i = 0; i < vertices.size(); ++i ) {
		pSt->add_vertex( vertices[i] );
	}
	pSt->index();
	pSt->optimize_indices_for_cache();

	const Array arrays = pSt->commit_to_arrays();
	ERR_FAIL_COND_V( arrays.size() <= Mesh::ARRAY_INDEX, pResult );
	const PackedVector3Array indexed_vertices = arrays[Mesh::ARRAY_VERTEX];
	const PackedInt32Array	 indices = arrays[Mesh::ARRAY_INDEX];
	ERR_FAIL_COND_V( indexed_vertices.is_empty() || indices.is_empty(), pResult );

	pResult.instantiate();
	pResult->set_arrays( indexed_vertices, indices );
	return pResult;
}
