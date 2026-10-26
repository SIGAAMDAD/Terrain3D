#ifndef TERRAIN3D_LOCAL_COMPUTE_BUFFER_SERVICE_H
#define TERRAIN3D_LOCAL_COMPUTE_BUFFER_SERVICE_H

#include "core/terrain_3d_sync.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_shader.h"
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
class Terrain3DLocalComputeBufferService
{
	RenderingDevice *m_device;

public:
	struct Stats {
		uint64_t bufferCreates = 0u;
		uint64_t uniformSetCreates = 0u;
		uint64_t pipelineCreates = 0u;
		uint64_t resourceFailures = 0u;
		uint64_t uploadCalls = 0u;
		uint64_t bytesUploaded = 0u;
		uint64_t readbackCalls = 0u;
		uint64_t bytesReadBack = 0u;
	};

private:
	mutable Stats m_stats;
	mutable t3dMutex_t m_statsMutex;

	void recordFailure( void ) const {
		t3dScopedLock_t lock( m_statsMutex );
		++m_stats.resourceFailures;
	}

	void recordBufferCreate( void ) const {
		t3dScopedLock_t lock( m_statsMutex );
		++m_stats.bufferCreates;
	}

	static t3dRdGpuBufferSpec_t storageSpec( void ) {
		t3dRdGpuBufferSpec_t spec;
		spec.m_type = Terrain3DRdContext::RESOURCE_STORAGE_BUFFER;
		spec.m_memory = Terrain3DRdContext::MEMORY_STORAGE;
		spec.m_growth = T3D_GPU_HIGH_WATER_1_5X;
		spec.m_nMinimumCapacityBytes = 4096u;
		return spec;
	}

	bool createReplacement( const uint32_t nRequired, Terrain3DRdGpuBuffer &rReplacement ) const {
		if ( !m_device || nRequired == 0u ) {
			recordFailure();
			return false;
		}
		const t3dRdGpuBufferSpec_t spec = storageSpec();
		if ( !rReplacement.configure( spec ) || !rReplacement.ensureLocal( m_device, nRequired ) ) {
			recordFailure();
			return false;
		}
		recordBufferCreate();
		return true;
	}

	static godot::Ref<godot::RDUniform> storageUniform( uint32_t nBinding, const Terrain3DRdGpuBuffer &rBuffer ) {
		godot::Ref<godot::RDUniform> uniform;
		uniform.instantiate();
		if ( uniform.is_valid() && rBuffer.isValid() ) {
			uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
			uniform->set_binding( nBinding );
			uniform->add_id( rBuffer.rid() );
		}
		return uniform;
	}

public:
	struct StorageSet {
		Terrain3DRdGpuBuffer buffer;
		RID uniformSet;

		uint64_t retainedBytes( void ) const { return buffer.isValid() ? buffer.capacityBytes() : 0u; }
	};

	struct StoragePairSet {
		Terrain3DRdGpuBuffer input;
		Terrain3DRdGpuBuffer output;
		RID uniformSet;

		uint64_t retainedBytes( void ) const {
			return ( uint64_t )( input.isValid() ? input.capacityBytes() : 0u )
			        + ( uint64_t )( output.isValid() ? output.capacityBytes() : 0u );
		}
	};

	explicit Terrain3DLocalComputeBufferService( RenderingDevice *pDevice = nullptr ) : m_device( pDevice ) {}

	void setDevice( RenderingDevice *pDevice ) { m_device = pDevice; }
	Stats getStats( void ) const {
		t3dScopedLock_t lock( m_statsMutex );
		return m_stats;
	}

	bool createPipeline( const godot::Ref<godot::RDShaderSPIRV> &rSpirv,
	        const godot::String &rName, Terrain3DRdShader &rShader, RID &rPipeline ) const {
		if ( !m_device || rSpirv.is_null() || rShader.isValid() || rPipeline.is_valid() ) {
			return false;
		}
		if ( !rShader.createLocal( m_device, rName, rSpirv ) ) {
			recordFailure();
			return false;
		}
		rPipeline = m_device->compute_pipeline_create( rShader.rid() );
		if ( !rPipeline.is_valid() ) {
			recordFailure();
			rShader.releaseLocal( m_device );
			return false;
		}
		{
			t3dScopedLock_t lock( m_statsMutex );
			++m_stats.pipelineCreates;
		}
		return true;
	}

