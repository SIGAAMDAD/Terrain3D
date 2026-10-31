#include "streaming/hierarchy/terrain_3d_hierarchy_scheduler.h"
#include <algorithm>
#include <math.h>
#include <string.h>

namespace {
static uint16_t t3dHierarchyId( const uint8_t level, const uint8_t x, const uint8_t z ) {
    if ( level > 5u || x >= ( 1u << level ) || z >= ( 1u << level ) ) return UINT16_MAX;
    uint32_t start = 0u;
    for ( uint8_t l = 0u; l < level; ++l ) start += 1u << ( 2u * l );
    return ( uint16_t ) ( start + ( uint32_t ) z * ( 1u << level ) + x );
}
static uint16_t t3dHierarchyParent( const uint16_t id ) {
    uint32_t start = 0u;
    for ( uint8_t l = 0u; l <= 5u; ++l ) {
        const uint32_t w = 1u << l;
        if ( id < start + w * w ) {
            if ( l == 0u ) return UINT16_MAX;
            const uint32_t ordinal = id - start;
            return t3dHierarchyId( l - 1u, ( uint8_t ) ( ordinal % w / 2u ),
                                   ( uint8_t ) ( ordinal / w / 2u ) );
        }
        start += w * w;
    }
    return UINT16_MAX;
}
static float t3dHierarchyClamp( const float v, const float lo, const float hi ) {
    return v < lo ? lo : ( v > hi ? hi : v );
}
}

void Terrain3DHierarchyScheduler::reset( void ) {
    memset( m_present, 0, sizeof( m_present ) );
    memset( m_gpuChannels, 0, sizeof( m_gpuChannels ) );
    memset( m_previousRefined, 0, sizeof( m_previousRefined ) );
    memset( m_currentRefined, 0, sizeof( m_currentRefined ) );
    m_nGeneration = m_nDemands = m_nCover = 0u;
    m_stats = t3dHierarchyScheduleStats_t{};
}

bool Terrain3DHierarchyScheduler::bindCold( const t3dRasterHierarchyStreamNode_t *const nodes,
                                           const uint8_t *const present,
                                           const uint32_t count, const uint32_t generation ) {
    reset();
    if ( !nodes || !present || count != T3D_HIERARCHY_MAX_NODES || generation == 0u ) return false;
    uint32_t offset = 0u;
    for ( uint8_t level = 0u; level <= 5u; ++level ) {
        const uint32_t width = 1u << level;
        for ( uint32_t index = 0u; index < width * width; ++index ) {
            const uint32_t i = offset + index;
            if ( !present[i] ) continue;
            const t3dRasterHierarchyStreamNode_t &n = nodes[i];
            if ( n.m_nRevision == 0u || n.m_nGeneration != generation ||
                 n.m_nLevel != level || ( n.m_nCoverageMask & ~7u ) != 0u ||
                 ( level == 5u && n.m_nChildMask != 0u ) ||
                 !isfinite( n.m_fMinHeight ) || !isfinite( n.m_fMaxHeight ) ||
                 !isfinite( n.m_fMaxHeightErrorM ) || n.m_fMinHeight > n.m_fMaxHeight ||
                 n.m_fMaxHeightErrorM < 0.0f || n.m_nCoverageMask == 0u ) {
                reset();
                return false;
            }
            m_nodes[i] = n;
            m_present[i] = 1u;
        }
        offset += width * width;
    }
    if ( !m_present[0] ) return false;
    // Reject an incomplete quadtree directory before any GPU publication.
    // A child may be absent in sparse worlds, but it must agree with the
    // parent's explicit child mask, including the empty quadrants.
    for ( uint8_t level = 0u; level < 5u; ++level ) {
        const uint32_t width = 1u << level;
        for ( uint32_t z = 0u; z < width; ++z ) for ( uint32_t x = 0u; x < width; ++x ) {
            const uint16_t parent = t3dHierarchyId( level, ( uint8_t ) x, ( uint8_t ) z );
            uint8_t mask = 0u;
            for ( uint8_t dz = 0u; dz < 2u; ++dz ) for ( uint8_t dx = 0u; dx < 2u; ++dx ) {
                const uint16_t child = t3dHierarchyId( level + 1u,
                    ( uint8_t ) ( x * 2u + dx ), ( uint8_t ) ( z * 2u + dz ) );
                if ( m_present[child] ) mask |= ( uint8_t ) ( 1u << ( dz * 2u + dx ) );
            }
            if ( ( m_present[parent] && m_nodes[parent].m_nChildMask != mask ) ||
                 ( !m_present[parent] && mask != 0u ) ) {
                reset();
                return false;
            }
        }
    }
    m_nGeneration = generation;
    return true;
}

