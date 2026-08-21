// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
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
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <utility>

#include "eastl_config.h"
#include "core/logger.h"
#include "terrain_3d.h"
#include "core/terrain_3d_integration_helpers.h"
#include "rendering/terrain_3d_compute.h"
#include "authoring/terrain_3d_editor.h"
#include "memory/terrain_3d_linear_arena.h"
#include "memory/terrain_3d_zone_allocator.h"
#include "lighting/terrain_3d_lightmap_streamer.h"
#include "hlod/terrain_3d_hlod_streamer.h"
#include "lighting/terrain_3d_lighting_policy.h"
#include "streaming/terrain_3d_node_streamer.h"
#include "simd/terrain_3d_parallel.h"
#include "streaming/terrain_3d_performance_profile.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_runtime_bake_codec.h"
#include "streaming/terrain_3d_streaming_policy.h"
#include "streaming/terrain_3d_upload_bridge.h"
#include "core/terrain_3d_util.h"


bool T3DTerrain::bakeRegionSnapshotLess( const t3dBakeRegionSnapshot_t &rA, const t3dBakeRegionSnapshot_t &rB ) {
	if ( rA.location.y != rB.location.y ) {
		return rA.location.y < rB.location.y;
	}
	return rA.location.x < rB.location.x;
}

t3dBakeSnapshot_t T3DTerrain::createBakeSnapshot( void ) const {
	t3dBakeSnapshot_t snapshot;
	snapshot.nRegionSize = ( int32_t )m_nRegionSize;
	snapshot.fVertexSpacing = m_fVertexSpacing;
	std::fill_n( snapshot.szRegionLookup, TERRAIN3D_MAX_REGION_SLOTS, ( int32_t ) ( -1 ) );

	const TypedArray<Vector2i> region_locations = m_pData->getRegionLocations();
	snapshot.regions.reserve( region_locations.size() );
	const int64_t expected_height_bytes =
		( int64_t ) ( snapshot.nRegionSize ) * ( int64_t ) ( snapshot.nRegionSize ) * ( int64_t ) ( sizeof( uint16_t ) );
	const int64_t expected_control_bytes =
		( int64_t ) ( snapshot.nRegionSize ) * ( int64_t ) ( snapshot.nRegionSize ) * ( int64_t ) ( sizeof( uint32_t ) );

	for ( const Vector2i regionLocation : region_locations ) {
		if ( !m_pData->ensureCpuRegionResident( regionLocation ) ) {
			LOG( ERROR, "Skipping terrain region that could not be paged for bake snapshot: ", regionLocation );
			continue;
		}
		T3DRegion *const pRegion = m_pData->getRegionPtr( regionLocation );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}

		const Ref<Image> pHeightMap = pRegion->getHeightMap();
		const Ref<Image> pControlMap = pRegion->getControlMap();
		if ( pHeightMap.is_null() || pHeightMap->get_format() != Image::FORMAT_R16 ||
			pHeightMap->get_width() != snapshot.nRegionSize || pHeightMap->get_height() != snapshot.nRegionSize ||
			( pControlMap.is_valid() && ( pControlMap->get_format() != Image::FORMAT_RF ||
				pControlMap->get_width() != snapshot.nRegionSize || pControlMap->get_height() != snapshot.nRegionSize ) ) ) {
			LOG( ERROR, "Skipping invalid terrain region during bake snapshot: ", regionLocation );
			continue;
		}

		t3dBakeRegionSnapshot_t region_snapshot;
		region_snapshot.location = regionLocation;
		region_snapshot.heightData = pHeightMap->get_data();
		if ( pControlMap.is_valid() ) {
			region_snapshot.controlData = pControlMap->get_data();
		}
		if ( region_snapshot.heightData.size() < expected_height_bytes ||
			( pControlMap.is_valid() && region_snapshot.controlData.size() < expected_control_bytes ) ) {
			LOG( ERROR, "Skipping terrain region with incomplete map data during bake snapshot: ", regionLocation );
			continue;
		}

		const int32_t map_index = T3DData::getRegionMapIndex( regionLocation );
		if ( map_index < 0 ) {
			continue;
		}
		snapshot.regions.push_back( std::move( region_snapshot ) );
	}

	// Keep adjacent terrain regions adjacent in the snapshot vector. Whole-terrain
	// bake tasks iterate this order, so spatial sorting improves reuse of neighboring
	// height/control payloads and makes worker task construction deterministic even
	// when the resource's active-region order reflects edit history.
	std::sort( snapshot.regions.begin(), snapshot.regions.end(), bakeRegionSnapshotLess );
	for ( size_t region_index = 0; region_index < snapshot.regions.size(); ++region_index ) {
		const int32_t map_index = T3DData::getRegionMapIndex( snapshot.regions[region_index].location );
		if ( map_index >= 0 ) {
			snapshot.szRegionLookup[map_index] = ( int32_t ) ( region_index );
		}
	}

	return snapshot;
}

