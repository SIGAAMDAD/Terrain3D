#ifndef TERRAIN3D_RD_DESCRIPTOR_CACHE_H
#define TERRAIN3D_RD_DESCRIPTOR_CACHE_H

#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_uniform_set.h"
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/typed_array.hpp>

// Key equality is supplied by the domain. Only shader, layout and bound RID
// identities belong in keys; per-frame buffer contents do not.
template <typename TKey, uint32_t Capacity>
class Terrain3DRdDescriptorCache
{
	static_assert( Capacity > 0u, "Descriptor cache needs at least one slot" );
	typedef struct Entry_s
	{

		TKey key;
		Terrain3DRdUniformSet set;
	} Entry;


	Terrain3DRdContext &m_context;
	t3dRdResourceOwner_t m_owner;
	Entry m_entries[Capacity]{};
	uint32_t m_nCount = 0u;
	uint32_t m_nEvictCursor = 0u;

public:
	Terrain3DRdDescriptorCache( Terrain3DRdContext &rContext, t3dRdResourceOwner_t owner ) :
	        m_context( rContext ), m_owner( owner ) {}

	RID find( const TKey &rKey ) const {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].key == rKey ) {
				return m_entries[i].set.rid();
			}
		}
		return RID();
	}

	RID getOrCreate( const TKey &rKey,
	        const String &rName,
	        const TypedArray<Ref<RDUniform>> &rUniforms,
	        const RID &rShader,
	        uint32_t nSetIndex ) {
		const RID existing = find( rKey );
		if ( existing.is_valid() ) {
			return existing;
		}
		Terrain3DRdUniformSet created;
		if ( !created.createTracked( m_context, m_owner, rName, rUniforms, rShader, nSetIndex ) ) {
			return RID();
		}
		const bool evicted = m_nCount == Capacity && m_entries[m_nEvictCursor % Capacity].set.isValid();
		const uint32_t index = m_nCount < Capacity ? m_nCount++ : ( m_nEvictCursor++ % Capacity );
		if ( evicted ) {
			m_context.recordRenderEvent( m_owner, EVENT_DESCRIPTOR_EVICT );
		}
		m_entries[index].set.releaseTracked( m_context );
		m_entries[index].key = rKey;
		m_entries[index].set.swap( created );
		return m_entries[index].set.rid();
	}

	void erase( const RID &rSet ) {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].set.rid() == rSet ) {
				m_entries[i].set.releaseTracked( m_context );
				const uint32_t last = --m_nCount;
				if ( i != last ) {
					m_entries[i].key = m_entries[last].key;
					m_entries[i].set.swap( m_entries[last].set );
				}
				return;
			}
		}
	}

	void clear() {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			m_entries[i].set.releaseTracked( m_context );
		}
		m_nCount = 0u;
		m_nEvictCursor = 0u;
	}

	void abandon() {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			m_entries[i].set.abandon();
		}
		m_nCount = 0u;
		m_nEvictCursor = 0u;
	}

	uint32_t size() const { return m_nCount; }
};

#endif // TERRAIN3D_RD_DESCRIPTOR_CACHE_H
