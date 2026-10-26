#include "terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_memory_domain.h"

using namespace godot;
bool Terrain3DRdGpuBuffer::isBufferType( const t3dRdResourceType_t type )
{
	return type == T3D_RD_CONTEXT_RESOURCE_UNIFORM_BUFFER || type == T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER
		|| type == T3D_RD_CONTEXT_RESOURCE_CULLING_BUFFER || type == T3D_RD_CONTEXT_RESOURCE_INDIRECT_BUFFER
		|| type == T3D_RD_CONTEXT_RESOURCE_VERTEX_BUFFER || type == T3D_RD_CONTEXT_RESOURCE_INDEX_BUFFER;
}

bool Terrain3DRdGpuBuffer::persistentMappingAvailable( RenderingDevice *const pRd )
{
	if ( !pRd )
	{
		return false;
	}
	static const StringName s_mapMethod( "buffer_persistent_map_advance" );
	static const StringName s_flushMethod( "buffer_flush" );
	return pRd->has_method( s_mapMethod ) && pRd->has_method( s_flushMethod );
}

uint8_t *Terrain3DRdGpuBuffer::persistentMapAdvance( RenderingDevice *const pRd, const RID &rBuffer )
{
	if ( !pRd || !rBuffer.is_valid() )
	{
		return NULL;
	}
	const uint64_t nAddress = pRd->buffer_persistent_map_advance( rBuffer );
	return (uint8_t *)( (uintptr_t)nAddress );
}

bool Terrain3DRdGpuBuffer::beginFrameWrite( RenderingDevice *device,
											const uint64_t frame,
											PackedByteArray &fallback,
											const bool clear_to_zero )
{
	if ( !device || !isValid() || m_frame.open || ( m_frame.attempted && frame <= m_frame.frame )
		 || ( m_bPersistent && m_frame.failed && m_frame.map_attempts && !m_frame.map_advances )
		 || ( !m_bPersistent && ( m_nCapacityBytes % 4u || fallback.size() < m_nCapacityBytes ) ) )
	{
		return false;
	}
	m_frame = {};
	m_frame.frame = frame;
	m_frame.generation = m_nGeneration;
	m_frame.attempted = true;
	if ( m_bPersistent )
	{
		++m_frame.map_attempts;
		m_pFrameData = persistentMapAdvance( device, m_hRid );
		if ( !m_pFrameData )
		{
			m_frame.failed = true;
			ERR_PRINT( "Terrain3D persistent frame mapping failed; publication is disabled until buffer recreation." );
			return false;
		}
		++m_frame.map_advances;
	}
	else
	{
		m_pFrameData = fallback.ptrw();
		m_pFrameFallback = &fallback;
	}
	m_pFrameDevice = device;
	m_frame.open = true;
	if ( clear_to_zero )
	{
		memset( m_pFrameData, 0, m_nCapacityBytes );
	}
	return true;
}

bool Terrain3DRdGpuBuffer::clearFrameSpan( const uint32_t offset, const uint32_t bytes ) const
{
	if ( !m_frame.open || m_frame.failed || m_frame.generation != m_nGeneration || !m_pFrameData
		 || ( m_pFrameFallback
			  && ( m_pFrameFallback->size() < m_nCapacityBytes || m_pFrameFallback->ptr() != m_pFrameData ) )
		 || offset > m_nCapacityBytes || bytes > m_nCapacityBytes - offset )
	{
		if ( m_frame.open )
		{
			m_frame.failed = true;
		}
		return false;
	}
	if ( bytes )
	{
		memset( m_pFrameData + offset, 0, bytes );
	}
	return true;
}