void Terrain3DHierarchyScheduler::markGpuCommitted( const uint16_t id, const uint8_t channels,
                                                     const uint32_t revision, const uint32_t generation ) {
    if ( generation == m_nGeneration && id < T3D_HIERARCHY_MAX_NODES && m_present[id] &&
         revision == m_nodes[id].m_nRevision )
        m_gpuChannels[id] |= channels & m_nodes[id].m_nCoverageMask & 7u;
}
void Terrain3DHierarchyScheduler::invalidateGpuNode( const uint16_t id ) {
    if ( id < T3D_HIERARCHY_MAX_NODES ) m_gpuChannels[id] = 0u;
}

bool Terrain3DHierarchyScheduler::canSee( const uint8_t level, const uint8_t x, const uint8_t z,
                                         const t3dHierarchyView_t &view, float &distance ) const {
    const uint16_t id = t3dHierarchyId( level, x, z );
    if ( id == UINT16_MAX || !m_present[id] ) return false;
    const float width = 4096.0f / ( float ) ( 1u << level );
    const float minX = -2048.0f + x * width, minZ = -2048.0f + z * width;
    const float maxX = minX + width, maxZ = minZ + width;
    const auto &node = m_nodes[id];
    const float dx = view.m_fCameraX - t3dHierarchyClamp( view.m_fCameraX, minX, maxX );
    const float dy = view.m_fCameraY - t3dHierarchyClamp( view.m_fCameraY, node.m_fMinHeight, node.m_fMaxHeight );
    const float dz = view.m_fCameraZ - t3dHierarchyClamp( view.m_fCameraZ, minZ, maxZ );
    distance = sqrtf( dx * dx + dy * dy + dz * dz );
    if ( distance > view.m_fVisibilityRadiusM ) return false;
    const float centerX = ( minX + maxX ) * 0.5f - view.m_fCameraX;
    const float centerZ = ( minZ + maxZ ) * 0.5f - view.m_fCameraZ;
    const float boundsRadius = 0.707107f * width;
    if ( centerX * centerX + centerZ * centerZ <= boundsRadius * boundsRadius ) return true;
    // Conservative horizontal frustum with a full node bounding-circle margin.
    const float len = sqrtf( centerX * centerX + centerZ * centerZ );
    const float fwdLen = sqrtf( view.m_fForwardX * view.m_fForwardX + view.m_fForwardZ * view.m_fForwardZ );
    if ( len <= 0.001f || fwdLen <= 0.001f ) return true;
    const float dot = ( centerX * view.m_fForwardX + centerZ * view.m_fForwardZ ) / ( len * fwdLen );
    const float fovRadians = view.m_fVerticalFovDegrees * 0.017453292519943295f;
    // Aspect is unknown at this boundary; treat vertical FOV as conservative
    // horizontal FOV and add a 20-degree prediction/frustum guard.
    const float cone = cosf( std::min( 1.56f, 0.5f * fovRadians + 0.35f ) );
    return dot >= cone - boundsRadius / len;
}

uint16_t Terrain3DHierarchyScheduler::closestCommitted( uint16_t id, const uint8_t channels ) const {
    while ( id != UINT16_MAX ) {
        if ( m_present[id] && ( m_gpuChannels[id] & channels ) == channels ) return id;
        id = t3dHierarchyParent( id );
    }
    return UINT16_MAX;
}

