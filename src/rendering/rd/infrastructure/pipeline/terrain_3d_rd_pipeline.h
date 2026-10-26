#ifndef TERRAIN3D_RD_PIPELINE_H
#define TERRAIN3D_RD_PIPELINE_H

#include "Terrain3DTypes.h"

#include "rendering/rd/core/terrain_3d_rd_context.h"

// t3dRdPipelineKind_e centralized in Terrain3DTypes.h.


// Owns one physical pipeline. Cache entries decide when to replace it; this
// object alone creates, registers and releases its RenderingDevice RID.
class Terrain3DRdPipeline {
private:
	RID m_rid;
	t3dRdPipelineKind_t m_kind = T3D_RD_PIPELINE_KIND_NONE;
	t3dRdResourceOwner_t m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
	uint32_t m_generation = 0u;
	bool m_local = false;

	static uint32_t nextGeneration( uint32_t value ) { return value == UINT32_MAX ? 1u : value + 1u; }

public:
	Terrain3DRdPipeline() = default;
	Terrain3DRdPipeline( const Terrain3DRdPipeline & ) = delete;
	Terrain3DRdPipeline &operator=( const Terrain3DRdPipeline & ) = delete;

	const RID &rid() const { return m_rid; }
	bool isValid() const { return m_rid.is_valid(); }
	t3dRdPipelineKind_t kind() const { return m_kind; }
	uint32_t generation() const { return m_generation; }
	t3dRdResourceOwner_t owner() const { return m_owner; }
	bool isLocal() const { return m_local; }

	template <typename TCreator>
	bool createTracked( Terrain3DRdContext &context, t3dRdResourceOwner_t owner,
	        const String &name, t3dRdPipelineKind_t kind, TCreator creator ) {
		if ( isValid() || kind == T3D_RD_PIPELINE_KIND_NONE || name.is_empty() ) { return false; }
		RenderingDevice *const rd = context.getDevice();
		if ( !rd ) { return false; }
		RID created = creator( rd );
		const t3dRdResourceType_t type = kind == T3D_RD_PIPELINE_KIND_RASTER
		        ? T3D_RD_CONTEXT_RESOURCE_RASTER_PIPELINE : T3D_RD_CONTEXT_RESOURCE_COMPUTE_PIPELINE;
		if ( !created.is_valid() || !context.registerResource( created, name, type,
		             T3D_RD_CONTEXT_MEMORY_NONE, 0u, owner ) ) {
			context.recordRenderEvent( owner, EVENT_RESOURCE_FAILURE );
			if ( created.is_valid() ) rd->free_rid( created );
			return false;
		}
		m_rid = created;
		m_kind = kind;
		m_owner = owner;
		m_local = false;
		m_generation = nextGeneration( m_generation );
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
