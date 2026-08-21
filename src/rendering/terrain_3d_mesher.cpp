// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include "eastl_config.h"
#include <EASTL/vector.h>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/version.hpp>

#include <algorithm>
#include <climits>

#include "core/logger.h"
#include "terrain_3d.h"
#include "rendering/terrain_3d_clipmap_bounds.h"
#include "rendering/terrain_3d_mesher.h"


static constexpr uint64_t s_nClipmapPackMagic = 0x314b504d504c4333ull; // 3CLMPPK1
static constexpr uint32_t s_nClipmapPackVersion = 1u;
static constexpr uint32_t s_nClipmapWeldPartCount = 5u;
EA_FORCE_INLINE static int t3dMesherGridVertexCount( const Vector2i &rSize ) { return ( rSize.x + 1 ) * ( rSize.y + 1 ); }
EA_FORCE_INLINE static int t3dMesherGridIndexCount( const Vector2i &rSize ) { return rSize.x * rSize.y * 6; }

void weldClipmapGeometry(
	PackedVector3Array &rVertices, PackedVector2Array &rMetadata,
	PackedInt32Array &rIndices
) {
	const int source_vertex_count = rVertices.size();
	if ( source_vertex_count <= 0 ) {
		return;
	}
	const Vector3 *const pSourceVertices = rVertices.ptr();
	const Vector2 *const pSourceMetadata = rMetadata.ptr();
	int32_t min_x = INT32_MAX;
	int32_t min_z = INT32_MAX;
	int32_t max_x = INT32_MIN;
	int32_t max_z = INT32_MIN;
	for ( int vertex_index = 0; vertex_index < source_vertex_count; ++vertex_index ) {
		const int32_t x = ( int32_t ) ( Math::round( pSourceVertices[vertex_index].x ) );
		const int32_t z = ( int32_t ) ( Math::round( pSourceVertices[vertex_index].z ) );
		min_x = MIN( min_x, x );
		min_z = MIN( min_z, z );
		max_x = MAX( max_x, x );
		max_z = MAX( max_z, z );
	}
	const uint64_t width = ( uint64_t ) ( ( int64_t ) ( max_x ) - ( int64_t ) ( min_x ) + 1ll );
	const uint64_t height = ( uint64_t ) ( ( int64_t ) ( max_z ) - ( int64_t ) ( min_z ) + 1ll );
	const uint64_t plane = width * height;
	const uint64_t lookup_count = plane * s_nClipmapWeldPartCount;
	if ( width == 0u || height == 0u || lookup_count > ( uint64_t ) ( INT32_MAX ) ) {
		return;
	}
	eastl::vector<int32_t> lookup;
	lookup.resize( ( size_t ) ( lookup_count ), -1 );
	PackedVector3Array welded_vertices;
	PackedVector2Array welded_metadata;
	PackedInt32Array remap;
	welded_vertices.resize( source_vertex_count );
	welded_metadata.resize( source_vertex_count );
	remap.resize( source_vertex_count );
	Vector3 *const pWeldedVertices = welded_vertices.ptrw();
	Vector2 *const pWeldedMetadata = welded_metadata.ptrw();
	int32_t *const pRemap = remap.ptrw();
	int welded_count = 0;
	for ( int vertex_index = 0; vertex_index < source_vertex_count; ++vertex_index ) {
		const Vector3 &vertex = pSourceVertices[vertex_index];
		const Vector2 &metadata = pSourceMetadata[vertex_index];
		const int32_t x = ( int32_t ) ( Math::round( vertex.x ) );
		const int32_t z = ( int32_t ) ( Math::round( vertex.z ) );
		const int32_t rounded_part = ( int32_t ) ( Math::round( metadata.x ) );
		if ( rounded_part < 0 || rounded_part >= ( int32_t ) s_nClipmapWeldPartCount ) return;
		const uint32_t part = ( uint32_t ) rounded_part;
		const uint64_t lookup_index = ( uint64_t ) part * plane +
			( uint64_t ) ( ( int64_t ) ( z ) - ( int64_t ) ( min_z ) ) * width +
			( uint64_t ) ( ( int64_t ) ( x ) - ( int64_t ) ( min_x ) );
		int32_t &welded_index = lookup[( size_t ) ( lookup_index )];
		if ( welded_index >= 0 ) {
			pRemap[vertex_index] = welded_index;
			continue;
		}
		welded_index = welded_count++;
		pWeldedVertices[welded_index] = vertex;
		pWeldedMetadata[welded_index] = metadata;
		pRemap[vertex_index] = welded_index;
	}
	int32_t *const pIndices = rIndices.ptrw();
	for ( int index = 0; index < rIndices.size(); ++index ) {
		pIndices[index] = pRemap[pIndices[index]];
	}
	welded_vertices.resize( welded_count );
	welded_metadata.resize( welded_count );
	rVertices = welded_vertices;
	rMetadata = welded_metadata;
}

///////////////////////////
// Private Functions
///////////////////////////

void T3DMesher::appendGrid(
	PackedVector3Array &rVertices, PackedVector2Array &rMetadata, PackedInt32Array &rIndices,
	int &nVertexWrite, int &nIndexWrite, const Vector2i &rSize, const Vector3 &rOrigin,
	const float fPartId, const bool bStandardGrid
) {
	const int vertex_base = nVertexWrite;
	Vector3 *const pVertices = rVertices.ptrw();
	Vector2 *const pMetadata = rMetadata.ptrw();
	int32_t *const pIndices = rIndices.ptrw();
	int vertex_write = nVertexWrite;
	for ( int y = 0; y <= rSize.y; ++y ) {
		for ( int x = 0; x <= rSize.x; ++x ) {
			pVertices[vertex_write] = rOrigin + Vector3( x, 0.0f, y );
			pMetadata[vertex_write++] = Vector2( fPartId, 0.0f );
		}
	}

	int index_write = nIndexWrite;
	for ( int y = 0; y < rSize.y; ++y ) {
		for ( int x = 0; x < rSize.x; ++x ) {
			const int bottom_left = vertex_base + y * ( rSize.x + 1 ) + x;
			const int bottom_right = bottom_left + 1;
			const int top_left = vertex_base + ( y + 1 ) * ( rSize.x + 1 ) + x;
			const int top_right = top_left + 1;

			if ( ( x + y ) % 2 == 0 || bStandardGrid ) {
				pIndices[index_write++] = bottom_left;
				pIndices[index_write++] = top_right;
				pIndices[index_write++] = top_left;
				pIndices[index_write++] = bottom_left;
				pIndices[index_write++] = bottom_right;
				pIndices[index_write++] = top_right;
			} else {
				pIndices[index_write++] = bottom_left;
				pIndices[index_write++] = bottom_right;
				pIndices[index_write++] = top_left;
				pIndices[index_write++] = top_left;
				pIndices[index_write++] = bottom_right;
				pIndices[index_write++] = top_right;
			}
		}
	}
	nVertexWrite = vertex_write;
	nIndexWrite = index_write;
}

