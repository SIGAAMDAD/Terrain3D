// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_TEXTURE_MANAGER_H
#define TERRAIN3D_RD_TEXTURE_MANAGER_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "core/terrain_3d_compiler.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_texture.h"

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

typedef enum t3dRdTextureSemantic_e : uint8_t {
	T3D_RD_TEXTURE_LINEAR_DATA = 0,
	T3D_RD_TEXTURE_SOURCE_COLOR = 1,
} t3dRdTextureSemantic_t;

typedef enum t3dRdTextureBatchState_e : uint8_t {
	T3D_RD_TEXTURE_BATCH_UNUSED = 0u,
	T3D_RD_TEXTURE_BATCH_PENDING = 1u,
	T3D_RD_TEXTURE_BATCH_SUCCEEDED = 2u,
	T3D_RD_TEXTURE_BATCH_FAILED = 3u,
	T3D_RD_TEXTURE_BATCH_WRITING = 4u,
} t3dRdTextureBatchState_t;

typedef struct t3dRdTextureBatchUpdate_s {
	Terrain3DRdTexture *m_pTexture = NULL;
	uint32_t m_nTextureGeneration = 0u;
	PackedByteArray m_data;
} t3dRdTextureBatchUpdate_t;

// Global-RD scheduling/batching authority for Terrain3D-generated textures.
// Terrain3DRdTexture owns physical texture identity, layout, tracked creation,
// I/O, and release. This manager only serializes cross-thread calls against
// canonical texture owners, tracked batches, Image-to-layout conversion, and
// telemetry. It never stores or mutates a texture RID directly.
class Terrain3DRdTextureManager : public Object {
	GDCLASS( Terrain3DRdTextureManager, Object );
	CLASS_NAME_STATIC( "Terrain3DRdTextureManager" );

private:
	typedef enum t3dRdTextureAsyncKind_e : uint8_t {
		T3D_RD_TEXTURE_ASYNC_NONE = 0u,
		T3D_RD_TEXTURE_ASYNC_UPDATE = 1u,
		T3D_RD_TEXTURE_ASYNC_REGION_UPDATE = 2u,
	} t3dRdTextureAsyncKind_t;
	typedef enum t3dRdTextureAsyncState_e : uint8_t {
		T3D_RD_TEXTURE_ASYNC_UNUSED = 0u,
		T3D_RD_TEXTURE_ASYNC_WRITING = 1u,
		T3D_RD_TEXTURE_ASYNC_PENDING = 2u,
	} t3dRdTextureAsyncState_t;
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
		Terrain3DRdTexture *m_szTextures[s_nTrackedBatchUpdateCount]{};
		uint32_t m_szTextureGenerations[s_nTrackedBatchUpdateCount]{};
		PackedByteArray m_szData[s_nTrackedBatchUpdateCount];
		uint32_t m_nLayer = 0u;
		uint32_t m_nCount = 0u;
	} t3dTrackedTextureBatchSlot_t;

	static Terrain3DRdTextureManager *s_pSingleton;
	t3dMutex_t m_createMutex;
	t3dMutex_t m_resultMutex;
	t3dMutex_t m_freeMutex;
	Terrain3DRdTexture m_createResult;
	t3dRdTextureSpec_t m_createSpec;
	TypedArray<PackedByteArray> m_createInitialData;
	bool m_bCreateSucceeded = false;
	Terrain3DRdTexture *m_pFreeTexture = NULL;
	t3dAtomicU64_t m_nCreateCount{ 0u };
	t3dAtomicU64_t m_nUpdateCount{ 0u };
	t3dAtomicU64_t m_nRegionUpdateCount{ 0u };
	t3dAtomicU64_t m_nUploadedBytes{ 0u };
	t3dAtomicU64_t m_nAsyncOperationGeneration{ 1u };
	t3dAtomicU32_t m_nAsyncOperationCursor{ 0u };
	t3dAtomicU64_t m_nTrackedBatchGeneration{ 1u };
	t3dAsyncTextureOperationSlot_t m_szAsyncOperations[s_nTrackedBatchSlotCount];
	t3dTrackedTextureBatchSlot_t m_szTrackedBatches[s_nTrackedBatchSlotCount];

	void clearTrackedTextureBatchPayload( t3dTrackedTextureBatchSlot_t &rSlot );
	void clearAsyncTextureOperationPayload( t3dAsyncTextureOperationSlot_t &rSlot );
	uint64_t nextAsyncTextureOperationGeneration( void );
	bool submitAsyncTextureOperation( t3dRdTextureAsyncKind_t kind,
	        Terrain3DRdTexture *pTexture, uint32_t nTextureGeneration,
	        Terrain3DRdTexture *pStaging, uint32_t nStagingGeneration,
	        uint32_t nLayer, uint32_t nDestinationX, uint32_t nDestinationY,
	        uint32_t nWidth, uint32_t nHeight, const PackedByteArray &rData );
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
		return s_nTrackedBatchSlotCount;
	}
	static EA_FORCE_INLINE uint32_t getTrackedBatchUpdateCount( void ) {
		return s_nTrackedBatchUpdateCount;
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
