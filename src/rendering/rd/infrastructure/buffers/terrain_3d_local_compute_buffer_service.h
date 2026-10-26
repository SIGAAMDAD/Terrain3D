#ifndef TERRAIN3D_LOCAL_COMPUTE_BUFFER_SERVICE_H
#define TERRAIN3D_LOCAL_COMPUTE_BUFFER_SERVICE_H

#include "Terrain3DTypes.h"

#include "core/utility/terrain_3d_sync.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_uniform_set.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <stdint.h>

// Local compute owns pipeline/uniform-set policy and statistics; Terrain3DRdShader
// owns physical shaders and Terrain3DRdGpuBuffer owns storage-buffer identity, capacity, growth, I/O and release.
// Local buffers are never registered with the global Terrain3DRdContext.
// t3dLocalComputeBufferStats_t centralized in Terrain3DTypes.h.


class t3dLocalComputeStorageSet_t {
public:

	Terrain3DRdGpuBuffer buffer;
	Terrain3DRdUniformSet uniformSet;
	uint64_t retainedBytes( void ) const { return buffer.isValid() ? buffer.capacityBytes() : 0u; }
};

class t3dLocalComputeStoragePairSet_t {
public:

	Terrain3DRdGpuBuffer input;
	Terrain3DRdGpuBuffer output;
	Terrain3DRdUniformSet uniformSet;
	uint64_t retainedBytes( void ) const {
		return ( uint64_t )( input.isValid() ? input.capacityBytes() : 0u )
		        + ( uint64_t )( output.isValid() ? output.capacityBytes() : 0u );
	}
};

class Terrain3DLocalComputeBufferService
{
	RenderingDevice *m_device;

private:
	mutable t3dLocalComputeBufferStats_t m_stats;
	mutable Terrain3DMutex m_statsMutex;

	void recordFailure( void ) const;

	void recordBufferCreate( void ) const;

	static t3dRdGpuBufferSpec_t storageSpec( void );

	bool createReplacement( const uint32_t nRequired, Terrain3DRdGpuBuffer &rReplacement ) const;

	static godot::Ref<godot::RDUniform> storageUniform( uint32_t nBinding, const Terrain3DRdGpuBuffer &rBuffer );

public:
	explicit Terrain3DLocalComputeBufferService( RenderingDevice *pDevice = NULL ) : m_device( pDevice ) {}

	void setDevice( RenderingDevice *pDevice ) { m_device = pDevice; }
	t3dLocalComputeBufferStats_t getStats( void ) const {
		Terrain3DScopedLock lock( m_statsMutex );
		return m_stats;
	}

	bool createPipeline( const godot::Ref<godot::RDShaderSPIRV> &rSpirv,
	        const godot::String &rName, Terrain3DRdShader &rShader, Terrain3DRdPipeline &rPipeline ) const;

	bool createUniformSet( Terrain3DRdUniformSet &set,
	        const godot::TypedArray<godot::Ref<godot::RDUniform>> &rUniforms,
	        const RID &rShader, uint32_t nSet ) const;

	static constexpr uint32_t grownCapacity( uint32_t nRequired ) {
		if ( nRequired == 0u ) {
			return 0u;
		}
		uint32_t capacity = 0u;
		return t3dRdGpuBufferGrownCapacity( 4096u, nRequired, T3D_GPU_HIGH_WATER_1_5X, capacity )
		        ? capacity
		        : 0u;
	}

	void release( RID &rRid ) const;
	void release( Terrain3DRdUniformSet &set ) const { set.releaseLocal( m_device ); }

	void releasePipeline( Terrain3DRdPipeline &rPipeline, Terrain3DRdShader &rShader ) const;

	void release( t3dLocalComputeStorageSet_t &rSet ) const;

	void release( t3dLocalComputeStoragePairSet_t &rSet ) const;

	bool ensureStorageSet( t3dLocalComputeStorageSet_t &rSet, uint32_t nRequired, const RID &rShader ) const;

	bool ensureStoragePairSet( t3dLocalComputeStoragePairSet_t &rSet, uint32_t nInputRequired,
	        uint32_t nOutputRequired, const RID &rShader ) const;

	bool upload( const t3dLocalComputeStorageSet_t &rSet, const PackedByteArray &rBytes, uint32_t nBytes ) const;

	bool uploadInput( const t3dLocalComputeStoragePairSet_t &rSet, const PackedByteArray &rBytes, uint32_t nBytes ) const;

	PackedByteArray readback( const t3dLocalComputeStorageSet_t &rSet, uint32_t nBytes ) const;

	PackedByteArray readbackOutput( const t3dLocalComputeStoragePairSet_t &rSet, uint32_t nBytes ) const;
};

static_assert( Terrain3DLocalComputeBufferService::grownCapacity( 0u ) == 0u
               && Terrain3DLocalComputeBufferService::grownCapacity( 1u ) == 4096u
               && Terrain3DLocalComputeBufferService::grownCapacity( 4097u ) == 6144u
               && Terrain3DLocalComputeBufferService::grownCapacity( UINT32_MAX ) == UINT32_MAX,
        "Local compute buffer growth policy changed" );

#endif // TERRAIN3D_LOCAL_COMPUTE_BUFFER_SERVICE_H