void Terrain3DHierarchyScheduler::appendDemand( const uint16_t id, const uint8_t klass,
                                                 const float priority, const uint32_t flags ) {
    if ( id == UINT16_MAX || m_nDemands >= T3D_HIERARCHY_MAX_DEMANDS ||
         !m_present[id] || !isfinite( priority ) ) return;
    const auto &n = m_nodes[id];
    if ( klass <= T3D_HIERARCHY_COLOR && ( n.m_nCoverageMask & ( 1u << klass ) ) == 0u ) return;
    t3dHierarchyDemand_t &d = m_demands[m_nDemands++];
    d.m_nNodeId = id;
    d.m_nPayloadClass = klass;
    d.m_nRequestedLod = n.m_nLevel;
    d.m_nSourceRevision = n.m_nRevision;
    d.m_fPriorityRatio = t3dHierarchyClamp( priority, 0.01f, 4096.0f );
    d.m_nFlags = flags;
}

void Terrain3DHierarchyScheduler::appendAncestor( uint16_t id, const float priority ) {
    // Parents are requested in coarse-to-fine order with a high, finite weight.
    uint16_t ancestors[6]{};
    uint32_t count = 0u;
    for ( id = t3dHierarchyParent( id ); id != UINT16_MAX; id = t3dHierarchyParent( id ) )
        ancestors[count++] = id;
    while ( count ) {
        const uint16_t parent = ancestors[--count];
        if ( ( m_gpuChannels[parent] & 7u ) == 7u ) continue;
        for ( uint8_t cls = 0u; cls < 3u; ++cls )
            if ( !( m_gpuChannels[parent] & ( 1u << cls ) ) )
                appendDemand( parent, cls, priority + ( 5u - m_nodes[parent].m_nLevel ) * 2.0f,
                              T3D_HIERARCHY_DEMAND_PARENT_HOLD | T3D_HIERARCHY_DEMAND_REQUIRED );
    }
}

void Terrain3DHierarchyScheduler::selectNode( const uint8_t level, const uint8_t x, const uint8_t z,
                                              const t3dHierarchyView_t &view,
                                              const t3dHierarchyScheduleLimits_t &limits,
                                              const float projection, uint32_t &traversalBudget ) {
    if ( traversalBudget == 0u ) return;
    --traversalBudget;
    const uint16_t id = t3dHierarchyId( level, x, z );
    if ( id == UINT16_MAX || !m_present[id] ) return;
    ++m_stats.m_nVisited;
    float distance = 0.0f;
    if ( !canSee( level, x, z, view, distance ) ) { ++m_stats.m_nCulled; return; }
    const auto &node = m_nodes[id];
    const float width = 4096.0f / ( float ) ( 1u << level );
    const float pixelsPerMeter = projection / std::max( 1.0f, distance );
    const float errorRatio = node.m_fMaxHeightErrorM * pixelsPerMeter /
                             std::max( 0.01f, limits.m_fGeometricErrorPixels );
    const float texelRatio = width / 32.0f * pixelsPerMeter /
                             std::max( 0.01f, limits.m_fRasterTexelPixels );
    float priority = std::max( errorRatio, texelRatio );
    // Look ahead for streaming demand without altering the actual camera's
    // visibility/frustum; a teleport invalidates prior temporal refinement.
    const float aheadX = view.m_fCameraX + view.m_fVelocityX * view.m_fLookaheadSeconds;
    const float aheadZ = view.m_fCameraZ + view.m_fVelocityZ * view.m_fLookaheadSeconds;
    const float centerX = -2048.0f + ( x + 0.5f ) * width;
    const float centerZ = -2048.0f + ( z + 0.5f ) * width;
    const float predDx = std::max( 0.0f, fabsf( centerX - aheadX ) - 0.5f * width );
    const float predDz = std::max( 0.0f, fabsf( centerZ - aheadZ ) - 0.5f * width );
    const float predDistance = sqrtf( predDx * predDx + predDz * predDz );
    if ( predDistance < distance ) priority = std::max( priority,
        std::max( node.m_fMaxHeightErrorM / std::max( 0.01f, limits.m_fGeometricErrorPixels ),
                  width / ( 32.0f * std::max( 0.01f, limits.m_fRasterTexelPixels ) ) ) *
                  projection / std::max( 1.0f, predDistance ) );
    const float threshold = m_previousRefined[id] ? limits.m_fRefineExit : limits.m_fRefineEnter;
    const bool refine = level < 5u && node.m_nChildMask != 0u && priority > threshold &&
                        traversalBudget >= 4u && m_nCover + 4u <= T3D_HIERARCHY_MAX_COVER;
    if ( refine ) {
        ++m_stats.m_nRefined;
        m_currentRefined[id] = 1u;
        for ( uint8_t dz = 0u; dz < 2u; ++dz )
            for ( uint8_t dx = 0u; dx < 2u; ++dx )
                if ( node.m_nChildMask & ( 1u << ( dz * 2u + dx ) ) )
                    selectNode( level + 1u, ( uint8_t ) ( x * 2u + dx ),
                                ( uint8_t ) ( z * 2u + dz ), view, limits, projection, traversalBudget );
        return;
    }
    if ( m_nCover >= T3D_HIERARCHY_MAX_COVER ) return;
    ++m_stats.m_nDesiredNodes;
    t3dHierarchyCoverNode_t &cover = m_cover[m_nCover++];
    cover.m_nDesiredNode = id;
    cover.m_nRevision = node.m_nRevision;
    cover.m_nRequiredChannels = node.m_nCoverageMask & 7u;
    cover.m_nAvailableAncestor = closestCommitted( id, cover.m_nRequiredChannels );
    cover.m_nFlags = cover.m_nAvailableAncestor != id ? T3D_HIERARCHY_DEMAND_PARENT_HOLD : 0u;
    if ( cover.m_nAvailableAncestor == UINT16_MAX ) ++m_stats.m_nLegacyFallback;
    if ( cover.m_nAvailableAncestor != id ) {
        ++m_stats.m_nParentHeld;
        appendAncestor( id, std::max( 1.0f, priority ) );
    }
    for ( uint8_t cls = 0u; cls < 3u; ++cls ) {
        const uint8_t channel = 1u << cls;
        if ( !( node.m_nCoverageMask & channel ) || ( m_gpuChannels[id] & channel ) ) continue;
        appendDemand( id, cls, priority, cover.m_nAvailableAncestor != id ?
                      T3D_HIERARCHY_DEMAND_REQUIRED : 0u );
    }
    // VT/decor demands are hints only; the original VT and instance caches
    // remain sole physical owners until the explicit F04 consumer migration.
    appendDemand( id, T3D_HIERARCHY_VT_HINT, priority * 0.65f, T3D_HIERARCHY_DEMAND_PREFETCH );
    appendDemand( id, T3D_HIERARCHY_DECORATION, priority * 0.35f, T3D_HIERARCHY_DEMAND_PREFETCH );
}