t3dClipmapGeometry_t T3DMesher::generateCombinedGeometry( const bool bLod0 ) {
	t3dClipmapGeometry_t geometry;
	PackedVector3Array &vertices = geometry.m_vertices;
	PackedVector2Array &metadata = geometry.m_metadata;
	PackedInt32Array &indices = geometry.m_indices;

	int total_vertices = 0;
	int total_indices = 0;
	auto measure_grid = [&]( const Vector2i &rSize ) { total_vertices += t3dMesherGridVertexCount( rSize ); total_indices += t3dMesherGridIndexCount( rSize ); };
	if ( bLod0 ) {
		for ( const Vector3 &position : m_szTilePosLod0 ) { ( void ) position; measure_grid( V2I( m_nMeshSize ) ); }
		for ( const Vector3 &position : m_szTrimAPos ) { ( void ) position; measure_grid( Vector2i( 2, m_nMeshSize * 4 + 2 ) ); }
		for ( const Vector3 &position : m_szTrimBPos ) { ( void ) position; measure_grid( Vector2i( m_nMeshSize * 4 + 2, 2 ) ); }
	} else {
		for ( const Vector3 &position : m_szTilePos ) { ( void ) position; measure_grid( V2I( m_nMeshSize ) ); }
		for ( const Vector3 &position : m_szFillAPos ) { ( void ) position; measure_grid( Vector2i( 4, m_nMeshSize ) ); }
		for ( const Vector3 &position : m_szFillBPos ) { ( void ) position; measure_grid( Vector2i( m_nMeshSize, 4 ) ); }
	}
	for ( int i = 0; i < s_nPairInstances; ++i ) { ( void ) i; measure_grid( Vector2i( 2, m_nMeshSize * 4 + 8 ) ); measure_grid( Vector2i( m_nMeshSize * 4 + 4, 2 ) ); }
	vertices.resize( total_vertices );
	metadata.resize( total_vertices );
	indices.resize( total_indices );
	int vertex_write = 0;
	int index_write = 0;

	if ( bLod0 ) {
		for ( const Vector3 &position : m_szTilePosLod0 ) appendGrid( vertices, metadata, indices, vertex_write, index_write, V2I( m_nMeshSize ), position, s_fClipmapPartStatic, true );
		for ( const Vector3 &position : m_szTrimAPos ) appendGrid( vertices, metadata, indices, vertex_write, index_write, Vector2i( 2, m_nMeshSize * 4 + 2 ), position, s_fClipmapPartStatic, true );
		for ( const Vector3 &position : m_szTrimBPos ) appendGrid( vertices, metadata, indices, vertex_write, index_write, Vector2i( m_nMeshSize * 4 + 2, 2 ), position, s_fClipmapPartStatic, true );
	} else {
		for ( const Vector3 &position : m_szTilePos ) appendGrid( vertices, metadata, indices, vertex_write, index_write, V2I( m_nMeshSize ), position, s_fClipmapPartStatic, false );
		for ( const Vector3 &position : m_szFillAPos ) appendGrid( vertices, metadata, indices, vertex_write, index_write, Vector2i( 4, m_nMeshSize ), position, s_fClipmapPartStatic, false );
		for ( const Vector3 &position : m_szFillBPos ) appendGrid( vertices, metadata, indices, vertex_write, index_write, Vector2i( m_nMeshSize, 4 ), position, s_fClipmapPartStatic, false );
	}

	// Edge placement is baked for stitch state (1, 1). INSTANCE_CUSTOM.rg
	// carries the live 3x3 stitch state and the vertex shader applies only the
	// small edge translation delta. This keeps one immutable complete-ring mesh
	// reusable by every LOD in a shader tier.
	for ( int i = 0; i < s_nPairInstances; ++i ) {
		Vector3 edge_a;
		edge_a.z = -m_fOffsetA - 2.0f;
		edge_a.x = m_szEdgePos[( size_t ) i][1];
		appendGrid( vertices, metadata, indices, vertex_write, index_write, Vector2i( 2, m_nMeshSize * 4 + 8 ), edge_a, i == 0 ? s_fClipmapPartEdgeA0 : s_fClipmapPartEdgeA1, bLod0 );

		Vector3 edge_b;
		edge_b.z = m_szEdgePos[( size_t ) i][1];
		edge_b.x = -m_fOffsetA;
		appendGrid( vertices, metadata, indices, vertex_write, index_write, Vector2i( m_nMeshSize * 4 + 4, 2 ), edge_b, i == 0 ? s_fClipmapPartEdgeB0 : s_fClipmapPartEdgeB1, bLod0 );
	}
	if ( vertex_write != total_vertices || index_write != total_indices ) { LOG( ERROR, "Clipmap geometry preallocation count mismatch" ); return t3dClipmapGeometry_t{}; }

	const int source_vertex_count = vertices.size();
	weldClipmapGeometry( vertices, metadata, indices );
	const real_t fRadius = m_fOffsetC + 2.0f;
	geometry.m_aabb = AABB( Vector3( -fRadius, -0.1f, -fRadius ), Vector3( fRadius * 2.0f, 0.2f, fRadius * 2.0f ) );
	LOG( INFO, "Generated ", bLod0 ? "LOD0" : "outer-ring", " clipmap geometry: ", vertices.size(),
		" welded vertices from ", source_vertex_count, " source vertices, ", indices.size() / 3, " triangles" );
	return geometry;
}

bool T3DMesher::loadRuntimeGeometryPack( const String &rDataDirectory ) {
	if ( rDataDirectory.is_empty() ) {
		return false;
	}
	const String path = rDataDirectory.path_join( "terrain3d_clipmap.t3dmeshpack" );
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::READ );
	if ( file.is_null() || file->get_length() < 24 ) {
		return false;
	}
	if ( file->get_64() != s_nClipmapPackMagic ||
		file->get_32() != s_nClipmapPackVersion ||
		( int ) file->get_32() != m_nMeshSize || file->get_32() != MESH_TYPE_COUNT ) {
		return false;
	}
	file->get_32();
	for ( int type = 0; type < MESH_TYPE_COUNT; ++type ) {
		t3dClipmapGeometry_t geometry;
		const uint32_t vertex_count = file->get_32();
		const uint32_t metadata_count = file->get_32();
		const uint32_t index_count = file->get_32();
		file->get_32();
		if ( vertex_count == 0u || metadata_count != vertex_count || index_count == 0u ||
			vertex_count > 4000000u || index_count > 12000000u ) {
			return false;
		}
		geometry.m_aabb.position = Vector3( file->get_float(), file->get_float(), file->get_float() );
		geometry.m_aabb.size = Vector3( file->get_float(), file->get_float(), file->get_float() );
		geometry.m_vertices.resize( ( int ) vertex_count );
		geometry.m_metadata.resize( ( int ) metadata_count );
		geometry.m_indices.resize( ( int ) index_count );
		Vector3 *const pVertices = geometry.m_vertices.ptrw();
		Vector2 *const pMetadata = geometry.m_metadata.ptrw();
		int32_t *const pIndices = geometry.m_indices.ptrw();
		for ( uint32_t i = 0; i < vertex_count; ++i ) {
			pVertices[i] = Vector3( file->get_float(), file->get_float(), file->get_float() );
		}
		for ( uint32_t i = 0; i < metadata_count; ++i ) {
			pMetadata[i] = Vector2( file->get_float(), file->get_float() );
		}
		for ( uint32_t i = 0; i < index_count; ++i ) {
			const int32_t index = ( int32_t ) file->get_32();
			if ( index < 0 || ( uint32_t ) index >= vertex_count ) {
				return false;
			}
			pIndices[i] = index;
		}
		if ( file->get_error() != OK ) {
			return false;
		}
		m_szMeshGeometry[( size_t ) type] = geometry;
	}
	if ( file->get_position() != file->get_length() ) {
		return false;
	}
	if ( !m_bStateMeshBatching ) {
		for ( int type = 0; type < MESH_TYPE_COUNT; ++type ) {
			const t3dClipmapGeometry_t &geometry = m_szMeshGeometry[( size_t ) type];
			m_szMeshRids[( size_t ) type] = instantiateMesh(
				geometry.m_vertices, geometry.m_metadata, geometry.m_indices, geometry.m_aabb );
		}
	}
	LOG( INFO, "Loaded cooked clipmap geometry pack for mesh size ", m_nMeshSize );
	return true;
}

bool T3DMesher::writeRuntimeGeometryPack( const String &rDataDirectory ) const {
	if ( rDataDirectory.is_empty() || m_nMeshSize <= 0 ) {
		return false;
	}
	for ( const t3dClipmapGeometry_t &geometry : m_szMeshGeometry ) {
		if ( geometry.m_vertices.is_empty() || geometry.m_metadata.size() != geometry.m_vertices.size() ||
			geometry.m_indices.is_empty() ) {
			return false;
		}
	}
	const String path = rDataDirectory.path_join( "terrain3d_clipmap.t3dmeshpack" );
	const String temporary = path + ".tmp";
	Ref<FileAccess> file = FileAccess::open( temporary, FileAccess::WRITE );
	if ( file.is_null() ) {
		return false;
	}
	file->store_64( s_nClipmapPackMagic );
	file->store_32( s_nClipmapPackVersion );
	file->store_32( ( uint32_t ) m_nMeshSize );
	file->store_32( MESH_TYPE_COUNT );
	file->store_32( 0u );
	for ( const t3dClipmapGeometry_t &geometry : m_szMeshGeometry ) {
		file->store_32( ( uint32_t ) geometry.m_vertices.size() );
		file->store_32( ( uint32_t ) geometry.m_metadata.size() );
		file->store_32( ( uint32_t ) geometry.m_indices.size() );
		file->store_32( 0u );
		file->store_float( geometry.m_aabb.position.x );
		file->store_float( geometry.m_aabb.position.y );
		file->store_float( geometry.m_aabb.position.z );
		file->store_float( geometry.m_aabb.size.x );
		file->store_float( geometry.m_aabb.size.y );
		file->store_float( geometry.m_aabb.size.z );
		for ( const Vector3 &vertex : geometry.m_vertices ) {
			file->store_float( vertex.x ); file->store_float( vertex.y ); file->store_float( vertex.z );
		}
		for ( const Vector2 &metadata : geometry.m_metadata ) {
			file->store_float( metadata.x ); file->store_float( metadata.y );
		}
		for ( const int32_t index : geometry.m_indices ) {
			file->store_32( ( uint32_t ) index );
		}
	}
	const Error error = file->get_error();
	file->close();
	if ( error != OK ) {
		DirAccess::remove_absolute( temporary );
		return false;
	}
	const String backup = path + ".bak";
	DirAccess::remove_absolute( backup );
	if ( FileAccess::file_exists( path ) && DirAccess::rename_absolute( path, backup ) != OK ) {
		DirAccess::remove_absolute( temporary );
		return false;
	}
	if ( DirAccess::rename_absolute( temporary, path ) != OK ) {
		if ( FileAccess::file_exists( backup ) ) {
			DirAccess::rename_absolute( backup, path );
		}
		return false;
	}
	DirAccess::remove_absolute( backup );
	return true;
}

