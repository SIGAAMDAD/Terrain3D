// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "assets/terrain_3d_mesh_asset.h"

#include "rendering/rd/vt/terrain_3d_vt_material_identity.h"

#include "core/logger.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_paths.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/occluder3d.hpp>
#include <godot_cpp/classes/occluder_instance3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/surface_tool.hpp>

// -----------------------------------------------------------------------------
// Private functions

void Terrain3DMeshAsset::clearLodRanges( void ) {
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Clearing LOD Ranges, setting last_lod to 128" );
	for ( int i = 0; i < s_nMeshMaxLodCount; i++ ) {
		m_fLodRanges[i] = ( i + 1 ) * s_nInstanceDefaultCellSize;
	}
	m_fLodRanges[m_iLastLod] = MAX( m_fLodRanges[m_iLastLod], 128.f );
}

bool Terrain3DMeshAsset::sortLodNodes( const Node *pA, const Node *pB ) {
	ASSERT( pA && pB, false );
	return pA->get_name().right( 1 ) < pB->get_name().right( 1 );
}

static bool t3dMeshAssetRelativeTransformToRoot( const Node *const pRoot, const Node3D *const pNode, Transform3D &rTransform ) {
	if ( !pRoot || !pNode ) {
		return false;
	}
	rTransform = Transform3D();
	const Node *pCursor = pNode;
	while ( pCursor && pCursor != pRoot ) {
		const Node3D *const pNode3D = Object::cast_to<Node3D>( pCursor );
		if ( pNode3D ) {
			rTransform = pNode3D->get_transform() * rTransform;
		}
		pCursor = pCursor->get_parent();
	}
	return pCursor == pRoot;
}

static bool t3dMeshAssetAppendCompoundOccluder( const Node *const pRoot,
        const OccluderInstance3D *const pOccluderInstance,
        PackedVector3Array &rVertices,
        PackedInt32Array &rIndices ) {
	if ( !pRoot || !pOccluderInstance || pOccluderInstance->get_occluder().is_null() ) {
		return false;
	}
	const PackedVector3Array source_vertices = pOccluderInstance->get_occluder()->get_vertices();
	const PackedInt32Array source_indices = pOccluderInstance->get_occluder()->get_indices();
	if ( source_vertices.size() < 3 || source_indices.size() < 3 || ( source_indices.size() % 3 ) != 0 ) {
		return false;
	}
	for ( int iIndex = 0; iIndex < source_indices.size(); ++iIndex ) {
		if ( source_indices[iIndex] < 0 || source_indices[iIndex] >= source_vertices.size() ) {
			return false;
		}
	}
	Transform3D relative;
	if ( !t3dMeshAssetRelativeTransformToRoot( pRoot, pOccluderInstance, relative ) ) {
		return false;
	}

	const int vertex_base = rVertices.size();
	const int index_base = rIndices.size();
	rVertices.resize( vertex_base + source_vertices.size() );
	rIndices.resize( index_base + source_indices.size() );
	for ( int iVertex = 0; iVertex < source_vertices.size(); ++iVertex ) {
		rVertices.set( vertex_base + iVertex, relative.xform( source_vertices[iVertex] ) );
	}
	const bool mirrored = relative.basis.determinant() < 0.0f;
	for ( int iIndex = 0; iIndex < source_indices.size(); iIndex += 3 ) {
		const int32_t i0 = source_indices[iIndex];
		const int32_t i1 = source_indices[iIndex + 1];
		const int32_t i2 = source_indices[iIndex + 2];
		rIndices.set( index_base + iIndex, vertex_base + i0 );
		rIndices.set( index_base + iIndex + 1, vertex_base + ( mirrored ? i2 : i1 ) );
		rIndices.set( index_base + iIndex + 2, vertex_base + ( mirrored ? i1 : i2 ) );
	}
	return true;
}

static int t3dMeshAssetBuildCompoundOccluderProxy( const Node *const pRoot, PackedVector3Array &rVertices, PackedInt32Array &rIndices ) {
	rVertices.clear();
	rIndices.clear();
	if ( !pRoot ) {
		return 0;
	}
	int appended_occluders = 0;
	const OccluderInstance3D *const pRootOccluder = Object::cast_to<OccluderInstance3D>( pRoot );
	if ( pRootOccluder && t3dMeshAssetAppendCompoundOccluder( pRoot, pRootOccluder, rVertices, rIndices ) ) {
		++appended_occluders;
	}
	const TypedArray<Node> occluder_instances = pRoot->find_children( "*", "OccluderInstance3D" );
	for ( int iOccluder = 0; iOccluder < occluder_instances.size(); ++iOccluder ) {
		const OccluderInstance3D *const pOccluder = Object::cast_to<OccluderInstance3D>( occluder_instances[iOccluder] );
		if ( pOccluder && t3dMeshAssetAppendCompoundOccluder( pRoot, pOccluder, rVertices, rIndices ) ) {
			++appended_occluders;
		}
	}
	return appended_occluders;
}

static Ref<ArrayMesh> t3dMeshAssetBuildCompoundMesh( const Node *const pRoot, const TypedArray<Node> &rMeshInstances ) {
	Ref<ArrayMesh> pCompound;
	pCompound.instantiate();
	uint32_t appended_surfaces = 0u;
	for ( int iMesh = 0; iMesh < rMeshInstances.size(); ++iMesh ) {
		MeshInstance3D *const pMeshInstance = Object::cast_to<MeshInstance3D>( rMeshInstances[iMesh] );
		if ( !pMeshInstance ) {
			continue;
		}
		const Ref<Mesh> pMesh = pMeshInstance->get_mesh();
		if ( pMesh.is_null() ) {
			continue;
		}
		Transform3D relative;
		if ( !t3dMeshAssetRelativeTransformToRoot( pRoot, pMeshInstance, relative ) ) {
			return Ref<ArrayMesh>();
		}
		ArrayMesh *const pArrayMesh = Object::cast_to<ArrayMesh>( pMesh.ptr() );
		if ( !pArrayMesh ) {
			return Ref<ArrayMesh>();
		}
		for ( int iSurface = 0; iSurface < pArrayMesh->get_surface_count(); ++iSurface ) {
			if ( pArrayMesh->surface_get_primitive_type( iSurface ) != Mesh::PRIMITIVE_TRIANGLES ) {
				continue;
			}
			Ref<SurfaceTool> pTool;
			pTool.instantiate();
			pTool->begin( Mesh::PRIMITIVE_TRIANGLES );
			pTool->append_from( pMesh, iSurface, relative );
			pTool->index();
			pTool->set_material( pMeshInstance->get_active_material( iSurface ) );
			pCompound = pTool->commit( pCompound );
			if ( pCompound.is_null() ) {
				return Ref<ArrayMesh>();
			}
			++appended_surfaces;
		}
	}
	return appended_surfaces > 0u ? pCompound : Ref<ArrayMesh>();
}

Ref<ArrayMesh> Terrain3DMeshAsset::createGeneratedMesh( const t3dGenType_t type ) const {
	if ( type != TYPE_TEXTURE_CARD ) {
		LOG( ERROR, "Only TYPE_TEXTURE_CARD is currently implemented" );
		return Ref<ArrayMesh>();
	}
	LOG( EXTREME, "Regenerating new mesh" );
	Ref<ArrayMesh> pArrayMesh;
	pArrayMesh.instantiate();
	const int vertex_count = m_nGeneratedFaces * 4;
	const int index_count = m_nGeneratedFaces * 6;
	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedVector2Array uvs;
	PackedInt32Array indices;
	vertices.resize( vertex_count );
	normals.resize( vertex_count );
	tangents.resize( vertex_count * 4 );
	uvs.resize( vertex_count );
	indices.resize( index_count );

	Vector3 *pVertexWrite = vertices.ptrw();
	Vector3 *pNormalWrite = normals.ptrw();
	float *pTangentWrite = tangents.ptrw();
	Vector2 *pUvWrite = uvs.ptrw();
	int32_t *pIndexWrite = indices.ptrw();
	const Size2 start_pos = Vector2( m_generatedSize.x * -0.5f, -0.5f );
	const Vector3 normal( 0.f, 0.f, 1.f );

	for ( int face = 0; face < m_nGeneratedFaces; ++face ) {
		const real_t fAngle = face == 0 ? 0.f : ( real_t )( face )*Math::PI / ( real_t )( m_nGeneratedFaces );
		const int vertex_base = face * 4;
		for ( int j = 0; j < 2; ++j ) {
			for ( int i = 0; i < 2; ++i ) {
				const int vertex_index = vertex_base + j * 2 + i;
				const float fX = start_pos.x + ( float )( i )*m_generatedSize.x;
				const float fZ = start_pos.y + ( float )( j )*m_generatedSize.y;
				pVertexWrite[vertex_index] = Vector3( -fX, fZ, 0.f ).rotated( s_v3Up, fAngle );
				pNormalWrite[vertex_index] = normal;
				const int tangent_index = vertex_index * 4;
				pTangentWrite[tangent_index] = 1.f;
				pTangentWrite[tangent_index + 1] = 0.f;
				pTangentWrite[tangent_index + 2] = 0.f;
				pTangentWrite[tangent_index + 3] = 1.f;
				pUvWrite[vertex_index] = Vector2( 1.f - ( float )( i ), 1.f - ( float )( j ) );
			}
		}

		const int index_base = face * 6;
		pIndexWrite[index_base] = vertex_base;
		pIndexWrite[index_base + 1] = vertex_base + 1;
		pIndexWrite[index_base + 2] = vertex_base + 2;
		pIndexWrite[index_base + 3] = vertex_base + 1;
		pIndexWrite[index_base + 4] = vertex_base + 3;
		pIndexWrite[index_base + 5] = vertex_base + 2;
	}
	Array arrays;
	arrays.resize( Mesh::ARRAY_MAX );
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TANGENT] = tangents;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;
	pArrayMesh->add_surface_from_arrays( Mesh::PRIMITIVE_TRIANGLES, arrays );
	return pArrayMesh;
}

void Terrain3DMeshAsset::assignGeneratedMesh( void ) {
	LOG( DEBUG, "Assiging generated mesh & lod settings" );
	m_packedScene.unref();
	m_pendingMeshes.clear();
	m_pendingMeshes.push_back( createGeneratedMesh() );
	m_iLastLod = 0;
	m_iLastShadowLod = 0;
	m_nShadowImpostor = 0;
	if ( m_renderingMaterialOverride.is_null() ) {
		Ref<Terrain3DRenderingMaterial> pMat;
		pMat.instantiate();
		pMat->setAlphaScissor( true );
		pMat->setAlphaScissorThreshold( 0.5f );
		pMat->setTwoSided( true );
		pMat->setVertexColor( true );
		pMat->setFoliage( true );
		pMat->set_meta( "terrain3d_generated_material", true );
		setRenderingMaterialOverride( pMat );
	}
	// If no existing meshes, commit immediately. Otherwise, will trigger on an instancer update
	if ( m_meshes.is_empty() ) {
		commitMeshes();
	}
}

