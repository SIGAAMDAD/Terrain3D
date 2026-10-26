#include "streaming/hierarchy/terrain_3d_hierarchical_tile_store.h"
#include <string.h>

Terrain3DHierarchicalTileStore::~Terrain3DHierarchicalTileStore( void ) {
    reset();
}

void Terrain3DHierarchicalTileStore::clearSlotLocked( Slot &slot ) {
    slot.m_state = FREE;
    slot.m_nId = UINT16_MAX;
    slot.m_nChannels = slot.m_nPriority = 0u;
    slot.m_nRevision = slot.m_nGeneration = 0u;
    slot.m_nRequestedUsec = 0u;
    slot.m_bCancelled = false;
}

bool Terrain3DHierarchicalTileStore::open( const godot::String &directory, const uint32_t maxSlots ) {
    reset();
    if ( maxSlots == 0u || maxSlots > T3D_HIERARCHY_TILE_MAX_SLOTS ) return false;
    Terrain3DRasterHierarchyIndexV2 index;
    if ( index.open( directory ) != godot::OK || index.getGeneration() == 0u ) return false;
    for ( uint16_t id = 0u; id < 1365u; ++id ) {
        const t3dRasterHierarchyV2Node_t *node = index.getNode( id );
        m_nodePresent[id] = node ? 1u : 0u;
        if ( node ) m_nodes[id] = *node;
    }
    m_path = index.getPath();
    m_slots = new Slot[maxSlots];
    m_nSlots = maxSlots;
    m_nGeneration = index.getGeneration();
    m_bStop = false;
    m_bRunning = true;
    m_worker = Terrain3DThread( [this]() { workerMain(); } );
    return true;
}

bool Terrain3DHierarchicalTileStore::snapshotDirectoryCold(
    t3dRasterHierarchyV2Node_t *const nodes, uint8_t *const present,
    const uint32_t capacity, uint32_t &generation ) const {
    generation = 0u;
    if ( !nodes || !present || capacity < 1365u ) return false;
    Terrain3DScopedLock lock( m_mutex );
    if ( !m_bRunning || !m_nGeneration ) return false;
    memcpy( nodes, m_nodes, sizeof( m_nodes ) );
    memcpy( present, m_nodePresent, sizeof( m_nodePresent ) );
    generation = m_nGeneration;
    return true;
}

void Terrain3DHierarchicalTileStore::requestNode(
    const uint16_t id, const uint8_t channels, const uint8_t priority, const uint32_t revision ) {
    if ( id >= 1365u || ( channels & ~T3D_HIERARCHY_TILE_ALL_CHANNELS ) != 0u ||
         channels == 0u || revision == 0u ) return;
    Terrain3DScopedLock lock( m_mutex );
    if ( !m_bRunning || !m_nodePresent[id] || m_nodes[id].m_nRevision != revision ) {
        ++m_stats.m_nRejected;
        return;
    }
    // Back off corrupted tiles for one second instead of issuing a disk read
    // on every frame until a recook/revision transition replaces the source.
    const uint64_t now = t3dSteadyClockUsec();
    for ( uint32_t i = 0u; i < m_nSlots; ++i ) {
        const Slot &failed = m_slots[i];
        if ( failed.m_state == FAILED && failed.m_nId == id &&
             failed.m_nRevision == revision && failed.m_nEpoch == m_nEpoch &&
             now - failed.m_nRequestedUsec < 1000000u ) return;
    }
    // One physical read per node; merged requests consume the same prepared
    // bytes even if individual consumers want different channel combinations.
    for ( uint32_t i = 0u; i < m_nSlots; ++i ) {
        Slot &slot = m_slots[i];
        if ( slot.m_state != FREE && slot.m_state != FAILED && !slot.m_bCancelled && slot.m_nId == id &&
             slot.m_nEpoch == m_nEpoch && slot.m_nRevision == revision &&
             slot.m_nGeneration == m_nGeneration ) {
            slot.m_nChannels |= channels;
            if ( slot.m_nPriority < priority ) slot.m_nPriority = priority;
            ++m_stats.m_nCoalescedRequests;
            m_wake.notifyOne();
            return;
        }
    }
    for ( uint32_t i = 0u; i < m_nSlots; ++i ) {
        Slot &slot = m_slots[i];
        if ( slot.m_state == FREE || slot.m_state == FAILED ) {
            slot.m_state = QUEUED;
            slot.m_nId = id;
            slot.m_nChannels = channels;
            slot.m_nPriority = priority;
            slot.m_nRevision = revision;
            slot.m_nGeneration = m_nGeneration;
            slot.m_nEpoch = m_nEpoch;
            slot.m_nRequestedUsec = now;
            slot.m_bCancelled = false;
            m_wake.notifyOne();
            return;
        }
    }
    ++m_stats.m_nRejected; // bounded queue full; caller retries next tick
}

