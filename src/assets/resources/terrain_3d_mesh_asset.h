// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_MESH_ASSET_CLASS_H
#define TERRAIN3D_MESH_ASSET_CLASS_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "assets/resources/terrain_3d_asset_resource.h"
#include "assets/resources/terrain_3d_rendering_material.h"
#include "eastl_config.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/array_occluder3d.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/aabb.hpp>

typedef GeometryInstance3D::ShadowCastingSetting ShadowCasting;

/* Mesh assets need different defaults for texture cards and scene meshes.
 * Serialized asset settings override those defaults, while newly created assets
 * receive defaults during initialize(). Loaded and newly created assets therefore
 * enter initialization through distinct Terrain3DAssets paths.
 */

// t3dInstanceCategory_e centralized in Terrain3DTypes.h.


// t3dTerrainAlignment_e centralized in Terrain3DTypes.h.


// t3dHLODContribution_e centralized in Terrain3DTypes.h.


// t3dInstanceOcclusionPolicy_e centralized in Terrain3DTypes.h.


// t3dGenType_e centralized in Terrain3DTypes.h.


class Terrain3DMeshAsset : public Terrain3DAssetResource {
	GDCLASS( Terrain3DMeshAsset, Terrain3DAssetResource );
	CLASS_NAME();

public:
private:
	// Saved data
	bool m_bEnabled = true;
	Ref<PackedScene> m_packedScene;
	t3dGenType_t m_generatedType = TYPE_NONE;
	t3dInstanceCategory_t m_instanceCategory = INSTANCE_CATEGORY_GROUND_COVER;
	t3dTerrainAlignment_t m_terrainAlignment = TERRAIN_ALIGNMENT_UPRIGHT;
	t3dHLODContribution_t m_hlodContribution = HLOD_AUTO;
	int m_nHlodMinLevel = 1;
	t3dInstanceOcclusionPolicy_t m_instanceOcclusionPolicy = INSTANCE_OCCLUSION_NONE;
	AABB m_instanceOcclusionBox = AABB( Vector3( -0.5f, 0.0f, -0.5f ), Vector3( 1.0f, 1.0f, 1.0f ) );
	Ref<ArrayMesh> m_instanceOcclusionProxyMesh;
	Ref<ArrayOccluder3D> m_instanceOccluder;
	Ref<ConcavePolygonShape3D> m_instanceCollisionShape;
	uint32_t m_nInstanceCollisionVertexCount = 0u;
	uint32_t m_nInstanceCollisionTriangleCount = 0u;
	uint32_t m_nInstanceCollisionGeneration = 1u;
	uint32_t m_nInstanceOcclusionProxyVertexCount = 0u;
	uint32_t m_nInstanceOcclusionProxyTriangleCount = 0u;
	uint32_t m_nInstanceOcclusionProxyGeneration = 1u;
	bool m_bInstanceCollisionDerived = false;
	bool m_bInstanceOcclusionProxyDerived = false;
	bool m_bInstanceProxyImportedOccluder = false;
	real_t m_fInstanceOcclusionRange = 256.0f;
	int m_nGeneratedFaces = 2;
	Vector2 m_generatedSize = V2( 1.f );
	real_t m_fHeightOffset = 0.f;
	real_t m_fDensity = 0.f;
	ShadowCasting m_castShadows = s_shadowCastingOn;
	uint32_t m_nVisibilityLayers = 1;
	Ref<Material> m_materialOverride; // Legacy import/migration source only.
	Ref<Material> m_materialOverlay;  // Legacy import/migration source only.
	Ref<Terrain3DRenderingMaterial> m_renderingMaterialOverride;
	Ref<Terrain3DRenderingMaterial> m_renderingMaterialOverlay;
	TypedArray<Terrain3DRenderingMaterial> m_surfaceRenderingMaterials;
	int m_iLastLod = T3D_MESH_MAX_LOD_COUNT - 1;
	int m_iLastShadowLod = T3D_MESH_MAX_LOD_COUNT - 1;
	int m_nShadowImpostor = 0;
	float m_fLodRanges[T3D_MESH_MAX_LOD_COUNT]{};
	real_t m_fFadeMargin = 0.f;

