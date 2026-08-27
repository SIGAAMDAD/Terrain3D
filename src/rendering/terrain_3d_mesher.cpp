// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/terrain_3d_mesher.h"

#include "core/logger.h"
#include "eastl_config.h"
#include "rendering/terrain_3d_clipmap_bounds.h"
#include "terrain_3d.h"

#include <EASTL/vector.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/version.hpp>

static constexpr uint64_t s_nClipmapPackMagic = 0x314b504d504c4333ull; // 3CLMPPK1
static constexpr uint32_t s_nClipmapPackVersion = 1u;
static constexpr uint32_t s_nClipmapWeldPartCount = 5u;
EA_FORCE_INLINE static int t3dMesherGridVertexCount( const Vector2i &rSize ) {
	return ( rSize.x + 1 ) * ( rSize.y + 1 );
}
EA_FORCE_INLINE static int t3dMesherGridIndexCount( const Vector2i &rSize ) {
	return rSize.x * rSize.y * 6;
}

void t3dWeldClipmapGeometry( PackedVector3Array &rVertices,
		PackedVector2Array &rMetadata,
		PackedInt32Array &rIndices ) {
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
		const int32_t x = ( int32_t )( Math::round( pSourceVertices[vertex_index].x ) );
		const int32_t z = ( int32_t )( Math::round( pSourceVertices[vertex_index].z ) );
		min_x = MIN( min_x, x );
		min_z = MIN( min_z, z );
		max_x = MAX( max_x, x );
		max_z = MAX( max_z, z );
	}
	const uint64_t width = ( uint64_t )( ( int64_t )( max_x ) - ( int64_t )( min_x ) + 1ll );
	const uint64_t height = ( uint64_t )( ( int64_t )( max_z ) - ( int64_t )( min_z ) + 1ll );
	const uint64_t plane = width * height;
	const uint64_t lookup_count = plane * s_nClipmapWeldPartCount;
	if ( width == 0u || height == 0u || lookup_count > ( uint64_t )( INT32_MAX ) ) {
		return;
	}
	eastl::vector<int32_t> lookup;
	lookup.resize( ( size_t )( lookup_count ), -1 );
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
		const int32_t x = ( int32_t )( Math::round( vertex.x ) );
		const int32_t z = ( int32_t )( Math::round( vertex.z ) );
		const int32_t rounded_part = ( int32_t )( Math::round( metadata.x ) );
		if ( rounded_part < 0 || rounded_part >= ( int32_t )s_nClipmapWeldPartCount ) {
			return;
		}
		const uint32_t part = ( uint32_t )rounded_part;
		const uint64_t lookup_index = ( uint64_t )part * plane +
				( uint64_t )( ( int64_t )( z ) - ( int64_t )( min_z ) ) * width +
				( uint64_t )( ( int64_t )( x ) - ( int64_t )( min_x ) );
		int32_t &welded_index = lookup[( size_t )( lookup_index )];
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

// -----------------------------------------------------------------------------
// Private functions