bool Terrain3DRdGpuBuffer::writeFrameSpan( const uint32_t offset, const void *source, const uint32_t bytes ) const
{
	if ( !m_frame.open || m_frame.failed || m_frame.generation != m_nGeneration || !m_pFrameData
		 || ( m_pFrameFallback
			  && ( m_pFrameFallback->size() < m_nCapacityBytes || m_pFrameFallback->ptr() != m_pFrameData ) )
		 || offset > m_nCapacityBytes || bytes > m_nCapacityBytes - offset || ( bytes && !source ) )
	{
		if ( m_frame.open )
		{
			m_frame.failed = true;
		}
		return false;
	}
	if ( bytes )
	{
		memmove( m_pFrameData + offset, source, bytes );
		++m_frame.sections_written;
		m_frame.bytes_copied += bytes;
	}
	return true;
}

bool Terrain3DRdGpuBuffer::endFrameWrite( Terrain3DRdContextAccess &context, const Terrain3DGpuBufferPolicy policy )
{
	if ( !m_frame.open )
	{
		return false;
	}
	RenderingDevice *device = context.getDevice();
	bool success =
		!m_frame.failed && device && device == m_pFrameDevice && isValid() && m_frame.generation == m_nGeneration;
	if ( success && !m_bPersistent )
	{
		success =
			m_pFrameFallback && m_pFrameFallback->size() >= m_nCapacityBytes && m_pFrameFallback->ptr() == m_pFrameData;
	}
	if ( success && m_bPersistent )
	{
		device->buffer_flush( m_hRid );
		++m_frame.flushes;
	}
	else if ( success )
	{
		++m_frame.buffer_updates;
		success = device->buffer_update( m_hRid, 0u, m_nCapacityBytes, *m_pFrameFallback ) == OK;
	}
	m_pFrameData = NULL;
	m_pFrameDevice = NULL;
	m_pFrameFallback = NULL;
	m_frame.open = false;
	m_frame.published = success;
	m_frame.failed = !success;
	if ( success )
	{
		context.recordBufferUpdate( m_nCapacityBytes, m_spec.m_owner, policy );
	}
	else
	{
		context.recordRenderEvent( m_spec.m_owner, EVENT_RESOURCE_FAILURE );
	}
	return success;
}

bool Terrain3DRdGpuBuffer::endFrameWriteWithoutSubmit( Terrain3DRdContextAccess &context )
{
	if ( !m_frame.open )
	{
		return false;
	}
	const bool success =
		!m_frame.failed && isValid() && m_pFrameDevice == context.getDevice() && m_frame.generation == m_nGeneration;
	m_pFrameData = NULL;
	m_pFrameDevice = NULL;
	m_pFrameFallback = NULL;
	m_frame.open = false;
	m_frame.published = success;
	m_frame.failed = !success;
	if ( !success )
	{
		context.recordRenderEvent( m_spec.m_owner, EVENT_RESOURCE_FAILURE );
	}
	return success;
}

RID Terrain3DRdGpuBuffer::createPhysical( RenderingDevice *const pRd,
										  const t3dRdGpuBufferSpec_t &rSpec,
										  const uint32_t nCapacityBytes,
										  const PackedByteArray &rInitialData )
{
	if ( !pRd || nCapacityBytes == 0u || !isBufferType( rSpec.m_type )
		 || (uint64_t)( rInitialData.size() ) > nCapacityBytes )
	{
		return RID();
	}
	switch ( rSpec.m_type )
	{
		case T3D_RD_CONTEXT_RESOURCE_UNIFORM_BUFFER:
			return pRd->uniform_buffer_create( nCapacityBytes, rInitialData, rSpec.m_creationBits );
		case T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER:
		case T3D_RD_CONTEXT_RESOURCE_CULLING_BUFFER:
			return pRd->storage_buffer_create( nCapacityBytes, rInitialData, rSpec.m_usage, rSpec.m_creationBits );
		case T3D_RD_CONTEXT_RESOURCE_INDIRECT_BUFFER:
			return pRd->storage_buffer_create( nCapacityBytes,
											   rInitialData,
											   BitField<RenderingDevice::StorageBufferUsage>(
												   RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT ),
											   rSpec.m_creationBits );
		case T3D_RD_CONTEXT_RESOURCE_VERTEX_BUFFER:
			return pRd->vertex_buffer_create( nCapacityBytes, rInitialData, rSpec.m_creationBits );
		case T3D_RD_CONTEXT_RESOURCE_INDEX_BUFFER: {
			const uint32_t stride = indexStride( rSpec.m_indexFormat );
			if ( stride == 0u || nCapacityBytes % stride != 0u )
			{
				return RID();
			}
			return pRd->index_buffer_create( nCapacityBytes / stride,
											 rSpec.m_indexFormat,
											 rInitialData,
											 false,
											 rSpec.m_creationBits );
		}
		default:
			break;
	}
	return RID();
}

