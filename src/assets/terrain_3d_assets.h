// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_ASSETS_CLASS_H
#define TERRAIN3D_ASSETS_CLASS_H

#include "eastl_config.h"
#include <EASTL/array.h>

#include "Terrain3DConstants.h"
#include "assets/generated_texture.h"
#include "assets/terrain_3d_mesh_asset.h"
#include "assets/terrain_3d_texture_asset.h"

class T3DTerrain;
class T3DInstancer;

typedef enum t3dAssetType_e {
	TYPE_TEXTURE,
	TYPE_MESH,
} t3dAssetType_t;

typedef enum t3dTexturePackingMode_e {
	TEXTURE_PACKING_LEGACY = 0,
	TEXTURE_PACKING_PACKED_PBR_R8 = 1,
	// Deprecated serialized alias for projects created by the intermediate
	// RGBA16UI implementation. It resolves to the new packed-PBR/R8 path.
	TEXTURE_PACKING_SURFACE_R16 = TEXTURE_PACKING_PACKED_PBR_R8,
	TEXTURE_PACKING_PBR_RGBA16UI = TEXTURE_PACKING_PACKED_PBR_R8,
} t3dTexturePackingMode_t;

typedef struct t3dTextureSettingsBlock_s {
	eastl::array<Color, TERRAIN3D_MAX_TEXTURE_ASSETS> colors{};
	eastl::array<float, TERRAIN3D_MAX_TEXTURE_ASSETS> normal_depths{};
	eastl::array<float, TERRAIN3D_MAX_TEXTURE_ASSETS> ao_strengths{};
	eastl::array<float, TERRAIN3D_MAX_TEXTURE_ASSETS> ao_light_affects{};
	eastl::array<float, TERRAIN3D_MAX_TEXTURE_ASSETS> roughness_mods{};
	eastl::array<float, TERRAIN3D_MAX_TEXTURE_ASSETS> metallics{};
	eastl::array<float, TERRAIN3D_MAX_TEXTURE_ASSETS> uv_scales{};
	eastl::array<Vector2, TERRAIN3D_MAX_TEXTURE_ASSETS> detiles{};
	eastl::array<Vector2, TERRAIN3D_MAX_TEXTURE_ASSETS> displacements{};
	int nCount = 0;
} t3dTextureSettingsBlock_t;

typedef enum t3dRuntimeMaterialChannel_e {
	RUNTIME_MATERIAL_CHANNEL_ALBEDO = 0,
	RUNTIME_MATERIAL_CHANNEL_NORMAL = 1,
	RUNTIME_MATERIAL_CHANNEL_SURFACE = 2,
	RUNTIME_MATERIAL_CHANNEL_DISPLACEMENT = 3,
	RUNTIME_MATERIAL_CHANNEL_FAR_ALBEDO = 4,
	RUNTIME_MATERIAL_CHANNEL_COUNT = 5,
} t3dRuntimeMaterialChannel_t;

typedef struct t3dRuntimeMaterialImageInfo_s {
	uint64_t nOffset = 0u;
	uint32_t nSize = 0u;
	uint32_t nChecksum = 0u;
	int32_t nWidth = 0;
	int32_t nHeight = 0;
	int32_t nFormat = ( int32_t ) ( Image::FORMAT_MAX );
	uint32_t bMipmaps = 0u;
} t3dRuntimeMaterialImageInfo_t;

typedef struct t3dRuntimeMaterialLayerInfo_s {
	eastl::array<t3dRuntimeMaterialImageInfo_t, RUNTIME_MATERIAL_CHANNEL_COUNT> channels{};
	Color averageAlbedo = COLOR_WHITE;
	Color albedoColor = COLOR_WHITE;
	float normalDepth = 1.0f;
	float aoStrength = 0.5f;
	float aoLightAffect = 0.0f;
	float roughness = 0.0f;
	float metallic = 0.0f;
	float uvScale = 0.1f;
	Vector2 detile = Vector2();
	Vector2 displacement = Vector2();
	uint32_t nBundleIndex = 0u;
} t3dRuntimeMaterialLayerInfo_t;

typedef struct t3dTextureArrayLayout_s {
	Vector2i vSize = V2I_ZERO;
	Image::Format eFormat = Image::FORMAT_MAX;
	bool bMipmaps = true;
	bool bNeedsNormalization = false;
} t3dTextureArrayLayout_t;

