// Copyright 2026 Terrain3D contributors.
#ifndef TERRAIN3D_VHM_RESIDENCY_H
#define TERRAIN3D_VHM_RESIDENCY_H

#include "rendering/rd/vhm/terrain_3d_vhm_reader.h"
#include <vector>
#include <stdint.h>

// A cold-prepared candidate bank. ST05 controls upload, atomic swap, and GPU
// fence retirement; this class never overwrites the published active bank.
class Terrain3DVhmRootAtlas
{
    std::vector<uint8_t> m_pixels;
    std::vector<t3dStVhmTail_t> m_tails;
public:
    bool initializeCold( void );
    bool stageRootCold( const Terrain3DVhmRegionView &rRegion );
    const uint8_t *getAtlasPixels( void ) const { return m_pixels.empty() ? nullptr : m_pixels.data(); }
    uint32_t getAtlasBytes( void ) const { return uint32_t( m_pixels.size() ); }
    const t3dStVhmTail_t *getTailEntries( void ) const { return m_tails.empty() ? nullptr : m_tails.data(); }
    uint32_t getTailCount( void ) const { return uint32_t( m_tails.size() ); }
};

struct t3dStVhmPhysicalSlot_t
{
    uint32_t m_nGlobalPage = T3D_ST_VHM_INVALID;
    uint32_t m_nGeneration = 0u;
    uint32_t m_nUploadTicket = 0u;
    uint32_t m_nState = 0u; // 0 FREE, 1 UPLOAD_PENDING, 2 RESIDENT
};

// Bounded, fixed-capacity, zero hot-path heap allocation. Slot reuse is
// explicitly delayed until the owner confirms GPU readers have retired.
// Texture RD creation, GPU upload and descriptor publication belong to ST05.
class Terrain3DVhmFinePagePool
{
    std::vector<t3dStVhmPhysicalSlot_t> m_slots;
    std::vector<uint8_t> m_payload;
    std::vector<uint32_t> m_freeSlots;
    std::vector<uint32_t> m_lookup; // 1024x11, invalid if not staged
    uint32_t m_nFreeCount = 0u;
    uint32_t m_nNextTicket = 0u;
public:
    bool initializeCold( uint32_t nPhysicalPages );
    bool preparePageHot( const Terrain3DVhmRegionView &rRegion, uint32_t nLocalPage,
                         uint32_t nGeneration, uint32_t &rSlot, uint32_t &rTicket );
    bool finishUpload( uint32_t nSlot, uint32_t nTicket );
    // Call only after an external GPU fence permits physical-slot reuse.
    bool retireAfterFence( uint32_t nSlot, uint32_t nTicket );
    bool isResident( uint32_t nGlobalPage, uint32_t nGeneration, uint32_t &rSlot ) const;
    const uint8_t *getUploadBytes( uint32_t nSlot, uint32_t nTicket ) const;
    // Snapshot for ST05's page-table publication, using only completed fine
    // uploads plus immutable parent IDs and generation-checked coarse tails.
    bool buildPageEntries( const Terrain3DVhmRegionView &rRegion,
                           uint32_t nGeneration,
                           t3dStVhmPageEntry_t *pElevenEntries ) const;
    uint32_t getSlotCount( void ) const { return uint32_t( m_slots.size() ); }
    uint32_t getFreeCount( void ) const { return m_nFreeCount; }
};

#endif // TERRAIN3D_VHM_RESIDENCY_H