void T3DMesher::generateMeshTypes( void ) {
	clearMeshTypes();
	LOG( INFO, "Generating two immutable complete clipmap geometry templates for size ", m_nMeshSize );
	m_szMeshGeometry[LOD0_MESH] = generateCombinedGeometry( true );
	m_szMeshGeometry[OUTER_RING_MESH] = generateCombinedGeometry( false );
	if ( !m_bStateMeshBatching ) {
		for ( int type = 0; type < MESH_TYPE_COUNT; ++type ) {
			const t3dClipmapGeometry_t &geometry = m_szMeshGeometry[( size_t ) ( type )];
			m_szMeshRids[( size_t ) ( type )] = instantiateMesh( geometry.m_vertices, geometry.m_metadata, geometry.m_indices, geometry.m_aabb );
		}
	}
}

RID T3DMesher::instantiateMesh(
	const PackedVector3Array &rVertices, const PackedVector2Array &rMetadata,
	const PackedInt32Array &rIndices, const AABB &rAabb, const bool bIncludeBasis
) {
	Array arrays;
	arrays.resize( RenderingServer::ARRAY_MAX );
	arrays[RenderingServer::ARRAY_VERTEX] = rVertices;
	arrays[RenderingServer::ARRAY_TEX_UV2] = rMetadata;
	arrays[RenderingServer::ARRAY_INDEX] = rIndices;

	if ( bIncludeBasis ) {
		PackedVector3Array normals;
		normals.resize( rVertices.size() );
		normals.fill( V3_UP );
		arrays[RenderingServer::ARRAY_NORMAL] = normals;

		PackedFloat32Array tangents;
		tangents.resize( rVertices.size() * 4 );
		tangents.fill( 0.0f );
		arrays[RenderingServer::ARRAY_TANGENT] = tangents;
	}

	const RID mesh = RS->mesh_create();
	RS->mesh_add_surface_from_arrays( mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays );
	RS->mesh_set_custom_aabb( mesh, rAabb );
	RS->mesh_surface_set_material( mesh, 0, m_szMaterials[0].is_valid() ? m_szMaterials[0] : RID() );
	return mesh;
}

RID T3DMesher::instantiateStateMesh( const int nFirstLod, const int nLastLod ) {
	int total_vertices = 0;
	int total_indices = 0;
	for ( int lod = nFirstLod; lod <= nLastLod; ++lod ) {
		const t3dClipmapGeometry_t &geometry = m_szMeshGeometry[( size_t ) ( lod == 0 ? LOD0_MESH : OUTER_RING_MESH )];
		total_vertices += geometry.m_vertices.size();
		total_indices += geometry.m_indices.size();
	}
	PackedVector3Array vertices;
	PackedVector2Array metadata;
	PackedInt32Array indices;
	vertices.resize( total_vertices );
	metadata.resize( total_vertices );
	indices.resize( total_indices );
	Vector3 *const pVertices = vertices.ptrw();
	Vector2 *const pMetadata = metadata.ptrw();
	int32_t *const pIndices = indices.ptrw();
	int vertex_write = 0;
	int index_write = 0;
	for ( int lod = nFirstLod; lod <= nLastLod; ++lod ) {
		const t3dClipmapGeometry_t &geometry = m_szMeshGeometry[( size_t ) ( lod == 0 ? LOD0_MESH : OUTER_RING_MESH )];
		const int vertex_base = vertex_write;
		const Vector3 *const pSourceVertices = geometry.m_vertices.ptr();
		const Vector2 *const pSourceMetadata = geometry.m_metadata.ptr();
		for ( int vertex_index = 0; vertex_index < geometry.m_vertices.size(); ++vertex_index ) {
			pVertices[vertex_write] = pSourceVertices[vertex_index];
			pMetadata[vertex_write++] = Vector2( pSourceMetadata[vertex_index].x, ( float ) ( lod ) );
		}
		const int32_t *const pSourceIndices = geometry.m_indices.ptr();
		for ( int index = 0; index < geometry.m_indices.size(); ++index ) {
			pIndices[index_write++] = vertex_base + pSourceIndices[index];
		}
	}
	const int highest_lod = MAX( nFirstLod, nLastLod );
	const real_t fDensity = ( real_t ) ( 1.0 ) / ( real_t ) ( ( uint64_t ) ( 1 ) << CLAMP( m_tessellationLevel, 0, 30 ) );
	const real_t fScale = m_fVertexSpacing * fDensity * ( real_t ) ( ( uint64_t ) ( 1 ) << CLAMP( highest_lod, 0, 30 ) );
	const real_t fRadius = ( m_fOffsetC + 2.0f ) * fScale * 2.0f;
	const AABB initialAabb(
		Vector3( -fRadius, m_fClipmapAabbMinY, -fRadius ),
		Vector3( fRadius * 2.0f, m_fClipmapAabbSizeY, fRadius * 2.0f )
	);
	LOG( INFO, "Generated state clipmap mesh for LODs ", nFirstLod, "-", nLastLod, ": ", vertices.size(),
		" vertices, ", indices.size() / 3, " triangles" );
	// Built-in terrain shaders derive their geometric normal/tangent basis from
	// terrain data in fragment shading, so state meshes do not need static basis
	// attributes. Fallback/custom meshes retain them for compatibility.
	return instantiateMesh( vertices, metadata, indices, initialAabb, false );
}

size_t T3DMesher::estimateGeometryPayloadBytes(
	const int nFirstLod, const int nLastLod,
	const bool bIncludeBasis
) const {
	size_t bytes = 0u;
	for ( int lod = nFirstLod; lod <= nLastLod; ++lod ) {
		const t3dClipmapGeometry_t &geometry = m_szMeshGeometry[( size_t ) ( lod == 0 ? LOD0_MESH : OUTER_RING_MESH )];
		const size_t vertex_count = ( size_t ) ( geometry.m_vertices.size() );
		bytes += vertex_count * ( sizeof( float ) * ( 3u + 2u ) );
		if ( bIncludeBasis ) {
			bytes += vertex_count * ( sizeof( float ) * ( 3u + 4u ) );
		}
		bytes += ( size_t ) ( geometry.m_indices.size() ) * sizeof( int32_t );
	}
	return bytes;
}

size_t T3DMesher::getVisibleGeometryPayloadBytes( void ) const {
	if ( !m_bStateMeshBatching ) {
		return estimateGeometryPayloadBytes( 0, 1, true );
	}
	size_t bytes = 0u;
	for ( int batch_index = 0; batch_index < m_nRenderBatchCount; ++batch_index ) {
		const t3dClipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( batch_index )];
		if ( batch.m_bOwnsMesh ) {
			bytes += estimateGeometryPayloadBytes( batch.m_nFirstLod, batch.m_nLastLod, false );
		}
	}
	return bytes;
}

size_t T3DMesher::getShadowGeometryPayloadBytes( void ) const {
	size_t bytes = 0u;
	for ( int index = 0; index < m_nShadowBatchCount; ++index ) {
		const t3dClipmapBatch_t &batch = m_szShadowStateBatches[( size_t ) ( index )];
		if ( batch.m_bOwnsMesh ) {
			bytes += estimateGeometryPayloadBytes( batch.m_nFirstLod, batch.m_nLastLod, false );
		}
	}
	return bytes;
}

size_t T3DMesher::getStreamingBufferBytes( void ) const {
	size_t bytes = ( size_t ) ( m_lodStateBuffer.size() ) * sizeof( float ) * 4u;
	for ( int batch_index = 0; batch_index < m_nRenderBatchCount; ++batch_index ) {
		bytes += ( size_t ) ( m_szRenderBatches[( size_t ) ( batch_index )].m_transformBuffer.size() ) * sizeof( float );
	}
	for ( int index = 0; index < m_nShadowBatchCount; ++index ) {
		bytes += ( size_t ) ( m_szShadowStateBatches[( size_t ) ( index )].m_transformBuffer.size() ) * sizeof( float );
	}
	return bytes;
}

