#ifndef TERRAIN3D_RD_PIPELINE_CACHE_H
#define TERRAIN3D_RD_PIPELINE_CACHE_H

#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"

// Fixed-capacity render-thread cache. Domains define key semantics and pipeline
// state; the cache owns lookup, deterministic eviction, and RID lifetime.
template <typename TKey, uint32_t Capacity>
class Terrain3DRdPipelineCache
{
	static_assert( Capacity > 0u, "Pipeline cache needs at least one slot" );

public:
	typedef struct Entry_s
	{
		TKey m_key;
		RID m_hPipeline;
	} Entry;

private:
	Terrain3DRdContext &m_context;
	Terrain3DRdContext::ResourceOwner m_owner;
	Entry m_entries[Capacity]{};
	uint32_t m_nCount = 0u;
	uint32_t m_nEvictCursor = 0u;

public:
	Terrain3DRdPipelineCache( Terrain3DRdContext &rContext, Terrain3DRdContext::ResourceOwner owner ) :
	        m_context( rContext ), m_owner( owner ) {}

	const Entry *find( const TKey &rKey ) const {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].m_key == rKey ) {
				return &m_entries[i];
			}
		}
		return nullptr;
	}

	template <typename TCreator>
	RID getOrCreate( const TKey &rKey,
	        const String &rName,
	        Terrain3DRdContext::ResourceType type,
	        TCreator creator ) {
		if ( const Entry *const existing = find( rKey ) ) {
			return existing->m_hPipeline;
		}
		RID pipeline;
		if ( !Terrain3DRdResourceService( m_context, m_owner )
	                        .create( pipeline, rName, type, Terrain3DRdContext::MEMORY_NONE, 0u, creator ) ) {
			return RID();
		}
		const bool evicted = m_nCount == Capacity && m_entries[m_nEvictCursor % Capacity].m_hPipeline.is_valid();
		const uint32_t index = m_nCount < Capacity ? m_nCount++ : ( m_nEvictCursor++ % Capacity );
		if ( evicted ) {
			m_context.recordRenderEvent( m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_PIPELINE_EVICT );
		}
		m_context.freeResource( m_entries[index].m_hPipeline );
		m_entries[index] = { rKey, pipeline };
		return pipeline;
	}

	template <typename... TArgs>
	RID getOrCreateRaster( const TKey &rKey, const String &rName, TArgs &&...args ) {
		return getOrCreate( rKey,
		        rName,
		        Terrain3DRdContext::RESOURCE_RASTER_PIPELINE,
		        [&]( RenderingDevice *pRd ) { return pRd->render_pipeline_create( args... ); } );
	}

	RID getOrCreateCompute( const TKey &rKey, const String &rName, const RID &rShader ) {
		return getOrCreate( rKey,
		        rName,
		        Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE,
		        [&]( RenderingDevice *pRd ) { return pRd->compute_pipeline_create( rShader ); } );
	}

	void erase( const RID &rPipeline ) {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].m_hPipeline == rPipeline ) {
				m_context.freeResource( m_entries[i].m_hPipeline );
				m_entries[i] = m_entries[--m_nCount];
				m_entries[m_nCount] = Entry{};
				return;
			}
		}
	}

	void clear() {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			m_context.freeResource( m_entries[i].m_hPipeline );
			m_entries[i] = Entry{};
		}
		m_nCount = 0u;
		m_nEvictCursor = 0u;
	}

	uint32_t size() const { return m_nCount; }
};

#endif // TERRAIN3D_RD_PIPELINE_CACHE_H
