// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_PACK_RUNTIME_H
#define TERRAIN3D_VT_PACK_RUNTIME_H

#include "authoring/vt/terrain_3d_vt_pack_writer.h"
#include "rendering/rd/vt/terrain_3d_vt_residency.h"

#include <stddef.h>
#include <stdint.h>

typedef void *( *T3DVTPackRuntimeAllocateFn )( void *pUser, size_t nBytes, size_t nAlignment );
typedef void ( *T3DVTPackRuntimeFreeFn )( void *pUser, void *pMemory );

struct T3DVTPackRuntimeAllocator {
	void *pUser;
	T3DVTPackRuntimeAllocateFn pAllocate;
	T3DVTPackRuntimeFreeFn pFree;
};

struct T3DVTQualityRuntimeStats {
	uint32_t nRequestedProfile;
	uint32_t nPackageFullySupportedProfileMask;
	uint32_t nFallbackAllocationCount;
	uint32_t nAllocationsByHighestProfile[T3D_VT_QUALITY_PROFILE_COUNT];
	uint32_t nAllocationsByResolvedProfile[T3D_VT_QUALITY_PROFILE_COUNT];
};

struct T3DVTPackRuntimeStorage {
	void *pOwnedBlock;
	uint64_t nOwnedBytes;
	uint8_t *pPackageBytes;
	uint64_t nPackageBytes;
	t3dVTPackViewV3_t PackViewV3;
	T3DVTDatabaseView Database;
	T3DVTFileHeader *pRuntimeHeader;
	T3DVTAllocationRecordV1 *pLegacyAllocations;
	T3DVTPageFileRecord *pLegacyPages;
	T3DVTPayloadRecord *pLegacyPayloads;
	t3dVTAllocation_t *pRuntimeAllocations;
	T3DVTRuntimePayloadRecord *pRuntimePayloads;
	t3dVTTailMetadata_t *pRuntimeTails;
	T3DVTRuntimeTailSourceRecord *pRuntimeTailSources;
};

void t3d_vt_pack_runtime_clear( T3DVTPackRuntimeStorage &rStorage );
bool t3d_vt_pack_runtime_open_cold(
        const void *pPackage, uint64_t nPackageBytes, const T3DVTPackRuntimeAllocator &rAllocator, T3DVTPackRuntimeStorage &rStorage );
void t3d_vt_pack_runtime_release_cold( const T3DVTPackRuntimeAllocator &rAllocator, T3DVTPackRuntimeStorage &rStorage );
bool t3d_vt_pack_runtime_find_allocation_by_stable_key(
        const T3DVTPackRuntimeStorage &rStorage, uint64_t nStableKey, uint32_t &rAllocationId );
bool t3d_vt_pack_runtime_apply_quality_profile_v3( const T3DVTDatabaseView &rDatabase,
        t3dVTAllocation_t *pRuntimeAllocations, uint32_t nRuntimeAllocationCount, uint32_t nRequestedProfile,
        T3DVTQualityRuntimeStats &rStats );
bool t3d_vt_pack_runtime_copy_tail_mip_chain( const T3DVTDatabaseView &rDatabase,
        uint32_t nRuntimeTailIndex,
        uint32_t nChannel,
        uint8_t *pDestination,
        uint64_t nDestinationBytes,
        uint32_t nDestinationWidth,
        uint32_t nDestinationHeight );

#endif // TERRAIN3D_VT_PACK_RUNTIME_H