	// Working data
	Ref<Material> m_highlightMat;
	TypedArray<Mesh> m_meshes;
	TypedArray<Mesh> m_pendingMeshes; // Queue to avoid renderer warnings on mesh swap
	TypedArray<Terrain3DRenderingMaterial> m_pendingSurfaceRenderingMaterials;
	PackedVector3Array m_pendingInstanceOccluderVertices;
	PackedInt32Array m_pendingInstanceOccluderIndices;
	bool m_bPendingImportedOccluder = false;
	uint32_t m_nInstanceCount = 0;

	void clearLodRanges( void );
	static bool sortLodNodes( const Node *pA, const Node *pB );
	Ref<ArrayMesh> createGeneratedMesh( const t3dGenType_t type = TYPE_TEXTURE_CARD ) const;
	void assignGeneratedMesh( void );
	EA_FORCE_INLINE void invalidateThumbnail( void ) {
		m_thumbnail.unref();
	}
	Ref<Material> getMaterial( void );
	TypedArray<Mesh> getMeshes( void ) const;
	void clearInstanceCollisionGeometry( void );
	void clearInstanceOcclusionGeometry( void );
	void clearInstanceDerivedGeometry( void );
	bool rebuildInstanceCollisionGeometry( void );
	bool rebuildInstanceOcclusionGeometry( void );
	bool rebuildInstanceDerivedGeometry( void );
	void onRenderingMaterialChanged( void );
	void disconnectRenderingMaterialSignals( void );
	void connectRenderingMaterialSignals( void );
	bool capturePendingSurfaceRenderingMaterialsCold( void );
	String getVtMaterialOwnerIdentityCold( void ) const;

public:
	EA_FORCE_INLINE Terrain3DMeshAsset( void ) {
		clear();
	}
	EA_FORCE_INLINE ~Terrain3DMeshAsset( void ) {
	}
	void initialize( void ) override;
	void clear( void ) override;

	void setName( const String &rName ) override;
	EA_FORCE_INLINE String getName( void ) const override {
		return m_name;
	}
	void setId( const int nNewId ) override;
	EA_FORCE_INLINE int getId( void ) const override {
		return m_nId;
	}
	void setHighlighted( const bool bHighlighted ) override;
	EA_FORCE_INLINE bool isHighlighted( void ) const override {
		return m_bHighlighted;
	}
	EA_FORCE_INLINE Ref<Material> getHighlightMaterial( void ) const {
		return m_bHighlighted ? m_highlightMat : Ref<Material>();
	}
	Color getHighlightColor( void ) const override;
	EA_FORCE_INLINE Ref<Texture2D> getThumbnail( void ) const override {
		return m_thumbnail;
	}

	void setEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool isEnabled( void ) const {
		return m_bEnabled;
	}

	void updateInstanceCount( const int64_t nAmount );
	void setInstanceCount( const uint32_t nAmount );
	EA_FORCE_INLINE uint32_t getInstanceCount( void ) const {
		return m_nInstanceCount;
	}

