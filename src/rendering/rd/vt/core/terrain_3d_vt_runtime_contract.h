// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_RUNTIME_CONTRACT_H
#define TERRAIN3D_VT_RUNTIME_CONTRACT_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include <stdint.h>


// Legacy channel-owned allocation ABI retained only for V1 pack compatibility.
// t3dVTAllocationRecordV1_s centralized in Terrain3DTypes.h.


// t3dVTPageTableEntry_s centralized in Terrain3DTypes.h.


// t3dVTGpuCounters_s centralized in Terrain3DTypes.h.


// t3dVTRuntimeRequest_s centralized in Terrain3DTypes.h.


static_assert( sizeof( t3dVTAllocationRecordV1_t ) == 32, "t3dVTAllocationRecordV1_t ABI mismatch" );
static_assert( sizeof( t3dVTPageTableEntry_t ) == 16, "t3dVTPageTableEntry_t ABI mismatch" );
static_assert( sizeof( t3dVTGpuCounters_t ) == 32, "t3dVTGpuCounters_t ABI mismatch" );
static_assert( sizeof( t3dVTRuntimeRequest_t ) == 8, "t3dVTRuntimeRequest_t ABI mismatch" );

#endif // TERRAIN3D_VT_RUNTIME_CONTRACT_H