bool Terrain3DRdGpuBuffer::registerTracked( Terrain3DRdContextAccess &rContext,
											RenderingDevice *const pRd,
											RID &rRid,
											const t3dRdGpuBufferSpec_t &rSpec,
											const uint32_t nCapacityBytes )
{
	const String name = rSpec.m_pName ? String::utf8( rSpec.m_pName ) : String();
	if ( !rRid.is_valid() || name.is_empty()
		 || !rContext.registerResource( rRid,
										name,
										rSpec.m_type,
										rSpec.m_memory,
										rSpec.m_nBackingBytes ? rSpec.m_nBackingBytes : nCapacityBytes,
										rSpec.m_owner ) )
	{
		rContext.recordRenderEvent( rSpec.m_owner, EVENT_RESOURCE_FAILURE );
		if ( pRd && rRid.is_valid() )
		{
			pRd->free_rid( rRid );
		}
		rRid = RID();
		return false;
	}
	if ( rSpec.m_memory == T3D_RD_CONTEXT_MEMORY_SCRATCH )
	{
		rContext.recordRenderCapacity( rSpec.m_owner, T3D_GPU_SCRATCH, nCapacityBytes );
	}
	else if ( rSpec.m_memory == T3D_RD_CONTEXT_MEMORY_HISTORY )
	{
		rContext.recordRenderCapacity( rSpec.m_owner, T3D_GPU_HISTORY, nCapacityBytes );
	}
	else if ( rSpec.m_memory == T3D_RD_CONTEXT_MEMORY_INDIRECT )
	{
		rContext.recordRenderCapacity( rSpec.m_owner, T3D_GPU_INDIRECT, nCapacityBytes );
	}
	rContext.recordRenderEvent( rSpec.m_owner, EVENT_BUFFER_CREATE );
	return true;
}

void Terrain3DRdGpuBuffer::publishPhysical( const RID &rRid,
											const uint32_t nCapacityBytes,
											const uint32_t nUsedBytes,
											const bool bPersistent,
											const uint32_t nGeneration )
{
	m_hRid = rRid;
	m_nCapacityBytes = nCapacityBytes;
	m_nUsedBytes = nUsedBytes;
	m_bPersistent = bPersistent;
	m_nGeneration = nGeneration;
	m_frame = {};
	m_pFrameData = NULL;
	m_pFrameDevice = NULL;
	m_pFrameFallback = NULL;
}

void Terrain3DRdGpuBuffer::clearPhysical( void )
{
	m_hRid = RID();
	m_memoryReservation.reset();
	m_nCapacityBytes = 0u;
	m_nUsedBytes = 0u;
	m_bPersistent = false;
	m_frame = {};
	m_pFrameData = NULL;
	m_pFrameDevice = NULL;
	m_pFrameFallback = NULL;
}

bool Terrain3DRdGpuBuffer::configure( const t3dRdGpuBufferSpec_t &rSpec )
{
	if ( isValid() || !isBufferType( rSpec.m_type ) )
	{
		return false;
	}
	m_spec = rSpec;
	m_bConfigured = true;
	return true;
}

