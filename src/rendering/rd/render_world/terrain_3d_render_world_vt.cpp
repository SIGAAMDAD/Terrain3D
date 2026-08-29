// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_vt.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static uint64_t t3d_rw_vt_align16( uint64_t pValue ) {
	return ( pValue + 15u ) & ~15ull;
}

static bool t3d_rw_vt_range_valid( uint64_t pOffset, uint64_t pBytes, uint64_t pLimit ) {
	return pOffset <= pLimit && pBytes <= pLimit - pOffset;
}

static uint64_t t3d_rw_vt_fnv1a64( const uint8_t *pBytes, uint64_t pCount ) {
	uint64_t nHash = 1469598103934665603ull;
	for ( uint64_t i = 0u; i < pCount; ++i ) {
		nHash ^= pBytes[i];
		nHash *= 1099511628211ull;
	}
	return nHash;
}

static const T3DVTPageFileRecord *t3d_rw_vt_find_page( const T3DVTDatabaseView &pDatabase, uint32_t pVirtualPage ) {
	if ( pVirtualPage >= pDatabase.pHeader->nPageCount ) {
		return NULL;
	}
	const T3DVTPageFileRecord &Page = pDatabase.pPages[pVirtualPage];
	return Page.nVirtualPage == pVirtualPage ? &Page : NULL;
}

uint64_t t3d_render_world_vt_compute_pack_bytes( const T3DVTPackSource &pSource ) {
	if ( pSource.pAllocations == NULL || pSource.nAllocationCount == 0u || pSource.pPages == NULL || pSource.nPageCount == 0u
	        || pSource.pPayloads == NULL || pSource.nPayloadCount == 0u || pSource.pPayloadData == NULL
	        || pSource.nPayloadDataBytes == 0u ) {
		return 0u;
	}

	uint64_t nOffset = sizeof( T3DVTFileHeader );
	nOffset = t3d_rw_vt_align16( nOffset + ( uint64_t )pSource.nAllocationCount * sizeof( T3DVTAllocationRecord ) );
	nOffset = t3d_rw_vt_align16( nOffset + ( uint64_t )pSource.nPageCount * sizeof( T3DVTPageFileRecord ) );
	nOffset = t3d_rw_vt_align16( nOffset + ( uint64_t )pSource.nPayloadCount * sizeof( T3DVTPayloadRecord ) );
	if ( nOffset > UINT64_MAX - pSource.nPayloadDataBytes ) {
		return 0u;
	}
	return nOffset + pSource.nPayloadDataBytes;
}

bool t3d_render_world_vt_write_pack_cold( const T3DVTPackSource &pSource, void *pDestination, uint64_t pDestinationBytes ) {
	const uint64_t nRequired = t3d_render_world_vt_compute_pack_bytes( pSource );
	if ( nRequired == 0u || pDestination == NULL || pDestinationBytes < nRequired ) {
		return false;
	}

	uint8_t *pOut = ( uint8_t * )pDestination;
	memset( pOut, 0, ( size_t )nRequired );
	T3DVTFileHeader *pHeader = ( T3DVTFileHeader * )pOut;
	pHeader->nMagic = T3D_RW_VT_FILE_MAGIC;
	pHeader->nVersion = T3D_RW_VT_FILE_VERSION;
	pHeader->nHeaderBytes = sizeof( T3DVTFileHeader );
	pHeader->nAllocationCount = pSource.nAllocationCount;
	pHeader->nPageCount = pSource.nPageCount;
	pHeader->nPayloadCount = pSource.nPayloadCount;
	pHeader->nChannelCount = T3D_RW_VT_CHANNELS;
	pHeader->nInteriorTexels = T3D_RW_VT_PAGE_INTERIOR;
	pHeader->nGutterTexels = T3D_RW_VT_PAGE_GUTTER;
	pHeader->nPhysicalTexels = T3D_RW_VT_PHYSICAL_TILE;

	uint64_t nOffset = sizeof( T3DVTFileHeader );
	pHeader->nAllocationOffset = nOffset;
	memcpy( pOut + nOffset, pSource.pAllocations, ( size_t )pSource.nAllocationCount * sizeof( T3DVTAllocationRecord ) );
	nOffset = t3d_rw_vt_align16( nOffset + ( uint64_t )pSource.nAllocationCount * sizeof( T3DVTAllocationRecord ) );
	pHeader->nPageOffset = nOffset;
	memcpy( pOut + nOffset, pSource.pPages, ( size_t )pSource.nPageCount * sizeof( T3DVTPageFileRecord ) );
	nOffset = t3d_rw_vt_align16( nOffset + ( uint64_t )pSource.nPageCount * sizeof( T3DVTPageFileRecord ) );
	pHeader->nPayloadDirectoryOffset = nOffset;
	memcpy( pOut + nOffset, pSource.pPayloads, ( size_t )pSource.nPayloadCount * sizeof( T3DVTPayloadRecord ) );
	nOffset = t3d_rw_vt_align16( nOffset + ( uint64_t )pSource.nPayloadCount * sizeof( T3DVTPayloadRecord ) );
	pHeader->nPayloadDataOffset = nOffset;
	pHeader->nPayloadDataBytes = pSource.nPayloadDataBytes;
	memcpy( pOut + nOffset, pSource.pPayloadData, ( size_t )pSource.nPayloadDataBytes );
	pHeader->nContentSignature = t3d_rw_vt_fnv1a64( pOut + sizeof( T3DVTFileHeader ), nRequired - sizeof( T3DVTFileHeader ) );
	return true;
}

