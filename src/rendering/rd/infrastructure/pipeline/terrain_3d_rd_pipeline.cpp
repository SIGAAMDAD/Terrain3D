#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline.h"

#include <EASTL/utility.h>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

uint64_t Terrain3DRdPipeline::beginPhysicalCreation(
	const String			 &name,
	const t3dRdPipelineKind_t kind ) {
	UtilityFunctions::print( "[Terrain3D][PipelineCompile] BEGIN ", name, " kind=", ( uint32_t ) kind );
	return Time::get_singleton()->get_ticks_usec();
}

void Terrain3DRdPipeline::finishPhysicalCreation(
	const String  &name,
	const RID	  &pipeline,
	const uint64_t started ) {
	UtilityFunctions::print( "[Terrain3D][PipelineCompile] ",
							 pipeline.is_valid() ? "READY " : "FAIL ",
							 name,
							 " elapsed=",
							 Time::get_singleton()->get_ticks_usec() - started,
							 " us" );
}

bool Terrain3DRdPipeline::createTrackedCompute(
	Terrain3DRdContext		  &context,
	const t3dRdResourceOwner_t owner,
	const String			  &name,
	const RID				  &shader ) {
	if ( !shader.is_valid() ) {
		return false;
	}
	return createTracked( context, owner, name, T3D_RD_PIPELINE_KIND_COMPUTE, [&]( RenderingDevice *rd ) {
		return rd->compute_pipeline_create( shader );
	} );
}

bool Terrain3DRdPipeline::createLocalCompute(
	RenderingDevice *rd,
	const RID		&shader ) {
	if ( isValid() || !rd || !shader.is_valid() ) {
		return false;
	}
	RID created = rd->compute_pipeline_create( shader );
	if ( !created.is_valid() ) {
		return false;
	}
	m_kind = T3D_RD_PIPELINE_KIND_COMPUTE;
	m_local = true;
	m_state.publish( created, T3D_RD_CONTEXT_OWNER_UNSPECIFIED );
	return true;
}

void Terrain3DRdPipeline::releaseTracked(
	Terrain3DRdContext &context ) {
	if ( !isValid() || m_local ) {
		return;
	}
	context.freeResource( m_state.rid() );
	abandon();
}

void Terrain3DRdPipeline::releaseLocal(
	RenderingDevice *rd ) {
	if ( !isValid() || !m_local || !rd ) {
		return;
	}
	rd->free_rid( m_state.rid() );
	abandon();
}

void Terrain3DRdPipeline::abandon() {
	m_kind = T3D_RD_PIPELINE_KIND_NONE;
	m_local = false;
	m_state.abandon();
}

void Terrain3DRdPipeline::swap(
	Terrain3DRdPipeline &other ) {
	using eastl::swap;
	m_state.swap( other.m_state, true );
	swap( m_kind, other.m_kind );
	swap( m_local, other.m_local );
}
