// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_RESOURCE_PLAN_H
#define TERRAIN3D_VT_RESOURCE_PLAN_H

#include "rendering/rd/render_world/terrain_3d_render_world_contract.h"

#include <stdint.h>

static const uint32_t T3D_RW_GPU_FRAME_RING_COUNT = 3u;
static const uint32_t T3D_RW_VT_CHANNEL_COUNT = 4u;
// GPU-P04: production VT cache is specialized to one sampler2DArray bank per channel.
// Physical-slot ABI remains bank:layer; bank is required to be zero in this specialization.
static const uint32_t T3D_RW_MAX_VT_TEXTURE_BANKS = 1u;

enum T3DVtGpuBufferId : uint32_t {
	T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE = 0,
	T3D_RW_GPU_BUFFER_VT_PAGE_TABLE,
	T3D_RW_GPU_BUFFER_VT_MIP_LAYOUT_TABLE,
	T3D_RW_GPU_BUFFER_VT_TAIL_METADATA,
	T3D_RW_GPU_BUFFER_COUNT
};

enum T3DVtChannel : uint32_t {
	T3D_RW_VT_ALBEDO = 0,
	T3D_RW_VT_NORMAL,
	T3D_RW_VT_SURFACE,
	T3D_RW_VT_HEIGHT,
	T3D_RW_VT_CHANNEL_COUNT_ENUM
};

struct T3DVtGpuProfile {
	uint32_t nVtAllocationRecordBytes;
	uint32_t nVtPageTableEntryBytes;
	uint32_t nVtPhysicalTileSize;
};

struct T3DVtBankLayout {
	uint32_t nBankCount;
	uint32_t nLayers[T3D_RW_MAX_VT_TEXTURE_BANKS];
};

struct T3DVtGpuResourcePlan {
	uint64_t nBufferBytes[T3D_RW_GPU_BUFFER_COUNT];
	uint64_t nVtPhysicalBytes[T3D_RW_VT_CHANNEL_COUNT];
	uint64_t nVtTailBytes[T3D_RW_VT_CHANNEL_COUNT];
	uint32_t nVtTailAtlasWidth;
	uint32_t nVtTailAtlasHeight;
	uint32_t nVtTailEntryCapacity;
	uint32_t nReserved0;
	uint64_t nTotalBufferBytes;
	uint64_t nTotalTextureBytes;
	uint64_t nTotalBytes;
};

T3DVtGpuProfile t3d_vt_make_development_gpu_profile();
T3DVtGpuProfile t3d_vt_make_runtime_gpu_profile( const t3dWorldResourceLimits_t &rWorldLimits );
T3DVtGpuProfile t3d_vt_make_preview_gpu_profile();
bool t3d_vt_validate_gpu_profile( const T3DVtGpuProfile &pProfile );
bool t3d_vt_compute_bank_layout( uint32_t pPageCount, uint32_t pMaxArrayLayers, T3DVtBankLayout &rLayout );
bool t3d_vt_compute_resource_plan(
        const T3DRenderWorldLimits &pLimits, const T3DVtGpuProfile &pProfile, T3DVtGpuResourcePlan &rPlan );

#endif // TERRAIN3D_VT_RESOURCE_PLAN_H