Ref<Material> Terrain3DMeshAsset::getMaterial( void ) {
	if ( m_materialOverride.is_null() ) {
		Ref<StandardMaterial3D> pMat;
		pMat.instantiate();
		pMat->set_transparency( BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR );
		pMat->set_alpha_scissor_threshold( 0.5f );
		pMat->set_shading_mode( BaseMaterial3D::SHADING_MODE_PER_VERTEX );
		pMat->set_cull_mode( BaseMaterial3D::CULL_DISABLED );
		pMat->set_feature( BaseMaterial3D::FEATURE_BACKLIGHT, true );
		pMat->set_backlight( Color( .5f, .5f, .5f ) );
		pMat->set_flag( BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true );
		pMat->set_distance_fade( BaseMaterial3D::DISTANCE_FADE_PIXEL_DITHER );
		pMat->set_distance_fade_min_distance( 128.f );
		pMat->set_distance_fade_max_distance( 96.f );
		pMat->set_meta( "terrain3d_generated_material", true );
		return pMat;
	} else {
		return m_materialOverride;
	}
}


static bool t3dMeshAssetBuildIndexedTriangles( const Ref<Mesh> &rSource, PackedVector3Array &rVertices, PackedInt32Array &rIndices ) {
	rVertices.clear();
	rIndices.clear();
	if ( rSource.is_null() ) {
		return false;
	}
	const PackedVector3Array faces = rSource->get_faces();
	if ( faces.size() < 3 || ( faces.size() % 3 ) != 0 ) {
		return false;
	}
	Ref<SurfaceTool> surface_tool;
	surface_tool.instantiate();
	surface_tool->begin( Mesh::PRIMITIVE_TRIANGLES );
	for ( int i = 0; i < faces.size(); ++i ) {
		surface_tool->add_vertex( faces[i] );
	}
	surface_tool->index();
	surface_tool->optimize_indices_for_cache();
	const Array generated_arrays = surface_tool->commit_to_arrays();
	if ( generated_arrays.size() <= Mesh::ARRAY_INDEX ) {
		return false;
	}
	rVertices = generated_arrays[Mesh::ARRAY_VERTEX];
	rIndices = generated_arrays[Mesh::ARRAY_INDEX];
	return !rVertices.is_empty() && rIndices.size() >= 3 && ( rIndices.size() % 3 ) == 0;
}

static Ref<ArrayMesh> t3dMeshAssetBuildProxyMesh( const PackedVector3Array &rVertices, const PackedInt32Array &rIndices ) {
	if ( rVertices.is_empty() || rIndices.size() < 3 || ( rIndices.size() % 3 ) != 0 ) {
		return Ref<ArrayMesh>();
	}
	Array proxy_arrays;
	proxy_arrays.resize( Mesh::ARRAY_MAX );
	proxy_arrays[Mesh::ARRAY_VERTEX] = rVertices;
	proxy_arrays[Mesh::ARRAY_INDEX] = rIndices;
	Ref<ArrayMesh> proxy_mesh;
	proxy_mesh.instantiate();
	proxy_mesh->add_surface_from_arrays( Mesh::PRIMITIVE_TRIANGLES, proxy_arrays );
	return proxy_mesh;
}

void Terrain3DMeshAsset::clearInstanceCollisionGeometry( void ) {
	m_instanceCollisionShape.unref();
	m_nInstanceCollisionVertexCount = 0u;
	m_nInstanceCollisionTriangleCount = 0u;
	m_bInstanceCollisionDerived = false;
}

void Terrain3DMeshAsset::clearInstanceOcclusionGeometry( void ) {
	m_instanceOcclusionProxyMesh.unref();
	m_instanceOccluder.unref();
	m_nInstanceOcclusionProxyVertexCount = 0u;
	m_nInstanceOcclusionProxyTriangleCount = 0u;
	m_bInstanceOcclusionProxyDerived = false;
	m_bInstanceProxyImportedOccluder = false;
}

void Terrain3DMeshAsset::clearInstanceDerivedGeometry( void ) {
	clearInstanceCollisionGeometry();
	clearInstanceOcclusionGeometry();
}

bool Terrain3DMeshAsset::rebuildInstanceCollisionGeometry( void ) {
	clearInstanceCollisionGeometry();
	if ( !requiresInstancePhysics() || m_meshes.is_empty() ) {
		return false;
	}
	const Ref<Mesh> source = m_meshes[0];
	PackedVector3Array vertices;
	PackedInt32Array indices;
	if ( !t3dMeshAssetBuildIndexedTriangles( source, vertices, indices ) ) {
		LOG( WARN, "ID ", m_nId, ", ", m_name, ": imported LOD0 has no triangle faces for instance physics" );
		return false;
	}
	const Ref<ArrayMesh> collision_mesh = t3dMeshAssetBuildProxyMesh( vertices, indices );
	if ( collision_mesh.is_null() ) {
		return false;
	}
	const Ref<ConcavePolygonShape3D> collision_shape = collision_mesh->create_trimesh_shape();
	if ( collision_shape.is_null() ) {
		LOG( WARN, "ID ", m_nId, ", ", m_name, ": failed generating LOD0-derived instance collision shape" );
		return false;
	}
	m_instanceCollisionShape = collision_shape;
	m_nInstanceCollisionVertexCount = ( uint32_t )( vertices.size() );
	m_nInstanceCollisionTriangleCount = ( uint32_t )( indices.size() / 3 );
	m_bInstanceCollisionDerived = true;
	if ( ++m_nInstanceCollisionGeneration == 0u ) {
		m_nInstanceCollisionGeneration = 1u;
	}
	return true;
}

bool Terrain3DMeshAsset::rebuildInstanceOcclusionGeometry( void ) {
	PackedVector3Array retained_imported_vertices;
	PackedInt32Array retained_imported_indices;
	if ( m_bInstanceProxyImportedOccluder && m_instanceOccluder.is_valid() ) {
		retained_imported_vertices = m_instanceOccluder->get_vertices();
		retained_imported_indices = m_instanceOccluder->get_indices();
	}
	clearInstanceOcclusionGeometry();
	if ( m_instanceCategory != INSTANCE_CATEGORY_OCCLUDER || m_meshes.is_empty() ) {
		return false;
	}

	PackedVector3Array vertices;
	PackedInt32Array indices;
	bool imported_occluder = false;
	if ( m_bPendingImportedOccluder && m_pendingInstanceOccluderVertices.size() >= 3 && m_pendingInstanceOccluderIndices.size() >= 3 ) {
		vertices = m_pendingInstanceOccluderVertices;
		indices = m_pendingInstanceOccluderIndices;
		imported_occluder = true;
	} else if ( retained_imported_vertices.size() >= 3 && retained_imported_indices.size() >= 3 ) {
		vertices = retained_imported_vertices;
		indices = retained_imported_indices;
		imported_occluder = true;
	} else if ( !t3dMeshAssetBuildIndexedTriangles( m_meshes[0], vertices, indices ) ) {
		LOG( WARN, "ID ", m_nId, ", ", m_name, ": imported LOD0 has no triangle faces for instance occlusion fallback" );
		return false;
	}

	const Ref<ArrayMesh> proxy_mesh = t3dMeshAssetBuildProxyMesh( vertices, indices );
	if ( proxy_mesh.is_null() ) {
		return false;
	}
	Ref<ArrayOccluder3D> occluder;
	occluder.instantiate();
	occluder->set_arrays( vertices, indices );

	m_instanceOcclusionProxyMesh = proxy_mesh;
	m_instanceOccluder = occluder;
	m_instanceOcclusionBox = proxy_mesh->get_aabb();
	m_nInstanceOcclusionProxyVertexCount = ( uint32_t )( vertices.size() );
	m_nInstanceOcclusionProxyTriangleCount = ( uint32_t )( indices.size() / 3 );
	m_bInstanceOcclusionProxyDerived = true;
	m_bInstanceProxyImportedOccluder = imported_occluder;
	LOG( DEBUG,
	        "ID ",
	        m_nId,
	        ", ",
	        m_name,
	        ": instance occlusion proxy source=",
	        imported_occluder ? "imported Occluder3D" : "LOD0 triangles",
	        "; vertices=",
	        m_nInstanceOcclusionProxyVertexCount,
	        "; triangles=",
	        m_nInstanceOcclusionProxyTriangleCount );
	if ( ++m_nInstanceOcclusionProxyGeneration == 0u ) {
		m_nInstanceOcclusionProxyGeneration = 1u;
	}
	return true;
}

bool Terrain3DMeshAsset::rebuildInstanceDerivedGeometry( void ) {
	const bool collision_valid = rebuildInstanceCollisionGeometry();
	const bool occlusion_valid = rebuildInstanceOcclusionGeometry();
	return collision_valid || occlusion_valid;
}

// -----------------------------------------------------------------------------
// Public functions

// Called by Terrain3DAssets::_set_asset_list() and _set_asset()
void Terrain3DMeshAsset::initialize( void ) {
	LOG( INFO, m_nId, ": ", m_name, ": initializing asset" );
	if ( m_packedScene.is_null() && m_generatedType == TYPE_NONE ) {
		LOG( DEBUG, "Blank mesh, setting up default texture card" );
		// Newly-created generated cards are overwhelmingly foliage/debris style
		// assets, so start them in the lean category. Existing serialized assets
		// without an instance_category property retain the compatibility default.
		m_instanceCategory = INSTANCE_CATEGORY_CLUTTER;
		m_instanceOcclusionPolicy = INSTANCE_OCCLUSION_NONE;
		m_hlodContribution = HLOD_EXCLUDE;
		setGeneratedType( TYPE_TEXTURE_CARD );
		m_fHeightOffset = 0.5f;
		clearLodRanges();
		m_fFadeMargin = CLAMP( m_fLodRanges[1] - m_fLodRanges[0], 0.f, 64.f );
	} else {
		// Category is the authoritative capability gate. Older Ground Cover
		// resources may have serialized the former EXPLICIT_MESH default; migrate
		// them (and any stale Clutter policy) to NONE on load. Occluders may be
		// explicitly disabled, but every non-NONE occluder policy normalizes to
		// the one supported explicit-mesh representation.
		if ( m_instanceCategory != INSTANCE_CATEGORY_OCCLUDER ) {
			m_instanceOcclusionPolicy = INSTANCE_OCCLUSION_NONE;
		} else if ( m_instanceOcclusionPolicy != INSTANCE_OCCLUSION_NONE ) {
			m_instanceOcclusionPolicy = INSTANCE_OCCLUSION_EXPLICIT_MESH;
		}
		if ( m_instanceCategory != INSTANCE_CATEGORY_CLUTTER && !m_meshes.is_empty() ) {
			rebuildInstanceDerivedGeometry();
		}
	}
}

