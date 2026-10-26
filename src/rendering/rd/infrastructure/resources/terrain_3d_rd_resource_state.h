#ifndef TERRAIN3D_RD_RESOURCE_STATE_H
#define TERRAIN3D_RD_RESOURCE_STATE_H

#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "core/utility/terrain_3d_generation.h"
#include <EASTL/utility.h>

// Metadata-only registration. Texture reservations and their failure policy
// remain in the texture wrapper; creation diagnostics stay caller-selectable.
inline bool t3dRdRegisterCreated(
	Terrain3DRdContext	&rContext,
	RenderingDevice		*pRd,
	const RID			&rRid,
	const String		&rName,
	t3dRdResourceType_t	 type,
	t3dRdResourceOwner_t owner,
	bool				 bRecordCreationFailure = true ) {
	if ( !rRid.is_valid() ) {
		if ( bRecordCreationFailure ) {
			rContext.recordResourceCreationFailure( rName, type );
		}
	} else if ( rContext.registerResource( rRid, rName, type, T3D_RD_CONTEXT_MEMORY_NONE, 0u, owner ) ) {
		return true;
	}
	rContext.recordRenderEvent( owner, EVENT_RESOURCE_FAILURE );
	if ( rRid.is_valid() ) {
		pRd->free_rid( rRid );
	}
	return false;
}

// Identity only: no destructor releases device resources. Typed wrappers retain
// ownership policy, accounting and explicit render-thread retirement.
template< uint32_t RidCount = 1u > class Terrain3DRdResourceState
{
	static_assert( RidCount > 0u, "Resource state needs at least one RID" );
	RID					 m_rids[RidCount];
	uint32_t			 m_nGeneration = 0u;
	t3dRdResourceOwner_t m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;

public:
	// Context retirement invalidates the supplied RID.
	RID &rid(
		uint32_t index = 0u ) {
		return m_rids[index];
	}
	const RID &rid(
		uint32_t index = 0u ) const {
		return m_rids[index];
	}
	uint32_t generation() const {
		return m_nGeneration;
	}
	t3dRdResourceOwner_t owner() const {
		return m_owner;
	}
	bool isValid() const {
		for ( uint32_t i = 0u; i < RidCount; ++i ) {
			if ( m_rids[i].is_valid() ) {
				return true;
			}
		}
		return false;
	}

	void publish(
		const RID			&rRid,
		t3dRdResourceOwner_t owner,
		uint32_t			 index = 0u ) {
		m_rids[index] = rRid;
		m_owner = owner;
		m_nGeneration = t3dNextGeneration( m_nGeneration );
	}

	void abandon() {
		for ( uint32_t i = 0u; i < RidCount; ++i ) {
			m_rids[i] = RID();
		}
		m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
		m_nGeneration = t3dNextGeneration( m_nGeneration );
	}

	// Generations stay with objects rather than following swapped RIDs. Preserve
	// the sequential self-swap increments used by pipeline and uniform-set APIs.
	void swap(
		Terrain3DRdResourceState &rOther,
		bool					  bAdvanceSelfTwice = false ) {
		using eastl::swap;
		const uint32_t next = t3dNextGeneration( m_nGeneration );
		const uint32_t otherNext =
			t3dNextGeneration( bAdvanceSelfTwice && this == &rOther ? next : rOther.m_nGeneration );
		for ( uint32_t i = 0u; i < RidCount; ++i ) {
			swap( m_rids[i], rOther.m_rids[i] );
		}
		swap( m_owner, rOther.m_owner );
		m_nGeneration = next;
		rOther.m_nGeneration = otherNext;
	}
};

#endif // TERRAIN3D_RD_RESOURCE_STATE_H
