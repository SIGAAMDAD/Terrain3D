#ifndef TERRAIN3D_RD_PIPELINE_CACHE_H
#define TERRAIN3D_RD_PIPELINE_CACHE_H

#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline.h"

template <typename TKey>
class t3dRdPipelineCacheEntry_t {
public:

	TKey m_key;
	Terrain3DRdPipeline m_pipeline;
};

// Fixed-capacity render-thread cache. Domains define key semantics and pipeline
// state; the cache owns lookup, deterministic eviction, and RID lifetime.
template <typename TKey, uint32_t Capacity>
class Terrain3DRdPipelineCache
{
	static_assert( Capacity > 0u, "Pipeline cache needs at least one slot" );

private:
	Terrain3DRdContext &m_context;
	t3dRdResourceOwner_t m_owner;
	t3dRdPipelineCacheEntry_t<TKey> m_entries[Capacity]{};
	uint32_t m_nCount = 0u;
	uint32_t m_nEvictCursor = 0u;

public:
	Terrain3DRdPipelineCache( Terrain3DRdContext &rContext, t3dRdResourceOwner_t owner ) :
	        m_context( rContext ), m_owner( owner ) {}

	const t3dRdPipelineCacheEntry_t<TKey> *find( const TKey &rKey ) const {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].m_key == rKey ) {
				return &m_entries[i];
			}
		}
		return NULL;
	}

	RID insert( const TKey &rKey, Terrain3DRdPipeline &candidate ) {
		if ( !candidate.isValid() ) { return RID(); }
		const bool evicted = m_nCount == Capacity && m_entries[m_nEvictCursor % Capacity].m_pipeline.isValid();
		const uint32_t index = m_nCount < Capacity ? m_nCount++ : ( m_nEvictCursor++ % Capacity );
		if ( evicted ) {
			m_context.recordRenderEvent( m_owner, EVENT_PIPELINE_EVICT );
		}
		m_entries[index].m_pipeline.releaseTracked( m_context );
		m_entries[index].m_key = rKey;
		m_entries[index].m_pipeline.swap( candidate );
		return m_entries[index].m_pipeline.rid();
	}

	template <typename... TArgs>
	RID getOrCreateRaster( const TKey &rKey, const String &rName, TArgs &&...args ) {
		if ( const t3dRdPipelineCacheEntry_t<TKey> *const existing = find( rKey ) ) {
			return existing->m_pipeline.rid();
		}
		Terrain3DRdPipeline candidate;
		if ( !candidate.createTrackedRaster( m_context, m_owner, rName, args... ) ) {
			return RID();
		}
		return insert( rKey, candidate );
	}

	RID getOrCreateCompute( const TKey &rKey, const String &rName, const RID &rShader ) {
		if ( const t3dRdPipelineCacheEntry_t<TKey> *const existing = find( rKey ) ) { return existing->m_pipeline.rid(); }
		Terrain3DRdPipeline candidate;
		if ( !candidate.createTrackedCompute( m_context, m_owner, rName, rShader ) ) { return RID(); }
		return insert( rKey, candidate );
	}

	void erase( const RID &rPipeline ) {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].m_pipeline.rid() == rPipeline ) {
				m_entries[i].m_pipeline.releaseTracked( m_context );
				const uint32_t last = --m_nCount;
				if ( i != last ) {
					m_entries[i].m_key = m_entries[last].m_key;
					m_entries[i].m_pipeline.swap( m_entries[last].m_pipeline );
				}
				return;
			}
		}
	}

	void clear() {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			m_entries[i].m_pipeline.releaseTracked( m_context );
		}
		m_nCount = 0u;
		m_nEvictCursor = 0u;
	}

	void abandon() {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) m_entries[i].m_pipeline.abandon();
		m_nCount = 0u;
		m_nEvictCursor = 0u;
	}

	uint32_t size() const { return m_nCount; }
};

#endif // TERRAIN3D_RD_PIPELINE_CACHE_H