	void setSceneFile( const Ref<PackedScene> &rSceneFile );
	EA_FORCE_INLINE Ref<PackedScene> getSceneFile( void ) const {
		return m_packedScene;
	}
	EA_FORCE_INLINE bool isSceneFilePending( void ) const {
		return m_pendingMeshes.size() > 0;
	}
	void commitMeshes( void );
	void setInstanceCategory( t3dInstanceCategory_t eCategory );
	EA_FORCE_INLINE t3dInstanceCategory_t getInstanceCategory( void ) const {
		return m_instanceCategory;
	}
	EA_FORCE_INLINE int getCategoryMaximumLodCount( void ) const {
		return m_instanceCategory == INSTANCE_CATEGORY_CLUTTER ? 2 : T3D_MESH_MAX_LOD_COUNT;
	}
	EA_FORCE_INLINE bool isIndividuallyEditable( void ) const {
		return m_instanceCategory != INSTANCE_CATEGORY_CLUTTER;
	}
	EA_FORCE_INLINE bool requiresInstancePhysics( void ) const {
		return m_instanceCategory == INSTANCE_CATEGORY_GROUND_COVER || m_instanceCategory == INSTANCE_CATEGORY_OCCLUDER;
	}
	EA_FORCE_INLINE bool usesInstanceOcclusion( void ) const {
		return m_instanceCategory == INSTANCE_CATEGORY_OCCLUDER && m_instanceOcclusionPolicy == INSTANCE_OCCLUSION_EXPLICIT_MESH;
	}
	EA_FORCE_INLINE bool usesFullInstanceOcclusion( void ) const {
		return m_instanceCategory == INSTANCE_CATEGORY_OCCLUDER && m_instanceOcclusionPolicy == INSTANCE_OCCLUSION_EXPLICIT_MESH;
	}
	EA_FORCE_INLINE bool hasInstanceCollisionProxy( void ) const {
		return m_bInstanceCollisionDerived && m_instanceCollisionShape.is_valid();
	}
	EA_FORCE_INLINE bool hasInstanceOcclusionProxy( void ) const {
		return m_bInstanceOcclusionProxyDerived && m_instanceOcclusionProxyMesh.is_valid() && m_instanceOccluder.is_valid();
	}
	// Compatibility query retained for callers that still require both products.
	EA_FORCE_INLINE bool hasMeshDerivedInstanceProxy( void ) const {
		return hasInstanceCollisionProxy() && hasInstanceOcclusionProxy();
	}
	EA_FORCE_INLINE bool isInstanceProxyImportedOccluder( void ) const {
		return m_bInstanceProxyImportedOccluder;
	}
	void setInstanceOcclusionPolicy( t3dInstanceOcclusionPolicy_t ePolicy );
	EA_FORCE_INLINE t3dInstanceOcclusionPolicy_t getInstanceOcclusionPolicy( void ) const {
		return m_instanceOcclusionPolicy;
	}
	void setInstanceOcclusionBox( const AABB &rBox );
	EA_FORCE_INLINE AABB getInstanceOcclusionBox( void ) const {
		return m_instanceOcclusionBox;
	}
	void setInstanceOcclusionProxyMesh( const Ref<ArrayMesh> &rMesh );
	EA_FORCE_INLINE Ref<ArrayMesh> getInstanceOcclusionProxyMesh( void ) const {
		return m_instanceOcclusionProxyMesh;
	}
	EA_FORCE_INLINE Ref<ArrayOccluder3D> getInstanceOccluder( void ) const {
		return m_instanceOccluder;
	}
	EA_FORCE_INLINE Ref<ConcavePolygonShape3D> getInstanceCollisionShape( void ) const {
		return m_instanceCollisionShape;
	}
	EA_FORCE_INLINE uint32_t getInstanceCollisionVertexCount( void ) const {
		return m_nInstanceCollisionVertexCount;
	}
	EA_FORCE_INLINE uint32_t getInstanceCollisionTriangleCount( void ) const {
		return m_nInstanceCollisionTriangleCount;
	}
	EA_FORCE_INLINE uint32_t getInstanceCollisionGeneration( void ) const {
		return m_nInstanceCollisionGeneration;
	}
	EA_FORCE_INLINE uint32_t getInstanceOcclusionProxyVertexCount( void ) const {
		return m_nInstanceOcclusionProxyVertexCount;
	}
	EA_FORCE_INLINE uint32_t getInstanceOcclusionProxyTriangleCount( void ) const {
		return m_nInstanceOcclusionProxyTriangleCount;
	}
	EA_FORCE_INLINE uint32_t getInstanceOcclusionProxyGeneration( void ) const {
		return m_nInstanceOcclusionProxyGeneration;
	}
	// Compatibility aliases: historical proxy statistics describe occlusion geometry.
	EA_FORCE_INLINE uint32_t getInstanceProxyVertexCount( void ) const {
		return getInstanceOcclusionProxyVertexCount();
	}
	EA_FORCE_INLINE uint32_t getInstanceProxyTriangleCount( void ) const {
		return getInstanceOcclusionProxyTriangleCount();
	}
	EA_FORCE_INLINE uint32_t getInstanceProxyGeneration( void ) const {
		return getInstanceOcclusionProxyGeneration();
	}
	void setInstanceOcclusionRange( real_t fRange );
	EA_FORCE_INLINE real_t getInstanceOcclusionRange( void ) const {
		return m_fInstanceOcclusionRange;
	}
	void setTerrainAlignment( t3dTerrainAlignment_t eAlignment );
	EA_FORCE_INLINE t3dTerrainAlignment_t getTerrainAlignment( void ) const {
		return m_terrainAlignment;
	}
	EA_FORCE_INLINE bool alignsToTerrainNormal( void ) const {
		return m_terrainAlignment == TERRAIN_ALIGNMENT_NORMAL;
	}
	void setHlodContribution( t3dHLODContribution_t mode );
	EA_FORCE_INLINE t3dHLODContribution_t getHlodContribution( void ) const {
		return m_hlodContribution;
	}
	void setHlodMinLevel( int nLevel );
	EA_FORCE_INLINE int getHlodMinLevel( void ) const {
		return m_nHlodMinLevel;
	}
	EA_FORCE_INLINE bool isHlodEligible( void ) const {
		return m_instanceCategory != INSTANCE_CATEGORY_CLUTTER;
	}