void T3DTerrain::runBakeTaskRange( void *const pContext, const size_t nBegin, const size_t nEnd ) {
	t3dBakeTaskContext_t *context = ( t3dBakeTaskContext_t * ) ( pContext );
	for ( size_t task_index = nBegin; task_index < nEnd; ++task_index ) {
		const t3dBakeRowTask_t &task = ( *context->pTasks )[task_index];
		t3dTriangleBuffer_t &buffer = ( *context->pBuffers )[task_index];
		for ( int32_t z = task.zStart; z < task.zEnd; z += context->step ) {
			for ( int32_t x = task.xStart; x < task.xEnd; x += context->step ) {
				context->pTerrain->generateTrianglePair( buffer, context->bGenerateUvs, context->lod, context->filter,
					context->bRequireNav, x, z, context->pSnapshot );
			}
		}
	}
}

Vector3 T3DTerrain::getTriangleVertex(
	const t3dBakeSnapshot_t *pSnapshot, const int32_t nLod,
	const t3dHeightFilter_t filter, const int32_t iX, const int32_t iZ,
	const Vector3 &rPosition, uint32_t &nControl
) const {
	if ( pSnapshot ) {
		return pSnapshot->getMeshVertex( nLod, filter, iX, iZ, nControl );
	}
	const Vector3 vertex = m_pData->getMeshVertex( nLod, filter, rPosition );
	nControl = m_pData->getControl( rPosition );
	return vertex;
}

