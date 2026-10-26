#ifndef TERRAIN3D_RD_RESOURCE_SERVICE_H
#define TERRAIN3D_RD_RESOURCE_SERVICE_H

#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/terrain_3d_gpu_buffer_layout.h"
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/typed_array.hpp>

// A render-thread transaction: a newly created RID is either registered with
// its typed owner/accounting data or freed before the caller can retain it.
class Terrain3DRdResourceService
{
	Terrain3DRdContext &m_context;
	Terrain3DRdContext::ResourceOwner m_owner;

public:
	Terrain3DRdResourceService( Terrain3DRdContext &rContext, Terrain3DRdContext::ResourceOwner owner ) :
	        m_context( rContext ), m_owner( owner ) {}

	bool adopt( RenderingDevice *const pRd,
	        RID &rRid,
	        const String &rName,
	        Terrain3DRdContext::ResourceType type,
	        Terrain3DRdContext::MemoryCategory memory = Terrain3DRdContext::MEMORY_NONE,
	        uint64_t nBytes = 0u ) const {
		if ( rRid.is_valid() && m_context.registerResource( rRid, rName, type, memory, nBytes, m_owner ) ) {
			const bool is_buffer = type == Terrain3DRdContext::RESOURCE_STORAGE_BUFFER
			        || type == Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER
			        || type == Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER
			        || type == Terrain3DRdContext::RESOURCE_CULLING_BUFFER
			        || type == Terrain3DRdContext::RESOURCE_VERTEX_BUFFER
			        || type == Terrain3DRdContext::RESOURCE_INDEX_BUFFER;
			if ( is_buffer && memory == Terrain3DRdContext::MEMORY_SCRATCH ) {
				m_context.recordRenderCapacity( m_owner, T3D_GPU_SCRATCH, nBytes );
			} else if ( is_buffer && memory == Terrain3DRdContext::MEMORY_HISTORY ) {
				m_context.recordRenderCapacity( m_owner, T3D_GPU_HISTORY, nBytes );
			} else if ( is_buffer && memory == Terrain3DRdContext::MEMORY_INDIRECT ) {
				m_context.recordRenderCapacity( m_owner, T3D_GPU_INDIRECT, nBytes );
			}
			if ( is_buffer ) {
				m_context.recordRenderEvent( m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_BUFFER_CREATE );
			} else if ( type == Terrain3DRdContext::RESOURCE_UNIFORM_SET ) {
				m_context.recordRenderEvent( m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_DESCRIPTOR_CREATE );
			} else if ( type == Terrain3DRdContext::RESOURCE_RASTER_PIPELINE
			        || type == Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE ) {
				m_context.recordRenderEvent( m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_PIPELINE_CREATE );
			}
			return true;
		}
		m_context.recordRenderEvent( m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_RESOURCE_FAILURE );
		if ( pRd && rRid.is_valid() ) {
			pRd->free_rid( rRid );
		}
		rRid = RID();
		return false;
	}

	template <typename TCreator>
	bool create( RID &rRid,
	        const String &rName,
	        Terrain3DRdContext::ResourceType type,
	        Terrain3DRdContext::MemoryCategory memory,
	        uint64_t nBytes,
	        TCreator creator ) const {
		RenderingDevice *const rd = m_context.getDevice();
		if ( !rd || rRid.is_valid() ) {
			return false;
		}
		RID created = creator( rd );
		if ( !adopt( rd, created, rName, type, memory, nBytes ) ) {
			return false;
		}
		rRid = created;
		return true;
	}

	bool createUniformBuffer( RID &rRid,
	        const String &rName,
	        uint32_t nBytes,
	        Terrain3DRdContext::MemoryCategory memory = Terrain3DRdContext::MEMORY_UNIFORM,
	        BitField<RenderingDevice::BufferCreationBits> creationBits = BitField<RenderingDevice::BufferCreationBits>( 0 ) ) const {
		return create( rRid,
		        rName,
		        Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER,
		        memory,
		        nBytes,
		        [nBytes, creationBits]( RenderingDevice *pRd ) {
		        return pRd->uniform_buffer_create( nBytes, PackedByteArray(), creationBits );
		        } );
	}

	bool createStorageBuffer( RID &rRid,
	        const String &rName,
	        uint32_t nBytes,
	        Terrain3DRdContext::MemoryCategory memory,
	        BitField<RenderingDevice::StorageBufferUsage> usage = BitField<RenderingDevice::StorageBufferUsage>( 0 ),
	        BitField<RenderingDevice::BufferCreationBits> creationBits = BitField<RenderingDevice::BufferCreationBits>( 0 ) ) const {
		return create( rRid,
		        rName,
		        Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
		        memory,
		        nBytes,
		        [nBytes, usage, creationBits]( RenderingDevice *pRd ) {
		        return pRd->storage_buffer_create( nBytes, PackedByteArray(), usage, creationBits );
		        } );
	}

