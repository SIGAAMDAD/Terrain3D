// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_MESH_ASSET_CLASS_H
#define TERRAIN3D_MESH_ASSET_CLASS_H

#include "eastl_config.h"
#include <EASTL/array.h>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include "Terrain3DConstants.h"
#include "assets/terrain_3d_asset_resource.h"

using ShadowCasting = RenderingServer::ShadowCastingSetting;

/* This class requires a bit of special care because:
 * - We want custom defaults depending on if it's a texture card or a scene file
 * - Any settings saved in the assets resource file need to override the defaults
 * - generated_type = TEXTURE_CARD is the default and isn't saved in the scene file
 *
 * The caveat of this is a texture card MeshAsset needs to determine if it is
 * new, and should apply defaults, or loaded, and should retain settings.
 * The specific defaults of concern are height_offset, density, lod0_range.
 *
 * The current solution to make the distinction is,
 * Loaded Assets go through Terrain3DAssets::set_mesh_list(), _set_asset_list()
 * New Assets go through Terrain3DAssets::set_mesh_asset(), _set_asset()
 * Both call initialize() with a new/loaded flag.
 *
 * New assets might be loaded from the API, AssetDock, or Terrain3DAssets::update_mesh_list()
 * when the mesh list is empty.
 */


class Terrain3DMeshAsset : public T3DAssetResource {
	GDCLASS( Terrain3DMeshAsset, T3DAssetResource );
	CLASS_NAME();

public:
	typedef enum t3dhlodContribution_e {
		HLOD_AUTO = 0,
		HLOD_INCLUDE = 1,
		HLOD_EXCLUDE = 2,
	} t3dhlodContribution_t;
	typedef enum t3dgenType_e {
		TYPE_NONE,
		TYPE_TEXTURE_CARD,
		TYPE_MAX,
	} t3dgenType_t;
private:
	// Saved data
	bool m_bEnabled = true;
	Ref<PackedScene> m_pPackedScene;
	t3dgenType_t m_generatedType = TYPE_NONE;
	t3dhlodContribution_t m_hlodContribution = HLOD_AUTO;
	int m_nHlodMinLevel = 1;
	int m_nGeneratedFaces = 2;
	Vector2 m_generatedSize = V2( 1.f );
	real_t m_fHeightOffset = 0.f;
	real_t m_fDensity = 0.f;
	ShadowCasting m_castShadows = TERRAIN3D_SHADOW_CASTING_ON;
	uint32_t m_visibilityLayers = 1;
	Ref<Material> m_pMaterialOverride;
	Ref<Material> m_pMaterialOverlay;
	int m_iLastLod = TERRAIN3D_MESH_MAX_LOD_COUNT - 1;
	int m_iLastShadowLod = TERRAIN3D_MESH_MAX_LOD_COUNT - 1;
	int m_shadowImpostor = 0;
	eastl::array<float, TERRAIN3D_MESH_MAX_LOD_COUNT> m_fLodRanges{};
	real_t m_fFadeMargin = 0.f;

	// Working data
	Ref<Material> m_pHighlightMat;
	TypedArray<Mesh> m_meshes;
	TypedArray<Mesh> m_pendingMeshes; // Queue to avoid warnings from RS on mesh swap
	uint32_t m_nInstanceCount = 0;

	void clearLodRanges( void );
	static bool sortLodNodes( const Node *pA, const Node *pB );
	Ref<ArrayMesh> createGeneratedMesh( const t3dgenType_t type = TYPE_TEXTURE_CARD ) const;
	void assignGeneratedMesh( void );
	void invalidateThumbnail( void ) {
		m_pThumbnail.unref();
	}
	Ref<Material> getMaterial( void );
	TypedArray<Mesh> getMeshes( void ) const;

public:
	Terrain3DMeshAsset( void ) {
		clear();
	}
	~Terrain3DMeshAsset( void ) {}
	void initialize( void ) override;
	void clear( void ) override;

	void setName( const String &rName ) override;
	String getName( void ) const override {
		return m_name;
	}
	void setId( const int nNewId ) override;
	int getId( void ) const override {
		return m_nId;
	}
	void setHighlighted( const bool bHighlighted ) override;
	bool isHighlighted( void ) const override {
		return m_bHighlighted;
	}
	Ref<Material> getHighlightMaterial( void ) const {
		return m_bHighlighted ? m_pHighlightMat : Ref<Material>();
	}
	Color getHighlightColor( void ) const override;
	Ref<Texture2D> getThumbnail( void ) const override {
		return m_pThumbnail;
	}

	void setEnabled( const bool bEnabled );
	bool isEnabled( void ) const {
		return m_bEnabled;
	}

	void updateInstanceCount( const int64_t nAmount );
	void setInstanceCount( const uint32_t nAmount );
	uint32_t getInstanceCount( void ) const {
		return m_nInstanceCount;
	}

	void setSceneFile( const Ref<PackedScene> &rSceneFile );
	Ref<PackedScene> getSceneFile( void ) const {
		return m_pPackedScene;
	}
	bool isSceneFilePending( void ) const {
		return m_pendingMeshes.size() > 0;
	}
	void commitMeshes( void );
	void setHlodContribution( t3dhlodContribution_t mode );
	t3dhlodContribution_t getHlodContribution( void ) const { return m_hlodContribution; }
	void setHlodMinLevel( int nLevel );
	int getHlodMinLevel( void ) const { return m_nHlodMinLevel; }
	bool isHlodEligible( void ) const {
		if ( m_hlodContribution == HLOD_EXCLUDE ) { return false; }
		return m_hlodContribution == HLOD_INCLUDE || m_generatedType == TYPE_NONE;
	}

