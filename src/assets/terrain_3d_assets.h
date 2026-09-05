// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_ASSETS_CLASS_H
#define TERRAIN3D_ASSETS_CLASS_H

#include "Terrain3DConstants.h"
#include "assets/generated_texture.h"
#include "assets/terrain_3d_mesh_asset.h"
#include "assets/terrain_3d_texture_asset.h"
#include "eastl_config.h"


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
	Color m_szColors[s_nMaxTextureAssets]{};
	float m_szNormalDepths[s_nMaxTextureAssets]{};
	float m_szAoStrengths[s_nMaxTextureAssets]{};
	float m_szAoLightAffects[s_nMaxTextureAssets]{};
	float m_szRoughnessMods[s_nMaxTextureAssets]{};
	float m_szMetallics[s_nMaxTextureAssets]{};
	float m_szUvScales[s_nMaxTextureAssets]{};
	Vector2 m_szDetiles[s_nMaxTextureAssets]{};
	Vector2 m_szDisplacements[s_nMaxTextureAssets]{};
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
	t3dRuntimeMaterialImageInfo_t m_szChannels[RUNTIME_MATERIAL_CHANNEL_COUNT]{};
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


typedef struct t3dRuntimeMaterialTokenBucket_s {
	uint64_t m_nTokens = 0u;
	uint64_t m_nRatePerSecond = 0u;
	uint64_t m_nBurst = 0u;
	uint64_t m_nLastRefillUsec = 0u;
} t3dRuntimeMaterialTokenBucket_t;

typedef struct t3dRuntimeMaterialReadRequest_s {
	int16_t m_nLogicalId = -1;
	uint32_t m_nReadBytes = 0u;
	uint32_t m_nPreparedBytes = 0u;
} t3dRuntimeMaterialReadRequest_t;