	bool createBuffer( RID &rRid,
	        const String &rName,
	        Terrain3DRdContext::ResourceType type,
	        Terrain3DRdContext::MemoryCategory memory,
	        uint32_t nBytes,
	        const PackedByteArray &rInitialData = PackedByteArray(),
	        BitField<RenderingDevice::StorageBufferUsage> usage = BitField<RenderingDevice::StorageBufferUsage>( 0 ),
	        BitField<RenderingDevice::BufferCreationBits> creationBits = BitField<RenderingDevice::BufferCreationBits>( 0 ) ) const {
		return create( rRid, rName, type, memory, nBytes,
		        [&]( RenderingDevice *pRd ) { return pRd->storage_buffer_create( nBytes, rInitialData, usage, creationBits ); } );
	}

	bool createIndirectBuffer( RID &rRid,
	        const String &rName,
	        uint32_t nBytes,
	        BitField<RenderingDevice::StorageBufferUsage> usage ) const {
		return create( rRid,
		        rName,
		        Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER,
		        Terrain3DRdContext::MEMORY_INDIRECT,
		        nBytes,
		        [nBytes, usage]( RenderingDevice *pRd ) {
		        return pRd->storage_buffer_create( nBytes, PackedByteArray(), usage );
		        } );
	}

	// Publish a registered replacement before releasing the old buffer. A failed
	// create leaves both the old RID and its capacity intact.
	bool replaceBuffer( RID &rRid,
	        uint32_t &rCapacity,
	        const String &rName,
	        Terrain3DRdContext::ResourceType type,
	        Terrain3DRdContext::MemoryCategory memory,
	        uint32_t nCapacity,
	        BitField<RenderingDevice::StorageBufferUsage> usage = BitField<RenderingDevice::StorageBufferUsage>( 0 ) ) const {
		if ( nCapacity == 0u || ( type != Terrain3DRdContext::RESOURCE_STORAGE_BUFFER
		                                && type != Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER
		                                && type != Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER ) ) {
			return false;
		}
		RID replacement;
		const bool created = type == Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER
		        ? createUniformBuffer( replacement, rName, nCapacity, memory )
		        : createBuffer( replacement, rName, type, memory, nCapacity, PackedByteArray(), usage );
		if ( !created ) {
			return false;
		}
		const bool rebuilding = rRid.is_valid();
		if ( rebuilding ) {
			m_context.freeResource( rRid, rCapacity );
		}
		rRid = replacement;
		rCapacity = nCapacity;
		if ( rebuilding ) {
			m_context.recordRenderEvent( m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_BUFFER_REBUILD );
		}
		return true;
	}

	bool ensureGrowingBuffer( RID &rRid,
	        uint32_t &rCapacity,
	        const String &rName,
	        Terrain3DRdContext::ResourceType type,
	        Terrain3DRdContext::MemoryCategory memory,
	        uint32_t nRequired,
	        uint32_t nMinimum,
	        Terrain3DGpuGrowthPolicy growthPolicy = T3D_GPU_POWER_OF_TWO ) const {
		if ( rRid.is_valid() && rCapacity >= nRequired ) {
			return true;
		}
		uint32_t capacity = 0u;
		if ( !Terrain3DGpuBufferLayout::grownCapacity( nMinimum, nRequired, growthPolicy, capacity ) ) {
			return false;
		}
		const BitField<RenderingDevice::StorageBufferUsage> usage = type == Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER
		        ? BitField<RenderingDevice::StorageBufferUsage>( RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT )
		        : BitField<RenderingDevice::StorageBufferUsage>( 0 );
		return replaceBuffer( rRid, rCapacity, rName, type, memory, capacity, usage );
	}

	bool createUniformSet( RID &rRid, const String &rName,
	        const TypedArray<Ref<RDUniform>> &rUniforms, const RID &rShader, uint32_t nSet ) const {
		return create( rRid, rName, Terrain3DRdContext::RESOURCE_UNIFORM_SET,
		        Terrain3DRdContext::MEMORY_NONE, 0u,
		        [&]( RenderingDevice *pRd ) { return pRd->uniform_set_create( rUniforms, rShader, nSet ); } );
	}

	void release( RID &rRid, uint64_t nBytes = 0u ) const {
		m_context.freeResource( rRid, nBytes );
	}
};

#endif // TERRAIN3D_RD_RESOURCE_SERVICE_H