bool Terrain3DRdGpuBuffer::createTracked( Terrain3DRdContextAccess &rContext,
										  const uint32_t nCapacityBytes,
										  const PackedByteArray &rInitialData )
{
	if ( !m_bConfigured || isValid() || !m_spec.m_pName || !m_spec.m_pName[0] || nCapacityBytes == 0u
		 || ( m_spec.m_nBackingBytes && m_spec.m_nBackingBytes < nCapacityBytes ) )
	{
		return false;
	}
	RenderingDevice *const rd = rContext.getDevice();
	Terrain3DMemoryReservation reservation;
	const uint64_t backing_bytes = m_spec.m_nBackingBytes ? m_spec.m_nBackingBytes : nCapacityBytes;
	const auto domain = t3dRdMemoryDomain( m_spec.m_owner, m_spec.m_memory );
	if ( !reservation.resize( backing_bytes, Terrain3DMemoryAccount::GPU, g_t3dMemoryAccount, domain ) )
	{
		const auto stats = g_t3dMemoryAccount->stats();
		const auto &bucket = stats.domains[domain];
		ERR_PRINT( String( "Terrain3D buffer memory budget denied: " ) + m_spec.m_pName + " requested_bytes="
				   + String::num_uint64( backing_bytes ) + " committed_bytes=" + String::num_uint64( stats.committed )
				   + " total_limit_bytes=" + String::num_uint64( stats.limit )
				   + " domain_committed_bytes=" + String::num_uint64( bucket.committed )
				   + " domain_limit_bytes=" + String::num_uint64( bucket.limit ) );
		return false;
	}
	RID created = createPhysical( rd, m_spec, nCapacityBytes, rInitialData );
	if ( !created.is_valid() )
	{
		ERR_PRINT( String( "Godot buffer allocation failed: " ) + m_spec.m_pName
				   + " capacity_bytes=" + String::num_uint64( nCapacityBytes ) );
	}
	if ( !registerTracked( rContext, rd, created, m_spec, nCapacityBytes ) )
	{
		return false;
	}
	m_memoryReservation.swap( reservation );
	const bool persistent = m_spec.m_creationBits.has_flag( RenderingDevice::BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT )
		&& persistentMappingAvailable( rd );
	const uint32_t initial_used = rInitialData.size() > 0 ? (uint32_t)rInitialData.size() : 0u;
	publishPhysical( created, nCapacityBytes, initial_used, persistent, t3dRdGpuBufferNextGeneration( m_nGeneration ) );
	if ( initial_used != 0u )
	{
		rContext.recordBufferUpdate( initial_used, m_spec.m_owner, T3D_GPU_PUBLICATION );
	}
	return true;
}

bool Terrain3DRdGpuBuffer::replaceTracked( Terrain3DRdContextAccess &rContext,
										   const uint32_t nCapacityBytes,
										   const PackedByteArray &rInitialData )
{
	if ( !m_bConfigured || nCapacityBytes == 0u || m_frame.open )
	{
		return false;
	}
	Terrain3DRdGpuBuffer replacement;
	if ( !replacement.configure( m_spec ) || !replacement.createTracked( rContext, nCapacityBytes, rInitialData ) )
	{
		return false;
	}
	commitTracked( rContext, replacement );
	return true;
}

bool Terrain3DRdGpuBuffer::ensureTracked( Terrain3DRdContextAccess &rContext, const uint32_t nRequiredBytes )
{
	if ( !m_bConfigured )
	{
		return false;
	}
	if ( !needsCapacity( nRequiredBytes ) )
	{
		return true;
	}
	const uint32_t base = MAX( m_nCapacityBytes, m_spec.m_nMinimumCapacityBytes );
	uint32_t capacity = 0u;
	if ( !t3dRdGpuBufferGrownCapacity( base, nRequiredBytes, m_spec.m_growth, capacity ) || capacity == 0u )
	{
		return false;
	}
	return replaceTracked( rContext, capacity );
}

