#ifndef TERRAIN3D_RD_GPU_BUFFER_H
#define TERRAIN3D_RD_GPU_BUFFER_H

#include "rendering/rd/infrastructure/terrain_3d_rd_gpu_buffer_types.h"
#include "rendering/rd/terrain_3d_rd_context.h"

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <stdint.h>
#include <string.h>


typedef struct t3dRdGpuBufferSpec_s {
	const char *m_pName = NULL;
	Terrain3DRdContext::ResourceOwner m_owner = Terrain3DRdContext::OWNER_UNSPECIFIED;
	Terrain3DRdContext::ResourceType m_type = Terrain3DRdContext::RESOURCE_STORAGE_BUFFER;
	Terrain3DRdContext::MemoryCategory m_memory = Terrain3DRdContext::MEMORY_STORAGE;
	Terrain3DGpuGrowthPolicy m_growth = T3D_GPU_EXACT;
	uint32_t m_nMinimumCapacityBytes = 0u;
	BitField<RenderingDevice::StorageBufferUsage> m_usage = BitField<RenderingDevice::StorageBufferUsage>( 0 );
	BitField<RenderingDevice::BufferCreationBits> m_creationBits = BitField<RenderingDevice::BufferCreationBits>( 0 );
	RenderingDevice::IndexBufferFormat m_indexFormat = RenderingDevice::INDEX_BUFFER_FORMAT_UINT32;
} t3dRdGpuBufferSpec_t;

// Cohesive owner for one physical RenderingDevice buffer. Configuration and
// physical state live together: domain code configures the logical buffer once,
// then asks this object to create/grow/replace/write/read/release that buffer.
class Terrain3DRdGpuBuffer {
	t3dRdGpuBufferSpec_t m_spec;
	RID m_hRid;
	uint32_t m_nCapacityBytes = 0u;
	uint32_t m_nUsedBytes = 0u;
	uint32_t m_nGeneration = 0u;
	bool m_bConfigured = false;
	bool m_bPersistent = false;

	static bool isBufferType( const Terrain3DRdContext::ResourceType type ) {
		return type == Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER
		        || type == Terrain3DRdContext::RESOURCE_STORAGE_BUFFER
		        || type == Terrain3DRdContext::RESOURCE_CULLING_BUFFER
		        || type == Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER
		        || type == Terrain3DRdContext::RESOURCE_VERTEX_BUFFER
		        || type == Terrain3DRdContext::RESOURCE_INDEX_BUFFER;
	}

	static uint32_t indexStride( const RenderingDevice::IndexBufferFormat format ) {
		return format == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ? 2u : 4u;
	}

	static bool persistentMappingAvailable( RenderingDevice *const pRd ) {
		if ( !pRd ) return false;
		static const StringName s_mapMethod( "buffer_persistent_map_advance" );
		static const StringName s_flushMethod( "buffer_flush" );
		return pRd->has_method( s_mapMethod ) && pRd->has_method( s_flushMethod );
	}

	static uint8_t *persistentMapAdvance( RenderingDevice *const pRd, const RID &rBuffer ) {
		if ( !pRd || !rBuffer.is_valid() ) return NULL;
		const uint64_t nAddress = pRd->buffer_persistent_map_advance( rBuffer );
		return ( uint8_t * )( ( uintptr_t )nAddress );
	}

	static bool persistentWrite( RenderingDevice *const pRd,
	        const RID &rBuffer,
	        const void *const pSource,
	        const uint32_t nBytes,
	        const uint32_t nOffset = 0u ) {
		if ( !pSource || nBytes == 0u ) return true;
		uint8_t *const pMapped = persistentMapAdvance( pRd, rBuffer );
		if ( !pMapped ) return false;
		memcpy( pMapped + nOffset, pSource, nBytes );
		pRd->buffer_flush( rBuffer );
		return true;
	}

