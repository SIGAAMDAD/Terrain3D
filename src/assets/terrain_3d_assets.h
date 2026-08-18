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

class T3DAssets : public Resource {
	using Terrain3DAssets = T3DAssets;
	GDCLASS( Terrain3DAssets, Resource );
	CLASS_NAME();

public: // Constants
	typedef enum t3dassetType_e {
		TYPE_TEXTURE,
		TYPE_MESH,
	} t3dassetType_t;

	typedef enum t3dtexturePackingMode_e {
		TEXTURE_PACKING_LEGACY = 0,
		TEXTURE_PACKING_PACKED_PBR_R8 = 1,
		// Deprecated serialized alias for projects created by the intermediate
		// RGBA16UI implementation. It resolves to the new packed-PBR/R8 path.
		TEXTURE_PACKING_SURFACE_R16 = TEXTURE_PACKING_PACKED_PBR_R8,
		TEXTURE_PACKING_PBR_RGBA16UI = TEXTURE_PACKING_PACKED_PBR_R8,
	} t3dtexturePackingMode_t;


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
	t3dtexturePackingMode_t m_texturePackingMode = TEXTURE_PACKING_LEGACY;
	typedef struct t3dtextureSettingsBlock_s {
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
	} t3dtextureSettingsBlock_t;
	t3dtextureSettingsBlock_t m_textureSettings;
	PackedColorArray m_textureColors;
	PackedFloat32Array m_textureNormalDepths;
	PackedFloat32Array m_textureAoStrengths;
	PackedFloat32Array m_textureAoLightAffects;
	PackedFloat32Array m_textureRoughnessMods;
	PackedFloat32Array m_textureMetallics;
	PackedFloat32Array m_textureUvScales;
	PackedVector2Array m_textureDetiles;
	PackedVector2Array m_textureDisplacements;

	typedef enum t3druntimeMaterialChannel_e {
		RUNTIME_MATERIAL_CHANNEL_ALBEDO = 0,
		RUNTIME_MATERIAL_CHANNEL_NORMAL = 1,
		RUNTIME_MATERIAL_CHANNEL_SURFACE = 2,
		RUNTIME_MATERIAL_CHANNEL_DISPLACEMENT = 3,
		RUNTIME_MATERIAL_CHANNEL_COUNT = 4,
	} t3druntimeMaterialChannel_t;
	typedef struct t3druntimeMaterialImageInfo_s {
		uint64_t nOffset = 0u;
		uint32_t nSize = 0u;
		uint32_t nChecksum = 0u;
		int32_t nWidth = 0;
		int32_t nHeight = 0;
		int32_t nFormat = ( int32_t ) ( Image::FORMAT_MAX );
		uint32_t bMipmaps = 0u;
	} t3druntimeMaterialImageInfo_t;
	typedef struct t3druntimeMaterialLayerInfo_s {
		eastl::array<t3druntimeMaterialImageInfo_t, RUNTIME_MATERIAL_CHANNEL_COUNT> channels{};
		Color averageAlbedo = COLOR_WHITE;
	} t3druntimeMaterialLayerInfo_t;
	eastl::array<t3druntimeMaterialLayerInfo_t, TERRAIN3D_MAX_TEXTURE_ASSETS> m_runtimeMaterialCookedLayers{};
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

	typedef struct t3dtextureArrayLayout_s {
		Vector2i vSize = V2I_ZERO;
		Image::Format eFormat = Image::FORMAT_MAX;
		bool bMipmaps = true;
		bool bNeedsNormalization = false;
	} t3dtextureArrayLayout_t;

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

	void swapIds( const t3dassetType_t type, const int nSrcId, const int nDstId );
	void connectAssetSignals( const t3dassetType_t type, const Ref<T3DAssetResource> &rAsset );
	void disconnectAssetSignals( const t3dassetType_t type, const Ref<T3DAssetResource> &rAsset );
	Ref<T3DAssetResource> makePlaceholderAsset( const t3dassetType_t type, const int nId ) const;
	void setAssetList( const t3dassetType_t type, const TypedArray<T3DAssetResource> &rList );
	void setAsset( const t3dassetType_t type, const int nId, const Ref<T3DAssetResource> &rAsset );