class T3DAssets : public Resource {
	GDCLASS( T3DAssets, Resource );
	CLASS_NAME();

public: // Constants





private:
	T3DTerrain *m_pTerrain = nullptr;
	bool m_bSuppressTextureChanged = false;
	bool m_bPersistenceDirty = false;

	TypedArray<T3DTextureAsset> m_textureList;
	TypedArray<T3DMeshAsset> m_meshList;

	T3DGeneratedTexture m_generatedAlbedoTextures;
	T3DGeneratedTexture m_generatedNormalTextures;
	T3DGeneratedTexture m_generatedSurfaceTextures;
	T3DGeneratedTexture m_generatedDisplacementTextures;
	T3DGeneratedTexture m_generatedFarAlbedoTextures;
	t3dTexturePackingMode_t m_texturePackingMode = TEXTURE_PACKING_LEGACY;

	t3dTextureSettingsBlock_t m_textureSettings;
	PackedColorArray m_textureColors;
	PackedFloat32Array m_textureNormalDepths;
	PackedFloat32Array m_textureAoStrengths;
	PackedFloat32Array m_textureAoLightAffects;
	PackedFloat32Array m_textureRoughnessMods;
	PackedFloat32Array m_textureMetallics;
	PackedFloat32Array m_textureUvScales;
	PackedVector2Array m_textureDetiles;
	PackedVector2Array m_textureDisplacements;




	eastl::array<t3dRuntimeMaterialLayerInfo_t, TERRAIN3D_MAX_TEXTURE_ASSETS> m_runtimeMaterialCookedLayers{};
	eastl::array<int, TERRAIN3D_MAX_TEXTURE_ASSETS> m_runtimeMaterialLogicalToPhysical{};
	eastl::array<int, TERRAIN3D_MAX_TEXTURE_ASSETS> m_runtimeMaterialPhysicalToLogical{};
	eastl::array<uint64_t, TERRAIN3D_MAX_TEXTURE_ASSETS> m_runtimeMaterialLastUseTick{};
	eastl::array<uint64_t, TERRAIN3D_MAX_TEXTURE_ASSETS> m_runtimeMaterialRetireAfterTick{};
	String m_runtimeMaterialCachePath;
	uint64_t m_nRuntimeMaterialCookedSignature = 0u;
	uint64_t m_nRuntimeMaterialTick = 0u;
	int m_nRuntimeMaterialPhysicalLayers = 0;
	int m_nRuntimeMaterialResidentCount = 0;
	int m_nRuntimeMaterialUploadsLastTick = 0;
	int m_nRuntimeMaterialEvictionsLastTick = 0;
	bool m_bRuntimeMaterialCacheActive = false;
	bool m_bRuntimeMaterialCookedValid = false;
	bool m_bRuntimeMaterialSourceTexturesReleased = false;



	// Mesh Thumbnail Generation
	RID m_scenario;
	RID m_viewport;
	RID m_viewportTexture;
	RID m_camera;
	RID m_keyLight;
	RID m_keyLightInstance;
	RID m_fillLight;
	RID m_fillLightInstance;
	RID m_meshInstance;

	void swapIds( const t3dAssetType_t type, const int nSrcId, const int nDstId );
	void connectAssetSignals( const t3dAssetType_t type, const Ref<T3DAssetResource> &rAsset );
	void disconnectAssetSignals( const t3dAssetType_t type, const Ref<T3DAssetResource> &rAsset );
	Ref<T3DAssetResource> makePlaceholderAsset( const t3dAssetType_t type, const int nId ) const;
	void setAssetList( const t3dAssetType_t type, const TypedArray<T3DAssetResource> &rList );
	void setAsset( const t3dAssetType_t type, const int nId, const Ref<T3DAssetResource> &rAsset );

