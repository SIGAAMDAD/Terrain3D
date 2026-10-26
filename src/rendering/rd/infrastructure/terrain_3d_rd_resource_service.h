#ifndef TERRAIN3D_RD_RESOURCE_SERVICE_H
#define TERRAIN3D_RD_RESOURCE_SERVICE_H

#include "rendering/rd/terrain_3d_rd_context.h"
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
		// Physical texture/sampler/shader/framebuffer/geometry-view ownership is centralized in cohesive owner types.
		if ( type == Terrain3DRdContext::RESOURCE_TEXTURE || type == Terrain3DRdContext::RESOURCE_SAMPLER
		        || type == Terrain3DRdContext::RESOURCE_SHADER || type == Terrain3DRdContext::RESOURCE_FRAMEBUFFER
		        || type == Terrain3DRdContext::RESOURCE_VERTEX_ARRAY || type == Terrain3DRdContext::RESOURCE_INDEX_ARRAY ) {
			m_context.recordRenderEvent( m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_RESOURCE_FAILURE );
			if ( pRd && rRid.is_valid() ) pRd->free_rid( rRid );
			rRid = RID();
			return false;
		}
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
