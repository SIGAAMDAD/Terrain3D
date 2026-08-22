// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_ASSETS_CLASS_H
#define TERRAIN3D_ASSETS_CLASS_H

#include "Terrain3DConstants.h"
#include "assets/generated_texture.h"
#include "assets/terrain_3d_mesh_asset.h"
#include "assets/terrain_3d_texture_asset.h"
#include "eastl_config.h"

#include <EASTL/array.h>

class Terrain3D;
class Terrain3DInstancer;

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
	eastl::array<Color, s_nMaxTextureAssets> m_szColors{};
	eastl::array<float, s_nMaxTextureAssets> m_szNormalDepths{};
	eastl::array<float, s_nMaxTextureAssets> m_szAoStrengths{};
	eastl::array<float, s_nMaxTextureAssets> m_szAoLightAffects{};
	eastl::array<float, s_nMaxTextureAssets> m_szRoughnessMods{};
	eastl::array<float, s_nMaxTextureAssets> m_szMetallics{};
	eastl::array<float, s_nMaxTextureAssets> m_szUvScales{};
	eastl::array<Vector2, s_nMaxTextureAssets> m_szDetiles{};
	eastl::array<Vector2, s_nMaxTextureAssets> m_szDisplacements{};
	int m_nCount = 0;
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
	uint64_t m_iOffset = 0u;
	uint32_t m_nSize = 0u;
	uint32_t m_nChecksum = 0u;
	int32_t m_nWidth = 0;
	int32_t m_nHeight = 0;
	int32_t m_nFormat = ( int32_t )( Image::FORMAT_MAX );
	uint32_t m_nMipmaps = 0u;
} t3dRuntimeMaterialImageInfo_t;

typedef struct t3dRuntimeMaterialLayerInfo_s {
	eastl::array<t3dRuntimeMaterialImageInfo_t, RUNTIME_MATERIAL_CHANNEL_COUNT> m_szChannels{};
	Color m_averageAlbedo = s_colorWhite;
	Color m_albedoColor = s_colorWhite;
	float m_fNormalDepth = 1.0f;
	float m_fAoStrength = 0.5f;
	float m_fAoLightAffect = 0.0f;
	float m_fRoughness = 0.0f;
	float m_fMetallic = 0.0f;
	float m_fUvScale = 0.1f;
	Vector2 m_detile = Vector2();
	Vector2 m_displacement = Vector2();
	uint32_t m_iBundleIndex = 0u;
} t3dRuntimeMaterialLayerInfo_t;

typedef struct t3dTextureArrayLayout_s {
	Vector2i m_vSize = s_v2iZero;
	Image::Format m_eFormat = Image::FORMAT_MAX;
	bool m_bMipmaps = true;
	bool m_bNeedsNormalization = false;
} t3dTextureArrayLayout_t;

class Terrain3DAssets : public Resource {
	GDCLASS( Terrain3DAssets, Resource );
	CLASS_NAME();

public: // Constants
	static constexpr int MAX_TEXTURE_ASSETS = s_nMaxTextureAssets;
	static constexpr int MAX_MESH_ASSETS = s_nMaxMeshAssets;

private:
	Terrain3D *m_pTerrain = nullptr;
	bool m_bSuppressTextureChanged = false;
	bool m_bPersistenceDirty = false;

	TypedArray<Terrain3DTextureAsset> m_textureList;
	TypedArray<Terrain3DMeshAsset> m_meshList;

	Terrain3DGeneratedTexture m_generatedAlbedoTextures;
	Terrain3DGeneratedTexture m_generatedNormalTextures;
	Terrain3DGeneratedTexture m_generatedSurfaceTextures;
	Terrain3DGeneratedTexture m_generatedDisplacementTextures;
	Terrain3DGeneratedTexture m_generatedFarAlbedoTextures;
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

	eastl::array<t3dRuntimeMaterialLayerInfo_t, s_nMaxTextureAssets> m_runtimeMaterialCookedLayers{};
	eastl::array<int, s_nMaxTextureAssets> m_runtimeMaterialLogicalToPhysical{};
	eastl::array<int, s_nMaxTextureAssets> m_runtimeMaterialPhysicalToLogical{};
	eastl::array<uint64_t, s_nMaxTextureAssets> m_runtimeMaterialLastUseTick{};
	eastl::array<uint64_t, s_nMaxTextureAssets> m_runtimeMaterialRetireAfterTick{};
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
	RID m_hScenario;
	RID m_hViewport;
	RID m_hViewportTexture;
	RID m_hCamera;
	RID m_hKeyLight;
	RID m_hKeyLightInstance;
	RID m_hFillLight;
	RID m_hFillLightInstance;
	RID m_hMeshInstance;