t3dClipmapBatch_t T3DMesher::createBatch(
	const RID &rMesh, const int nCount,
	const int nFirstLod, const int nLastLod, const int nTier, const bool bCastsShadows
) {
	t3dClipmapBatch_t batch;
	if ( !rMesh.is_valid() || nCount <= 0 || nCount > s_nMaxBatchInstances ) {
		LOG( ERROR, "Invalid clipmap render batch request. Mesh valid: ", rMesh.is_valid(), ", count: ", nCount );
		return batch;
	}

	batch.m_hMultimesh = RS->multimesh_create();
	if ( !batch.m_hMultimesh.is_valid() ) {
		LOG( ERROR, "Failed to create clipmap MultiMesh" );
		return batch;
	}
	RS->multimesh_allocate_data( batch.m_hMultimesh, nCount, RenderingServer::MULTIMESH_TRANSFORM_3D,
		false, true, false );
	RS->multimesh_set_mesh( batch.m_hMultimesh, rMesh );
	RS->multimesh_set_visible_instances( batch.m_hMultimesh, nCount );
	batch.m_transformBuffer.resize( ( int64_t ) ( nCount ) * s_nMultimeshStrideFloats );
	batch.m_transformBuffer.fill( 0.0f );
	batch.m_hInstance = RS->instance_create2( batch.m_hMultimesh, m_hScenario );
	if ( !batch.m_hInstance.is_valid() ) {
		LOG( ERROR, "Failed to create clipmap MultiMesh instance" );
		RS->free_rid( batch.m_hMultimesh );
		batch.m_hMultimesh = RID();
		return batch;
	}
	batch.m_nCount = ( uint8_t ) ( nCount );
	batch.m_nFirstLod = ( uint8_t ) ( nFirstLod );
	batch.m_nLastLod = ( uint8_t ) ( nLastLod );
	batch.m_nTier = ( uint8_t ) ( nTier );
	batch.m_bCastsShadows = bCastsShadows;
	return batch;
}

t3dClipmapBatch_t T3DMesher::createStateBatch(
	const int nFirstLod,
	const int nLastLod, const int nTier, const bool bCastsShadows
) {
	t3dClipmapBatch_t batch;
	if ( nFirstLod < 0 || nLastLod < nFirstLod || nLastLod >= m_nClipmapLevelCount ) {
		LOG( ERROR, "Invalid state clipmap batch range: ", nFirstLod, "-", nLastLod );
		return batch;
	}
	batch.m_hMesh = instantiateStateMesh( nFirstLod, nLastLod );
	batch.m_bOwnsMesh = true;
	if ( !batch.m_hMesh.is_valid() ) {
		return batch;
	}
	batch.m_hInstance = RS->instance_create2( batch.m_hMesh, m_hScenario );
	if ( !batch.m_hInstance.is_valid() ) {
		RS->free_rid( batch.m_hMesh );
		batch.m_hMesh = RID();
		return batch;
	}
	batch.m_nCount = ( uint8_t ) ( nLastLod - nFirstLod + 1 );
	batch.m_nFirstLod = ( uint8_t ) ( nFirstLod );
	batch.m_nLastLod = ( uint8_t ) ( nLastLod );
	batch.m_nTier = ( uint8_t ) ( nTier );
	batch.m_bCastsShadows = bCastsShadows;
	return batch;
}

void T3DMesher::freeBatch( t3dClipmapBatch_t &rBatch ) {
	if ( rBatch.m_hInstance.is_valid() ) {
		RS->free_rid( rBatch.m_hInstance );
	}
	if ( rBatch.m_hMultimesh.is_valid() ) {
		RS->free_rid( rBatch.m_hMultimesh );
	}
	if ( rBatch.m_bOwnsMesh && rBatch.m_hMesh.is_valid() ) {
		RS->free_rid( rBatch.m_hMesh );
	}
	rBatch = t3dClipmapBatch_t{};
}

void T3DMesher::setBatchInstance(
	t3dClipmapBatch_t &rBatch, const int iIndex,
	const Transform3D &rTransform, const int iTestX, const int iTestZ
) {
	if ( !rBatch.m_hMultimesh.is_valid() || iIndex < 0 || iIndex >= rBatch.m_nCount ||
		rBatch.m_transformBuffer.size() != ( int64_t ) ( rBatch.m_nCount ) * s_nMultimeshStrideFloats ) {
		LOG( ERROR, "Clipmap batch instance index out of bounds: ", iIndex, " / ", ( int ) ( rBatch.m_nCount ) );
		return;
	}
	float *pRecord = rBatch.m_transformBuffer.ptrw() + ( size_t ) ( iIndex ) * s_nMultimeshStrideFloats;
	pRecord[0] = rTransform.basis[0].x;
	pRecord[1] = rTransform.basis[0].y;
	pRecord[2] = rTransform.basis[0].z;
	pRecord[3] = rTransform.origin.x;
	pRecord[4] = rTransform.basis[1].x;
	pRecord[5] = rTransform.basis[1].y;
	pRecord[6] = rTransform.basis[1].z;
	pRecord[7] = rTransform.origin.y;
	pRecord[8] = rTransform.basis[2].x;
	pRecord[9] = rTransform.basis[2].y;
	pRecord[10] = rTransform.basis[2].z;
	pRecord[11] = rTransform.origin.z;
	pRecord[12] = ( float ) ( iTestX );
	pRecord[13] = ( float ) ( iTestZ );
	pRecord[14] = 0.0f;
	pRecord[15] = 0.0f;
}

int T3DMesher::getLodTier( const int nLod ) const {
	if ( nLod < m_nNearShaderLodCount ) {
		return 0;
	}
	if ( nLod < m_nNearShaderLodCount + m_nMiddleShaderLodCount ) {
		return 1;
	}
	return 2;
}

bool T3DMesher::lodCastsShadows( const int nLod ) const {
	if ( m_castShadows == RenderingServer::SHADOW_CASTING_SETTING_OFF || nLod < 0 ) {
		return false;
	}
	const int tier = getLodTier( nLod );
	if ( !m_bTierShadowEnabled[( size_t ) ( tier )] ) {
		return false;
	}
	const int explicit_count = m_nTierShadowLodCounts[( size_t ) ( tier )];
	if ( explicit_count < 0 ) {
		return nLod < m_nShadowLodCount;
	}
	const int tier_first = tier == 0 ? 0 :
		( tier == 1 ? m_nNearShaderLodCount : m_nNearShaderLodCount + m_nMiddleShaderLodCount );
	return nLod >= tier_first && nLod < tier_first + explicit_count;
}

bool T3DMesher::usesDedicatedShadowBatches( void ) const {
	const bool any_explicit = m_nTierShadowLodCounts[0] > 0 ||
		m_nTierShadowLodCounts[1] > 0 || m_nTierShadowLodCounts[2] > 0;
	return m_bStateMeshBatching && ( m_nShadowLodCount > 0 || any_explicit ) &&
		( m_castShadows == RenderingServer::SHADOW_CASTING_SETTING_ON ||
			m_castShadows == RenderingServer::SHADOW_CASTING_SETTING_SHADOWS_ONLY );
}