bool t3d_render_world_vt_open_pack_cold( const void *pBytes, uint64_t pByteCount, T3DVTDatabaseView &rView ) {
	memset( &rView, 0, sizeof( rView ) );
	if ( pBytes == NULL || pByteCount < sizeof( T3DVTFileHeader ) ) {
		return false;
	}
	const uint8_t *pData = ( const uint8_t * )pBytes;
	const T3DVTFileHeader *pHeader = ( const T3DVTFileHeader * )pData;
	if ( pHeader->nMagic != T3D_RW_VT_FILE_MAGIC || pHeader->nVersion != T3D_RW_VT_FILE_VERSION
	        || pHeader->nHeaderBytes != sizeof( T3DVTFileHeader ) ) {
		return false;
	}
	if ( pHeader->nChannelCount != T3D_RW_VT_CHANNELS || pHeader->nInteriorTexels != T3D_RW_VT_PAGE_INTERIOR
	        || pHeader->nGutterTexels != T3D_RW_VT_PAGE_GUTTER || pHeader->nPhysicalTexels != T3D_RW_VT_PHYSICAL_TILE ) {
		return false;
	}
	if ( pHeader->nAllocationCount == 0u || pHeader->nPageCount == 0u || pHeader->nPayloadCount == 0u
	        || pHeader->nPayloadDataBytes == 0u ) {
		return false;
	}
	if ( !t3d_rw_vt_range_valid(
	             pHeader->nAllocationOffset, ( uint64_t )pHeader->nAllocationCount * sizeof( T3DVTAllocationRecord ), pByteCount ) ) {
		return false;
	}
	if ( !t3d_rw_vt_range_valid( pHeader->nPageOffset, ( uint64_t )pHeader->nPageCount * sizeof( T3DVTPageFileRecord ), pByteCount ) ) {
		return false;
	}
	if ( !t3d_rw_vt_range_valid(
	             pHeader->nPayloadDirectoryOffset, ( uint64_t )pHeader->nPayloadCount * sizeof( T3DVTPayloadRecord ), pByteCount ) ) {
		return false;
	}
	if ( !t3d_rw_vt_range_valid( pHeader->nPayloadDataOffset, pHeader->nPayloadDataBytes, pByteCount ) ) {
		return false;
	}
	if ( t3d_rw_vt_fnv1a64( pData + sizeof( T3DVTFileHeader ), pByteCount - sizeof( T3DVTFileHeader ) ) != pHeader->nContentSignature ) {
		return false;
	}

	rView.pHeader = pHeader;
	rView.pAllocations = ( const T3DVTAllocationRecord * )( pData + pHeader->nAllocationOffset );
	rView.pPages = ( const T3DVTPageFileRecord * )( pData + pHeader->nPageOffset );
	rView.pPayloads = ( const T3DVTPayloadRecord * )( pData + pHeader->nPayloadDirectoryOffset );
	rView.pPayloadData = pData + pHeader->nPayloadDataOffset;
	rView.nFileBytes = pByteCount;

	for ( uint32_t i = 0u; i < pHeader->nPageCount; ++i ) {
		const T3DVTPageFileRecord &Page = rView.pPages[i];
		if ( Page.nVirtualPage != i || Page.nChannel >= T3D_RW_VT_CHANNELS || Page.nPayloadId >= pHeader->nPayloadCount ) {
			return false;
		}
		if ( Page.nParentPage != T3D_RW_VT_INVALID_PAGE && Page.nParentPage >= pHeader->nPageCount ) {
			return false;
		}
	}
	for ( uint32_t i = 0u; i < pHeader->nPayloadCount; ++i ) {
		const T3DVTPayloadRecord &Payload = rView.pPayloads[i];
		if ( !t3d_rw_vt_range_valid( Payload.nOffset, Payload.nCompressedBytes, pHeader->nPayloadDataBytes ) ) {
			return false;
		}
	}
	return true;
}