	RID createUniformSet( const godot::TypedArray<godot::Ref<godot::RDUniform>> &rUniforms,
	        const RID &rShader, uint32_t nSet ) const {
		const RID set = m_device && rShader.is_valid() ? m_device->uniform_set_create( rUniforms, rShader, nSet ) : RID();
		if ( set.is_valid() ) {
			t3dScopedLock_t lock( m_statsMutex );
			++m_stats.uniformSetCreates;
		} else {
			recordFailure();
		}
		return set;
	}

	static constexpr uint32_t grownCapacity( uint32_t nRequired ) {
		if ( nRequired == 0u ) {
			return 0u;
		}
		uint32_t capacity = 0u;
		return t3dRdGpuBufferGrownCapacity( 4096u, nRequired, T3D_GPU_HIGH_WATER_1_5X, capacity )
		        ? capacity
		        : 0u;
	}

	void release( RID &rRid ) const {
		if ( m_device && rRid.is_valid() ) {
			m_device->free_rid( rRid );
		}
		rRid = RID();
	}

	void releasePipeline( RID &rPipeline, Terrain3DRdShader &rShader ) const {
		release( rPipeline );
		rShader.releaseLocal( m_device );
	}

	void release( StorageSet &rSet ) const {
		release( rSet.uniformSet );
		rSet.buffer.releaseLocal( m_device );
	}

	void release( StoragePairSet &rSet ) const {
		release( rSet.uniformSet );
		rSet.input.releaseLocal( m_device );
		rSet.output.releaseLocal( m_device );
	}

	bool ensureStorageSet( StorageSet &rSet, uint32_t nRequired, const RID &rShader ) const {
		if ( rSet.buffer.isValid() && !rSet.buffer.needsCapacity( nRequired ) && rSet.uniformSet.is_valid() ) {
			return true;
		}
		if ( !m_device || !rShader.is_valid() || nRequired == 0u ) {
			return false;
		}
		const bool replace = rSet.buffer.needsCapacity( nRequired );
		Terrain3DRdGpuBuffer replacement;
		if ( replace && !createReplacement( nRequired, replacement ) ) {
			return false;
		}
		const Terrain3DRdGpuBuffer &binding_buffer = replace ? replacement : rSet.buffer;
		godot::TypedArray<godot::Ref<godot::RDUniform>> uniforms;
		const godot::Ref<godot::RDUniform> uniform = storageUniform( 0u, binding_buffer );
		if ( uniform.is_null() ) {
			replacement.releaseLocal( m_device );
			return false;
		}
		uniforms.push_back( uniform );
		RID new_set = createUniformSet( uniforms, rShader, 0u );
		if ( !new_set.is_valid() ) {
			replacement.releaseLocal( m_device );
			return false;
		}
		release( rSet.uniformSet );
		if ( replace ) {
			rSet.buffer.swap( replacement );
			replacement.releaseLocal( m_device );
		}
		rSet.uniformSet = new_set;
		return true;
	}

