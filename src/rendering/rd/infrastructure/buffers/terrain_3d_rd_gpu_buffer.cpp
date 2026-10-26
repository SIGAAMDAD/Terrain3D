#include "terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"

using namespace godot;
bool Terrain3DRdGpuBuffer::isBufferType( const t3dRdResourceType_t type ) {
		return type == T3D_RD_CONTEXT_RESOURCE_UNIFORM_BUFFER
		        || type == T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER
		        || type == T3D_RD_CONTEXT_RESOURCE_CULLING_BUFFER
		        || type == T3D_RD_CONTEXT_RESOURCE_INDIRECT_BUFFER
		        || type == T3D_RD_CONTEXT_RESOURCE_VERTEX_BUFFER
		        || type == T3D_RD_CONTEXT_RESOURCE_INDEX_BUFFER;
	}

bool Terrain3DRdGpuBuffer::persistentMappingAvailable( RenderingDevice *const pRd ) {
		if ( !pRd ) { return false; }
		static const StringName s_mapMethod( "buffer_persistent_map_advance" );
		static const StringName s_flushMethod( "buffer_flush" );
		return pRd->has_method( s_mapMethod ) && pRd->has_method( s_flushMethod );
	}

uint8_t *Terrain3DRdGpuBuffer::persistentMapAdvance( RenderingDevice *const pRd, const RID &rBuffer ) {
		if ( !pRd || !rBuffer.is_valid() ) { return NULL; }
		const uint64_t nAddress = pRd->buffer_persistent_map_advance( rBuffer );
		return ( uint8_t * )( ( uintptr_t )nAddress );
	}

bool Terrain3DRdGpuBuffer::persistentWrite( RenderingDevice *const pRd,
	        const RID &rBuffer,
	        const void *const pSource,
	        const uint32_t nBytes,
	        const uint32_t nOffset ) {
		if ( !pSource || nBytes == 0u ) { return true; }
		uint8_t *const pMapped = persistentMapAdvance( pRd, rBuffer );
		if ( !pMapped ) { return false; }
		memcpy( pMapped + nOffset, pSource, nBytes );
		pRd->buffer_flush( rBuffer );
		return true;
	}