void Terrain3DMeshAsset::clear( void ) {
	LOG( INFO, "Clearing MeshAsset" );
	m_name = "New Mesh";
	m_nId = 0;
	m_bEnabled = true;
	m_bHighlighted = false;
	m_highlightMat = Ref<Material>();
	m_packedScene.unref();
	m_meshes.clear();
	m_pendingMeshes.clear();
	m_pendingInstanceOccluderVertices.clear();
	m_pendingInstanceOccluderIndices.clear();
	m_bPendingImportedOccluder = false;
	m_generatedType = TYPE_NONE;
	m_instanceCategory = INSTANCE_CATEGORY_GROUND_COVER;
	m_terrainAlignment = TERRAIN_ALIGNMENT_UPRIGHT;
	m_hlodContribution = HLOD_AUTO;
	m_nHlodMinLevel = 1;
	m_instanceOcclusionPolicy = INSTANCE_OCCLUSION_NONE;
	m_instanceOcclusionBox = AABB( Vector3( -0.5f, 0.0f, -0.5f ), Vector3( 1.0f, 1.0f, 1.0f ) );
	clearInstanceDerivedGeometry();
	m_nInstanceCollisionGeneration = 1u;
	m_nInstanceOcclusionProxyGeneration = 1u;
	m_fInstanceOcclusionRange = 256.0f;
	m_nGeneratedFaces = 2;
	m_generatedSize = V2( 1.f );
	m_fHeightOffset = 0.f;
	m_fDensity = 0.f;
	m_castShadows = s_shadowCastingOn;
	m_nVisibilityLayers = 1;
	m_materialOverride.unref();
	m_materialOverlay.unref();
	disconnectRenderingMaterialSignals();
	m_renderingMaterialOverride.unref();
	m_renderingMaterialOverlay.unref();
	m_surfaceRenderingMaterials.clear();
	m_pendingSurfaceRenderingMaterials.clear();
	m_iLastLod = s_nMeshMaxLodCount - 1;
	m_iLastShadowLod = s_nMeshMaxLodCount - 1;
	m_nShadowImpostor = 0;
	clearLodRanges();
	m_fFadeMargin = 0.f;
	m_thumbnail.unref();
}

void Terrain3DMeshAsset::setName( const String &rName ) {
	if ( rName.length() > 96 ) {
		LOG( WARN, "Name too long, truncating to 96 characters" );
	}
	SET_IF_DIFF( m_name, rName.left( 96 ) );
	LOG( INFO, "ID ", m_nId, ": Setting name: ", m_name );
	LOG( DEBUG, "Emitting setting_changed, ID: ", m_nId );
	emit_signal( "setting_changed", m_nId );
}

void Terrain3DMeshAsset::setId( const int nNewId ) {
	const int old_id = m_nId;
	SET_IF_DIFF( m_nId, CLAMP( nNewId, 0, s_nMaxMeshAssets - 1 ) );
	LOG( INFO, m_name, ": Setting mesh ID: ", m_nId );
	LOG( DEBUG, "Emitting id_changed, TYPE_MESH, ", old_id, ", ", nNewId );
	emit_signal( "id_changed", TYPE_MESH, old_id, nNewId );
}

void Terrain3DMeshAsset::setHighlighted( const bool bHighlighted ) {
	SET_IF_DIFF( m_bHighlighted, bHighlighted );
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Set mesh ID highlight: ", bHighlighted );
	if ( m_bHighlighted && m_highlightMat.is_null() ) {
		Ref<StandardMaterial3D> pMat;
		pMat.instantiate();
		pMat->set_cull_mode( BaseMaterial3D::CULL_DISABLED );
		Color color;
		const real_t fRandomFloat = ( real_t )( rand() ) / ( real_t )( RAND_MAX );
		color.set_hsv( fRandomFloat, 1.f, 1.f, 1.f );
		pMat->set_albedo( color );
		m_highlightMat = pMat;
	}
	LOG( DEBUG, "Emitting preview_changed, ID: ", m_nId );
	emit_signal( "preview_changed", m_nId );
}

Color Terrain3DMeshAsset::getHighlightColor( void ) const {
	StandardMaterial3D *pMat = cast_to<StandardMaterial3D>( m_highlightMat.ptr() );
	if ( m_bHighlighted && pMat ) {
		return pMat->get_albedo();
	}
	return s_colorWhite;
}

void Terrain3DMeshAsset::setEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bEnabled, bEnabled );
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting enabled: ", m_bEnabled );
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "vt_material_changed" );
	emit_signal( "instancer_setting_changed", m_nId );
}

void Terrain3DMeshAsset::updateInstanceCount( const int64_t nAmount ) {
	if ( nAmount == 0 ) {
		return;
	}
	const int64_t new_count = ( int64_t )( m_nInstanceCount ) + nAmount;
	m_nInstanceCount = ( uint32_t )( CLAMP( new_count, ( int64_t )( 0 ), ( int64_t )( UINT32_MAX ) ) );
	LOG( EXTREME, "Emitting instance_count_changed, ID: ", m_nId, ", count: ", m_nInstanceCount );
	emit_signal( "instance_count_changed" );
}

void Terrain3DMeshAsset::setInstanceCount( const uint32_t nAmount ) {
	SET_IF_DIFF( m_nInstanceCount, nAmount );
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting instance_count: ", m_nInstanceCount );
	LOG( DEBUG, "Emitting instance_count_changed" );
	emit_signal( "instance_count_changed" );
}