bool t3d_render_world_vt_initialize_residency_cold( const T3DVTDatabaseView &pDatabase,
        const T3DVTResidencyBuffers &pBuffers,
        uint32_t pDatabaseGeneration,
        T3DVTResidencyState &rState ) {
	memset( &rState, 0, sizeof( rState ) );
	if ( pDatabase.pHeader == NULL || pBuffers.pPageTable == NULL || pBuffers.nPageTableCapacity < pDatabase.pHeader->nPageCount ) {
		return false;
	}
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNELS; ++c ) {
		if ( pBuffers.pPhysicalSlots[c] == NULL || pBuffers.nPhysicalSlotCapacity[c] < 2u ) {
			return false;
		}
	}

	rState.pDatabase = &pDatabase;
	rState.Buffers = pBuffers;
	rState.nDatabaseGeneration = pDatabaseGeneration;
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNELS; ++c ) {
		rState.nNeutralRootPage[c] = T3D_RW_VT_INVALID_PAGE;
	}

	for ( uint32_t i = 0u; i < pDatabase.pHeader->nPageCount; ++i ) {
		const T3DVTPageFileRecord &Page = pDatabase.pPages[i];
		T3DVTPageTableEntry &Entry = pBuffers.pPageTable[i];
		Entry.nPhysicalSlot = T3D_RW_VT_INVALID_SLOT;
		Entry.nParentPage = Page.nParentPage;
		Entry.nGeneration = Page.nGeneration;
		Entry.nResolvedMip = Page.nMip;
		Entry.nFlags = T3D_VT_PAGE_VALID;
		if ( ( Page.nFlags & T3D_VT_PAGE_NEUTRAL ) != 0u ) {
			rState.nNeutralRootPage[Page.nChannel] = i;
		}
	}
	// Make every allocation terminal root fall back to the channel neutral root.
	// This gives CPU and GPU walkers one explicit parent chain instead of a hidden
	// out-of-band neutral fallback rule.
	for ( uint32_t i = 0u; i < pDatabase.pHeader->nPageCount; ++i ) {
		const T3DVTPageFileRecord &Page = pDatabase.pPages[i];
		if ( Page.nChannel >= T3D_RW_VT_CHANNELS ) {
			return false;
		}
		if ( pBuffers.pPageTable[i].nParentPage == T3D_RW_VT_INVALID_PAGE && i != rState.nNeutralRootPage[Page.nChannel] ) {
			pBuffers.pPageTable[i].nParentPage = rState.nNeutralRootPage[Page.nChannel];
		}
	}

	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNELS; ++c ) {
		for ( uint32_t i = 0u; i < pBuffers.nPhysicalSlotCapacity[c]; ++i ) {
			T3DVTPhysicalSlot &Slot = pBuffers.pPhysicalSlots[c][i];
			memset( &Slot, 0, sizeof( Slot ) );
			Slot.nVirtualPage = T3D_RW_VT_INVALID_PAGE;
			Slot.nPendingVictimPage = T3D_RW_VT_INVALID_PAGE;
			Slot.nChannel = ( uint16_t )c;
		}
		rState.Stats.nFreeSlotCount[c] = pBuffers.nPhysicalSlotCapacity[c];
		if ( rState.nNeutralRootPage[c] == T3D_RW_VT_INVALID_PAGE ) {
			return false;
		}
	}
	rState.bInitialized = true;
	return true;
}

bool t3d_render_world_vt_commit_neutral_backing_warm( T3DVTResidencyState &rState, uint32_t pChannel, uint32_t pPhysicalSlot ) {
	if ( !rState.bInitialized || pChannel >= T3D_RW_VT_CHANNELS || pPhysicalSlot >= rState.Buffers.nPhysicalSlotCapacity[pChannel] ) {
		return false;
	}
	const uint32_t nPage = rState.nNeutralRootPage[pChannel];
	if ( nPage == T3D_RW_VT_INVALID_PAGE ) {
		return false;
	}
	T3DVTPhysicalSlot &Slot = rState.Buffers.pPhysicalSlots[pChannel][pPhysicalSlot];
	if ( Slot.nVirtualPage != T3D_RW_VT_INVALID_PAGE ) {
		return false;
	}
	Slot.nVirtualPage = nPage;
	Slot.nGeneration = rState.Buffers.pPageTable[nPage].nGeneration;
	Slot.nFlags = T3D_VT_SLOT_PINNED | T3D_VT_SLOT_REFERENCED | T3D_VT_SLOT_HOT;
	T3DVTPageTableEntry &Entry = rState.Buffers.pPageTable[nPage];
	Entry.nPhysicalSlot = pPhysicalSlot;
	Entry.nFlags |= T3D_VT_PAGE_RESIDENT | T3D_VT_PAGE_PINNED | T3D_VT_PAGE_NEUTRAL | T3D_VT_PAGE_ROOT;
	--rState.Stats.nFreeSlotCount[pChannel];
	++rState.Stats.nResidentCount[pChannel];
	++rState.nPageTableGeneration;
	return true;
}