void T3DMesher::rebuildRenderBatches( void ) {
	// Shadow-only batches may borrow visible state meshes. Free their instances
	// before freeing the visible batches that own those mesh RIDs.
	for ( t3dClipmapBatch_t &batch : m_szShadowStateBatches ) {
		freeBatch( batch );
	}
	for ( t3dClipmapBatch_t &batch : m_szRenderBatches ) {
		freeBatch( batch );
	}
	m_nRenderBatchCount = 0;
	m_bBatchDirty.fill( false );
	m_bShadowStateBatchDirty.fill( false );
	m_nShadowBatchCount = 0;
	for ( t3dLodBatchSlot_t &slot : m_szLodBatchSlots ) {
		slot = t3dLodBatchSlot_t{};
	}

	const bool dedicated_shadows = usesDedicatedShadowBatches();

	int lod = 0;
	while ( lod < m_nClipmapLevelCount ) {
		const bool bLod0 = lod == 0;
		const int tier = getLodTier( lod );
		const bool casts_shadows = !dedicated_shadows && lodCastsShadows( lod );
		int end_lod = lod;
		while ( end_lod + 1 < m_nClipmapLevelCount && getLodTier( end_lod + 1 ) == tier &&
			( dedicated_shadows || ( lodCastsShadows( end_lod + 1 ) == casts_shadows ) ) &&
			( m_bStateMeshBatching || !bLod0 ) ) {
			++end_lod;
		}
		if ( m_nRenderBatchCount >= s_nMaxRenderBatches ) {
			LOG( ERROR, "Clipmap batching exceeded s_nFixed render batch capacity: ", s_nMaxRenderBatches );
			return;
		}
		const int count = end_lod - lod + 1;
		t3dClipmapBatch_t batch;
		if ( m_bStateMeshBatching ) {
			batch = createStateBatch( lod, end_lod, tier, casts_shadows );
		} else {
			const RID mesh = m_szMeshRids[bLod0 ? LOD0_MESH : OUTER_RING_MESH];
			batch = createBatch( mesh, count, lod, end_lod, tier, casts_shadows );
		}
		if ( !batch.m_hInstance.is_valid() ) {
			return;
		}
		const int batch_index = m_nRenderBatchCount;
		m_szRenderBatches[( size_t ) ( batch_index )] = batch;
		for ( int level = lod; level <= end_lod; ++level ) {
			m_szLodBatchSlots[( size_t ) ( level )].m_nBatch = ( int8_t ) ( batch_index );
			m_szLodBatchSlots[( size_t ) ( level )].m_nInstance = m_bStateMeshBatching ? ( int8_t ) ( -1 ) : ( int8_t ) ( level - lod );
		}
		++m_nRenderBatchCount;
		lod = end_lod + 1;
	}

	if ( dedicated_shadows ) {
		const int shadow_end = MIN( m_nShadowLodCount, m_nClipmapLevelCount ) - 1;
		for ( int tier = 0; tier < 3 && m_nShadowBatchCount < 3; ++tier ) {
			if ( !m_bTierShadowEnabled[( size_t ) ( tier )] ) {
				continue;
			}
			const int tier_first = tier == 0 ? 0 : ( tier == 1 ? m_nNearShaderLodCount : m_nNearShaderLodCount + m_nMiddleShaderLodCount );
			const int tier_last = tier == 0 ? m_nNearShaderLodCount - 1 :
				( tier == 1 ? m_nNearShaderLodCount + m_nMiddleShaderLodCount - 1 : m_nClipmapLevelCount - 1 );
			const int first_lod = CLAMP( tier_first, 0, m_nClipmapLevelCount );
			const int explicit_count = m_nTierShadowLodCounts[( size_t ) ( tier )];
			const int last_lod = explicit_count >= 0 ?
				MIN( tier_last, first_lod + explicit_count - 1 ) : MIN( tier_last, shadow_end );
			if ( first_lod > last_lod ) {
				continue;
			}

			t3dClipmapBatch_t shadow_batch;
			for ( int visible_index = 0; visible_index < m_nRenderBatchCount; ++visible_index ) {
				const t3dClipmapBatch_t &visible = m_szRenderBatches[( size_t ) ( visible_index )];
				if ( visible.m_nFirstLod == first_lod && visible.m_nLastLod == last_lod && visible.m_hMesh.is_valid() ) {
					shadow_batch.m_hMesh = visible.m_hMesh;
					shadow_batch.m_bOwnsMesh = false;
					shadow_batch.m_hInstance = RS->instance_create2( shadow_batch.m_hMesh, m_hScenario );
					shadow_batch.m_nCount = ( uint8_t ) ( last_lod - first_lod + 1 );
					shadow_batch.m_nFirstLod = ( uint8_t ) ( first_lod );
					shadow_batch.m_nLastLod = ( uint8_t ) ( last_lod );
					shadow_batch.m_nTier = ( uint8_t ) ( tier );
					shadow_batch.m_bCastsShadows = true;
					break;
				}
			}
			if ( !shadow_batch.m_hInstance.is_valid() ) {
				shadow_batch = createStateBatch( first_lod, last_lod, tier, true );
			}
			if ( !shadow_batch.m_hInstance.is_valid() ) {
				LOG( ERROR, "Failed to create clipmap shadow state batch for tier ", tier,
					" LODs ", first_lod, "-", last_lod );
				continue;
			}
			m_szShadowStateBatches[( size_t ) ( m_nShadowBatchCount )] = shadow_batch;
			m_bShadowStateBatchDirty[( size_t ) ( m_nShadowBatchCount )] = true;
			++m_nShadowBatchCount;
		}
	}

	LOG( INFO, "Clipmap render batching: ", m_nClipmapLevelCount, " LODs -> ", m_nRenderBatchCount,
		" visible state batches + ", m_nShadowBatchCount, " shadow state batches using ",
		getBaseMeshCount(), " mesh resources; mode=", m_bStateMeshBatching ? "combined-state" : "multimesh-fallback" );
}

void T3DMesher::updateBatchCustomAabb( const int iBatchIndex ) {
	if ( iBatchIndex < 0 || iBatchIndex >= m_nRenderBatchCount ) {
		return;
	}
	t3dClipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( iBatchIndex )];
	bool bHasAabb = false;
	AABB combined;
	const Vector2 anchor = m_bStateMeshBatching ? batch.m_anchorPosition : Vector2();
	for ( int lod = batch.m_nFirstLod; lod <= batch.m_nLastLod; ++lod ) {
		const Vector2 position = m_szLastLodPositions[( size_t ) ( lod )];
		if ( position == V2_MAX ) {
			continue;
		}
		const real_t fRadius = ( m_fOffsetC + 2.0f ) * m_fLodScales[( size_t ) ( lod )];
		const Vector2 local_position = m_bStateMeshBatching ? position - anchor : position;
		const AABB lodAabb(
			Vector3( local_position.x - fRadius, m_fClipmapAabbMinY, local_position.y - fRadius ),
			Vector3( fRadius * 2.0f, m_fClipmapAabbSizeY, fRadius * 2.0f )
		);
		if ( !bHasAabb ) {
			combined = lodAabb;
			bHasAabb = true;
		} else {
			combined.merge_with( lodAabb );
		}
	}
	if ( !bHasAabb ) {
		return;
	}
	if ( m_bStateMeshBatching && batch.m_hMesh.is_valid() ) {
		RS->mesh_set_custom_aabb( batch.m_hMesh, combined );
	} else if ( batch.m_hMultimesh.is_valid() ) {
		RS->multimesh_set_custom_aabb( batch.m_hMultimesh, combined );
	}
}

void T3DMesher::configureStateMaterials( void ) {
	if ( !m_bStateMeshBatching ) {
		return;
	}
	static const StringName s_lodStatesParameter( "_clipmap_lod_states" );
	for ( const RID &material : m_szMaterials ) {
		if ( material.is_valid() ) {
			RS->material_set_param( material, s_lodStatesParameter, m_lodStateBuffer );
		}
	}
	for ( const RID &material : m_hszShadowMaterials ) {
		if ( material.is_valid() ) {
			RS->material_set_param( material, s_lodStatesParameter, m_lodStateBuffer );
		}
	}
}

void T3DMesher::refreshStateBatch( const int iBatchIndex, eastl::array<bool, 3> &bDirtyTiers ) {
	if ( iBatchIndex < 0 || iBatchIndex >= m_nRenderBatchCount ) {
		return;
	}
	t3dClipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( iBatchIndex )];
	const Vector2 anchor = m_szLastLodPositions[( size_t ) ( batch.m_nFirstLod )];
	if ( anchor == V2_MAX ) {
		return;
	}
	if ( batch.m_anchorPosition != anchor ) {
		Transform3D transform;
		transform.origin = Vector3( anchor.x, 0.0f, anchor.y );
		RS->instance_set_transform( batch.m_hInstance, transform );
		batch.m_anchorPosition = anchor;
	}
	for ( int lod = batch.m_nFirstLod; lod <= batch.m_nLastLod; ++lod ) {
		const Vector2 position = m_szLastLodPositions[( size_t ) ( lod )];
		if ( position == V2_MAX ) {
			continue;
		}
		const int stitch_code = CLAMP( m_szLastLodTestX[( size_t ) ( lod )], 0, 2 ) +
			CLAMP( m_szLastLodTestZ[( size_t ) ( lod )], 0, 2 ) * 3;
		m_lodStateBuffer.set( lod, Vector4( position.x - anchor.x, position.y - anchor.y,
			m_fLodScales[( size_t ) ( lod )], ( float ) ( stitch_code ) ) );
	}
	bDirtyTiers[( size_t ) ( batch.m_nTier )] = true;
	updateBatchCustomAabb( iBatchIndex );
	RS->instance_teleport( batch.m_hInstance );
	++m_nTeleportsLastSnap;
}

void T3DMesher::refreshShadowStateBatch( const int nIndex ) {
	if ( nIndex < 0 || nIndex >= m_nShadowBatchCount ) {
		return;
	}
	t3dClipmapBatch_t &batch = m_szShadowStateBatches[( size_t ) ( nIndex )];
	if ( !batch.m_hInstance.is_valid() ) {
		return;
	}
	const Vector2 anchor = m_szLastLodPositions[( size_t ) ( batch.m_nFirstLod )];
	if ( anchor == V2_MAX ) {
		return;
	}
	if ( batch.m_anchorPosition != anchor ) {
		Transform3D transform;
		transform.origin = Vector3( anchor.x, 0.0f, anchor.y );
		RS->instance_set_transform( batch.m_hInstance, transform );
		batch.m_anchorPosition = anchor;
	}
	bool bHasAabb = false;
	AABB combined;
	for ( int lod = batch.m_nFirstLod; lod <= batch.m_nLastLod; ++lod ) {
		const Vector2 position = m_szLastLodPositions[( size_t ) ( lod )];
		if ( position == V2_MAX ) {
			continue;
		}
		const real_t fRadius = ( m_fOffsetC + 2.0f ) * m_fLodScales[( size_t ) ( lod )];
		const Vector2 local_position = position - anchor;
		const AABB lodAabb(
			Vector3( local_position.x - fRadius, m_fClipmapAabbMinY, local_position.y - fRadius ),
			Vector3( fRadius * 2.0f, m_fClipmapAabbSizeY, fRadius * 2.0f )
		);
		if ( !bHasAabb ) {
			combined = lodAabb;
			bHasAabb = true;
		} else {
			combined.merge_with( lodAabb );
		}
	}
	if ( bHasAabb && batch.m_hMesh.is_valid() ) {
		RS->mesh_set_custom_aabb( batch.m_hMesh, combined );
	}
	RS->instance_teleport( batch.m_hInstance );
	++m_nTeleportsLastSnap;
}