	void validateArrayMember(
		const Ref<Texture2D> &rTexture, const Ref<T3DTextureAsset> &rAsset,
		const String &rLabel, t3dTextureArrayLayout_t &rLayout,
		Image::Format eRequiredFormat = Image::FORMAT_MAX, bool bAllowCompressedSurface = false
	);
	void updateTextureFiles( void );
	void updateTextureSettingsInternal( bool bPersistentChange );
	void updateTextureSettings( void );
	void updateTexturePreview( void );
	void updateMeshInternal( const int nId, bool bPersistentChange );
	void updateMesh( const int nId );
	void updateMeshPreview( const int nId );
	void setupThumbnailCreation( void );
	void destroyThumbnailCreation( void );
	String getRuntimeMaterialCachePath( void ) const;
	uint64_t computeRuntimeMaterialSourceSignature( void ) const;
	bool loadRuntimeMaterialCookedIndex( void );
	void publishRuntimeMaterialCookedSettings( void );
	Ref<Image> loadRuntimeMaterialCookedImage( int nLogicalId, int nChannel ) const;
	bool initializeRuntimeMaterialPhysicalArrays( int nLayerLimit = -1 );
	bool initializeFarMaterialArray( void );
	bool publishRuntimeMaterialLayer( int nLogicalId, int nPhysicalLayer );
	void updateRuntimeMaterialResidencyInternal( const uint8_t *pPriorityIds, int nPriorityCount, int nUploadBudget );
	void releaseRuntimeMaterialSourceTextures( void );
	void resetRuntimeMaterialCacheState( bool bClearGeneratedArrays );

public:
	EA_FORCE_INLINE T3DAssets( void ) {}
	EA_FORCE_INLINE ~T3DAssets( void ) { destroy(); }
	void initialize( T3DTerrain *pTerrain );
	EA_FORCE_INLINE bool isInitialized( void ) const { return m_pTerrain != nullptr; }
	EA_FORCE_INLINE bool isInitializedFor( const T3DTerrain *pTerrain ) const { return m_pTerrain == pTerrain; }
	EA_FORCE_INLINE T3DTerrain *getTerrainOwner( void ) const { return m_pTerrain; }
	void uninitialize( T3DTerrain *pTerrain );
	void destroy( void );

