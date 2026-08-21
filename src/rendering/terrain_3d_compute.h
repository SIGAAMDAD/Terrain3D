// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_COMPUTE_H
#define TERRAIN3D_COMPUTE_H

#include <cstdint>
#include <mutex>
#include "eastl_config.h"
#include <EASTL/vector.h>

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector3.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Compute acceleration has two explicit device domains:
// - a local RenderingDevice for CPU-authoritative brush/derived jobs that require readback;
// - the renderer-owned global RenderingDevice for render-thread displacement generation.
// The caller retains terrain mutation ownership and publishes results through the
// existing Terrain3D transactions.
class T3DTerrain;

typedef struct t3dBrushElement_s {
	uint32_t value = 0u;  // Height float bits, packed pControl, or RGBA8.
	uint32_t alpha = 0u;  // Float bits.
	uint32_t target = 0u; // Gradient/height target float bits.
	uint32_t flags = 0u;  // Bit 0: eligible.
} t3dBrushElement_t;

typedef struct t3dDisplacementRequest_s {
	// RenderingServer texture RIDs. The render callback resolves their
	// global RenderingDevice texture RIDs on the render thread.
	RID heightTexture;
	RID controlTexture;
	RID displacementTexture;
	RID materialRid;
	Ref<RDShaderSPIRV> pShaderSpirv;
	PackedInt32Array regionMap;
	PackedByteArray materialData;
	PackedByteArray pushConstants;
	uint64_t regionMapHash = 0u;
	uint64_t materialDataHash = 0u;
	uint32_t nWidth = 0u;
	uint32_t nHeight = 0u;
	uint32_t nRegionMapSize = 0u;

	bool valid( void ) const {
		return pShaderSpirv.is_valid() && heightTexture.is_valid() && controlTexture.is_valid() &&
			   displacementTexture.is_valid() && materialRid.is_valid() &&
			   nWidth > 0u && nHeight > 0u && nRegionMapSize > 0u && nRegionMapSize <= 64u &&
			   regionMap.size() == ( int64_t ) ( nRegionMapSize ) * ( int64_t ) ( nRegionMapSize ) && !materialData.is_empty() &&
			   !pushConstants.is_empty();
	}
} t3dDisplacementRequest_t;

typedef struct t3dBrushParams_s {
	uint32_t mapType = 0u;
	uint32_t tool = 0u;
	uint32_t operation = 0u;
	uint32_t nCount = 0u;
	float fStrength = 0.0f;
	float fSelectedHeight = 0.0f;
	float fBrushCenterHeight = 0.0f;
	float fRoughness = 0.0f;
	uint32_t colorRgba8 = 0xffffffffu;
	uint32_t assetId = 0u;
	uint32_t uvRotation = 0u;
	uint32_t uvScale = 0u;
	uint32_t featureFlags = 0u;
} t3dBrushParams_t;

typedef struct t3dPipeline_s {
	RID pShader;
	RID pipeline;
	bool bAttempted = false;
} t3dPipeline_t;

class T3DCompute {
public:






	T3DCompute( void ) = default;
	~T3DCompute( void );

	T3DCompute( const T3DCompute & ) = delete;
	T3DCompute &operator=( const T3DCompute & ) = delete;

	bool isAvailable( void );
	EA_FORCE_INLINE String getLastError( void ) const { std::lock_guard<std::mutex> lock( m_errorMutex ); return m_lastError; }

	bool processBrush( eastl::vector<t3dBrushElement_t> &rElements, const t3dBrushParams_t &rParams );
	static void processBrushCpu( eastl::vector<t3dBrushElement_t> &rElements, const t3dBrushParams_t &rParams );
	bool generateDerivedMap(
		const eastl::vector<float> &fHeightHalo, int nWidth, int nHeight,
		float fVertexSpacing, PackedByteArray &rRgba8
	);
	// Builds a render-thread-safe snapshot without touching the global RenderingDevice.
	// The raw GLSL path is intentionally restricted to non-projected, non-detiled
	// packed-PBR/R8 materials; all other variants retain the raster fallback.
	bool prepareDisplacementRequest(
		T3DTerrain *pTerrain, const Vector3 &rTargetPosition,
		t3dDisplacementRequest_t &rRequest
	);
	// Must execute through RenderingServer.call_on_render_thread().
	bool dispatchDisplacementRequest( const t3dDisplacementRequest_t &rRequest, RID &rTexture );
	// Must execute on the render thread before T3DCompute is destroyed.
	void shutdownDisplacementResources( void );

private:


	RenderingDevice *m_pRd = nullptr;		   // Owned local device for readback workloads.
	RenderingDevice *m_pRenderRd = nullptr; // Non-owning global renderer device.
	t3dPipeline_t m_brushPipeline;
	t3dPipeline_t m_derivedPipeline;
	t3dPipeline_t m_displacementPipeline;
	Ref<RDShaderSPIRV> m_pDisplacementSpirv;
	RID m_brushBuffer;
	RID m_derivedInputBuffer;
	RID m_derivedOutputBuffer;
	RID m_brushUniformSet;
	RID m_derivedUniformSet;
	RID m_displacementUniformSet;
	RID m_displacementRegionBuffer;
	RID m_displacementMaterialBuffer;
	RID m_displacementOutputTexture;
	RID m_displacementRsTexture;
	RID m_displacementBoundHeight;
	RID m_displacementBoundControl;
	RID m_displacementBoundDisplacement;
	RID m_nearestSampler;
	RID m_linearRepeatSampler;
	uint64_t m_nDisplacementRegionHash = 0u;
	uint64_t m_displacementMaterialHash = 0u;
	bool m_bDisplacementRegionUploaded = false;
	bool m_bDisplacementMaterialUploaded = false;
	uint32_t m_nDisplacementWidth = 0u;
	uint32_t m_nDisplacementHeight = 0u;
	uint32_t m_nBrushCapacity = 0u;
	uint32_t m_nDerivedInputCapacity = 0u;
	uint32_t m_nDerivedOutputCapacity = 0u;
	mutable std::mutex m_errorMutex;
	String m_lastError;
	bool m_bDeviceAttempted = false;
	bool m_bRenderDeviceAttempted = false;

	bool ensureDevice( void );
	bool ensureRenderDevice( void );
	bool ensurePipeline( t3dPipeline_t &rPipeline, const String &rPath, const String &rName );
	bool ensureRenderPipeline( t3dPipeline_t &rPipeline, const Ref<RDShaderSPIRV> &rSpirv, const String &rName );
	bool ensureBrushCapacity( uint32_t nBytes );
	bool ensureDerivedCapacity( uint32_t nInputBytes, uint32_t nOutputBytes );
	bool ensureDisplacementResources( uint32_t nWidth, uint32_t nHeight, const Ref<RDShaderSPIRV> &rSpirv );
	void freeUniformSet( RID &rUniformSet );
	void freeBuffer( RID &rBuffer );
	void freePipeline( t3dPipeline_t &rPipeline );
	void freeRenderUniformSet( RID &rUniformSet );
	void freeRenderRid( RID &rRid );
	void freeRenderPipeline( t3dPipeline_t &rPipeline );
	void freeDisplacementTexture( void );
	void shutdown( void );
	EA_FORCE_INLINE void setError( const String &rError ) { std::lock_guard<std::mutex> lock( m_errorMutex ); m_lastError = rError; }
	static uint32_t growthCapacity( uint32_t nRequired );
	static PackedByteArray copyToBytes( const void *pData, size_t nSize );
};

#endif // TERRAIN3D_COMPUTE_H
