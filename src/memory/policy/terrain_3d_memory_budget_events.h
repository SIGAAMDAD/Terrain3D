// Event-owned memory budget invalidation. No sampling or frame counters.
#ifndef TERRAIN3D_MEMORY_BUDGET_EVENTS_H
#define TERRAIN3D_MEMORY_BUDGET_EVENTS_H

#include <stdint.h>

enum t3dMemoryBudgetDirtyReason_t : uint32_t
{
    T3D_BUDGET_DIRTY_STARTUP = 1u << 0u,
    T3D_BUDGET_DIRTY_CONFIGURATION = 1u << 1u,
    T3D_BUDGET_DIRTY_PROFILE = 1u << 2u,
    T3D_BUDGET_DIRTY_RESOURCE_LAYOUT = 1u << 3u,
    T3D_BUDGET_DIRTY_CAPACITY_CHANGE = 1u << 4u,
    T3D_BUDGET_DIRTY_EXPLICIT = 1u << 5u
};

struct t3dMemoryBudgetEvents_t
{
    bool m_bPending = true;
    bool m_bApplying = false;
    uint32_t m_nPendingReasons = T3D_BUDGET_DIRTY_STARTUP;
    uint32_t m_nLastAppliedReasons = 0u;
    uint64_t m_nRequestRevision = 1u;
    uint64_t m_nProcessedRevision = 0u;
    uint64_t m_nRequestCount = 0u;
    uint64_t m_nCoalescedCount = 0u;
    uint64_t m_nComputeCount = 0u;
    uint64_t m_nApplyCount = 0u;

    void request( uint32_t nReasons ) {
        if ( m_bPending ) {
            ++m_nCoalescedCount;
        }
        m_bPending = true;
        m_nPendingReasons |= nReasons;
        ++m_nRequestRevision;
        ++m_nRequestCount;
    }

    bool begin( bool bForce, uint64_t &rRevision, uint32_t &rReasons ) {
        if ( m_bApplying || ( !bForce && !m_bPending ) ) {
            return false;
        }
        m_bApplying = true;
        rRevision = m_nRequestRevision;
        rReasons = m_nPendingReasons | ( bForce ? T3D_BUDGET_DIRTY_EXPLICIT : 0u );
        m_nPendingReasons = 0u;
        m_bPending = false;
        ++m_nComputeCount;
        return true;
    }

    void finish( uint64_t nRevision, uint32_t nReasons ) {
        m_nProcessedRevision = nRevision;
        m_nLastAppliedReasons = nReasons;
        ++m_nApplyCount;
        m_bApplying = false;
    }
};

#endif // TERRAIN3D_MEMORY_BUDGET_EVENTS_H