typedef struct t3dRuntimeMaterialCompletion_s {
	Ref<Image> m_szImages[RUNTIME_MATERIAL_CHANNEL_DISPLACEMENT + 1];
	int16_t m_nLogicalId = -1;
	uint32_t m_nReadBytes = 0u;
	uint32_t m_nUploadBytes = 0u;
	uint8_t m_nTextureUpdateCalls = 0u;
	bool m_bSuccess = false;
} t3dRuntimeMaterialCompletion_t;

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
private:
	Terrain3D *m_pTerrain = NULL;
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
	uint64_t m_nTextureLayerMappingGeneration = 1u;
	uint64_t m_nMeshSourceGeneration = 1u;

	t3dRuntimeMaterialLayerInfo_t m_runtimeMaterialCookedLayers[s_nMaxTextureAssets]{};
	int m_runtimeMaterialLogicalToPhysical[s_nMaxTextureAssets]{};
	int m_runtimeMaterialPhysicalToLogical[s_nMaxTextureAssets]{};
	uint64_t m_runtimeMaterialLastUseTick[s_nMaxTextureAssets]{};
	uint64_t m_runtimeMaterialRetireAfterTick[s_nMaxTextureAssets]{};
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

	// Runtime material residency is producer/consumer: the owner thread admits
	// byte-bounded work and performs GPU updates; one persistent worker owns the
	// cooked-file handle, reads/checksums payloads, and constructs Images.
	t3dThread_t m_runtimeMaterialWorker;
	mutable t3dMutex_t m_runtimeMaterialWorkerMutex;
	t3dCondition_t m_runtimeMaterialWorkerWake;
	t3dRuntimeMaterialReadRequest_t m_runtimeMaterialRequestRing[s_nMaxTextureAssets]{};
	t3dRuntimeMaterialCompletion_t m_runtimeMaterialCompletionRing[s_nMaxTextureAssets]{};
	uint8_t m_runtimeMaterialRequestState[s_nMaxTextureAssets]{};
	uint8_t m_runtimeMaterialChecksumVerified[s_nMaxTextureAssets][RUNTIME_MATERIAL_CHANNEL_DISPLACEMENT + 1]{};
	int m_iRuntimeMaterialRequestRead = 0;
	int m_iRuntimeMaterialRequestWrite = 0;
	int m_nRuntimeMaterialRequestCount = 0;
	int m_iRuntimeMaterialCompletionRead = 0;
	int m_iRuntimeMaterialCompletionWrite = 0;
	int m_nRuntimeMaterialCompletionCount = 0;
	uint64_t m_nRuntimeMaterialPreparedBytesInFlight = 0u;
	uint64_t m_nRuntimeMaterialReadBytesLastTick = 0u;
	uint64_t m_nRuntimeMaterialUploadBytesLastTick = 0u;
	uint64_t m_nRuntimeMaterialBudgetRejects = 0u;
	uint64_t m_nRuntimeMaterialChecksumCacheHits = 0u;
	uint64_t m_nRuntimeMaterialWorkerFileOpens = 0u;
	int m_nRuntimeMaterialCompletionsLastTick = 0;
	bool m_bRuntimeMaterialWorkerStop = false;
	bool m_bRuntimeMaterialWorkerRunning = false;
	t3dRuntimeMaterialTokenBucket_t m_runtimeMaterialDiskBucket;
	t3dRuntimeMaterialTokenBucket_t m_runtimeMaterialGpuBucket;
	t3dRuntimeMaterialTokenBucket_t m_runtimeMaterialUpdateCallBucket;

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
	String getRuntimeMaterialCachePath( void ) const;
	uint64_t computeRuntimeMaterialSourceSignature( void ) const;
	bool loadRuntimeMaterialCookedIndex( void );
	void publishRuntimeMaterialCookedSettings( void );
	Ref<Image> loadRuntimeMaterialCookedImage( int nLogicalId, int nChannel ) const;
	bool createRuntimeMaterialChannelArray(
	        Terrain3DGeneratedTexture &rGenerated, int nChannel, int nPhysicalLayers, bool bFullyResident ) const;
	void refreshRuntimeMaterialBindings( void );
	bool initializeRuntimeMaterialPhysicalArrays( int nLayerLimit = -1 );
	bool initializeFarMaterialArray( void );
	bool publishRuntimeMaterialPreparedLayer( const t3dRuntimeMaterialCompletion_t &rCompletion, int nPhysicalLayer );
	void startRuntimeMaterialWorker( void );
	void stopRuntimeMaterialWorker( void );
	void runtimeMaterialWorkerMain( void );
	bool queueRuntimeMaterialRead( int nLogicalId );
	bool peekRuntimeMaterialCompletion( t3dRuntimeMaterialCompletion_t &rCompletion ) const;
	void popRuntimeMaterialCompletion( void );
	void initializeRuntimeMaterialBudgets( void );
	static void refillRuntimeMaterialBucket( t3dRuntimeMaterialTokenBucket_t &rBucket, uint64_t nNowUsec );
	static bool consumeRuntimeMaterialBucket( t3dRuntimeMaterialTokenBucket_t &rBucket, uint64_t nAmount, uint64_t nNowUsec );
	void updateRuntimeMaterialResidencyInternal( const uint8_t *pPriorityIds, int nPriorityCount, int nUploadBudget );
	void releaseRuntimeMaterialSourceTextures( void );
	void resetRuntimeMaterialCacheState( bool bClearGeneratedArrays );
	void advanceTexturePublicationGeneration( bool bMappingChanged );
	void advanceMeshSourceGeneration( void );

public:
	EA_FORCE_INLINE Terrain3DAssets( void ) {
	}
	EA_FORCE_INLINE ~Terrain3DAssets( void ) {
		destroy();
	}
	void initialize( Terrain3D *pTerrain );
	EA_FORCE_INLINE bool isInitialized( void ) const {
		return m_pTerrain != NULL;
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
	EA_FORCE_INLINE int getTextureLayerPhysicalIndexInternal( const int nLogical ) const {
		if ( nLogical < 0 || nLogical >= s_nMaxTextureAssets ) {
			return nLogical;
		}
		return m_bRuntimeMaterialCacheActive ? m_runtimeMaterialLogicalToPhysical[( size_t )( nLogical )] : nLogical;
	}
	EA_FORCE_INLINE uint64_t getTexturePublicationGeneration( void ) const {
		return m_nTexturePublicationGeneration;
	}
	EA_FORCE_INLINE uint64_t getTextureLayerMappingGeneration( void ) const {
		return m_nTextureLayerMappingGeneration;
	}
	EA_FORCE_INLINE uint64_t getMeshSourceGeneration( void ) const {
		return m_nMeshSourceGeneration;
	}
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
