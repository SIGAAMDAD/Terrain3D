// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_RESIDENCY_H
#define TERRAIN3D_VT_RESIDENCY_H

#include "rendering/rd/vt/terrain_3d_vt_contract.h"
#include "rendering/rd/vt/terrain_3d_vt_runtime_contract.h"

#include <stdint.h>

// Sparse V3 residency uses the runtime-normalized allocation/page tables;
// physical storage remains format-family based and shared across logical channels.
static const uint32_t T3D_VT_LEGACY_FILE_MAGIC = 0x56543354u; // T3TV
static const uint32_t T3D_VT_LEGACY_FILE_VERSION = 1u;
static const uint32_t T3D_VT_PAGE_SET_CHANNEL_MASK = T3D_VT_CHANNEL_MASK_ALL;
static const uint32_t T3D_VT_RESIDENCY_CATEGORY_COUNT = 5u;

enum T3DVTResidencyCategory : uint16_t {
	T3D_VT_RESIDENCY_TERRAIN = 0u,
	T3D_VT_RESIDENCY_CLUTTER = 1u,
	T3D_VT_RESIDENCY_GROUND_COVER = 2u,
	T3D_VT_RESIDENCY_OCCLUDER = 3u,
	T3D_VT_RESIDENCY_HLOD = 4u,
};

enum T3DVTResidencyTemperature : uint16_t {
	T3D_VT_RESIDENCY_HOT = 0u,
	T3D_VT_RESIDENCY_WARM = 1u,
	T3D_VT_RESIDENCY_COLD = 2u,
};

