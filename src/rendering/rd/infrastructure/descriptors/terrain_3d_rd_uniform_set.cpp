#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_uniform_set.h"

#include <EASTL/utility.h>

bool Terrain3DRdUniformSet::createTracked( Terrain3DRdContext &context,
        const t3dRdResourceOwner_t owner, const String &name,
        const TypedArray<Ref<RDUniform>> &uniforms, const RID &shader, const uint32_t set ) {
	if ( isValid() || name.is_empty() || !shader.is_valid() ) { return false; }
	RenderingDevice *const rd = context.getDevice();
	if ( !rd ) { return false; }
	RID created = rd->uniform_set_create( uniforms, shader, set );
	if ( !t3dRdRegisterCreated( context, rd, created, name, T3D_RD_CONTEXT_RESOURCE_UNIFORM_SET, owner, false ) ) { return false; }
	m_local = false;
	m_state.publish( created, owner );
	context.recordRenderEvent( owner, EVENT_DESCRIPTOR_CREATE );
	return true;
}

bool Terrain3DRdUniformSet::createLocal( RenderingDevice *rd,
        const TypedArray<Ref<RDUniform>> &uniforms, const RID &shader, const uint32_t set ) {
	if ( isValid() || !rd || !shader.is_valid() ) { return false; }
	RID created = rd->uniform_set_create( uniforms, shader, set );
	if ( !created.is_valid() ) { return false; }
	m_local = true;
	m_state.publish( created, T3D_RD_CONTEXT_OWNER_UNSPECIFIED );
	return true;
}

void Terrain3DRdUniformSet::releaseTracked( Terrain3DRdContext &context ) {
	if ( !isValid() || m_local ) { return; }
	context.freeResource( m_state.rid() );
	abandon();
}

void Terrain3DRdUniformSet::releaseLocal( RenderingDevice *rd ) {
	if ( !isValid() || !m_local || !rd ) { return; }
	rd->free_rid( m_state.rid() );
	abandon();
}

void Terrain3DRdUniformSet::abandon() {
	m_local = false;
	m_state.abandon();
}

void Terrain3DRdUniformSet::swap( Terrain3DRdUniformSet &other ) {
	using eastl::swap;
	m_state.swap( other.m_state, true );
	swap( m_local, other.m_local );
}