	void swapIds( const t3dAssetType_t type, const int nSrcId, const int nDstId );
	void connectAssetSignals( const t3dAssetType_t type, const Ref<Terrain3DAssetResource> &rAsset );
	void disconnectAssetSignals( const t3dAssetType_t type, const Ref<Terrain3DAssetResource> &rAsset );
	Ref<Terrain3DAssetResource> makePlaceholderAsset( const t3dAssetType_t type, const int nId ) const;
	void setAssetList( const t3dAssetType_t type, const TypedArray<Terrain3DAssetResource> &rList );
	void setAsset( const t3dAssetType_t type, const int nId, const Ref<Terrain3DAssetResource> &rAsset );

	void validateArrayMember( const Ref<Texture2D> &rTexture,
			const Ref<Terrain3DTextureAsset> &rAsset,
			const String &rLabel,
			t3dTextureArrayLayout_t &rLayout,
			Image::Format eRequiredFormat = Image::FORMAT_MAX,
			bool bAllowCompressedSurface = false );
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
	EA_FORCE_INLINE Terrain3DAssets( void ) {}
	EA_FORCE_INLINE ~Terrain3DAssets( void ) {
		destroy();
	}
	void initialize( Terrain3D *pTerrain );
	EA_FORCE_INLINE bool isInitialized( void ) const {
		return m_pTerrain != nullptr;
	}
	EA_FORCE_INLINE bool isInitializedFor( const Terrain3D *pTerrain ) const {
		return m_pTerrain == pTerrain;
	}
	EA_FORCE_INLINE Terrain3D *getTerrainOwner( void ) const {
		return m_pTerrain;
	}
	void uninitialize( Terrain3D *pTerrain );
	void destroy( void );

