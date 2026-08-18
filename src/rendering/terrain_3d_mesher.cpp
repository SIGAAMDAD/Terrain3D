// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <godot_cpp/classes/rendering_server.hpp>
#include <EASTL/unordered_map.h>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/version.hpp>

#include <algorithm>
#include <unordered_map>

#include "core/logger.h"
#include "terrain_3d.h"
#include "rendering/terrain_3d_clipmap_bounds.h"
#include "rendering/terrain_3d_mesher.h"

typedef struct t3dclipmapVertexKey_s {
	int32_t x = 0;
	int32_t z = 0;
	uint8_t part = 0;

	bool operator==( const t3dclipmapVertexKey_s &rOther ) const {
		return x == rOther.x && z == rOther.z && part == rOther.part;
	}
} t3dclipmapVertexKey_t;

typedef struct t3dclipmapVertexKeyHash_s {
	size_t operator()( const t3dclipmapVertexKey_t &rKey ) const {
		uint64_t hash = ( uint64_t ) ( ( uint32_t ) ( rKey.x ) ) * 0x9E3779B185EBCA87ull;
		hash ^= ( uint64_t ) ( ( uint32_t ) ( rKey.z ) ) * 0xC2B2AE3D27D4EB4Full;
		hash ^= ( uint64_t ) ( rKey.part ) * 0x165667B19E3779F9ull;
		hash ^= hash >> 29u;
		return ( size_t ) ( hash );
	}
} t3dclipmapVertexKeyHash_t;

void weldClipmapGeometry(
	PackedVector3Array &rVertices, PackedVector2Array &rMetadata,
	PackedInt32Array &rIndices
) {
	eastl::unordered_map<t3dclipmapVertexKey_t, int, t3dclipmapVertexKeyHash_t> unique_vertices;
	unique_vertices.reserve( ( size_t ) ( rVertices.size() ) );
	PackedVector3Array welded_vertices;
	PackedVector2Array welded_metadata;
	PackedInt32Array remap;
	remap.resize( rVertices.size() );
	for ( int vertex_index = 0; vertex_index < rVertices.size(); ++vertex_index ) {
		const Vector3 vertex = rVertices[vertex_index];
		const Vector2 metadata = rMetadata[vertex_index];
		const t3dclipmapVertexKey_t key{
			( int32_t ) ( Math::round( vertex.x ) ), ( int32_t ) ( Math::round( vertex.z ) ), ( uint8_t ) ( Math::round( metadata.x ) )
		};
		const eastl::unordered_map<t3dclipmapVertexKey_t, int, t3dclipmapVertexKeyHash_t>::const_iterator found = unique_vertices.find( key );
		if ( found != unique_vertices.end() ) {
			remap.set( vertex_index, found->second );
			continue;
		}
		const int welded_index = welded_vertices.size();
		unique_vertices.emplace( key, welded_index );
		welded_vertices.push_back( vertex );
		welded_metadata.push_back( metadata );
		remap.set( vertex_index, welded_index );
	}
	for ( int index = 0; index < rIndices.size(); ++index ) {
		rIndices.set( index, remap[rIndices[index]] );
	}
	rVertices = welded_vertices;
	rMetadata = welded_metadata;
}

///////////////////////////
// Private Functions
///////////////////////////

void T3DMesher::appendGrid(
	PackedVector3Array &rVertices, PackedVector2Array &rMetadata,
	PackedInt32Array &rIndices, const Vector2i &rSize, const Vector3 &rOrigin,
	const float fPartId, const bool bStandardGrid
) {
	const int vertex_base = rVertices.size();
	for ( int y = 0; y <= rSize.y; ++y ) {
		for ( int x = 0; x <= rSize.x; ++x ) {
			rVertices.push_back( rOrigin + Vector3( x, 0.0f, y ) );
			rMetadata.push_back( Vector2( fPartId, 0.0f ) );
		}
	}

	for ( int y = 0; y < rSize.y; ++y ) {
		for ( int x = 0; x < rSize.x; ++x ) {
			const int bottom_left = vertex_base + y * ( rSize.x + 1 ) + x;
			const int bottom_right = bottom_left + 1;
			const int top_left = vertex_base + ( y + 1 ) * ( rSize.x + 1 ) + x;
			const int top_right = top_left + 1;

			if ( ( x + y ) % 2 == 0 || bStandardGrid ) {
				rIndices.push_back( bottom_left );
				rIndices.push_back( top_right );
				rIndices.push_back( top_left );
				rIndices.push_back( bottom_left );
				rIndices.push_back( bottom_right );
				rIndices.push_back( top_right );
			} else {
				rIndices.push_back( bottom_left );
				rIndices.push_back( bottom_right );
				rIndices.push_back( top_left );
				rIndices.push_back( top_left );
				rIndices.push_back( bottom_right );
				rIndices.push_back( top_right );
			}
		}
	}
}

