// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_COMPUTE_H
#define TERRAIN3D_COMPUTE_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "core/terrain_3d_eastl_helpers.h"
#include "eastl_config.h"

#include <EASTL/vector.h>
#include <stdint.h>
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
class Terrain3D;

typedef struct t3dBrushElement_s {
	uint32_t m_nValue = 0u;  // Height float bits, packed pControl, or RGBA8.
	uint32_t m_nAlpha = 0u;  // Float bits.
	uint32_t m_nTarget = 0u; // Gradient/height m_nTarget float bits.
	uint32_t m_nFlags = 0u;  // Bit 0: eligible.
} t3dBrushElement_t;

typedef struct t3dDisplacementRequest_s {
	// Direct global RenderingDevice texture RIDs published by Terrain3D generated resources.
	RID m_hHeightTexture;
	RID m_hControlTexture;
	RID m_hDisplacementTexture;
	Ref<RDShaderSPIRV> m_shaderSpirv;
	PackedInt32Array m_regionMap;
	PackedByteArray m_materialData;
	PackedByteArray m_pushConstants;
	uint64_t m_nRegionMapHash = 0u;
	uint64_t m_nMaterialDataHash = 0u;
	uint32_t m_nWidth = 0u;
	uint32_t m_nHeight = 0u;

	bool valid( void ) const {
		return m_shaderSpirv.is_valid() && m_hHeightTexture.is_valid() && m_hControlTexture.is_valid() && m_hDisplacementTexture.is_valid()
		        && m_nWidth > 0u && m_nHeight > 0u
		        && m_regionMap.size() == s_nRegionSlots && !m_materialData.is_empty()
		        && !m_pushConstants.is_empty();
	}
} t3dDisplacementRequest_t;

typedef struct t3dBrushParams_s {
	uint32_t m_nMapType = 0u;
	uint32_t m_nTool = 0u;
	uint32_t m_nOperation = 0u;
	uint32_t m_nCount = 0u;
	float m_fStrength = 0.0f;
	float m_fSelectedHeight = 0.0f;
	float m_fBrushCenterHeight = 0.0f;
	float m_fRoughness = 0.0f;
	uint32_t m_nColorRgba8 = 0xffffffffu;
	uint32_t m_nAssetId = 0u;
	uint32_t m_nUvRotation = 0u;
	uint32_t m_nUvScale = 0u;
	uint32_t m_nFeatureFlags = 0u;
} t3dBrushParams_t;

typedef struct t3dDerivedMapComputeJob_s {
	const float *m_pHeightHalo = NULL;
	uint8_t *m_pRgbaOutput = NULL;
	uint32_t m_nHaloElements = 0u;
	uint32_t m_nOutputBytes = 0u;
	uint32_t m_nWidth = 0u;
	uint32_t m_nHeight = 0u;
	float m_fVertexSpacing = 1.0f;
	bool m_bSuccess = false;
} t3dDerivedMapComputeJob_t;

typedef struct t3dPipeline_s {
	RID m_hShader;
	RID m_hPipeline;
	bool m_bAttempted = false;
} t3dPipeline_t;

class Terrain3DCompute {
public:
	Terrain3DCompute( void ) = default;
	~Terrain3DCompute( void );

	Terrain3DCompute( const Terrain3DCompute & ) = delete;
	Terrain3DCompute &operator=( const Terrain3DCompute & ) = delete;

	bool isAvailable( void );
	EA_FORCE_INLINE String getLastError( void ) const {
		t3dScopedLock_t lock( m_errorMutex );
		return m_lastError;
	}

