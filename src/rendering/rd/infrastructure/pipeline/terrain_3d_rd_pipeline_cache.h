#ifndef TERRAIN3D_RD_PIPELINE_CACHE_H
#define TERRAIN3D_RD_PIPELINE_CACHE_H

#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_resource_cache.h"

template< typename TKey > class t3dRdPipelineCacheEntry_t
{
public:
	TKey				 m_key;
	Terrain3DRdPipeline	 m_pipeline;
	Terrain3DRdPipeline &resource() {
		return m_pipeline;
	}
	const Terrain3DRdPipeline &resource() const {
		return m_pipeline;
	}
};

// Fixed-capacity render-thread cache. Domains define key semantics and pipeline
// state; the cache owns lookup, deterministic eviction, and RID lifetime.
template< typename TKey, uint32_t Capacity >
class Terrain3DRdPipelineCache : public Terrain3DRdResourceCache< TKey,
																  Terrain3DRdPipeline,
																  Capacity,
																  EVENT_PIPELINE_EVICT,
																  t3dRdPipelineCacheEntry_t< TKey > >
{
	using Cache = Terrain3DRdResourceCache< TKey,
											Terrain3DRdPipeline,
											Capacity,
											EVENT_PIPELINE_EVICT,
											t3dRdPipelineCacheEntry_t< TKey > >;

public:
	Terrain3DRdPipelineCache(
		Terrain3DRdContext	&rContext,
		t3dRdResourceOwner_t owner ) :
		Cache( rContext, owner ) {
	}

	template< typename... TArgs >
	RID getOrCreateRaster(
		const TKey	 &rKey,
		const String &rName,
		TArgs &&...args ) {
		if ( const t3dRdPipelineCacheEntry_t< TKey > *const existing = this->find( rKey ) ) {
			return existing->m_pipeline.rid();
		}
		Terrain3DRdPipeline candidate;
		if ( !candidate.createTrackedRaster( this->m_context, this->m_owner, rName, args... ) ) {
			return RID();
		}
		return this->insert( rKey, candidate );
	}

	RID getOrCreateCompute(
		const TKey	 &rKey,
		const String &rName,
		const RID	 &rShader ) {
		if ( const t3dRdPipelineCacheEntry_t< TKey > *const existing = this->find( rKey ) ) {
			return existing->m_pipeline.rid();
		}
		Terrain3DRdPipeline candidate;
		if ( !candidate.createTrackedCompute( this->m_context, this->m_owner, rName, rShader ) ) {
			return RID();
		}
		return this->insert( rKey, candidate );
	}
};

#endif // TERRAIN3D_RD_PIPELINE_CACHE_H
