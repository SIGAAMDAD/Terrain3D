// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_COMPUTE_H
#define TERRAIN3D_COMPUTE_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_owner.h"
#include "core/utility/terrain_3d_sync.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "eastl_config.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_local_compute_buffer_service.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_sampler.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_texture.h"

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

// t3dBrushElement_s centralized in Terrain3DTypes.h.


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
		        && m_nWidth > 0u && m_nHeight > 0u && m_regionMap.size() == T3D_REGION_SLOTS && !m_materialData.is_empty()
		        && !m_pushConstants.is_empty();
	}
} t3dDisplacementRequest_t;


// t3dBrushParams_s centralized in Terrain3DTypes.h.


// t3dDerivedMapComputeJob_s centralized in Terrain3DTypes.h.


typedef struct t3dPipeline_s {

	Terrain3DRdShader m_hShader;
	Terrain3DRdPipeline m_hPipeline;
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
		Terrain3DScopedLock lock( m_errorMutex );
		return m_lastError;
	}

	bool processBrush( eastl::vector<t3dBrushElement_t, Terrain3DEastlAllocator> &rElements, const t3dBrushParams_t &rParams );
	static void processBrushCpu( eastl::vector<t3dBrushElement_t, Terrain3DEastlAllocator> &rElements, const t3dBrushParams_t &rParams );
	bool generateDerivedMaps( t3dDerivedMapComputeJob_t *pJobs, uint32_t nJobCount );
	// Builds a render-thread-safe snapshot without touching the global RenderingDevice.
	// The raw GLSL path is intentionally restricted to non-projected, non-detiled
	// packed-PBR/R8 materials; all other variants retain the raster fallback.
	bool prepareDisplacementRequest( Terrain3DOwner *pTerrain, const Vector3 &rTargetPosition, t3dDisplacementRequest_t &rRequest );
	// Must execute through the render-thread gateway.
	bool dispatchDisplacementRequest( const t3dDisplacementRequest_t &rRequest, RID &rTexture );
	// Must execute on the render thread before Terrain3DCompute is destroyed.
	void shutdownDisplacementResources( void );
	EA_FORCE_INLINE uint64_t getRetainedLocalGpuBufferBytes( void ) const {
		uint64_t bytes = m_brushStorage.retainedBytes();
		for ( uint32_t slot = 0u; slot < T3D_DERIVED_READBACK_SLOT_COUNT; ++slot ) {
			bytes += m_derivedStorage[slot].retainedBytes();
		}
		return bytes;
	}
	t3dLocalComputeBufferStats_t getLocalBufferStats( void ) const { return m_localBuffers.getStats(); }

private:
	RenderingDevice *m_pRd = NULL;       // Owned local device for readback workloads.
	Terrain3DLocalComputeBufferService m_localBuffers;
	RenderingDevice *m_pRenderRd = NULL; // Non-owning global renderer device.
	t3dPipeline_t m_brushPipeline;
	t3dPipeline_t m_derivedPipeline;
	t3dPipeline_t m_displacementPipeline;
	t3dLocalComputeStorageSet_t m_brushStorage;
	t3dLocalComputeStoragePairSet_t m_derivedStorage[T3D_DERIVED_READBACK_SLOT_COUNT];
	Terrain3DRdUniformSet m_hDisplacementUniformSet;
	Terrain3DRdGpuBuffer m_hDisplacementRegionBuffer;
	Terrain3DRdGpuBuffer m_hDisplacementMaterialBuffer;
	Terrain3DRdTexture m_hDisplacementOutputTexture;
	RID m_hDisplacementBoundHeight;
	RID m_hDisplacementBoundControl;
	RID m_hDisplacementBoundDisplacement;
	Terrain3DRdSampler m_hNearestSampler;
	Terrain3DRdSampler m_hLinearRepeatSampler;
	uint64_t m_nDisplacementRegionHash = 0u;
	uint64_t m_nDisplacementMaterialHash = 0u;
	bool m_bDisplacementRegionUploaded = false;
	bool m_bDisplacementMaterialUploaded = false;
	bool m_bPersistentDisplacementBuffers = false;
	uint32_t m_nDisplacementWidth = 0u;
	uint32_t m_nDisplacementHeight = 0u;
	mutable Terrain3DMutex m_errorMutex;
	String m_lastError;
	bool m_bDeviceAttempted = false;
	bool m_bRenderDeviceAttempted = false;

	bool ensureDevice( void );
	bool ensureRenderDevice( void );
	bool ensurePipeline( t3dPipeline_t &rPipeline, t3dRdShaderId_t id, const String &rName );
	bool ensureRenderPipeline( t3dPipeline_t &rPipeline, const Ref<RDShaderSPIRV> &rSpirv, const String &rName );
	bool ensureBrushCapacity( uint32_t nBytes );
	bool ensureDerivedCapacity( uint32_t nSlot, uint32_t nInputBytes, uint32_t nOutputBytes );
	bool ensureDisplacementResources( uint32_t nWidth, uint32_t nHeight, const Ref<RDShaderSPIRV> &rSpirv );
	void freeRenderUniformSet( Terrain3DRdUniformSet &rUniformSet );
	void freeRenderPipeline( t3dPipeline_t &rPipeline );
	void freeDisplacementTexture( void );
	void shutdown( void );
	EA_FORCE_INLINE void setError( const String &rError ) {
		Terrain3DScopedLock lock( m_errorMutex );
		m_lastError = rError;
	}
	static PackedByteArray copyToBytes( const void *pData, size_t nSize );
};

#endif // TERRAIN3D_COMPUTE_H
