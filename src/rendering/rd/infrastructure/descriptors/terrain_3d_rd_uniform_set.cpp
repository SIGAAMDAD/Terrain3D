#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_uniform_set.h"

#include <EASTL/utility.h>

bool Terrain3DRdUniformSet::createTracked( Terrain3DRdContext &context,
        const t3dRdResourceOwner_t owner, const String &name,
        const TypedArray<Ref<RDUniform>> &uniforms, const RID &shader, const uint32_t set ) {
	if ( isValid() || name.is_empty() || !shader.is_valid() ) { return false; }
	RenderingDevice *const rd = context.getDevice();
	if ( !rd ) { return false; }
	RID created = rd->uniform_set_create( uniforms, shader, set );
	if ( !created.is_valid() || !context.registerResource( created, name,
	             T3D_RD_CONTEXT_RESOURCE_UNIFORM_SET, T3D_RD_CONTEXT_MEMORY_NONE, 0u, owner ) ) {
		context.recordRenderEvent( owner, EVENT_RESOURCE_FAILURE );
		if ( created.is_valid() ) rd->free_rid( created );
		return false;
	}
	m_rid = created;
	m_owner = owner;
	m_local = false;
	m_generation = nextGeneration( m_generation );
	context.recordRenderEvent( owner, EVENT_DESCRIPTOR_CREATE );
	return true;
}

bool Terrain3DRdUniformSet::createLocal( RenderingDevice *rd,
        const TypedArray<Ref<RDUniform>> &uniforms, const RID &shader, const uint32_t set ) {
	if ( isValid() || !rd || !shader.is_valid() ) { return false; }
	RID created = rd->uniform_set_create( uniforms, shader, set );
	if ( !created.is_valid() ) { return false; }
	m_rid = created;
	m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
	m_local = true;
	m_generation = nextGeneration( m_generation );
	return true;
}

void Terrain3DRdUniformSet::releaseTracked( Terrain3DRdContext &context ) {
	if ( !isValid() || m_local ) { return; }
	context.freeResource( m_rid );
	abandon();
}

void Terrain3DRdUniformSet::releaseLocal( RenderingDevice *rd ) {
	if ( !isValid() || !m_local || !rd ) { return; }
	rd->free_rid( m_rid );
	abandon();
}

void Terrain3DRdUniformSet::abandon() {
	m_rid = RID();
	m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
	m_local = false;
	m_generation = nextGeneration( m_generation );
}

void Terrain3DRdUniformSet::swap( Terrain3DRdUniformSet &other ) {
	using eastl::swap;
	swap( m_rid, other.m_rid );
	swap( m_owner, other.m_owner );
	swap( m_local, other.m_local );
	m_generation = nextGeneration( m_generation );
	other.m_generation = nextGeneration( other.m_generation );
}