void Terrain3DMesher::appendGrid( PackedVector3Array &rVertices,
		PackedVector2Array &rMetadata,
		PackedInt32Array &rIndices,
		int &nVertexWrite,
		int &nIndexWrite,
		const Vector2i &rSize,
		const Vector3 &rOrigin,
		const float fPartId,
		const bool bStandardGrid ) {
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

t3dClipmapGeometry_t Terrain3DMesher::generateCombinedGeometry( const bool bLod0 ) {
	t3dClipmapGeometry_t geometry;
	PackedVector3Array &vertices = geometry.m_vertices;
	PackedVector2Array &metadata = geometry.m_metadata;
	PackedInt32Array &indices = geometry.m_indices;

	int total_vertices = 0;
	int total_indices = 0;
	auto measure_grid = [&]( const Vector2i &rSize ) {
		total_vertices += t3dMesherGridVertexCount( rSize );
		total_indices += t3dMesherGridIndexCount( rSize );
	};
	if ( bLod0 ) {
		for ( const Vector3 &position : m_szTilePosLod0 ) {
			( void )position;
			measure_grid( V2I( m_nMeshSize ) );
		}
		for ( const Vector3 &position : m_szTrimAPos ) {
			( void )position;
			measure_grid( Vector2i( 2, m_nMeshSize * 4 + 2 ) );
		}
		for ( const Vector3 &position : m_szTrimBPos ) {
			( void )position;
			measure_grid( Vector2i( m_nMeshSize * 4 + 2, 2 ) );
		}
	} else {
		for ( const Vector3 &position : m_szTilePos ) {
			( void )position;
			measure_grid( V2I( m_nMeshSize ) );
		}
		for ( const Vector3 &position : m_szFillAPos ) {
			( void )position;
			measure_grid( Vector2i( 4, m_nMeshSize ) );
		}
		for ( const Vector3 &position : m_szFillBPos ) {
			( void )position;
			measure_grid( Vector2i( m_nMeshSize, 4 ) );
		}
	}
	for ( int i = 0; i < s_nPairInstances; ++i ) {
		( void )i;
		measure_grid( Vector2i( 2, m_nMeshSize * 4 + 8 ) );
		measure_grid( Vector2i( m_nMeshSize * 4 + 4, 2 ) );
	}
	vertices.resize( total_vertices );
	metadata.resize( total_vertices );
	indices.resize( total_indices );
	int vertex_write = 0;
	int index_write = 0;

	if ( bLod0 ) {
		for ( const Vector3 &position : m_szTilePosLod0 ) {
			appendGrid( vertices,
					metadata,
					indices,
					vertex_write,
					index_write,
					V2I( m_nMeshSize ),
					position,
					s_fClipmapPartStatic,
					true );
		}
		for ( const Vector3 &position : m_szTrimAPos ) {
			appendGrid( vertices,
					metadata,
					indices,
					vertex_write,
					index_write,
					Vector2i( 2, m_nMeshSize * 4 + 2 ),
					position,
					s_fClipmapPartStatic,
					true );
		}
		for ( const Vector3 &position : m_szTrimBPos ) {
			appendGrid( vertices,
					metadata,
					indices,
					vertex_write,
					index_write,
					Vector2i( m_nMeshSize * 4 + 2, 2 ),
					position,
					s_fClipmapPartStatic,
					true );
		}
	} else {
		for ( const Vector3 &position : m_szTilePos ) {
			appendGrid( vertices,
					metadata,
					indices,
					vertex_write,
					index_write,
					V2I( m_nMeshSize ),
					position,
					s_fClipmapPartStatic,
					false );
		}
		for ( const Vector3 &position : m_szFillAPos ) {
			appendGrid( vertices,
					metadata,
					indices,
					vertex_write,
					index_write,
					Vector2i( 4, m_nMeshSize ),
					position,
					s_fClipmapPartStatic,
					false );
		}
		for ( const Vector3 &position : m_szFillBPos ) {
			appendGrid( vertices,
					metadata,
					indices,
					vertex_write,
					index_write,
					Vector2i( m_nMeshSize, 4 ),
					position,
					s_fClipmapPartStatic,
					false );
		}
	}

	// Edge placement is baked for stitch state (1, 1). INSTANCE_CUSTOM.rg
	// carries the live 3x3 stitch state and the vertex shader applies only the
	// small edge translation delta. This keeps one immutable complete-ring mesh
	// reusable by every LOD in a shader tier.
	for ( int i = 0; i < s_nPairInstances; ++i ) {
		Vector3 edge_a;
		edge_a.z = -m_fOffsetA - 2.0f;
		edge_a.x = m_szEdgePos[( size_t )i][1];
		appendGrid( vertices,
				metadata,
				indices,
				vertex_write,
				index_write,
				Vector2i( 2, m_nMeshSize * 4 + 8 ),
				edge_a,
				i == 0 ? s_fClipmapPartEdgeA0 : s_fClipmapPartEdgeA1,
				bLod0 );

		Vector3 edge_b;
		edge_b.z = m_szEdgePos[( size_t )i][1];
		edge_b.x = -m_fOffsetA;
		appendGrid( vertices,
				metadata,
				indices,
				vertex_write,
				index_write,
				Vector2i( m_nMeshSize * 4 + 4, 2 ),
				edge_b,
				i == 0 ? s_fClipmapPartEdgeB0 : s_fClipmapPartEdgeB1,
				bLod0 );
	}
	if ( vertex_write != total_vertices || index_write != total_indices ) {
		LOG( ERROR, "Clipmap geometry preallocation count mismatch" );
		return t3dClipmapGeometry_t{};
	}

	const int source_vertex_count = vertices.size();
	t3dWeldClipmapGeometry( vertices, metadata, indices );
	const real_t fRadius = m_fOffsetC + 2.0f;
	geometry.m_aabb = AABB( Vector3( -fRadius, -0.1f, -fRadius ), Vector3( fRadius * 2.0f, 0.2f, fRadius * 2.0f ) );
	LOG( INFO,
			"Generated ",
			bLod0 ? "LOD0" : "outer-ring",
			" clipmap geometry: ",
			vertices.size(),
			" welded vertices from ",
			source_vertex_count,
			" source vertices, ",
			indices.size() / 3,
			" triangles" );
	return geometry;
}

bool Terrain3DMesher::loadRuntimeGeometryPack( const String &rDataDirectory ) {
	if ( rDataDirectory.is_empty() ) {
		return false;
	}
	const String path = rDataDirectory.path_join( "terrain3d_clipmap.t3dmeshpack" );
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::READ );
	if ( file.is_null() || file->get_length() < 24 ) {
		return false;
	}
	if ( file->get_64() != s_nClipmapPackMagic || file->get_32() != s_nClipmapPackVersion ||
			( int )file->get_32() != m_nMeshSize || file->get_32() != MESH_TYPE_COUNT ) {
		return false;
	}
	file->get_32();
	for ( int type = 0; type < MESH_TYPE_COUNT; ++type ) {
		t3dClipmapGeometry_t geometry;
		const uint32_t vertex_count = file->get_32();
		const uint32_t metadata_count = file->get_32();
		const uint32_t index_count = file->get_32();
		file->get_32();
		if ( vertex_count == 0u || metadata_count != vertex_count || index_count == 0u || vertex_count > 4000000u ||
				index_count > 12000000u ) {
			return false;
		}
		geometry.m_aabb.position = Vector3( file->get_float(), file->get_float(), file->get_float() );
		geometry.m_aabb.size = Vector3( file->get_float(), file->get_float(), file->get_float() );
		geometry.m_vertices.resize( ( int )vertex_count );
		geometry.m_metadata.resize( ( int )metadata_count );
		geometry.m_indices.resize( ( int )index_count );
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
			const int32_t index = ( int32_t )file->get_32();
			if ( index < 0 || ( uint32_t )index >= vertex_count ) {
				return false;
			}
			pIndices[i] = index;
		}
		if ( file->get_error() != OK ) {
			return false;
		}
		m_szMeshGeometry[( size_t )type] = geometry;
	}
	if ( file->get_position() != file->get_length() ) {
		return false;
	}
	LOG( INFO, "Loaded cooked clipmap geometry pack for mesh size ", m_nMeshSize );
	return true;
}

