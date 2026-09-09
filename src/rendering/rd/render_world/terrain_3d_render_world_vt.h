// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_VT_H
#define TERRAIN3D_RENDER_WORLD_VT_H

#include "rendering/rd/render_world/terrain_3d_render_world_contract.h"

#include <stdint.h>

static const uint32_t T3D_RW_VT_FILE_MAGIC = 0x56543354u; // T3TV
static const uint32_t T3D_RW_VT_FILE_VERSION = 1u;
static const uint32_t T3D_RW_VT_PAGE_INTERIOR = 128u;
static const uint32_t T3D_RW_VT_PAGE_GUTTER = 4u;
static const uint32_t T3D_RW_VT_PHYSICAL_TILE = 136u;
static const uint32_t T3D_RW_VT_INVALID_PAGE = 0xffffffffu;
static const uint32_t T3D_RW_VT_INVALID_SLOT = 0xffffffffu;
static const uint32_t T3D_RW_VT_MAX_MIPS = 16u;
static const uint32_t T3D_RW_VT_CHANNELS = 4u;

enum T3DVTPhysicalSlotFlags : uint16_t {
	T3D_VT_SLOT_PINNED = 1u << 0u,
	T3D_VT_SLOT_PENDING = 1u << 1u,
	T3D_VT_SLOT_REFERENCED = 1u << 2u,
	T3D_VT_SLOT_HOT = 1u << 3u,
	T3D_VT_SLOT_RETIRED = 1u << 4u
};

struct T3DVTFileHeader {
	uint32_t nMagic;
	uint32_t nVersion;
	uint32_t nHeaderBytes;
	uint32_t nFlags;
	uint32_t nAllocationCount;
	uint32_t nPageCount;
	uint32_t nPayloadCount;
	uint32_t nChannelCount;
	uint64_t nAllocationOffset;
	uint64_t nPageOffset;
	uint64_t nPayloadDirectoryOffset;
	uint64_t nPayloadDataOffset;
	uint64_t nPayloadDataBytes;
	uint64_t nContentSignature;
	uint32_t nInteriorTexels;
	uint32_t nGutterTexels;
	uint32_t nPhysicalTexels;
	uint32_t nReserved0;
	uint64_t nReserved[4];
};

struct T3DVTPageFileRecord {
	uint32_t nVirtualPage;
	uint32_t nParentPage;
	uint32_t nAllocationId;
	uint32_t nPayloadId;
	uint16_t nMip;
	uint16_t nChannel;
	uint16_t nFlags;
	uint16_t nReserved0;
	uint32_t nGeneration;
	uint32_t nReserved1;
};

struct T3DVTPayloadRecord {
	uint64_t nOffset;
	uint32_t nCompressedBytes;
	uint32_t nDecodedBytes;
	uint32_t nCodec;
	uint32_t nChecksum;
	uint32_t nFlags;
	uint32_t nReserved;
};

struct T3DVTPackSource {
	const T3DVTAllocationRecord *pAllocations;
	uint32_t nAllocationCount;
	const T3DVTPageFileRecord *pPages;
	uint32_t nPageCount;
	const T3DVTPayloadRecord *pPayloads;
	uint32_t nPayloadCount;
	const uint8_t *pPayloadData;
	uint64_t nPayloadDataBytes;
};

struct T3DVTDatabaseView {
	const T3DVTFileHeader *pHeader;
	const T3DVTAllocationRecord *pAllocations;
	const T3DVTPageFileRecord *pPages;
	const T3DVTPayloadRecord *pPayloads;
	const uint8_t *pPayloadData;
	uint64_t nFileBytes;
};

struct T3DVTPhysicalSlot {
	uint32_t nVirtualPage;
	uint32_t nGeneration;
	uint32_t nLastTouchFrame;
	uint32_t nPendingVictimPage;
	uint16_t nChannel;
	uint16_t nFlags;
	uint16_t nClockAge;
	uint16_t nReserved0;
	uint32_t nReserved1;
	uint32_t nReserved2;
};

struct T3DVTResidencyBuffers {
	T3DVTPageTableEntry *pPageTable;
	uint32_t nPageTableCapacity;
	T3DVTPhysicalSlot *pPhysicalSlots[T3D_RW_VT_CHANNELS];
	uint32_t nPhysicalSlotCapacity[T3D_RW_VT_CHANNELS];
	uint16_t *pRetiredSlots[T3D_RW_VT_CHANNELS];
};

struct T3DVTUploadReservation {
	uint32_t nVirtualPage;
	uint32_t nPhysicalSlot;
	uint32_t nVictimPage;
	uint32_t nExpectedGeneration;
	uint16_t nChannel;
	uint16_t nWantedMip;
};