RID Terrain3DRdGpuBuffer::createPhysical( RenderingDevice *const pRd,
	        const t3dRdGpuBufferSpec_t &rSpec,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData ) {
		if ( !pRd || nCapacityBytes == 0u || !isBufferType( rSpec.m_type )
		        || ( uint64_t )( rInitialData.size() ) > nCapacityBytes ) {
			return RID();
		}
		switch ( rSpec.m_type ) {
			case T3D_RD_CONTEXT_RESOURCE_UNIFORM_BUFFER:
				return pRd->uniform_buffer_create( nCapacityBytes, rInitialData, rSpec.m_creationBits );
			case T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER:
			case T3D_RD_CONTEXT_RESOURCE_CULLING_BUFFER:
				return pRd->storage_buffer_create( nCapacityBytes, rInitialData, rSpec.m_usage, rSpec.m_creationBits );
			case T3D_RD_CONTEXT_RESOURCE_INDIRECT_BUFFER:
				return pRd->storage_buffer_create( nCapacityBytes, rInitialData,
				        BitField<RenderingDevice::StorageBufferUsage>( RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT ),
				        rSpec.m_creationBits );
			case T3D_RD_CONTEXT_RESOURCE_VERTEX_BUFFER:
				return pRd->vertex_buffer_create( nCapacityBytes, rInitialData, rSpec.m_creationBits );
			case T3D_RD_CONTEXT_RESOURCE_INDEX_BUFFER: {
				const uint32_t stride = indexStride( rSpec.m_indexFormat );
				if ( stride == 0u || nCapacityBytes % stride != 0u ) {
					return RID();
				}
				return pRd->index_buffer_create( nCapacityBytes / stride, rSpec.m_indexFormat, rInitialData, false, rSpec.m_creationBits );
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
	        const uint32_t nCapacityBytes ) {
		const String name = rSpec.m_pName ? String::utf8( rSpec.m_pName ) : String();
		if ( !rRid.is_valid() || name.is_empty()
		        || !rContext.registerResource( rRid, name, rSpec.m_type, rSpec.m_memory, nCapacityBytes, rSpec.m_owner ) ) {
			rContext.recordRenderEvent( rSpec.m_owner, EVENT_RESOURCE_FAILURE );
			if ( pRd && rRid.is_valid() ) {
				pRd->free_rid( rRid );
			}
			rRid = RID();
			return false;
		}
		if ( rSpec.m_memory == T3D_RD_CONTEXT_MEMORY_SCRATCH ) {
			rContext.recordRenderCapacity( rSpec.m_owner, T3D_GPU_SCRATCH, nCapacityBytes );
		} else if ( rSpec.m_memory == T3D_RD_CONTEXT_MEMORY_HISTORY ) {
			rContext.recordRenderCapacity( rSpec.m_owner, T3D_GPU_HISTORY, nCapacityBytes );
		} else if ( rSpec.m_memory == T3D_RD_CONTEXT_MEMORY_INDIRECT ) {
			rContext.recordRenderCapacity( rSpec.m_owner, T3D_GPU_INDIRECT, nCapacityBytes );
		}
		rContext.recordRenderEvent( rSpec.m_owner, EVENT_BUFFER_CREATE );
		return true;
	}

void Terrain3DRdGpuBuffer::publishPhysical( const RID &rRid,
	        const uint32_t nCapacityBytes,
	        const uint32_t nUsedBytes,
	        const bool bPersistent,
	        const uint32_t nGeneration ) {
		m_hRid = rRid;
		m_nCapacityBytes = nCapacityBytes;
		m_nUsedBytes = nUsedBytes;
		m_bPersistent = bPersistent;
		m_nGeneration = nGeneration;
	}

void Terrain3DRdGpuBuffer::clearPhysical( void ) {
		m_hRid = RID();
		m_nCapacityBytes = 0u;
		m_nUsedBytes = 0u;
		m_bPersistent = false;
	}

bool Terrain3DRdGpuBuffer::configure( const t3dRdGpuBufferSpec_t &rSpec ) {
		if ( isValid() || !isBufferType( rSpec.m_type ) ) {
			return false;
		}
		m_spec = rSpec;
		m_bConfigured = true;
		return true;
	}

bool Terrain3DRdGpuBuffer::createTracked( Terrain3DRdContextAccess &rContext,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData ) {
		if ( !m_bConfigured || isValid() || !m_spec.m_pName || !m_spec.m_pName[0] || nCapacityBytes == 0u ) {
			return false;
		}
		RenderingDevice *const rd = rContext.getDevice();
		RID created = createPhysical( rd, m_spec, nCapacityBytes, rInitialData );
		if ( !registerTracked( rContext, rd, created, m_spec, nCapacityBytes ) ) {
			return false;
		}
		const bool persistent = m_spec.m_creationBits.has_flag( RenderingDevice::BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT )
		        && persistentMappingAvailable( rd );
		const uint32_t initial_used = rInitialData.size() > 0 ? ( uint32_t )rInitialData.size() : 0u;
		publishPhysical( created, nCapacityBytes, initial_used, persistent,
		        t3dRdGpuBufferNextGeneration( m_nGeneration ) );
		return true;
	}

bool Terrain3DRdGpuBuffer::replaceTracked( Terrain3DRdContextAccess &rContext,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData ) {
		if ( !m_bConfigured || nCapacityBytes == 0u ) {
			return false;
		}
		Terrain3DRdGpuBuffer replacement;
		if ( !replacement.configure( m_spec ) || !replacement.createTracked( rContext, nCapacityBytes, rInitialData ) ) {
			return false;
		}
		replacement.m_nGeneration = t3dRdGpuBufferNextGeneration( m_nGeneration );
		const bool rebuilding = isValid();
		swap( replacement );
		if ( rebuilding ) {
			rContext.recordRenderEvent( m_spec.m_owner, EVENT_BUFFER_REBUILD );
		}
		replacement.releaseTracked( rContext );
		return true;
	}

bool Terrain3DRdGpuBuffer::ensureTracked( Terrain3DRdContextAccess &rContext, const uint32_t nRequiredBytes ) {
		if ( !m_bConfigured ) {
			return false;
		}
		if ( !needsCapacity( nRequiredBytes ) ) {
			return true;
		}
		const uint32_t base = MAX( m_nCapacityBytes, m_spec.m_nMinimumCapacityBytes );
		uint32_t capacity = 0u;
		if ( !t3dRdGpuBufferGrownCapacity( base, nRequiredBytes, m_spec.m_growth, capacity ) || capacity == 0u ) {
			return false;
		}
		return replaceTracked( rContext, capacity );
	}

bool Terrain3DRdGpuBuffer::createLocal( RenderingDevice *const pDevice,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData ) {
		if ( !m_bConfigured || isValid() || !pDevice || nCapacityBytes == 0u ) {
			return false;
		}
		RID created = createPhysical( pDevice, m_spec, nCapacityBytes, rInitialData );
		if ( !created.is_valid() ) {
			return false;
		}
		const bool persistent = m_spec.m_creationBits.has_flag( RenderingDevice::BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT )
		        && persistentMappingAvailable( pDevice );
		publishPhysical( created, nCapacityBytes, rInitialData.size() > 0 ? ( uint32_t )rInitialData.size() : 0u,
		        persistent, t3dRdGpuBufferNextGeneration( m_nGeneration ) );
		return true;
	}

bool Terrain3DRdGpuBuffer::replaceLocal( RenderingDevice *const pDevice,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData ) {
		if ( !m_bConfigured || !pDevice || nCapacityBytes == 0u ) {
			return false;
		}
		Terrain3DRdGpuBuffer replacement;
		if ( !replacement.configure( m_spec ) || !replacement.createLocal( pDevice, nCapacityBytes, rInitialData ) ) {
			return false;
		}
		replacement.m_nGeneration = t3dRdGpuBufferNextGeneration( m_nGeneration );
		swap( replacement );
		replacement.releaseLocal( pDevice );
		return true;
	}

bool Terrain3DRdGpuBuffer::ensureLocal( RenderingDevice *const pDevice, const uint32_t nRequiredBytes ) {
		if ( !m_bConfigured ) {
			return false;
		}
		if ( !needsCapacity( nRequiredBytes ) ) {
			return true;
		}
		const uint32_t base = MAX( m_nCapacityBytes, m_spec.m_nMinimumCapacityBytes );
		uint32_t capacity = 0u;
		if ( !t3dRdGpuBufferGrownCapacity( base, nRequiredBytes, m_spec.m_growth, capacity ) || capacity == 0u ) {
			return false;
		}
		return replaceLocal( pDevice, capacity );
	}

void Terrain3DRdGpuBuffer::releaseTracked( Terrain3DRdContextAccess &rContext ) {
		if ( m_hRid.is_valid() ) {
			rContext.freeResource( m_hRid, m_nCapacityBytes );
		}
		clearPhysical();
	}

void Terrain3DRdGpuBuffer::releaseLocal( RenderingDevice *const pDevice ) {
		if ( pDevice && m_hRid.is_valid() ) {
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
	        const Terrain3DGpuBufferPolicy policy ) const {
		if ( nBytes == 0u ) { return true; }
		RenderingDevice *const rd = rContext.getDevice();
		if ( !rd || !isValid() || !pSource || nOffsetBytes > m_nCapacityBytes || nBytes > m_nCapacityBytes - nOffsetBytes ) {
			return false;
		}
		bool success = false;
		if ( m_bPersistent ) {
			success = persistentWrite( rd, m_hRid, pSource, nBytes, nOffsetBytes );
		} else if ( rFallbackScratch.size() >= nBytes ) {
			if ( pSource != rFallbackScratch.ptr() ) {
				memcpy( rFallbackScratch.ptrw(), pSource, nBytes );
			}
			success = rd->buffer_update( m_hRid, nOffsetBytes, nBytes, rFallbackScratch ) == OK;
		}
		if ( success ) {
			rContext.recordBufferUpdate( nBytes, owner, policy );
		}
		return success;
	}

bool Terrain3DRdGpuBuffer::writeTracked( Terrain3DRdContextAccess &rContext,
	        const uint32_t nOffsetBytes,
	        const void *const pSource,
	        const uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        const Terrain3DGpuBufferPolicy policy ) const {
		return writeTrackedForOwner( rContext, nOffsetBytes, pSource, nBytes, rFallbackScratch, m_spec.m_owner, policy );
	}

bool Terrain3DRdGpuBuffer::writePackedTrackedForOwner( Terrain3DRdContextAccess &rContext,
	        const uint32_t nOffsetBytes,
	        const PackedByteArray &rBytes,
	        const uint32_t nBytes,
	        const t3dRdResourceOwner_t owner,
	        const Terrain3DGpuBufferPolicy policy ) const {
		if ( nBytes == 0u ) { return true; }
		RenderingDevice *const rd = rContext.getDevice();
		if ( !rd || !isValid() || rBytes.size() < nBytes || nOffsetBytes > m_nCapacityBytes
		        || nBytes > m_nCapacityBytes - nOffsetBytes ) {
			return false;
		}
		const bool success = m_bPersistent ? persistentWrite( rd, m_hRid, rBytes.ptr(), nBytes, nOffsetBytes )
		                                  : rd->buffer_update( m_hRid, nOffsetBytes, nBytes, rBytes ) == OK;
		if ( success ) {
			rContext.recordBufferUpdate( nBytes, owner, policy );
		}
		return success;
	}

bool Terrain3DRdGpuBuffer::writePackedTracked( Terrain3DRdContextAccess &rContext,
	        const uint32_t nOffsetBytes,
	        const PackedByteArray &rBytes,
	        const uint32_t nBytes,
	        const Terrain3DGpuBufferPolicy policy ) const {
		return writePackedTrackedForOwner( rContext, nOffsetBytes, rBytes, nBytes, m_spec.m_owner, policy );
	}

bool Terrain3DRdGpuBuffer::writeLocal( RenderingDevice *const pDevice,
	        const uint32_t nOffsetBytes,
	        const void *const pSource,
	        const uint32_t nBytes,
	        PackedByteArray &rFallbackScratch ) const {
		if ( nBytes == 0u ) { return true; }
		if ( !pDevice || !isValid() || !pSource || nOffsetBytes > m_nCapacityBytes || nBytes > m_nCapacityBytes - nOffsetBytes ) {
			return false;
		}
		if ( m_bPersistent ) {
			return persistentWrite( pDevice, m_hRid, pSource, nBytes, nOffsetBytes );
		}
		if ( rFallbackScratch.size() < nBytes ) { return false; }
		if ( pSource != rFallbackScratch.ptr() ) {
			memcpy( rFallbackScratch.ptrw(), pSource, nBytes );
		}
		return pDevice->buffer_update( m_hRid, nOffsetBytes, nBytes, rFallbackScratch ) == OK;
	}

bool Terrain3DRdGpuBuffer::writePackedLocal( RenderingDevice *const pDevice,
	        const uint32_t nOffsetBytes,
	        const PackedByteArray &rBytes,
	        const uint32_t nBytes ) const {
		if ( nBytes == 0u ) { return true; }
		if ( !pDevice || !isValid() || rBytes.size() < nBytes || nOffsetBytes > m_nCapacityBytes
		        || nBytes > m_nCapacityBytes - nOffsetBytes ) {
			return false;
		}
		return m_bPersistent ? persistentWrite( pDevice, m_hRid, rBytes.ptr(), nBytes, nOffsetBytes )
		                     : pDevice->buffer_update( m_hRid, nOffsetBytes, nBytes, rBytes ) == OK;
	}

PackedByteArray Terrain3DRdGpuBuffer::readbackTracked( Terrain3DRdContextAccess &rContext,
	        const uint32_t nOffsetBytes,
	        const uint32_t nBytes ) const {
		RenderingDevice *const rd = rContext.getDevice();
		const uint32_t bytes = nBytes == 0u ? m_nCapacityBytes - MIN( nOffsetBytes, m_nCapacityBytes ) : nBytes;
		if ( !rd || !isValid() || nOffsetBytes > m_nCapacityBytes || bytes > m_nCapacityBytes - nOffsetBytes ) {
			return PackedByteArray();
		}
		return rd->buffer_get_data( m_hRid, nOffsetBytes, bytes );
	}

PackedByteArray Terrain3DRdGpuBuffer::readbackLocal( RenderingDevice *const pDevice,
	        const uint32_t nOffsetBytes,
	        const uint32_t nBytes ) const {
		const uint32_t bytes = nBytes == 0u ? m_nCapacityBytes - MIN( nOffsetBytes, m_nCapacityBytes ) : nBytes;
		if ( !pDevice || !isValid() || nOffsetBytes > m_nCapacityBytes || bytes > m_nCapacityBytes - nOffsetBytes ) {
			return PackedByteArray();
		}
		return pDevice->buffer_get_data( m_hRid, nOffsetBytes, bytes );
	}

bool Terrain3DRdGpuBuffer::slice( const uint32_t nOffsetBytes,
	        const uint32_t nSizeBytes,
	        const uint32_t nAlignmentBytes,
	        t3dRdGpuBufferSlice_t &rSlice ) const {
		if ( !isValid() || nAlignmentBytes == 0u || ( nAlignmentBytes & ( nAlignmentBytes - 1u ) ) != 0u
		        || nOffsetBytes % nAlignmentBytes != 0u || nOffsetBytes > m_nCapacityBytes
		        || nSizeBytes > m_nCapacityBytes - nOffsetBytes ) {
			return false;
		}
		rSlice = { m_hRid, nOffsetBytes, nSizeBytes, nSizeBytes, nAlignmentBytes, m_nGeneration };
		return true;
	}

bool Terrain3DRdGpuBuffer::ownsSlice( const t3dRdGpuBufferSlice_t &rSlice ) const {
		return isValid() && rSlice.m_hBuffer == m_hRid && rSlice.m_nGeneration == m_nGeneration
		        && rSlice.m_nAlignmentBytes != 0u
		        && ( rSlice.m_nAlignmentBytes & ( rSlice.m_nAlignmentBytes - 1u ) ) == 0u
		        && rSlice.m_nOffsetBytes % rSlice.m_nAlignmentBytes == 0u
		        && rSlice.m_nOffsetBytes <= m_nCapacityBytes
		        && rSlice.m_nCapacityBytes <= m_nCapacityBytes - rSlice.m_nOffsetBytes
		        && rSlice.m_nSizeBytes <= rSlice.m_nCapacityBytes;
	}

bool Terrain3DRdGpuBuffer::reserveSlice( uint32_t &rCursorBytes,
	        const uint32_t nSizeBytes,
	        const uint32_t nAlignmentBytes,
	        t3dRdGpuBufferSlice_t &rSlice ) const {
		uint32_t offset = 0u;
		uint32_t next = 0u;
		if ( !isValid() || !t3dRdGpuBufferReserveOffset( rCursorBytes, nSizeBytes, nAlignmentBytes, m_nCapacityBytes, offset, next ) ) {
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
	        const t3dRdResourceOwner_t owner ) const {
		if ( !ownsSlice( rSlice ) || nRelativeOffset > rSlice.m_nCapacityBytes
		        || nBytes > rSlice.m_nCapacityBytes - nRelativeOffset
		        || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset ) {
			return false;
		}
		const t3dRdResourceOwner_t resolved_owner = owner == T3D_RD_CONTEXT_OWNER_UNSPECIFIED ? m_spec.m_owner : owner;
		return writeTrackedForOwner( rContext, rSlice.m_nOffsetBytes + nRelativeOffset, pSource, nBytes, rFallbackScratch, resolved_owner, policy );
	}

bool Terrain3DRdGpuBuffer::writePackedSliceTracked( Terrain3DRdContextAccess &rContext,
	        const t3dRdGpuBufferSlice_t &rSlice,
	        const uint32_t nRelativeOffset,
	        const PackedByteArray &rBytes,
	        const uint32_t nBytes,
	        const Terrain3DGpuBufferPolicy policy,
	        const t3dRdResourceOwner_t owner ) const {
		if ( !ownsSlice( rSlice ) || nRelativeOffset > rSlice.m_nCapacityBytes
		        || nBytes > rSlice.m_nCapacityBytes - nRelativeOffset
		        || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset ) {
			return false;
		}
		const t3dRdResourceOwner_t resolved_owner = owner == T3D_RD_CONTEXT_OWNER_UNSPECIFIED ? m_spec.m_owner : owner;
		return writePackedTrackedForOwner( rContext, rSlice.m_nOffsetBytes + nRelativeOffset, rBytes, nBytes, resolved_owner, policy );
	}

PackedByteArray Terrain3DRdGpuBuffer::readbackSliceTracked( Terrain3DRdContextAccess &rContext,
	        const t3dRdGpuBufferSlice_t &rSlice,
	        const uint32_t nRelativeOffset,
	        const uint32_t nBytes ) const {
		if ( !ownsSlice( rSlice ) || nRelativeOffset > rSlice.m_nCapacityBytes
		        || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset ) {
			return PackedByteArray();
		}
		const uint32_t bytes = nBytes == 0u ? rSlice.m_nCapacityBytes - nRelativeOffset : nBytes;
		if ( bytes > rSlice.m_nCapacityBytes - nRelativeOffset ) {
			return PackedByteArray();
		}
		return readbackTracked( rContext, rSlice.m_nOffsetBytes + nRelativeOffset, bytes );
	}

bool Terrain3DRdGpuBuffer::setSliceUsedBytes( t3dRdGpuBufferSlice_t &rSlice, const uint32_t nBytes ) const {
		if ( !ownsSlice( rSlice ) || nBytes > rSlice.m_nCapacityBytes ) {
			return false;
		}
		rSlice.m_nSizeBytes = nBytes;
		return true;
	}

bool Terrain3DRdGpuBuffer::setUsedBytes( const uint32_t nBytes ) {
		if ( nBytes > m_nCapacityBytes ) { return false; }
		m_nUsedBytes = nBytes;
		return true;
	}

void Terrain3DRdGpuBuffer::swap( Terrain3DRdGpuBuffer &rOther ) {
		t3dRdGpuBufferSpec_t spec = m_spec;
		m_spec = rOther.m_spec;
		rOther.m_spec = spec;
		RID rid = m_hRid;
		m_hRid = rOther.m_hRid;
		rOther.m_hRid = rid;
		uint32_t value = m_nCapacityBytes;
		m_nCapacityBytes = rOther.m_nCapacityBytes;
		rOther.m_nCapacityBytes = value;
		value = m_nUsedBytes;
		m_nUsedBytes = rOther.m_nUsedBytes;
		rOther.m_nUsedBytes = value;
		value = m_nGeneration;
		m_nGeneration = rOther.m_nGeneration;
		rOther.m_nGeneration = value;
		const bool configured = m_bConfigured;
		m_bConfigured = rOther.m_bConfigured;
		rOther.m_bConfigured = configured;
		const bool persistent = m_bPersistent;
		m_bPersistent = rOther.m_bPersistent;
		rOther.m_bPersistent = persistent;
	}
