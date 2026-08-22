// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_CLIPMAP_RENDERER_H
#define TERRAIN3D_RD_CLIPMAP_RENDERER_H

#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/terrain_3d_rd_topology.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <atomic>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <mutex>

namespace godot {
class RenderData;
}

class Terrain3DRdClipmapRenderer {
private:
	static constexpr uint32_t s_nMaxViews = 4u;

	typedef struct t3dRdViewState_s {
		RID m_hFramebuffer;
		RID m_hszPipelines[3];
		RID m_hColor;
		RID m_hDepth;
		RenderingDevice::TextureSamples m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
		bool m_bColorWrite = true;
	} t3dRdViewState_t;

	Terrain3DRdContext m_context;
	Terrain3DRdTopology m_szTopology[MESH_TYPE_COUNT];
	RID m_hszShaders[3];
	RID m_hCullShader;
	RID m_hCullPipeline;
	RID m_hTerrainSampler;
	RID m_hMaterialSampler;
	RID m_hFallbackDisplacementTexture;
	RID m_hFrameUniformBuffer;
	RID m_hRegionMapBuffer;
	RID m_hMaterialBuffer;
	RID m_hPatchBuffer;
	RID m_hVisibleBuffer;
	RID m_hIndirectBuffer;
	RID m_hszRasterUniformSets[3];
	RID m_hCullUniformSet;
	RID m_hBoundHeightTexture;
	RID m_hBoundControlTexture;
	RID m_hBoundColorTexture;
	RID m_hBoundAlbedoTexture;
	RID m_hBoundNormalTexture;
	RID m_hBoundSurfaceTexture;
	RID m_hBoundDisplacementTexture;
	t3dRdViewState_t m_szViewStates[s_nMaxViews]{};
	int64_t m_nVertexFormat = RenderingDevice::INVALID_FORMAT_ID;
	uint32_t m_nRegionMapCapacityBytes = 0u;
	uint64_t m_nRegionMapGeneration = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nPatchGeneration = 0u;
	bool m_bGpuCullingReady = false;
	std::atomic_uint64_t m_nFramesRendered{ 0u };
	std::atomic_uint64_t m_nDrawsRendered{ 0u };
	std::atomic_uint64_t m_nCullDispatches{ 0u };
	std::atomic_uint64_t m_nIndirectDraws{ 0u };
	std::atomic_uint64_t m_nPatchUploads{ 0u };
	mutable std::mutex m_diagnosticsMutex;
	String m_lastError;
	PackedByteArray m_frameUploadBytes;
	PackedByteArray m_materialUploadBytes;
	PackedByteArray m_patchUploadBytes;
	PackedByteArray m_cullPushBytes;
	PackedByteArray m_cpuVisibleBytes;
	PackedByteArray m_cpuIndirectBytes;

	void setError( const String &rError );
	void clearError( void );
	void freeViewState( t3dRdViewState_t &rState );
	void freeUniformSets( void );
	bool ensureDevice( void );
	bool ensureShaders( const Ref<RDShaderSPIRV> &rNear,
			const Ref<RDShaderSPIRV> &rMiddle,
			const Ref<RDShaderSPIRV> &rFar,
			const Ref<RDShaderSPIRV> &rCull );
	bool ensureSamplers( void );
	bool ensureFallbackTextures( void );
	bool ensureBuffers( void );
	bool ensureRegionMapBuffer( uint32_t nRequiredBytes );
	bool ensureUniformSets( const RID &rHeight,
			const RID &rControl,
			const RID &rColor,
			const RID &rAlbedo,
			const RID &rNormal,
			const RID &rSurface,
			const RID &rDisplacement );
	bool ensureViewState( uint32_t nView,
			const RID &rColor,
			const RID &rDepth,
			RenderingDevice::TextureSamples samples,
			bool bColorWrite );
	bool updateFrameUniforms( const t3dRdFrameSnapshot_t &rSnapshot, const Projection &rViewProjection, const Vector3 &rCamera );
	bool uploadSnapshotBuffers( const t3dRdFrameSnapshot_t &rSnapshot,
			const PackedByteArray &rRegionMapBytes,
			uint64_t nRegionMapGeneration );
	bool runGpuCulling( const t3dRdFrameSnapshot_t &rSnapshot );
	bool buildCpuIndirect( const t3dRdFrameSnapshot_t &rSnapshot );
	bool drawView( uint32_t nView );
	void copyProjection( const Projection &rProjection, float *pDestination ) const;

public:
	Terrain3DRdClipmapRenderer( void ) = default;
	~Terrain3DRdClipmapRenderer( void );

	bool prepareTopologies( const t3dClipmapGeometry_t &rLod0, const t3dClipmapGeometry_t &rOuter );
	bool initialize( const Ref<RDShaderSPIRV> &rNear,
			const Ref<RDShaderSPIRV> &rMiddle,
			const Ref<RDShaderSPIRV> &rFar,
			const Ref<RDShaderSPIRV> &rCull );
	void shutdown( void );
	bool render( RenderData *pRenderData,
			const t3dRdFrameSnapshot_t &rSnapshot,
			const PackedByteArray &rRegionMapBytes,
			uint64_t nRegionMapGeneration,
			const RID &rHeightTextureRs,
			const RID &rControlTextureRs,
			const RID &rColorTextureRs,
			const RID &rAlbedoTextureRs,
			const RID &rNormalTextureRs,
			const RID &rSurfaceTextureRs,
			const RID &rDisplacementTextureRs,
			bool bColorWrite );
	Dictionary getDiagnostics( void ) const;

	EA_FORCE_INLINE bool isReady( void ) const {
		return m_hszShaders[0].is_valid() && m_hszShaders[1].is_valid() && m_hszShaders[2].is_valid() &&
				m_szTopology[LOD0_MESH].isGpuReady() && m_szTopology[OUTER_RING_MESH].isGpuReady();
	}
	EA_FORCE_INLINE uint64_t getFramesRendered( void ) const { return m_nFramesRendered.load( std::memory_order_acquire ); }
	EA_FORCE_INLINE uint64_t getDrawsRendered( void ) const { return m_nDrawsRendered.load( std::memory_order_acquire ); }
	EA_FORCE_INLINE uint64_t getTopologyBytes( void ) const {
		return m_szTopology[LOD0_MESH].getGpuBytes() + m_szTopology[OUTER_RING_MESH].getGpuBytes();
	}
	String getLastError( void ) const;
};

#endif // TERRAIN3D_RD_CLIPMAP_RENDERER_H