void Terrain3DHierarchyScheduler::schedule( const t3dHierarchyView_t &view,
                                            const t3dHierarchyScheduleLimits_t &limits,
                                            const uint32_t exactRequired ) {
    m_nDemands = m_nCover = 0u;
    m_stats = t3dHierarchyScheduleStats_t{};
    m_stats.m_nExactReserved = std::min( exactRequired, ( uint32_t ) limits.m_nExactHeightReserve );
    if ( !m_nGeneration || !m_present[0] || !isfinite( view.m_fCameraX ) ||
         !isfinite( view.m_fCameraY ) || !isfinite( view.m_fCameraZ ) ||
         !isfinite( view.m_fVerticalFovDegrees ) || !isfinite( view.m_fViewportHeight ) ||
         !isfinite( view.m_fVisibilityRadiusM ) || !isfinite( view.m_fVelocityX ) ||
         !isfinite( view.m_fVelocityZ ) || !isfinite( view.m_fLookaheadSeconds ) ||
         view.m_fVerticalFovDegrees <= 1.0f || view.m_fVerticalFovDegrees >= 175.0f ||
         view.m_fViewportHeight < 1.0f || view.m_fVisibilityRadiusM <= 0.0f ||
         view.m_fLookaheadSeconds < 0.0f ) return;
    memset( m_currentRefined, 0, sizeof( m_currentRefined ) );
    const float projection = view.m_fViewportHeight /
        ( 2.0f * tanf( view.m_fVerticalFovDegrees * 0.008726646259971648f ) );
    uint32_t budget = T3D_HIERARCHY_MAX_NODES;
    selectNode( 0u, 0u, 0u, view, limits, projection, budget );
    memcpy( m_previousRefined, m_currentRefined, sizeof( m_previousRefined ) );
    // Deduplicate co-located parent+child demand: one physical tile request,
    // but retain each logical class so downstream admissions remain distinct.
    std::sort( m_demands, m_demands + m_nDemands, []( const t3dHierarchyDemand_t &a,
                                                       const t3dHierarchyDemand_t &b ) {
        if ( a.m_nNodeId != b.m_nNodeId ) return a.m_nNodeId < b.m_nNodeId;
        if ( a.m_nPayloadClass != b.m_nPayloadClass ) return a.m_nPayloadClass < b.m_nPayloadClass;
        return t3dHierarchyDemandHigherPriority( a, b );
    } );
    uint32_t unique = 0u;
    for ( uint32_t i = 0u; i < m_nDemands; ++i ) {
        const auto &d = m_demands[i];
        if ( unique && m_demands[unique - 1u].m_nNodeId == d.m_nNodeId &&
             m_demands[unique - 1u].m_nPayloadClass == d.m_nPayloadClass ) {
            m_demands[unique - 1u].m_nFlags |= d.m_nFlags;
            m_demands[unique - 1u].m_fPriorityRatio = std::max(
                d.m_fPriorityRatio, m_demands[unique - 1u].m_fPriorityRatio );
        } else m_demands[unique++] = d;
    }
    m_nDemands = unique;
    std::sort( m_demands, m_demands + m_nDemands, t3dHierarchyDemandHigherPriority );
    m_stats.m_nDemandGenerated = m_nDemands;
}