bool t3d_render_world_vt_finalize_neutral_publication_warm( T3DVTResidencyState &rState ) {
	if ( !rState.bInitialized ) {
		return false;
	}
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNELS; ++c ) {
		const uint32_t nRoot = rState.nNeutralRootPage[c];
		if ( nRoot == T3D_RW_VT_INVALID_PAGE || ( rState.Buffers.pPageTable[nRoot].nFlags & T3D_VT_PAGE_RESIDENT ) == 0u ) {
			return false;
		}
		if ( rState.Stats.nFreeSlotCount[c] == 0u ) {
			return false; // permanent upload-before-evict spare
		}
	}
	rState.bNeutralPublished = true;
	return true;
}

uint32_t t3d_render_world_vt_resolve_page_hot( T3DVTResidencyState &rState, uint32_t pVirtualPage, uint16_t &rResolvedMip ) {
	rResolvedMip = 0u;
	if ( !rState.bNeutralPublished || pVirtualPage >= rState.pDatabase->pHeader->nPageCount ) {
		return T3D_RW_VT_INVALID_SLOT;
	}
	uint32_t nPage = pVirtualPage;
	for ( uint32_t i = 0u; i < T3D_RW_VT_MAX_MIPS + 2u; ++i ) {
		T3DVTPageTableEntry &Entry = rState.Buffers.pPageTable[nPage];
		if ( ( Entry.nFlags & T3D_VT_PAGE_RESIDENT ) != 0u && Entry.nPhysicalSlot != T3D_RW_VT_INVALID_SLOT ) {
			rResolvedMip = Entry.nResolvedMip;
			if ( nPage != pVirtualPage ) {
				++rState.Stats.nFallbackCount;
			}
			return Entry.nPhysicalSlot;
		}
		if ( Entry.nParentPage == T3D_RW_VT_INVALID_PAGE ) {
			break;
		}
		nPage = Entry.nParentPage;
	}
	const T3DVTPageFileRecord *pPage = t3d_rw_vt_find_page( *rState.pDatabase, pVirtualPage );
	if ( pPage == NULL ) {
		return T3D_RW_VT_INVALID_SLOT;
	}
	const uint32_t nRoot = rState.nNeutralRootPage[pPage->nChannel];
	if ( nRoot == T3D_RW_VT_INVALID_PAGE ) {
		return T3D_RW_VT_INVALID_SLOT;
	}
	++rState.Stats.nFallbackCount;
	rResolvedMip = rState.Buffers.pPageTable[nRoot].nResolvedMip;
	return rState.Buffers.pPageTable[nRoot].nPhysicalSlot;
}

bool t3d_render_world_vt_touch_page_hot( T3DVTResidencyState &rState, uint32_t pVirtualPage, uint32_t pFrameId ) {
	if ( !rState.bInitialized || pVirtualPage >= rState.pDatabase->pHeader->nPageCount ) {
		return false;
	}
	const T3DVTPageFileRecord &Page = rState.pDatabase->pPages[pVirtualPage];
	T3DVTPageTableEntry &Entry = rState.Buffers.pPageTable[pVirtualPage];
	if ( ( Entry.nFlags & T3D_VT_PAGE_RESIDENT ) == 0u || Entry.nPhysicalSlot >= rState.Buffers.nPhysicalSlotCapacity[Page.nChannel] ) {
		return false;
	}
	T3DVTPhysicalSlot &Slot = rState.Buffers.pPhysicalSlots[Page.nChannel][Entry.nPhysicalSlot];
	Slot.nLastTouchFrame = pFrameId;
	Slot.nFlags |= T3D_VT_SLOT_REFERENCED;
	if ( Slot.nClockAge < 0xffffu ) {
		++Slot.nClockAge;
	}
	if ( Slot.nClockAge >= 2u ) {
		Slot.nFlags |= T3D_VT_SLOT_HOT;
	}
	++rState.Stats.nTouchCount;
	return true;
}

static uint32_t t3d_rw_vt_find_free_slot( T3DVTResidencyState &rState, uint32_t pChannel ) {
	for ( uint32_t i = 0u; i < rState.Buffers.nPhysicalSlotCapacity[pChannel]; ++i ) {
		if ( rState.Buffers.pPhysicalSlots[pChannel][i].nVirtualPage == T3D_RW_VT_INVALID_PAGE
		        && ( rState.Buffers.pPhysicalSlots[pChannel][i].nFlags & T3D_VT_SLOT_PENDING ) == 0u ) {
			return i;
		}
	}
	return T3D_RW_VT_INVALID_SLOT;
}

