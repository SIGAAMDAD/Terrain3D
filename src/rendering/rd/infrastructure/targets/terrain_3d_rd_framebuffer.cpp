#include "rendering/rd/infrastructure/targets/terrain_3d_rd_framebuffer.h"

#include <EASTL/utility.h>

bool Terrain3DRdFramebuffer::createTracked( Terrain3DRdContext &rContext,
        const t3dRdResourceOwner_t owner,
        const String &rName,
        const TypedArray<RID> &rAttachments,
        const int64_t nValidateWithFormat,
        const uint32_t nViewCount ) {
	if ( isValid() || rName.is_empty() || rAttachments.size() == 0 || nViewCount == 0u ) { return false; }
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd ) { return false; }

	RID created = rd->framebuffer_create( rAttachments, nValidateWithFormat, nViewCount );
	if ( !created.is_valid() ) {
		rContext.recordResourceCreationFailure( rName, T3D_RD_CONTEXT_RESOURCE_FRAMEBUFFER );
		rContext.recordRenderEvent( owner, EVENT_RESOURCE_FAILURE );
		return false;
	}
	if ( !rContext.registerResource( created, rName, T3D_RD_CONTEXT_RESOURCE_FRAMEBUFFER,
	             T3D_RD_CONTEXT_MEMORY_NONE, 0u, owner ) ) {
		rContext.recordRenderEvent( owner, EVENT_RESOURCE_FAILURE );
		rd->free_rid( created );
		return false;
	}

	m_hRid = created;
	m_owner = owner;
	m_nGeneration = nextGeneration( m_nGeneration );
	return true;
}

bool Terrain3DRdFramebuffer::replaceTracked( Terrain3DRdContext &rContext,
        const t3dRdResourceOwner_t owner,
        const String &rName,
        const TypedArray<RID> &rAttachments,
        const int64_t nValidateWithFormat,
        const uint32_t nViewCount ) {
	Terrain3DRdFramebuffer replacement;
	if ( !replacement.createTracked( rContext, owner, rName, rAttachments, nValidateWithFormat, nViewCount ) ) {
		return false;
	}
	swap( replacement );
	replacement.releaseTracked( rContext );
	return true;
}

int64_t Terrain3DRdFramebuffer::format( RenderingDevice *const pRd ) const {
	if ( !pRd || !m_hRid.is_valid() ) { return RenderingDevice::INVALID_ID; }
	return pRd->framebuffer_get_format( m_hRid );
}

void Terrain3DRdFramebuffer::releaseTracked( Terrain3DRdContext &rContext ) {
	if ( m_hRid.is_valid() ) rContext.freeResource( m_hRid );
	abandon();
}

void Terrain3DRdFramebuffer::abandon( void ) {
	m_hRid = RID();
	m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
	m_nGeneration = nextGeneration( m_nGeneration );
}

void Terrain3DRdFramebuffer::swap( Terrain3DRdFramebuffer &rOther ) {
	using eastl::swap;
	const uint32_t this_generation = nextGeneration( m_nGeneration );
	const uint32_t other_generation = nextGeneration( rOther.m_nGeneration );
	swap( m_hRid, rOther.m_hRid );
	swap( m_owner, rOther.m_owner );
	m_nGeneration = this_generation;
	rOther.m_nGeneration = other_generation;
}