bool Terrain3DMesher::writeRuntimeGeometryPack( const String &rDataDirectory ) const {
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
	const String temporary = path + String( ".tmp" );
	Ref<FileAccess> file = FileAccess::open( temporary, FileAccess::WRITE );
	if ( file.is_null() ) {
		return false;
	}
	file->store_64( s_nClipmapPackMagic );
	file->store_32( s_nClipmapPackVersion );
	file->store_32( ( uint32_t )m_nMeshSize );
	file->store_32( MESH_TYPE_COUNT );
	file->store_32( 0u );
	for ( const t3dClipmapGeometry_t &geometry : m_szMeshGeometry ) {
		file->store_32( ( uint32_t )geometry.m_vertices.size() );
		file->store_32( ( uint32_t )geometry.m_metadata.size() );
		file->store_32( ( uint32_t )geometry.m_indices.size() );
		file->store_32( 0u );
		file->store_float( geometry.m_aabb.position.x );
		file->store_float( geometry.m_aabb.position.y );
		file->store_float( geometry.m_aabb.position.z );
		file->store_float( geometry.m_aabb.size.x );
		file->store_float( geometry.m_aabb.size.y );
		file->store_float( geometry.m_aabb.size.z );
		for ( const Vector3 &vertex : geometry.m_vertices ) {
			file->store_float( vertex.x );
			file->store_float( vertex.y );
			file->store_float( vertex.z );
		}
		for ( const Vector2 &metadata : geometry.m_metadata ) {
			file->store_float( metadata.x );
			file->store_float( metadata.y );
		}
		for ( const int32_t index : geometry.m_indices ) {
			file->store_32( ( uint32_t )index );
		}
	}
	const Error error = file->get_error();
	file->close();
	if ( error != OK ) {
		DirAccess::remove_absolute( temporary );
		return false;
	}
	const String backup = path + String( ".bak" );
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


void Terrain3DMesher::generateMeshTypes( void ) {
	clearMeshTypes();
	LOG( INFO, "Generating two immutable CPU clipmap geometry templates for size ", m_nMeshSize );
	m_szMeshGeometry[LOD0_MESH] = generateCombinedGeometry( true );
	m_szMeshGeometry[OUTER_RING_MESH] = generateCombinedGeometry( false );
}

size_t Terrain3DMesher::estimateGeometryPayloadBytes( const int nFirstLod, const int nLastLod ) const {
	size_t bytes = 0u;
	for ( int lod = nFirstLod; lod <= nLastLod; ++lod ) {
		const t3dClipmapGeometry_t &geometry = m_szMeshGeometry[( size_t )( lod == 0 ? LOD0_MESH : OUTER_RING_MESH )];
		bytes += ( size_t )( geometry.m_vertices.size() ) * sizeof( Vector3 );
		bytes += ( size_t )( geometry.m_metadata.size() ) * sizeof( Vector2 );
		bytes += ( size_t )( geometry.m_indices.size() ) * sizeof( int32_t );
	}
	return bytes;
}

size_t Terrain3DMesher::getVisibleGeometryPayloadBytes( void ) const {
	return estimateGeometryPayloadBytes( 0, MESH_TYPE_COUNT - 1 );
}

size_t Terrain3DMesher::getStreamingBufferBytes( void ) const {
	return sizeof( m_szLastLodPositions ) + sizeof( m_szLastLodTestX ) + sizeof( m_szLastLodTestZ ) +
			sizeof( m_fLodScales ) + sizeof( m_fLodSnapSteps ) + sizeof( m_fLodNextSnapSteps );
}

int Terrain3DMesher::getLodTier( const int nLod ) const {
	if ( nLod < m_runtimeConfig.m_nNearShaderLodCount ) return 0;
	if ( nLod < m_runtimeConfig.m_nNearShaderLodCount + m_runtimeConfig.m_nMiddleShaderLodCount ) return 1;
	return 2;
}

bool Terrain3DMesher::lodCastsShadows( const int nLod ) const {
	if ( m_castShadows == GeometryInstance3D::SHADOW_CASTING_SETTING_OFF ) return false;
	const int tier = getLodTier( nLod );
	if ( tier < 0 || tier >= 3 || !m_runtimeConfig.m_szTierShadowEnabled[( size_t )( tier )] ) return false;
	int limit = m_runtimeConfig.m_nShadowLodCount;
	if ( m_runtimeConfig.m_szTierShadowLodCounts[( size_t )( tier )] >= 0 ) {
		limit = m_runtimeConfig.m_szTierShadowLodCounts[( size_t )( tier )];
	}
	return limit > 0 && nLod < limit;
}

int Terrain3DMesher::getRenderBatchCountForTier( const int nTier ) const {
	for ( int lod = 0; lod < m_nClipmapLevelCount; ++lod ) if ( getLodTier( lod ) == nTier ) return 1;
	return 0;
}

int Terrain3DMesher::getRenderBatchCount( void ) const {
	return getRenderBatchCountForTier( 0 ) + getRenderBatchCountForTier( 1 ) + getRenderBatchCountForTier( 2 );
}

int Terrain3DMesher::getShadowBatchCountForTier( const int nTier ) const {
	for ( int lod = 0; lod < m_nClipmapLevelCount; ++lod ) if ( getLodTier( lod ) == nTier && lodCastsShadows( lod ) ) return 1;
	return 0;
}

int Terrain3DMesher::getShadowBatchCount( void ) const {
	return getShadowBatchCountForTier( 0 ) + getShadowBatchCountForTier( 1 ) + getShadowBatchCountForTier( 2 );
}

void Terrain3DMesher::generateClipmap( void ) {
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
	m_nClipmapLevelCount = CLAMP( m_nLods + m_nTessellationLevel, 0, s_nMaxClipmapLevels );
}
void Terrain3DMesher::generateOffsetData( void ) {
	LOG( INFO, "Computing fixed clipmap positioning offsets" );
	m_szTilePosLod0 = {
		Vector3( 0, 0, m_nMeshSize ),
		Vector3( m_nMeshSize, 0, m_nMeshSize ),
		Vector3( m_nMeshSize, 0, 0 ),
		Vector3( m_nMeshSize, 0, -m_nMeshSize ),
		Vector3( m_nMeshSize, 0, -m_nMeshSize * 2 ),
		Vector3( 0, 0, -m_nMeshSize * 2 ),
		Vector3( -m_nMeshSize, 0, -m_nMeshSize * 2 ),
		Vector3( -m_nMeshSize * 2, 0, -m_nMeshSize * 2 ),
		Vector3( -m_nMeshSize * 2, 0, -m_nMeshSize ),
		Vector3( -m_nMeshSize * 2, 0, 0 ),
		Vector3( -m_nMeshSize * 2, 0, m_nMeshSize ),
		Vector3( -m_nMeshSize, 0, m_nMeshSize ),
		s_v3Zero,
		Vector3( -m_nMeshSize, 0, 0 ),
		Vector3( 0, 0, -m_nMeshSize ),
		Vector3( -m_nMeshSize, 0, -m_nMeshSize ),
	};
	m_szTrimAPos = { Vector3( m_nMeshSize * 2, 0, -m_nMeshSize * 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, -m_nMeshSize * 2 - 2 ) };
	m_szTrimBPos = { Vector3( -m_nMeshSize * 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, m_nMeshSize * 2 ) };
	m_szTilePos = {
		Vector3( 2, 0, m_nMeshSize + 2 ),
		Vector3( m_nMeshSize + 2, 0, m_nMeshSize + 2 ),
		Vector3( m_nMeshSize + 2, 0, -2 ),
		Vector3( m_nMeshSize + 2, 0, -m_nMeshSize - 2 ),
		Vector3( m_nMeshSize + 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize - 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, -m_nMeshSize + 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, m_nMeshSize + 2 ),
		Vector3( -m_nMeshSize + 2, 0, m_nMeshSize + 2 ),
	};
	m_fOffsetA = ( real_t )( m_nMeshSize * 2 ) + 2.f;
	m_fOffsetB = ( real_t )( m_nMeshSize * 2 ) + 4.f;
	m_fOffsetC = ( real_t )( m_nMeshSize * 2 ) + 6.f;
	m_szEdgePos = { Vector3( m_fOffsetA, m_fOffsetA, -m_fOffsetB ), Vector3( m_fOffsetB, -m_fOffsetB, -m_fOffsetC ) };
	m_szFillAPos = { Vector3( m_nMeshSize - 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize - 2, 0, m_nMeshSize + 2 ) };
	m_szFillBPos = { Vector3( m_nMeshSize + 2, 0, m_nMeshSize - 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, -m_nMeshSize - 2 ) };
}

void Terrain3DMesher::generateLodCache( void ) {
	m_nClipmapLevelCount = CLAMP( m_nLods + m_nTessellationLevel, 0, s_nMaxClipmapLevels );
	refreshImmediateLodCount();
	for ( int lod = 0; lod < s_nMaxClipmapLevels; ++lod ) {
		m_szLastLodPositions[( size_t )( lod )] = s_v2Max;
		m_szLastLodTestX[( size_t )( lod )] = -1;
		m_szLastLodTestZ[( size_t )( lod )] = -1;
	}
	const real_t fDensity = ( real_t )( 1.0 ) / ( real_t )( ( uint64_t )( 1 ) << CLAMP( m_nTessellationLevel, 0, 30 ) );
	const real_t fBaseSpacing = m_fVertexSpacing * fDensity;
	for ( int lod = 0; lod < m_nClipmapLevelCount; ++lod ) {
		const real_t fMultiplier = ( real_t )( ( uint64_t )( 1 ) << CLAMP( lod, 0, 30 ) );
		m_fLodScales[( size_t )( lod )] = fBaseSpacing * fMultiplier;
		m_fLodSnapSteps[( size_t )( lod )] = m_fLodScales[( size_t )( lod )] * 2.0f;
		m_fLodNextSnapSteps[( size_t )( lod )] = m_fLodSnapSteps[( size_t )( lod )] * 2.0f;
	}
}


void Terrain3DMesher::clearClipmap( void ) {
	m_nClipmapLevelCount = 0;
	refreshImmediateLodCount();
}

int Terrain3DMesher::getImmediateLodCount( void ) const {
	return m_runtimeConfig.m_nImmediateLodCount;
}

void Terrain3DMesher::refreshImmediateLodCount( void ) {
	const int continuity_ring = m_runtimeConfig.m_nMiddleShaderLodCount > 0 ? 1 : 0;
	m_runtimeConfig.m_nImmediateLodCount =
			CLAMP( m_runtimeConfig.m_nNearShaderLodCount + continuity_ring, 0, m_nClipmapLevelCount );
}

void Terrain3DMesher::compileTierRuntimeConfig( const int nNearShaderLodCount,
		const int nMiddleShaderLodCount,
		const int nClipmapUpdateBudget ) {
	const int total_levels = MAX( 0, m_nLods + m_nTessellationLevel );
	m_runtimeConfig.m_nNearShaderLodCount = CLAMP( nNearShaderLodCount, 0, total_levels );
	m_runtimeConfig.m_nMiddleShaderLodCount =
			CLAMP( nMiddleShaderLodCount, 0, total_levels - m_runtimeConfig.m_nNearShaderLodCount );
	m_runtimeConfig.m_nClipmapUpdateBudget = CLAMP( nClipmapUpdateBudget, 1, s_nMaxClipmapLevels );
	refreshImmediateLodCount();
}

void Terrain3DMesher::compileShadowRuntimeConfig( const int nShadowLodCount,
		const eastl::array<bool, 3> &bTierShadowEnabled,
		const eastl::array<int, 3> &nTierShadowLodCounts ) {
	m_runtimeConfig.m_nShadowLodCount = CLAMP( nShadowLodCount, 0, s_nMaxClipmapLevels );
	for ( int tier = 0; tier < 3; ++tier ) {
		m_runtimeConfig.m_szTierShadowEnabled[tier] = bTierShadowEnabled[( size_t )( tier )];
		m_runtimeConfig.m_szTierShadowLodCounts[tier] =
				CLAMP( nTierShadowLodCounts[( size_t )( tier )], -1, s_nMaxClipmapLevels );
	}
}

void Terrain3DMesher::clearMeshTypes( void ) {
	for ( t3dClipmapGeometry_t &geometry : m_szMeshGeometry ) geometry = t3dClipmapGeometry_t{};
}

void Terrain3DMesher::initialize( Terrain3D *const pTerrain,
		const int nMeshSize,
		const int nLods,
		const int nTessellationLevel,
		const real_t fVertexSpacing,
		const int nNearShaderLodCount,
		const int nMiddleShaderLodCount,
		const int nClipmapUpdateBudget,
		const GeometryInstance3D::ShadowCastingSetting castShadows,
		const int nShadowLodCount,
		const eastl::array<bool, 3> &bTierShadowEnabled,
		const eastl::array<int, 3> &nTierShadowLodCounts,
		const bool bRequireCookedGeometryPack ) {
	if ( !pTerrain ) return;
	m_pTerrain = pTerrain;
	m_bRequireCookedGeometryPack = bRequireCookedGeometryPack;
	m_nLods = nLods;
	m_nTessellationLevel = nTessellationLevel;
	m_nMeshSize = nMeshSize;
	m_fVertexSpacing = fVertexSpacing;
	m_castShadows = castShadows;
	compileTierRuntimeConfig( nNearShaderLodCount, nMiddleShaderLodCount, nClipmapUpdateBudget );
	compileShadowRuntimeConfig( nShadowLodCount, bTierShadowEnabled, nTierShadowLodCounts );
	generateClipmap();
	generateLodCache();
	m_iDeferredLodCursor = getImmediateLodCount();
	updateAabbs();
	resetTargetPosition();
	snap();
}

void Terrain3DMesher::destroy( void ) {
	clearClipmap();
	clearMeshTypes();
	m_pTerrain = nullptr;
	for ( int lod = 0; lod < s_nMaxClipmapLevels; ++lod ) {
		m_szLastLodPositions[( size_t )( lod )] = s_v2Max;
		m_szLastLodTestX[( size_t )( lod )] = -1;
		m_szLastLodTestZ[( size_t )( lod )] = -1;
	}
	std::memset( m_fLodScales.data(), 0, sizeof( m_fLodScales ) );
	std::memset( m_fLodSnapSteps.data(), 0, sizeof( m_fLodSnapSteps ) );
	std::memset( m_fLodNextSnapSteps.data(), 0, sizeof( m_fLodNextSnapSteps ) );
}

void Terrain3DMesher::setTierLayout( const int nNearShaderLodCount,
		const int nMiddleShaderLodCount,
		const int nClipmapUpdateBudget,
		const bool bForceFullSnap ) {
	const int total_levels = MAX( 0, m_nLods + m_nTessellationLevel );
	const int near_count = CLAMP( nNearShaderLodCount, 0, total_levels );
	const int middle_count = CLAMP( nMiddleShaderLodCount, 0, total_levels - near_count );
	const int update_budget = CLAMP( nClipmapUpdateBudget, 1, s_nMaxClipmapLevels );
	const int old_immediate_count = getImmediateLodCount();
	const bool layout_changed = m_runtimeConfig.m_nNearShaderLodCount != near_count ||
			m_runtimeConfig.m_nMiddleShaderLodCount != middle_count;
	const bool budget_changed = m_runtimeConfig.m_nClipmapUpdateBudget != update_budget;
	if ( !layout_changed && !budget_changed ) return;
	compileTierRuntimeConfig( near_count, middle_count, update_budget );
	m_iDeferredLodCursor = getImmediateLodCount();
	m_bForceFullSnap = bForceFullSnap || old_immediate_count != m_iDeferredLodCursor;
	if ( layout_changed ) { resetTargetPosition(); snap(); }
}

void Terrain3DMesher::setCastShadows( const GeometryInstance3D::ShadowCastingSetting castShadows ) { m_castShadows = castShadows; }
void Terrain3DMesher::setShadowLodCount( const int nCount ) {
	m_runtimeConfig.m_nShadowLodCount = CLAMP( nCount, 0, s_nMaxClipmapLevels );
}
void Terrain3DMesher::setTierShadowEnabled( const eastl::array<bool, 3> &bEnabled ) {
	for ( int tier = 0; tier < 3; ++tier ) {
		m_runtimeConfig.m_szTierShadowEnabled[tier] = bEnabled[( size_t )( tier )];
	}
}
void Terrain3DMesher::setTierShadowLodCounts( const eastl::array<int, 3> &nCounts ) {
	for ( int tier = 0; tier < 3; ++tier ) {
		m_runtimeConfig.m_szTierShadowLodCounts[tier] = CLAMP( nCounts[( size_t )( tier )], -1, s_nMaxClipmapLevels );
	}
}

void Terrain3DMesher::resetTargetPosition( void ) {
	for ( int lod = 0; lod < s_nMaxClipmapLevels; ++lod ) {
		m_szLastLodPositions[( size_t )( lod )] = s_v2Max;
		m_szLastLodTestX[( size_t )( lod )] = -1;
		m_szLastLodTestZ[( size_t )( lod )] = -1;
	}
	m_bForceFullSnap = true;
	m_iDeferredLodCursor = getImmediateLodCount();
}

void Terrain3DMesher::snap( void ) {
	if ( m_pTerrain ) snap( m_pTerrain->getClipmapTargetPosition() );
}

bool Terrain3DMesher::snapLevel( const int nLod, const Vector3 &rSnappedTarget ) {
	const real_t fSnapStep = m_fLodSnapSteps[( size_t )( nLod )];
	const real_t fNextSnapStep = m_fLodNextSnapSteps[( size_t )( nLod )];
	Vector3 pos = s_v3Zero;
	pos.x = Math::round( rSnappedTarget.x / fSnapStep ) * fSnapStep;
	pos.z = Math::round( rSnappedTarget.z / fSnapStep ) * fSnapStep;
	const real_t fNextX = Math::round( rSnappedTarget.x / fNextSnapStep ) * fNextSnapStep;
	const real_t fNextZ = Math::round( rSnappedTarget.z / fNextSnapStep ) * fNextSnapStep;
	const int test_x = CLAMP( ( int )( Math::round( ( pos.x - fNextX ) / fSnapStep ) ) + 1, 0, 2 );
	const int test_z = CLAMP( ( int )( Math::round( ( pos.z - fNextZ ) / fSnapStep ) ) + 1, 0, 2 );
	const Vector2 snapped_lod_pos( pos.x, pos.z );
	if ( m_szLastLodPositions[( size_t )( nLod )] == snapped_lod_pos && m_szLastLodTestX[( size_t )( nLod )] == test_x &&
			m_szLastLodTestZ[( size_t )( nLod )] == test_z ) return false;
	m_szLastLodPositions[( size_t )( nLod )] = snapped_lod_pos;
	m_szLastLodTestX[( size_t )( nLod )] = test_x;
	m_szLastLodTestZ[( size_t )( nLod )] = test_z;
	const int tier = getLodTier( nLod );
	if ( tier == 0 ) ++m_nNearUpdatesLastSnap;
	else if ( tier == 1 ) ++m_nMiddleUpdatesLastSnap;
	else ++m_nFarUpdatesLastSnap;
	return true;
}

void Terrain3DMesher::snap( const Vector3 &rTargetPos ) {
	if ( m_nClipmapLevelCount <= 0 ) return;
	m_nNearUpdatesLastSnap = m_nMiddleUpdatesLastSnap = m_nFarUpdatesLastSnap = 0u;
	m_nTeleportsLastSnap = 0u;
	m_nBufferUploadsLastSnap = 0u;
	const real_t base_spacing = m_fLodScales[0] > 0.0f ? m_fLodScales[0] : m_fVertexSpacing;
	const Vector3 snapped_target( Math::floor( rTargetPos.x / base_spacing ) * base_spacing,
			rTargetPos.y,
			Math::floor( rTargetPos.z / base_spacing ) * base_spacing );
	const int immediate_count = getImmediateLodCount();
	for ( int lod = 0; lod < immediate_count; ++lod ) snapLevel( lod, snapped_target );
	if ( m_bForceFullSnap ) {
		for ( int lod = immediate_count; lod < m_nClipmapLevelCount; ++lod ) snapLevel( lod, snapped_target );
		m_bForceFullSnap = false;
		m_iDeferredLodCursor = immediate_count;
		return;
	}
	const int deferred_count = m_nClipmapLevelCount - immediate_count;
	if ( deferred_count <= 0 ) return;
	int updated = 0;
	int scanned = 0;
	int lod = CLAMP( m_iDeferredLodCursor, immediate_count, m_nClipmapLevelCount - 1 );
	while ( scanned < deferred_count && updated < m_runtimeConfig.m_nClipmapUpdateBudget ) {
		if ( snapLevel( lod, snapped_target ) ) ++updated;
		++scanned;
		if ( ++lod >= m_nClipmapLevelCount ) lod = immediate_count;
	}
	m_iDeferredLodCursor = lod;
}

int Terrain3DMesher::fillRdPatches( t3dRdPatch_t *const pPatches, const int nCapacity ) const {
	if ( !pPatches || nCapacity <= 0 ) return 0;
	const int count = MIN( MIN( m_nClipmapLevelCount, nCapacity ), s_nMaxClipmapLevels );
	int written = 0;
	for ( int lod = 0; lod < count; ++lod ) {
		const Vector2 &position = m_szLastLodPositions[( size_t )( lod )];
		if ( position == s_v2Max || m_szLastLodTestX[( size_t )( lod )] < 0 || m_szLastLodTestZ[( size_t )( lod )] < 0 ) continue;
		t3dRdPatch_t &patch = pPatches[written++];
		patch.m_fOriginX = ( float )( position.x );
		patch.m_fOriginZ = ( float )( position.y );
		patch.m_fScale = ( float )( m_fLodScales[( size_t )( lod )] );
		patch.m_nLod = ( uint32_t )( lod );
		patch.m_nTier = ( uint32_t )( getLodTier( lod ) );
		patch.m_nStitchX = ( uint32_t )( m_szLastLodTestX[( size_t )( lod )] );
		patch.m_nStitchZ = ( uint32_t )( m_szLastLodTestZ[( size_t )( lod )] );
		patch.m_nFlags = lodCastsShadows( lod ) ? 1u : 0u;
		patch.m_fBoundsMinY = ( float )( m_fClipmapAabbMinY );
		patch.m_fBoundsMaxY = ( float )( m_fClipmapAabbMinY + m_fClipmapAabbSizeY );
	}
	return written;
}

void Terrain3DMesher::updateAabbs( const real_t fCullMargin, const Vector2 &rHeightRange ) {
	if ( !m_pTerrain || !m_pTerrain->getData() ) return;
	const real_t cull_margin = fCullMargin < 0.0f ? m_pTerrain->getCullMargin() : fCullMargin;
	const Vector2 height_range = rHeightRange.x == FLT_MAX ? m_pTerrain->getData()->getHeightRange() : rHeightRange;
	const t3dClipmapVerticalBounds_t vertical_bounds = t3dClipmapVerticalBounds(
			( double )( height_range.x ), ( double )( height_range.y ), ( double )( cull_margin ) );
	m_fClipmapAabbMinY = ( real_t )( vertical_bounds.m_fMinY );
	m_fClipmapAabbSizeY = ( real_t )( vertical_bounds.m_fSizeY );
}
