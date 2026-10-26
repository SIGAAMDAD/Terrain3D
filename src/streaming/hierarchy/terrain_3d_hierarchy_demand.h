#ifndef TERRAIN3D_HIERARCHY_DEMAND_H
#define TERRAIN3D_HIERARCHY_DEMAND_H

#include <stdint.h>

// F01: immutable owner-thread demand ABI. A node ID identifies one of the
// 1,365 slots in the six-level 32x32-region hierarchy; it is not a GPU RID.
#define T3D_HIERARCHY_MAX_NODES 1365u

typedef struct t3dHierarchyNodeKey_s
{
	uint16_t m_nNodeId;
	uint16_t m_nGenerationTag;
} t3dHierarchyNodeKey_t;

enum t3dHierarchyPayloadClass_t : uint8_t
{
	T3D_HIERARCHY_HEIGHT_VISUAL = 0,
	T3D_HIERARCHY_CONTROL,
	T3D_HIERARCHY_COLOR,
	T3D_HIERARCHY_VT_HINT,
	T3D_HIERARCHY_DECORATION,
	T3D_HIERARCHY_PAYLOAD_CLASS_COUNT
};

enum t3dHierarchyDemandFlag_t : uint32_t
{
	T3D_HIERARCHY_DEMAND_REQUIRED = 1u << 0u,
	T3D_HIERARCHY_DEMAND_PREFETCH = 1u << 1u,
	T3D_HIERARCHY_DEMAND_STALE = 1u << 2u,
	T3D_HIERARCHY_DEMAND_PARENT_HOLD = 1u << 3u
};

typedef struct t3dHierarchyDemand_s
{
	uint16_t m_nNodeId = UINT16_MAX;
	uint8_t	 m_nPayloadClass = T3D_HIERARCHY_HEIGHT_VISUAL;
	uint8_t	 m_nRequestedLod = 0u;
	uint32_t m_nSourceRevision = 0u;
	float	 m_fPriorityRatio = 0.0f; // normalized; 1.0 = target threshold
	uint32_t m_nFlags = 0u;
} t3dHierarchyDemand_t;

static_assert( sizeof( t3dHierarchyDemand_t ) == 16u, "Hierarchy demand packing changed" );

// Stable deterministic tie-breaking. Priority units are NOT VT/page-store units.
static inline bool t3dHierarchyDemandHigherPriority(
	const t3dHierarchyDemand_t &a, const t3dHierarchyDemand_t &b ) {
	if ( ( a.m_nFlags & T3D_HIERARCHY_DEMAND_REQUIRED ) != ( b.m_nFlags & T3D_HIERARCHY_DEMAND_REQUIRED ) ) {
		return ( a.m_nFlags & T3D_HIERARCHY_DEMAND_REQUIRED ) != 0u;
	}
	if ( a.m_fPriorityRatio != b.m_fPriorityRatio ) {
		return a.m_fPriorityRatio > b.m_fPriorityRatio;
	}
	if ( a.m_nNodeId != b.m_nNodeId ) {
		return a.m_nNodeId < b.m_nNodeId;
	}
	return a.m_nPayloadClass < b.m_nPayloadClass;
}

#endif