void T3DMesher::flushDirtyBatches( void ) {
	if ( m_bStateMeshBatching ) {
		eastl::array<bool, 3> dirty_tiers{};
		for ( int batch_index = 0; batch_index < m_nRenderBatchCount; ++batch_index ) {
			if ( !m_bBatchDirty[( size_t ) ( batch_index )] ) {
				continue;
			}
			refreshStateBatch( batch_index, dirty_tiers );
			m_bBatchDirty[( size_t ) ( batch_index )] = false;
		}
		static const StringName s_lodStatesParameter( "_clipmap_lod_states" );
		for ( int shadow_index = 0; shadow_index < m_nShadowBatchCount; ++shadow_index ) {
			if ( !m_bShadowStateBatchDirty[( size_t ) ( shadow_index )] ) {
				continue;
			}
			refreshShadowStateBatch( shadow_index );
			m_bShadowStateBatchDirty[( size_t ) ( shadow_index )] = false;
		}
		for ( int tier = 0; tier < 3; ++tier ) {
			if ( !dirty_tiers[( size_t ) ( tier )] ) {
				continue;
			}
			const RID material = m_szMaterials[( size_t ) ( tier )].is_valid() ? m_szMaterials[( size_t ) ( tier )] : m_szMaterials[0];
			if ( material.is_valid() ) {
				RS->material_set_param( material, s_lodStatesParameter, m_lodStateBuffer );
				++m_nBufferUploadsLastSnap;
			}
			const RID shadow_material = m_hszShadowMaterials[( size_t ) ( tier )];
			if ( shadow_material.is_valid() ) {
				RS->material_set_param( shadow_material, s_lodStatesParameter, m_lodStateBuffer );
				++m_nBufferUploadsLastSnap;
			}
		}
		return;
	}
	for ( int batch_index = 0; batch_index < m_nRenderBatchCount; ++batch_index ) {
		if ( !m_bBatchDirty[( size_t ) ( batch_index )] ) {
			continue;
		}
		t3dClipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( batch_index )];
		if ( batch.m_hMultimesh.is_valid() && !batch.m_transformBuffer.is_empty() ) {
			RS->multimesh_set_buffer( batch.m_hMultimesh, batch.m_transformBuffer );
			++m_nBufferUploadsLastSnap;
		}
		updateBatchCustomAabb( batch_index );
		if ( batch.m_hInstance.is_valid() ) {
			RS->instance_teleport( batch.m_hInstance );
			++m_nTeleportsLastSnap;
		}
		m_bBatchDirty[( size_t ) ( batch_index )] = false;
	}
}

void T3DMesher::generateClipmap( void ) {
	clearClipmap();
	generateOffsetData();
	clearMeshTypes();
	if ( m_bRequireCookedGeometryPack ) {
		if ( !m_pTerrain || !loadRuntimeGeometryPack( m_pTerrain->getDataDirectory() ) ) {
			ERR_PRINT( "Terrain3D strict cooked runtime requires a valid terrain3d_clipmap.t3dmeshpack." );
			m_nClipmapLevelCount = 0;
			return;
		}
	} else {
		generateMeshTypes();
	}
	m_nClipmapLevelCount = CLAMP( m_nLods + m_tessellationLevel, 0, s_nMaxClipmapLevels );
	rebuildRenderBatches();
}

void T3DMesher::generateOffsetData( void ) {
	LOG( INFO, "Computing s_nFixed clipmap positioning offsets" );
	m_szTilePosLod0 = {
		Vector3( 0, 0, m_nMeshSize ), Vector3( m_nMeshSize, 0, m_nMeshSize ), Vector3( m_nMeshSize, 0, 0 ),
		Vector3( m_nMeshSize, 0, -m_nMeshSize ), Vector3( m_nMeshSize, 0, -m_nMeshSize * 2 ),
		Vector3( 0, 0, -m_nMeshSize * 2 ), Vector3( -m_nMeshSize, 0, -m_nMeshSize * 2 ),
		Vector3( -m_nMeshSize * 2, 0, -m_nMeshSize * 2 ), Vector3( -m_nMeshSize * 2, 0, -m_nMeshSize ),
		Vector3( -m_nMeshSize * 2, 0, 0 ), Vector3( -m_nMeshSize * 2, 0, m_nMeshSize ),
		Vector3( -m_nMeshSize, 0, m_nMeshSize ), V3_ZERO, Vector3( -m_nMeshSize, 0, 0 ),
		Vector3( 0, 0, -m_nMeshSize ), Vector3( -m_nMeshSize, 0, -m_nMeshSize ),
	};
	m_szTrimAPos = {Vector3( m_nMeshSize * 2, 0, -m_nMeshSize * 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, -m_nMeshSize * 2 - 2 )};
	m_szTrimBPos = {Vector3( -m_nMeshSize * 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, m_nMeshSize * 2 )};
	m_szTilePos = {
		Vector3( 2, 0, m_nMeshSize + 2 ), Vector3( m_nMeshSize + 2, 0, m_nMeshSize + 2 ),
		Vector3( m_nMeshSize + 2, 0, -2 ), Vector3( m_nMeshSize + 2, 0, -m_nMeshSize - 2 ),
		Vector3( m_nMeshSize + 2, 0, -m_nMeshSize * 2 - 2 ), Vector3( -2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize - 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, -m_nMeshSize + 2 ), Vector3( -m_nMeshSize * 2 - 2, 0, 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, m_nMeshSize + 2 ), Vector3( -m_nMeshSize + 2, 0, m_nMeshSize + 2 ),
	};
	m_fOffsetA = ( real_t ) ( m_nMeshSize * 2 ) + 2.f;
	m_fOffsetB = ( real_t ) ( m_nMeshSize * 2 ) + 4.f;
	m_fOffsetC = ( real_t ) ( m_nMeshSize * 2 ) + 6.f;
	m_szEdgePos = {Vector3( m_fOffsetA, m_fOffsetA, -m_fOffsetB ), Vector3( m_fOffsetB, -m_fOffsetB, -m_fOffsetC )};
	m_szFillAPos = {Vector3( m_nMeshSize - 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize - 2, 0, m_nMeshSize + 2 )};
	m_szFillBPos = {Vector3( m_nMeshSize + 2, 0, m_nMeshSize - 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, -m_nMeshSize - 2 )};
}

void T3DMesher::generateLodCache( void ) {
	m_nClipmapLevelCount = CLAMP( m_nLods + m_tessellationLevel, 0, s_nMaxClipmapLevels );
	m_szLastLodPositions.fill( V2_MAX );
	m_szLastLodTestX.fill( -1 );
	m_szLastLodTestZ.fill( -1 );
	const real_t fDensity = ( real_t ) ( 1.0 ) / ( real_t ) ( ( uint64_t ) ( 1 ) << CLAMP( m_tessellationLevel, 0, 30 ) );
	const real_t fBaseSpacing = m_fVertexSpacing * fDensity;
	for ( int lod = 0; lod < m_nClipmapLevelCount; ++lod ) {
		const real_t fMultiplier = ( real_t ) ( ( uint64_t ) ( 1 ) << CLAMP( lod, 0, 30 ) );
		m_fLodScales[( size_t ) ( lod )] = fBaseSpacing * fMultiplier;
		m_fLodSnapSteps[( size_t ) ( lod )] = m_fLodScales[( size_t ) ( lod )] * 2.0f;
		m_fLodNextSnapSteps[( size_t ) ( lod )] = m_fLodSnapSteps[( size_t ) ( lod )] * 2.0f;
	}
}

void T3DMesher::clearClipmap( void ) {
	LOG( INFO, "Freeing clipmap render batches" );
	// Free shadows-only instances first because they may borrow visible state
	// meshes rather than owning duplicate geometry resources.
	for ( t3dClipmapBatch_t &batch : m_szShadowStateBatches ) {
		freeBatch( batch );
	}
	for ( t3dClipmapBatch_t &batch : m_szRenderBatches ) {
		freeBatch( batch );
	}
	m_bShadowStateBatchDirty.fill( false );
	m_nShadowBatchCount = 0;
	m_nRenderBatchCount = 0;
	m_bBatchDirty.fill( false );
	for ( t3dLodBatchSlot_t &slot : m_szLodBatchSlots ) {
		slot = t3dLodBatchSlot_t{};
	}
	m_nClipmapLevelCount = 0;
}

int T3DMesher::getImmediateLodCount( void ) const {
	// Keep the first middle ring synchronized with the near clipmap. Delaying
	// this boundary can expose a one-frame seam while the remaining coarse
	// rings are intentionally amortized across later ticks.
	const int continuity_ring = m_nMiddleShaderLodCount > 0 ? 1 : 0;
	return CLAMP( m_nNearShaderLodCount + continuity_ring, 0, m_nClipmapLevelCount );
}