static uint32_t t3d_rw_vt_choose_victim( T3DVTResidencyState &rState, uint32_t pChannel ) {
	const uint32_t nCapacity = rState.Buffers.nPhysicalSlotCapacity[pChannel];
	for ( uint32_t i = 0u; i < nCapacity * 3u; ++i ) {
		const uint32_t nSlot = rState.nClockHand[pChannel]++ % nCapacity;
		T3DVTPhysicalSlot &Slot = rState.Buffers.pPhysicalSlots[pChannel][nSlot];
		++rState.Stats.nClockScanCount;
		if ( Slot.nVirtualPage == T3D_RW_VT_INVALID_PAGE
		        || ( Slot.nFlags & ( T3D_VT_SLOT_PINNED | T3D_VT_SLOT_PENDING | T3D_VT_SLOT_RETIRED ) ) != 0u ) {
			continue;
		}
		if ( ( Slot.nFlags & T3D_VT_SLOT_REFERENCED ) != 0u ) {
			Slot.nFlags &= ( uint16_t )~T3D_VT_SLOT_REFERENCED;
			continue;
		}
		if ( ( Slot.nFlags & T3D_VT_SLOT_HOT ) != 0u ) {
			Slot.nFlags &= ( uint16_t )~T3D_VT_SLOT_HOT;
			if ( Slot.nClockAge != 0u ) {
				--Slot.nClockAge;
			}
			continue;
		}
		return nSlot;
	}
	return T3D_RW_VT_INVALID_SLOT;
}

bool t3d_render_world_vt_reserve_upload_hot(
        T3DVTResidencyState &rState, uint32_t pVirtualPage, uint16_t pWantedMip, uint32_t pFrameId, T3DVTUploadReservation &rReservation ) {
	memset( &rReservation, 0, sizeof( rReservation ) );
	rReservation.nVirtualPage = T3D_RW_VT_INVALID_PAGE;
	rReservation.nPhysicalSlot = T3D_RW_VT_INVALID_SLOT;
	rReservation.nVictimPage = T3D_RW_VT_INVALID_PAGE;
	if ( !rState.bNeutralPublished || pVirtualPage >= rState.pDatabase->pHeader->nPageCount ) {
		return false;
	}
	const T3DVTPageFileRecord &Page = rState.pDatabase->pPages[pVirtualPage];
	T3DVTPageTableEntry &Entry = rState.Buffers.pPageTable[pVirtualPage];
	if ( ( Entry.nFlags & T3D_VT_PAGE_RESIDENT ) != 0u ) {
		return false;
	}

	const uint32_t nChannel = Page.nChannel;
	uint32_t nFree = t3d_rw_vt_find_free_slot( rState, nChannel );
	if ( nFree == T3D_RW_VT_INVALID_SLOT ) {
		return false;
	}
	uint32_t nVictimPage = T3D_RW_VT_INVALID_PAGE;

	// Preserve one free upload target. When this is the spare, nominate an old resident
	// now but do not unpublish it until the replacement upload is known-good.
	if ( rState.Stats.nFreeSlotCount[nChannel] <= 1u ) {
		const uint32_t nVictimSlot = t3d_rw_vt_choose_victim( rState, nChannel );
		if ( nVictimSlot == T3D_RW_VT_INVALID_SLOT ) {
			++rState.Stats.nReservationFailureCount;
			return false;
		}
		nVictimPage = rState.Buffers.pPhysicalSlots[nChannel][nVictimSlot].nVirtualPage;
	}

	T3DVTPhysicalSlot &FreeSlot = rState.Buffers.pPhysicalSlots[nChannel][nFree];
	FreeSlot.nFlags |= T3D_VT_SLOT_PENDING;
	FreeSlot.nPendingVictimPage = nVictimPage;
	FreeSlot.nLastTouchFrame = pFrameId;
	rReservation.nVirtualPage = pVirtualPage;
	rReservation.nPhysicalSlot = nFree;
	rReservation.nVictimPage = nVictimPage;
	rReservation.nExpectedGeneration = Entry.nGeneration;
	rReservation.nChannel = ( uint16_t )nChannel;
	rReservation.nWantedMip = pWantedMip;
	++rState.Stats.nUploadReservationCount;
	return true;
}

