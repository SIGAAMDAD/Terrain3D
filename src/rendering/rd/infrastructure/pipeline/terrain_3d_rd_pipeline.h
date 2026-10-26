#ifndef TERRAIN3D_RD_PIPELINE_H
#define TERRAIN3D_RD_PIPELINE_H

#include "Terrain3DTypes.h"

#include "rendering/rd/infrastructure/resources/terrain_3d_rd_resource_state.h"

// t3dRdPipelineKind_e centralized in Terrain3DTypes.h.


// Owns one physical pipeline. Cache entries decide when to replace it; this
// object alone creates, registers and releases its RenderingDevice RID.
class Terrain3DRdPipeline {
private:
	Terrain3DRdResourceState<1u> m_state;
	t3dRdPipelineKind_t m_kind = T3D_RD_PIPELINE_KIND_NONE;
	bool m_local = false;

	static uint64_t beginPhysicalCreation( const String& name, t3dRdPipelineKind_t kind );
	static void finishPhysicalCreation( const String& name, const RID& pipeline, uint64_t started );

public:
	Terrain3DRdPipeline() = default;
	Terrain3DRdPipeline( const Terrain3DRdPipeline & ) = delete;
	Terrain3DRdPipeline &operator=( const Terrain3DRdPipeline & ) = delete;

	const RID &rid() const { return m_state.rid(); }
	bool isValid() const { return m_state.isValid(); }
	t3dRdPipelineKind_t kind() const { return m_kind; }
	uint32_t generation() const { return m_state.generation(); }
	t3dRdResourceOwner_t owner() const { return m_state.owner(); }
	bool isLocal() const { return m_local; }

	template <typename TCreator>
	bool createTracked( Terrain3DRdContext &context, t3dRdResourceOwner_t owner,
	        const String &name, t3dRdPipelineKind_t kind, TCreator creator ) {
		if ( isValid() || kind == T3D_RD_PIPELINE_KIND_NONE || name.is_empty() ) { return false; }
		RenderingDevice *const rd = context.getDevice();
		if ( !rd ) { return false; }
		const uint64_t started = beginPhysicalCreation( name, kind );
		RID created = creator( rd );
		finishPhysicalCreation( name, created, started );
		const t3dRdResourceType_t type = kind == T3D_RD_PIPELINE_KIND_RASTER
		        ? T3D_RD_CONTEXT_RESOURCE_RASTER_PIPELINE : T3D_RD_CONTEXT_RESOURCE_COMPUTE_PIPELINE;
		if ( !t3dRdRegisterCreated( context, rd, created, name, type, owner, false ) ) { return false; }
		m_kind = kind;
		m_local = false;
		m_state.publish( created, owner );
		context.recordRenderEvent( owner, EVENT_PIPELINE_CREATE );
		return true;
	}

	template <typename... TArgs>
	bool createTrackedRaster( Terrain3DRdContext &context, t3dRdResourceOwner_t owner,
	        const String &name, TArgs &&...args ) {
		return createTracked( context, owner, name, T3D_RD_PIPELINE_KIND_RASTER,
		        [&]( RenderingDevice *rd ) { return rd->render_pipeline_create( args... ); } );
	}

	bool createTrackedCompute( Terrain3DRdContext &context, t3dRdResourceOwner_t owner,
	        const String &name, const RID &shader );
	bool createLocalCompute( RenderingDevice *rd, const RID &shader );
	void releaseTracked( Terrain3DRdContext &context );
	void releaseLocal( RenderingDevice *rd );
	void abandon();
	void swap( Terrain3DRdPipeline &other );
};

#endif // TERRAIN3D_RD_PIPELINE_H