struct T3DVTResidencyPolicy {
	uint32_t nHotHoldFrames[T3D_VT_RESIDENCY_CATEGORY_COUNT];
	uint32_t nWarmHoldFrames[T3D_VT_RESIDENCY_CATEGORY_COUNT];
	// GPU-P01: capacity-scaled cold-page protection. Percentages deliberately
	// sum below 100 so categories can borrow unused physical cache from peers.
	uint32_t nProtectedResidentPercent[T3D_VT_RESIDENCY_CATEGORY_COUNT];
	uint32_t nThrashWindowFrames;
	uint32_t nRetireDelayFrames;
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

enum T3DVTRuntimeDatabaseKind : uint32_t {
	T3D_VT_RUNTIME_DATABASE_LEGACY_NEUTRAL = 0u,
	T3D_VT_RUNTIME_DATABASE_PACK_V3 = 2u,
};


struct T3DVTRuntimePayloadRecord {
	uint64_t nOffset;
	uint32_t nBytes;
	uint32_t nChannel;
	uint32_t nContentId;
	uint32_t nChecksum;
};

// Runtime source metadata for the authored compact mip chain stored in a sparse
// V3 tail payload. The chain remains in its cooked horizontal layout.
struct T3DVTRuntimeTailSourceRecord {
	uint32_t nPayloadId[T3D_VT_CHANNEL_COUNT];
	uint16_t nAtlasInteriorWidth;
	uint16_t nAtlasInteriorHeight;
};

struct T3DVTPackSource {
	const T3DVTAllocationRecordV1 *pAllocations;
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
	const T3DVTAllocationRecordV1 *pAllocations;
	const T3DVTPageFileRecord *pPages;
	const T3DVTPayloadRecord *pPayloads;
	const uint8_t *pPayloadData;
	uint64_t nFileBytes;
	const t3dVTAllocation_t *pRuntimeAllocations;
	const t3dVTTailMetadata_t *pRuntimeTailMetadata;
	const T3DVTRuntimeTailSourceRecord *pRuntimeTailSources;
	const T3DVTRuntimePayloadRecord *pRuntimePayloads;
	uint32_t nRuntimePayloadCount;
	uint32_t nRuntimeTailCount;
	const t3dVTPackAllocationRecordV3_t *pV3Allocations;
	const t3dVTChannelAllocation_t *pV3ChannelAllocations;
	const t3dVTPackPageRecordV3_t *pV3Pages;
	const t3dVTPackTailRecordV3_t *pV3Tails;
	const t3dVTPackQualityViewV3_t *pV3QualityViews;
	const t3dVTPackFormatFamilyRecordV3_t *pV3FormatFamilies;
	uint32_t nV3ChannelAllocationCount;
	uint32_t nV3PageCount;
	uint32_t nV3TailCount;
	uint32_t nV3QualityViewCount;
	uint32_t nV3FormatFamilyCount;
	uint32_t nV3FullySupportedProfileMask;
	uint32_t nPackageGeneration;
	uint32_t nRuntimeKind;
	uint64_t nPackageContentSignature;
};

struct T3DVTRetiredPageSet {
	uint32_t nPhysicalSlot;
	uint32_t nVirtualPage;
	uint32_t nGeneration;
	uint32_t nReleaseFrame;
};

struct T3DVTPageSetPayloadView {
	const uint8_t *pChannelBytes[T3D_VT_CHANNEL_COUNT];
	uint32_t nChannelBytes[T3D_VT_CHANNEL_COUNT];
	uint32_t nPresentMask;
};

struct T3DVTResidencyBuffers {
	t3dVTAllocation_t *pAllocations;
	uint32_t nAllocationCapacity;
	T3DVTPageTableEntry *pPageTable;
	uint32_t nPageTableCapacity;
	t3dVTTailMetadata_t *pTailMetadata;
	uint32_t nTailMetadataCapacity;
	t3dVTPhysicalSlot_t *pPhysicalPageSets;
	uint32_t nPhysicalPageSetCapacity;
	uint32_t *pFreeSlots;
	T3DVTRetiredPageSet *pRetiredPageSets;
	uint32_t *pLastEvictedFrame;
};

struct T3DVTUploadReservation {
	uint32_t nVirtualPage;
	uint32_t nPhysicalSlot;
	uint32_t nVictimPage;
	uint32_t nExpectedGeneration;
	uint32_t nDatabaseGeneration;
	uint16_t nWantedMip;
	uint16_t nReserved0;
};

struct T3DVTResidencyStats {
	uint64_t nTouchCount;
	uint64_t nFallbackCount;
	uint64_t nAncestorFallbackCount;
	uint64_t nTailFallbackCount;
	uint64_t nUploadReservationCount;
	uint64_t nUploadCommitCount;
	uint64_t nEvictionCount;
	uint64_t nClockScanCount;
	uint64_t nReservationFailureCount;
	uint64_t nRetireFinalizeCount;
	uint64_t nRetireDeferredCount;
	uint64_t nThrashCount;
	uint64_t nHotScanCount;
	uint64_t nWarmScanCount;
	uint64_t nColdScanCount;
	uint64_t nPinnedProtectionCount;
	uint32_t nFreeSlotCount;
	uint32_t nResidentCount;
	uint32_t nPendingSlotCount;
	uint32_t nRetiredSlotCount;
};

struct T3DVTResidencyState {
	const T3DVTDatabaseView *pDatabase;
	T3DVTResidencyBuffers Buffers;
	T3DVTResidencyStats Stats;
	T3DVTResidencyPolicy Policy;
	uint32_t nNeutralRootPage[T3D_VT_CHANNEL_COUNT];
	uint32_t nClockHand;
	uint32_t nRetireHead;
	uint32_t nRetireTail;
	uint32_t nRetirementDelayFrames;
	uint32_t nDatabaseGeneration;
	uint32_t nPageTableGeneration;
	uint32_t nAllocationCount;
	uint32_t nTailCount;
	bool bInitialized;
	bool bNeutralPublished;
};

void t3d_vt_default_residency_policy( T3DVTResidencyPolicy &rPolicy );
bool t3d_vt_validate_residency_policy( const T3DVTResidencyPolicy &pPolicy );
uint16_t t3d_vt_residency_category_reference_debug( const T3DVTDatabaseView &pDatabase, uint32_t pVirtualPage );
uint16_t t3d_vt_residency_temperature_reference_debug(
        const T3DVTResidencyState &pState, const t3dVTPhysicalSlot_t &pSlot, uint32_t pFrameId );
bool t3d_vt_retirement_ready_reference_debug( const T3DVTResidencyState &pState, const T3DVTRetiredPageSet &pRetired, uint32_t pFrameId );

uint64_t t3d_vt_compute_pack_bytes( const T3DVTPackSource &pSource );
bool t3d_vt_write_pack_cold( const T3DVTPackSource &pSource, void *pDestination, uint64_t pDestinationBytes );
bool t3d_vt_open_pack_cold( const void *pBytes, uint64_t pByteCount, T3DVTDatabaseView &rView );

bool t3d_vt_initialize_residency_cold( const T3DVTDatabaseView &pDatabase,
        const T3DVTResidencyBuffers &pBuffers,
        uint32_t pDatabaseGeneration,
        uint32_t pRetirementDelayFrames,
        T3DVTResidencyState &rState );
bool t3d_vt_commit_neutral_backing_warm( T3DVTResidencyState &rState, uint32_t pPhysicalSlot );
bool t3d_vt_finalize_neutral_publication_warm( T3DVTResidencyState &rState );
bool t3d_vt_finalize_package_publication_warm( T3DVTResidencyState &rState );
uint32_t t3d_vt_resolve_page_hot( T3DVTResidencyState &rState, uint32_t pVirtualPage, uint16_t &rResolvedMip );
bool t3d_vt_allocation_lookup_hot( const T3DVTResidencyState &pState, uint32_t pAllocationId, const t3dVTAllocation_t *&rAllocation );
bool t3d_vt_compute_page( const t3dVTAllocation_t &pAllocation, uint32_t pMip, uint32_t pPageX, uint32_t pPageY, uint32_t &rVirtualPage );
bool t3d_vt_resolve_allocation_page_hot( T3DVTResidencyState &rState,
        uint32_t pAllocationId,
        uint32_t pRequestedPage,
        uint16_t pRequestedMip,
        t3dVTResolveResult_t &rResult );
bool t3d_vt_resolve_allocation_page_reference_debug( const T3DVTResidencyState &rState,
        uint32_t pAllocationId,
        uint32_t pRequestedPage,
        uint16_t pRequestedMip,
        t3dVTResolveResult_t &rResult );
bool t3d_vt_resolve_allocation_hot( T3DVTResidencyState &rState,
        uint32_t pAllocationId,
        uint16_t pRequestedMip,
        uint32_t pPageX,
        uint32_t pPageY,
        t3dVTResolveResult_t &rResult );
bool t3d_vt_touch_page_hot( T3DVTResidencyState &rState, uint32_t pVirtualPage, uint32_t pFrameId );
bool t3d_vt_reserve_upload_hot(
        T3DVTResidencyState &rState, uint32_t pVirtualPage, uint16_t pWantedMip, uint32_t pFrameId, T3DVTUploadReservation &rReservation );
bool t3d_vt_can_commit_upload_hot( const T3DVTResidencyState &pState, const T3DVTUploadReservation &pReservation );
bool t3d_vt_commit_upload_hot(
        T3DVTResidencyState &rState, const T3DVTUploadReservation &pReservation, uint32_t pUploadedChannelMask, uint32_t pFrameId );
bool t3d_vt_begin_eviction_hot( T3DVTResidencyState &rState, uint32_t pPhysicalSlot, uint32_t pVictimPage, uint32_t pFrameId );
bool t3d_vt_finalize_eviction_hot( T3DVTResidencyState &rState, uint32_t pPhysicalSlot, uint32_t pVictimPage, uint32_t pFrameId );
bool t3d_vt_service_retired_slots_hot( T3DVTResidencyState &rState, uint32_t pFrameId );
void t3d_vt_abort_upload_hot( T3DVTResidencyState &rState, const T3DVTUploadReservation &pReservation );
bool t3d_vt_validate_residency_reference_debug( const T3DVTResidencyState &pState );
bool t3d_vt_allocation_page_id(
        const t3dVTAllocation_t &pAllocation, uint32_t pMip, uint32_t pPageX, uint32_t pPageY, uint32_t &rVirtualPage );

struct T3DVTPredictedPageRect {
	uint32_t nMip;
	uint32_t nMinPageX;
	uint32_t nMinPageY;
	uint32_t nMaxPageX;
	uint32_t nMaxPageY;
	uint32_t nPageCount;
};

bool t3d_vt_predict_page_rect_reference_debug( const t3dVTAllocation_t &pAllocation,
        const float pUvMin[2],
        const float pUvMax[2],
        uint32_t pWantedMip,
        uint32_t pMaximumPages,
        T3DVTPredictedPageRect &rRect );

static_assert( sizeof( T3DVTFileHeader ) == 128, "T3DVTFileHeader ABI mismatch" );
static_assert( sizeof( T3DVTPageFileRecord ) == 32, "T3DVTPageFileRecord ABI mismatch" );
static_assert( sizeof( T3DVTPayloadRecord ) == 32, "T3DVTPayloadRecord ABI mismatch" );
static_assert( sizeof( t3dVTPhysicalSlot_t ) == 32, "P17 physical page-set slot ABI mismatch" );
static_assert( sizeof( T3DVTRetiredPageSet ) == 16, "P17 retirement ring record ABI mismatch" );
static_assert( sizeof( T3DVTUploadReservation ) == 24, "P17 upload reservation ABI mismatch" );

#endif // TERRAIN3D_VT_RESIDENCY_H
