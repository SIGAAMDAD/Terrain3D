// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_TEXTURE_MANAGER_H
#define TERRAIN3D_RD_TEXTURE_MANAGER_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"

#include <atomic>
#include <cstdint>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <mutex>

using namespace godot;

typedef enum t3dRdTextureSemantic_e : uint8_t {
	T3D_RD_TEXTURE_LINEAR_DATA = 0,
	T3D_RD_TEXTURE_SOURCE_COLOR = 1,
} t3dRdTextureSemantic_t;

typedef struct t3dRdTextureLayout_s
{
	RenderingDevice::DataFormat m_format = RenderingDevice::DATA_FORMAT_MAX;
	RenderingDevice::TextureType m_type = RenderingDevice::TEXTURE_TYPE_2D;
	uint32_t m_nWidth = 0u;
	uint32_t m_nHeight = 0u;
	uint32_t m_nLayers = 0u;
	uint32_t m_nMipmaps = 0u;
	uint64_t m_nUsageBits = 0u;
	uint64_t m_nBytes = 0u;

	EA_FORCE_INLINE bool valid( void ) const
	{
		return m_format != RenderingDevice::DATA_FORMAT_MAX && m_nWidth > 0u && m_nHeight > 0u && m_nLayers > 0u &&
				m_nMipmaps > 0u && m_nBytes > 0u;
	}
} t3dRdTextureLayout_t;

// Global-RD texture gateway for Terrain3D-owned generated resources. Creation
// and destruction are serialized onto Godot's render thread; ordinary dirty
// layer/subregion uploads are queued without a frame-thread force_sync.
class Terrain3DRdTextureManager : public Object
{
	GDCLASS( Terrain3DRdTextureManager, Object );
	CLASS_NAME_STATIC( "Terrain3DRdTextureManager" );

private:
	static Terrain3DRdTextureManager *s_pSingleton;
	std::mutex m_createMutex;
	std::mutex m_resultMutex;
	RID m_hCreateResult;
	bool m_bCreateSucceeded = false;
	std::atomic_uint64_t m_nCreateCount{ 0u };
	std::atomic_uint64_t m_nUpdateCount{ 0u };
	std::atomic_uint64_t m_nRegionUpdateCount{ 0u };
	std::atomic_uint64_t m_nUploadedBytes{ 0u };

	void renderCreateTexture( const Ref<RDTextureFormat>& rFormat,
			const TypedArray<PackedByteArray>& rInitialData,
			const String& rName,
			uint64_t nBytes,
			int nMemoryCategory );
	void renderUpdateTexture( RID hTexture, uint32_t nLayer, const PackedByteArray& rData );
	void renderUpdateTextureRegion( RID hTexture,
			RID hStaging,
			uint32_t nDestinationX,
			uint32_t nDestinationY,
			uint32_t nWidth,
			uint32_t nHeight,
			const PackedByteArray& rData );
	void renderFreeTexture( RID hTexture, uint64_t nBytes );
	void renderFreeResource( RID hRid, uint64_t nBytes );
	void renderShutdown( void );

protected:
	static void _bind_methods( void ) {}

public:
	static void initializeSingleton( void );
	static void shutdownSingleton( void );
	static Terrain3DRdTextureManager *getSingleton( void )
	{
		return s_pSingleton;
	}

	static bool imageFormatToDataFormat( Image::Format format,
			t3dRdTextureSemantic_t semantic,
			RenderingDevice::DataFormat& rFormat );
	static uint32_t getImageMipmapCount( const Ref<Image>& rImage );
	static bool buildLayout( const Ref<Image>& rImage,
			uint32_t nLayers,
			bool bLayered,
			uint64_t nPayloadBytes,
			t3dRdTextureSemantic_t semantic,
			t3dRdTextureLayout_t& rLayout );

	RID createTexture( const t3dRdTextureLayout_t& rLayout,
			const TypedArray<PackedByteArray>& rInitialData,
			const String& rName,
			int nMemoryCategory = -1 );
	bool updateTexture( RID hTexture, uint32_t nLayer, const PackedByteArray& rData );
	bool updateTextureRegion( RID hTexture,
			RID hStaging,
			uint32_t nDestinationX,
			uint32_t nDestinationY,
			uint32_t nWidth,
			uint32_t nHeight,
			const PackedByteArray& rData );
	void freeTexture( RID hTexture, uint64_t nBytes );
	void freeResource( RID hRid, uint64_t nBytes );
	Dictionary getStats( void ) const;
};

#endif // TERRAIN3D_RD_TEXTURE_MANAGER_H