uint32_t Terrain3DHierarchyScheduler::selectTileDispatch(
    t3dHierarchyTileRequest_t *const output, const uint32_t capacity,
    const t3dHierarchyScheduleLimits_t &limits ) {
    if ( !output || !capacity ) return 0u;
    const uint32_t maxima[3] = { limits.m_nVisualHeight, limits.m_nControl, limits.m_nColor };
    uint32_t perClass[3]{};
    uint16_t byNode[T3D_HIERARCHY_MAX_NODES];
    for ( uint32_t i = 0u; i < T3D_HIERARCHY_MAX_NODES; ++i ) byNode[i] = UINT16_MAX;
    const uint32_t overall = std::min( capacity, ( uint32_t ) limits.m_nMaxDispatch );
    // Required exact height is owned by the existing region store. This
    // scheduler can use only the remainder of the shared tile-service budget.
    const uint32_t visualBudget = overall - std::min( overall, m_stats.m_nExactReserved );
    uint32_t count = 0u;
    m_stats.m_nDemandDisplaced = 0u;
    for ( uint32_t i = 0u; i < m_nDemands; ++i ) {
        const t3dHierarchyDemand_t &d = m_demands[i];
        const uint32_t cls = d.m_nPayloadClass;
        if ( cls > T3D_HIERARCHY_COLOR ) continue;
        if ( perClass[cls] >= maxima[cls] ) { ++m_stats.m_nDemandDisplaced; continue; }
        const uint16_t existing = byNode[d.m_nNodeId];
        if ( existing != UINT16_MAX ) {
            output[existing].m_nChannels |= ( uint8_t ) ( 1u << cls );
            output[existing].m_nFlags |= d.m_nFlags;
            output[existing].m_nPriority = std::max( output[existing].m_nPriority,
                ( uint8_t ) std::min( 255.0f, d.m_fPriorityRatio * 16.0f ) );
            ++perClass[cls];
        } else {
            if ( count >= visualBudget ) { ++m_stats.m_nDemandDisplaced; continue; }
            t3dHierarchyTileRequest_t &out = output[count];
            out.m_nNodeId = d.m_nNodeId;
            out.m_nChannels = ( uint8_t ) ( 1u << cls );
            out.m_nSourceRevision = d.m_nSourceRevision;
            out.m_nFlags = d.m_nFlags;
            out.m_nPriority = ( uint8_t ) std::min( 255.0f, d.m_fPriorityRatio * 16.0f );
            byNode[d.m_nNodeId] = ( uint16_t ) count++;
            ++perClass[cls];
        }
    }
    m_stats.m_nDispatched = count;
    return count;
}