	bool processBrush( eastl::vector<t3dBrushElement_t, Terrain3DEastlAllocator> &rElements, const t3dBrushParams_t &rParams );
	static void processBrushCpu( eastl::vector<t3dBrushElement_t, Terrain3DEastlAllocator> &rElements, const t3dBrushParams_t &rParams );
	bool generateDerivedMaps( t3dDerivedMapComputeJob_t *pJobs, uint32_t nJobCount );
	// Builds a render-thread-safe snapshot without touching the global RenderingDevice.
	// The raw GLSL path is intentionally restricted to non-projected, non-detiled
	// packed-PBR/R8 materials; all other variants retain the raster fallback.
	bool prepareDisplacementRequest( Terrain3D *pTerrain, const Vector3 &rTargetPosition, t3dDisplacementRequest_t &rRequest );
	// Must execute through the render-thread gateway.
	bool dispatchDisplacementRequest( const t3dDisplacementRequest_t &rRequest, RID &rTexture );
	// Must execute on the render thread before Terrain3DCompute is destroyed.
	void shutdownDisplacementResources( void );
	EA_FORCE_INLINE uint64_t getRetainedLocalGpuBufferBytes( void ) const {
		uint64_t bytes = m_hBrushBuffer.is_valid() ? m_nBrushCapacity : 0u;
		for ( uint32_t slot = 0u; slot < s_nDerivedReadbackSlotCount; ++slot ) {
			if ( m_szDerivedInputBuffer[slot].is_valid() ) {
				bytes += m_nszDerivedInputCapacity[slot];
			}
			if ( m_szDerivedOutputBuffer[slot].is_valid() ) {
				bytes += m_nszDerivedOutputCapacity[slot];
			}
		}
		return bytes;
	}

private:
	RenderingDevice *m_pRd = NULL;       // Owned local device for readback workloads.
	RenderingDevice *m_pRenderRd = NULL; // Non-owning global renderer device.
	t3dPipeline_t m_brushPipeline;
	t3dPipeline_t m_derivedPipeline;
	t3dPipeline_t m_displacementPipeline;
	Ref<RDShaderSPIRV> m_displacementSpirv;
	RID m_hBrushBuffer;

	RID m_szDerivedInputBuffer[s_nDerivedReadbackSlotCount];
	RID m_szDerivedOutputBuffer[s_nDerivedReadbackSlotCount];
	RID m_hBrushUniformSet;
	RID m_szDerivedUniformSet[s_nDerivedReadbackSlotCount];
	RID m_hDisplacementUniformSet;
	RID m_hDisplacementRegionBuffer;
	RID m_hDisplacementMaterialBuffer;
	RID m_hDisplacementOutputTexture;
	RID m_hDisplacementBoundHeight;
	RID m_hDisplacementBoundControl;
	RID m_hDisplacementBoundDisplacement;
	RID m_hNearestSampler;
	RID m_hLinearRepeatSampler;
	uint64_t m_nDisplacementRegionHash = 0u;
	uint64_t m_nDisplacementMaterialHash = 0u;
	bool m_bDisplacementRegionUploaded = false;
	bool m_bDisplacementMaterialUploaded = false;
	bool m_bPersistentDisplacementBuffers = false;
	uint32_t m_nDisplacementWidth = 0u;
	uint32_t m_nDisplacementHeight = 0u;
	uint32_t m_nBrushCapacity = 0u;
	uint32_t m_nszDerivedInputCapacity[s_nDerivedReadbackSlotCount]{};
	uint32_t m_nszDerivedOutputCapacity[s_nDerivedReadbackSlotCount]{};
	mutable t3dMutex_t m_errorMutex;
	String m_lastError;
	bool m_bDeviceAttempted = false;
	bool m_bRenderDeviceAttempted = false;

	bool ensureDevice( void );
	bool ensureRenderDevice( void );
	bool ensurePipeline( t3dPipeline_t &rPipeline, const String &rPath, const String &rName );
	bool ensureRenderPipeline( t3dPipeline_t &rPipeline, const Ref<RDShaderSPIRV> &rSpirv, const String &rName );
	bool ensureBrushCapacity( uint32_t nBytes );
	bool ensureDerivedCapacity( uint32_t nSlot, uint32_t nInputBytes, uint32_t nOutputBytes );
	bool ensureDisplacementResources( uint32_t nWidth, uint32_t nHeight, const Ref<RDShaderSPIRV> &rSpirv );
	void freeUniformSet( RID &rUniformSet );
	void freeBuffer( RID &rBuffer );
	void freePipeline( t3dPipeline_t &rPipeline );
	void freeRenderUniformSet( RID &rUniformSet );
	void freeRenderRid( RID &rRid );
	void freeRenderPipeline( t3dPipeline_t &rPipeline );
	void freeDisplacementTexture( void );
	void shutdown( void );
	EA_FORCE_INLINE void setError( const String &rError ) {
		t3dScopedLock_t lock( m_errorMutex );
		m_lastError = rError;
	}
	static uint32_t growthCapacity( uint32_t nRequired );
	static PackedByteArray copyToBytes( const void *pData, size_t nSize );
};

#endif // TERRAIN3D_COMPUTE_H
