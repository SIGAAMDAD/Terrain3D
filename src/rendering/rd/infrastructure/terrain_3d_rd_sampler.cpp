#include "rendering/rd/infrastructure/terrain_3d_rd_sampler.h"

#include <EASTL/utility.h>

bool Terrain3DRdSampler::createTracked( Terrain3DRdContext &rContext, const Terrain3DRdContext::ResourceOwner owner,
        const String &rName, const Ref<RDSamplerState> &rState ) {
	if ( isValid() || rName.is_empty() || rState.is_null() ) return false;
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd ) return false;

	RID created = rd->sampler_create( rState );
	if ( !created.is_valid() ) {
		rContext.recordResourceCreationFailure( rName, Terrain3DRdContext::RESOURCE_SAMPLER );
		rContext.recordRenderEvent( owner, Terrain3DRdContext::RenderTelemetry::EVENT_RESOURCE_FAILURE );
		return false;
	}
	if ( !rContext.registerResource( created, rName, Terrain3DRdContext::RESOURCE_SAMPLER,
	             Terrain3DRdContext::MEMORY_NONE, 0u, owner ) ) {
		rContext.recordRenderEvent( owner, Terrain3DRdContext::RenderTelemetry::EVENT_RESOURCE_FAILURE );
		rd->free_rid( created );
		return false;
	}

	m_hRid = created;
	m_owner = owner;
	m_nGeneration = nextGeneration( m_nGeneration );
	return true;
}

void Terrain3DRdSampler::releaseTracked( Terrain3DRdContext &rContext ) {
	if ( m_hRid.is_valid() ) rContext.freeResource( m_hRid );
	abandon();
}

void Terrain3DRdSampler::abandon( void ) {
	m_hRid = RID();
	m_owner = Terrain3DRdContext::OWNER_UNSPECIFIED;
	m_nGeneration = nextGeneration( m_nGeneration );
}

void Terrain3DRdSampler::swap( Terrain3DRdSampler &rOther ) {
	using eastl::swap;
	const uint32_t this_generation = nextGeneration( m_nGeneration );
	const uint32_t other_generation = nextGeneration( rOther.m_nGeneration );
	swap( m_hRid, rOther.m_hRid );
	swap( m_owner, rOther.m_owner );
	m_nGeneration = this_generation;
	rOther.m_nGeneration = other_generation;
}
