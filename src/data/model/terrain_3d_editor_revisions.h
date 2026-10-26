// Generic authoring invalidation counters, absent from shipping region objects.
#ifndef TERRAIN3D_EDITOR_REVISIONS_H
#define TERRAIN3D_EDITOR_REVISIONS_H
#if defined(T3D_EDITOR_NATIVE)
#include "core/utility/terrain_3d_sync.h"

struct Terrain3DEditorRevisions {
    uint64_t source = 1u;
    uint64_t instances = 1u;
    inline static Terrain3DAtomic<uint64_t> stamp{1u};
    void touchSource() {
        if (++source == 0u) { source = 1u; }
        if (stamp.fetch_add(1u, T3D_MEMORY_ORDER_ACQ_REL) + 1u == 0u) {
            stamp.store(1u, T3D_MEMORY_ORDER_RELEASE);
        }
    }
    void touchInstances() { if (++instances == 0u) { instances = 1u; } }
    static uint64_t sourceStamp() { return stamp.load(T3D_MEMORY_ORDER_ACQUIRE); }
};
#endif
#endif
