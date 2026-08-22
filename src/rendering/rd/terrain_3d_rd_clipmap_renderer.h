// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_CLIPMAP_RENDERER_H
#define TERRAIN3D_RD_CLIPMAP_RENDERER_H

#include "rendering/rd/terrain_3d_rd_types.h"
#include "rendering/terrain_3d_mesher.h"

#include <atomic>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <mutex>

namespace godot {
class RenderData;
class RDShaderSPIRV;
}

class Terrain3DRdClipmapRenderer {
private:
	RenderingDevice *m_pRd = nullptr;
	RID m_hShader;
	RID m_hTerrainSampler;
	RID m_hMaterialSampler;
	RID m_hFrameUniformBuffer;
	RID m_hRegionMapBuffer;
	RID m_hUniformSet;
	RID m_hFramebuffer;
	RID m_hPipeline;
	RID m_hBoundHeightTexture;
	RID m_hBoundControlTexture;
	RID m_hBoundColorTexture;
	RID m_hBoundAlbedoTexture;
	RID m_hFramebufferColor;
	RID m_hFramebufferDepth;
	RenderingDevice::TextureSamples m_framebufferSamples = RenderingDevice::TEXTURE_SAMPLES_1;
	uint32_t m_nFramebufferViewCount = 0u;
	uint64_t m_nRegionMapGeneration = 0u;
	uint32_t m_nRegionMapCapacityBytes = 0u;
	std::atomic_uint64_t m_nFramesRendered{ 0u };
	std::atomic_uint64_t m_nDrawsRendered{ 0u };
	std::atomic_uint64_t m_nTopologyBytes{ 0u };
	mutable std::mutex m_diagnosticsMutex;
	String m_lastError;
	PackedByteArray m_frameUploadBytes;
	PackedByteArray m_pushConstantBytes;

	typedef struct t3dRdTopology_s {
		RID m_hVertexBuffer;
		RID m_hVertexArray;
		RID m_hIndexBuffer;
		RID m_hIndexArray;
		uint32_t m_nVertexCount = 0u;
		uint32_t m_nIndexCount = 0u;
	} t3dRdTopology_t;
	t3dRdTopology_t m_szTopology[MESH_TYPE_COUNT]{};
	int64_t m_nVertexFormat = RenderingDevice::INVALID_FORMAT_ID;

	void setError( const String &rError );
	void freeRid( RID &rRid );
	void freeFramebufferState( void );
	void freeUniformSet( void );
	bool ensureDevice( void );
	bool ensureShader( const Ref<RDShaderSPIRV> &rSpirv );
	bool ensureSamplers( void );
	bool ensureFrameUniformBuffer( void );
	bool ensureRegionMapBuffer( uint32_t nRequiredBytes );
	bool ensureUniformSet( const RID &rHeightTexture, const RID &rControlTexture, const RID &rColorTexture,
			const RID &rAlbedoTexture );
	bool ensureFramebufferAndPipeline( const RID &rColor,
			const RID &rDepth,
			RenderingDevice::TextureSamples samples,
			uint32_t nViewCount );
	bool createTopology( int nType, const PackedByteArray &rVertexBytes, uint32_t nVertexCount,
			const PackedByteArray &rIndexBytes, uint32_t nIndexCount );
	void copyProjection( const Projection &rProjection, float *pDestination ) const;

public:
	Terrain3DRdClipmapRenderer( void ) = default;
	~Terrain3DRdClipmapRenderer( void );

	bool initialize( const Ref<RDShaderSPIRV> &rSpirv,
			const PackedByteArray &rLod0Vertices,
			uint32_t nLod0VertexCount,
			const PackedByteArray &rLod0Indices,
			uint32_t nLod0IndexCount,
			const PackedByteArray &rOuterVertices,
			uint32_t nOuterVertexCount,
			const PackedByteArray &rOuterIndices,
			uint32_t nOuterIndexCount );
	void shutdown( void );
	bool render( RenderData *pRenderData,
			const t3dRdFrameSnapshot_t &rSnapshot,
			const PackedByteArray &rRegionMapBytes,
			uint64_t nRegionMapGeneration,
			const RID &rHeightTextureRs,
			const RID &rControlTextureRs,
			const RID &rColorTextureRs,
			const RID &rAlbedoTextureRs );

	EA_FORCE_INLINE bool isReady( void ) const {
		return m_hShader.is_valid() && m_szTopology[LOD0_MESH].m_hVertexArray.is_valid() &&
				m_szTopology[OUTER_RING_MESH].m_hVertexArray.is_valid();
	}
	EA_FORCE_INLINE uint64_t getFramesRendered( void ) const { return m_nFramesRendered.load( std::memory_order_acquire ); }
	EA_FORCE_INLINE uint64_t getDrawsRendered( void ) const { return m_nDrawsRendered.load( std::memory_order_acquire ); }
	EA_FORCE_INLINE uint64_t getTopologyBytes( void ) const { return m_nTopologyBytes.load( std::memory_order_acquire ); }
	String getLastError( void ) const;
};

#endif // TERRAIN3D_RD_CLIPMAP_RENDERER_H