	static RID createPhysical( RenderingDevice *const pRd,
	        const t3dRdGpuBufferSpec_t &rSpec,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData ) {
		if ( !pRd || nCapacityBytes == 0u || !isBufferType( rSpec.m_type )
		        || ( uint64_t )( rInitialData.size() ) > nCapacityBytes ) {
			return RID();
		}
		switch ( rSpec.m_type ) {
			case Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER:
				return pRd->uniform_buffer_create( nCapacityBytes, rInitialData, rSpec.m_creationBits );
			case Terrain3DRdContext::RESOURCE_STORAGE_BUFFER:
			case Terrain3DRdContext::RESOURCE_CULLING_BUFFER:
				return pRd->storage_buffer_create( nCapacityBytes, rInitialData, rSpec.m_usage, rSpec.m_creationBits );
			case Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER:
				return pRd->storage_buffer_create( nCapacityBytes, rInitialData,
				        BitField<RenderingDevice::StorageBufferUsage>( RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT ),
				        rSpec.m_creationBits );
			case Terrain3DRdContext::RESOURCE_VERTEX_BUFFER:
				return pRd->vertex_buffer_create( nCapacityBytes, rInitialData, rSpec.m_creationBits );
			case Terrain3DRdContext::RESOURCE_INDEX_BUFFER: {
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

	static bool registerTracked( Terrain3DRdContext &rContext,
	        RenderingDevice *const pRd,
	        RID &rRid,
	        const t3dRdGpuBufferSpec_t &rSpec,
	        const uint32_t nCapacityBytes ) {
		const String name = rSpec.m_pName ? String::utf8( rSpec.m_pName ) : String();
		if ( !rRid.is_valid() || name.is_empty()
		        || !rContext.registerResource( rRid, name, rSpec.m_type, rSpec.m_memory, nCapacityBytes, rSpec.m_owner ) ) {
			rContext.recordRenderEvent( rSpec.m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_RESOURCE_FAILURE );
			if ( pRd && rRid.is_valid() ) {
				pRd->free_rid( rRid );
			}
			rRid = RID();
			return false;
		}
		if ( rSpec.m_memory == Terrain3DRdContext::MEMORY_SCRATCH ) {
			rContext.recordRenderCapacity( rSpec.m_owner, T3D_GPU_SCRATCH, nCapacityBytes );
		} else if ( rSpec.m_memory == Terrain3DRdContext::MEMORY_HISTORY ) {
			rContext.recordRenderCapacity( rSpec.m_owner, T3D_GPU_HISTORY, nCapacityBytes );
		} else if ( rSpec.m_memory == Terrain3DRdContext::MEMORY_INDIRECT ) {
			rContext.recordRenderCapacity( rSpec.m_owner, T3D_GPU_INDIRECT, nCapacityBytes );
		}
		rContext.recordRenderEvent( rSpec.m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_BUFFER_CREATE );
		return true;
	}

	void publishPhysical( const RID &rRid,
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

	void clearPhysical( void ) {
		m_hRid = RID();
		m_nCapacityBytes = 0u;
		m_nUsedBytes = 0u;
		m_bPersistent = false;
	}

public:
	Terrain3DRdGpuBuffer( void ) = default;
	Terrain3DRdGpuBuffer( const Terrain3DRdGpuBuffer & ) = delete;
	Terrain3DRdGpuBuffer &operator=( const Terrain3DRdGpuBuffer & ) = delete;

	static BitField<RenderingDevice::BufferCreationBits> persistentCreationBits( void ) {
		return BitField<RenderingDevice::BufferCreationBits>( RenderingDevice::BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT );
	}

	static bool supportsPersistentMapping( RenderingDevice *const pRd ) {
		return persistentMappingAvailable( pRd );
	}

	bool configure( const t3dRdGpuBufferSpec_t &rSpec ) {
		if ( isValid() || !isBufferType( rSpec.m_type ) ) {
			return false;
		}
		m_spec = rSpec;
		m_bConfigured = true;
		return true;
	}

	bool isConfigured( void ) const { return m_bConfigured; }
	const t3dRdGpuBufferSpec_t &spec( void ) const { return m_spec; }
	bool isValid( void ) const { return m_hRid.is_valid(); }
	bool is_valid( void ) const { return isValid(); }
	const RID &rid( void ) const { return m_hRid; }
	uint32_t capacityBytes( void ) const { return m_nCapacityBytes; }
	uint32_t usedBytes( void ) const { return m_nUsedBytes; }
	uint32_t generation( void ) const { return m_nGeneration; }
	bool isPersistent( void ) const { return m_bPersistent; }
	Terrain3DRdContext::ResourceOwner owner( void ) const { return m_spec.m_owner; }
	operator const RID &( void ) const { return m_hRid; }

	bool needsCapacity( const uint32_t nRequiredBytes ) const {
		return !m_hRid.is_valid() || nRequiredBytes > m_nCapacityBytes;
	}

	bool createTracked( Terrain3DRdContext &rContext,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData = PackedByteArray() ) {
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

	bool replaceTracked( Terrain3DRdContext &rContext,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData = PackedByteArray() ) {
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
			rContext.recordRenderEvent( m_spec.m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_BUFFER_REBUILD );
		}
		replacement.releaseTracked( rContext );
		return true;
	}

	bool ensureTracked( Terrain3DRdContext &rContext, const uint32_t nRequiredBytes ) {
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

	bool createLocal( RenderingDevice *const pDevice,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData = PackedByteArray() ) {
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

	bool replaceLocal( RenderingDevice *const pDevice,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData = PackedByteArray() ) {
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

	bool ensureLocal( RenderingDevice *const pDevice, const uint32_t nRequiredBytes ) {
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

	void releaseTracked( Terrain3DRdContext &rContext ) {
		if ( m_hRid.is_valid() ) {
			rContext.freeResource( m_hRid, m_nCapacityBytes );
		}
		clearPhysical();
	}

	void releaseLocal( RenderingDevice *const pDevice ) {
		if ( pDevice && m_hRid.is_valid() ) {
			pDevice->free_rid( m_hRid );
		}
		clearPhysical();
	}

	// Device loss invalidates the RID externally; retain logical configuration
	// so the same buffer object can recreate its physical backing afterward.
	void abandon( void ) {
		clearPhysical();
		m_nGeneration = t3dRdGpuBufferNextGeneration( m_nGeneration );
	}

	bool writeTrackedForOwner( Terrain3DRdContext &rContext,
	        const uint32_t nOffsetBytes,
	        const void *const pSource,
	        const uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        const Terrain3DRdContext::ResourceOwner owner,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) const {
		if ( nBytes == 0u ) return true;
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

	bool writeTracked( Terrain3DRdContext &rContext,
	        const uint32_t nOffsetBytes,
	        const void *const pSource,
	        const uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) const {
		return writeTrackedForOwner( rContext, nOffsetBytes, pSource, nBytes, rFallbackScratch, m_spec.m_owner, policy );
	}

	bool writePackedTrackedForOwner( Terrain3DRdContext &rContext,
	        const uint32_t nOffsetBytes,
	        const PackedByteArray &rBytes,
	        const uint32_t nBytes,
	        const Terrain3DRdContext::ResourceOwner owner,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) const {
		if ( nBytes == 0u ) return true;
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

	bool writePackedTracked( Terrain3DRdContext &rContext,
	        const uint32_t nOffsetBytes,
	        const PackedByteArray &rBytes,
	        const uint32_t nBytes,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) const {
		return writePackedTrackedForOwner( rContext, nOffsetBytes, rBytes, nBytes, m_spec.m_owner, policy );
	}

	bool writeLocal( RenderingDevice *const pDevice,
	        const uint32_t nOffsetBytes,
	        const void *const pSource,
	        const uint32_t nBytes,
	        PackedByteArray &rFallbackScratch ) const {
		if ( nBytes == 0u ) return true;
		if ( !pDevice || !isValid() || !pSource || nOffsetBytes > m_nCapacityBytes || nBytes > m_nCapacityBytes - nOffsetBytes ) {
			return false;
		}
		if ( m_bPersistent ) {
			return persistentWrite( pDevice, m_hRid, pSource, nBytes, nOffsetBytes );
		}
		if ( rFallbackScratch.size() < nBytes ) return false;
		if ( pSource != rFallbackScratch.ptr() ) {
			memcpy( rFallbackScratch.ptrw(), pSource, nBytes );
		}
		return pDevice->buffer_update( m_hRid, nOffsetBytes, nBytes, rFallbackScratch ) == OK;
	}

	bool writePackedLocal( RenderingDevice *const pDevice,
	        const uint32_t nOffsetBytes,
	        const PackedByteArray &rBytes,
	        const uint32_t nBytes ) const {
		if ( nBytes == 0u ) return true;
		if ( !pDevice || !isValid() || rBytes.size() < nBytes || nOffsetBytes > m_nCapacityBytes
		        || nBytes > m_nCapacityBytes - nOffsetBytes ) {
			return false;
		}
		return m_bPersistent ? persistentWrite( pDevice, m_hRid, rBytes.ptr(), nBytes, nOffsetBytes )
		                     : pDevice->buffer_update( m_hRid, nOffsetBytes, nBytes, rBytes ) == OK;
	}

	PackedByteArray readbackTracked( Terrain3DRdContext &rContext,
	        const uint32_t nOffsetBytes = 0u,
	        const uint32_t nBytes = 0u ) const {
		RenderingDevice *const rd = rContext.getDevice();
		const uint32_t bytes = nBytes == 0u ? m_nCapacityBytes - MIN( nOffsetBytes, m_nCapacityBytes ) : nBytes;
		if ( !rd || !isValid() || nOffsetBytes > m_nCapacityBytes || bytes > m_nCapacityBytes - nOffsetBytes ) {
			return PackedByteArray();
		}
		return rd->buffer_get_data( m_hRid, nOffsetBytes, bytes );
	}

	PackedByteArray readbackLocal( RenderingDevice *const pDevice,
	        const uint32_t nOffsetBytes = 0u,
	        const uint32_t nBytes = 0u ) const {
		const uint32_t bytes = nBytes == 0u ? m_nCapacityBytes - MIN( nOffsetBytes, m_nCapacityBytes ) : nBytes;
		if ( !pDevice || !isValid() || nOffsetBytes > m_nCapacityBytes || bytes > m_nCapacityBytes - nOffsetBytes ) {
			return PackedByteArray();
		}
		return pDevice->buffer_get_data( m_hRid, nOffsetBytes, bytes );
	}

	t3dRdGpuBufferSlice_t wholeSlice( void ) const {
		return { m_hRid, 0u, m_nCapacityBytes, m_nCapacityBytes, 1u, m_nGeneration };
	}

	bool slice( const uint32_t nOffsetBytes,
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

	bool ownsSlice( const t3dRdGpuBufferSlice_t &rSlice ) const {
		return isValid() && rSlice.m_hBuffer == m_hRid && rSlice.m_nGeneration == m_nGeneration
		        && rSlice.m_nAlignmentBytes != 0u
		        && ( rSlice.m_nAlignmentBytes & ( rSlice.m_nAlignmentBytes - 1u ) ) == 0u
		        && rSlice.m_nOffsetBytes % rSlice.m_nAlignmentBytes == 0u
		        && rSlice.m_nOffsetBytes <= m_nCapacityBytes
		        && rSlice.m_nCapacityBytes <= m_nCapacityBytes - rSlice.m_nOffsetBytes
		        && rSlice.m_nSizeBytes <= rSlice.m_nCapacityBytes;
	}

	bool reserveSlice( uint32_t &rCursorBytes,
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

	bool writeSliceTracked( Terrain3DRdContext &rContext,
	        const t3dRdGpuBufferSlice_t &rSlice,
	        const uint32_t nRelativeOffset,
	        const void *const pSource,
	        const uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL,
	        const Terrain3DRdContext::ResourceOwner owner = Terrain3DRdContext::OWNER_UNSPECIFIED ) const {
		if ( !ownsSlice( rSlice ) || nRelativeOffset > rSlice.m_nCapacityBytes
		        || nBytes > rSlice.m_nCapacityBytes - nRelativeOffset
		        || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset ) {
			return false;
		}
		const Terrain3DRdContext::ResourceOwner resolved_owner = owner == Terrain3DRdContext::OWNER_UNSPECIFIED ? m_spec.m_owner : owner;
		return writeTrackedForOwner( rContext, rSlice.m_nOffsetBytes + nRelativeOffset, pSource, nBytes, rFallbackScratch, resolved_owner, policy );
	}

	bool writePackedSliceTracked( Terrain3DRdContext &rContext,
	        const t3dRdGpuBufferSlice_t &rSlice,
	        const uint32_t nRelativeOffset,
	        const PackedByteArray &rBytes,
	        const uint32_t nBytes,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL,
	        const Terrain3DRdContext::ResourceOwner owner = Terrain3DRdContext::OWNER_UNSPECIFIED ) const {
		if ( !ownsSlice( rSlice ) || nRelativeOffset > rSlice.m_nCapacityBytes
		        || nBytes > rSlice.m_nCapacityBytes - nRelativeOffset
		        || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset ) {
			return false;
		}
		const Terrain3DRdContext::ResourceOwner resolved_owner = owner == Terrain3DRdContext::OWNER_UNSPECIFIED ? m_spec.m_owner : owner;
		return writePackedTrackedForOwner( rContext, rSlice.m_nOffsetBytes + nRelativeOffset, rBytes, nBytes, resolved_owner, policy );
	}

	PackedByteArray readbackSliceTracked( Terrain3DRdContext &rContext,
	        const t3dRdGpuBufferSlice_t &rSlice,
	        const uint32_t nRelativeOffset = 0u,
	        const uint32_t nBytes = 0u ) const {
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

	bool setSliceUsedBytes( t3dRdGpuBufferSlice_t &rSlice, const uint32_t nBytes ) const {
		if ( !ownsSlice( rSlice ) || nBytes > rSlice.m_nCapacityBytes ) {
			return false;
		}
		rSlice.m_nSizeBytes = nBytes;
		return true;
	}

	bool setUsedBytes( const uint32_t nBytes ) {
		if ( nBytes > m_nCapacityBytes ) return false;
		m_nUsedBytes = nBytes;
		return true;
	}

	void swap( Terrain3DRdGpuBuffer &rOther ) {
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
};

#endif // TERRAIN3D_RD_GPU_BUFFER_H