	void setGeneratedType( const t3dGenType_t type );
	EA_FORCE_INLINE t3dGenType_t getGeneratedType( void ) const {
		return m_generatedType;
	}
	Ref<Mesh> getMesh( const int nLod = 0 ) const;
	EA_FORCE_INLINE void setThumbnail( const Ref<Texture2D> tex ) {
		m_thumbnail = tex;
	}
	void setHeightOffset( const real_t fOffset );
	EA_FORCE_INLINE real_t getHeightOffset( void ) const {
		return m_fHeightOffset;
	}
	void setDensity( const real_t fDensity );
	EA_FORCE_INLINE real_t getDensity( void ) const {
		return m_fDensity;
	}
	void setCastShadows( const ShadowCasting castShadows );
	EA_FORCE_INLINE ShadowCasting getCastShadows( void ) const {
		return m_castShadows;
	}
	ShadowCasting getLodCastShadows( const int nLodId ) const;
	void setVisibilityLayers( const uint32_t nLayers );
	EA_FORCE_INLINE uint32_t getVisibilityLayers( void ) const {
		return m_nVisibilityLayers;
	}
	void setMaterialOverride( const Ref<Material> &rMaterial );
	EA_FORCE_INLINE Ref<Material> getMaterialOverride( void ) const {
		return m_materialOverride;
	}
	void setMaterialOverlay( const Ref<Material> &rMaterial );
	EA_FORCE_INLINE Ref<Material> getMaterialOverlay( void ) const {
		return m_materialOverlay;
	}
	void setRenderingMaterialOverride( const Ref<Terrain3DRenderingMaterial> &rMaterial );
	EA_FORCE_INLINE Ref<Terrain3DRenderingMaterial> getRenderingMaterialOverride( void ) const {
		return m_renderingMaterialOverride;
	}
	void setRenderingMaterialOverlay( const Ref<Terrain3DRenderingMaterial> &rMaterial );
	bool ensureRenderingMaterialStableKeysCold( void );
	EA_FORCE_INLINE Ref<Terrain3DRenderingMaterial> getRenderingMaterialOverlay( void ) const {
		return m_renderingMaterialOverlay;
	}
	void setSurfaceRenderingMaterials( const TypedArray<Terrain3DRenderingMaterial> &rMaterials );
	EA_FORCE_INLINE TypedArray<Terrain3DRenderingMaterial> getSurfaceRenderingMaterials( void ) const {
		return m_surfaceRenderingMaterials;
	}
	Ref<Terrain3DRenderingMaterial> getSurfaceRenderingMaterial( int nLod, uint32_t nSurface ) const;
	const Terrain3DRenderingMaterial *getSurfaceRenderingMaterialPtr( int nLod, uint32_t nSurface ) const;

	void setGeneratedFaces( const int nCount );
	EA_FORCE_INLINE int getGeneratedFaces( void ) const {
		return m_nGeneratedFaces;
	}
	void setGeneratedSize( const Vector2 &rSize );
	EA_FORCE_INLINE Vector2 getGeneratedSize( void ) const {
		return m_generatedSize;
	}