bool Terrain3DRdGpuBuffer::createLocal( RenderingDevice *const pDevice,
										const uint32_t nCapacityBytes,
										const PackedByteArray &rInitialData )
{
	if ( !m_bConfigured || isValid() || !pDevice || nCapacityBytes == 0u
		 || ( m_spec.m_nBackingBytes && m_spec.m_nBackingBytes < nCapacityBytes ) )
	{
		return false;
	}
	Terrain3DMemoryReservation reservation;
	if ( !reservation.resize( m_spec.m_nBackingBytes ? m_spec.m_nBackingBytes : nCapacityBytes,
							  Terrain3DMemoryAccount::GPU,
							  g_t3dMemoryAccount,
							  t3dRdMemoryDomain( m_spec.m_owner, m_spec.m_memory ) ) )
	{
		return false;
	}
	RID created = createPhysical( pDevice, m_spec, nCapacityBytes, rInitialData );
	if ( !created.is_valid() )
	{
		return false;
	}
	const bool persistent = m_spec.m_creationBits.has_flag( RenderingDevice::BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT )
		&& persistentMappingAvailable( pDevice );
	publishPhysical( created,
					 nCapacityBytes,
					 rInitialData.size() > 0 ? (uint32_t)rInitialData.size() : 0u,
					 persistent,
					 t3dRdGpuBufferNextGeneration( m_nGeneration ) );
	m_memoryReservation.swap( reservation );
	return true;
}

bool Terrain3DRdGpuBuffer::replaceLocal( RenderingDevice *const pDevice,
										 const uint32_t nCapacityBytes,
										 const PackedByteArray &rInitialData )
{
	if ( !m_bConfigured || !pDevice || nCapacityBytes == 0u || m_frame.open )
	{
		return false;
	}
	Terrain3DRdGpuBuffer replacement;
	if ( !replacement.configure( m_spec ) || !replacement.createLocal( pDevice, nCapacityBytes, rInitialData ) )
	{
		return false;
	}
	replacement.m_nGeneration = t3dRdGpuBufferNextGeneration( m_nGeneration );
	swap( replacement );
	replacement.releaseLocal( pDevice );
	return true;
}

bool Terrain3DRdGpuBuffer::ensureLocal( RenderingDevice *const pDevice, const uint32_t nRequiredBytes )
{
	if ( !m_bConfigured )
	{
		return false;
	}
	if ( !needsCapacity( nRequiredBytes ) )
	{
		return true;
	}
	const uint32_t base = MAX( m_nCapacityBytes, m_spec.m_nMinimumCapacityBytes );
	uint32_t capacity = 0u;
	if ( !t3dRdGpuBufferGrownCapacity( base, nRequiredBytes, m_spec.m_growth, capacity ) || capacity == 0u )
	{
		return false;
	}
	return replaceLocal( pDevice, capacity );
}

void Terrain3DRdGpuBuffer::releaseTracked( Terrain3DRdContextAccess &rContext )
{
	ERR_FAIL_COND( m_frame.open );
	if ( m_hRid.is_valid() )
	{
		rContext.freeResource( m_hRid, m_spec.m_nBackingBytes ? m_spec.m_nBackingBytes : m_nCapacityBytes );
	}
	clearPhysical();
}

void Terrain3DRdGpuBuffer::releaseLocal( RenderingDevice *const pDevice )
{
	ERR_FAIL_COND( m_frame.open );
	if ( pDevice && m_hRid.is_valid() )
	{
		pDevice->free_rid( m_hRid );
	}
	clearPhysical();
}

bool Terrain3DRdGpuBuffer::writeTrackedForOwner( Terrain3DRdContextAccess &rContext,
												 const uint32_t nOffsetBytes,
												 const void *const pSource,
												 const uint32_t nBytes,
												 PackedByteArray &rFallbackScratch,
												 const t3dRdResourceOwner_t owner,
												 const Terrain3DGpuBufferPolicy policy ) const
{
	if ( nBytes == 0u )
	{
		return true;
	}
	const bool success = writeLocal( rContext.getDevice(), nOffsetBytes, pSource, nBytes, rFallbackScratch );
	if ( success && !m_frame.open )
	{
		rContext.recordBufferUpdate( nBytes, owner, policy );
	}
	return success;
}

