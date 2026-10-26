#ifndef TERRAIN3D_RD_RESOURCE_CACHE_H
#define TERRAIN3D_RD_RESOURCE_CACHE_H

#include "rendering/rd/core/terrain_3d_rd_context.h"

template <typename TKey, typename TResource>
struct t3dRdResourceCacheEntry_t {
	TKey m_key;
	TResource m_resource;
	TResource &resource() { return m_resource; }
	const TResource &resource() const { return m_resource; }
};

// Fixed-capacity render-thread storage. Typed frontends own resource creation;
// this core owns lookup, deterministic replacement and explicit RID retirement.
template <typename TKey, typename TResource, uint32_t Capacity,
        t3dRenderTelemetryEvent_t EvictionEvent,
        typename TEntry = t3dRdResourceCacheEntry_t<TKey, TResource>>
class Terrain3DRdResourceCache {
	static_assert( Capacity > 0u, "Resource cache needs at least one slot" );

protected:
	Terrain3DRdContext &m_context;
	t3dRdResourceOwner_t m_owner;

private:
	TEntry m_entries[Capacity]{};
	uint32_t m_nCount = 0u;
	uint32_t m_nEvictCursor = 0u;

public:
	Terrain3DRdResourceCache( Terrain3DRdContext &rContext, t3dRdResourceOwner_t owner ) :
	        m_context( rContext ), m_owner( owner ) {}

	const TEntry *find( const TKey &rKey ) const {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].m_key == rKey ) { return &m_entries[i]; }
		}
		return NULL;
	}

	RID insert( const TKey &rKey, TResource &candidate ) {
		if ( !candidate.isValid() ) { return RID(); }
		const bool evicted = m_nCount == Capacity && m_entries[m_nEvictCursor % Capacity].resource().isValid();
		const uint32_t index = m_nCount < Capacity ? m_nCount++ : ( m_nEvictCursor++ % Capacity );
		if ( evicted ) { m_context.recordRenderEvent( m_owner, EvictionEvent ); }
		m_entries[index].resource().releaseTracked( m_context );
		m_entries[index].m_key = rKey;
		m_entries[index].resource().swap( candidate );
		return m_entries[index].resource().rid();
	}

	void erase( const RID &rResource ) {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].resource().rid() == rResource ) {
				m_entries[i].resource().releaseTracked( m_context );
				const uint32_t last = --m_nCount;
				if ( i != last ) {
					m_entries[i].m_key = m_entries[last].m_key;
					m_entries[i].resource().swap( m_entries[last].resource() );
				}
				return;
			}
		}
	}

	void clear() {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			m_entries[i].resource().releaseTracked( m_context );
		}
		m_nCount = m_nEvictCursor = 0u;
	}

	void abandon() {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) { m_entries[i].resource().abandon(); }
		m_nCount = m_nEvictCursor = 0u;
	}

	uint32_t size() const { return m_nCount; }
};

#endif // TERRAIN3D_RD_RESOURCE_CACHE_H
