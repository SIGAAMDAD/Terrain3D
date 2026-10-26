#ifndef TERRAIN3D_HIERARCHICAL_TILE_STORE_H
#define TERRAIN3D_HIERARCHICAL_TILE_STORE_H

#include "core/utility/terrain_3d_sync.h"
#include "data/persistence/terrain_3d_raster_hierarchy_bundle.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <stdint.h>

// The tile store owns only v2 *visual* hierarchy payloads, never collision
// heights or VT physical pages. No steady-state traversal performs file I/O.
#define T3D_HIERARCHY_TILE_MAX_SLOTS 32u
#define T3D_HIERARCHY_TILE_ALL_CHANNELS 7u

enum t3dHierarchyTileChannel_t : uint8_t
{
    T3D_HIERARCHY_TILE_HEIGHT = 1u,
    T3D_HIERARCHY_TILE_CONTROL = 2u,
    T3D_HIERARCHY_TILE_COLOR = 4u
};

struct t3dHierarchyPreparedTile_t
{
    // Caller-owned reusable arrays; only m_nChannels identifies valid data.
    godot::PackedByteArray m_height, m_control, m_color;
    uint16_t m_nNodeId = UINT16_MAX;
    uint8_t m_nChannels = 0u;
    uint32_t m_nSourceRevision = 0u;
    uint32_t m_nContentGeneration = 0u;
    uint64_t m_nStoreEpoch = 0u;
};

struct t3dHierarchyTileStoreStats_t
{
    uint64_t m_nFileReads = 0u;
    uint64_t m_nVerifiedBytes = 0u;
    uint64_t m_nCoalescedRequests = 0u;
    uint64_t m_nCancelled = 0u;
    uint64_t m_nRejected = 0u;
    uint64_t m_nFailed = 0u;
    uint64_t m_nPrepared = 0u;
    uint32_t m_nQueued = 0u;
    uint32_t m_nLoading = 0u;
    uint32_t m_nReady = 0u;
    uint32_t m_nSlotCount = 0u;
    uint32_t m_nResidentPoolBytes = 0u;
};

class Terrain3DHierarchicalTileStore
{
public:
    Terrain3DHierarchicalTileStore( void ) = default;
    ~Terrain3DHierarchicalTileStore( void );
    Terrain3DHierarchicalTileStore( const Terrain3DHierarchicalTileStore & ) = delete;
    Terrain3DHierarchicalTileStore &operator=( const Terrain3DHierarchicalTileStore & ) = delete;

    // Bind on the owner thread only, at a cold streaming boundary. Opens only
    // the directory, then copies node descriptors so workers hold no index ptr.
    bool open( const godot::String &rDirectory, uint32_t nMaxSlots = 16u );
    void requestNode( uint16_t nId, uint8_t nChannels, uint8_t nPriority, uint32_t nRevision );
    void service( void );
    bool isReady( uint16_t nId, uint8_t nChannels, uint32_t nRevision ) const;
    bool takePrepared( uint16_t nId, uint8_t nChannels, uint32_t nRevision,
                       t3dHierarchyPreparedTile_t &rTile );
    void cancel( uint16_t nId );
    // reset() acknowledges in-flight reads by joining the worker before
    // resetting slots. No thread or buffer escapes the store's lifetime.
    void reset( void );
    void quiesce( void ) { reset(); }
    t3dHierarchyTileStoreStats_t getStats( void ) const;
    // Cold snapshot for the F03 scheduler. Keep the only authoritative v2
    // directory in this store, and copy it under the worker/owner lock.
    bool snapshotDirectoryCold( t3dRasterHierarchyV2Node_t *pNodes,
                                uint8_t *pPresent, uint32_t nCapacity,
                                uint32_t &rGeneration ) const;
    uint64_t getEpoch( void ) const { return m_nEpoch; } // owner thread only

private:
    enum SlotState : uint8_t { FREE = 0u, QUEUED, LOADING, READY, FAILED };
    struct Slot
    {
        uint8_t m_bytes[T3D_RASTER_HIERARCHY_V2_TILE_BYTES]{};
        uint16_t m_nId = UINT16_MAX;
        uint8_t m_nChannels = 0u;
        uint8_t m_nPriority = 0u;
        uint32_t m_nRevision = 0u;
        uint32_t m_nGeneration = 0u;
        uint64_t m_nEpoch = 0u;
        uint64_t m_nRequestedUsec = 0u;
        SlotState m_state = FREE;
        bool m_bCancelled = false;
    };

    void workerMain( void );
    int32_t selectQueuedLocked( void ) const;
    void clearSlotLocked( Slot &rSlot );

    // Maximum 32 x 10,504-byte tiles. Pool allocated once at cold open,
    // using the requested slot count; no hot-path allocations on requests.
    Slot *m_slots = nullptr;
    t3dRasterHierarchyV2Node_t m_nodes[1365]{};
    uint8_t m_nodePresent[1365]{};
    godot::String m_path;
    uint64_t m_nEpoch = 1u;
    uint32_t m_nGeneration = 0u;
    uint32_t m_nSlots = 0u;
    bool m_bStop = false;
    bool m_bRunning = false;
    mutable Terrain3DMutex m_mutex;
    Terrain3DCondition m_wake;
    Terrain3DThread m_worker;
    t3dHierarchyTileStoreStats_t m_stats{};
};

#endif
