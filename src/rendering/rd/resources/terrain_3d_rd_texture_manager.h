// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_TEXTURE_MANAGER_H
#define TERRAIN3D_RD_TEXTURE_MANAGER_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_sync.h"
#include "core/config/terrain_3d_compiler.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_texture.h"

#include <stdint.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
using namespace godot;

// t3dRdTextureSemantic_e centralized in Terrain3DTypes.h.


// t3dRdTextureBatchState_e centralized in Terrain3DTypes.h.


typedef struct t3dRdTextureBatchUpdate_s {

	Terrain3DRdTexture *m_pTexture = NULL;
	uint32_t m_nTextureGeneration = 0u;
	PackedByteArray m_data;
} t3dRdTextureBatchUpdate_t;

typedef struct t3dRdTextureUpdateRect_s {
	uint32_t m_nX = 0u;
	uint32_t m_nY = 0u;
	uint32_t m_nWidth = 0u;
	uint32_t m_nHeight = 0u;
} t3dRdTextureUpdateRect_t;

typedef struct t3dRdTextureOperationRequest_s {
	t3dRdTextureAsyncKind_t m_kind = T3D_RD_TEXTURE_ASYNC_NONE;
	Terrain3DRdTexture *m_pTexture = NULL;
	uint32_t m_nTextureGeneration = 0u;
	Terrain3DRdTexture *m_pStaging = NULL;
	uint32_t m_nStagingGeneration = 0u;
	uint32_t m_nLayer = 0u;
	t3dRdTextureUpdateRect_t m_destination;
	PackedByteArray m_data;
} t3dRdTextureOperationRequest_t;

// Global-RD scheduling/batching authority for Terrain3D-generated textures.
// Terrain3DRdTexture owns physical texture identity, layout, tracked creation,
// I/O, and release. This manager only serializes cross-thread calls against
// canonical texture owners, tracked batches, Image-to-layout conversion, and
// telemetry. It never stores or mutates a texture RID directly.
class Terrain3DRdTextureManager : public Object {
	GDCLASS( Terrain3DRdTextureManager, Object );
	CLASS_NAME_STATIC( "Terrain3DRdTextureManager" );

private:
typedef struct t3dAsyncTextureOperationSlot_s {

		t3dAtomicU64_t m_nGeneration{ 0u };
		t3dAtomicU8_t m_nState{ T3D_RD_TEXTURE_ASYNC_UNUSED };
		t3dRdTextureAsyncKind_t m_kind = T3D_RD_TEXTURE_ASYNC_NONE;
		Terrain3DRdTexture *m_pTexture = NULL;
		Terrain3DRdTexture *m_pStaging = NULL;
		uint32_t m_nTextureGeneration = 0u;
		uint32_t m_nStagingGeneration = 0u;
		uint32_t m_nLayer = 0u;
		uint32_t m_nDestinationX = 0u;
		uint32_t m_nDestinationY = 0u;
		uint32_t m_nWidth = 0u;
		uint32_t m_nHeight = 0u;
		PackedByteArray m_data;
	} t3dAsyncTextureOperationSlot_t;

	typedef struct t3dTrackedTextureBatchSlot_s {

		t3dAtomicU64_t m_nGeneration{ 0u };
		t3dAtomicU8_t m_nState{ T3D_RD_TEXTURE_BATCH_UNUSED };
		Terrain3DRdTexture *m_szTextures[T3D_TRACKED_BATCH_UPDATE_COUNT]{};
		uint32_t m_szTextureGenerations[T3D_TRACKED_BATCH_UPDATE_COUNT]{};
		PackedByteArray m_szData[T3D_TRACKED_BATCH_UPDATE_COUNT];
		uint32_t m_nLayer = 0u;
		uint32_t m_nCount = 0u;
	} t3dTrackedTextureBatchSlot_t;