T3DMesher::t3dclipmapGeometry_t T3DMesher::generateCombinedGeometry( const bool bLod0 ) {
	t3dclipmapGeometry_t geometry;
	PackedVector3Array &vertices = geometry.vertices;
	PackedVector2Array &metadata = geometry.metadata;
	PackedInt32Array &indices = geometry.indices;

	if ( bLod0 ) {
		for ( const Vector3 &position : m_szTilePosLod0 ) {
			appendGrid( vertices, metadata, indices, V2I( m_nMeshSize ), position, TERRAIN3D_CLIPMAP_PART_STATIC, true );
		}
		for ( const Vector3 &position : m_szTrimAPos ) {
			appendGrid( vertices, metadata, indices, Vector2i( 2, m_nMeshSize * 4 + 2 ), position,
				TERRAIN3D_CLIPMAP_PART_STATIC, true );
		}
		for ( const Vector3 &position : m_szTrimBPos ) {
			appendGrid( vertices, metadata, indices, Vector2i( m_nMeshSize * 4 + 2, 2 ), position,
				TERRAIN3D_CLIPMAP_PART_STATIC, true );
		}
	} else {
		for ( const Vector3 &position : m_szTilePos ) {
			appendGrid( vertices, metadata, indices, V2I( m_nMeshSize ), position, TERRAIN3D_CLIPMAP_PART_STATIC, false );
		}
		for ( const Vector3 &position : m_szFillAPos ) {
			appendGrid( vertices, metadata, indices, Vector2i( 4, m_nMeshSize ), position,
				TERRAIN3D_CLIPMAP_PART_STATIC, false );
		}
		for ( const Vector3 &position : m_szFillBPos ) {
			appendGrid( vertices, metadata, indices, Vector2i( m_nMeshSize, 4 ), position,
				TERRAIN3D_CLIPMAP_PART_STATIC, false );
		}
	}

	// Edge placement is baked for stitch state (1, 1). INSTANCE_CUSTOM.rg
	// carries the live 3x3 stitch state and the vertex shader applies only the
	// small edge translation delta. This keeps one immutable complete-ring mesh
	// reusable by every LOD in a shader tier.
	for ( int i = 0; i < TERRAIN3D_PAIR_INSTANCES; ++i ) {
		Vector3 edge_a;
		edge_a.z = -m_fOffsetA - 2.0f;
		edge_a.x = m_szEdgePos[( size_t ) ( i )][1];
		appendGrid( vertices, metadata, indices, Vector2i( 2, m_nMeshSize * 4 + 8 ), edge_a,
			i == 0 ? TERRAIN3D_CLIPMAP_PART_EDGE_A0 : TERRAIN3D_CLIPMAP_PART_EDGE_A1, bLod0 );

		Vector3 edge_b;
		edge_b.z = m_szEdgePos[( size_t ) ( i )][1];
		edge_b.x = -m_fOffsetA;
		appendGrid( vertices, metadata, indices, Vector2i( m_nMeshSize * 4 + 4, 2 ), edge_b,
			i == 0 ? TERRAIN3D_CLIPMAP_PART_EDGE_B0 : TERRAIN3D_CLIPMAP_PART_EDGE_B1, bLod0 );
	}

	const int source_vertex_count = vertices.size();
	weldClipmapGeometry( vertices, metadata, indices );
	const real_t fRadius = m_fOffsetC + 2.0f;
	geometry.aabb = AABB( Vector3( -fRadius, -0.1f, -fRadius ), Vector3( fRadius * 2.0f, 0.2f, fRadius * 2.0f ) );
	LOG( T3DTerrain::INFO, "Generated ", bLod0 ? "LOD0" : "outer-ring", " clipmap geometry: ", vertices.size(),
		" welded vertices from ", source_vertex_count, " source vertices, ", indices.size() / 3, " triangles" );
	return geometry;
}

