// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_RUNTIME_CONTRACT_H
#define TERRAIN3D_VT_RUNTIME_CONTRACT_H

#include <stdint.h>

static const uint32_t T3D_VT_INVALID_ID = 0xffffffffu;

// Legacy channel-owned allocation ABI retained only for V1 pack compatibility.
struct T3DVTAllocationRecordV1 {
	uint32_t nBaseVirtualPage;
	uint32_t nPageCount;
	uint32_t nMip0WidthPages;
	uint32_t nMip0HeightPages;
	uint16_t nMipCount;
	uint16_t nChannel;
	uint16_t nFlags;
	uint16_t nReserved0;
	uint32_t nRootPage;
	uint32_t nReserved1;
};

struct T3DVTPageTableEntry {
	uint32_t nPhysicalSlot;
	uint32_t nParentPage;
	uint32_t nGeneration;
	uint16_t nResolvedMip;
	uint16_t nFlags;
};

struct T3DVTGpuCounters {
	uint32_t nRequestCount;
	uint32_t nPredictedRequestCount;
	uint32_t nDedupRequestCount;
	uint32_t nHashOverflowCount;
	uint32_t nListOverflowCount;
	uint32_t nFragmentFaultCount;
	uint32_t nReadbackIssueCount;
	uint32_t nReserved;
};

struct T3DVTRequest {
	uint32_t nVirtualPage;
	uint16_t nWantedMip;
	uint16_t nPriority;
};

static_assert( sizeof( T3DVTAllocationRecordV1 ) == 32, "T3DVTAllocationRecordV1 ABI mismatch" );
static_assert( sizeof( T3DVTPageTableEntry ) == 16, "T3DVTPageTableEntry ABI mismatch" );
static_assert( sizeof( T3DVTGpuCounters ) == 32, "T3DVTGpuCounters ABI mismatch" );
static_assert( sizeof( T3DVTRequest ) == 8, "T3DVTRequest ABI mismatch" );

#endif // TERRAIN3D_VT_RUNTIME_CONTRACT_H
