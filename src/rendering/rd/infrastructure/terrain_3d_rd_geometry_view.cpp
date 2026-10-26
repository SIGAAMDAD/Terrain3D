#include "rendering/rd/infrastructure/terrain_3d_rd_geometry_view.h"

#include <EASTL/utility.h>

Terrain3DRdGeometryView::Terrain3DRdGeometryView( Terrain3DRdGeometryView &&rOther ) noexcept {
	swap( rOther );
}

bool Terrain3DRdGeometryView::ownerCompatible( const Terrain3DRdContext::ResourceOwner owner ) const {
	return m_owner == Terrain3DRdContext::OWNER_UNSPECIFIED || m_owner == owner;
}

bool Terrain3DRdGeometryView::createVertexTracked( Terrain3DRdContext &rContext,
        const Terrain3DRdContext::ResourceOwner owner,
        const String &rName,
        const uint32_t nVertexCount,
        const int64_t nVertexFormat,
        const TypedArray<RID> &rBuffers,
        const PackedInt64Array &rOffsets ) {
	if ( m_hVertexArray.is_valid() || !ownerCompatible( owner ) || rName.is_empty() || nVertexCount == 0u
	        || nVertexFormat == RenderingDevice::INVALID_FORMAT_ID || rBuffers.size() == 0 ) {
		return false;
	}
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd ) return false;

	RID created = rd->vertex_array_create( nVertexCount, nVertexFormat, rBuffers, rOffsets );
	if ( !created.is_valid() ) {
		rContext.recordResourceCreationFailure( rName, Terrain3DRdContext::RESOURCE_VERTEX_ARRAY );
		rContext.recordRenderEvent( owner, Terrain3DRdContext::RenderTelemetry::EVENT_RESOURCE_FAILURE );
		return false;
	}
	if ( !rContext.registerResource( created, rName, Terrain3DRdContext::RESOURCE_VERTEX_ARRAY,
	             Terrain3DRdContext::MEMORY_NONE, 0u, owner ) ) {
		rContext.recordRenderEvent( owner, Terrain3DRdContext::RenderTelemetry::EVENT_RESOURCE_FAILURE );
		rd->free_rid( created );
		return false;
	}

	m_hVertexArray = created;
	m_owner = owner;
	m_nGeneration = nextGeneration( m_nGeneration );
	return true;
}

bool Terrain3DRdGeometryView::createIndexTracked( Terrain3DRdContext &rContext,
        const Terrain3DRdContext::ResourceOwner owner,
        const String &rName,
        const RID &rBuffer,
        const uint32_t nOffset,
        const uint32_t nCount ) {
	if ( m_hIndexArray.is_valid() || !ownerCompatible( owner ) || rName.is_empty() || !rBuffer.is_valid() || nCount == 0u ) {
		return false;
	}
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd ) return false;

	RID created = rd->index_array_create( rBuffer, nOffset, nCount );
	if ( !created.is_valid() ) {
		rContext.recordResourceCreationFailure( rName, Terrain3DRdContext::RESOURCE_INDEX_ARRAY );
		rContext.recordRenderEvent( owner, Terrain3DRdContext::RenderTelemetry::EVENT_RESOURCE_FAILURE );
		return false;
	}
	if ( !rContext.registerResource( created, rName, Terrain3DRdContext::RESOURCE_INDEX_ARRAY,
	             Terrain3DRdContext::MEMORY_NONE, 0u, owner ) ) {
		rContext.recordRenderEvent( owner, Terrain3DRdContext::RenderTelemetry::EVENT_RESOURCE_FAILURE );
		rd->free_rid( created );
		return false;
	}

	m_hIndexArray = created;
	m_owner = owner;
	m_nGeneration = nextGeneration( m_nGeneration );
	return true;
}

bool Terrain3DRdGeometryView::createIndexedTracked( Terrain3DRdContext &rContext,
        const Terrain3DRdContext::ResourceOwner owner,
        const String &rVertexName,
        const String &rIndexName,
        const uint32_t nVertexCount,
        const int64_t nVertexFormat,
        const TypedArray<RID> &rVertexBuffers,
        const PackedInt64Array &rVertexOffsets,
        const RID &rIndexBuffer,
        const uint32_t nIndexOffset,
        const uint32_t nIndexCount ) {
	if ( isValid() ) return false;

	Terrain3DRdGeometryView replacement;
	if ( !replacement.createVertexTracked( rContext, owner, rVertexName, nVertexCount, nVertexFormat,
	             rVertexBuffers, rVertexOffsets ) ) {
		return false;
	}
	if ( !replacement.createIndexTracked( rContext, owner, rIndexName, rIndexBuffer, nIndexOffset, nIndexCount ) ) {
		replacement.releaseTracked( rContext );
		return false;
	}
	swap( replacement );
	return true;
}

void Terrain3DRdGeometryView::releaseTracked( Terrain3DRdContext &rContext ) {
	if ( m_hIndexArray.is_valid() ) rContext.freeResource( m_hIndexArray );
	if ( m_hVertexArray.is_valid() ) rContext.freeResource( m_hVertexArray );
	abandon();
}

void Terrain3DRdGeometryView::abandon( void ) {
	m_hVertexArray = RID();
	m_hIndexArray = RID();
	m_owner = Terrain3DRdContext::OWNER_UNSPECIFIED;
	m_nGeneration = nextGeneration( m_nGeneration );
}

void Terrain3DRdGeometryView::swap( Terrain3DRdGeometryView &rOther ) {
	using eastl::swap;
	const uint32_t this_generation = nextGeneration( m_nGeneration );
	const uint32_t other_generation = nextGeneration( rOther.m_nGeneration );
	swap( m_hVertexArray, rOther.m_hVertexArray );
	swap( m_hIndexArray, rOther.m_hIndexArray );
	swap( m_owner, rOther.m_owner );
	m_nGeneration = this_generation;
	rOther.m_nGeneration = other_generation;
}