void T3DMesher::generateMeshTypes( void ) {
	clearMeshTypes();
	LOG( T3DTerrain::INFO, "Generating two immutable complete clipmap geometry templates for size ", m_nMeshSize );
	m_szMeshGeometry[LOD0_MESH] = generateCombinedGeometry( true );
	m_szMeshGeometry[OUTER_RING_MESH] = generateCombinedGeometry( false );
	if ( !m_bStateMeshBatching ) {
		for ( int type = 0; type < MESH_TYPE_COUNT; ++type ) {
			const t3dclipmapGeometry_t &geometry = m_szMeshGeometry[( size_t ) ( type )];
			m_szMeshRids[( size_t ) ( type )] = instantiateMesh( geometry.vertices, geometry.metadata, geometry.indices, geometry.aabb );
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
	PackedVector3Array vertices;
	PackedVector2Array metadata;
	PackedInt32Array indices;
	for ( int lod = nFirstLod; lod <= nLastLod; ++lod ) {
		const t3dclipmapGeometry_t &geometry = m_szMeshGeometry[( size_t ) ( lod == 0 ? LOD0_MESH : OUTER_RING_MESH )];
		const int vertex_base = vertices.size();
		for ( int vertex_index = 0; vertex_index < geometry.vertices.size(); ++vertex_index ) {
			vertices.push_back( geometry.vertices[vertex_index] );
			metadata.push_back( Vector2( geometry.metadata[vertex_index].x, ( float ) ( lod ) ) );
		}
		for ( const int32_t index : geometry.indices ) {
			indices.push_back( vertex_base + index );
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
	LOG( T3DTerrain::INFO, "Generated state clipmap mesh for LODs ", nFirstLod, "-", nLastLod, ": ", vertices.size(),
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
		const t3dclipmapGeometry_t &geometry = m_szMeshGeometry[( size_t ) ( lod == 0 ? LOD0_MESH : OUTER_RING_MESH )];
		const size_t vertex_count = ( size_t ) ( geometry.vertices.size() );
		bytes += vertex_count * ( sizeof( float ) * ( 3u + 2u ) );
		if ( bIncludeBasis ) {
			bytes += vertex_count * ( sizeof( float ) * ( 3u + 4u ) );
		}
		bytes += ( size_t ) ( geometry.indices.size() ) * sizeof( int32_t );
	}
	return bytes;
}

size_t T3DMesher::getVisibleGeometryPayloadBytes( void ) const {
	if ( !m_bStateMeshBatching ) {
		return estimateGeometryPayloadBytes( 0, 1, true );
	}
	size_t bytes = 0u;
	for ( int batch_index = 0; batch_index < m_nRenderBatchCount; ++batch_index ) {
		const t3dclipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( batch_index )];
		if ( batch.bOwnsMesh ) {
			bytes += estimateGeometryPayloadBytes( batch.firstLod, batch.lastLod, false );
		}
	}
	return bytes;
}

size_t T3DMesher::getShadowGeometryPayloadBytes( void ) const {
	return m_bSeparateShadowStateBatch && m_shadowStateBatch.bOwnsMesh ?
		estimateGeometryPayloadBytes( m_shadowStateBatch.firstLod, m_shadowStateBatch.lastLod, false ) : 0u;
}

size_t T3DMesher::getStreamingBufferBytes( void ) const {
	size_t bytes = ( size_t ) ( m_lodStateBuffer.size() ) * sizeof( float ) * 4u;
	for ( int batch_index = 0; batch_index < m_nRenderBatchCount; ++batch_index ) {
		bytes += ( size_t ) ( m_szRenderBatches[( size_t ) ( batch_index )].transformBuffer.size() ) * sizeof( float );
	}
	bytes += ( size_t ) ( m_shadowStateBatch.transformBuffer.size() ) * sizeof( float );
	return bytes;
}

T3DMesher::t3dclipmapBatch_t T3DMesher::createBatch(
	const RID &rMesh, const int nCount,
	const int nFirstLod, const int nLastLod, const int nTier, const bool bCastsShadows
) {
	t3dclipmapBatch_t batch;
	if ( !rMesh.is_valid() || nCount <= 0 || nCount > TERRAIN3D_MAX_BATCH_INSTANCES ) {
		LOG( T3DTerrain::ERROR, "Invalid clipmap render batch request. Mesh valid: ", rMesh.is_valid(), ", count: ", nCount );
		return batch;
	}

	batch.multimesh = RS->multimesh_create();
	if ( !batch.multimesh.is_valid() ) {
		LOG( T3DTerrain::ERROR, "Failed to create clipmap MultiMesh" );
		return batch;
	}
	RS->multimesh_allocate_data( batch.multimesh, nCount, RenderingServer::MULTIMESH_TRANSFORM_3D,
		false, true, false );
	RS->multimesh_set_mesh( batch.multimesh, rMesh );
	RS->multimesh_set_visible_instances( batch.multimesh, nCount );
	batch.transformBuffer.resize( ( int64_t ) ( nCount ) * TERRAIN3D_MULTIMESH_STRIDE_FLOATS );
	batch.transformBuffer.fill( 0.0f );
	batch.instance = RS->instance_create2( batch.multimesh, m_scenario );
	if ( !batch.instance.is_valid() ) {
		LOG( T3DTerrain::ERROR, "Failed to create clipmap MultiMesh instance" );
		RS->free_rid( batch.multimesh );
		batch.multimesh = RID();
		return batch;
	}
	batch.nCount = ( uint8_t ) ( nCount );
	batch.firstLod = ( uint8_t ) ( nFirstLod );
	batch.lastLod = ( uint8_t ) ( nLastLod );
	batch.tier = ( uint8_t ) ( nTier );
	batch.bCastsShadows = bCastsShadows;
	return batch;
}

T3DMesher::t3dclipmapBatch_t T3DMesher::createStateBatch(
	const int nFirstLod,
	const int nLastLod, const int nTier, const bool bCastsShadows
) {
	t3dclipmapBatch_t batch;
	if ( nFirstLod < 0 || nLastLod < nFirstLod || nLastLod >= m_nClipmapLevelCount ) {
		LOG( T3DTerrain::ERROR, "Invalid state clipmap batch range: ", nFirstLod, "-", nLastLod );
		return batch;
	}
	batch.mesh = instantiateStateMesh( nFirstLod, nLastLod );
	batch.bOwnsMesh = true;
	if ( !batch.mesh.is_valid() ) {
		return batch;
	}
	batch.instance = RS->instance_create2( batch.mesh, m_scenario );
	if ( !batch.instance.is_valid() ) {
		RS->free_rid( batch.mesh );
		batch.mesh = RID();
		return batch;
	}
	batch.nCount = ( uint8_t ) ( nLastLod - nFirstLod + 1 );
	batch.firstLod = ( uint8_t ) ( nFirstLod );
	batch.lastLod = ( uint8_t ) ( nLastLod );
	batch.tier = ( uint8_t ) ( nTier );
	batch.bCastsShadows = bCastsShadows;
	return batch;
}

void T3DMesher::freeBatch( t3dclipmapBatch_t &rBatch ) {
	if ( rBatch.instance.is_valid() ) {
		RS->free_rid( rBatch.instance );
	}
	if ( rBatch.multimesh.is_valid() ) {
		RS->free_rid( rBatch.multimesh );
	}
	if ( rBatch.bOwnsMesh && rBatch.mesh.is_valid() ) {
		RS->free_rid( rBatch.mesh );
	}
	rBatch = t3dclipmapBatch_t{};
}

void T3DMesher::setBatchInstance(
	t3dclipmapBatch_t &rBatch, const int iIndex,
	const Transform3D &rTransform, const int iTestX, const int iTestZ
) {
	if ( !rBatch.multimesh.is_valid() || iIndex < 0 || iIndex >= rBatch.nCount ||
		rBatch.transformBuffer.size() != ( int64_t ) ( rBatch.nCount ) * TERRAIN3D_MULTIMESH_STRIDE_FLOATS ) {
		LOG( T3DTerrain::ERROR, "Clipmap batch instance index out of bounds: ", iIndex, " / ", ( int ) ( rBatch.nCount ) );
		return;
	}
	float *pRecord = rBatch.transformBuffer.ptrw() + ( size_t ) ( iIndex ) * TERRAIN3D_MULTIMESH_STRIDE_FLOATS;
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
	if ( m_castShadows == RenderingServer::SHADOW_CASTING_SETTING_OFF || nLod < 0 ||
		nLod >= m_nShadowLodCount ) {
		return false;
	}
	return m_bTierShadowEnabled[( size_t ) ( getLodTier( nLod ) )];
}

void T3DMesher::rebuildRenderBatches( void ) {
	// The optimized shadow batch may borrow the Near state mesh. Free its
	// instance before freeing the visible batch that owns that mesh RID.
	freeBatch( m_shadowStateBatch );
	for ( t3dclipmapBatch_t &batch : m_szRenderBatches ) {
		freeBatch( batch );
	}
	m_nRenderBatchCount = 0;
	m_bBatchDirty.fill( false );
	m_bShadowStateBatchDirty = false;
	for ( t3dlodBatchSlot_t &slot : m_szLodBatchSlots ) {
		slot = t3dlodBatchSlot_t{};
	}

	// When normal single-sided shadow LODs fit entirely inside the Near tier,
	// keep visible batching purely shader-based and submit one exact shadows-only
	// state mesh for that subset. This removes the authoring shadow-state split
	// while preserving the requested shadow LOD count and cascade coverage.
	m_bSeparateShadowStateBatch = m_bStateMeshBatching && m_bTierShadowEnabled[0] && m_nShadowLodCount > 0 &&
		m_nShadowLodCount <= m_nNearShaderLodCount &&
		( m_castShadows == RenderingServer::SHADOW_CASTING_SETTING_ON ||
			m_castShadows == RenderingServer::SHADOW_CASTING_SETTING_SHADOWS_ONLY );

	int lod = 0;
	while ( lod < m_nClipmapLevelCount ) {
		const bool bLod0 = lod == 0;
		const int tier = getLodTier( lod );
		const bool casts_shadows = !m_bSeparateShadowStateBatch && lodCastsShadows( lod );
		int end_lod = lod;
		while ( end_lod + 1 < m_nClipmapLevelCount && getLodTier( end_lod + 1 ) == tier &&
			( m_bSeparateShadowStateBatch || ( lodCastsShadows( end_lod + 1 ) == casts_shadows ) ) &&
			( m_bStateMeshBatching || !bLod0 ) ) {
			++end_lod;
		}
		if ( m_nRenderBatchCount >= TERRAIN3D_MAX_RENDER_BATCHES ) {
			LOG( T3DTerrain::ERROR, "Clipmap batching exceeded fixed render batch capacity: ", TERRAIN3D_MAX_RENDER_BATCHES );
			return;
		}
		const int count = end_lod - lod + 1;
		t3dclipmapBatch_t batch;
		if ( m_bStateMeshBatching ) {
			batch = createStateBatch( lod, end_lod, tier, casts_shadows );
		} else {
			const RID mesh = m_szMeshRids[bLod0 ? LOD0_MESH : OUTER_RING_MESH];
			batch = createBatch( mesh, count, lod, end_lod, tier, casts_shadows );
		}
		if ( !batch.instance.is_valid() ) {
			return;
		}
		const int batch_index = m_nRenderBatchCount;
		m_szRenderBatches[( size_t ) ( batch_index )] = batch;
		for ( int level = lod; level <= end_lod; ++level ) {
			m_szLodBatchSlots[( size_t ) ( level )].batch = ( int8_t ) ( batch_index );
			m_szLodBatchSlots[( size_t ) ( level )].instance = m_bStateMeshBatching ? ( int8_t ) ( -1 ) : ( int8_t ) ( level - lod );
		}
		++m_nRenderBatchCount;
		lod = end_lod + 1;
	}

	if ( m_bSeparateShadowStateBatch ) {
		const int last_shadow_lod = MIN( m_nShadowLodCount, m_nClipmapLevelCount ) - 1;
		if ( last_shadow_lod >= 0 ) {
			const t3dclipmapBatch_t &near_batch = m_szRenderBatches[0];
			if ( near_batch.firstLod == 0 && near_batch.lastLod == last_shadow_lod && near_batch.mesh.is_valid() ) {
				m_shadowStateBatch.mesh = near_batch.mesh;
				m_shadowStateBatch.bOwnsMesh = false;
				m_shadowStateBatch.instance = RS->instance_create2( m_shadowStateBatch.mesh, m_scenario );
				m_shadowStateBatch.nCount = ( uint8_t ) ( last_shadow_lod + 1 );
				m_shadowStateBatch.firstLod = 0;
				m_shadowStateBatch.lastLod = ( uint8_t ) ( last_shadow_lod );
				m_shadowStateBatch.tier = 0;
				m_shadowStateBatch.bCastsShadows = true;
			} else {
				m_shadowStateBatch = createStateBatch( 0, last_shadow_lod, 0, true );
			}
			if ( !m_shadowStateBatch.instance.is_valid() ) {
				LOG( T3DTerrain::ERROR, "Failed to create optimized clipmap shadow state batch" );
				m_bSeparateShadowStateBatch = false;
			} else {
				m_bShadowStateBatchDirty = true;
			}
		}
	}

	LOG( T3DTerrain::INFO, "Clipmap render batching: ", m_nClipmapLevelCount, " LODs -> ", m_nRenderBatchCount,
		" visible state batches + ", m_bSeparateShadowStateBatch ? 1 : 0, " shadow state batch using ",
		getBaseMeshCount(), " mesh resources; mode=", m_bStateMeshBatching ? "combined-state" : "multimesh-fallback" );
}

void T3DMesher::updateBatchCustomAabb( const int iBatchIndex ) {
	if ( iBatchIndex < 0 || iBatchIndex >= m_nRenderBatchCount ) {
		return;
	}
	t3dclipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( iBatchIndex )];
	bool bHasAabb = false;
	AABB combined;
	const Vector2 anchor = m_bStateMeshBatching ? batch.anchorPosition : Vector2();
	for ( int lod = batch.firstLod; lod <= batch.lastLod; ++lod ) {
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
	if ( m_bStateMeshBatching && batch.mesh.is_valid() ) {
		RS->mesh_set_custom_aabb( batch.mesh, combined );
	} else if ( batch.multimesh.is_valid() ) {
		RS->multimesh_set_custom_aabb( batch.multimesh, combined );
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
}

void T3DMesher::refreshStateBatch( const int iBatchIndex, eastl::array<bool, 3> &bDirtyTiers ) {
	if ( iBatchIndex < 0 || iBatchIndex >= m_nRenderBatchCount ) {
		return;
	}
	t3dclipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( iBatchIndex )];
	const Vector2 anchor = m_szLastLodPositions[( size_t ) ( batch.firstLod )];
	if ( anchor == V2_MAX ) {
		return;
	}
	if ( batch.anchorPosition != anchor ) {
		Transform3D transform;
		transform.origin = Vector3( anchor.x, 0.0f, anchor.y );
		RS->instance_set_transform( batch.instance, transform );
		batch.anchorPosition = anchor;
	}
	for ( int lod = batch.firstLod; lod <= batch.lastLod; ++lod ) {
		const Vector2 position = m_szLastLodPositions[( size_t ) ( lod )];
		if ( position == V2_MAX ) {
			continue;
		}
		const int stitch_code = CLAMP( m_szLastLodTestX[( size_t ) ( lod )], 0, 2 ) +
			CLAMP( m_szLastLodTestZ[( size_t ) ( lod )], 0, 2 ) * 3;
		m_lodStateBuffer.set( lod, Vector4( position.x - anchor.x, position.y - anchor.y,
			m_fLodScales[( size_t ) ( lod )], ( float ) ( stitch_code ) ) );
	}
	bDirtyTiers[( size_t ) ( batch.tier )] = true;
	updateBatchCustomAabb( iBatchIndex );
	RS->instance_teleport( batch.instance );
	++m_nTeleportsLastSnap;
}

void T3DMesher::refreshShadowStateBatch( void ) {
	if ( !m_bSeparateShadowStateBatch || !m_shadowStateBatch.instance.is_valid() ) {
		return;
	}
	const Vector2 anchor = m_szLastLodPositions[0];
	if ( anchor == V2_MAX ) {
		return;
	}
	if ( m_shadowStateBatch.anchorPosition != anchor ) {
		Transform3D transform;
		transform.origin = Vector3( anchor.x, 0.0f, anchor.y );
		RS->instance_set_transform( m_shadowStateBatch.instance, transform );
		m_shadowStateBatch.anchorPosition = anchor;
	}
	bool bHasAabb = false;
	AABB combined;
	for ( int lod = 0; lod <= m_shadowStateBatch.lastLod; ++lod ) {
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
	if ( bHasAabb && m_shadowStateBatch.mesh.is_valid() ) {
		RS->mesh_set_custom_aabb( m_shadowStateBatch.mesh, combined );
	}
	RS->instance_teleport( m_shadowStateBatch.instance );
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
		if ( m_bShadowStateBatchDirty ) {
			refreshShadowStateBatch();
			m_bShadowStateBatchDirty = false;
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
		}
		return;
	}
	for ( int batch_index = 0; batch_index < m_nRenderBatchCount; ++batch_index ) {
		if ( !m_bBatchDirty[( size_t ) ( batch_index )] ) {
			continue;
		}
		t3dclipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( batch_index )];
		if ( batch.multimesh.is_valid() && !batch.transformBuffer.is_empty() ) {
			RS->multimesh_set_buffer( batch.multimesh, batch.transformBuffer );
			++m_nBufferUploadsLastSnap;
		}
		updateBatchCustomAabb( batch_index );
		if ( batch.instance.is_valid() ) {
			RS->instance_teleport( batch.instance );
			++m_nTeleportsLastSnap;
		}
		m_bBatchDirty[( size_t ) ( batch_index )] = false;
	}
}

void T3DMesher::generateClipmap( void ) {
	clearClipmap();
	generateOffsetData();
	generateMeshTypes();
	m_nClipmapLevelCount = CLAMP( m_nLods + m_tessellationLevel, 0, TERRAIN3D_MAX_CLIPMAP_LEVELS );
	rebuildRenderBatches();
}

void T3DMesher::generateOffsetData( void ) {
	LOG( T3DTerrain::INFO, "Computing fixed clipmap positioning offsets" );
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
	m_nClipmapLevelCount = CLAMP( m_nLods + m_tessellationLevel, 0, TERRAIN3D_MAX_CLIPMAP_LEVELS );
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
	LOG( T3DTerrain::INFO, "Freeing clipmap render batches" );
	// Free the shadows-only instance first because it may borrow a visible
	// Near mesh rather than owning a duplicate geometry resource.
	freeBatch( m_shadowStateBatch );
	for ( t3dclipmapBatch_t &batch : m_szRenderBatches ) {
		freeBatch( batch );
	}
	m_bShadowStateBatchDirty = false;
	m_bSeparateShadowStateBatch = false;
	m_nRenderBatchCount = 0;
	m_bBatchDirty.fill( false );
	for ( t3dlodBatchSlot_t &slot : m_szLodBatchSlots ) {
		slot = t3dlodBatchSlot_t{};
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
	LOG( T3DTerrain::INFO, "Freeing shared clipmap meshes" );
	for ( RID &rid : m_szMeshRids ) {
		if ( rid.is_valid() ) {
			RS->free_rid( rid );
		}
		rid = RID();
	}
	for ( t3dclipmapGeometry_t &geometry : m_szMeshGeometry ) {
		geometry = t3dclipmapGeometry_t{};
	}
}

///////////////////////////
// Public Functions
///////////////////////////

void T3DMesher::initialize(
	T3DTerrain *const pTerrain, const int nMeshSize, const int nLods,
	const int nTessellationLevel, const real_t fVertexSpacing, const eastl::array<RID, 3> &rMaterials,
	const int nNearShaderLodCount, const int nMiddleShaderLodCount,
	const int nClipmapUpdateBudget, const uint32_t nRenderLayers,
	const RenderingServer::ShadowCastingSetting castShadows,
	const GeometryInstance3D::GIMode giMode, const int nShadowLodCount,
	const eastl::array<bool, 3> &bTierShadowEnabled, const bool bStateMeshBatching
) {
	if ( !pTerrain ) {
		return;
	}
	m_pTerrain = pTerrain;
	if ( !m_pTerrain->isInsideWorld() ) {
		LOG( T3DTerrain::DEBUG, "Terrain3D's world3D is null" );
		return;
	}
	LOG( T3DTerrain::INFO, "Initializing GeoMesh" );
	m_scenario = m_pTerrain->get_world_3d()->get_scenario();
	m_szMaterials = rMaterials;
	m_bStateMeshBatching = bStateMeshBatching;
	m_lodStateBuffer.resize( TERRAIN3D_MAX_CLIPMAP_LEVELS );
	m_lodStateBuffer.fill( Vector4( 0.0f, 0.0f, 1.0f, 4.0f ) );
	configureStateMaterials();
	m_nNearShaderLodCount = CLAMP( nNearShaderLodCount, 0, nLods + nTessellationLevel );
	m_nMiddleShaderLodCount = CLAMP( nMiddleShaderLodCount, 0,
		nLods + nTessellationLevel - m_nNearShaderLodCount );
	m_nClipmapUpdateBudget = CLAMP( nClipmapUpdateBudget, 1, TERRAIN3D_MAX_CLIPMAP_LEVELS );
	m_bForceFullSnap = true;
	m_nLods = nLods;
	m_tessellationLevel = nTessellationLevel;
	m_nMeshSize = nMeshSize;
	m_fVertexSpacing = fVertexSpacing;
	m_renderLayers = nRenderLayers;
	m_castShadows = castShadows;
	m_giMode = giMode;
	m_nShadowLodCount = CLAMP( nShadowLodCount, 0, TERRAIN3D_MAX_CLIPMAP_LEVELS );
	m_bTierShadowEnabled = bTierShadowEnabled;
	generateClipmap();
	generateLodCache();
	m_iDeferredLodCursor = getImmediateLodCount();
	update();
	updateAabbs();
	resetTargetPosition();
	snap();
}

void T3DMesher::destroy( void ) {
	LOG( T3DTerrain::INFO, "Destroying clipmap" );
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
	const int update_budget = CLAMP( nClipmapUpdateBudget, 1, TERRAIN3D_MAX_CLIPMAP_LEVELS );
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
	const bool bOldEnabled = m_castShadows != RenderingServer::SHADOW_CASTING_SETTING_OFF;
	const bool bNewEnabled = castShadows != RenderingServer::SHADOW_CASTING_SETTING_OFF;
	const bool bOldSeparateEligible = m_bStateMeshBatching && m_nShadowLodCount > 0 &&
		m_nShadowLodCount <= m_nNearShaderLodCount &&
		( m_castShadows == RenderingServer::SHADOW_CASTING_SETTING_ON ||
			m_castShadows == RenderingServer::SHADOW_CASTING_SETTING_SHADOWS_ONLY );
	const bool bNewSeparateEligible = m_bStateMeshBatching && m_nShadowLodCount > 0 &&
		m_nShadowLodCount <= m_nNearShaderLodCount &&
		( castShadows == RenderingServer::SHADOW_CASTING_SETTING_ON ||
			castShadows == RenderingServer::SHADOW_CASTING_SETTING_SHADOWS_ONLY );
	m_castShadows = castShadows;
	if ( bOldEnabled != bNewEnabled || bOldSeparateEligible != bNewSeparateEligible ) {
		rebuildRenderBatches();
		resetTargetPosition();
		update();
		snap();
		return;
	}
	update();
}

void T3DMesher::setShadowLodCount( const int nCount ) {
	const int count = CLAMP( nCount, 0, TERRAIN3D_MAX_CLIPMAP_LEVELS );
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

	const t3dlodBatchSlot_t slot = m_szLodBatchSlots[( size_t ) ( nLod )];
	if ( slot.batch < 0 || slot.batch >= m_nRenderBatchCount ) {
		LOG( T3DTerrain::ERROR, "Missing clipmap render batch for LOD ", nLod );
		return false;
	}
	t3dclipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( slot.batch )];
	if ( !m_bStateMeshBatching ) {
		Transform3D transform;
		transform = transform.scaled( Vector3( m_fLodScales[( size_t ) ( nLod )], 1.0f, m_fLodScales[( size_t ) ( nLod )] ) );
		transform.origin = pos;
		setBatchInstance( batch, slot.instance, transform, test_x, test_z );
	}
	m_bBatchDirty[( size_t ) ( slot.batch )] = true;
	if ( m_bSeparateShadowStateBatch && nLod < m_nShadowLodCount ) {
		m_bShadowStateBatchDirty = true;
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
	RS->instance_set_scenario( rRid, m_scenario );
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
		const t3dclipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( batch_index )];
		const RID material = m_szMaterials[( size_t ) ( batch.tier )].is_valid() ? m_szMaterials[( size_t ) ( batch.tier )] : m_szMaterials[0];
		const RenderingServer::ShadowCastingSetting shadows = !m_bSeparateShadowStateBatch && batch.bCastsShadows ?
			m_castShadows : RenderingServer::SHADOW_CASTING_SETTING_OFF;
		updateInstanceRid( batch.instance, material, shadows, bVisibleSurface, bBakedLight, bDynamicGi );
	}
	if ( m_bSeparateShadowStateBatch && m_shadowStateBatch.instance.is_valid() ) {
		updateInstanceRid( m_shadowStateBatch.instance, m_szMaterials[0],
			RenderingServer::SHADOW_CASTING_SETTING_SHADOWS_ONLY, bTerrainVisible, false, false );
	}
}

void T3DMesher::updateAabbs( const real_t fCullMargin, const Vector2 &rHeightRange ) {
	IS_DATA_INIT( VOID );
	const real_t cull_margin = fCullMargin < 0.0f ? m_pTerrain->getCullMargin() : fCullMargin;
	const Vector2 height_range = rHeightRange.x == FLT_MAX ? m_pTerrain->getData()->getHeightRange() : rHeightRange;
	const t3dclipmapVerticalBounds_t vertical_bounds = terrain3dClipmapVerticalBounds(
		( double ) ( height_range.x ), ( double ) ( height_range.y ), ( double ) ( cull_margin ) );
	m_fClipmapAabbMinY = ( real_t ) ( vertical_bounds.fMinY );
	m_fClipmapAabbSizeY = ( real_t ) ( vertical_bounds.fSizeY );
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
	if ( m_bSeparateShadowStateBatch ) {
		refreshShadowStateBatch();
	}
}