	EA_FORCE_INLINE int getLodCount( void ) const {
		return getMeshes().size();
	}
	void setLastLod( const int nLod );
	EA_FORCE_INLINE int getLastLod( void ) const {
		return m_iLastLod;
	}
	void setLastShadowLod( const int nLod );
	EA_FORCE_INLINE int getLastShadowLod( void ) const {
		return m_iLastShadowLod;
	}
	void setShadowImpostor( const int nLod );
	EA_FORCE_INLINE int getShadowImpostor( void ) const {
		return m_nShadowImpostor;
	}

	void setLodRange( const int nLod, const real_t fDistance );
	real_t getLodRange( const int nLod ) const;
	real_t getLodRangeBegin( const int nLod ) const;
	real_t getLodRangeEnd( const int nLod ) const;
	EA_FORCE_INLINE void setLod0Range( const real_t fDistance ) {
		setLodRange( 0, fDistance );
	}
	EA_FORCE_INLINE real_t getLod0Range( void ) const {
		return m_fLodRanges[0];
	}
	EA_FORCE_INLINE void setLod1Range( const real_t fDistance ) {
		setLodRange( 1, fDistance );
	}
	EA_FORCE_INLINE real_t getLod1Range( void ) const {
		return m_fLodRanges[1];
	}
	EA_FORCE_INLINE void setLod2Range( const real_t fDistance ) {
		setLodRange( 2, fDistance );
	}
	EA_FORCE_INLINE real_t getLod2Range( void ) const {
		return m_fLodRanges[2];
	}
	EA_FORCE_INLINE void setLod3Range( const real_t fDistance ) {
		setLodRange( 3, fDistance );
	}
	EA_FORCE_INLINE real_t getLod3Range( void ) const {
		return m_fLodRanges[3];
	}
	EA_FORCE_INLINE void setLod4Range( const real_t fDistance ) {
		setLodRange( 4, fDistance );
	}
	EA_FORCE_INLINE real_t getLod4Range( void ) const {
		return m_fLodRanges[4];
	}
	EA_FORCE_INLINE void setLod5Range( const real_t fDistance ) {
		setLodRange( 5, fDistance );
	}
	EA_FORCE_INLINE real_t getLod5Range( void ) const {
		return m_fLodRanges[5];
	}
	EA_FORCE_INLINE void setLod6Range( const real_t fDistance ) {
		setLodRange( 6, fDistance );
	}
	EA_FORCE_INLINE real_t getLod6Range( void ) const {
		return m_fLodRanges[6];
	}
	EA_FORCE_INLINE void setLod7Range( const real_t fDistance ) {
		setLodRange( 7, fDistance );
	}
	EA_FORCE_INLINE real_t getLod7Range( void ) const {
		return m_fLodRanges[7];
	}
	EA_FORCE_INLINE void setLod8Range( const real_t fDistance ) {
		setLodRange( 8, fDistance );
	}
	EA_FORCE_INLINE real_t getLod8Range( void ) const {
		return m_fLodRanges[8];
	}
	EA_FORCE_INLINE void setLod9Range( const real_t fDistance ) {
		setLodRange( 9, fDistance );
	}
	EA_FORCE_INLINE real_t getLod9Range( void ) const {
		return m_fLodRanges[9];
	}
	void setFadeMargin( const real_t fFadeMargin );
	EA_FORCE_INLINE real_t getFadeMargin( void ) const {
		return m_fFadeMargin;
	}

protected:
	void _validate_property( PropertyInfo &rProperty ) const;
	bool _property_can_revert( const StringName &rName ) const;
	bool _property_get_revert( const StringName &rName, Variant &rProperty ) const;
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dGenType_t );
VARIANT_ENUM_CAST( t3dInstanceCategory_t );
VARIANT_ENUM_CAST( t3dTerrainAlignment_t );
VARIANT_ENUM_CAST( t3dHLODContribution_t );
VARIANT_ENUM_CAST( t3dInstanceOcclusionPolicy_t );


#endif // TERRAIN3D_MESH_ASSET_CLASS_H
