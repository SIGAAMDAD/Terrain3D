// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_ASSETS_CLASS_H
#define TERRAIN3D_ASSETS_CLASS_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_owner.h"
#include "assets/resources/generated_texture.h"
#include "assets/resources/terrain_3d_mesh_asset.h"
#include "assets/resources/terrain_3d_texture_asset.h"
#include "eastl_config.h"



// t3dAssetType_e centralized in Terrain3DTypes.h.


// t3dTexturePackingMode_e centralized in Terrain3DTypes.h.


// t3dTextureSettingsBlock_s centralized in Terrain3DTypes.h.


// t3dTextureArrayLayout_s centralized in Terrain3DTypes.h.


class Terrain3DAssets : public Resource {
	GDCLASS( Terrain3DAssets, Resource );
	CLASS_NAME();

public: // Constants
private:
	Terrain3DOwner *m_pTerrain = NULL;
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
	uint64_t m_nTexturePublicationGeneration = 1u;
	uint64_t m_nTerrainVtContentGeneration = 1u;
	uint64_t m_nMeshSourceGeneration = 1u;
	uint64_t m_nInstanceVtMaterialSourceGeneration = 1u;
	int m_szThumbnailBatchIds[T3D_MAX_MESH_ASSETS]{};
	int m_nThumbnailBatchCount = 0;
	int m_iThumbnailBatchCursor = 0;
	Vector2i m_thumbnailBatchSize = V2I( 256 );
	uint64_t m_nThumbnailBatchGeneration = 0u;
	uint32_t m_nThumbnailBatchDeferredRetries = 0u;
	bool m_bThumbnailBatchActive = false;


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
	void serviceMeshThumbnailAsync( void );
	void completeMeshThumbnailAsync( int64_t nAssetId,
	        const Ref<Image> &rImage,
	        int64_t nFailure,
	        int64_t nBatchGeneration,
	        int64_t nMeshSourceGeneration,
	        int64_t nAssetObjectId );
	uint64_t computeRuntimeMaterialSourceSignature( void ) const;
	bool initializeFarMaterialArray( void );
	void advanceTexturePublicationGeneration( bool bMappingChanged );
	void advanceMeshSourceGeneration( void );
	void advanceInstanceVtMaterialSourceGeneration( void );

public:
	EA_FORCE_INLINE Terrain3DAssets( void ) {
	}
	EA_FORCE_INLINE ~Terrain3DAssets( void ) {
		destroy();
	}
	void initialize( Terrain3DOwner *pTerrain );
	EA_FORCE_INLINE bool isInitialized( void ) const {
		return m_pTerrain != NULL;
	}
	EA_FORCE_INLINE bool isInitializedFor( const Terrain3DOwner *pTerrain ) const {
		return m_pTerrain == pTerrain;
	}
	EA_FORCE_INLINE Terrain3DOwner *getTerrainOwner( void ) const {
		return m_pTerrain;
	}
	void uninitialize( Terrain3DOwner *pTerrain );
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
	EA_FORCE_INLINE int getNormalArraySize( void ) const {
		return m_generatedNormalTextures.size();
	}
	EA_FORCE_INLINE RID getSurfaceArrayRid( void ) const {
		return m_generatedSurfaceTextures.getRid();
	}
	EA_FORCE_INLINE int getSurfaceArraySize( void ) const {
		return m_generatedSurfaceTextures.size();
	}
	EA_FORCE_INLINE RID getDisplacementArrayRid( void ) const {
		return m_generatedDisplacementTextures.getRid();
	}
	EA_FORCE_INLINE int getDisplacementArraySize( void ) const {
		return m_generatedDisplacementTextures.size();
	}
	EA_FORCE_INLINE size_t getGeneratedTexturePayloadBytes( void ) const {
		return m_generatedAlbedoTextures.payloadBytes() + m_generatedNormalTextures.payloadBytes()
		        + m_generatedSurfaceTextures.payloadBytes() + m_generatedDisplacementTextures.payloadBytes()
		        + m_generatedFarAlbedoTextures.payloadBytes();
	}
	void setTexturePackingMode( const t3dTexturePackingMode_t mode );
	EA_FORCE_INLINE t3dTexturePackingMode_t getTexturePackingMode( void ) const {
		return m_texturePackingMode;
	}
	EA_FORCE_INLINE bool usesPackedPbr( void ) const {
		return m_texturePackingMode == TEXTURE_PACKING_PACKED_PBR_R8;
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
	EA_FORCE_INLINE uint64_t getTexturePublicationGeneration( void ) const {
		return m_nTexturePublicationGeneration;
	}
	EA_FORCE_INLINE uint64_t getTerrainVtContentGeneration( void ) const {
		return m_nTerrainVtContentGeneration;
	}
	EA_FORCE_INLINE uint64_t getMeshSourceGeneration( void ) const {
		return m_nMeshSourceGeneration;
	}
	EA_FORCE_INLINE uint64_t getInstanceVtMaterialSourceGeneration( void ) const {
		return m_nInstanceVtMaterialSourceGeneration;
	}
	EA_FORCE_INLINE uint64_t getRuntimeMaterialSourceSignature( void ) const {
		return computeRuntimeMaterialSourceSignature();
	}

	void setMeshAsset( const int nId, const Ref<Terrain3DMeshAsset> &rMeshAsset );
	Ref<Terrain3DMeshAsset> getMeshAsset( const int nId ) const;
	void setMeshList( const TypedArray<Terrain3DMeshAsset> &rMeshList );
	bool setMeshAssetsBatch( const Dictionary &rAssetsById );
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




#endif // TERRAIN3D_ASSETS_CLASS_H