bool Terrain3DRdGpuBuffer::writeTracked( Terrain3DRdContextAccess &rContext,
										 const uint32_t nOffsetBytes,
										 const void *const pSource,
										 const uint32_t nBytes,
										 PackedByteArray &rFallbackScratch,
										 const Terrain3DGpuBufferPolicy policy ) const
{
	return writeTrackedForOwner( rContext, nOffsetBytes, pSource, nBytes, rFallbackScratch, m_spec.m_owner, policy );
}

bool Terrain3DRdGpuBuffer::writePackedTrackedForOwner( Terrain3DRdContextAccess &rContext,
													   const uint32_t nOffsetBytes,
													   const PackedByteArray &rBytes,
													   const uint32_t nBytes,
													   const t3dRdResourceOwner_t owner,
													   const Terrain3DGpuBufferPolicy policy ) const
{
	if ( nBytes == 0u )
	{
		return true;
	}
	const bool success = writePackedLocal( rContext.getDevice(), nOffsetBytes, rBytes, nBytes );
	if ( success && !m_frame.open )
	{
		rContext.recordBufferUpdate( nBytes, owner, policy );
	}
	return success;
}

bool Terrain3DRdGpuBuffer::writePackedTracked( Terrain3DRdContextAccess &rContext,
											   const uint32_t nOffsetBytes,
											   const PackedByteArray &rBytes,
											   const uint32_t nBytes,
											   const Terrain3DGpuBufferPolicy policy ) const
{
	return writePackedTrackedForOwner( rContext, nOffsetBytes, rBytes, nBytes, m_spec.m_owner, policy );
}

bool Terrain3DRdGpuBuffer::writeLocal( RenderingDevice *const pDevice,
									   const uint32_t nOffsetBytes,
									   const void *const pSource,
									   const uint32_t nBytes,
									   PackedByteArray &rFallbackScratch ) const
{
	if ( nBytes == 0u )
	{
		return true;
	}
	if ( m_frame.open )
	{
		if ( pDevice != m_pFrameDevice )
		{
			m_frame.failed = true;
			return false;
		}
		return writeFrameSpan( nOffsetBytes, pSource, nBytes );
	}
	if ( !pDevice || !isValid() || !pSource || nOffsetBytes > m_nCapacityBytes
		 || nBytes > m_nCapacityBytes - nOffsetBytes )
	{
		return false;
	}
	if ( m_bPersistent || rFallbackScratch.size() < nBytes )
	{
		return false;
	}
	if ( pSource != rFallbackScratch.ptr() )
	{
		memmove( rFallbackScratch.ptrw(), pSource, nBytes );
	}
	return pDevice->buffer_update( m_hRid, nOffsetBytes, nBytes, rFallbackScratch ) == OK;
}

bool Terrain3DRdGpuBuffer::writePackedLocal( RenderingDevice *const pDevice,
											 const uint32_t nOffsetBytes,
											 const PackedByteArray &rBytes,
											 const uint32_t nBytes ) const
{
	if ( nBytes == 0u )
	{
		return true;
	}
	if ( m_frame.open )
	{
		if ( pDevice != m_pFrameDevice || rBytes.size() < nBytes )
		{
			m_frame.failed = true;
			return false;
		}
		return writeFrameSpan( nOffsetBytes, rBytes.ptr(), nBytes );
	}
	if ( !pDevice || !isValid() || rBytes.size() < nBytes || nOffsetBytes > m_nCapacityBytes
		 || nBytes > m_nCapacityBytes - nOffsetBytes )
	{
		return false;
	}
	return !m_bPersistent && pDevice->buffer_update( m_hRid, nOffsetBytes, nBytes, rBytes ) == OK;
}