void T3DMesher::clearMeshTypes( void ) {
	LOG( INFO, "Freeing shared clipmap meshes" );
	for ( RID &rid : m_szMeshRids ) {
		if ( rid.is_valid() ) {
			RS->free_rid( rid );
		}
		rid = RID();
	}
	for ( t3dClipmapGeometry_t &geometry : m_szMeshGeometry ) {
		geometry = t3dClipmapGeometry_t{};
	}
}

///////////////////////////
// Public Functions
///////////////////////////

void T3DMesher::initialize(
	T3DTerrain *const pTerrain, const int nMeshSize, const int nLods,
	const int nTessellationLevel, const real_t fVertexSpacing, const eastl::array<RID, 3> &rMaterials,
	const eastl::array<RID, 3> &rShadowMaterials,
	const int nNearShaderLodCount, const int nMiddleShaderLodCount,
	const int nClipmapUpdateBudget, const uint32_t nRenderLayers,
	const RenderingServer::ShadowCastingSetting castShadows,
	const GeometryInstance3D::GIMode giMode, const int nShadowLodCount,
	const eastl::array<bool, 3> &bTierShadowEnabled,
	const eastl::array<int, 3> &nTierShadowLodCounts, const bool bStateMeshBatching,
	const bool bRequireCookedGeometryPack
) {
	if ( !pTerrain ) {
		return;
	}
	m_pTerrain = pTerrain;
	if ( !m_pTerrain->isInsideWorld() ) {
		LOG( DEBUG, "Terrain3D's world3D is null" );
		return;
	}
	LOG( INFO, "Initializing GeoMesh" );
	m_hScenario = m_pTerrain->get_world_3d()->get_scenario();
	m_szMaterials = rMaterials;
	m_hszShadowMaterials = rShadowMaterials;
	m_bStateMeshBatching = bStateMeshBatching;
	m_bRequireCookedGeometryPack = bRequireCookedGeometryPack;
	m_lodStateBuffer.resize( s_nMaxClipmapLevels );
	m_lodStateBuffer.fill( Vector4( 0.0f, 0.0f, 1.0f, 4.0f ) );
	configureStateMaterials();
	m_nNearShaderLodCount = CLAMP( nNearShaderLodCount, 0, nLods + nTessellationLevel );
	m_nMiddleShaderLodCount = CLAMP( nMiddleShaderLodCount, 0,
		nLods + nTessellationLevel - m_nNearShaderLodCount );
	m_nClipmapUpdateBudget = CLAMP( nClipmapUpdateBudget, 1, s_nMaxClipmapLevels );
	m_bForceFullSnap = true;
	m_nLods = nLods;
	m_tessellationLevel = nTessellationLevel;
	m_nMeshSize = nMeshSize;
	m_fVertexSpacing = fVertexSpacing;
	m_renderLayers = nRenderLayers;
	m_castShadows = castShadows;
	m_giMode = giMode;
	m_nShadowLodCount = CLAMP( nShadowLodCount, 0, s_nMaxClipmapLevels );
	m_bTierShadowEnabled = bTierShadowEnabled;
	m_nTierShadowLodCounts = nTierShadowLodCounts;
	generateClipmap();
	generateLodCache();
	m_iDeferredLodCursor = getImmediateLodCount();
	update();
	updateAabbs();
	resetTargetPosition();
	snap();
}

void T3DMesher::destroy( void ) {
	LOG( INFO, "Destroying clipmap" );
	clearClipmap();
	clearMeshTypes();
	m_szLastLodPositions.fill( V2_MAX );
	m_szLastLodTestX.fill( -1 );
	m_szLastLodTestZ.fill( -1 );
	m_fLodScales.fill( 0.0f );
	m_fLodSnapSteps.fill( 0.0f );
	m_fLodNextSnapSteps.fill( 0.0f );
}

void T3DMesher::setTierLayout(
	const int nNearShaderLodCount,
	const int nMiddleShaderLodCount, const int nClipmapUpdateBudget,
	const bool bForceFullSnap
) {
	const int total_levels = MAX( 0, m_nLods + m_tessellationLevel );
	const int near_count = CLAMP( nNearShaderLodCount, 0, total_levels );
	const int middle_count = CLAMP( nMiddleShaderLodCount, 0, total_levels - near_count );
	const int update_budget = CLAMP( nClipmapUpdateBudget, 1, s_nMaxClipmapLevels );
	const bool bLayoutChanged = m_nNearShaderLodCount != near_count || m_nMiddleShaderLodCount != middle_count;
	const bool bBudgetChanged = m_nClipmapUpdateBudget != update_budget;
	if ( !bLayoutChanged && !bBudgetChanged ) {
		return;
	}
	const int old_immediate_count = getImmediateLodCount();
	m_nNearShaderLodCount = near_count;
	m_nMiddleShaderLodCount = middle_count;
	m_nClipmapUpdateBudget = update_budget;
	const int new_immediate_count = getImmediateLodCount();
	m_iDeferredLodCursor = new_immediate_count;
	m_bForceFullSnap = bForceFullSnap || old_immediate_count != new_immediate_count;
	if ( !bLayoutChanged ) {
		return;
	}
	rebuildRenderBatches();
	resetTargetPosition();
	update();
	snap();
}

void T3DMesher::setCastShadows( const RenderingServer::ShadowCastingSetting castShadows ) {
	if ( m_castShadows == castShadows ) {
		return;
	}
	m_castShadows = castShadows;
	rebuildRenderBatches();
	resetTargetPosition();
	update();
	snap();
}

void T3DMesher::setShadowLodCount( const int nCount ) {
	const int count = CLAMP( nCount, 0, s_nMaxClipmapLevels );
	if ( m_nShadowLodCount == count ) {
		return;
	}
	m_nShadowLodCount = count;
	if ( m_castShadows == RenderingServer::SHADOW_CASTING_SETTING_OFF ) {
		return;
	}
	rebuildRenderBatches();
	resetTargetPosition();
	update();
	snap();
}

void T3DMesher::setTierShadowEnabled( const eastl::array<bool, 3> &bEnabled ) {
	if ( m_bTierShadowEnabled == bEnabled ) {
		return;
	}
	m_bTierShadowEnabled = bEnabled;
	if ( m_castShadows == RenderingServer::SHADOW_CASTING_SETTING_OFF ) {
		return;
	}
	rebuildRenderBatches();
	resetTargetPosition();
	update();
	snap();
}

void T3DMesher::setTierShadowLodCounts( const eastl::array<int, 3> &nCounts ) {
	eastl::array<int, 3> counts{};
	for ( int tier = 0; tier < 3; ++tier ) {
		counts[( size_t ) ( tier )] = CLAMP( nCounts[( size_t ) ( tier )], -1, s_nMaxClipmapLevels );
	}
	if ( m_nTierShadowLodCounts == counts ) {
		return;
	}
	m_nTierShadowLodCounts = counts;
	if ( m_castShadows == RenderingServer::SHADOW_CASTING_SETTING_OFF ) {
		return;
	}
	rebuildRenderBatches();
	resetTargetPosition();
	update();
	snap();
}

void T3DMesher::resetTargetPosition( void ) {
	m_lastShaderTargetPosition = V3_MAX;
	m_szLastLodPositions.fill( V2_MAX );
	m_szLastLodTestX.fill( -1 );
	m_szLastLodTestZ.fill( -1 );
	m_bForceFullSnap = true;
	m_iDeferredLodCursor = getImmediateLodCount();
}

void T3DMesher::snap( void ) {
	IS_INIT( VOID );
	snap( m_pTerrain->getClipmapTargetPosition() );
}