	void setTextureAsset( const int nId, const Ref<T3DTextureAsset> &rTexture );
	Ref<T3DTextureAsset> getTextureAsset( const int nId ) const;
	void setTextureList( const TypedArray<T3DTextureAsset> &rTextureList );
	TypedArray<T3DTextureAsset> getTextureList( void ) const;
	EA_FORCE_INLINE int getTextureCount( void ) const { return m_textureList.size(); }
	EA_FORCE_INLINE int getGeneratedArraySize( void ) const { return m_generatedAlbedoTextures.size(); }
	EA_FORCE_INLINE RID getAlbedoArrayRid( void ) const { return m_generatedAlbedoTextures.getRid(); }
	EA_FORCE_INLINE RID getFarAlbedoArrayRid( void ) const { return m_generatedFarAlbedoTextures.getRid(); }
	EA_FORCE_INLINE int getFarAlbedoArraySize( void ) const { return m_generatedFarAlbedoTextures.size(); }
	EA_FORCE_INLINE int getFarAlbedoArrayMaxLod( void ) const { return m_generatedFarAlbedoTextures.maxMipmapLevel(); }
	EA_FORCE_INLINE int getAlbedoArrayMaxLod( void ) const { return m_generatedAlbedoTextures.maxMipmapLevel(); }
	EA_FORCE_INLINE RID getNormalArrayRid( void ) const { return m_generatedNormalTextures.getRid(); }
	EA_FORCE_INLINE RID getSurfaceArrayRid( void ) const { return m_generatedSurfaceTextures.getRid(); }
	EA_FORCE_INLINE RID getDisplacementArrayRid( void ) const { return m_generatedDisplacementTextures.getRid(); }
	EA_FORCE_INLINE size_t getGeneratedTexturePayloadBytes( void ) const { return m_generatedAlbedoTextures.payloadBytes() + m_generatedNormalTextures.payloadBytes() + m_generatedSurfaceTextures.payloadBytes() + m_generatedDisplacementTextures.payloadBytes() + m_generatedFarAlbedoTextures.payloadBytes(); }
	// Deprecated compatibility alias.
	EA_FORCE_INLINE RID getPbrArrayRid( void ) const { return getSurfaceArrayRid(); }
	void setTexturePackingMode( const t3dTexturePackingMode_t mode );
	EA_FORCE_INLINE t3dTexturePackingMode_t getTexturePackingMode( void ) const { return m_texturePackingMode; }
	EA_FORCE_INLINE bool usesPackedPbr( void ) const { return m_texturePackingMode == TEXTURE_PACKING_PACKED_PBR_R8; }
	// Deprecated compatibility name. The displacement plane is R8.
	EA_FORCE_INLINE bool usesSurfaceR16( void ) const { return usesPackedPbr(); }
	EA_FORCE_INLINE int getPackedPbrMipmapCount( void ) const { return 1; }
	EA_FORCE_INLINE PackedColorArray getTextureColors( void ) const { return m_textureColors; }
	EA_FORCE_INLINE PackedFloat32Array getTextureNormalDepths( void ) const { return m_textureNormalDepths; }
	EA_FORCE_INLINE PackedFloat32Array getTextureAoStrengths( void ) const { return m_textureAoStrengths; }
	EA_FORCE_INLINE PackedFloat32Array getTextureAoLightAffects( void ) const { return m_textureAoLightAffects; }
	EA_FORCE_INLINE PackedFloat32Array getTextureRoughnessMods( void ) const { return m_textureRoughnessMods; }
	EA_FORCE_INLINE PackedFloat32Array getTextureMetallics( void ) const { return m_textureMetallics; }
	EA_FORCE_INLINE PackedFloat32Array getTextureUvScales( void ) const { return m_textureUvScales; }
	EA_FORCE_INLINE PackedVector2Array getTextureDetiles( void ) const { return m_textureDetiles; }
	EA_FORCE_INLINE PackedVector2Array getTextureDisplacements( void ) const { return m_textureDisplacements; }
	void clearTextures( const bool bUpdate = false );
	void updateTextureList( void );
	bool configureRuntimeMaterialCache( void );
	Dictionary cookRuntimeMaterialCache( void );
	void updateRuntimeMaterialResidency( const uint8_t *pPriorityIds, int nPriorityCount );
	PackedInt32Array getTextureLayerMap( void ) const;
	PackedColorArray getRuntimeMaterialAverageAlbedo( void ) const;
	Dictionary getRuntimeMaterialCacheStats( void ) const;
	EA_FORCE_INLINE uint64_t getRuntimeMaterialSourceSignature( void ) const { return computeRuntimeMaterialSourceSignature(); }
	EA_FORCE_INLINE bool isRuntimeMaterialCacheActive( void ) const { return m_bRuntimeMaterialCacheActive; }
	EA_FORCE_INLINE bool areRuntimeMaterialSourceTexturesReleased( void ) const { return m_bRuntimeMaterialSourceTexturesReleased; }
	EA_FORCE_INLINE void invalidateRuntimeMaterialCache( void ) { resetRuntimeMaterialCacheState( false ); }

	void setMeshAsset( const int nId, const Ref<T3DMeshAsset> &rMeshAsset );
	Ref<T3DMeshAsset> getMeshAsset( const int nId ) const;
	void setMeshList( const TypedArray<T3DMeshAsset> &rMeshList );
	TypedArray<T3DMeshAsset> getMeshList( void ) const;
	EA_FORCE_INLINE int getMeshCount( void ) const { return m_meshList.size(); }
	void createMeshThumbnails( const int nId = -1, const Vector2i &rSize = V2I( 256 ), const bool bForce = false );
	void updateMeshList( void );
	void loadPendingMeshes( void );

	Error save( const String &rPath = "" );

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dAssetType_t );
VARIANT_ENUM_CAST( t3dTexturePackingMode_t );

inline Ref<T3DTextureAsset> T3DAssets::getTextureAsset( const int nId ) const {
	if ( nId >= 0 && nId < m_textureList.size() ) {
		const Ref<T3DTextureAsset> pAsset = m_textureList[nId];
		return pAsset.is_valid() && !pAsset->m_bPlaceholder ? pAsset : Ref<T3DTextureAsset>();
	}
	return Ref<T3DTextureAsset>();
}

inline Ref<T3DMeshAsset> T3DAssets::getMeshAsset( const int nId ) const {
	if ( nId >= 0 && nId < m_meshList.size() ) {
		const Ref<T3DMeshAsset> pAsset = m_meshList[nId];
		return pAsset.is_valid() && !pAsset->m_bPlaceholder ? pAsset : Ref<T3DMeshAsset>();
	}
	return Ref<T3DMeshAsset>();
}
#endif // TERRAIN3D_ASSETS_CLASS_H