void T3DTerrain::generateTriangles(
	PackedVector3Array &rVertices, PackedVector2Array *const pUvs, const int32_t nLod,
	const t3dHeightFilter_t filter, const bool bRequireNav,
	const AABB &rGlobalAabb
) const {
	ERR_FAIL_COND( m_pData == nullptr );
	const int32_t step = 1 << CLAMP( nLod, 0, 8 );
	const bool generate_uvs = pUvs != nullptr;
	eastl::vector<t3dTriangleBuffer_t> buffers;
	T3DLinearArena bake_arena;
	T3DArenaBuffer<Vector3> bake_vertex_slab;
	T3DArenaBuffer<Vector2> bake_uv_slab;

	// Whole-terrain bakes are split into row chunks. This parallelizes even a single large region,
	// while keeping each worker's output isolated and preserving deterministic merge order.
	if ( !rGlobalAabb.has_volume() ) {


		const int32_t nRegionSize = ( int32_t )m_nRegionSize;
		const int32_t rows_per_task = 16;
		t3dBakeSnapshot_t snapshot = createBakeSnapshot();
		for ( t3dBakeRegionSnapshot_t &region : snapshot.regions ) {
			region.pHeightBytes = region.heightData.ptr();
			region.pControlBytes = region.controlData.is_empty() ? nullptr : region.controlData.ptr();
		}
		eastl::vector<t3dBakeRowTask_t> tasks;
		tasks.reserve( snapshot.regions.size() * MAX( 1, nRegionSize / ( step * rows_per_task ) ) );

		size_t total_output_capacity = 0u;
		for ( const t3dBakeRegionSnapshot_t &region : snapshot.regions ) {
			const Vector2i region_loc = region.location;
			const Vector2i region_pos = region_loc * nRegionSize;
			const int32_t region_z_end = region_pos.y + nRegionSize;
			for ( int32_t z = region_pos.y; z < region_z_end; z += step * rows_per_task ) {
				const int32_t z_end = MIN( z + step * rows_per_task, region_z_end );
				const size_t column_count = ( size_t ) ( ( nRegionSize + step - 1 ) / step );
				const size_t row_count = ( size_t ) ( ( z_end - z + step - 1 ) / step );
				ERR_FAIL_COND_MSG( column_count != 0u && row_count > std::numeric_limits<size_t>::max() / column_count,
					"Terrain bake task dimensions overflow size_t." );
				const size_t quad_count = column_count * row_count;
				ERR_FAIL_COND_MSG( quad_count > std::numeric_limits<size_t>::max() / 6u,
					"Terrain bake triangle capacity overflows size_t." );
				const size_t output_capacity = quad_count * 6u;
				// Isolate adjacent worker slabs on independent cacheline boundaries. The
				// merge uses each buffer's written size, so padding remains invisible.
				ERR_FAIL_COND_MSG( total_output_capacity > std::numeric_limits<size_t>::max() -
					( TERRAIN3D_BAKE_TASK_ALIGNMENT_ELEMENTS - 1u ),
					"Terrain bake task alignment overflows size_t." );
				total_output_capacity = TERRAIN3D_ALIGN_BAKE_TASK_OFFSET( total_output_capacity );
				ERR_FAIL_COND_MSG( output_capacity > std::numeric_limits<size_t>::max() - total_output_capacity,
					"Terrain bake output capacity overflows size_t." );
				tasks.push_back( {region_pos.x, region_pos.x + nRegionSize, z, z_end, total_output_capacity,
							 output_capacity} );
				total_output_capacity += output_capacity;
			}
		}

		T3DLinearArenaLayout bake_layout;
		const bool bLayoutValid = bake_layout.addArray<Vector3>( total_output_capacity ) &&
			( !generate_uvs || bake_layout.addArray<Vector2>( total_output_capacity ) );
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
			buffers[task_index].bindExternal( bake_vertex_slab.data() + task.iOutputOffset,
				generate_uvs ? bake_uv_slab.data() + task.iOutputOffset : nullptr, task.nOutputCapacity );
		}
		if ( !tasks.empty() ) {
#if defined( __EMSCRIPTEN__ ) && !defined( __EMSCRIPTEN_PTHREADS__ )
			const uint32_t worker_count = 1u;
#else
			const uint32_t hardware_threads = MAX( 1u, ( uint32_t ) ( MAX( 1, OS::get_singleton()->get_processor_count() ) ) );
			const uint32_t worker_count = std::min( {hardware_threads, ( uint32_t )tasks.size(), 32u} );
#endif
			const T3DParallelExecutor executor{( int32_t ) ( worker_count )};
			t3dBakeTaskContext_t task_context;
			task_context.pTerrain = this;
			task_context.pSnapshot = &snapshot;
			task_context.pTasks = &tasks;
			task_context.pBuffers = &buffers;
			task_context.step = step;
			task_context.lod = nLod;
			task_context.filter = filter;
			task_context.bGenerateUvs = generate_uvs;
			task_context.bRequireNav = bRequireNav;

			LOG( INFO, "Generating terrain bake geometry with up to ", worker_count, " persistent worker tasks across ",
				tasks.size(), " chunks" );
			executor.run( tasks.size(), 1u, runBakeTaskRange, &task_context );
		}

		for ( const t3dTriangleBuffer_t &buffer : buffers ) {
			ERR_FAIL_COND_MSG( buffer.bOverflowed,
				"Terrain bake output exceeded its precomputed slab capacity. This indicates a bake sizing invariant failure." );
		}
	} else {
		// AABB generation is primarily used by runtime navigation baking. Keep it serial because these
		// requests are generally small and thread setup costs can exceed the geometry work.
		t3dTriangleBuffer_t buffer;
		const int32_t z_start = ( int32_t )Math::ceil( rGlobalAabb.position.z / m_fVertexSpacing );
		const int32_t z_end = ( int32_t )Math::floor( rGlobalAabb.get_end().z / m_fVertexSpacing ) + 1;
		const int32_t x_start = ( int32_t )Math::ceil( rGlobalAabb.position.x / m_fVertexSpacing );
		const int32_t x_end = ( int32_t )Math::floor( rGlobalAabb.get_end().x / m_fVertexSpacing ) + 1;
		const size_t column_count = ( size_t ) ( MAX( 0, x_end - x_start + step - 1 ) / step );
		const size_t row_count = ( size_t ) ( MAX( 0, z_end - z_start + step - 1 ) / step );
		buffer.reserve( column_count * row_count * 6u, generate_uvs );

		for ( int32_t z = z_start; z < z_end; z += step ) {
			for ( int32_t x = x_start; x < x_end; x += step ) {
				const real_t fHeight =
					m_pData->getHeight( Vector3( ( real_t ) ( x ) * m_fVertexSpacing, 0.f, ( real_t ) ( z ) * m_fVertexSpacing ) );
				if ( fHeight >= rGlobalAabb.position.y && fHeight <= rGlobalAabb.get_end().y ) {
					generateTrianglePair( buffer, generate_uvs, nLod, filter, bRequireNav, x, z );
				}
			}
		}
		buffers.push_back( std::move( buffer ) );
	}

	size_t vertex_count = 0;
	size_t uv_count = 0;
	for ( const t3dTriangleBuffer_t &buffer : buffers ) {
		vertex_count += buffer.size();
		uv_count += buffer.uvSize();
	}
	ERR_FAIL_COND( generate_uvs && vertex_count != uv_count );
	ERR_FAIL_COND( vertex_count > ( size_t ) ( std::numeric_limits<int32_t>::max() ) );

	rVertices.resize( ( int64_t )vertex_count );
	if ( vertex_count > 0 ) {
		Vector3 *const pVertexWrite = rVertices.ptrw();
		size_t vertex_offset = 0;
		for ( const t3dTriangleBuffer_t &buffer : buffers ) {
			if ( buffer.size() > 0u ) {
				std::copy_n( buffer.vertexData(), buffer.size(), pVertexWrite + vertex_offset );
			}
			vertex_offset += buffer.size();
		}
	}

	if ( generate_uvs ) {
		pUvs->resize( ( int64_t )uv_count );
		if ( uv_count > 0 ) {
			Vector2 *const pUvWrite = pUvs->ptrw();
			size_t uv_offset = 0;
			for ( const t3dTriangleBuffer_t &buffer : buffers ) {
				if ( buffer.uvSize() > 0u ) {
					std::copy_n( buffer.uvData(), buffer.uvSize(), pUvWrite + uv_offset );
				}
				uv_offset += buffer.uvSize();
			}
		}
	}
}