int32_t Terrain3DHierarchicalTileStore::selectQueuedLocked( void ) const {
    int32_t best = -1;
    // Priority dominates short-term demand, but after 1ms of waiting a
    // lower-priority node gains one point; this prevents indefinite starvation. Tie-break by node ID.
    const uint64_t now = t3dSteadyClockUsec();
    uint64_t bestScore = 0u;
    for ( uint32_t i = 0u; i < m_nSlots; ++i ) {
        const Slot &s = m_slots[i];
        if ( s.m_state != QUEUED ) continue;
        const uint64_t age = ( now >= s.m_nRequestedUsec ? now - s.m_nRequestedUsec : 0u ) / 1000u;
        const uint64_t score = ( uint64_t ) s.m_nPriority * 16u + ( age < 8192u ? age : 8192u );
        if ( best == -1 || score > bestScore ||
             ( score == bestScore && s.m_nId < m_slots[best].m_nId ) ) {
            best = ( int32_t ) i;
            bestScore = score;
        }
    }
    return best;
}

void Terrain3DHierarchicalTileStore::workerMain( void ) {
    for ( ;; ) {
        int32_t chosen = -1;
        t3dRasterHierarchyV2Node_t node;
        uint64_t epoch = 0u;
        godot::String path;
        {
            Terrain3DUniqueLock lock( m_mutex );
            while ( !m_bStop && ( chosen = selectQueuedLocked() ) < 0 ) m_wake.wait( lock );
            if ( m_bStop ) return;
            Slot &slot = m_slots[chosen];
            slot.m_state = LOADING;
            node = m_nodes[slot.m_nId];
            epoch = slot.m_nEpoch;
            path = m_path;
        }
        // Only the worker touches LOADING bytes. The owner may cancel a slot
        // but cannot reuse its storage until the worker reacquires this lock.
        const bool valid = t3dRasterHierarchyReadV2Tile(
            path, node, m_slots[chosen].m_bytes, T3D_RASTER_HIERARCHY_V2_TILE_BYTES );
        {
            Terrain3DScopedLock lock( m_mutex );
            Slot &slot = m_slots[chosen];
            ++m_stats.m_nFileReads;
            if ( valid ) m_stats.m_nVerifiedBytes += T3D_RASTER_HIERARCHY_V2_TILE_BYTES;
            if ( slot.m_bCancelled || slot.m_nEpoch != epoch || m_bStop ) {
                ++m_stats.m_nCancelled;
                clearSlotLocked( slot );
            } else if ( valid ) {
                slot.m_state = READY;
                ++m_stats.m_nPrepared;
            } else {
                slot.m_state = FAILED;
                slot.m_nRequestedUsec = t3dSteadyClockUsec();
                ++m_stats.m_nFailed;
            }
        }
    }
}

void Terrain3DHierarchicalTileStore::service( void ) {
    m_wake.notifyOne();
}

bool Terrain3DHierarchicalTileStore::isReady(
    const uint16_t id, const uint8_t channels, const uint32_t revision ) const {
    Terrain3DScopedLock lock( m_mutex );
    for ( uint32_t i = 0u; i < m_nSlots; ++i ) {
        const Slot &slot = m_slots[i];
        if ( slot.m_state == READY && !slot.m_bCancelled && slot.m_nId == id &&
             slot.m_nRevision == revision && slot.m_nEpoch == m_nEpoch &&
             ( slot.m_nChannels & channels ) == channels ) return true;
    }
    return false;
}

