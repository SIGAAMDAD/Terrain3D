// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_paths.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>

#include "core/logger.h"
#include "assets/terrain_3d_mesh_asset.h"

///////////////////////////
// Private Functions
///////////////////////////

void T3DMeshAsset::clearLodRanges( void ) {
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": Clearing LOD Ranges, setting last_lod to 128" );
	for ( int i = 0; i < TERRAIN3D_MESH_MAX_LOD_COUNT; i++ ) {
		m_fLodRanges[i] = ( i + 1 ) * TERRAIN3D_INSTANCE_DEFAULT_CELL_SIZE;
	}
	m_fLodRanges[m_iLastLod] = MAX( m_fLodRanges[m_iLastLod], 128.f );
}

bool T3DMeshAsset::sortLodNodes( const Node *pA, const Node *pB ) {
	ASSERT( pA && pB, false );
	return pA->get_name().right( 1 ) < pB->get_name().right( 1 );
}

Ref<ArrayMesh> T3DMeshAsset::createGeneratedMesh( const t3dGenType_t type ) const {
	if ( type != TYPE_TEXTURE_CARD ) {
		LOG( T3DTerrain::ERROR, "Only TYPE_TEXTURE_CARD is currently implemented" );
		return Ref<ArrayMesh>();
	}
	LOG( T3DTerrain::EXTREME, "Regenerating new mesh" );
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
		const real_t fAngle = face == 0 ? 0.f : ( real_t ) ( face ) * Math_PI / ( real_t ) ( m_nGeneratedFaces );
		const int vertex_base = face * 4;
		for ( int j = 0; j < 2; ++j ) {
			for ( int i = 0; i < 2; ++i ) {
				const int vertex_index = vertex_base + j * 2 + i;
				const float fX = start_pos.x + ( float ) ( i ) * m_generatedSize.x;
				const float fZ = start_pos.y + ( float ) ( j ) * m_generatedSize.y;
				pVertexWrite[vertex_index] = Vector3( -fX, fZ, 0.f ).rotated( V3_UP, fAngle );
				pNormalWrite[vertex_index] = normal;
				const int tangent_index = vertex_index * 4;
				pTangentWrite[tangent_index] = 1.f;
				pTangentWrite[tangent_index + 1] = 0.f;
				pTangentWrite[tangent_index + 2] = 0.f;
				pTangentWrite[tangent_index + 3] = 1.f;
				pUvWrite[vertex_index] = Vector2( 1.f - ( float ) ( i ), 1.f - ( float ) ( j ) );
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

void T3DMeshAsset::assignGeneratedMesh( void ) {
	LOG( T3DTerrain::DEBUG, "Assiging generated mesh & lod settings" );
	m_pPackedScene.unref();
	m_pendingMeshes.clear();
	m_pendingMeshes.push_back( createGeneratedMesh() );
	m_iLastLod = 0;
	m_iLastShadowLod = 0;
	m_shadowImpostor = 0;
	if ( m_pMaterialOverride.is_null() ) {
		m_pMaterialOverride = getMaterial();
	}
	// If no existing meshes, commit immediately. Otherwise, will trigger on an instancer update
	if ( m_meshes.is_empty() ) {
		commitMeshes();
	}
}

Ref<Material> T3DMeshAsset::getMaterial( void ) {
	if ( m_pMaterialOverride.is_null() ) {
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
		return m_pMaterialOverride;
	}
}

///////////////////////////
// Public Functions
///////////////////////////

// Called by T3DAssets::_set_asset_list() and _set_asset()
void T3DMeshAsset::initialize( void ) {
	LOG( T3DTerrain::INFO, m_nId, ": ", m_name, ": initializing asset" );
	if ( m_pPackedScene.is_null() && m_generatedType == TYPE_NONE ) {
		LOG( T3DTerrain::DEBUG, "Blank mesh, setting up default texture card" );
		setGeneratedType( TYPE_TEXTURE_CARD );
		m_fHeightOffset = 0.5f;
		clearLodRanges();
	}
}

void T3DMeshAsset::clear( void ) {
	LOG( T3DTerrain::INFO, "Clearing MeshAsset" );
	m_name = "New Mesh";
	m_nId = 0;
	m_bEnabled = true;
	m_bHighlighted = false;
	m_pHighlightMat = Ref<Material>();
	m_pPackedScene.unref();
	m_meshes.clear();
	m_pendingMeshes.clear();
	m_generatedType = TYPE_NONE;
	m_nGeneratedFaces = 2;
	m_generatedSize = V2( 1.f );
	m_fHeightOffset = 0.f;
	m_fDensity = 0.f;
	m_castShadows = TERRAIN3D_SHADOW_CASTING_ON;
	m_visibilityLayers = 1;
	m_pMaterialOverride.unref();
	m_pMaterialOverlay.unref();
	m_iLastLod = TERRAIN3D_MESH_MAX_LOD_COUNT - 1;
	m_iLastShadowLod = TERRAIN3D_MESH_MAX_LOD_COUNT - 1;
	m_shadowImpostor = 0;
	clearLodRanges();
	m_fFadeMargin = 0.f;
	m_pThumbnail.unref();
}

void T3DMeshAsset::setName( const String &rName ) {
	if ( rName.length() > 96 ) {
		LOG( T3DTerrain::WARN, "Name too long, truncating to 96 characters" );
	}
	SET_IF_DIFF( m_name, rName.left( 96 ) );
	LOG( T3DTerrain::INFO, "ID ", m_nId, ": t3dSetting_t name: ", m_name );
	LOG( T3DTerrain::DEBUG, "Emitting setting_changed, ID: ", m_nId );
	emit_signal( "setting_changed", m_nId );
}

void T3DMeshAsset::setId( const int nNewId ) {
	const int old_id = m_nId;
	SET_IF_DIFF( m_nId, CLAMP( nNewId, 0, TERRAIN3D_MAX_MESH_ASSETS - 1 ) );
	LOG( T3DTerrain::INFO, m_name, ": t3dSetting_t mesh ID: ", m_nId );
	LOG( T3DTerrain::DEBUG, "Emitting id_changed, TYPE_MESH, ", old_id, ", ", nNewId );
	emit_signal( "id_changed", T3DAssets::TYPE_MESH, old_id, nNewId );
}

void T3DMeshAsset::setHighlighted( const bool bHighlighted ) {
	SET_IF_DIFF( m_bHighlighted, bHighlighted );
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": Set mesh ID highlight: ", bHighlighted );
	if ( m_bHighlighted && m_pHighlightMat.is_null() ) {
		Ref<StandardMaterial3D> pMat;
		pMat.instantiate();
		pMat->set_cull_mode( BaseMaterial3D::CULL_DISABLED );
		Color color;
		const real_t fRandomFloat = ( real_t ) ( rand() ) / ( real_t ) ( RAND_MAX );
		color.set_hsv( fRandomFloat, 1.f, 1.f, 1.f );
		pMat->set_albedo( color );
		m_pHighlightMat = pMat;
	}
	LOG( T3DTerrain::DEBUG, "Emitting preview_changed, ID: ", m_nId );
	emit_signal( "preview_changed", m_nId );
}

Color T3DMeshAsset::getHighlightColor( void ) const {
	StandardMaterial3D *pMat = cast_to<StandardMaterial3D>( m_pHighlightMat.ptr() );
	if ( m_bHighlighted && pMat ) {
		return pMat->get_albedo();
	}
	return COLOR_WHITE;
}

void T3DMeshAsset::setEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bEnabled, bEnabled );
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t enabled: ", m_bEnabled );
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void T3DMeshAsset::updateInstanceCount( const int64_t nAmount ) {
	if ( nAmount == 0 ) {
		return;
	}
	const int64_t new_count = ( int64_t ) ( m_nInstanceCount ) + nAmount;
	m_nInstanceCount = ( uint32_t ) ( CLAMP( new_count, ( int64_t ) ( 0 ), ( int64_t ) ( UINT32_MAX ) ) );
	LOG( T3DTerrain::EXTREME, "Emitting instance_count_changed, ID: ", m_nId, ", count: ", m_nInstanceCount );
	emit_signal( "instance_count_changed" );
}

void T3DMeshAsset::setInstanceCount( const uint32_t nAmount ) {
	SET_IF_DIFF( m_nInstanceCount, nAmount );
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t instance_count: ", m_nInstanceCount );
	LOG( T3DTerrain::DEBUG, "Emitting instance_count_changed" );
	emit_signal( "instance_count_changed" );
}

void T3DMeshAsset::setSceneFile( const Ref<PackedScene> &rSceneFile ) {
	SET_IF_DIFF( m_pPackedScene, rSceneFile );
	invalidateThumbnail();
	m_pendingMeshes.clear();
	if ( m_pPackedScene.is_valid() ) {
		Node *pNode = m_pPackedScene->instantiate();
		if ( !pNode ) {
			LOG( T3DTerrain::ERROR, "Drag a non-empty glb, fbx, scn, or tscn file into the scene_file slot" );
			m_pPackedScene.unref();
			return;
		}
		LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": Instantiating scene root node: ", m_pPackedScene->get_path() );
		m_fHeightOffset = 0.0f;
		m_generatedType = TYPE_NONE;
		if ( m_pMaterialOverride.is_valid() && m_pMaterialOverride->has_meta( "terrain3d_generated_material" ) ) {
			m_pMaterialOverride.unref();
		}

		// Look for MeshInstance3D nodes
		LOG( T3DTerrain::DEBUG, "Loaded scene with parent node: ", pNode );
		TypedArray<Node> mesh_instances;

		// First look for XXXXLOD# meshes, sorted by last digit
		mesh_instances = pNode->find_children( "*LOD?", "MeshInstance3D" );
		if ( mesh_instances.size() > 0 ) {
			LOG( T3DTerrain::INFO, "Found ", mesh_instances.size(), " meshes using LOD# naming convention, using the first ",
				TERRAIN3D_MESH_MAX_LOD_COUNT );
			mesh_instances.sort_custom( callable_mp_static( &T3DMeshAsset::sortLodNodes ) );
		}

		// Fallback to using all the meshes in provided order
		if ( mesh_instances.size() == 0 ) {
			mesh_instances = pNode->find_children( "*", "MeshInstance3D" );
			if ( mesh_instances.size() > 0 ) {
				LOG( T3DTerrain::INFO, "No meshes with LOD# suffixes found, using the first ", TERRAIN3D_MESH_MAX_LOD_COUNT,
					" meshes as LOD0-LOD3" );
			}
		}

		// Fallback to the scene root mesh
		if ( mesh_instances.size() == 0 ) {
			if ( pNode->is_class( "MeshInstance3D" ) ) {
				LOG( T3DTerrain::INFO, "No LOD# meshes found, assuming the root mesh is LOD0" );
				mesh_instances.push_back( pNode );
			}
		}
		if ( mesh_instances.size() == 0 ) {
			LOG( T3DTerrain::ERROR, "No MeshInstance3D found in scene file" );
		}

		// Now process the meshes
		for ( int i = 0, count = MIN( mesh_instances.size(), TERRAIN3D_MESH_MAX_LOD_COUNT ); i < count; i++ ) {
			MeshInstance3D *const pMi = cast_to<MeshInstance3D>( mesh_instances[i] );
			LOG( T3DTerrain::DEBUG, "Found mesh: ", pMi->get_name() );
			const String filename = m_pPackedScene->get_path().get_file().get_basename();
			if ( m_name == "New Mesh" && !m_pPackedScene->get_path().contains( "::" ) ) {
				m_name = filename;
				LOG( T3DTerrain::INFO, "t3dSetting_t name based on filename: ", m_name );
			}
			Ref<Mesh> pMesh = pMi->get_mesh();
			if ( pMesh.is_null() ) {
				LOG( T3DTerrain::WARN, "MeshInstance3D ", pMi->get_name(), " has no mesh, skipping" );
				continue;
			}
			// Duplicate the mesh to make each T3DMeshAsset unique
			pMesh = pMesh->duplicate();
			// Apply the active material from the scene to the mesh, including MI or Geom overrides
			for ( int j = 0; j < pMi->get_surface_override_material_count(); j++ ) {
				const Ref<Material> pMat = pMi->get_active_material( j );
				pMesh->surface_set_material( j, pMat );
			}
			m_pendingMeshes.push_back( pMesh );
		}
		pNode->queue_free();
	}
	if ( m_pendingMeshes.size() > 0 ) {
		const Ref<Mesh> pMesh = m_pendingMeshes[0];
		if ( pMesh.is_null() ) {
			LOG( T3DTerrain::ERROR, "First mesh is null after loading scene" );
			return;
		}
		const AABB aabb = pMesh->get_aabb();
		m_fDensity = CLAMP( 10.f / ( aabb.has_volume() ? aabb.get_volume() : 1.f ), 0.01f, 10.0f );
		m_iLastLod = m_pendingMeshes.size() - 1;
		m_iLastShadowLod = m_iLastLod;
		m_shadowImpostor = 0;
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
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void T3DMeshAsset::commitMeshes( void ) {
	LOG( T3DTerrain::INFO, m_name, ": Committing ", m_pendingMeshes.size(), " pending meshes" );
	invalidateThumbnail();
	m_meshes.clear();
	m_meshes = m_pendingMeshes;
	m_pendingMeshes = TypedArray<Mesh>();
}

void T3DMeshAsset::setGeneratedType( const t3dGenType_t type ) {
	const bool bEnteringGeneratedMode = m_generatedType == TYPE_NONE;
	SET_IF_DIFF( m_generatedType, CLAMP( type, ( t3dGenType_t ) ( TYPE_NONE + 1 ), ( t3dGenType_t ) ( TYPE_MAX - 1 ) ) );
	if ( bEnteringGeneratedMode ) {
		m_castShadows = TERRAIN3D_SHADOW_CASTING_OFF;
		m_iLastShadowLod = 0;
		m_shadowImpostor = 0;
	}
	invalidateThumbnail();
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t generated type: ", m_generatedType );
	assignGeneratedMesh();
	m_fDensity = 10.f;
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
	notify_property_list_changed();
}

Ref<Mesh> T3DMeshAsset::getMesh( const int nLod ) const {
	if ( nLod >= 0 && nLod < getMeshes().size() ) {
		return getMeshes()[nLod];
	}
	return Ref<Mesh>();
}

void T3DMeshAsset::setHeightOffset( const real_t fOffset ) {
	SET_IF_DIFF( m_fHeightOffset, CLAMP( fOffset, -50.f, 50.f ) );
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t height offset: ", m_fHeightOffset );
	LOG( T3DTerrain::DEBUG, "Emitting setting_changed, ID: ", m_nId );
	emit_signal( "setting_changed", m_nId );
}

void T3DMeshAsset::setDensity( const real_t fDensity ) {
	SET_IF_DIFF( m_fDensity, CLAMP( fDensity, 0.01f, 10.f ) );
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t mesh density: ", fDensity );
	LOG( T3DTerrain::DEBUG, "Emitting setting_changed, ID: ", m_nId );
	emit_signal( "setting_changed", m_nId );
}

void T3DMeshAsset::setCastShadows( const ShadowCasting castShadows ) {
	SET_IF_DIFF( m_castShadows, castShadows );
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t shadow casting mode: ", m_castShadows );
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

// Returns the approproate cast_shadows setting for the given LOD ID
ShadowCasting T3DMeshAsset::getLodCastShadows( const int nLodId ) const {
	// If cast shadows is off, disable all shadows
	if ( m_castShadows == TERRAIN3D_SHADOW_CASTING_OFF ) {
		return m_castShadows;
	}
	// Return shadows only if set, ensuring shadow impostor and last lod are processed first
	if ( m_castShadows == TERRAIN3D_SHADOW_CASTING_ONLY ) {
		return m_castShadows;
	}
	// Set to shadows only if this lod is the shadow impostor, which is only ever set to shadows only
	if ( nLodId == TERRAIN3D_MESH_SHADOW_LOD_ID ) {
		return TERRAIN3D_SHADOW_CASTING_ONLY;
	}
	// Disable shadows if this lod uses the shadow impostor
	if ( nLodId < m_shadowImpostor ) {
		return TERRAIN3D_SHADOW_CASTING_OFF;
	}
	// Disable shadows if this lod is too far
	if ( nLodId > m_iLastShadowLod ) {
		return TERRAIN3D_SHADOW_CASTING_OFF;
	}
	return m_castShadows;
}

inline void T3DMeshAsset::setVisibilityLayers( const uint32_t nLayers ) {
	SET_IF_DIFF( m_visibilityLayers, nLayers );
	LOG( T3DTerrain::INFO, m_name, ": t3dSetting_t visibility layers: ", nLayers );
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void T3DMeshAsset::setMaterialOverride( const Ref<Material> &rMaterial ) {
	SET_IF_DIFF( m_pMaterialOverride, rMaterial );
	invalidateThumbnail();
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t material override: ", rMaterial );
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void T3DMeshAsset::setMaterialOverlay( const Ref<Material> &rMaterial ) {
	SET_IF_DIFF( m_pMaterialOverlay, rMaterial );
	invalidateThumbnail();
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t material overlay: ", rMaterial );
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void T3DMeshAsset::setGeneratedFaces( const int nCount ) {
	SET_IF_DIFF( m_nGeneratedFaces, CLAMP( nCount, 1, 3 ) );
	invalidateThumbnail();
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t generated face count: ", m_nGeneratedFaces );
	// If already a texture card, just recreate the mesh w/o other settings
	if ( m_generatedType > TYPE_NONE && m_generatedType < TYPE_MAX && getMeshes().size() == 1 ) {
		assignGeneratedMesh();
	}
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void T3DMeshAsset::setGeneratedSize( const Vector2 &rSize ) {
	SET_IF_DIFF( m_generatedSize, rSize );
	invalidateThumbnail();
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t generated size: ", m_generatedSize );
	// If already a texture card, just recreate the mesh w/o other settings
	if ( m_generatedType > TYPE_NONE && m_generatedType < TYPE_MAX && getMeshes().size() == 1 ) {
		assignGeneratedMesh();
	}
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void T3DMeshAsset::setLastLod( const int nLod ) {
	const int max_lod = m_generatedType != TYPE_NONE ? 0 : CLAMP( getMeshes().size(), 1, TERRAIN3D_MESH_MAX_LOD_COUNT ) - 1;
	SET_IF_DIFF( m_iLastLod, CLAMP( nLod, 0, max_lod ) );
	if ( m_iLastShadowLod > m_iLastLod ) {
		m_iLastShadowLod = m_iLastLod;
	}
	if ( m_shadowImpostor > m_iLastLod ) {
		m_shadowImpostor = m_iLastLod;
	}
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t last LOD: ", m_iLastLod );
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
	notify_property_list_changed();
}

void T3DMeshAsset::setLastShadowLod( const int nLod ) {
	SET_IF_DIFF( m_iLastShadowLod, CLAMP( nLod, 0, m_iLastLod ) );
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t last shadow LOD: ", m_iLastShadowLod );
	if ( m_shadowImpostor > m_iLastShadowLod ) {
		m_shadowImpostor = m_iLastShadowLod;
	}
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void T3DMeshAsset::setShadowImpostor( const int nLod ) {
	SET_IF_DIFF( m_shadowImpostor, CLAMP( nLod, 0, MIN( m_iLastLod, m_iLastShadowLod ) ) );
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t shadow imposter LOD: ", m_shadowImpostor );
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

void T3DMeshAsset::setLodRange( const int nLod, const real_t fDistance ) {
	if ( nLod < 0 || nLod >= TERRAIN3D_MESH_MAX_LOD_COUNT ) {
		LOG( T3DTerrain::ERROR, "p_lod out of range. Valid range is 0 - ", TERRAIN3D_MESH_MAX_LOD_COUNT - 1 );
		return;
	}
	SET_IF_DIFF( m_fLodRanges[nLod], CLAMP( ( float ) ( fDistance ), 0.f, 100000.f ) );
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t LOD ", nLod, " visibility range: ", m_fLodRanges[nLod] );
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

real_t T3DMeshAsset::getLodRange( const int nLod ) const {
	if ( nLod < 0 || nLod >= TERRAIN3D_MESH_MAX_LOD_COUNT ) {
		return -1.f;
	}
	return m_fLodRanges[nLod];
}

real_t T3DMeshAsset::getLodRangeBegin( const int nLod ) const {
	if ( nLod <= 0 ) {
		return 0.f;
	}
	ASSERT( nLod <= m_iLastLod, 0.f );
	return m_fLodRanges[nLod - 1];
}

real_t T3DMeshAsset::getLodRangeEnd( const int nLod ) const {
	if ( nLod == TERRAIN3D_MESH_SHADOW_LOD_ID ) {
		return m_fLodRanges[MAX( m_shadowImpostor - 1, 0 )];
	}
	ASSERT( nLod >= 0 && nLod <= m_iLastLod, 0.f );
	return m_fLodRanges[nLod];
}

void T3DMeshAsset::setFadeMargin( const real_t fFadeMargin ) {
	const int max_range = CLAMP( m_fLodRanges[1] - m_fLodRanges[0], 0.f, 64.f );
	SET_IF_DIFF( m_fFadeMargin, CLAMP( fFadeMargin, 0.f, max_range ) );
	LOG( T3DTerrain::INFO, "ID ", m_nId, ", ", m_name, ": t3dSetting_t visibility margin: ", m_fFadeMargin );
	LOG( T3DTerrain::DEBUG, "Emitting instancer_setting_changed, ID: ", m_nId );
	emit_signal( "instancer_setting_changed", m_nId );
}

///////////////////////////
// Protected Functions
///////////////////////////

void T3DMeshAsset::_validate_property( PropertyInfo &rProperty ) const {
	if ( rProperty.name != StringName( "generated_type" ) && rProperty.name.begins_with( "generated_" ) ) {
		if ( m_generatedType == TYPE_NONE ) {
			rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
		} else {
			rProperty.usage = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY;
		}
		return;
	} else if ( rProperty.name.match( "lod?_range" ) ) {
		const int lod = rProperty.name.substr( 3, 1 ).to_int();
		if ( m_iLastLod < lod ) {
			rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
		} else {
			rProperty.usage = PROPERTY_USAGE_DEFAULT;
		}
	}
}

bool T3DMeshAsset::_property_can_revert( const StringName &rName ) const {
	return ( rName.match( "last_lod" ) || rName.match( "last_shadow_lod" ) || rName.match( "density" ) ||
			rName.match( "generated_type" ) ||
			( m_generatedType > TYPE_NONE && ( rName.match( "height_offset" ) || rName.match( "lod0_range" ) ) ) );
}

bool T3DMeshAsset::_property_get_revert( const StringName &rName, Variant &rProperty ) const {
	if ( rName.match( "last_lod" ) ) {
		rProperty = getLodCount() - 1;
		return true;
	} else if ( rName.match( "last_shadow_lod" ) ) {
		rProperty = getLodCount() - 1;
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

void T3DMeshAsset::setHlodContribution( const t3dHlodContribution_t mode ) {
	const t3dHlodContribution_t value = ( t3dHlodContribution_t ) CLAMP( ( int ) mode, ( int ) HLOD_AUTO, ( int ) HLOD_EXCLUDE );
	if ( m_hlodContribution == value ) { return; }
	m_hlodContribution = value;
	emit_signal( "setting_changed" );
	emit_changed();
}

void T3DMeshAsset::setHlodMinLevel( const int nLevel ) {
	const int value = CLAMP( nLevel, 1, 6 );
	if ( m_nHlodMinLevel == value ) { return; }
	m_nHlodMinLevel = value;
	emit_signal( "setting_changed" );
	emit_changed();
}

void T3DMeshAsset::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( HLOD_AUTO );
	BIND_ENUM_CONSTANT( HLOD_INCLUDE );
	BIND_ENUM_CONSTANT( HLOD_EXCLUDE );
	ClassDB::bind_method( D_METHOD( "set_hlod_contribution", "mode" ), &T3DMeshAsset::setHlodContribution );
	ClassDB::bind_method( D_METHOD( "get_hlod_contribution" ), &T3DMeshAsset::getHlodContribution );
	ClassDB::bind_method( D_METHOD( "set_hlod_min_level", "level" ), &T3DMeshAsset::setHlodMinLevel );
	ClassDB::bind_method( D_METHOD( "get_hlod_min_level" ), &T3DMeshAsset::getHlodMinLevel );
	ClassDB::bind_method( D_METHOD( "is_hlod_eligible" ), &T3DMeshAsset::isHlodEligible );
	BIND_ENUM_CONSTANT( TYPE_NONE );
	BIND_ENUM_CONSTANT( TYPE_TEXTURE_CARD );
	BIND_ENUM_CONSTANT( TYPE_MAX );

	ADD_SIGNAL( MethodInfo( "id_changed" ) );
	ADD_SIGNAL( MethodInfo( "setting_changed" ) );
	ADD_SIGNAL( MethodInfo( "instancer_setting_changed" ) );
	ADD_SIGNAL( MethodInfo( "preview_changed" ) );
	ADD_SIGNAL( MethodInfo( "instance_count_changed" ) );

	ClassDB::bind_method( D_METHOD( "clear" ), &T3DMeshAsset::clear );
	ClassDB::bind_method( D_METHOD( "set_name", "name" ), &T3DMeshAsset::setName );
	ClassDB::bind_method( D_METHOD( "get_name" ), &T3DMeshAsset::getName );
	ClassDB::bind_method( D_METHOD( "set_id", "id" ), &T3DMeshAsset::setId );
	ClassDB::bind_method( D_METHOD( "get_id" ), &T3DMeshAsset::getId );
	ClassDB::bind_method( D_METHOD( "set_highlighted", "enabled" ), &T3DMeshAsset::setHighlighted );
	ClassDB::bind_method( D_METHOD( "is_highlighted" ), &T3DMeshAsset::isHighlighted );
	ClassDB::bind_method( D_METHOD( "get_highlight_color" ), &T3DMeshAsset::getHighlightColor );
	ClassDB::bind_method( D_METHOD( "get_thumbnail" ), &T3DMeshAsset::getThumbnail );

	ClassDB::bind_method( D_METHOD( "set_enabled", "enabled" ), &T3DMeshAsset::setEnabled );
	ClassDB::bind_method( D_METHOD( "is_enabled" ), &T3DMeshAsset::isEnabled );

	ClassDB::bind_method( D_METHOD( "set_scene_file", "scene_file" ), &T3DMeshAsset::setSceneFile );
	ClassDB::bind_method( D_METHOD( "get_scene_file" ), &T3DMeshAsset::getSceneFile );
	ClassDB::bind_method( D_METHOD( "set_generated_type", "type" ), &T3DMeshAsset::setGeneratedType );
	ClassDB::bind_method( D_METHOD( "get_generated_type" ), &T3DMeshAsset::getGeneratedType );
	ClassDB::bind_method( D_METHOD( "get_mesh", "lod" ), &T3DMeshAsset::getMesh, DEFVAL( 0 ) );
	ClassDB::bind_method( D_METHOD( "set_height_offset", "offset" ), &T3DMeshAsset::setHeightOffset );
	ClassDB::bind_method( D_METHOD( "get_height_offset" ), &T3DMeshAsset::getHeightOffset );
	ClassDB::bind_method( D_METHOD( "set_density", "density" ), &T3DMeshAsset::setDensity );
	ClassDB::bind_method( D_METHOD( "get_density" ), &T3DMeshAsset::getDensity );
	ClassDB::bind_method( D_METHOD( "set_cast_shadows", "mode" ), &T3DMeshAsset::setCastShadows );
	ClassDB::bind_method( D_METHOD( "get_cast_shadows" ), &T3DMeshAsset::getCastShadows );
	ClassDB::bind_method( D_METHOD( "set_visibility_layers", "layers" ), &T3DMeshAsset::setVisibilityLayers );
	ClassDB::bind_method( D_METHOD( "get_visibility_layers" ), &T3DMeshAsset::getVisibilityLayers );
	ClassDB::bind_method( D_METHOD( "set_material_override", "material" ), &T3DMeshAsset::setMaterialOverride );
	ClassDB::bind_method( D_METHOD( "get_material_override" ), &T3DMeshAsset::getMaterialOverride );
	ClassDB::bind_method( D_METHOD( "set_material_overlay", "material" ), &T3DMeshAsset::setMaterialOverlay );
	ClassDB::bind_method( D_METHOD( "get_material_overlay" ), &T3DMeshAsset::getMaterialOverlay );

	ClassDB::bind_method( D_METHOD( "set_generated_faces", "count" ), &T3DMeshAsset::setGeneratedFaces );
	ClassDB::bind_method( D_METHOD( "get_generated_faces" ), &T3DMeshAsset::getGeneratedFaces );
	ClassDB::bind_method( D_METHOD( "set_generated_size", "size" ), &T3DMeshAsset::setGeneratedSize );
	ClassDB::bind_method( D_METHOD( "get_generated_size" ), &T3DMeshAsset::getGeneratedSize );

	ClassDB::bind_method( D_METHOD( "get_lod_count" ), &T3DMeshAsset::getLodCount );
	ClassDB::bind_method( D_METHOD( "set_last_lod", "lod" ), &T3DMeshAsset::setLastLod );
	ClassDB::bind_method( D_METHOD( "get_last_lod" ), &T3DMeshAsset::getLastLod );
	ClassDB::bind_method( D_METHOD( "set_last_shadow_lod", "lod" ), &T3DMeshAsset::setLastShadowLod );
	ClassDB::bind_method( D_METHOD( "get_last_shadow_lod" ), &T3DMeshAsset::getLastShadowLod );
	ClassDB::bind_method( D_METHOD( "set_shadow_impostor", "lod" ), &T3DMeshAsset::setShadowImpostor );
	ClassDB::bind_method( D_METHOD( "get_shadow_impostor" ), &T3DMeshAsset::getShadowImpostor );

	ClassDB::bind_method( D_METHOD( "set_lod_range", "lod", "distance" ), &T3DMeshAsset::setLodRange );
	ClassDB::bind_method( D_METHOD( "get_lod_range", "lod" ), &T3DMeshAsset::getLodRange );
	ClassDB::bind_method( D_METHOD( "set_lod0_range", "distance" ), &T3DMeshAsset::setLod0Range );
	ClassDB::bind_method( D_METHOD( "get_lod0_range" ), &T3DMeshAsset::getLod0Range );
	ClassDB::bind_method( D_METHOD( "set_lod1_range", "distance" ), &T3DMeshAsset::setLod1Range );
	ClassDB::bind_method( D_METHOD( "get_lod1_range" ), &T3DMeshAsset::getLod1Range );
	ClassDB::bind_method( D_METHOD( "set_lod2_range", "distance" ), &T3DMeshAsset::setLod2Range );
	ClassDB::bind_method( D_METHOD( "get_lod2_range" ), &T3DMeshAsset::getLod2Range );
	ClassDB::bind_method( D_METHOD( "set_lod3_range", "distance" ), &T3DMeshAsset::setLod3Range );
	ClassDB::bind_method( D_METHOD( "get_lod3_range" ), &T3DMeshAsset::getLod3Range );
	ClassDB::bind_method( D_METHOD( "set_lod4_range", "distance" ), &T3DMeshAsset::setLod4Range );
	ClassDB::bind_method( D_METHOD( "get_lod4_range" ), &T3DMeshAsset::getLod4Range );
	ClassDB::bind_method( D_METHOD( "set_lod5_range", "distance" ), &T3DMeshAsset::setLod5Range );
	ClassDB::bind_method( D_METHOD( "get_lod5_range" ), &T3DMeshAsset::getLod5Range );
	ClassDB::bind_method( D_METHOD( "set_lod6_range", "distance" ), &T3DMeshAsset::setLod6Range );
	ClassDB::bind_method( D_METHOD( "get_lod6_range" ), &T3DMeshAsset::getLod6Range );
	ClassDB::bind_method( D_METHOD( "set_lod7_range", "distance" ), &T3DMeshAsset::setLod7Range );
	ClassDB::bind_method( D_METHOD( "get_lod7_range" ), &T3DMeshAsset::getLod7Range );
	ClassDB::bind_method( D_METHOD( "set_lod8_range", "distance" ), &T3DMeshAsset::setLod8Range );
	ClassDB::bind_method( D_METHOD( "get_lod8_range" ), &T3DMeshAsset::getLod8Range );
	ClassDB::bind_method( D_METHOD( "set_lod9_range", "distance" ), &T3DMeshAsset::setLod9Range );
	ClassDB::bind_method( D_METHOD( "get_lod9_range" ), &T3DMeshAsset::getLod9Range );
	ClassDB::bind_method( D_METHOD( "set_fade_margin", "distance" ), &T3DMeshAsset::setFadeMargin );
	ClassDB::bind_method( D_METHOD( "get_fade_margin" ), &T3DMeshAsset::getFadeMargin );

	ClassDB::bind_method( D_METHOD( "get_instance_count" ), &T3DMeshAsset::getInstanceCount );

	ADD_GROUP( "HLOD", "hlod_" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_contribution", PROPERTY_HINT_ENUM, "Auto,Include,Exclude" ), "set_hlod_contribution", "get_hlod_contribution" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_min_level", PROPERTY_HINT_RANGE, "1,6,1" ), "set_hlod_min_level", "get_hlod_min_level" );

	ADD_GROUP( "General", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "name", PROPERTY_HINT_NONE ), "set_name", "get_name" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "id", PROPERTY_HINT_NONE ), "set_id", "get_id" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "enabled", PROPERTY_HINT_NONE ), "set_enabled", "is_enabled" );

	ADD_GROUP( "Source / Placement", "" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "scene_file", PROPERTY_HINT_RESOURCE_TYPE, "PackedScene" ),
				 "set_scene_file", "get_scene_file" );
	ADD_PROPERTY(
		PropertyInfo( Variant::INT, "generated_type", PROPERTY_HINT_ENUM, "None,Texture Card", PROPERTY_USAGE_STORAGE ),
		"set_generated_type", "get_generated_type" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "height_offset", PROPERTY_HINT_RANGE, "-20.0,20.0,.005" ),
				 "set_height_offset", "get_height_offset" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "density", PROPERTY_HINT_RANGE, ".01,10.0,.005" ), "set_density",
				 "get_density" );

	ADD_GROUP( "Rendering", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cast_shadows", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only" ),
				 "set_cast_shadows", "get_cast_shadows" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "visibility_layers", PROPERTY_HINT_LAYERS_3D_RENDER ),
				 "set_visibility_layers", "get_visibility_layers" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "material_override", PROPERTY_HINT_RESOURCE_TYPE,
							  "BaseMaterial3D,ShaderMaterial" ),
				 "set_material_override", "get_material_override" );
	ADD_PROPERTY(
		PropertyInfo( Variant::OBJECT, "material_overlay", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial" ),
		"set_material_overlay", "get_material_overlay" );

	ADD_GROUP( "Generated Mesh", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "generated_faces", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY ), "set_generated_faces",
				 "get_generated_faces" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2, "generated_size", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY ), "set_generated_size",
				 "get_generated_size" );

	ADD_GROUP( "LODs", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "lod_count", PROPERTY_HINT_NONE, "",
							  PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				 "", "get_lod_count" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "last_lod", PROPERTY_HINT_NONE ), "set_last_lod", "get_last_lod" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "last_shadow_lod", PROPERTY_HINT_NONE ), "set_last_shadow_lod",
				 "get_last_shadow_lod" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "shadow_impostor", PROPERTY_HINT_NONE ), "set_shadow_impostor",
				 "get_shadow_impostor" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod0_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
				 "set_lod0_range", "get_lod0_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod1_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
				 "set_lod1_range", "get_lod1_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod2_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
				 "set_lod2_range", "get_lod2_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod3_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
				 "set_lod3_range", "get_lod3_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod4_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
				 "set_lod4_range", "get_lod4_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod5_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
				 "set_lod5_range", "get_lod5_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod6_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
				 "set_lod6_range", "get_lod6_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod7_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
				 "set_lod7_range", "get_lod7_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod8_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
				 "set_lod8_range", "get_lod8_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lod9_range", PROPERTY_HINT_RANGE, "0.,4096.0,.05,or_greater" ),
				 "set_lod9_range", "get_lod9_range" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "fade_margin", PROPERTY_HINT_RANGE, "0.,64.0,.05,or_greater" ),
				 "set_fade_margin", "get_fade_margin" );

}