void T3DTerrain::generateTrianglePair(
	t3dTriangleBuffer_t &rBuffer, const bool bGenerateUvs, const int32_t nLod,
	const t3dHeightFilter_t filter, const bool bRequireNav,
	const int32_t x, const int32_t z, const t3dBakeSnapshot_t *pSnapshot
) const {
	const int32_t step = 1 << CLAMP( nLod, 0, 8 );
	const Vector3 xz = Vector3( x, 0.0f, z ) * m_fVertexSpacing;
	const Vector3 xsz = Vector3( x + step, 0.0f, z ) * m_fVertexSpacing;
	const Vector3 xzs = Vector3( x, 0.0f, z + step ) * m_fVertexSpacing;
	const Vector3 xszs = Vector3( x + step, 0.0f, z + step ) * m_fVertexSpacing;
	uint32_t ctrl1 = UINT32_MAX;
	uint32_t ctrl2 = UINT32_MAX;
	uint32_t ctrl3 = UINT32_MAX;
	uint32_t ctrl4 = UINT32_MAX;
	const Vector3 v1 = getTriangleVertex( pSnapshot, nLod, filter, x, z, xz, ctrl1 );
	const bool bNan1 = std::isnan( v1.y );
	if ( bNan1 ) {
		return;
	}
	Vector3 v2 = getTriangleVertex( pSnapshot, nLod, filter, x + step, z, xsz, ctrl2 );
	Vector3 v3 = getTriangleVertex( pSnapshot, nLod, filter, x, z + step, xzs, ctrl3 );
	Vector3 v4 = getTriangleVertex( pSnapshot, nLod, filter, x + step, z + step, xszs, ctrl4 );
	const bool bNan2 = std::isnan( v2.y );
	const bool bNan3 = std::isnan( v3.y );
	const bool bNan4 = std::isnan( v4.y );
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
	const bool bHole1 = ctrl1 != UINT32_MAX && isHole( ctrl1 );
	const bool bHole2 = ctrl2 != UINT32_MAX && isHole( ctrl2 );
	const bool bHole3 = ctrl3 != UINT32_MAX && isHole( ctrl3 );
	const bool bHole4 = ctrl4 != UINT32_MAX && isHole( ctrl4 );
	const bool bNav1 = ctrl1 != UINT32_MAX && isNav( ctrl1 );
	const bool bNav2 = ( ctrl2 != UINT32_MAX && isNav( ctrl2 ) ) || ( bNan2 && bNav1 );
	const bool bNav3 = ( ctrl3 != UINT32_MAX && isNav( ctrl3 ) ) || ( bNan3 && bNav1 );
	const bool bNav4 = ( ctrl4 != UINT32_MAX && isNav( ctrl4 ) ) || ( bNan4 && bNav1 );

	if ( !( bHole1 || bHole4 || bHole3 ) && ( !bRequireNav || ( bNav1 && bNav4 && bNav3 ) ) ) {
		rBuffer.appendTriangle( v1, v4, v3, bGenerateUvs );
	}
	if ( !( bHole1 || bHole2 || bHole4 ) && ( !bRequireNav || ( bNav1 && bNav2 && bNav4 ) ) ) {
		rBuffer.appendTriangle( v1, v2, v4, bGenerateUvs );
	}
}

