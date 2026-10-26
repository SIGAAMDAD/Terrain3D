#ifndef TERRAIN3D_HIERARCHY_SCHEDULER_H
#define TERRAIN3D_HIERARCHY_SCHEDULER_H

#include "streaming/hierarchy/terrain_3d_hierarchy_demand.h"
#include "streaming/hierarchy/terrain_3d_hierarchy_metadata.h"
#include <stdint.h>

// F03: cold-bound, owner-thread only hierarchy policy. No RID, FileAccess,
// Godot resource or mutable cache pointer crosses the demand boundary.
#define T3D_HIERARCHY_MAX_DEMANDS 4096u
#define T3D_HIERARCHY_MAX_COVER 1024u

struct t3dHierarchyView_t
{
    float m_fCameraX = 0.0f, m_fCameraY = 0.0f, m_fCameraZ = 0.0f;
    float m_fForwardX = 1.0f, m_fForwardZ = 0.0f;
    float m_fVelocityX = 0.0f, m_fVelocityZ = 0.0f;
    float m_fVerticalFovDegrees = 70.0f;
    float m_fViewportHeight = 1080.0f;
    float m_fVisibilityRadiusM = 4096.0f;
    float m_fLookaheadSeconds = 0.4f;
};

struct t3dHierarchyScheduleLimits_t
{
    uint16_t m_nVisualHeight = 32u;
    uint16_t m_nControl = 32u;
    uint16_t m_nColor = 32u;
    uint16_t m_nVtHints = 16u;
    uint16_t m_nDecoration = 16u;
    uint16_t m_nExactHeightReserve = 16u; // external exact pages always run first
    uint16_t m_nMaxDispatch = 32u;
    float m_fGeometricErrorPixels = 2.0f;
    float m_fRasterTexelPixels = 1.5f;
    float m_fRefineEnter = 1.15f;
    float m_fRefineExit = 0.85f;
};

struct t3dHierarchyTileRequest_t
{
    uint16_t m_nNodeId = UINT16_MAX;
    uint8_t m_nChannels = 0u;
    uint8_t m_nPriority = 0u;
    uint32_t m_nSourceRevision = 0u;
    uint32_t m_nFlags = 0u;
};

struct t3dHierarchyCoverNode_t
{
    uint16_t m_nDesiredNode = UINT16_MAX;
    uint16_t m_nAvailableAncestor = UINT16_MAX; // UINT16_MAX => legacy clipmap
    uint32_t m_nRevision = 0u;
    uint8_t m_nRequiredChannels = 0u;
    uint8_t m_nFlags = 0u; // T3D_HIERARCHY_DEMAND_PARENT_HOLD
};

struct t3dHierarchyScheduleStats_t
{
    uint32_t m_nVisited = 0u;
    uint32_t m_nCulled = 0u;
    uint32_t m_nDesiredNodes = 0u;
    uint32_t m_nRefined = 0u;
    uint32_t m_nParentHeld = 0u;
    uint32_t m_nLegacyFallback = 0u;
    uint32_t m_nDemandGenerated = 0u;
    uint32_t m_nDemandDisplaced = 0u;
    uint32_t m_nExactReserved = 0u;
    uint32_t m_nDispatched = 0u;
};

class Terrain3DHierarchyScheduler
{
public:
    void reset( void );
    // Called only on cold hierarchy publication. The directory is copied;
    // no borrowed pointer into the persisted index survives this call.
    bool bindCold( const t3dRasterHierarchyV2Node_t *pNodes,
                   const uint8_t *pPresent, uint32_t nCount, uint32_t nGeneration );
    void markGpuCommitted( uint16_t nNode, uint8_t nChannels,
                           uint32_t nRevision, uint32_t nGeneration );
    void invalidateGpuNode( uint16_t nNode );
    void schedule( const t3dHierarchyView_t &rView,
                   const t3dHierarchyScheduleLimits_t &rLimits,
                   uint32_t nExactRequiredCount );
    const t3dHierarchyDemand_t *demands( void ) const { return m_demands; }
    uint32_t demandCount( void ) const { return m_nDemands; }
    const t3dHierarchyCoverNode_t *cover( void ) const { return m_cover; }
    uint32_t coverCount( void ) const { return m_nCover; }
    const t3dHierarchyScheduleStats_t &stats( void ) const { return m_stats; }
    uint32_t generation( void ) const { return m_nGeneration; }

    // Cross-channel acceptance applies budgets BEFORE F02 requests; a request
    // that cannot be admitted is omitted, never silently counted as submitted.
    uint32_t selectTileDispatch( t3dHierarchyTileRequest_t *pOutput,
                                 uint32_t nCapacity,
                                 const t3dHierarchyScheduleLimits_t &rLimits );

private:
    void selectNode( uint8_t nLevel, uint8_t nX, uint8_t nZ,
                     const t3dHierarchyView_t &rView,
                     const t3dHierarchyScheduleLimits_t &rLimits,
                     float fProjectedScale, uint32_t &rTraversalBudget );
    void appendDemand( uint16_t nNode, uint8_t nClass, float fPriority, uint32_t nFlags );
    void appendAncestor( uint16_t nNode, float fPriority );
    bool canSee( uint8_t nLevel, uint8_t nX, uint8_t nZ,
                 const t3dHierarchyView_t &rView, float &rDistance ) const;
    uint16_t closestCommitted( uint16_t nNode, uint8_t nRequired ) const;

    t3dRasterHierarchyV2Node_t m_nodes[T3D_HIERARCHY_MAX_NODES]{};
    uint8_t m_present[T3D_HIERARCHY_MAX_NODES]{};
    uint8_t m_gpuChannels[T3D_HIERARCHY_MAX_NODES]{};
    uint8_t m_previousRefined[T3D_HIERARCHY_MAX_NODES]{};
    uint8_t m_currentRefined[T3D_HIERARCHY_MAX_NODES]{};
    t3dHierarchyDemand_t m_demands[T3D_HIERARCHY_MAX_DEMANDS]{};
    t3dHierarchyCoverNode_t m_cover[T3D_HIERARCHY_MAX_COVER]{};
    uint32_t m_nGeneration = 0u;
    uint32_t m_nDemands = 0u;
    uint32_t m_nCover = 0u;
    t3dHierarchyScheduleStats_t m_stats{};
};

#endif