struct T3DVTResidencyStats {
	uint64_t nTouchCount;
	uint64_t nFallbackCount;
	uint64_t nUploadReservationCount;
	uint64_t nUploadCommitCount;
	uint64_t nEvictionCount;
	uint64_t nClockScanCount;
	uint64_t nReservationFailureCount;
	uint64_t nRetireFinalizeCount;
	uint64_t nRetireDeferredCount;
	uint32_t nFreeSlotCount[T3D_RW_VT_CHANNELS];
	uint32_t nResidentCount[T3D_RW_VT_CHANNELS];
};

struct T3DVTResidencyState {
	const T3DVTDatabaseView *pDatabase;
	T3DVTResidencyBuffers Buffers;
	T3DVTResidencyStats Stats;
	uint32_t nNeutralRootPage[T3D_RW_VT_CHANNELS];
	uint32_t nClockHand[T3D_RW_VT_CHANNELS];
	uint32_t nRetiredSlotCount[T3D_RW_VT_CHANNELS];
	uint32_t nDatabaseGeneration;
	uint32_t nPageTableGeneration;
	bool bInitialized;
	bool bNeutralPublished;
};

uint64_t t3d_render_world_vt_compute_pack_bytes( const T3DVTPackSource &pSource );
bool t3d_render_world_vt_write_pack_cold( const T3DVTPackSource &pSource, void *pDestination, uint64_t pDestinationBytes );
bool t3d_render_world_vt_open_pack_cold( const void *pBytes, uint64_t pByteCount, T3DVTDatabaseView &rView );

bool t3d_render_world_vt_initialize_residency_cold( const T3DVTDatabaseView &pDatabase,
        const T3DVTResidencyBuffers &pBuffers,
        uint32_t pDatabaseGeneration,
        T3DVTResidencyState &rState );
bool t3d_render_world_vt_commit_neutral_backing_warm( T3DVTResidencyState &rState, uint32_t pChannel, uint32_t pPhysicalSlot );
bool t3d_render_world_vt_finalize_neutral_publication_warm( T3DVTResidencyState &rState );
uint32_t t3d_render_world_vt_resolve_page_hot( T3DVTResidencyState &rState, uint32_t pVirtualPage, uint16_t &rResolvedMip );
bool t3d_render_world_vt_touch_page_hot( T3DVTResidencyState &rState, uint32_t pVirtualPage, uint32_t pFrameId );
bool t3d_render_world_vt_reserve_upload_hot(
        T3DVTResidencyState &rState, uint32_t pVirtualPage, uint16_t pWantedMip, uint32_t pFrameId, T3DVTUploadReservation &rReservation );
bool t3d_render_world_vt_can_commit_upload_hot( const T3DVTResidencyState &pState, const T3DVTUploadReservation &pReservation );
bool t3d_render_world_vt_commit_upload_hot( T3DVTResidencyState &rState, const T3DVTUploadReservation &pReservation, uint32_t pFrameId );
bool t3d_render_world_vt_finalize_eviction_hot(
        T3DVTResidencyState &rState, uint32_t pChannel, uint32_t pPhysicalSlot, uint32_t pVictimPage );
void t3d_render_world_vt_abort_upload_hot( T3DVTResidencyState &rState, const T3DVTUploadReservation &pReservation );
bool t3d_render_world_vt_validate_residency_reference_debug( const T3DVTResidencyState &pState );
bool t3d_render_world_vt_allocation_page_id(
        const T3DVTAllocationRecord &pAllocation, uint32_t pMip, uint32_t pPageX, uint32_t pPageY, uint32_t &rVirtualPage );

struct T3DVTPredictedPageRect {
	uint32_t nMip;
	uint32_t nMinPageX;
	uint32_t nMinPageY;
	uint32_t nMaxPageX;
	uint32_t nMaxPageY;
	uint32_t nPageCount;
};

bool t3d_render_world_vt_predict_page_rect_reference_debug( const T3DVTAllocationRecord &pAllocation,
        const float pUvMin[2],
        const float pUvMax[2],
        uint32_t pWantedMip,
        uint32_t pMaximumPages,
        T3DVTPredictedPageRect &rRect );

static_assert( sizeof( T3DVTFileHeader ) == 128, "T3DVTFileHeader ABI mismatch" );
static_assert( sizeof( T3DVTPageFileRecord ) == 32, "T3DVTPageFileRecord ABI mismatch" );
static_assert( sizeof( T3DVTPayloadRecord ) == 32, "T3DVTPayloadRecord ABI mismatch" );
static_assert( sizeof( T3DVTPhysicalSlot ) == 32, "T3DVTPhysicalSlot ABI mismatch" );
static_assert( sizeof( T3DVTUploadReservation ) == 20, "T3DVTUploadReservation ABI mismatch" );

#endif // TERRAIN3D_RENDER_WORLD_VT_H
