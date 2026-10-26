#include "rendering/rd/infrastructure/buffers/terrain_3d_local_compute_buffer_service.h"

void Terrain3DLocalComputeBufferService::recordFailure(
	void ) const {
	Terrain3DScopedLock lock( m_statsMutex );
	++m_stats.resourceFailures;
}

void Terrain3DLocalComputeBufferService::recordBufferCreate(
	void ) const {
	Terrain3DScopedLock lock( m_statsMutex );
	++m_stats.bufferCreates;
}

t3dRdGpuBufferSpec_t Terrain3DLocalComputeBufferService::storageSpec(
	void ) {
	t3dRdGpuBufferSpec_t spec;
	spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
	spec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
	spec.m_growth = T3D_GPU_HIGH_WATER_1_5X;
	spec.m_nMinimumCapacityBytes = 4096u;
	return spec;
}

bool Terrain3DLocalComputeBufferService::createReplacement(
	const uint32_t		  nRequired,
	Terrain3DRdGpuBuffer &rReplacement ) const {
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

godot::Ref< godot::RDUniform > Terrain3DLocalComputeBufferService::storageUniform(
	uint32_t					nBinding,
	const Terrain3DRdGpuBuffer &rBuffer ) {
	godot::Ref< godot::RDUniform > uniform;
	uniform.instantiate();
	if ( uniform.is_valid() && rBuffer.isValid() ) {
		uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
		uniform->set_binding( nBinding );
		uniform->add_id( rBuffer.rid() );
	}
	return uniform;
}

bool Terrain3DLocalComputeBufferService::createPipeline(
	const godot::Ref< godot::RDShaderSPIRV > &rSpirv,
	const godot::String						 &rName,
	Terrain3DRdShader						 &rShader,
	Terrain3DRdPipeline						 &rPipeline ) const {
	if ( !m_device || rSpirv.is_null() || rShader.isValid() || rPipeline.isValid() ) {
		return false;
	}
	if ( !rShader.createLocal( m_device, rName, rSpirv ) ) {
		recordFailure();
		return false;
	}
	if ( !rPipeline.createLocalCompute( m_device, rShader.rid() ) ) {
		recordFailure();
		rShader.releaseLocal( m_device );
		return false;
	}
	{
		Terrain3DScopedLock lock( m_statsMutex );
		++m_stats.pipelineCreates;
	}
	return true;
}

bool Terrain3DLocalComputeBufferService::createUniformSet(
	Terrain3DRdUniformSet									  &set,
	const godot::TypedArray< godot::Ref< godot::RDUniform > > &rUniforms,
	const RID												  &rShader,
	uint32_t												   nSet ) const {
	if ( set.createLocal( m_device, rUniforms, rShader, nSet ) ) {
		Terrain3DScopedLock lock( m_statsMutex );
		++m_stats.uniformSetCreates;
		return true;
	}
	recordFailure();
	return false;
}

void Terrain3DLocalComputeBufferService::release(
	RID &rRid ) const {
	if ( m_device && rRid.is_valid() ) {
		m_device->free_rid( rRid );
	}
	rRid = RID();
}

void Terrain3DLocalComputeBufferService::releasePipeline(
	Terrain3DRdPipeline &rPipeline,
	Terrain3DRdShader	&rShader ) const {
	rPipeline.releaseLocal( m_device );
	rShader.releaseLocal( m_device );
}

void Terrain3DLocalComputeBufferService::release(
	t3dLocalComputeStorageSet_t &rSet ) const {
	release( rSet.uniformSet );
	rSet.buffer.releaseLocal( m_device );
}

void Terrain3DLocalComputeBufferService::release(
	t3dLocalComputeStoragePairSet_t &rSet ) const {
	release( rSet.uniformSet );
	rSet.input.releaseLocal( m_device );
	rSet.output.releaseLocal( m_device );
}

bool Terrain3DLocalComputeBufferService::ensureStorageSet(
	t3dLocalComputeStorageSet_t &rSet,
	uint32_t					 nRequired,
	const RID					&rShader ) const {
	if ( rSet.buffer.isValid() && !rSet.buffer.needsCapacity( nRequired ) && rSet.uniformSet.isValid() ) {
		return true;
	}
	if ( !m_device || !rShader.is_valid() || nRequired == 0u ) {
		return false;
	}
	const bool			 replace = rSet.buffer.needsCapacity( nRequired );
	Terrain3DRdGpuBuffer replacement;
	if ( replace && !createReplacement( nRequired, replacement ) ) {
		return false;
	}
	const Terrain3DRdGpuBuffer						   &binding_buffer = replace ? replacement : rSet.buffer;
	godot::TypedArray< godot::Ref< godot::RDUniform > > uniforms;
	const godot::Ref< godot::RDUniform >				uniform = storageUniform( 0u, binding_buffer );
	if ( uniform.is_null() ) {
		replacement.releaseLocal( m_device );
		return false;
	}
	uniforms.push_back( uniform );
	Terrain3DRdUniformSet new_set;
	if ( !createUniformSet( new_set, uniforms, rShader, 0u ) ) {
		replacement.releaseLocal( m_device );
		return false;
	}
	release( rSet.uniformSet );
	if ( replace ) {
		rSet.buffer.swap( replacement );
		replacement.releaseLocal( m_device );
	}
	rSet.uniformSet.swap( new_set );
	return true;
}

bool Terrain3DLocalComputeBufferService::ensureStoragePairSet(
	t3dLocalComputeStoragePairSet_t &rSet,
	uint32_t						 nInputRequired,
	uint32_t						 nOutputRequired,
	const RID						&rShader ) const {
	if ( rSet.input.isValid() && !rSet.input.needsCapacity( nInputRequired ) && rSet.output.isValid() &&
		 !rSet.output.needsCapacity( nOutputRequired ) && rSet.uniformSet.isValid() ) {
		return true;
	}
	if ( !m_device || !rShader.is_valid() || nInputRequired == 0u || nOutputRequired == 0u ) {
		return false;
	}
	const bool			 replace_input = rSet.input.needsCapacity( nInputRequired );
	const bool			 replace_output = rSet.output.needsCapacity( nOutputRequired );
	Terrain3DRdGpuBuffer new_input;
	Terrain3DRdGpuBuffer new_output;
	if ( ( replace_input && !createReplacement( nInputRequired, new_input ) ) ||
		 ( replace_output && !createReplacement( nOutputRequired, new_output ) ) ) {
		new_input.releaseLocal( m_device );
		new_output.releaseLocal( m_device );
		return false;
	}
	const Terrain3DRdGpuBuffer						   &input_buffer = replace_input ? new_input : rSet.input;
	const Terrain3DRdGpuBuffer						   &output_buffer = replace_output ? new_output : rSet.output;
	godot::TypedArray< godot::Ref< godot::RDUniform > > uniforms;
	const godot::Ref< godot::RDUniform >				input_uniform = storageUniform( 0u, input_buffer );
	const godot::Ref< godot::RDUniform >				output_uniform = storageUniform( 1u, output_buffer );
	if ( input_uniform.is_null() || output_uniform.is_null() ) {
		new_input.releaseLocal( m_device );
		new_output.releaseLocal( m_device );
		return false;
	}
	uniforms.push_back( input_uniform );
	uniforms.push_back( output_uniform );
	Terrain3DRdUniformSet new_set;
	if ( !createUniformSet( new_set, uniforms, rShader, 0u ) ) {
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
	rSet.uniformSet.swap( new_set );
	return true;
}

bool Terrain3DLocalComputeBufferService::upload(
	const t3dLocalComputeStorageSet_t &rSet,
	const PackedByteArray			  &rBytes,
	uint32_t						   nBytes ) const {
	const bool success = rSet.buffer.writePackedLocal( m_device, 0u, rBytes, nBytes );
	if ( success ) {
		Terrain3DScopedLock lock( m_statsMutex );
		++m_stats.uploadCalls;
		m_stats.bytesUploaded += nBytes;
	}
	return success;
}

bool Terrain3DLocalComputeBufferService::uploadInput(
	const t3dLocalComputeStoragePairSet_t &rSet,
	const PackedByteArray				  &rBytes,
	uint32_t							   nBytes ) const {
	const bool success = rSet.input.writePackedLocal( m_device, 0u, rBytes, nBytes );
	if ( success ) {
		Terrain3DScopedLock lock( m_statsMutex );
		++m_stats.uploadCalls;
		m_stats.bytesUploaded += nBytes;
	}
	return success;
}

PackedByteArray Terrain3DLocalComputeBufferService::readback(
	const t3dLocalComputeStorageSet_t &rSet,
	uint32_t						   nBytes ) const {
	PackedByteArray bytes = rSet.buffer.readbackLocal( m_device, 0u, nBytes );
	if ( bytes.size() == nBytes ) {
		Terrain3DScopedLock lock( m_statsMutex );
		++m_stats.readbackCalls;
		m_stats.bytesReadBack += nBytes;
	}
	return bytes;
}

PackedByteArray Terrain3DLocalComputeBufferService::readbackOutput(
	const t3dLocalComputeStoragePairSet_t &rSet,
	uint32_t							   nBytes ) const {
	PackedByteArray bytes = rSet.output.readbackLocal( m_device, 0u, nBytes );
	if ( bytes.size() == nBytes ) {
		Terrain3DScopedLock lock( m_statsMutex );
		++m_stats.readbackCalls;
		m_stats.bytesReadBack += nBytes;
	}
	return bytes;
}