Dictionary T3DTerrain::getRaycastResult(
	const Vector3 &rSrcPos, const Vector3 &rDirection,
	const uint32_t nColMask, const bool bExcludeSelf
) const {
	if ( !m_bIsInsideWorld ) {
		return Dictionary();
	}
	PhysicsDirectSpaceState3D *pSpaceState = get_world_3d()->get_direct_space_state();
	if ( !pSpaceState ) {
		LOG( ERROR, "Invalid PhysicsDirectSpaceState3D" );
		return Dictionary();
	}
	Ref<PhysicsRayQueryParameters3D> pQuery =
		PhysicsRayQueryParameters3D::create( rSrcPos, rSrcPos + rDirection, nColMask );
	if ( m_pCollision && bExcludeSelf ) {
		pQuery->set_exclude( TypedArray<RID>( m_pCollision->getRid() ) );
	}
	return pSpaceState->intersect_ray( pQuery );
}

Ref<Mesh> T3DTerrain::bakeMesh( const int nLod, const t3dHeightFilter_t filter ) const {
	LOG( INFO, "Baking mesh at lod: ", nLod, " with filter: ", filter );
	Ref<Mesh> pResult;
	ERR_FAIL_COND_V( m_pData == nullptr, pResult );

	Ref<SurfaceTool> pSt;
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

Ref<Mesh> T3DTerrain::bakeRegionLightmapMesh(
	const Vector2i &rRegionLocation, const int nLod,
	const int nContextRadiusRegions, const real_t fTexelSize
) const {
	return bakeLightmapClusterMesh( rRegionLocation, rRegionLocation, nLod,
		nContextRadiusRegions, fTexelSize );
}

Ref<Mesh> T3DTerrain::bakeLightmapClusterMesh(
	const Vector2i &rRegionMin, const Vector2i &rRegionMax,
	const int nLod, const int nContextRadiusRegions, const real_t fTexelSize
) const {
	Ref<Mesh> pResult;
	ERR_FAIL_COND_V( m_pData == nullptr, pResult );
	ERR_FAIL_COND_V( fTexelSize <= 0.0f, pResult );

	const int lod = CLAMP( nLod, 0, 8 );
	const int context = CLAMP( nContextRadiusRegions, 0, 4 );
	const int nRegionSize = m_pData->getRegionSizeInternal();
	ERR_FAIL_COND_V( nRegionSize <= 0, pResult );
	const Vector2i ownerMin( MIN( rRegionMin.x, rRegionMax.x ), MIN( rRegionMin.y, rRegionMax.y ) );
	const Vector2i ownerMax( MAX( rRegionMin.x, rRegionMax.x ), MAX( rRegionMin.y, rRegionMax.y ) );
	const Vector2i min_region = ownerMin - Vector2i( context, context );
	const Vector2i max_region = ownerMax + Vector2i( context, context );
	const real_t fMinX = ( real_t ) ( min_region.x * nRegionSize ) * m_fVertexSpacing;
	const real_t fMinZ = ( real_t ) ( min_region.y * nRegionSize ) * m_fVertexSpacing;
	const real_t fSpanX = ( real_t ) ( ( max_region.x - min_region.x + 1 ) * nRegionSize ) * m_fVertexSpacing;
	const real_t fSpanZ = ( real_t ) ( ( max_region.y - min_region.y + 1 ) * nRegionSize ) * m_fVertexSpacing;
	const AABB bounds( Vector3( fMinX, -1000000.0f, fMinZ ), Vector3( fSpanX, 2000000.0f, fSpanZ ) );

	PackedVector3Array vertices;
	PackedVector2Array terrain_uvs;
	generateTriangles( vertices, &terrain_uvs, lod, HEIGHT_FILTER_NEAREST, false, bounds );
	ERR_FAIL_COND_V( vertices.is_empty() || vertices.size() != terrain_uvs.size(), pResult );

	Ref<SurfaceTool> pSt;
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

Ref<ArrayOccluder3D> T3DTerrain::bakeOccluder( const int nLod ) const {
	LOG( INFO, "Baking occluder at lod: ", nLod );
	Ref<ArrayOccluder3D> pResult;
	ERR_FAIL_COND_V( m_pData == nullptr, pResult );

	PackedVector3Array vertices;
	generateTriangles( vertices, nullptr, nLod, HEIGHT_FILTER_MINIMUM, false, AABB() );
	ERR_FAIL_COND_V( vertices.is_empty(), pResult );

	Ref<SurfaceTool> pSt;
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
	const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
	ERR_FAIL_COND_V( indexed_vertices.is_empty() || indices.is_empty(), pResult );

	pResult.instantiate();
	pResult->set_arrays( indexed_vertices, indices );
	return pResult;
}

PackedVector3Array T3DTerrain::generateNavMeshSourceGeometry(
	const AABB &rGlobalAabb,
	const bool bRequireNav
) const {
	LOG( INFO, "Generating NavMesh source geometry from terrain" );
	PackedVector3Array faces;
	const bool strict_cooked = m_pData && isGpuRegionCookedPagesEnabled() && !isEditorAuthoringModeInternal();
	if ( !strict_cooked ) {
		generateTriangles( faces, nullptr, 0, HEIGHT_FILTER_NEAREST, bRequireNav, rGlobalAabb );
		return faces;
	}

	const int region_size = ( int ) m_nRegionSize;
	const int expected_width = region_size + 1;
	const TypedArray<Vector2i> locations = m_pData->getRegionLocations();
	for ( const Vector2i location : locations ) {
		const real_t region_world = ( real_t ) region_size * m_fVertexSpacing;
		const real_t region_min_x = ( real_t ) location.x * region_world;
		const real_t region_min_z = ( real_t ) location.y * region_world;
		if ( rGlobalAabb.has_volume() ) {
			if ( region_min_x + region_world < rGlobalAabb.position.x || region_min_x > rGlobalAabb.get_end().x ||
				region_min_z + region_world < rGlobalAabb.position.z || region_min_z > rGlobalAabb.get_end().z ) {
				continue;
			}
		}
		PackedByteArray navigation_blob;
		PackedByteArray collision_blob;
		PackedByteArray minmax_blob;
		PackedByteArray *const pMinMaxBlob = rGlobalAabb.has_volume() ? &minmax_blob : nullptr;
		if ( !m_pData->loadCookedNavigationTileNow( location, navigation_blob, &collision_blob, pMinMaxBlob ) ) {
			LOG( ERROR, "Missing/invalid baked navigation tile for region ", location );
			continue;
		}
		PackedFloat32Array collision_heights; T3DNavigationMaskView navigation_view; T3DMinMaxLevelView minmax_view; int collision_width = 0;
		const bool use_minmax = pMinMaxBlob && T3DRuntimeBakeCodec::getMinMaxLevelView( minmax_blob, 16, minmax_view ) && minmax_view.nSourceSide == region_size && minmax_view.nLevelSide > 0;
		if ( !T3DRuntimeBakeCodec::getNavigationMaskView( navigation_blob, navigation_view ) ||
			!T3DRuntimeBakeCodec::decodeCollisionHeight( collision_blob, collision_heights, collision_width ) ||
			navigation_view.nWidth != expected_width || collision_width != expected_width || ( pMinMaxBlob && !use_minmax ) ) {
			LOG( ERROR, "Baked navigation/collision acceleration data does not match region ", location );
			continue;
		}
		auto height_at = [&]( const int x, const int z ) -> real_t {
			return ( real_t ) collision_heights[x * collision_width + ( collision_width - 1 - z )];
		};
		const real_t aabb_min_y = rGlobalAabb.position.y;
		const real_t aabb_max_y = rGlobalAabb.get_end().y;
		for ( int z = 0; z < region_size; ++z ) {
			for ( int x = 0; x < region_size; ++x ) {
				if ( use_minmax && x + 1 < region_size && z + 1 < region_size ) {
					const int coarse_x0 = x * minmax_view.nLevelSide / minmax_view.nSourceSide;
					const int coarse_z0 = z * minmax_view.nLevelSide / minmax_view.nSourceSide;
					const int coarse_x1 = ( x + 1 ) * minmax_view.nLevelSide / minmax_view.nSourceSide;
					const int coarse_z1 = ( z + 1 ) * minmax_view.nLevelSide / minmax_view.nSourceSide;
					if ( coarse_x0 == coarse_x1 && coarse_z0 == coarse_z1 ) {
						const Vector2 range = T3DRuntimeBakeCodec::getMinMaxRange( minmax_view, ( uint32_t ) ( coarse_z0 * minmax_view.nLevelSide + coarse_x0 ) );
						if ( range.y < aabb_min_y || range.x > aabb_max_y ) {
							continue;
						}
					}
				}
				const int nav_width = navigation_view.nWidth; const int i1 = z * nav_width + x; const int i2 = i1 + 1; const int i3 = ( z + 1 ) * nav_width + x; const int i4 = i3 + 1;
				const bool valid1 = navigation_view.isValidSample( ( uint32_t ) i1 ); const bool valid2 = navigation_view.isValidSample( ( uint32_t ) i2 ); const bool valid3 = navigation_view.isValidSample( ( uint32_t ) i3 ); const bool valid4 = navigation_view.isValidSample( ( uint32_t ) i4 );
				if ( !valid1 ) continue;
				const bool nav1 = navigation_view.isNavigationSample( ( uint32_t ) i1 ); const bool nav2 = navigation_view.isNavigationSample( ( uint32_t ) i2 ); const bool nav3 = navigation_view.isNavigationSample( ( uint32_t ) i3 ); const bool nav4 = navigation_view.isNavigationSample( ( uint32_t ) i4 );
				const int global_x = location.x * region_size + x;
				const int global_z = location.y * region_size + z;
				Vector3 v1( ( real_t ) global_x * m_fVertexSpacing, height_at( x, z ), ( real_t ) global_z * m_fVertexSpacing );
				Vector3 v2( ( real_t ) ( global_x + 1 ) * m_fVertexSpacing, height_at( x + 1, z ), ( real_t ) global_z * m_fVertexSpacing );
				Vector3 v3( ( real_t ) global_x * m_fVertexSpacing, height_at( x, z + 1 ), ( real_t ) ( global_z + 1 ) * m_fVertexSpacing );
				Vector3 v4( ( real_t ) ( global_x + 1 ) * m_fVertexSpacing, height_at( x + 1, z + 1 ), ( real_t ) ( global_z + 1 ) * m_fVertexSpacing );
				if ( rGlobalAabb.has_volume() && ( v1.y < rGlobalAabb.position.y || v1.y > rGlobalAabb.get_end().y ||
					v1.x < rGlobalAabb.position.x || v1.x > rGlobalAabb.get_end().x ||
					v1.z < rGlobalAabb.position.z || v1.z > rGlobalAabb.get_end().z ) ) continue;
				if ( std::isnan( v2.y ) ) v2.y = v1.y;
				if ( std::isnan( v3.y ) ) v3.y = v1.y;
				if ( std::isnan( v4.y ) ) v4.y = !std::isnan( v2.y ) ? v2.y : ( !std::isnan( v3.y ) ? v3.y : v1.y );
				if ( valid1 && valid4 && valid3 && ( !bRequireNav || ( nav1 && nav4 && nav3 ) ) ) {
					faces.push_back( v1 ); faces.push_back( v4 ); faces.push_back( v3 );
				}
				if ( valid1 && valid2 && valid4 && ( !bRequireNav || ( nav1 && nav2 && nav4 ) ) ) {
					faces.push_back( v1 ); faces.push_back( v2 ); faces.push_back( v4 );
				}
			}
		}
	}
	return faces;
}