bool Terrain3DHierarchicalTileStore::takePrepared(
    const uint16_t id, const uint8_t channels, const uint32_t revision,
    t3dHierarchyPreparedTile_t &result ) {
    if ( channels == 0u || ( channels & ~T3D_HIERARCHY_TILE_ALL_CHANNELS ) != 0u ) return false;
    Terrain3DScopedLock lock( m_mutex );
    for ( uint32_t i = 0u; i < m_nSlots; ++i ) {
        Slot &slot = m_slots[i];
        if ( slot.m_state != READY || slot.m_bCancelled || slot.m_nId != id ||
             slot.m_nRevision != revision || slot.m_nEpoch != m_nEpoch ||
             ( slot.m_nChannels & channels ) != channels ) continue;
        // Reuse the caller's owning PackedByteArray storage after the first
        // successful take; no transient candidate arrays on the hot path.
        // Consumers must inspect m_nChannels, since unrequested arrays retain
        // their previous bytes to avoid deallocating reusable staging memory.
        const uint32_t lengths[3] = { T3D_RASTER_HIERARCHY_V2_HEIGHT_BYTES,
            T3D_RASTER_HIERARCHY_V2_CONTROL_BYTES, T3D_RASTER_HIERARCHY_V2_COLOR_BYTES };
        const uint32_t offsets[3] = { 0u, lengths[0], lengths[0] + lengths[1] };
        godot::PackedByteArray *arrays[3] = { &result.m_height, &result.m_control, &result.m_color };
        for ( uint32_t c = 0u; c < 3u; ++c ) {
            if ( !( channels & ( 1u << c ) ) ) continue;
            if ( arrays[c]->size() != lengths[c] && arrays[c]->resize( lengths[c] ) != godot::OK ) return false;
        }
        for ( uint32_t c = 0u; c < 3u; ++c ) {
            if ( channels & ( 1u << c ) )
                memcpy( arrays[c]->ptrw(), slot.m_bytes + offsets[c], lengths[c] );
        }
        result.m_nNodeId = id;
        result.m_nChannels = channels;
        result.m_nSourceRevision = revision;
        result.m_nContentGeneration = slot.m_nGeneration;
        result.m_nStoreEpoch = slot.m_nEpoch;
        clearSlotLocked( slot );
        return true;
    }
    return false;
}

void Terrain3DHierarchicalTileStore::cancel( const uint16_t id ) {
    Terrain3DScopedLock lock( m_mutex );
    for ( uint32_t i = 0u; i < m_nSlots; ++i ) {
        Slot &slot = m_slots[i];
        if ( slot.m_nId != id || slot.m_state == FREE ) continue;
        if ( slot.m_state == LOADING ) slot.m_bCancelled = true;
        else {
            clearSlotLocked( slot );
            ++m_stats.m_nCancelled;
        }
    }
}

void Terrain3DHierarchicalTileStore::reset( void ) {
    {
        Terrain3DScopedLock lock( m_mutex );
        m_bStop = true;
        m_wake.notifyAll();
    }
    if ( m_worker.joinable() ) m_worker.join();
    Terrain3DScopedLock lock( m_mutex );
    ++m_nEpoch;
    if ( m_nEpoch == 0u ) ++m_nEpoch;
    delete [] m_slots;
    m_slots = nullptr;
    memset( m_nodePresent, 0, sizeof( m_nodePresent ) );
    m_path = godot::String();
    m_nSlots = 0u;
    m_nGeneration = 0u;
    m_bRunning = false;
    m_bStop = false;
    m_stats = t3dHierarchyTileStoreStats_t{};
}

t3dHierarchyTileStoreStats_t Terrain3DHierarchicalTileStore::getStats( void ) const {
    Terrain3DScopedLock lock( m_mutex );
    t3dHierarchyTileStoreStats_t stats = m_stats;
    stats.m_nSlotCount = m_nSlots;
    stats.m_nResidentPoolBytes = m_nSlots * sizeof( Slot );
    for ( uint32_t i = 0u; i < m_nSlots; ++i ) {
        switch ( m_slots[i].m_state ) {
        case QUEUED: ++stats.m_nQueued; break;
        case LOADING: ++stats.m_nLoading; break;
        case READY: ++stats.m_nReady; break;
        default: break;
        }
    }
    return stats;
}