	void setTextureAsset( const int nId, const Ref<Terrain3DTextureAsset> &rTexture );
	Ref<Terrain3DTextureAsset> getTextureAsset( const int nId ) const;
	void setTextureList( const TypedArray<Terrain3DTextureAsset> &rTextureList );
	TypedArray<Terrain3DTextureAsset> getTextureList( void ) const;
	EA_FORCE_INLINE int getTextureCount( void ) const {
		return m_textureList.size();
	}
	EA_FORCE_INLINE int getGeneratedArraySize( void ) const {
		return m_generatedAlbedoTextures.size();
	}
	EA_FORCE_INLINE RID getAlbedoArrayRid( void ) const {
		return m_generatedAlbedoTextures.getRid();
	}
	EA_FORCE_INLINE RID getFarAlbedoArrayRid( void ) const {
		return m_generatedFarAlbedoTextures.getRid();
	}
	EA_FORCE_INLINE int getFarAlbedoArraySize( void ) const {
		return m_generatedFarAlbedoTextures.size();
	}
	EA_FORCE_INLINE int getFarAlbedoArrayMaxLod( void ) const {
		return m_generatedFarAlbedoTextures.maxMipmapLevel();
	}
	EA_FORCE_INLINE int getAlbedoArrayMaxLod( void ) const {
		return m_generatedAlbedoTextures.maxMipmapLevel();
	}
	EA_FORCE_INLINE RID getNormalArrayRid( void ) const {
		return m_generatedNormalTextures.getRid();
	}
	EA_FORCE_INLINE RID getSurfaceArrayRid( void ) const {
		return m_generatedSurfaceTextures.getRid();
	}
	EA_FORCE_INLINE RID getDisplacementArrayRid( void ) const {
		return m_generatedDisplacementTextures.getRid();
	}
	EA_FORCE_INLINE size_t getGeneratedTexturePayloadBytes( void ) const {
		return m_generatedAlbedoTextures.payloadBytes() + m_generatedNormalTextures.payloadBytes() +
				m_generatedSurfaceTextures.payloadBytes() + m_generatedDisplacementTextures.payloadBytes() +
				m_generatedFarAlbedoTextures.payloadBytes();
	}
	// Deprecated compatibility alias.
	EA_FORCE_INLINE RID getPbrArrayRid( void ) const {
		return getSurfaceArrayRid();
	}
	void setTexturePackingMode( const t3dTexturePackingMode_t mode );
	EA_FORCE_INLINE t3dTexturePackingMode_t getTexturePackingMode( void ) const {
		return m_texturePackingMode;
	}
	EA_FORCE_INLINE bool usesPackedPbr( void ) const {
		return m_texturePackingMode == TEXTURE_PACKING_PACKED_PBR_R8;
	}
	// Deprecated compatibility name. The displacement plane is R8.
	EA_FORCE_INLINE bool usesSurfaceR16( void ) const {
		return usesPackedPbr();
	}
	EA_FORCE_INLINE int getPackedPbrMipmapCount( void ) const {
		return 1;
	}
	EA_FORCE_INLINE PackedColorArray getTextureColors( void ) const {
		return m_textureColors;
	}
	EA_FORCE_INLINE PackedFloat32Array getTextureNormalDepths( void ) const {
		return m_textureNormalDepths;
	}
	EA_FORCE_INLINE PackedFloat32Array getTextureAoStrengths( void ) const {
		return m_textureAoStrengths;
	}
	EA_FORCE_INLINE PackedFloat32Array getTextureAoLightAffects( void ) const {
		return m_textureAoLightAffects;
	}
	EA_FORCE_INLINE PackedFloat32Array getTextureRoughnessMods( void ) const {
		return m_textureRoughnessMods;
	}
	EA_FORCE_INLINE PackedFloat32Array getTextureMetallics( void ) const {
		return m_textureMetallics;
	}
	EA_FORCE_INLINE PackedFloat32Array getTextureUvScales( void ) const {
		return m_textureUvScales;
	}
	EA_FORCE_INLINE PackedVector2Array getTextureDetiles( void ) const {
		return m_textureDetiles;
	}
	EA_FORCE_INLINE PackedVector2Array getTextureDisplacements( void ) const {
		return m_textureDisplacements;
	}
	void clearTextures( const bool bUpdate = false );
	void updateTextureList( void );
	bool configureRuntimeMaterialCache( void );
	Dictionary cookRuntimeMaterialCache( void );
	void updateRuntimeMaterialResidency( const uint8_t *pPriorityIds, int nPriorityCount );
	PackedInt32Array getTextureLayerMap( void ) const;
	PackedColorArray getRuntimeMaterialAverageAlbedo( void ) const;
	Dictionary getRuntimeMaterialCacheStats( void ) const;
	EA_FORCE_INLINE uint64_t getRuntimeMaterialSourceSignature( void ) const {
		return computeRuntimeMaterialSourceSignature();
	}
	EA_FORCE_INLINE bool isRuntimeMaterialCacheActive( void ) const {
		return m_bRuntimeMaterialCacheActive;
	}
	EA_FORCE_INLINE bool areRuntimeMaterialSourceTexturesReleased( void ) const {
		return m_bRuntimeMaterialSourceTexturesReleased;
	}
	EA_FORCE_INLINE void invalidateRuntimeMaterialCache( void ) {
		resetRuntimeMaterialCacheState( false );
	}

	void setMeshAsset( const int nId, const Ref<Terrain3DMeshAsset> &rMeshAsset );
	Ref<Terrain3DMeshAsset> getMeshAsset( const int nId ) const;
	void setMeshList( const TypedArray<Terrain3DMeshAsset> &rMeshList );
	TypedArray<Terrain3DMeshAsset> getMeshList( void ) const;
	EA_FORCE_INLINE int getMeshCount( void ) const {
		return m_meshList.size();
	}
	void createMeshThumbnails( const int nId = -1, const Vector2i &rSize = V2I( 256 ), const bool bForce = false );
	void updateMeshList( void );
	void loadPendingMeshes( void );

	Error save( const String &rPath = "" );

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dAssetType_t );
VARIANT_ENUM_CAST( t3dTexturePackingMode_t );

inline Ref<Terrain3DTextureAsset> Terrain3DAssets::getTextureAsset( const int nId ) const {
	if ( nId >= 0 && nId < m_textureList.size() ) {
		const Ref<Terrain3DTextureAsset> pAsset = m_textureList[nId];
		return pAsset.is_valid() && !pAsset->m_bPlaceholder ? pAsset : Ref<Terrain3DTextureAsset>();
	}
	return Ref<Terrain3DTextureAsset>();
}

inline Ref<Terrain3DMeshAsset> Terrain3DAssets::getMeshAsset( const int nId ) const {
	if ( nId >= 0 && nId < m_meshList.size() ) {
		const Ref<Terrain3DMeshAsset> pAsset = m_meshList[nId];
		return pAsset.is_valid() && !pAsset->m_bPlaceholder ? pAsset : Ref<Terrain3DMeshAsset>();
	}
	return Ref<Terrain3DMeshAsset>();
}
#endif // TERRAIN3D_ASSETS_CLASS_H
