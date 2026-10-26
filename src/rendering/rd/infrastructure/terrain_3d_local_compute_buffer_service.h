#ifndef TERRAIN3D_LOCAL_COMPUTE_BUFFER_SERVICE_H
#define TERRAIN3D_LOCAL_COMPUTE_BUFFER_SERVICE_H

#include "core/terrain_3d_sync.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <stdint.h>

// Buffers made by a local RenderingDevice stay on that device. They cannot be
// registered with, or suballocated from, the global render-RD context.
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
	bool createBuffer( uint32_t nRequired, RID &rBuffer, uint32_t &rCapacity ) const {
		rCapacity = grownCapacity( nRequired );
		if ( !m_device || rCapacity == 0u ) {
			recordFailure();
			return false;
		}
		rBuffer = m_device->storage_buffer_create( rCapacity );
		if ( rBuffer.is_valid() ) {
			t3dScopedLock_t lock( m_statsMutex );
			++m_stats.bufferCreates;
		} else {
			recordFailure();
		}
		return rBuffer.is_valid();
	}

	static godot::Ref<godot::RDUniform> storageUniform( uint32_t nBinding, const RID &rBuffer ) {
		godot::Ref<godot::RDUniform> uniform;
		uniform.instantiate();
		if ( uniform.is_valid() ) {
			uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
			uniform->set_binding( nBinding );
			uniform->add_id( rBuffer );
		}
		return uniform;
	}