void Terrain3DMeshAsset::setSceneFile( const Ref<PackedScene> &rSceneFile ) {
	SET_IF_DIFF( m_packedScene, rSceneFile );
	invalidateThumbnail();
	m_pendingMeshes.clear();
	m_pendingSurfaceRenderingMaterials.clear();
	m_pendingInstanceOccluderVertices.clear();
	m_pendingInstanceOccluderIndices.clear();
	m_bPendingImportedOccluder = false;
	if ( m_packedScene.is_valid() ) {
		Node *pNode = m_packedScene->instantiate();
		if ( !pNode ) {
			LOG( ERROR, "Drag a non-empty glb, fbx, scn, or tscn file into the scene_file slot" );
			m_packedScene.unref();
			return;
		}
		LOG( INFO, "ID ", m_nId, ", ", m_name, ": Instantiating scene root node: ", m_packedScene->get_path() );
		m_fHeightOffset = 0.0f;
		m_generatedType = TYPE_NONE;
		if ( m_materialOverride.is_valid() && m_materialOverride->has_meta( "terrain3d_generated_material" ) ) {
			m_materialOverride.unref();
		}
		if ( m_renderingMaterialOverride.is_valid() && m_renderingMaterialOverride->has_meta( "terrain3d_generated_material" ) ) {
			setRenderingMaterialOverride( Ref<Terrain3DRenderingMaterial>() );
		}

		// Look for MeshInstance3D nodes. Explicit LOD naming keeps the historical
		// LOD-chain contract. A scene with multiple ordinary mesh nodes is a single
		// compound object and must be merged into one LOD0 mesh; treating its
		// components as successive LODs corrupts scene identity for GLBs such as
		// buildings assembled from several meshes.
		LOG( DEBUG, "Loaded scene with parent node: ", pNode );
		TypedArray<Node> mesh_instances = pNode->find_children( "*LOD?", "MeshInstance3D" );
		const bool explicit_lod_chain = mesh_instances.size() > 0;
		if ( explicit_lod_chain ) {
			LOG( INFO, "Found ", mesh_instances.size(), " meshes using LOD# naming convention, using the first ", s_nMeshMaxLodCount );
			mesh_instances.sort_custom( callable_mp_static( &Terrain3DMeshAsset::sortLodNodes ) );
		} else {
			mesh_instances.clear();
			if ( pNode->is_class( "MeshInstance3D" ) ) {
				mesh_instances.push_back( pNode );
			}
			const TypedArray<Node> child_meshes = pNode->find_children( "*", "MeshInstance3D" );
			for ( int iChild = 0; iChild < child_meshes.size(); ++iChild ) {
				mesh_instances.push_back( child_meshes[iChild] );
			}
		}

		if ( mesh_instances.size() == 0 ) {
			LOG( ERROR, "No MeshInstance3D found in scene file" );
		} else {
			const String filename = m_packedScene->get_path().get_file().get_basename();
			if ( m_name == "New Mesh" && !m_packedScene->get_path().contains( "::" ) ) {
				m_name = filename;
				LOG( INFO, "Setting name based on filename: ", m_name );
			}
		}

		if ( !explicit_lod_chain && mesh_instances.size() > 1 ) {
			LOG( INFO, "No LOD# meshes found; merging ", mesh_instances.size(), " MeshInstance3D components into one compound LOD0 mesh" );
			const Ref<ArrayMesh> pCompound = t3dMeshAssetBuildCompoundMesh( pNode, mesh_instances );
			if ( pCompound.is_valid() && pCompound->get_surface_count() > 0 ) {
				m_pendingMeshes.push_back( pCompound );
				PackedVector3Array compound_occluder_vertices;
				PackedInt32Array compound_occluder_indices;
				const int imported_occluder_count =
				        t3dMeshAssetBuildCompoundOccluderProxy( pNode, compound_occluder_vertices, compound_occluder_indices );
				if ( imported_occluder_count > 0 ) {
					m_pendingInstanceOccluderVertices = compound_occluder_vertices;
					m_pendingInstanceOccluderIndices = compound_occluder_indices;
					m_bPendingImportedOccluder = true;
					LOG( INFO,
					        "ID ",
					        m_nId,
					        ", ",
					        m_name,
					        ": merged ",
					        imported_occluder_count,
					        " imported OccluderInstance3D nodes into compound explicit proxy; vertices=",
					        compound_occluder_vertices.size(),
					        "; triangles=",
					        compound_occluder_indices.size() / 3 );
				}
			} else {
				LOG( ERROR, "Unable to merge compound scene meshes into one LOD0 ArrayMesh" );
			}
		} else {
			// Single-mesh scenes and explicit LOD chains preserve the existing import
			// path, including an adjacent imported Occluder3D for LOD0.
			for ( int i = 0, count = MIN( mesh_instances.size(), s_nMeshMaxLodCount ); i < count; i++ ) {
				MeshInstance3D *const pMi = cast_to<MeshInstance3D>( mesh_instances[i] );
				if ( !pMi ) {
					continue;
				}
				LOG( DEBUG, "Found mesh: ", pMi->get_name() );
				Ref<Mesh> pMesh = pMi->get_mesh();
				if ( pMesh.is_null() ) {
					LOG( WARN, "MeshInstance3D ", pMi->get_name(), " has no mesh, skipping" );
					continue;
				}
				if ( i == 0 && pMi->get_parent() ) {
					OccluderInstance3D *pNamedOccluder = NULL;
					OccluderInstance3D *pOnlyOccluder = NULL;
					int valid_occluder_count = 0;
					Node *const pParent = pMi->get_parent();
					for ( int iChild = 0; iChild < pParent->get_child_count(); ++iChild ) {
						OccluderInstance3D *const pCandidate = cast_to<OccluderInstance3D>( pParent->get_child( iChild ) );
						if ( !pCandidate || pCandidate->get_occluder().is_null() ) {
							continue;
						}
						const PackedVector3Array candidate_vertices = pCandidate->get_occluder()->get_vertices();
						const PackedInt32Array candidate_indices = pCandidate->get_occluder()->get_indices();
						if ( candidate_vertices.size() < 3 || candidate_indices.size() < 3 || ( candidate_indices.size() % 3 ) != 0 ) {
							continue;
						}
						pOnlyOccluder = pCandidate;
						++valid_occluder_count;
						const String mesh_name = String( pMi->get_name() ).to_lower();
						const String occluder_name = String( pCandidate->get_name() ).to_lower();
						if ( occluder_name.begins_with( mesh_name ) || occluder_name.contains( mesh_name ) ) {
							pNamedOccluder = pCandidate;
							break;
						}
					}
					OccluderInstance3D *const pImportedOccluder =
					        pNamedOccluder ? pNamedOccluder : ( valid_occluder_count == 1 ? pOnlyOccluder : NULL );
					if ( pImportedOccluder ) {
						PackedVector3Array imported_vertices = pImportedOccluder->get_occluder()->get_vertices();
						PackedInt32Array imported_indices = pImportedOccluder->get_occluder()->get_indices();
						const Transform3D relative = pMi->get_transform().affine_inverse() * pImportedOccluder->get_transform();
						for ( int iVertex = 0; iVertex < imported_vertices.size(); ++iVertex ) {
							imported_vertices.set( iVertex, relative.xform( imported_vertices[iVertex] ) );
						}
						if ( relative.basis.determinant() < 0.0f ) {
							for ( int iIndex = 0; iIndex + 2 < imported_indices.size(); iIndex += 3 ) {
								const int32_t swap_index = imported_indices[iIndex + 1];
								imported_indices.set( iIndex + 1, imported_indices[iIndex + 2] );
								imported_indices.set( iIndex + 2, swap_index );
							}
						}
						m_pendingInstanceOccluderVertices = imported_vertices;
						m_pendingInstanceOccluderIndices = imported_indices;
						m_bPendingImportedOccluder = true;
						LOG( INFO,
						        "ID ",
						        m_nId,
						        ", ",
						        m_name,
						        ": using imported Occluder3D generated beside LOD0; vertices=",
						        imported_vertices.size(),
						        "; triangles=",
						        imported_indices.size() / 3 );
					}
				}
				pMesh = pMesh->duplicate();
				for ( int j = 0; j < pMesh->get_surface_count(); j++ ) {
					const Ref<Material> pMat = pMi->get_active_material( j );
					if ( pMat.is_valid() ) {
						pMesh->surface_set_material( j, pMat );
					}
				}
				m_pendingMeshes.push_back( pMesh );
			}
		}
		pNode->queue_free();
	}
	if ( m_pendingMeshes.size() > 0 && !capturePendingSurfaceRenderingMaterialsCold() ) {
		LOG( ERROR, "Unable to build canonical Terrain3DRenderingMaterial table for imported mesh surfaces" );
		m_pendingMeshes.clear();
		m_pendingSurfaceRenderingMaterials.clear();
	}
	if ( m_pendingMeshes.size() > 0 ) {
		const Ref<Mesh> pMesh = m_pendingMeshes[0];
		if ( pMesh.is_null() ) {
			LOG( ERROR, "First mesh is null after loading scene" );
			return;
		}
		const AABB aabb = pMesh->get_aabb();
		m_fDensity = CLAMP( 10.f / ( aabb.has_volume() ? aabb.get_volume() : 1.f ), 0.01f, 10.0f );
		m_iLastLod = m_pendingMeshes.size() - 1;
		m_iLastShadowLod = m_iLastLod;
		m_nShadowImpostor = 0;
		clearLodRanges();
	} else {
		setGeneratedType( TYPE_TEXTURE_CARD );
		m_fHeightOffset = 0.5f;
		clearLodRanges();
	}
	// If no existing meshes, commit immediately. Otherwise, will trigger on an instancer update
	if ( m_meshes.is_empty() ) {
		commitMeshes();
	}
	notify_property_list_changed(); // Call _validate_property to update inspector
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void Terrain3DMeshAsset::commitMeshes( void ) {
	LOG( INFO, m_name, ": Committing ", m_pendingMeshes.size(), " pending meshes" );
	invalidateThumbnail();
	m_meshes.clear();
	m_meshes = m_pendingMeshes;
	m_pendingMeshes = TypedArray<Mesh>();
	disconnectRenderingMaterialSignals();
	m_surfaceRenderingMaterials = m_pendingSurfaceRenderingMaterials;
	m_pendingSurfaceRenderingMaterials = TypedArray<Terrain3DRenderingMaterial>();
	connectRenderingMaterialSignals();
	if ( m_instanceCategory != INSTANCE_CATEGORY_CLUTTER ) {
		if ( m_instanceOcclusionPolicy != INSTANCE_OCCLUSION_NONE ) {
			m_instanceOcclusionPolicy = INSTANCE_OCCLUSION_EXPLICIT_MESH;
		}
		rebuildInstanceDerivedGeometry();
	} else {
		clearInstanceDerivedGeometry();
	}
	m_pendingInstanceOccluderVertices.clear();
	m_pendingInstanceOccluderIndices.clear();
	m_bPendingImportedOccluder = false;
	emit_signal( "vt_material_changed" );
}

void Terrain3DMeshAsset::setGeneratedType( const t3dGenType_t type ) {
	const bool bEnteringGeneratedMode = m_generatedType == TYPE_NONE;
	SET_IF_DIFF( m_generatedType, CLAMP( type, ( t3dGenType_t )( TYPE_NONE + 1 ), ( t3dGenType_t )( MESH_TYPE_MAX - 1 ) ) );
	if ( bEnteringGeneratedMode ) {
		m_castShadows = s_shadowCastingOff;
		m_iLastShadowLod = 0;
		m_nShadowImpostor = 0;
	}
	invalidateThumbnail();
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting generated type: ", m_generatedType );
	assignGeneratedMesh();
	m_fDensity = 10.f;
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
	notify_property_list_changed();
}

Ref<Mesh> Terrain3DMeshAsset::getMesh( const int nLod ) const {
	if ( nLod >= 0 && nLod < getMeshes().size() ) {
		return getMeshes()[nLod];
	}
	return Ref<Mesh>();
}

void Terrain3DMeshAsset::setHeightOffset( const real_t fOffset ) {
	SET_IF_DIFF( m_fHeightOffset, CLAMP( fOffset, -50.f, 50.f ) );
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting height offset: ", m_fHeightOffset );
	LOG( DEBUG, "Emitting setting_changed, ID: ", m_nId );
	emit_signal( "setting_changed", m_nId );
}

void Terrain3DMeshAsset::setDensity( const real_t fDensity ) {
	SET_IF_DIFF( m_fDensity, CLAMP( fDensity, 0.01f, 10.f ) );
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting mesh density: ", fDensity );
	LOG( DEBUG, "Emitting setting_changed, ID: ", m_nId );
	emit_signal( "setting_changed", m_nId );
}

void Terrain3DMeshAsset::setCastShadows( const ShadowCasting castShadows ) {
	SET_IF_DIFF( m_castShadows, castShadows );
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting shadow casting mode: ", m_castShadows );
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

// Returns the approproate cast_shadows setting for the given LOD ID
ShadowCasting Terrain3DMeshAsset::getLodCastShadows( const int nLodId ) const {
	// If cast shadows is off, disable all shadows
	if ( m_castShadows == s_shadowCastingOff ) {
		return m_castShadows;
	}
	// Return shadows only if set, ensuring shadow impostor and last lod are processed first
	if ( m_castShadows == s_shadowCastingOnly ) {
		return m_castShadows;
	}
	// Set to shadows only if this lod is the shadow impostor, which is only ever set to shadows only
	if ( nLodId == s_nMeshShadowLodId ) {
		return s_shadowCastingOnly;
	}
	// Disable shadows if this lod uses the shadow impostor
	if ( nLodId < m_nShadowImpostor ) {
		return s_shadowCastingOff;
	}
	// Disable shadows if this lod is too far
	if ( nLodId > m_iLastShadowLod ) {
		return s_shadowCastingOff;
	}
	return m_castShadows;
}

inline void Terrain3DMeshAsset::setVisibilityLayers( const uint32_t nLayers ) {
	SET_IF_DIFF( m_nVisibilityLayers, nLayers );
	LOG( INFO, m_name, ": Setting visibility layers: ", nLayers );
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void Terrain3DMeshAsset::setMaterialOverride( const Ref<Material> &rMaterial ) {
	SET_IF_DIFF( m_materialOverride, rMaterial );
	if ( rMaterial.is_valid() && m_renderingMaterialOverride.is_null() ) {
		Ref<Terrain3DRenderingMaterial> converted;
		converted.instantiate();
		if ( converted->importBaseMaterialCold( rMaterial ) ) {
			converted->set_meta( "terrain3d_legacy_material_conversion", true );
			setRenderingMaterialOverride( converted );
		}
	}
	invalidateThumbnail();
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting legacy material override: ", rMaterial );
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void Terrain3DMeshAsset::setMaterialOverlay( const Ref<Material> &rMaterial ) {
	SET_IF_DIFF( m_materialOverlay, rMaterial );
	if ( rMaterial.is_valid() && m_renderingMaterialOverlay.is_null() ) {
		Ref<Terrain3DRenderingMaterial> converted;
		converted.instantiate();
		if ( converted->importBaseMaterialCold( rMaterial ) ) {
			converted->set_meta( "terrain3d_legacy_material_conversion", true );
			setRenderingMaterialOverlay( converted );
		}
	}
	invalidateThumbnail();
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting legacy material overlay: ", rMaterial );
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void Terrain3DMeshAsset::onRenderingMaterialChanged( void ) {
	invalidateThumbnail();
	LOG( DEBUG, "Terrain3D rendering material changed; emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "vt_material_changed" );
	emit_signal( "instancer_setting_changed", m_nId );
}

void Terrain3DMeshAsset::disconnectRenderingMaterialSignals( void ) {
	const Callable material_callback = callable_mp( this, &Terrain3DMeshAsset::onRenderingMaterialChanged );
	if ( m_renderingMaterialOverride.is_valid() && m_renderingMaterialOverride->is_connected( "changed", material_callback ) ) {
		m_renderingMaterialOverride->disconnect( "changed", material_callback );
	}
	if ( m_renderingMaterialOverlay.is_valid() && m_renderingMaterialOverlay->is_connected( "changed", material_callback ) ) {
		m_renderingMaterialOverlay->disconnect( "changed", material_callback );
	}
	for ( int64_t iMaterial = 0; iMaterial < m_surfaceRenderingMaterials.size(); ++iMaterial ) {
		const Ref<Terrain3DRenderingMaterial> material = m_surfaceRenderingMaterials[iMaterial];
		if ( material.is_valid() && material->is_connected( "changed", material_callback ) ) {
			material->disconnect( "changed", material_callback );
		}
	}
}

void Terrain3DMeshAsset::connectRenderingMaterialSignals( void ) {
	const Callable material_callback = callable_mp( this, &Terrain3DMeshAsset::onRenderingMaterialChanged );
	if ( m_renderingMaterialOverride.is_valid() && !m_renderingMaterialOverride->is_connected( "changed", material_callback ) ) {
		m_renderingMaterialOverride->connect( "changed", material_callback );
	}
	if ( m_renderingMaterialOverlay.is_valid() && !m_renderingMaterialOverlay->is_connected( "changed", material_callback ) ) {
		m_renderingMaterialOverlay->connect( "changed", material_callback );
	}
	for ( int64_t iMaterial = 0; iMaterial < m_surfaceRenderingMaterials.size(); ++iMaterial ) {
		const Ref<Terrain3DRenderingMaterial> material = m_surfaceRenderingMaterials[iMaterial];
		if ( material.is_valid() && !material->is_connected( "changed", material_callback ) ) {
			material->connect( "changed", material_callback );
		}
	}
}

String Terrain3DMeshAsset::getVtMaterialOwnerIdentityCold( void ) const {
	const String asset_path = get_path();
	if ( !asset_path.is_empty() ) {
		return String( "mesh-asset:" ) + asset_path;
	}
	if ( m_packedScene.is_valid() && !m_packedScene->get_path().is_empty() ) {
		return String( "mesh-scene:" ) + m_packedScene->get_path() + String( ":name:" ) + m_name;
	}
	if ( !m_name.is_empty() ) {
		return String( "mesh-name:" ) + m_name + String( ":generated:" ) + String::num_int64( ( int64_t )m_generatedType );
	}
	return String();
}

bool Terrain3DMeshAsset::ensureRenderingMaterialStableKeysCold( void ) {
	const String owner = getVtMaterialOwnerIdentityCold();
	if ( m_renderingMaterialOverride.is_valid() && !m_renderingMaterialOverride->ensureVtStableKeyCold( owner, String( "override" ) ) ) {
		return false;
	}
	if ( m_renderingMaterialOverlay.is_valid() && !m_renderingMaterialOverlay->ensureVtStableKeyCold( owner, String( "overlay" ) ) ) {
		return false;
	}
	for ( int64_t iMaterial = 0; iMaterial < m_surfaceRenderingMaterials.size(); ++iMaterial ) {
		const Ref<Terrain3DRenderingMaterial> material = m_surfaceRenderingMaterials[iMaterial];
		if ( material.is_null() ) {
			continue;
		}
		const String role = String( "surface:" ) + String::num_int64( iMaterial );
		if ( !material->ensureVtStableKeyCold( owner, role ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DMeshAsset::capturePendingSurfaceRenderingMaterialsCold( void ) {
	TypedArray<Terrain3DRenderingMaterial> materials;
	for ( int lod = 0; lod < m_pendingMeshes.size(); ++lod ) {
		const Ref<Mesh> mesh = m_pendingMeshes[lod];
		if ( mesh.is_null() ) {
			continue;
		}
		for ( int surface = 0; surface < mesh->get_surface_count(); ++surface ) {
			Ref<Terrain3DRenderingMaterial> canonical;
			canonical.instantiate();
			const Ref<Material> source = mesh->surface_get_material( surface );
			if ( source.is_valid() && !canonical->importBaseMaterialCold( source ) ) {
				LOG( ERROR, "LOD ", lod, " surface ", surface, " cannot be converted to Terrain3DRenderingMaterial" );
				return false;
			}
			if ( source.is_valid() && !source->get_path().is_empty() && !source->get_path().contains( "::" ) ) {
				canonical->setVtStableKey( t3d_vt_material_stable_key_from_resource_path( source->get_path() ) );
			} else {
				const String role = String( "pending-surface:" ) + String::num_int64( materials.size() );
				if ( !canonical->ensureVtStableKeyCold( getVtMaterialOwnerIdentityCold(), role ) ) {
					LOG( ERROR, "LOD ", lod, " surface ", surface, " has no deterministic VT material identity" );
					return false;
				}
			}
			canonical->set_meta( "terrain3d_surface_material_conversion", true );
			materials.push_back( canonical );
		}
	}
	m_pendingSurfaceRenderingMaterials = materials;
	return true;
}

void Terrain3DMeshAsset::setSurfaceRenderingMaterials( const TypedArray<Terrain3DRenderingMaterial> &rMaterials ) {
	disconnectRenderingMaterialSignals();
	m_surfaceRenderingMaterials = rMaterials;
	if ( !ensureRenderingMaterialStableKeysCold() ) {
		LOG( ERROR, "ID ", m_nId, ", ", m_name, ": Unable to establish stable VT identities for surface materials" );
	}
	connectRenderingMaterialSignals();
	invalidateThumbnail();
	emit_signal( "vt_material_changed" );
	emit_signal( "instancer_setting_changed", m_nId );
}

Ref<Terrain3DRenderingMaterial> Terrain3DMeshAsset::getSurfaceRenderingMaterial( const int nLod, const uint32_t nSurface ) const {
	if ( m_renderingMaterialOverride.is_valid() ) {
		return m_renderingMaterialOverride;
	}
	if ( nLod < 0 || nLod >= m_meshes.size() ) {
		return Ref<Terrain3DRenderingMaterial>();
	}
	uint64_t offset = 0u;
	for ( int lod = 0; lod < nLod; ++lod ) {
		const Ref<Mesh> mesh = m_meshes[lod];
		if ( mesh.is_valid() ) {
			offset += ( uint64_t )( MAX( mesh->get_surface_count(), 0 ) );
		}
	}
	const Ref<Mesh> mesh = m_meshes[nLod];
	if ( mesh.is_null() || nSurface >= ( uint32_t )( mesh->get_surface_count() ) ) {
		return Ref<Terrain3DRenderingMaterial>();
	}
	const uint64_t index = offset + nSurface;
	if ( index >= ( uint64_t )( m_surfaceRenderingMaterials.size() ) ) {
		return Ref<Terrain3DRenderingMaterial>();
	}
	return m_surfaceRenderingMaterials[( int64_t )( index )];
}

const Terrain3DRenderingMaterial *Terrain3DMeshAsset::getSurfaceRenderingMaterialPtr( const int nLod, const uint32_t nSurface ) const {
	const Ref<Terrain3DRenderingMaterial> material = getSurfaceRenderingMaterial( nLod, nSurface );
	return material.ptr();
}

void Terrain3DMeshAsset::setRenderingMaterialOverride( const Ref<Terrain3DRenderingMaterial> &rMaterial ) {
	if ( !t3dDiffers( m_renderingMaterialOverride, rMaterial ) ) {
		return;
	}
	disconnectRenderingMaterialSignals();
	m_renderingMaterialOverride = rMaterial;
	if ( m_renderingMaterialOverride.is_valid()
	        && !m_renderingMaterialOverride->ensureVtStableKeyCold( getVtMaterialOwnerIdentityCold(), String( "override" ) ) ) {
		LOG( ERROR, "ID ", m_nId, ", ", m_name, ": Rendering-material override has no deterministic VT identity" );
	}
	connectRenderingMaterialSignals();
	invalidateThumbnail();
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting Terrain3D rendering material override: ", rMaterial );
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "vt_material_changed" );
	emit_signal( "instancer_setting_changed", m_nId );
}

void Terrain3DMeshAsset::setRenderingMaterialOverlay( const Ref<Terrain3DRenderingMaterial> &rMaterial ) {
	if ( !t3dDiffers( m_renderingMaterialOverlay, rMaterial ) ) {
		return;
	}
	disconnectRenderingMaterialSignals();
	m_renderingMaterialOverlay = rMaterial;
	if ( m_renderingMaterialOverlay.is_valid()
	        && !m_renderingMaterialOverlay->ensureVtStableKeyCold( getVtMaterialOwnerIdentityCold(), String( "overlay" ) ) ) {
		LOG( ERROR, "ID ", m_nId, ", ", m_name, ": Rendering-material overlay has no deterministic VT identity" );
	}
	connectRenderingMaterialSignals();
	invalidateThumbnail();
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting Terrain3D rendering material overlay: ", rMaterial );
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "vt_material_changed" );
	emit_signal( "instancer_setting_changed", m_nId );
}

void Terrain3DMeshAsset::setGeneratedFaces( const int nCount ) {
	SET_IF_DIFF( m_nGeneratedFaces, CLAMP( nCount, 1, 3 ) );
	invalidateThumbnail();
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting generated face count: ", m_nGeneratedFaces );
	// If already a texture card, just recreate the mesh w/o other settings
	if ( m_generatedType > TYPE_NONE && m_generatedType < MESH_TYPE_MAX && getMeshes().size() == 1 ) {
		assignGeneratedMesh();
	}
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void Terrain3DMeshAsset::setGeneratedSize( const Vector2 &rSize ) {
	SET_IF_DIFF( m_generatedSize, rSize );
	invalidateThumbnail();
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting generated size: ", m_generatedSize );
	// If already a texture card, just recreate the mesh w/o other settings
	if ( m_generatedType > TYPE_NONE && m_generatedType < MESH_TYPE_MAX && getMeshes().size() == 1 ) {
		assignGeneratedMesh();
	}
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void Terrain3DMeshAsset::setLastLod( const int nLod ) {
	int max_lod = m_generatedType != TYPE_NONE ? 0 : CLAMP( getMeshes().size(), 1, s_nMeshMaxLodCount ) - 1;
	max_lod = MIN( max_lod, getCategoryMaximumLodCount() - 1 );
	SET_IF_DIFF( m_iLastLod, CLAMP( nLod, 0, max_lod ) );
	if ( m_iLastShadowLod > m_iLastLod ) {
		m_iLastShadowLod = m_iLastLod;
	}
	if ( m_nShadowImpostor > m_iLastLod ) {
		m_nShadowImpostor = m_iLastLod;
	}
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting last LOD: ", m_iLastLod );
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
	notify_property_list_changed();
}

void Terrain3DMeshAsset::setLastShadowLod( const int nLod ) {
	SET_IF_DIFF( m_iLastShadowLod, CLAMP( nLod, 0, m_iLastLod ) );
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting last shadow LOD: ", m_iLastShadowLod );
	if ( m_nShadowImpostor > m_iLastShadowLod ) {
		m_nShadowImpostor = m_iLastShadowLod;
	}
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void Terrain3DMeshAsset::setShadowImpostor( const int nLod ) {
	SET_IF_DIFF( m_nShadowImpostor, CLAMP( nLod, 0, MIN( m_iLastLod, m_iLastShadowLod ) ) );
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting shadow imposter LOD: ", m_nShadowImpostor );
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void Terrain3DMeshAsset::setLodRange( const int nLod, const real_t fDistance ) {
	if ( nLod < 0 || nLod >= s_nMeshMaxLodCount ) {
		LOG( ERROR, "p_lod out of range. Valid range is 0 - ", s_nMeshMaxLodCount - 1 );
		return;
	}
	SET_IF_DIFF( m_fLodRanges[nLod], CLAMP( ( float )( fDistance ), 0.f, 100000.f ) );
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting LOD ", nLod, " visibility range: ", m_fLodRanges[nLod] );
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

real_t Terrain3DMeshAsset::getLodRange( const int nLod ) const {
	if ( nLod < 0 || nLod >= s_nMeshMaxLodCount ) {
		return -1.f;
	}
	return m_fLodRanges[nLod];
}

real_t Terrain3DMeshAsset::getLodRangeBegin( const int nLod ) const {
	if ( nLod <= 0 ) {
		return 0.f;
	}
	ASSERT( nLod <= m_iLastLod, 0.f );
	return m_fLodRanges[nLod - 1];
}

real_t Terrain3DMeshAsset::getLodRangeEnd( const int nLod ) const {
	if ( nLod == s_nMeshShadowLodId ) {
		return m_fLodRanges[MAX( m_nShadowImpostor - 1, 0 )];
	}
	ASSERT( nLod >= 0 && nLod <= m_iLastLod, 0.f );
	return m_fLodRanges[nLod];
}

void Terrain3DMeshAsset::setFadeMargin( const real_t fFadeMargin ) {
	const int max_range = CLAMP( m_fLodRanges[1] - m_fLodRanges[0], 0.f, 64.f );
	SET_IF_DIFF( m_fFadeMargin, CLAMP( fFadeMargin, 0.f, max_range ) );
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting visibility margin: ", m_fFadeMargin );
	LOG( DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

// -----------------------------------------------------------------------------
// Protected functions

void Terrain3DMeshAsset::_validate_property( PropertyInfo &rProperty ) const {
	if ( rProperty.name != StringName( "generated_type" ) && rProperty.name.begins_with( "generated_" ) ) {
		if ( m_generatedType == TYPE_NONE ) {
			rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
		} else {
			rProperty.usage = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY;
		}
		return;
	} else if ( rProperty.name == StringName( "hlod_contribution" ) ) {
		rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
		return;
	} else if ( m_instanceCategory == INSTANCE_CATEGORY_CLUTTER && rProperty.name == StringName( "hlod_min_level" ) ) {
		rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
		return;
	} else if ( rProperty.name == StringName( "instance_occlusion_box" ) ) {
		rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
		return;
	} else if ( rProperty.name == StringName( "instance_occlusion_proxy_mesh" ) ) {
		if ( !usesInstanceOcclusion() ) {
			rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
		} else if ( m_packedScene.is_valid() ) {
			rProperty.usage = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY;
		}
		return;
	} else if ( rProperty.name == StringName( "instance_occlusion_range" ) && !usesInstanceOcclusion() ) {
		rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
		return;
	} else if ( rProperty.name.match( "lod?_range" ) ) {
		const int lod = rProperty.name.substr( 3, 1 ).to_int();
		const int effective_last_lod = MIN( m_iLastLod, getCategoryMaximumLodCount() - 1 );
		if ( effective_last_lod < lod ) {
			rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
		} else {
			rProperty.usage = PROPERTY_USAGE_DEFAULT;
		}
	}
}

bool Terrain3DMeshAsset::_property_can_revert( const StringName &rName ) const {
	return ( rName.match( "instance_category" ) || rName.match( "instance_occlusion_policy" ) || rName.match( "instance_occlusion_range" )
	        || rName.match( "terrain_alignment" ) || rName.match( "last_lod" ) || rName.match( "last_shadow_lod" )
	        || rName.match( "density" ) || rName.match( "generated_type" )
	        || ( m_generatedType > TYPE_NONE && ( rName.match( "height_offset" ) || rName.match( "lod0_range" ) ) ) );
}

bool Terrain3DMeshAsset::_property_get_revert( const StringName &rName, Variant &rProperty ) const {
	if ( rName.match( "instance_category" ) ) {
		rProperty = INSTANCE_CATEGORY_GROUND_COVER;
		return true;
	} else if ( rName.match( "instance_occlusion_policy" ) ) {
		rProperty = m_instanceCategory == INSTANCE_CATEGORY_OCCLUDER ? INSTANCE_OCCLUSION_EXPLICIT_MESH : INSTANCE_OCCLUSION_NONE;
		return true;
	} else if ( rName.match( "instance_occlusion_range" ) ) {
		rProperty = 256.0f;
		return true;
	} else if ( rName.match( "terrain_alignment" ) ) {
		rProperty = TERRAIN_ALIGNMENT_UPRIGHT;
		return true;
	} else if ( rName.match( "last_lod" ) ) {
		rProperty = MIN( getLodCount() - 1, getCategoryMaximumLodCount() - 1 );
		return true;
	} else if ( rName.match( "last_shadow_lod" ) ) {
		rProperty = MIN( getLodCount() - 1, getCategoryMaximumLodCount() - 1 );
		return true;
	} else if ( rName.match( "density" ) ) {
		rProperty = 10.f;
		return true;
	} else if ( rName.match( "generated_type" ) ) {
		rProperty = TYPE_TEXTURE_CARD;
		return true;
	} else if ( m_generatedType > TYPE_NONE ) {
		if ( rName.match( "height_offset" ) ) {
			rProperty = 0.5f;
			return true;
		} else if ( rName.match( "lod0_range" ) ) {
			rProperty = 128.f;
			return true;
		}
	}
	return false;
}

void Terrain3DMeshAsset::setInstanceCategory( const t3dInstanceCategory_t eCategory ) {
	const t3dInstanceCategory_t value = ( t3dInstanceCategory_t )CLAMP(
	        ( int )( eCategory ), ( int )( INSTANCE_CATEGORY_CLUTTER ), ( int )( INSTANCE_CATEGORY_OCCLUDER ) );
	const t3dHlodContribution_t desired_hlod = value == INSTANCE_CATEGORY_CLUTTER ? HLOD_EXCLUDE : HLOD_INCLUDE;
	const t3dInstanceOcclusionPolicy_t desired_occlusion =
	        value == INSTANCE_CATEGORY_OCCLUDER ? INSTANCE_OCCLUSION_EXPLICIT_MESH : INSTANCE_OCCLUSION_NONE;
	const int desired_last_lod = value == INSTANCE_CATEGORY_CLUTTER ? MIN( m_iLastLod, 1 ) : m_iLastLod;
	const int desired_last_shadow_lod = MIN( m_iLastShadowLod, desired_last_lod );
	const int desired_shadow_impostor = MIN( m_nShadowImpostor, desired_last_lod );
	const real_t desired_fade = value == INSTANCE_CATEGORY_CLUTTER && m_fFadeMargin <= 0.f
	        ? CLAMP( m_fLodRanges[1] - m_fLodRanges[0], 0.f, 64.f )
	        : m_fFadeMargin;
	if ( m_instanceCategory == value && m_hlodContribution == desired_hlod && m_instanceOcclusionPolicy == desired_occlusion
	        && m_iLastLod == desired_last_lod && m_iLastShadowLod == desired_last_shadow_lod && m_nShadowImpostor == desired_shadow_impostor
	        && m_fFadeMargin == desired_fade ) {
		return;
	}

	m_instanceCategory = value;
	m_hlodContribution = desired_hlod;
	m_instanceOcclusionPolicy = desired_occlusion;
	m_iLastLod = desired_last_lod;
	m_iLastShadowLod = desired_last_shadow_lod;
	m_nShadowImpostor = desired_shadow_impostor;
	m_fFadeMargin = desired_fade;
	if ( m_instanceCategory == INSTANCE_CATEGORY_CLUTTER ) {
		clearInstanceDerivedGeometry();
	} else if ( !m_meshes.is_empty() ) {
		rebuildInstanceDerivedGeometry();
	}

	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting instance category: ", ( int )( m_instanceCategory ) );
	emit_signal( "setting_changed" );
	emit_signal( "instancer_setting_changed", m_nId );
	emit_changed();
	notify_property_list_changed();
}

void Terrain3DMeshAsset::setInstanceOcclusionPolicy( const t3dInstanceOcclusionPolicy_t ePolicy ) {
	t3dInstanceOcclusionPolicy_t value = ( t3dInstanceOcclusionPolicy_t )CLAMP(
	        ( int )( ePolicy ), ( int )( INSTANCE_OCCLUSION_NONE ), ( int )( INSTANCE_OCCLUSION_EXPLICIT_MESH ) );
	if ( m_instanceCategory != INSTANCE_CATEGORY_OCCLUDER ) {
		value = INSTANCE_OCCLUSION_NONE;
	} else if ( value != INSTANCE_OCCLUSION_NONE ) {
		value = INSTANCE_OCCLUSION_EXPLICIT_MESH;
	}
	if ( m_instanceOcclusionPolicy == value ) {
		return;
	}
	m_instanceOcclusionPolicy = value;
	emit_signal( "setting_changed" );
	emit_signal( "instancer_setting_changed", m_nId );
	emit_changed();
	notify_property_list_changed();
}

void Terrain3DMeshAsset::setInstanceOcclusionBox( const AABB &rBox ) {
	if ( m_packedScene.is_valid() && m_instanceCategory == INSTANCE_CATEGORY_OCCLUDER ) {
		if ( !m_meshes.is_empty() ) {
			rebuildInstanceOcclusionGeometry();
		}
		return;
	}
	AABB value = rBox;
	value.size.x = MAX( Math::abs( value.size.x ), 0.01f );
	value.size.y = MAX( Math::abs( value.size.y ), 0.01f );
	value.size.z = MAX( Math::abs( value.size.z ), 0.01f );
	if ( m_instanceOcclusionBox == value ) {
		return;
	}
	m_instanceOcclusionBox = value;
	emit_signal( "setting_changed" );
	emit_signal( "instancer_setting_changed", m_nId );
	emit_changed();
}

void Terrain3DMeshAsset::setInstanceOcclusionProxyMesh( const Ref<ArrayMesh> &rMesh ) {
	if ( m_packedScene.is_valid() && m_instanceCategory == INSTANCE_CATEGORY_OCCLUDER ) {
		if ( !m_meshes.is_empty() ) {
			rebuildInstanceOcclusionGeometry();
		}
		return;
	}
	if ( m_instanceOcclusionProxyMesh == rMesh ) {
		return;
	}
	m_instanceOcclusionProxyMesh = rMesh;
	m_instanceOccluder.unref();
	m_nInstanceOcclusionProxyVertexCount = 0u;
	m_nInstanceOcclusionProxyTriangleCount = 0u;
	m_bInstanceOcclusionProxyDerived = false;
	m_bInstanceProxyImportedOccluder = false;
	if ( ++m_nInstanceOcclusionProxyGeneration == 0u ) {
		m_nInstanceOcclusionProxyGeneration = 1u;
	}
	emit_signal( "setting_changed" );
	emit_signal( "instancer_setting_changed", m_nId );
	emit_changed();
}

void Terrain3DMeshAsset::setInstanceOcclusionRange( const real_t fRange ) {
	const real_t value = CLAMP( fRange, 0.0f, 16384.0f );
	if ( Math::is_equal_approx( m_fInstanceOcclusionRange, value ) ) {
		return;
	}
	m_fInstanceOcclusionRange = value;
	emit_signal( "setting_changed" );
	emit_signal( "instancer_setting_changed", m_nId );
	emit_changed();
}

void Terrain3DMeshAsset::setTerrainAlignment( const t3dTerrainAlignment_t eAlignment ) {
	const t3dTerrainAlignment_t value = ( t3dTerrainAlignment_t )CLAMP(
	        ( int )( eAlignment ), ( int )( TERRAIN_ALIGNMENT_UPRIGHT ), ( int )( TERRAIN_ALIGNMENT_NORMAL ) );
	if ( m_terrainAlignment == value ) {
		return;
	}
	m_terrainAlignment = value;
	LOG( INFO, "ID ", m_nId, ", ", m_name, ": Setting terrain alignment: ", ( int )( m_terrainAlignment ) );
	emit_signal( "setting_changed" );
	emit_changed();
}

void Terrain3DMeshAsset::setHlodContribution( const t3dHlodContribution_t mode ) {
	( void )( mode );
	const t3dHlodContribution_t value = m_instanceCategory == INSTANCE_CATEGORY_CLUTTER ? HLOD_EXCLUDE : HLOD_INCLUDE;
	if ( m_hlodContribution == value ) {
		return;
	}
	m_hlodContribution = value;
	emit_signal( "setting_changed" );
	emit_changed();
}

void Terrain3DMeshAsset::setHlodMinLevel( const int nLevel ) {
	const int value = CLAMP( nLevel, 1, 6 );
	if ( m_nHlodMinLevel == value ) {
		return;
	}
	m_nHlodMinLevel = value;
	emit_signal( "setting_changed" );
	emit_changed();
}

void Terrain3DMeshAsset::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( INSTANCE_CATEGORY_CLUTTER );
	BIND_ENUM_CONSTANT( INSTANCE_CATEGORY_GROUND_COVER );
	BIND_ENUM_CONSTANT( INSTANCE_CATEGORY_OCCLUDER );
	BIND_ENUM_CONSTANT( TERRAIN_ALIGNMENT_UPRIGHT );
	BIND_ENUM_CONSTANT( TERRAIN_ALIGNMENT_NORMAL );
	BIND_ENUM_CONSTANT( HLOD_AUTO );
	BIND_ENUM_CONSTANT( HLOD_INCLUDE );
	BIND_ENUM_CONSTANT( HLOD_EXCLUDE );
	BIND_ENUM_CONSTANT( INSTANCE_OCCLUSION_NONE );
	BIND_ENUM_CONSTANT( INSTANCE_OCCLUSION_BOX );
	BIND_ENUM_CONSTANT( INSTANCE_OCCLUSION_EXPLICIT_MESH );
	ClassDB::bind_method( D_METHOD( "set_instance_category", "category" ), &Terrain3DMeshAsset::setInstanceCategory );
	ClassDB::bind_method( D_METHOD( "get_instance_category" ), &Terrain3DMeshAsset::getInstanceCategory );
	ClassDB::bind_method( D_METHOD( "get_category_maximum_lod_count" ), &Terrain3DMeshAsset::getCategoryMaximumLodCount );
	ClassDB::bind_method( D_METHOD( "is_individually_editable" ), &Terrain3DMeshAsset::isIndividuallyEditable );
	ClassDB::bind_method( D_METHOD( "requires_instance_physics" ), &Terrain3DMeshAsset::requiresInstancePhysics );
	ClassDB::bind_method( D_METHOD( "uses_instance_occlusion" ), &Terrain3DMeshAsset::usesInstanceOcclusion );
	ClassDB::bind_method( D_METHOD( "uses_full_instance_occlusion" ), &Terrain3DMeshAsset::usesFullInstanceOcclusion );
	ClassDB::bind_method( D_METHOD( "set_instance_occlusion_policy", "policy" ), &Terrain3DMeshAsset::setInstanceOcclusionPolicy );
	ClassDB::bind_method( D_METHOD( "get_instance_occlusion_policy" ), &Terrain3DMeshAsset::getInstanceOcclusionPolicy );
	ClassDB::bind_method( D_METHOD( "set_instance_occlusion_box", "box" ), &Terrain3DMeshAsset::setInstanceOcclusionBox );
	ClassDB::bind_method( D_METHOD( "get_instance_occlusion_box" ), &Terrain3DMeshAsset::getInstanceOcclusionBox );
	ClassDB::bind_method( D_METHOD( "set_instance_occlusion_proxy_mesh", "mesh" ), &Terrain3DMeshAsset::setInstanceOcclusionProxyMesh );
	ClassDB::bind_method( D_METHOD( "get_instance_occlusion_proxy_mesh" ), &Terrain3DMeshAsset::getInstanceOcclusionProxyMesh );
	ClassDB::bind_method( D_METHOD( "has_instance_collision_proxy" ), &Terrain3DMeshAsset::hasInstanceCollisionProxy );
	ClassDB::bind_method( D_METHOD( "has_instance_occlusion_proxy" ), &Terrain3DMeshAsset::hasInstanceOcclusionProxy );
	ClassDB::bind_method( D_METHOD( "has_mesh_derived_instance_proxy" ), &Terrain3DMeshAsset::hasMeshDerivedInstanceProxy );
	ClassDB::bind_method( D_METHOD( "is_instance_proxy_imported_occluder" ), &Terrain3DMeshAsset::isInstanceProxyImportedOccluder );
	ClassDB::bind_method( D_METHOD( "get_instance_occluder" ), &Terrain3DMeshAsset::getInstanceOccluder );
	ClassDB::bind_method( D_METHOD( "get_instance_collision_shape" ), &Terrain3DMeshAsset::getInstanceCollisionShape );
	ClassDB::bind_method( D_METHOD( "get_instance_collision_vertex_count" ), &Terrain3DMeshAsset::getInstanceCollisionVertexCount );
	ClassDB::bind_method( D_METHOD( "get_instance_collision_triangle_count" ), &Terrain3DMeshAsset::getInstanceCollisionTriangleCount );
	ClassDB::bind_method( D_METHOD( "get_instance_collision_generation" ), &Terrain3DMeshAsset::getInstanceCollisionGeneration );
	ClassDB::bind_method(
	        D_METHOD( "get_instance_occlusion_proxy_vertex_count" ), &Terrain3DMeshAsset::getInstanceOcclusionProxyVertexCount );
	ClassDB::bind_method(
	        D_METHOD( "get_instance_occlusion_proxy_triangle_count" ), &Terrain3DMeshAsset::getInstanceOcclusionProxyTriangleCount );
	ClassDB::bind_method( D_METHOD( "get_instance_occlusion_proxy_generation" ), &Terrain3DMeshAsset::getInstanceOcclusionProxyGeneration );
	ClassDB::bind_method( D_METHOD( "get_instance_proxy_vertex_count" ), &Terrain3DMeshAsset::getInstanceProxyVertexCount );
	ClassDB::bind_method( D_METHOD( "get_instance_proxy_triangle_count" ), &Terrain3DMeshAsset::getInstanceProxyTriangleCount );
	ClassDB::bind_method( D_METHOD( "set_instance_occlusion_range", "distance" ), &Terrain3DMeshAsset::setInstanceOcclusionRange );
	ClassDB::bind_method( D_METHOD( "get_instance_occlusion_range" ), &Terrain3DMeshAsset::getInstanceOcclusionRange );
	ClassDB::bind_method( D_METHOD( "set_terrain_alignment", "alignment" ), &Terrain3DMeshAsset::setTerrainAlignment );
	ClassDB::bind_method( D_METHOD( "get_terrain_alignment" ), &Terrain3DMeshAsset::getTerrainAlignment );
	ClassDB::bind_method( D_METHOD( "aligns_to_terrain_normal" ), &Terrain3DMeshAsset::alignsToTerrainNormal );
	ClassDB::bind_method( D_METHOD( "set_hlod_contribution", "mode" ), &Terrain3DMeshAsset::setHlodContribution );
	ClassDB::bind_method( D_METHOD( "get_hlod_contribution" ), &Terrain3DMeshAsset::getHlodContribution );
	ClassDB::bind_method( D_METHOD( "set_hlod_min_level", "level" ), &Terrain3DMeshAsset::setHlodMinLevel );
	ClassDB::bind_method( D_METHOD( "get_hlod_min_level" ), &Terrain3DMeshAsset::getHlodMinLevel );
	ClassDB::bind_method( D_METHOD( "is_hlod_eligible" ), &Terrain3DMeshAsset::isHlodEligible );
	BIND_ENUM_CONSTANT( TYPE_NONE );
	BIND_ENUM_CONSTANT( TYPE_TEXTURE_CARD );
	BIND_ENUM_CONSTANT( MESH_TYPE_MAX );

	ADD_SIGNAL( MethodInfo( "id_changed" ) );
	ADD_SIGNAL( MethodInfo( "setting_changed" ) );
	ADD_SIGNAL( MethodInfo( "instancer_setting_changed" ) );
	ADD_SIGNAL( MethodInfo( "vt_material_changed" ) );
	ADD_SIGNAL( MethodInfo( "preview_changed" ) );
	ADD_SIGNAL( MethodInfo( "instance_count_changed" ) );

	ClassDB::bind_method( D_METHOD( "clear" ), &Terrain3DMeshAsset::clear );
	ClassDB::bind_method( D_METHOD( "set_name", "name" ), &Terrain3DMeshAsset::setName );
	ClassDB::bind_method( D_METHOD( "get_name" ), &Terrain3DMeshAsset::getName );
	ClassDB::bind_method( D_METHOD( "set_id", "id" ), &Terrain3DMeshAsset::setId );
	ClassDB::bind_method( D_METHOD( "get_id" ), &Terrain3DMeshAsset::getId );
	ClassDB::bind_method( D_METHOD( "set_highlighted", "enabled" ), &Terrain3DMeshAsset::setHighlighted );
	ClassDB::bind_method( D_METHOD( "is_highlighted" ), &Terrain3DMeshAsset::isHighlighted );
	ClassDB::bind_method( D_METHOD( "get_highlight_color" ), &Terrain3DMeshAsset::getHighlightColor );
	ClassDB::bind_method( D_METHOD( "get_thumbnail" ), &Terrain3DMeshAsset::getThumbnail );

	ClassDB::bind_method( D_METHOD( "set_enabled", "enabled" ), &Terrain3DMeshAsset::setEnabled );
	ClassDB::bind_method( D_METHOD( "is_enabled" ), &Terrain3DMeshAsset::isEnabled );

	ClassDB::bind_method( D_METHOD( "set_scene_file", "scene_file" ), &Terrain3DMeshAsset::setSceneFile );
	ClassDB::bind_method( D_METHOD( "get_scene_file" ), &Terrain3DMeshAsset::getSceneFile );
	ClassDB::bind_method( D_METHOD( "set_generated_type", "type" ), &Terrain3DMeshAsset::setGeneratedType );
	ClassDB::bind_method( D_METHOD( "get_generated_type" ), &Terrain3DMeshAsset::getGeneratedType );
	ClassDB::bind_method( D_METHOD( "get_mesh", "lod" ), &Terrain3DMeshAsset::getMesh, DEFVAL( 0 ) );
	ClassDB::bind_method( D_METHOD( "set_height_offset", "offset" ), &Terrain3DMeshAsset::setHeightOffset );
	ClassDB::bind_method( D_METHOD( "get_height_offset" ), &Terrain3DMeshAsset::getHeightOffset );
	ClassDB::bind_method( D_METHOD( "set_density", "density" ), &Terrain3DMeshAsset::setDensity );
	ClassDB::bind_method( D_METHOD( "get_density" ), &Terrain3DMeshAsset::getDensity );
	ClassDB::bind_method( D_METHOD( "set_cast_shadows", "mode" ), &Terrain3DMeshAsset::setCastShadows );
	ClassDB::bind_method( D_METHOD( "get_cast_shadows" ), &Terrain3DMeshAsset::getCastShadows );
	ClassDB::bind_method( D_METHOD( "set_visibility_layers", "layers" ), &Terrain3DMeshAsset::setVisibilityLayers );
	ClassDB::bind_method( D_METHOD( "get_visibility_layers" ), &Terrain3DMeshAsset::getVisibilityLayers );
	ClassDB::bind_method( D_METHOD( "set_material_override", "material" ), &Terrain3DMeshAsset::setMaterialOverride );
	ClassDB::bind_method( D_METHOD( "get_material_override" ), &Terrain3DMeshAsset::getMaterialOverride );
	ClassDB::bind_method( D_METHOD( "set_material_overlay", "material" ), &Terrain3DMeshAsset::setMaterialOverlay );
	ClassDB::bind_method( D_METHOD( "get_material_overlay" ), &Terrain3DMeshAsset::getMaterialOverlay );
	ClassDB::bind_method( D_METHOD( "set_rendering_material_override", "material" ), &Terrain3DMeshAsset::setRenderingMaterialOverride );
	ClassDB::bind_method( D_METHOD( "get_rendering_material_override" ), &Terrain3DMeshAsset::getRenderingMaterialOverride );
	ClassDB::bind_method( D_METHOD( "set_rendering_material_overlay", "material" ), &Terrain3DMeshAsset::setRenderingMaterialOverlay );
	ClassDB::bind_method( D_METHOD( "get_rendering_material_overlay" ), &Terrain3DMeshAsset::getRenderingMaterialOverlay );
	ClassDB::bind_method( D_METHOD( "set_surface_rendering_materials", "materials" ), &Terrain3DMeshAsset::setSurfaceRenderingMaterials );
	ClassDB::bind_method( D_METHOD( "get_surface_rendering_materials" ), &Terrain3DMeshAsset::getSurfaceRenderingMaterials );

	ClassDB::bind_method( D_METHOD( "set_generated_faces", "count" ), &Terrain3DMeshAsset::setGeneratedFaces );
	ClassDB::bind_method( D_METHOD( "get_generated_faces" ), &Terrain3DMeshAsset::getGeneratedFaces );
	ClassDB::bind_method( D_METHOD( "set_generated_size", "size" ), &Terrain3DMeshAsset::setGeneratedSize );
	ClassDB::bind_method( D_METHOD( "get_generated_size" ), &Terrain3DMeshAsset::getGeneratedSize );

	ClassDB::bind_method( D_METHOD( "get_lod_count" ), &Terrain3DMeshAsset::getLodCount );
	ClassDB::bind_method( D_METHOD( "set_last_lod", "lod" ), &Terrain3DMeshAsset::setLastLod );
	ClassDB::bind_method( D_METHOD( "get_last_lod" ), &Terrain3DMeshAsset::getLastLod );
	ClassDB::bind_method( D_METHOD( "set_last_shadow_lod", "lod" ), &Terrain3DMeshAsset::setLastShadowLod );
	ClassDB::bind_method( D_METHOD( "get_last_shadow_lod" ), &Terrain3DMeshAsset::getLastShadowLod );
	ClassDB::bind_method( D_METHOD( "set_shadow_impostor", "lod" ), &Terrain3DMeshAsset::setShadowImpostor );
	ClassDB::bind_method( D_METHOD( "get_shadow_impostor" ), &Terrain3DMeshAsset::getShadowImpostor );

	ClassDB::bind_method( D_METHOD( "set_lod_range", "lod", "distance" ), &Terrain3DMeshAsset::setLodRange );
	ClassDB::bind_method( D_METHOD( "get_lod_range", "lod" ), &Terrain3DMeshAsset::getLodRange );
	ClassDB::bind_method( D_METHOD( "set_lod0_range", "distance" ), &Terrain3DMeshAsset::setLod0Range );
	ClassDB::bind_method( D_METHOD( "get_lod0_range" ), &Terrain3DMeshAsset::getLod0Range );
	ClassDB::bind_method( D_METHOD( "set_lod1_range", "distance" ), &Terrain3DMeshAsset::setLod1Range );
	ClassDB::bind_method( D_METHOD( "get_lod1_range" ), &Terrain3DMeshAsset::getLod1Range );
	ClassDB::bind_method( D_METHOD( "set_lod2_range", "distance" ), &Terrain3DMeshAsset::setLod2Range );
	ClassDB::bind_method( D_METHOD( "get_lod2_range" ), &Terrain3DMeshAsset::getLod2Range );
	ClassDB::bind_method( D_METHOD( "set_lod3_range", "distance" ), &Terrain3DMeshAsset::setLod3Range );
	ClassDB::bind_method( D_METHOD( "get_lod3_range" ), &Terrain3DMeshAsset::getLod3Range );
	ClassDB::bind_method( D_METHOD( "set_lod4_range", "distance" ), &Terrain3DMeshAsset::setLod4Range );
	ClassDB::bind_method( D_METHOD( "get_lod4_range" ), &Terrain3DMeshAsset::getLod4Range );
	ClassDB::bind_method( D_METHOD( "set_lod5_range", "distance" ), &Terrain3DMeshAsset::setLod5Range );
	ClassDB::bind_method( D_METHOD( "get_lod5_range" ), &Terrain3DMeshAsset::getLod5Range );
	ClassDB::bind_method( D_METHOD( "set_lod6_range", "distance" ), &Terrain3DMeshAsset::setLod6Range );
	ClassDB::bind_method( D_METHOD( "get_lod6_range" ), &Terrain3DMeshAsset::getLod6Range );
	ClassDB::bind_method( D_METHOD( "set_lod7_range", "distance" ), &Terrain3DMeshAsset::setLod7Range );
	ClassDB::bind_method( D_METHOD( "get_lod7_range" ), &Terrain3DMeshAsset::getLod7Range );
	ClassDB::bind_method( D_METHOD( "set_lod8_range", "distance" ), &Terrain3DMeshAsset::setLod8Range );
	ClassDB::bind_method( D_METHOD( "get_lod8_range" ), &Terrain3DMeshAsset::getLod8Range );
	ClassDB::bind_method( D_METHOD( "set_lod9_range", "distance" ), &Terrain3DMeshAsset::setLod9Range );
	ClassDB::bind_method( D_METHOD( "get_lod9_range" ), &Terrain3DMeshAsset::getLod9Range );
	ClassDB::bind_method( D_METHOD( "set_fade_margin", "distance" ), &Terrain3DMeshAsset::setFadeMargin );
	ClassDB::bind_method( D_METHOD( "get_fade_margin" ), &Terrain3DMeshAsset::getFadeMargin );

	ClassDB::bind_method( D_METHOD( "get_instance_count" ), &Terrain3DMeshAsset::getInstanceCount );

	ADD_GROUP( "Instance Policy", "instance_" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instance_category", PROPERTY_HINT_ENUM, "Clutter,Ground Cover,Occluder" ),
	        "set_instance_category",
	        "get_instance_category" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instance_occlusion_policy", PROPERTY_HINT_ENUM, "None,Box,Explicit Mesh" ),
	        "set_instance_occlusion_policy",
	        "get_instance_occlusion_policy" );
	ADD_PROPERTY( PropertyInfo( Variant::AABB, "instance_occlusion_box" ), "set_instance_occlusion_box", "get_instance_occlusion_box" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "instance_occlusion_proxy_mesh", PROPERTY_HINT_RESOURCE_TYPE, "ArrayMesh" ),
	        "set_instance_occlusion_proxy_mesh",
	        "get_instance_occlusion_proxy_mesh" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "instance_occlusion_range", PROPERTY_HINT_RANGE, "0,16384,1,or_greater" ),
	        "set_instance_occlusion_range",
	        "get_instance_occlusion_range" );

	ADD_GROUP( "HLOD", "hlod_" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_contribution", PROPERTY_HINT_ENUM, "Auto,Include,Exclude" ),
	        "set_hlod_contribution",
	        "get_hlod_contribution" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::INT, "hlod_min_level", PROPERTY_HINT_RANGE, "1,6,1" ), "set_hlod_min_level", "get_hlod_min_level" );

	ADD_GROUP( "General", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "name", PROPERTY_HINT_NONE ), "set_name", "get_name" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "id", PROPERTY_HINT_NONE ), "set_id", "get_id" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "enabled", PROPERTY_HINT_NONE ), "set_enabled", "is_enabled" );

	ADD_GROUP( "Source / Placement", "" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::OBJECT, "scene_file", PROPERTY_HINT_RESOURCE_TYPE, "PackedScene" ), "set_scene_file", "get_scene_file" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "generated_type", PROPERTY_HINT_ENUM, "None,Texture Card", PROPERTY_USAGE_STORAGE ),
	        "set_generated_type",
	        "get_generated_type" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "terrain_alignment", PROPERTY_HINT_ENUM, "Upright,Align To Terrain Normal" ),
	        "set_terrain_alignment",
	        "get_terrain_alignment" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "height_offset", PROPERTY_HINT_RANGE, "-20.0,20.0,.005" ),
	        "set_height_offset",
	        "get_height_offset" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "density", PROPERTY_HINT_RANGE, ".01,10.0,.005" ), "set_density", "get_density" );

	ADD_GROUP( "Rendering", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cast_shadows", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only" ),
	        "set_cast_shadows",
	        "get_cast_shadows" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "visibility_layers", PROPERTY_HINT_LAYERS_3D_RENDER ),
	        "set_visibility_layers",
	        "get_visibility_layers" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "rendering_material_override", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DRenderingMaterial" ),
	        "set_rendering_material_override",
	        "get_rendering_material_override" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "rendering_material_overlay", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DRenderingMaterial" ),
	        "set_rendering_material_overlay",
	        "get_rendering_material_overlay" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY,
	                      "surface_rendering_materials",
	                      PROPERTY_HINT_ARRAY_TYPE,
	                      "Terrain3DRenderingMaterial",
	                      PROPERTY_USAGE_STORAGE ),
	        "set_surface_rendering_materials",
	        "get_surface_rendering_materials" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT,
	                      "material_override",
	                      PROPERTY_HINT_RESOURCE_TYPE,
	                      "BaseMaterial3D,ShaderMaterial",
	                      PROPERTY_USAGE_STORAGE ),
	        "set_material_override",
	        "get_material_override" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT,
	                      "material_overlay",
	                      PROPERTY_HINT_RESOURCE_TYPE,
	                      "BaseMaterial3D,ShaderMaterial",
	                      PROPERTY_USAGE_STORAGE ),
	        "set_material_overlay",
	        "get_material_overlay" );

	ADD_GROUP( "Generated Mesh", "" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::INT, "generated_faces", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY ),
	        "set_generated_faces",
	        "get_generated_faces" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::VECTOR2, "generated_size", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY ),
	        "set_generated_size",
	        "get_generated_size" );

	ADD_GROUP( "LODs", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "lod_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
	        "",
	        "get_lod_count" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "last_lod", PROPERTY_HINT_NONE ), "set_last_lod", "get_last_lod" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "last_shadow_lod", PROPERTY_HINT_NONE ), "set_last_shadow_lod", "get_last_shadow_lod" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "shadow_impostor", PROPERTY_HINT_NONE ), "set_shadow_impostor", "get_shadow_impostor" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod0_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
	        "set_lod0_range",
	        "get_lod0_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod1_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
	        "set_lod1_range",
	        "get_lod1_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod2_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
	        "set_lod2_range",
	        "get_lod2_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod3_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
	        "set_lod3_range",
	        "get_lod3_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod4_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
	        "set_lod4_range",
	        "get_lod4_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod5_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
	        "set_lod5_range",
	        "get_lod5_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod6_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
	        "set_lod6_range",
	        "get_lod6_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod7_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
	        "set_lod7_range",
	        "get_lod7_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod8_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
	        "set_lod8_range",
	        "get_lod8_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod9_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
	        "set_lod9_range",
	        "get_lod9_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "fade_margin", PROPERTY_HINT_RANGE, "0.,64.0,.05,or_greater" ),
	        "set_fade_margin",
	        "get_fade_margin" );
}