bool t3d_render_world_vt_can_commit_upload_hot( const T3DVTResidencyState &pState, const T3DVTUploadReservation &pReservation ) {
	if ( !pState.bInitialized || pState.pDatabase == NULL || pReservation.nVirtualPage >= pState.pDatabase->pHeader->nPageCount
	        || pReservation.nChannel >= T3D_RW_VT_CHANNELS
	        || pReservation.nPhysicalSlot >= pState.Buffers.nPhysicalSlotCapacity[pReservation.nChannel] ) {
		return false;
	}
	const T3DVTPageFileRecord &NewPage = pState.pDatabase->pPages[pReservation.nVirtualPage];
	if ( NewPage.nChannel != pReservation.nChannel ) {
		return false;
	}
	const T3DVTPageTableEntry &NewEntry = pState.Buffers.pPageTable[pReservation.nVirtualPage];
	const T3DVTPhysicalSlot &NewSlot = pState.Buffers.pPhysicalSlots[pReservation.nChannel][pReservation.nPhysicalSlot];
	if ( ( NewEntry.nFlags & T3D_VT_PAGE_RESIDENT ) != 0u || NewEntry.nPhysicalSlot != T3D_RW_VT_INVALID_SLOT
	        || ( NewSlot.nFlags & T3D_VT_SLOT_PENDING ) == 0u || NewSlot.nVirtualPage != T3D_RW_VT_INVALID_PAGE
	        || NewEntry.nGeneration != pReservation.nExpectedGeneration || pState.Stats.nFreeSlotCount[pReservation.nChannel] == 0u ) {
		return false;
	}
	if ( pReservation.nVictimPage != T3D_RW_VT_INVALID_PAGE ) {
		if ( pReservation.nVictimPage >= pState.pDatabase->pHeader->nPageCount ) {
			return false;
		}
		const T3DVTPageFileRecord &VictimPage = pState.pDatabase->pPages[pReservation.nVictimPage];
		const T3DVTPageTableEntry &VictimEntry = pState.Buffers.pPageTable[pReservation.nVictimPage];
		if ( VictimPage.nChannel != pReservation.nChannel || ( VictimEntry.nFlags & T3D_VT_PAGE_RESIDENT ) == 0u
		        || VictimEntry.nPhysicalSlot >= pState.Buffers.nPhysicalSlotCapacity[pReservation.nChannel] ) {
			return false;
		}
		const T3DVTPhysicalSlot &VictimSlot = pState.Buffers.pPhysicalSlots[pReservation.nChannel][VictimEntry.nPhysicalSlot];
		if ( VictimSlot.nVirtualPage != pReservation.nVictimPage
		        || ( VictimSlot.nFlags & ( T3D_VT_SLOT_PENDING | T3D_VT_SLOT_RETIRED | T3D_VT_SLOT_PINNED ) ) != 0u ) {
			return false;
		}
	}
	return true;
}

bool t3d_render_world_vt_commit_upload_hot( T3DVTResidencyState &rState, const T3DVTUploadReservation &pReservation, uint32_t pFrameId ) {
	if ( !t3d_render_world_vt_can_commit_upload_hot( rState, pReservation ) ) {
		return false;
	}
	T3DVTPageTableEntry &NewEntry = rState.Buffers.pPageTable[pReservation.nVirtualPage];
	T3DVTPhysicalSlot &NewSlot = rState.Buffers.pPhysicalSlots[pReservation.nChannel][pReservation.nPhysicalSlot];
	T3DVTPhysicalSlot *pVictimSlot = NULL;
	if ( pReservation.nVictimPage != T3D_RW_VT_INVALID_PAGE ) {
		const T3DVTPageTableEntry &VictimEntry = rState.Buffers.pPageTable[pReservation.nVictimPage];
		pVictimSlot = &rState.Buffers.pPhysicalSlots[pReservation.nChannel][VictimEntry.nPhysicalSlot];
	}

	// The owner prevalidated this exact reservation before publishing the GPU
	// page-table entry. No owner-visible state may change between validation and
	// this commit, so the CPU publication below is a non-failing mutation phase.
	NewSlot.nVirtualPage = pReservation.nVirtualPage;
	NewSlot.nGeneration = NewEntry.nGeneration;
	NewSlot.nLastTouchFrame = pFrameId;
	NewSlot.nPendingVictimPage = T3D_RW_VT_INVALID_PAGE;
	NewSlot.nFlags = T3D_VT_SLOT_REFERENCED;
	NewSlot.nClockAge = 1u;
	NewEntry.nPhysicalSlot = pReservation.nPhysicalSlot;
	NewEntry.nResolvedMip = pReservation.nWantedMip;
	NewEntry.nFlags |= T3D_VT_PAGE_RESIDENT;
	--rState.Stats.nFreeSlotCount[pReservation.nChannel];
	++rState.Stats.nResidentCount[pReservation.nChannel];
	++rState.Stats.nUploadCommitCount;
	++rState.nPageTableGeneration;

	// The victim remains logically and physically resident until its GPU page-table
	// invalidation succeeds. RETIRED removes it from CLOCK/free-slot reuse only.
	if ( pVictimSlot != NULL ) {
		pVictimSlot->nFlags |= T3D_VT_SLOT_RETIRED;
		pVictimSlot->nPendingVictimPage = pReservation.nVictimPage;
	}
	return true;
}

