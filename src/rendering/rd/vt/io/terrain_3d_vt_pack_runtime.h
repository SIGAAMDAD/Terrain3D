// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_PACK_RUNTIME_H
#define TERRAIN3D_VT_PACK_RUNTIME_H

#include "Terrain3DTypes.h"

#include "authoring/vt/io/terrain_3d_vt_pack_writer.h"
#include "rendering/rd/vt/streaming/terrain_3d_vt_residency.h"

#include <stddef.h>
#include <stdint.h>

typedef void *( *t3dVTPackRuntimeAllocateFn_t )( void *pUser, size_t nBytes, size_t nAlignment );
typedef void ( *t3dVTPackRuntimeFreeFn_t )( void *pUser, void *pMemory );

// t3dVTPackRuntimeAllocator_s centralized in Terrain3DTypes.h.


// t3dVTQualityRuntimeStats_s centralized in Terrain3DTypes.h.


// t3dVTPackRuntimeStorage_s centralized in Terrain3DTypes.h.


void t3d_vt_pack_runtime_clear( t3dVTPackRuntimeStorage_t &rStorage );
bool t3d_vt_pack_runtime_open_cold(
        const void *pPackage, uint64_t nPackageBytes, const t3dVTPackRuntimeAllocator_t &rAllocator, t3dVTPackRuntimeStorage_t &rStorage );
void t3d_vt_pack_runtime_release_cold( const t3dVTPackRuntimeAllocator_t &rAllocator, t3dVTPackRuntimeStorage_t &rStorage );
bool t3d_vt_pack_runtime_find_allocation_by_stable_key(
        const t3dVTPackRuntimeStorage_t &rStorage, uint64_t nStableKey, uint32_t &rAllocationId );
bool t3d_vt_pack_runtime_apply_quality_profile_v3( const t3dVTDatabaseView_t &rDatabase,
        t3dVTAllocation_t *pRuntimeAllocations, uint32_t nRuntimeAllocationCount, uint32_t nRequestedProfile,
        t3dVTQualityRuntimeStats_t &rStats );
bool t3d_vt_pack_runtime_copy_tail_mip_chain( const t3dVTDatabaseView_t &rDatabase,
        uint32_t nRuntimeTailIndex,
        uint32_t nChannel,
        uint8_t *pDestination,
        uint64_t nDestinationBytes,
        uint32_t nDestinationWidth,
        uint32_t nDestinationHeight );

#endif // TERRAIN3D_VT_PACK_RUNTIME_H
