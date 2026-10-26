#ifndef TERRAIN3D_RD_DESCRIPTOR_CACHE_H
#define TERRAIN3D_RD_DESCRIPTOR_CACHE_H

#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"
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
		RID set;
	} Entry;

	Terrain3DRdContext &m_context;
	Terrain3DRdContext::ResourceOwner m_owner;
	Entry m_entries[Capacity]{};
	uint32_t m_nCount = 0u;
	uint32_t m_nEvictCursor = 0u;

public:
	Terrain3DRdDescriptorCache( Terrain3DRdContext &rContext, Terrain3DRdContext::ResourceOwner owner ) :
	        m_context( rContext ), m_owner( owner ) {}

	RID find( const TKey &rKey ) const {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].key == rKey ) {
				return m_entries[i].set;
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
		RID created;
		if ( !Terrain3DRdResourceService( m_context, m_owner )
	                        .createUniformSet( created, rName, rUniforms, rShader, nSetIndex ) ) {
			return RID();
		}
		const bool evicted = m_nCount == Capacity && m_entries[m_nEvictCursor % Capacity].set.is_valid();
		const uint32_t index = m_nCount < Capacity ? m_nCount++ : ( m_nEvictCursor++ % Capacity );
		if ( evicted ) {
			m_context.recordRenderEvent( m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_DESCRIPTOR_EVICT );
		}
		m_context.freeResource( m_entries[index].set );
		m_entries[index] = { rKey, created };
		return created;
	}

	void erase( const RID &rSet ) {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].set == rSet ) {
				m_context.freeResource( m_entries[i].set );
				m_entries[i] = m_entries[--m_nCount];
				m_entries[m_nCount] = Entry{};
				return;
			}
		}
	}

	void clear() {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			m_context.freeResource( m_entries[i].set );
			m_entries[i] = Entry{};
		}
		m_nCount = 0u;
		m_nEvictCursor = 0u;
	}

	void abandon() {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			m_entries[i] = Entry{};
		}
		m_nCount = 0u;
		m_nEvictCursor = 0u;
	}

	uint32_t size() const { return m_nCount; }
};

#endif // TERRAIN3D_RD_DESCRIPTOR_CACHE_H