	void setGeneratedType( const t3dgenType_t type );
	t3dgenType_t getGeneratedType( void ) const {
		return m_generatedType;
	}
	Ref<Mesh> getMesh( const int nLod = 0 ) const;
	void setThumbnail( const Ref<Texture2D> tex ) {
		m_pThumbnail = tex;
	}
	void setHeightOffset( const real_t fOffset );
	real_t getHeightOffset( void ) const {
		return m_fHeightOffset;
	}
	void setDensity( const real_t fDensity );
	real_t getDensity( void ) const {
		return m_fDensity;
	}
	void setCastShadows( const ShadowCasting castShadows );
	ShadowCasting getCastShadows( void ) const {
		return m_castShadows;
	}
	ShadowCasting getLodCastShadows( const int nLodId ) const;
	void setVisibilityLayers( const uint32_t nLayers );
	uint32_t getVisibilityLayers( void ) const {
		return m_visibilityLayers;
	}
	void setMaterialOverride( const Ref<Material> &rMaterial );
	Ref<Material> getMaterialOverride( void ) const {
		return m_pMaterialOverride;
	}
	void setMaterialOverlay( const Ref<Material> &rMaterial );
	Ref<Material> getMaterialOverlay( void ) const {
		return m_pMaterialOverlay;
	}

	void setGeneratedFaces( const int nCount );
	int getGeneratedFaces( void ) const {
		return m_nGeneratedFaces;
	}
	void setGeneratedSize( const Vector2 &rSize );
	Vector2 getGeneratedSize( void ) const {
		return m_generatedSize;
	}

	int getLodCount( void ) const {
		return getMeshes().size();
	}
	void setLastLod( const int nLod );
	int getLastLod( void ) const {
		return m_iLastLod;
	}
	void setLastShadowLod( const int nLod );
	int getLastShadowLod( void ) const {
		return m_iLastShadowLod;
	}
	void setShadowImpostor( const int nLod );
	int getShadowImpostor( void ) const {
		return m_shadowImpostor;
	}

	void setLodRange( const int nLod, const real_t fDistance );
	real_t getLodRange( const int nLod ) const;
	real_t getLodRangeBegin( const int nLod ) const;
	real_t getLodRangeEnd( const int nLod ) const;
	void setLod0Range( const real_t fDistance ) {
		setLodRange( 0, fDistance );
	}
	real_t getLod0Range( void ) const {
		return m_fLodRanges[0];
	}
	void setLod1Range( const real_t fDistance ) {
		setLodRange( 1, fDistance );
	}
	real_t getLod1Range( void ) const {
		return m_fLodRanges[1];
	}
	void setLod2Range( const real_t fDistance ) {
		setLodRange( 2, fDistance );
	}
	real_t getLod2Range( void ) const {
		return m_fLodRanges[2];
	}
	void setLod3Range( const real_t fDistance ) {
		setLodRange( 3, fDistance );
	}
	real_t getLod3Range( void ) const {
		return m_fLodRanges[3];
	}
	void setLod4Range( const real_t fDistance ) {
		setLodRange( 4, fDistance );
	}
	real_t getLod4Range( void ) const {
		return m_fLodRanges[4];
	}
	void setLod5Range( const real_t fDistance ) {
		setLodRange( 5, fDistance );
	}
	real_t getLod5Range( void ) const {
		return m_fLodRanges[5];
	}
	void setLod6Range( const real_t fDistance ) {
		setLodRange( 6, fDistance );
	}
	real_t getLod6Range( void ) const {
		return m_fLodRanges[6];
	}
	void setLod7Range( const real_t fDistance ) {
		setLodRange( 7, fDistance );
	}
	real_t getLod7Range( void ) const {
		return m_fLodRanges[7];
	}
	void setLod8Range( const real_t fDistance ) {
		setLodRange( 8, fDistance );
	}
	real_t getLod8Range( void ) const {
		return m_fLodRanges[8];
	}
	void setLod9Range( const real_t fDistance ) {
		setLodRange( 9, fDistance );
	}
	real_t getLod9Range( void ) const {
		return m_fLodRanges[9];
	}
	void setFadeMargin( const real_t fFadeMargin );
	real_t getFadeMargin( void ) const {
		return m_fFadeMargin;
	}

protected:
	void _validate_property( PropertyInfo &rProperty ) const;
	bool _property_can_revert( const StringName &rName ) const;
	bool _property_get_revert( const StringName &rName, Variant &rProperty ) const;
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( Terrain3DMeshAsset::t3dgenType_t );
VARIANT_ENUM_CAST( Terrain3DMeshAsset::t3dhlodContribution_t );

inline TypedArray<Mesh> Terrain3DMeshAsset::getMeshes( void ) const {
	if ( !m_pendingMeshes.is_empty() ) {
		return m_pendingMeshes;
	}
	return m_meshes;
}
#endif // TERRAIN3D_MESH_ASSET_CLASS_H
