#include "rendering/rd/infrastructure/resources/terrain_3d_rd_sampler.h"

bool Terrain3DRdSampler::createTracked( Terrain3DRdContext &rContext, const t3dRdResourceOwner_t owner,
        const String &rName, const Ref<RDSamplerState> &rState ) {
	if ( isValid() || rName.is_empty() || rState.is_null() ) { return false; }
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd ) { return false; }

	RID created = rd->sampler_create( rState );
	if ( !t3dRdRegisterCreated( rContext, rd, created, rName, T3D_RD_CONTEXT_RESOURCE_SAMPLER, owner ) ) { return false; }

	m_state.publish( created, owner );
	return true;
}

void Terrain3DRdSampler::releaseTracked( Terrain3DRdContext &rContext ) {
	if ( m_state.rid().is_valid() ) rContext.freeResource( m_state.rid() );
	abandon();
}

void Terrain3DRdSampler::abandon( void ) {
	m_state.abandon();
}

void Terrain3DRdSampler::swap( Terrain3DRdSampler &rOther ) {
	m_state.swap( rOther.m_state );
}
