#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline.h"

#include <EASTL/utility.h>

bool Terrain3DRdPipeline::createTrackedCompute( Terrain3DRdContext &context,
        const t3dRdResourceOwner_t owner, const String &name, const RID &shader ) {
	if ( !shader.is_valid() ) { return false; }
	return createTracked( context, owner, name, T3D_RD_PIPELINE_KIND_COMPUTE,
	        [&]( RenderingDevice *rd ) { return rd->compute_pipeline_create( shader ); } );
}

bool Terrain3DRdPipeline::createLocalCompute( RenderingDevice *rd, const RID &shader ) {
	if ( isValid() || !rd || !shader.is_valid() ) { return false; }
	RID created = rd->compute_pipeline_create( shader );
	if ( !created.is_valid() ) { return false; }
	m_rid = created;
	m_kind = T3D_RD_PIPELINE_KIND_COMPUTE;
	m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
	m_local = true;
	m_generation = nextGeneration( m_generation );
	return true;
}

void Terrain3DRdPipeline::releaseTracked( Terrain3DRdContext &context ) {
	if ( !isValid() || m_local ) { return; }
	context.freeResource( m_rid );
	abandon();
}

void Terrain3DRdPipeline::releaseLocal( RenderingDevice *rd ) {
	if ( !isValid() || !m_local || !rd ) { return; }
	rd->free_rid( m_rid );
	abandon();
}

void Terrain3DRdPipeline::abandon() {
	m_rid = RID();
	m_kind = T3D_RD_PIPELINE_KIND_NONE;
	m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
	m_local = false;
	m_generation = nextGeneration( m_generation );
}

void Terrain3DRdPipeline::swap( Terrain3DRdPipeline &other ) {
	using eastl::swap;
	swap( m_rid, other.m_rid );
	swap( m_kind, other.m_kind );
	swap( m_owner, other.m_owner );
	swap( m_local, other.m_local );
	m_generation = nextGeneration( m_generation );
	other.m_generation = nextGeneration( other.m_generation );
}