bool T3DMesher::snapLevel( const int nLod, const Vector3 &rSnappedTarget ) {
	const real_t fSnapStep = m_fLodSnapSteps[( size_t ) ( nLod )];
	const real_t fNextSnapStep = m_fLodNextSnapSteps[( size_t ) ( nLod )];
	Vector3 pos = V3_ZERO;
	pos.x = Math::round( rSnappedTarget.x / fSnapStep ) * fSnapStep;
	pos.z = Math::round( rSnappedTarget.z / fSnapStep ) * fSnapStep;
	const real_t fNextX = Math::round( rSnappedTarget.x / fNextSnapStep ) * fNextSnapStep;
	const real_t fNextZ = Math::round( rSnappedTarget.z / fNextSnapStep ) * fNextSnapStep;
	const int test_x = CLAMP( ( int ) ( Math::round( ( pos.x - fNextX ) / fSnapStep ) ) + 1, 0, 2 );
	const int test_z = CLAMP( ( int ) ( Math::round( ( pos.z - fNextZ ) / fSnapStep ) ) + 1, 0, 2 );
	const Vector2 snappedLodPos( pos.x, pos.z );
	if ( m_szLastLodPositions[( size_t ) ( nLod )] == snappedLodPos &&
		m_szLastLodTestX[( size_t ) ( nLod )] == test_x && m_szLastLodTestZ[( size_t ) ( nLod )] == test_z ) {
		return false;
	}
	m_szLastLodPositions[( size_t ) ( nLod )] = snappedLodPos;
	m_szLastLodTestX[( size_t ) ( nLod )] = test_x;
	m_szLastLodTestZ[( size_t ) ( nLod )] = test_z;

	const t3dLodBatchSlot_t slot = m_szLodBatchSlots[( size_t ) ( nLod )];
	if ( slot.m_nBatch < 0 || slot.m_nBatch >= m_nRenderBatchCount ) {
		LOG( ERROR, "Missing clipmap render batch for LOD ", nLod );
		return false;
	}
	t3dClipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( slot.m_nBatch )];
	if ( !m_bStateMeshBatching ) {
		Transform3D transform;
		transform = transform.scaled( Vector3( m_fLodScales[( size_t ) ( nLod )], 1.0f, m_fLodScales[( size_t ) ( nLod )] ) );
		transform.origin = pos;
		setBatchInstance( batch, slot.m_nInstance, transform, test_x, test_z );
	}
	m_bBatchDirty[( size_t ) ( slot.m_nBatch )] = true;
	for ( int shadow_index = 0; shadow_index < m_nShadowBatchCount; ++shadow_index ) {
		const t3dClipmapBatch_t &shadow_batch = m_szShadowStateBatches[( size_t ) ( shadow_index )];
		if ( nLod >= shadow_batch.m_nFirstLod && nLod <= shadow_batch.m_nLastLod ) {
			m_bShadowStateBatchDirty[( size_t ) ( shadow_index )] = true;
		}
	}

	const int tier = getLodTier( nLod );
	if ( tier == 0 ) {
		++m_nNearUpdatesLastSnap;
	} else if ( tier == 1 ) {
		++m_nMiddleUpdatesLastSnap;
	} else {
		++m_nFarUpdatesLastSnap;
	}
	return true;
}

void T3DMesher::snap( const Vector3 &rTargetPos ) {
	IS_INIT( VOID );
	if ( rTargetPos != m_lastShaderTargetPosition ) {
		static const StringName s_targetPositionParameter( "_target_pos" );
		for ( const RID &material : m_szMaterials ) {
			if ( material.is_valid() ) {
				RS->material_set_param( material, s_targetPositionParameter, rTargetPos );
			}
		}
		for ( const RID &material : m_hszShadowMaterials ) {
			if ( material.is_valid() ) {
				RS->material_set_param( material, s_targetPositionParameter, rTargetPos );
			}
		}
		m_lastShaderTargetPosition = rTargetPos;
	}
	m_nNearUpdatesLastSnap = 0u;
	m_nMiddleUpdatesLastSnap = 0u;
	m_nFarUpdatesLastSnap = 0u;
	m_nTeleportsLastSnap = 0u;
	m_nBufferUploadsLastSnap = 0u;
	const real_t fBaseSpacing = m_nClipmapLevelCount > 0 ? m_fLodScales[0] : m_fVertexSpacing;
	const Vector3 snappedTarget(
		Math::floor( rTargetPos.x / fBaseSpacing ) * fBaseSpacing, rTargetPos.y,
		Math::floor( rTargetPos.z / fBaseSpacing ) * fBaseSpacing
	);

	const int immediate_count = getImmediateLodCount();
	for ( int lod = 0; lod < immediate_count; ++lod ) {
		snapLevel( lod, snappedTarget );
	}
	if ( m_bForceFullSnap ) {
		for ( int lod = immediate_count; lod < m_nClipmapLevelCount; ++lod ) {
			snapLevel( lod, snappedTarget );
		}
		m_bForceFullSnap = false;
		m_iDeferredLodCursor = immediate_count;
		flushDirtyBatches();
		return;
	}
	const int deferred_count = m_nClipmapLevelCount - immediate_count;
	if ( deferred_count <= 0 ) {
		flushDirtyBatches();
		return;
	}
	int updated = 0;
	int scanned = 0;
	int lod = CLAMP( m_iDeferredLodCursor, immediate_count, m_nClipmapLevelCount - 1 );
	while ( scanned < deferred_count && updated < m_nClipmapUpdateBudget ) {
		if ( snapLevel( lod, snappedTarget ) ) {
			++updated;
		}
		++scanned;
		++lod;
		if ( lod >= m_nClipmapLevelCount ) {
			lod = immediate_count;
		}
	}
	m_iDeferredLodCursor = lod;
	flushDirtyBatches();
}

void T3DMesher::updateInstanceRid(
	const RID &rRid, const RID &rMaterial,
	const RenderingServer::ShadowCastingSetting castShadows, const bool bVisible,
	const bool bBakedLight, const bool bDynamicGi
) {
	if ( !rRid.is_valid() ) {
		return;
	}
	RS->instance_set_visible( rRid, bVisible );
	RS->instance_geometry_set_material_override( rRid, rMaterial );
	RS->instance_set_scenario( rRid, m_hScenario );
	RS->instance_set_layer_mask( rRid, m_renderLayers );
	RS->instance_geometry_set_cast_shadows_setting( rRid, castShadows );
	RS->instance_geometry_set_flag( rRid, RenderingServer::INSTANCE_FLAG_USE_BAKED_LIGHT, bBakedLight );
	RS->instance_geometry_set_flag( rRid, RenderingServer::INSTANCE_FLAG_USE_DYNAMIC_GI, bDynamicGi );
}

void T3DMesher::update( void ) {
	IS_INIT( VOID );
	if ( !m_pTerrain->isInsideWorld() ) {
		return;
	}
	bool bBakedLight = false;
	bool bDynamicGi = false;
	if ( m_giMode == GeometryInstance3D::GI_MODE_DYNAMIC ) {
		bDynamicGi = true;
	} else if ( m_giMode == GeometryInstance3D::GI_MODE_STATIC ) {
		bBakedLight = true;
	}
	const bool bTerrainVisible = m_pTerrain->is_visible_in_tree();
	const bool bVisibleSurface = bTerrainVisible && m_castShadows != RenderingServer::SHADOW_CASTING_SETTING_SHADOWS_ONLY;
	for ( int batch_index = 0; batch_index < m_nRenderBatchCount; ++batch_index ) {
		const t3dClipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( batch_index )];
		const RID material = m_szMaterials[( size_t ) ( batch.m_nTier )].is_valid() ? m_szMaterials[( size_t ) ( batch.m_nTier )] : m_szMaterials[0];
		const RenderingServer::ShadowCastingSetting shadows = batch.m_bCastsShadows ? m_castShadows :
			RenderingServer::SHADOW_CASTING_SETTING_OFF;
		updateInstanceRid( batch.m_hInstance, material, shadows, bVisibleSurface, bBakedLight, bDynamicGi );
	}
	for ( int shadow_index = 0; shadow_index < m_nShadowBatchCount; ++shadow_index ) {
		const t3dClipmapBatch_t &batch = m_szShadowStateBatches[( size_t ) ( shadow_index )];
		const RID material = m_hszShadowMaterials[( size_t ) ( batch.m_nTier )].is_valid() ?
			m_hszShadowMaterials[( size_t ) ( batch.m_nTier )] :
			( m_szMaterials[( size_t ) ( batch.m_nTier )].is_valid() ?
				m_szMaterials[( size_t ) ( batch.m_nTier )] : m_szMaterials[0] );
		updateInstanceRid( batch.m_hInstance, material,
			RenderingServer::SHADOW_CASTING_SETTING_SHADOWS_ONLY, bTerrainVisible, false, false );
	}
}

void T3DMesher::updateAabbs( const real_t fCullMargin, const Vector2 &rHeightRange ) {
	IS_DATA_INIT( VOID );
	const real_t cull_margin = fCullMargin < 0.0f ? m_pTerrain->getCullMargin() : fCullMargin;
	const Vector2 height_range = rHeightRange.x == FLT_MAX ? m_pTerrain->getData()->getHeightRange() : rHeightRange;
	const t3dClipmapVerticalBounds_t vertical_bounds = t3dClipmapVerticalBounds(
		( double ) ( height_range.x ), ( double ) ( height_range.y ), ( double ) ( cull_margin ) );
	m_fClipmapAabbMinY = ( real_t ) ( vertical_bounds.m_fMinY );
	m_fClipmapAabbSizeY = ( real_t ) ( vertical_bounds.m_fSizeY );
	if ( !m_bStateMeshBatching ) {
		for ( const RID &rid : m_szMeshRids ) {
			if ( !rid.is_valid() ) {
				continue;
			}
			AABB aabb = RS->mesh_get_custom_aabb( rid );
			aabb.position.y = m_fClipmapAabbMinY;
			aabb.size.y = m_fClipmapAabbSizeY;
			RS->mesh_set_custom_aabb( rid, aabb );
		}
	}
	for ( int batch_index = 0; batch_index < m_nRenderBatchCount; ++batch_index ) {
		updateBatchCustomAabb( batch_index );
	}
	for ( int shadow_index = 0; shadow_index < m_nShadowBatchCount; ++shadow_index ) {
		refreshShadowStateBatch( shadow_index );
	}
}