bool t3d_render_world_vt_finalize_eviction_hot(
        T3DVTResidencyState &rState, uint32_t pChannel, uint32_t pPhysicalSlot, uint32_t pVictimPage ) {
	if ( !rState.bInitialized || pChannel >= T3D_RW_VT_CHANNELS || pPhysicalSlot >= rState.Buffers.nPhysicalSlotCapacity[pChannel] ) {
		return false;
	}
	if ( pVictimPage >= rState.pDatabase->pHeader->nPageCount ) {
		return false;
	}
	T3DVTPhysicalSlot &Slot = rState.Buffers.pPhysicalSlots[pChannel][pPhysicalSlot];
	T3DVTPageTableEntry &Entry = rState.Buffers.pPageTable[pVictimPage];
	if ( Slot.nVirtualPage != pVictimPage || ( Slot.nFlags & T3D_VT_SLOT_RETIRED ) == 0u ) {
		return false;
	}
	if ( ( Entry.nFlags & T3D_VT_PAGE_RESIDENT ) == 0u || Entry.nPhysicalSlot != pPhysicalSlot ) {
		return false;
	}

	Entry.nPhysicalSlot = T3D_RW_VT_INVALID_SLOT;
	Entry.nFlags &= ( uint16_t )~T3D_VT_PAGE_RESIDENT;
	Slot.nVirtualPage = T3D_RW_VT_INVALID_PAGE;
	Slot.nPendingVictimPage = T3D_RW_VT_INVALID_PAGE;
	Slot.nFlags = 0u;
	Slot.nClockAge = 0u;
	++rState.Stats.nFreeSlotCount[pChannel];
	if ( rState.Stats.nResidentCount[pChannel] == 0u ) {
		return false;
	}
	--rState.Stats.nResidentCount[pChannel];
	++rState.Stats.nEvictionCount;
	++rState.Stats.nRetireFinalizeCount;
	++rState.nPageTableGeneration;
	return true;
}

void t3d_render_world_vt_abort_upload_hot( T3DVTResidencyState &rState, const T3DVTUploadReservation &pReservation ) {
	if ( !rState.bInitialized || pReservation.nChannel >= T3D_RW_VT_CHANNELS
	        || pReservation.nPhysicalSlot >= rState.Buffers.nPhysicalSlotCapacity[pReservation.nChannel] ) {
		return;
	}
	T3DVTPhysicalSlot &Slot = rState.Buffers.pPhysicalSlots[pReservation.nChannel][pReservation.nPhysicalSlot];
	if ( Slot.nVirtualPage == T3D_RW_VT_INVALID_PAGE ) {
		Slot.nFlags &= ( uint16_t )~T3D_VT_SLOT_PENDING;
		Slot.nPendingVictimPage = T3D_RW_VT_INVALID_PAGE;
	}
}

bool t3d_render_world_vt_validate_residency_reference_debug( const T3DVTResidencyState &pState ) {
	if ( !pState.bInitialized || pState.pDatabase == NULL ) {
		return false;
	}
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNELS; ++c ) {
		uint32_t nFree = 0u;
		uint32_t nResident = 0u;
		uint32_t nRetired = 0u;
		for ( uint32_t s = 0u; s < pState.Buffers.nPhysicalSlotCapacity[c]; ++s ) {
			const T3DVTPhysicalSlot &Slot = pState.Buffers.pPhysicalSlots[c][s];
			if ( Slot.nVirtualPage == T3D_RW_VT_INVALID_PAGE ) {
				++nFree;
				continue;
			}
			if ( Slot.nVirtualPage >= pState.pDatabase->pHeader->nPageCount ) {
				return false;
			}
			const T3DVTPageTableEntry &Entry = pState.Buffers.pPageTable[Slot.nVirtualPage];
			if ( ( Entry.nFlags & T3D_VT_PAGE_RESIDENT ) == 0u || Entry.nPhysicalSlot != s ) {
				return false;
			}
			if ( ( Slot.nFlags & T3D_VT_SLOT_RETIRED ) != 0u ) {
				if ( Slot.nPendingVictimPage != Slot.nVirtualPage ) {
					return false;
				}
				++nRetired;
			}
			++nResident;
		}
		if ( nFree != pState.Stats.nFreeSlotCount[c] || nResident != pState.Stats.nResidentCount[c] ) {
			return false;
		}
		// A zero-free transitional state is legal only while an old resident is
		// RETIRED and awaiting GPU page-table invalidation. Once retirement
		// finalizes, the permanent upload spare must exist again.
		if ( pState.bNeutralPublished && nFree == 0u && nRetired == 0u ) {
			return false;
		}
	}
	return true;
}

bool t3d_render_world_vt_allocation_page_id(
        const T3DVTAllocationRecord &pAllocation, uint32_t pMip, uint32_t pPageX, uint32_t pPageY, uint32_t &rVirtualPage ) {
	if ( pMip >= pAllocation.nMipCount || pMip >= T3D_RW_VT_MAX_MIPS ) {
		return false;
	}
	uint32_t nWidth = pAllocation.nMip0WidthPages;
	uint32_t nHeight = pAllocation.nMip0HeightPages;
	uint64_t nOffset = 0u;
	for ( uint32_t i = 0u; i < pMip; ++i ) {
		nOffset += ( uint64_t )nWidth * nHeight;
		nWidth = nWidth > 1u ? ( nWidth + 1u ) >> 1u : 1u;
		nHeight = nHeight > 1u ? ( nHeight + 1u ) >> 1u : 1u;
	}
	if ( pPageX >= nWidth || pPageY >= nHeight ) {
		return false;
	}
	const uint64_t nPage = ( uint64_t )pAllocation.nBaseVirtualPage + nOffset + ( uint64_t )pPageY * nWidth + pPageX;
	if ( nPage >= ( uint64_t )pAllocation.nBaseVirtualPage + pAllocation.nPageCount || nPage > UINT32_MAX ) {
		return false;
	}
	rVirtualPage = ( uint32_t )nPage;
	return true;
}