public:
	struct StorageSet {
		RID buffer;
		RID uniformSet;
		uint32_t capacity = 0u;

		uint64_t retainedBytes( void ) const { return buffer.is_valid() ? capacity : 0u; }
	};

	struct StoragePairSet {
		RID input;
		RID output;
		RID uniformSet;
		uint32_t inputCapacity = 0u;
		uint32_t outputCapacity = 0u;

		uint64_t retainedBytes( void ) const {
			return ( uint64_t )( input.is_valid() ? inputCapacity : 0u ) + ( output.is_valid() ? outputCapacity : 0u );
		}
	};

	explicit Terrain3DLocalComputeBufferService( RenderingDevice *pDevice = nullptr ) : m_device( pDevice ) {}

	void setDevice( RenderingDevice *pDevice ) { m_device = pDevice; }
	Stats getStats( void ) const {
		t3dScopedLock_t lock( m_statsMutex );
		return m_stats;
	}

	bool createPipeline( const godot::Ref<godot::RDShaderSPIRV> &rSpirv,
	        const godot::String &rName, RID &rShader, RID &rPipeline ) const {
		if ( !m_device || rSpirv.is_null() ) {
			return false;
		}
		rShader = m_device->shader_create_from_spirv( rSpirv, rName );
		if ( !rShader.is_valid() ) {
			recordFailure();
			return false;
		}
		rPipeline = m_device->compute_pipeline_create( rShader );
		if ( !rPipeline.is_valid() ) {
			recordFailure();
			release( rShader );
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
		uint64_t capacity = 4096u;
		while ( capacity < nRequired ) {
			capacity += capacity / 2u;
			if ( capacity > UINT32_MAX ) {
				return nRequired;
			}
		}
		return ( uint32_t )capacity;
	}

	void release( RID &rBuffer ) const {
		if ( m_device && rBuffer.is_valid() ) {
			m_device->free_rid( rBuffer );
		}
		rBuffer = RID();
	}

	void releasePipeline( RID &rPipeline, RID &rShader ) const {
		release( rPipeline );
		release( rShader );
	}

	void release( StorageSet &rSet ) const {
		release( rSet.uniformSet );
		release( rSet.buffer );
		rSet.capacity = 0u;
	}

	void release( StoragePairSet &rSet ) const {
		release( rSet.uniformSet );
		release( rSet.input );
		release( rSet.output );
		rSet.inputCapacity = 0u;
		rSet.outputCapacity = 0u;
	}

	bool ensureStorageSet( StorageSet &rSet, uint32_t nRequired, const RID &rShader ) const {
		return ensureStorageSet( rSet.buffer, rSet.capacity, nRequired, rSet.uniformSet, rShader );
	}

	bool ensureStoragePairSet( StoragePairSet &rSet, uint32_t nInputRequired,
	        uint32_t nOutputRequired, const RID &rShader ) const {
		return ensureStoragePairSet( rSet.input, rSet.inputCapacity, nInputRequired,
		        rSet.output, rSet.outputCapacity, nOutputRequired, rSet.uniformSet, rShader );
	}

private:
	bool ensureStorageSet( RID &rBuffer, uint32_t &rCapacity, uint32_t nRequired,
	        RID &rUniformSet, const RID &rShader ) const {
		if ( rBuffer.is_valid() && rCapacity >= nRequired && rUniformSet.is_valid() ) {
			return true;
		}
		if ( !m_device || !rShader.is_valid() ) {
			return false;
		}
		const bool replace = !rBuffer.is_valid() || rCapacity < nRequired;
		RID replacement;
		uint32_t capacity = rCapacity;
		if ( replace && !createBuffer( nRequired, replacement, capacity ) ) {
			return false;
		}
		godot::TypedArray<godot::Ref<godot::RDUniform>> uniforms;
		const godot::Ref<godot::RDUniform> uniform = storageUniform( 0u, replace ? replacement : rBuffer );
		if ( uniform.is_null() ) {
			release( replacement );
			return false;
		}
		uniforms.push_back( uniform );
		RID new_set = createUniformSet( uniforms, rShader, 0u );
		if ( !new_set.is_valid() ) {
			release( replacement );
			return false;
		}
		release( rUniformSet );
		if ( replace ) {
			release( rBuffer );
			rBuffer = replacement;
			rCapacity = capacity;
		}
		rUniformSet = new_set;
		return true;
	}

	bool ensureStoragePairSet( RID &rInput, uint32_t &rInputCapacity, uint32_t nInputRequired,
	        RID &rOutput, uint32_t &rOutputCapacity, uint32_t nOutputRequired,
	        RID &rUniformSet, const RID &rShader ) const {
		if ( rInput.is_valid() && rInputCapacity >= nInputRequired && rOutput.is_valid()
		        && rOutputCapacity >= nOutputRequired && rUniformSet.is_valid() ) {
			return true;
		}
		if ( !m_device || !rShader.is_valid() ) {
			return false;
		}
		const bool replace_input = !rInput.is_valid() || rInputCapacity < nInputRequired;
		const bool replace_output = !rOutput.is_valid() || rOutputCapacity < nOutputRequired;
		RID new_input;
		RID new_output;
		uint32_t input_capacity = rInputCapacity;
		uint32_t output_capacity = rOutputCapacity;
		if ( ( replace_input && !createBuffer( nInputRequired, new_input, input_capacity ) )
		        || ( replace_output && !createBuffer( nOutputRequired, new_output, output_capacity ) ) ) {
			release( new_input );
			release( new_output );
			return false;
		}
		godot::TypedArray<godot::Ref<godot::RDUniform>> uniforms;
		const godot::Ref<godot::RDUniform> input_uniform = storageUniform( 0u, replace_input ? new_input : rInput );
		const godot::Ref<godot::RDUniform> output_uniform = storageUniform( 1u, replace_output ? new_output : rOutput );
		if ( input_uniform.is_null() || output_uniform.is_null() ) {
			release( new_input );
			release( new_output );
			return false;
		}
		uniforms.push_back( input_uniform );
		uniforms.push_back( output_uniform );
		RID new_set = createUniformSet( uniforms, rShader, 0u );
		if ( !new_set.is_valid() ) {
			release( new_input );
			release( new_output );
			return false;
		}
		release( rUniformSet );
		if ( replace_input ) {
			release( rInput );
			rInput = new_input;
			rInputCapacity = input_capacity;
		}
		if ( replace_output ) {
			release( rOutput );
			rOutput = new_output;
			rOutputCapacity = output_capacity;
		}
		rUniformSet = new_set;
		return true;
	}

	bool upload( const RID &rBuffer, uint32_t nCapacity, const PackedByteArray &rBytes, uint32_t nBytes ) const {
		const bool success = m_device && rBuffer.is_valid() && nBytes <= nCapacity && rBytes.size() >= nBytes
		        && m_device->buffer_update( rBuffer, 0u, nBytes, rBytes ) == godot::OK;
		if ( success ) {
			t3dScopedLock_t lock( m_statsMutex );
			++m_stats.uploadCalls;
			m_stats.bytesUploaded += nBytes;
		}
		return success;
	}

public:
	bool upload( const StorageSet &rSet, const PackedByteArray &rBytes, uint32_t nBytes ) const {
		return upload( rSet.buffer, rSet.capacity, rBytes, nBytes );
	}

	bool uploadInput( const StoragePairSet &rSet, const PackedByteArray &rBytes, uint32_t nBytes ) const {
		return upload( rSet.input, rSet.inputCapacity, rBytes, nBytes );
	}

private:
	PackedByteArray readback( const RID &rBuffer, uint32_t nCapacity, uint32_t nBytes ) const {
		if ( !m_device || !rBuffer.is_valid() || nBytes > nCapacity ) {
			return PackedByteArray();
		}
		PackedByteArray bytes = m_device->buffer_get_data( rBuffer, 0u, nBytes );
		if ( bytes.size() == nBytes ) {
			t3dScopedLock_t lock( m_statsMutex );
			++m_stats.readbackCalls;
			m_stats.bytesReadBack += nBytes;
		}
		return bytes;
	}

public:
	PackedByteArray readback( const StorageSet &rSet, uint32_t nBytes ) const {
		return readback( rSet.buffer, rSet.capacity, nBytes );
	}

	PackedByteArray readbackOutput( const StoragePairSet &rSet, uint32_t nBytes ) const {
		return readback( rSet.output, rSet.outputCapacity, nBytes );
	}
};

static_assert( Terrain3DLocalComputeBufferService::grownCapacity( 0u ) == 0u
               && Terrain3DLocalComputeBufferService::grownCapacity( 1u ) == 4096u
               && Terrain3DLocalComputeBufferService::grownCapacity( 4097u ) == 6144u
               && Terrain3DLocalComputeBufferService::grownCapacity( UINT32_MAX ) == UINT32_MAX,
        "Local compute buffer growth policy changed" );

#endif // TERRAIN3D_LOCAL_COMPUTE_BUFFER_SERVICE_H