	static Terrain3DRdTextureManager *s_pSingleton;
	Terrain3DMutex m_createMutex;
	Terrain3DMutex m_resultMutex;
	Terrain3DMutex m_freeMutex;
	Terrain3DRdTexture m_createResult;
	t3dRdTextureSpec_t m_createSpec;
	TypedArray<PackedByteArray> m_createInitialData;
	Terrain3DMemoryAccount *m_createAccount = nullptr;
	bool m_bCreateSucceeded = false;
	Terrain3DRdTexture *m_pFreeTexture = NULL;
	t3dAtomicU64_t m_nCreateCount{ 0u };
	t3dAtomicU64_t m_nUpdateCount{ 0u };
	t3dAtomicU64_t m_nRegionUpdateCount{ 0u };
	t3dAtomicU64_t m_nUploadedBytes{ 0u };
	t3dAtomicU64_t m_nAsyncOperationGeneration{ 1u };
	t3dAtomicU32_t m_nAsyncOperationCursor{ 0u };
	t3dAtomicU64_t m_nTrackedBatchGeneration{ 1u };
	t3dAsyncTextureOperationSlot_t m_szAsyncOperations[T3D_TRACKED_BATCH_SLOT_COUNT];
	t3dTrackedTextureBatchSlot_t m_szTrackedBatches[T3D_TRACKED_BATCH_SLOT_COUNT];

	void clearTrackedTextureBatchPayload( t3dTrackedTextureBatchSlot_t &rSlot );
	void clearAsyncTextureOperationPayload( t3dAsyncTextureOperationSlot_t &rSlot );
	uint64_t nextAsyncTextureOperationGeneration( void );
	bool submitAsyncTextureOperation( const t3dRdTextureOperationRequest_t &rRequest );
	bool updateOwnedTexture( Terrain3DRdTexture *pTexture, uint32_t nGeneration, uint32_t nLayer, const PackedByteArray &rData );
	bool updateOwnedTextureRegion( Terrain3DRdTexture *pTexture, uint32_t nTextureGeneration,
	        Terrain3DRdTexture *pStaging, uint32_t nStagingGeneration,
	        uint32_t nDestinationX, uint32_t nDestinationY, uint32_t nWidth, uint32_t nHeight,
	        const PackedByteArray &rData );
	void renderCreateTexture( void );
	void renderAsyncTextureOperation( uint32_t nSlot, uint64_t nGeneration );
	void renderTrackedTextureLayerBatch( uint32_t nSlot, uint64_t nGeneration );
	void renderFreeTexture( void );
	void renderShutdown( void );

protected:
	static void _bind_methods( void ) {
	}

public:
	static EA_FORCE_INLINE uint32_t getTrackedBatchSlotCount( void ) {
		return T3D_TRACKED_BATCH_SLOT_COUNT;
	}
	static EA_FORCE_INLINE uint32_t getTrackedBatchUpdateCount( void ) {
		return T3D_TRACKED_BATCH_UPDATE_COUNT;
	}

	static void initializeSingleton( void );
	static void shutdownSingleton( void );
	static Terrain3DRdTextureManager *getSingleton( void ) {
		return s_pSingleton;
	}

	static bool imageFormatToDataFormat( Image::Format format, t3dRdTextureSemantic_t semantic, RenderingDevice::DataFormat &rFormat );
	static uint32_t getImageMipmapCount( const Ref<Image> &rImage );
	static bool buildLayout( const Ref<Image> &rImage,
	        uint32_t nLayers,
	        bool bLayered,
	        uint64_t nPayloadBytes,
	        t3dRdTextureSemantic_t semantic,
	        t3dRdTextureLayout_t &rLayout );

	bool createTexture( Terrain3DRdTexture &rTexture, const t3dRdTextureLayout_t &rLayout,
	        const TypedArray<PackedByteArray> &rInitialData,
	        const String &rName,
	        int nMemoryCategory = -1,
	        bool bFailureFatal = true );
	bool updateTexture( Terrain3DRdTexture &rTexture, uint32_t nLayer, const PackedByteArray &rData );
	uint64_t nextTrackedTextureBatchGeneration( void );
	bool submitTrackedTextureLayerBatch(
	        uint32_t nSlot, uint64_t nGeneration, uint32_t nLayer, const t3dRdTextureBatchUpdate_t *pUpdates, uint32_t nUpdateCount );
	t3dRdTextureBatchState_t getTrackedTextureLayerBatchState( uint32_t nSlot, uint64_t nGeneration ) const;
	bool updateTextureRegion( Terrain3DRdTexture &rTexture,
	        Terrain3DRdTexture &rStaging,
	        uint32_t nDestinationX,
	        uint32_t nDestinationY,
	        uint32_t nWidth,
	        uint32_t nHeight,
	        const PackedByteArray &rData );
	void freeTexture( Terrain3DRdTexture &rTexture );
	Dictionary getStats( void ) const;
};

#endif // TERRAIN3D_RD_TEXTURE_MANAGER_H