	void validateArrayMember(
		const Ref<Texture2D> &rTexture, const Ref<T3DTextureAsset> &rAsset,
		const String &rLabel, t3dtextureArrayLayout_t &rLayout,
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
	Ref<Image> loadRuntimeMaterialCookedImage( int nLogicalId, int nChannel ) const;
	bool initializeRuntimeMaterialPhysicalArrays( int nLayerLimit = -1 );
	bool publishRuntimeMaterialLayer( int nLogicalId, int nPhysicalLayer );
	void releaseRuntimeMaterialSourceTextures( void );
	void resetRuntimeMaterialCacheState( bool bClearGeneratedArrays );

public:
	T3DAssets( void ) {}
	~T3DAssets( void ) {
		destroy();
	}
	void initialize( T3DTerrain *pTerrain );
	bool isInitialized( void ) const {
		return m_pTerrain != nullptr;
	}
	bool isInitializedFor( const T3DTerrain *pTerrain ) const {
		return m_pTerrain == pTerrain;
	}
	T3DTerrain *getTerrainOwner( void ) const {
		return m_pTerrain;
	}
	void uninitialize( T3DTerrain *pTerrain );
	void destroy( void );

	void setTextureAsset( const int nId, const Ref<T3DTextureAsset> &rTexture );
	Ref<T3DTextureAsset> getTextureAsset( const int nId ) const;
	void setTextureList( const TypedArray<T3DTextureAsset> &rTextureList );
	TypedArray<T3DTextureAsset> getTextureList( void ) const;
	int getTextureCount( void ) const {
		return m_textureList.size();
	}
	int getGeneratedArraySize( void ) const {
		return m_generatedAlbedoTextures.size();
	}
	RID getAlbedoArrayRid( void ) const {
		return m_generatedAlbedoTextures.getRid();
	}
	int getAlbedoArrayMaxLod( void ) const {
		return m_generatedAlbedoTextures.maxMipmapLevel();
	}
	RID getNormalArrayRid( void ) const {
		return m_generatedNormalTextures.getRid();
	}
	RID getSurfaceArrayRid( void ) const {
		return m_generatedSurfaceTextures.getRid();
	}
	RID getDisplacementArrayRid( void ) const {
		return m_generatedDisplacementTextures.getRid();
	}
	size_t getGeneratedTexturePayloadBytes( void ) const {
		return m_generatedAlbedoTextures.payloadBytes() + m_generatedNormalTextures.payloadBytes() +
			m_generatedSurfaceTextures.payloadBytes() + m_generatedDisplacementTextures.payloadBytes();
	}
	// Deprecated compatibility alias.
	RID getPbrArrayRid( void ) const {
		return getSurfaceArrayRid();
	}
	void setTexturePackingMode( const t3dtexturePackingMode_t mode );
	t3dtexturePackingMode_t getTexturePackingMode( void ) const {
		return m_texturePackingMode;
	}
	bool usesPackedPbr( void ) const {
		return m_texturePackingMode == TEXTURE_PACKING_PACKED_PBR_R8;
	}
	// Deprecated compatibility name. The displacement plane is R8.
	bool usesSurfaceR16( void ) const {
		return usesPackedPbr();
	}
	int getPackedPbrMipmapCount( void ) const {
		return 1;
	}
	PackedColorArray getTextureColors( void ) const {
		return m_textureColors;
	}
	PackedFloat32Array getTextureNormalDepths( void ) const {
		return m_textureNormalDepths;
	}
	PackedFloat32Array getTextureAoStrengths( void ) const {
		return m_textureAoStrengths;
	}
	PackedFloat32Array getTextureAoLightAffects( void ) const {
		return m_textureAoLightAffects;
	}
	PackedFloat32Array getTextureRoughnessMods( void ) const {
		return m_textureRoughnessMods;
	}
	PackedFloat32Array getTextureMetallics( void ) const {
		return m_textureMetallics;
	}
	PackedFloat32Array getTextureUvScales( void ) const {
		return m_textureUvScales;
	}
	PackedVector2Array getTextureDetiles( void ) const {
		return m_textureDetiles;
	}
	PackedVector2Array getTextureDisplacements( void ) const {
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
	bool isRuntimeMaterialCacheActive( void ) const { return m_bRuntimeMaterialCacheActive; }

	void setMeshAsset( const int nId, const Ref<T3DMeshAsset> &rMeshAsset );
	Ref<T3DMeshAsset> getMeshAsset( const int nId ) const;
	void setMeshList( const TypedArray<T3DMeshAsset> &rMeshList );
	TypedArray<T3DMeshAsset> getMeshList( void ) const;
	int getMeshCount( void ) const {
		return m_meshList.size();
	}
	void createMeshThumbnails( const int nId = -1, const Vector2i &rSize = V2I( 256 ), const bool bForce = false );
	void updateMeshList( void );
	void loadPendingMeshes( void );

	Error save( const String &rPath = "" );

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( T3DAssets::t3dassetType_t );
VARIANT_ENUM_CAST( T3DAssets::t3dtexturePackingMode_t );

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

using Terrain3DAssets = T3DAssets;

#endif // TERRAIN3D_ASSETS_CLASS_H