PackedByteArray Terrain3DRdGpuBuffer::readbackTracked( Terrain3DRdContextAccess &rContext,
													   const uint32_t nOffsetBytes,
													   const uint32_t nBytes ) const
{
	RenderingDevice *const rd = rContext.getDevice();
	const uint32_t bytes = nBytes == 0u ? m_nCapacityBytes - MIN( nOffsetBytes, m_nCapacityBytes ) : nBytes;
	if ( !rd || !isValid() || nOffsetBytes > m_nCapacityBytes || bytes > m_nCapacityBytes - nOffsetBytes )
	{
		return PackedByteArray();
	}
	return rd->buffer_get_data( m_hRid, nOffsetBytes, bytes );
}

PackedByteArray Terrain3DRdGpuBuffer::readbackLocal( RenderingDevice *const pDevice,
													 const uint32_t nOffsetBytes,
													 const uint32_t nBytes ) const
{
	const uint32_t bytes = nBytes == 0u ? m_nCapacityBytes - MIN( nOffsetBytes, m_nCapacityBytes ) : nBytes;
	if ( !pDevice || !isValid() || nOffsetBytes > m_nCapacityBytes || bytes > m_nCapacityBytes - nOffsetBytes )
	{
		return PackedByteArray();
	}
	return pDevice->buffer_get_data( m_hRid, nOffsetBytes, bytes );
}

bool Terrain3DRdGpuBuffer::slice( const uint32_t nOffsetBytes,
								  const uint32_t nSizeBytes,
								  const uint32_t nAlignmentBytes,
								  t3dRdGpuBufferSlice_t &rSlice ) const
{
	if ( !isValid() || nAlignmentBytes == 0u || ( nAlignmentBytes & ( nAlignmentBytes - 1u ) ) != 0u
		 || nOffsetBytes % nAlignmentBytes != 0u || nOffsetBytes > m_nCapacityBytes
		 || nSizeBytes > m_nCapacityBytes - nOffsetBytes )
	{
		return false;
	}
	rSlice = { m_hRid, nOffsetBytes, nSizeBytes, nSizeBytes, nAlignmentBytes, m_nGeneration };
	return true;
}

bool Terrain3DRdGpuBuffer::ownsSlice( const t3dRdGpuBufferSlice_t &rSlice ) const
{
	return isValid() && rSlice.m_hBuffer == m_hRid && rSlice.m_nGeneration == m_nGeneration
		&& rSlice.m_nAlignmentBytes != 0u && ( rSlice.m_nAlignmentBytes & ( rSlice.m_nAlignmentBytes - 1u ) ) == 0u
		&& rSlice.m_nOffsetBytes % rSlice.m_nAlignmentBytes == 0u && rSlice.m_nOffsetBytes <= m_nCapacityBytes
		&& rSlice.m_nCapacityBytes <= m_nCapacityBytes - rSlice.m_nOffsetBytes
		&& rSlice.m_nSizeBytes <= rSlice.m_nCapacityBytes;
}

bool Terrain3DRdGpuBuffer::reserveSlice( uint32_t &rCursorBytes,
										 const uint32_t nSizeBytes,
										 const uint32_t nAlignmentBytes,
										 t3dRdGpuBufferSlice_t &rSlice ) const
{
	uint32_t offset = 0u;
	uint32_t next = 0u;
	if ( !isValid()
		 || !t3dRdGpuBufferReserveOffset( rCursorBytes, nSizeBytes, nAlignmentBytes, m_nCapacityBytes, offset, next ) )
	{
		return false;
	}
	rSlice = { m_hRid, offset, nSizeBytes, nSizeBytes, nAlignmentBytes, m_nGeneration };
	rCursorBytes = next;
	return true;
}

bool Terrain3DRdGpuBuffer::writeSliceTracked( Terrain3DRdContextAccess &rContext,
											  const t3dRdGpuBufferSlice_t &rSlice,
											  const uint32_t nRelativeOffset,
											  const void *const pSource,
											  const uint32_t nBytes,
											  PackedByteArray &rFallbackScratch,
											  const Terrain3DGpuBufferPolicy policy,
											  const t3dRdResourceOwner_t owner ) const
{
	if ( !ownsSlice( rSlice ) || nRelativeOffset > rSlice.m_nCapacityBytes
		 || nBytes > rSlice.m_nCapacityBytes - nRelativeOffset || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset )
	{
		return false;
	}
	const t3dRdResourceOwner_t resolved_owner = owner == T3D_RD_CONTEXT_OWNER_UNSPECIFIED ? m_spec.m_owner : owner;
	return writeTrackedForOwner( rContext,
								 rSlice.m_nOffsetBytes + nRelativeOffset,
								 pSource,
								 nBytes,
								 rFallbackScratch,
								 resolved_owner,
								 policy );
}

