#include "rendering/rd/infrastructure/targets/terrain_3d_rd_framebuffer.h"
#include "rendering/rd/infrastructure/targets/terrain_3d_rd_geometry_view.h"

Terrain3DRdGeometryView::Terrain3DRdGeometryView(
	Terrain3DRdGeometryView &&rOther ) noexcept {
	swap( rOther );
}

bool Terrain3DRdGeometryView::ownerCompatible(
	const t3dRdResourceOwner_t owner ) const {
	return m_state.owner() == T3D_RD_CONTEXT_OWNER_UNSPECIFIED || m_state.owner() == owner;
}

bool Terrain3DRdGeometryView::createVertexTracked(
	Terrain3DRdContext		  &rContext,
	const t3dRdResourceOwner_t owner,
	const String			  &rName,
	const uint32_t			   nVertexCount,
	const int64_t			   nVertexFormat,
	const TypedArray< RID >	  &rBuffers,
	const PackedInt64Array	  &rOffsets ) {
	if ( m_state.rid().is_valid() || !ownerCompatible( owner ) || rName.is_empty() || nVertexCount == 0u ||
		 nVertexFormat == RenderingDevice::INVALID_FORMAT_ID || rBuffers.size() == 0 ) {
		return false;
	}
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd ) {
		return false;
	}

	RID created = rd->vertex_array_create( nVertexCount, nVertexFormat, rBuffers, rOffsets );
	if ( !t3dRdRegisterCreated( rContext, rd, created, rName, T3D_RD_CONTEXT_RESOURCE_VERTEX_ARRAY, owner ) ) {
		return false;
	}

	m_state.publish( created, owner );
	return true;
}

bool Terrain3DRdGeometryView::createIndexTracked(
	Terrain3DRdContext		  &rContext,
	const t3dRdResourceOwner_t owner,
	const String			  &rName,
	const RID				  &rBuffer,
	const uint32_t			   nOffset,
	const uint32_t			   nCount ) {
	if ( m_state.rid( 1u ).is_valid() || !ownerCompatible( owner ) || rName.is_empty() || !rBuffer.is_valid() ||
		 nCount == 0u ) {
		return false;
	}
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd ) {
		return false;
	}

	RID created = rd->index_array_create( rBuffer, nOffset, nCount );
	if ( !t3dRdRegisterCreated( rContext, rd, created, rName, T3D_RD_CONTEXT_RESOURCE_INDEX_ARRAY, owner ) ) {
		return false;
	}

	m_state.publish( created, owner, 1u );
	return true;
}

bool Terrain3DRdGeometryView::createIndexedTracked(
	Terrain3DRdContext		  &rContext,
	const t3dRdResourceOwner_t owner,
	const String			  &rVertexName,
	const String			  &rIndexName,
	const uint32_t			   nVertexCount,
	const int64_t			   nVertexFormat,
	const TypedArray< RID >	  &rVertexBuffers,
	const PackedInt64Array	  &rVertexOffsets,
	const RID				  &rIndexBuffer,
	const uint32_t			   nIndexOffset,
	const uint32_t			   nIndexCount ) {
	if ( isValid() ) {
		return false;
	}

	Terrain3DRdGeometryView replacement;
	if ( !replacement.createVertexTracked( rContext,
										   owner,
										   rVertexName,
										   nVertexCount,
										   nVertexFormat,
										   rVertexBuffers,
										   rVertexOffsets ) ) {
		return false;
	}
	if ( !replacement.createIndexTracked( rContext, owner, rIndexName, rIndexBuffer, nIndexOffset, nIndexCount ) ) {
		replacement.releaseTracked( rContext );
		return false;
	}
	swap( replacement );
	return true;
}

void Terrain3DRdGeometryView::releaseTracked(
	Terrain3DRdContext &rContext ) {
	if ( m_state.rid( 1u ).is_valid() )
		rContext.freeResource( m_state.rid( 1u ) );
	if ( m_state.rid().is_valid() )
		rContext.freeResource( m_state.rid() );
	abandon();
}

void Terrain3DRdGeometryView::abandon(
	void ) {
	m_state.abandon();
}

void Terrain3DRdGeometryView::swap(
	Terrain3DRdGeometryView &rOther ) {
	m_state.swap( rOther.m_state );
}

// -------- Consolidated from rendering/rd/infrastructure/targets/terrain_3d_rd_framebuffer.cpp --------
bool Terrain3DRdFramebuffer::createTracked(
	Terrain3DRdContext		  &rContext,
	const t3dRdResourceOwner_t owner,
	const String			  &rName,
	const TypedArray< RID >	  &rAttachments,
	const int64_t			   nValidateWithFormat,
	const uint32_t			   nViewCount ) {
	if ( isValid() || rName.is_empty() || rAttachments.size() == 0 || nViewCount == 0u ) {
		return false;
	}
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd ) {
		return false;
	}

	RID created = rd->framebuffer_create( rAttachments, nValidateWithFormat, nViewCount );
	if ( !t3dRdRegisterCreated( rContext, rd, created, rName, T3D_RD_CONTEXT_RESOURCE_FRAMEBUFFER, owner ) ) {
		return false;
	}

	m_state.publish( created, owner );
	return true;
}

bool Terrain3DRdFramebuffer::replaceTracked(
	Terrain3DRdContext		  &rContext,
	const t3dRdResourceOwner_t owner,
	const String			  &rName,
	const TypedArray< RID >	  &rAttachments,
	const int64_t			   nValidateWithFormat,
	const uint32_t			   nViewCount ) {
	Terrain3DRdFramebuffer replacement;
	if ( !replacement.createTracked( rContext, owner, rName, rAttachments, nValidateWithFormat, nViewCount ) ) {
		return false;
	}
	swap( replacement );
	replacement.releaseTracked( rContext );
	return true;
}

int64_t Terrain3DRdFramebuffer::format(
	RenderingDevice *const pRd ) const {
	if ( !pRd || !m_state.rid().is_valid() ) {
		return RenderingDevice::INVALID_ID;
	}
	return pRd->framebuffer_get_format( m_state.rid() );
}

void Terrain3DRdFramebuffer::releaseTracked(
	Terrain3DRdContext &rContext ) {
	if ( m_state.rid().is_valid() )
		rContext.freeResource( m_state.rid() );
	abandon();
}

void Terrain3DRdFramebuffer::abandon(
	void ) {
	m_state.abandon();
}

void Terrain3DRdFramebuffer::swap(
	Terrain3DRdFramebuffer &rOther ) {
	m_state.swap( rOther.m_state );
}