	bool ensureStoragePairSet( StoragePairSet &rSet, uint32_t nInputRequired,
	        uint32_t nOutputRequired, const RID &rShader ) const {
		if ( rSet.input.isValid() && !rSet.input.needsCapacity( nInputRequired )
		        && rSet.output.isValid() && !rSet.output.needsCapacity( nOutputRequired ) && rSet.uniformSet.is_valid() ) {
			return true;
		}
		if ( !m_device || !rShader.is_valid() || nInputRequired == 0u || nOutputRequired == 0u ) {
			return false;
		}
		const bool replace_input = rSet.input.needsCapacity( nInputRequired );
		const bool replace_output = rSet.output.needsCapacity( nOutputRequired );
		Terrain3DRdGpuBuffer new_input;
		Terrain3DRdGpuBuffer new_output;
		if ( ( replace_input && !createReplacement( nInputRequired, new_input ) )
		        || ( replace_output && !createReplacement( nOutputRequired, new_output ) ) ) {
			new_input.releaseLocal( m_device );
			new_output.releaseLocal( m_device );
			return false;
		}
		const Terrain3DRdGpuBuffer &input_buffer = replace_input ? new_input : rSet.input;
		const Terrain3DRdGpuBuffer &output_buffer = replace_output ? new_output : rSet.output;
		godot::TypedArray<godot::Ref<godot::RDUniform>> uniforms;
		const godot::Ref<godot::RDUniform> input_uniform = storageUniform( 0u, input_buffer );
		const godot::Ref<godot::RDUniform> output_uniform = storageUniform( 1u, output_buffer );
		if ( input_uniform.is_null() || output_uniform.is_null() ) {
			new_input.releaseLocal( m_device );
			new_output.releaseLocal( m_device );
			return false;
		}
		uniforms.push_back( input_uniform );
		uniforms.push_back( output_uniform );
		RID new_set = createUniformSet( uniforms, rShader, 0u );
		if ( !new_set.is_valid() ) {
			new_input.releaseLocal( m_device );
			new_output.releaseLocal( m_device );
			return false;
		}
		release( rSet.uniformSet );
		if ( replace_input ) {
			rSet.input.swap( new_input );
			new_input.releaseLocal( m_device );
		}
		if ( replace_output ) {
			rSet.output.swap( new_output );
			new_output.releaseLocal( m_device );
		}
		rSet.uniformSet = new_set;
		return true;
	}

	bool upload( const StorageSet &rSet, const PackedByteArray &rBytes, uint32_t nBytes ) const {
		const bool success = rSet.buffer.writePackedLocal( m_device, 0u, rBytes, nBytes );
		if ( success ) {
			t3dScopedLock_t lock( m_statsMutex );
			++m_stats.uploadCalls;
			m_stats.bytesUploaded += nBytes;
		}
		return success;
	}

	bool uploadInput( const StoragePairSet &rSet, const PackedByteArray &rBytes, uint32_t nBytes ) const {
		const bool success = rSet.input.writePackedLocal( m_device, 0u, rBytes, nBytes );
		if ( success ) {
			t3dScopedLock_t lock( m_statsMutex );
			++m_stats.uploadCalls;
			m_stats.bytesUploaded += nBytes;
		}
		return success;
	}

	PackedByteArray readback( const StorageSet &rSet, uint32_t nBytes ) const {
		PackedByteArray bytes = rSet.buffer.readbackLocal( m_device, 0u, nBytes );
		if ( bytes.size() == nBytes ) {
			t3dScopedLock_t lock( m_statsMutex );
			++m_stats.readbackCalls;
			m_stats.bytesReadBack += nBytes;
		}
		return bytes;
	}

	PackedByteArray readbackOutput( const StoragePairSet &rSet, uint32_t nBytes ) const {
		PackedByteArray bytes = rSet.output.readbackLocal( m_device, 0u, nBytes );
		if ( bytes.size() == nBytes ) {
			t3dScopedLock_t lock( m_statsMutex );
			++m_stats.readbackCalls;
			m_stats.bytesReadBack += nBytes;
		}
		return bytes;
	}
};

static_assert( Terrain3DLocalComputeBufferService::grownCapacity( 0u ) == 0u
               && Terrain3DLocalComputeBufferService::grownCapacity( 1u ) == 4096u
               && Terrain3DLocalComputeBufferService::grownCapacity( 4097u ) == 6144u
               && Terrain3DLocalComputeBufferService::grownCapacity( UINT32_MAX ) == UINT32_MAX,
        "Local compute buffer growth policy changed" );

#endif // TERRAIN3D_LOCAL_COMPUTE_BUFFER_SERVICE_H
