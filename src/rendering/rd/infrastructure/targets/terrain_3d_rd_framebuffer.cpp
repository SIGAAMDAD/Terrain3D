#include "rendering/rd/infrastructure/targets/terrain_3d_rd_framebuffer.h"

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
	if ( !t3dRdRegisterCreated( rContext, rd, created, rName, T3D_RD_CONTEXT_RESOURCE_FRAMEBUFFER, owner ) ) { return false; }

	m_state.publish( created, owner );
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
	if ( !pRd || !m_state.rid().is_valid() ) { return RenderingDevice::INVALID_ID; }
	return pRd->framebuffer_get_format( m_state.rid() );
}

void Terrain3DRdFramebuffer::releaseTracked( Terrain3DRdContext &rContext ) {
	if ( m_state.rid().is_valid() ) rContext.freeResource( m_state.rid() );
	abandon();
}

void Terrain3DRdFramebuffer::abandon( void ) {
	m_state.abandon();
}

void Terrain3DRdFramebuffer::swap( Terrain3DRdFramebuffer &rOther ) {
	m_state.swap( rOther.m_state );
}