bool t3d_render_world_vt_predict_page_rect_reference_debug( const T3DVTAllocationRecord &pAllocation,
        const float pUvMin[2],
        const float pUvMax[2],
        uint32_t pWantedMip,
        uint32_t pMaximumPages,
        T3DVTPredictedPageRect &rRect ) {
	memset( &rRect, 0, sizeof( rRect ) );
	if ( pUvMin == NULL || pUvMax == NULL || pAllocation.nPageCount == 0u || pAllocation.nMipCount == 0u
	        || pAllocation.nMip0WidthPages == 0u || pAllocation.nMip0HeightPages == 0u || pMaximumPages == 0u ) {
		return false;
	}
	if ( !isfinite( pUvMin[0] ) || !isfinite( pUvMin[1] ) || !isfinite( pUvMax[0] ) || !isfinite( pUvMax[1] ) ) {
		return false;
	}

	uint32_t nMip = pWantedMip < pAllocation.nMipCount ? pWantedMip : ( uint32_t )pAllocation.nMipCount - 1u;
	for ( ;; ) {
		uint32_t nWidth = pAllocation.nMip0WidthPages;
		uint32_t nHeight = pAllocation.nMip0HeightPages;
		for ( uint32_t m = 0u; m < nMip; ++m ) {
			nWidth = nWidth > 1u ? ( nWidth + 1u ) >> 1u : 1u;
			nHeight = nHeight > 1u ? ( nHeight + 1u ) >> 1u : 1u;
		}

		const float fMinU = pUvMin[0] < pUvMax[0] ? pUvMin[0] : pUvMax[0];
		const float fMaxU = pUvMin[0] > pUvMax[0] ? pUvMin[0] : pUvMax[0];
		const float fMinV = pUvMin[1] < pUvMax[1] ? pUvMin[1] : pUvMax[1];
		const float fMaxV = pUvMin[1] > pUvMax[1] ? pUvMin[1] : pUvMax[1];
		const bool bFullU = fMinU < 0.0f || fMaxU > 1.0f || fMaxU - fMinU >= 1.0f;
		const bool bFullV = fMinV < 0.0f || fMaxV > 1.0f || fMaxV - fMinV >= 1.0f;
		const float fClampedMinU = fMinU < 0.0f ? 0.0f : ( fMinU > 1.0f ? 1.0f : fMinU );
		const float fClampedMaxU = fMaxU < 0.0f ? 0.0f : ( fMaxU > 1.0f ? 1.0f : fMaxU );
		const float fClampedMinV = fMinV < 0.0f ? 0.0f : ( fMinV > 1.0f ? 1.0f : fMinV );
		const float fClampedMaxV = fMaxV < 0.0f ? 0.0f : ( fMaxV > 1.0f ? 1.0f : fMaxV );
		uint32_t nMinX = bFullU ? 0u : ( uint32_t )floorf( fClampedMinU * ( float )nWidth );
		uint32_t nMaxX = bFullU ? nWidth - 1u : ( uint32_t )floorf( fClampedMaxU * ( float )nWidth );
		uint32_t nMinY = bFullV ? 0u : ( uint32_t )floorf( fClampedMinV * ( float )nHeight );
		uint32_t nMaxY = bFullV ? nHeight - 1u : ( uint32_t )floorf( fClampedMaxV * ( float )nHeight );
		if ( nMinX >= nWidth ) {
			nMinX = nWidth - 1u;
		}
		if ( nMaxX >= nWidth ) {
			nMaxX = nWidth - 1u;
		}
		if ( nMinY >= nHeight ) {
			nMinY = nHeight - 1u;
		}
		if ( nMaxY >= nHeight ) {
			nMaxY = nHeight - 1u;
		}
		if ( nMinX > nMaxX ) {
			const uint32_t t = nMinX;
			nMinX = nMaxX;
			nMaxX = t;
		}
		if ( nMinY > nMaxY ) {
			const uint32_t t = nMinY;
			nMinY = nMaxY;
			nMaxY = t;
		}
		const uint64_t nPages = ( uint64_t )( nMaxX - nMinX + 1u ) * ( uint64_t )( nMaxY - nMinY + 1u );
		if ( nPages <= pMaximumPages || nMip + 1u >= pAllocation.nMipCount ) {
			rRect.nMip = nMip;
			rRect.nMinPageX = nMinX;
			rRect.nMinPageY = nMinY;
			rRect.nMaxPageX = nMaxX;
			rRect.nMaxPageY = nMaxY;
			rRect.nPageCount = ( uint32_t )nPages;
			return nPages <= 0xffffffffull;
		}
		++nMip;
	}
}