bool Terrain3DRdGpuBuffer::writePackedSliceTracked( Terrain3DRdContextAccess &rContext,
													const t3dRdGpuBufferSlice_t &rSlice,
													const uint32_t nRelativeOffset,
													const PackedByteArray &rBytes,
													const uint32_t nBytes,
													const Terrain3DGpuBufferPolicy policy,
													const t3dRdResourceOwner_t owner ) const
{
	if ( !ownsSlice( rSlice ) || nRelativeOffset > rSlice.m_nCapacityBytes
		 || nBytes > rSlice.m_nCapacityBytes - nRelativeOffset || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset )
	{
		return false;
	}
	const t3dRdResourceOwner_t resolved_owner = owner == T3D_RD_CONTEXT_OWNER_UNSPECIFIED ? m_spec.m_owner : owner;
	return writePackedTrackedForOwner( rContext,
									   rSlice.m_nOffsetBytes + nRelativeOffset,
									   rBytes,
									   nBytes,
									   resolved_owner,
									   policy );
}

PackedByteArray Terrain3DRdGpuBuffer::readbackSliceTracked( Terrain3DRdContextAccess &rContext,
															const t3dRdGpuBufferSlice_t &rSlice,
															const uint32_t nRelativeOffset,
															const uint32_t nBytes ) const
{
	if ( !ownsSlice( rSlice ) || nRelativeOffset > rSlice.m_nCapacityBytes
		 || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset )
	{
		return PackedByteArray();
	}
	const uint32_t bytes = nBytes == 0u ? rSlice.m_nCapacityBytes - nRelativeOffset : nBytes;
	if ( bytes > rSlice.m_nCapacityBytes - nRelativeOffset )
	{
		return PackedByteArray();
	}
	return readbackTracked( rContext, rSlice.m_nOffsetBytes + nRelativeOffset, bytes );
}

bool Terrain3DRdGpuBuffer::setSliceUsedBytes( t3dRdGpuBufferSlice_t &rSlice, const uint32_t nBytes ) const
{
	if ( !ownsSlice( rSlice ) || nBytes > rSlice.m_nCapacityBytes )
	{
		return false;
	}
	rSlice.m_nSizeBytes = nBytes;
	return true;
}

bool Terrain3DRdGpuBuffer::setUsedBytes( const uint32_t nBytes )
{
	if ( nBytes > m_nCapacityBytes )
	{
		return false;
	}
	m_nUsedBytes = nBytes;
	return true;
}

void Terrain3DRdGpuBuffer::swap( Terrain3DRdGpuBuffer &rOther )
{
	ERR_FAIL_COND( m_frame.open || rOther.m_frame.open );

	eastl::swap( m_frame, rOther.m_frame );
	m_memoryReservation.swap( rOther.m_memoryReservation );
	eastl::swap( m_spec, rOther.m_spec );
	// Transfer the physical RID exactly once. A second swap leaves the
	// destination with the old (potentially invalid) RID while its capacity,
	// generation, and memory reservation have already been transferred.
	eastl::swap( m_hRid, rOther.m_hRid );
	eastl::swap( m_nCapacityBytes, rOther.m_nCapacityBytes );
	eastl::swap( m_nUsedBytes, rOther.m_nUsedBytes );
	eastl::swap( m_nGeneration, rOther.m_nGeneration );
	eastl::swap( m_bConfigured, rOther.m_bConfigured );
	eastl::swap( m_bPersistent, rOther.m_bPersistent );
}
