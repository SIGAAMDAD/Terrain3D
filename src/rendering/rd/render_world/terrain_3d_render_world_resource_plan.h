// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_RESOURCE_PLAN_H
#define TERRAIN3D_RENDER_WORLD_RESOURCE_PLAN_H

#include "rendering/rd/render_world/terrain_3d_render_world_contract.h"

#include <stdint.h>

static const uint32_t T3D_RW_GPU_FRAME_RING_COUNT = 3u;
static const uint32_t T3D_RW_VT_CHANNEL_COUNT = 4u;
static const uint32_t T3D_RW_MAX_VT_TEXTURE_BANKS = 16u;

// Shared indirect buffer layout. Raster draw args occupy the first three 16-byte
// records. Compute dispatch triplets follow at 4-byte aligned offsets.
static const uint32_t T3D_RW_INDIRECT_DRAW_WORLD_OFFSET = 0u;
static const uint32_t T3D_RW_INDIRECT_DRAW_CLUTTER_OFFSET = 16u;
static const uint32_t T3D_RW_INDIRECT_DRAW_FOLIAGE_OFFSET = 32u;
static const uint32_t T3D_RW_INDIRECT_VIS_INSTANCE_OFFSET = 48u;
static const uint32_t T3D_RW_INDIRECT_VIS_CLUSTER_OFFSET = 60u;
static const uint32_t T3D_RW_INDIRECT_VT_WORLD_OFFSET = 72u;
static const uint32_t T3D_RW_INDIRECT_VT_CLUTTER_OFFSET = 84u;
static const uint32_t T3D_RW_INDIRECT_VT_FOLIAGE_OFFSET = 96u;
static const uint32_t T3D_RW_INDIRECT_BUFFER_BYTES = 108u;

enum T3DRenderWorldGpuBufferId : uint32_t {
	T3D_RW_GPU_BUFFER_GEOMETRY_VERTEX = 0,
	T3D_RW_GPU_BUFFER_CLUSTER_VERTEX_INDEX,
	T3D_RW_GPU_BUFFER_CLUSTER_TRIANGLE,
	T3D_RW_GPU_BUFFER_MESH_RECORDS,
	T3D_RW_GPU_BUFFER_MESH_LOD_RECORDS,
	T3D_RW_GPU_BUFFER_CLUSTER_RECORDS,
	T3D_RW_GPU_BUFFER_INSTANCE_RECORDS,
	T3D_RW_GPU_BUFFER_CELL_RECORDS,
	T3D_RW_GPU_BUFFER_MATERIAL_RECORDS,
	T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE,
	T3D_RW_GPU_BUFFER_VT_PAGE_TABLE,
	T3D_RW_GPU_BUFFER_VISIBLE_CELLS,
	T3D_RW_GPU_BUFFER_VISIBLE_INSTANCES,
	T3D_RW_GPU_BUFFER_VISIBLE_WORLD_CLUSTERS,
	T3D_RW_GPU_BUFFER_VISIBLE_CLUTTER_CLUSTERS,
	T3D_RW_GPU_BUFFER_VISIBLE_FOLIAGE_CLUSTERS,
	T3D_RW_GPU_BUFFER_SELECTED_LOD,
	T3D_RW_GPU_BUFFER_COUNTERS,
	T3D_RW_GPU_BUFFER_INDIRECT_ARGS,
	T3D_RW_GPU_BUFFER_VT_REQUEST_HASH,
	T3D_RW_GPU_BUFFER_VT_REQUEST_LIST,
	T3D_RW_GPU_BUFFER_UPLOAD_RING,
	T3D_RW_GPU_BUFFER_READBACK_RING,
	T3D_RW_GPU_BUFFER_VISIBILITY_FRAME,
	T3D_RW_GPU_BUFFER_VISIBILITY_HISTORY,
	T3D_RW_GPU_BUFFER_COUNT
};

enum T3DRenderWorldVTChannel : uint32_t {
	T3D_RW_VT_ALBEDO = 0,
	T3D_RW_VT_NORMAL,
	T3D_RW_VT_SURFACE,
	T3D_RW_VT_HEIGHT,
	T3D_RW_VT_CHANNEL_COUNT_ENUM
};

struct T3DRenderWorldGpuProfile {
	uint32_t nMeshLodRecordBytes;
	uint32_t nVtAllocationRecordBytes;
	uint32_t nVtPageTableEntryBytes;
	uint32_t nVtRequestHashEntryBytes;
	uint32_t nGpuCounterBytes;
	uint32_t nVtPhysicalTileSize;
	uint32_t nHiZWidth;
	uint32_t nHiZHeight;
	uint32_t nHiZMipCount;
	uint32_t nReserved[7];
};


struct T3DRenderWorldVTBankLayout {
	uint32_t nBankCount;
	uint32_t nLayers[T3D_RW_MAX_VT_TEXTURE_BANKS];
};

struct T3DRenderWorldGpuResourcePlan {
	uint64_t nBufferBytes[T3D_RW_GPU_BUFFER_COUNT];
	uint64_t nVtPhysicalBytes[T3D_RW_VT_CHANNEL_COUNT];
	uint64_t nHiZBytes;
	uint64_t nTotalBufferBytes;
	uint64_t nTotalTextureBytes;
	uint64_t nTotalBytes;
};

T3DRenderWorldGpuProfile t3d_render_world_make_development_gpu_profile();
bool t3d_render_world_validate_gpu_profile( const T3DRenderWorldGpuProfile &pProfile );
bool t3d_render_world_compute_vt_bank_layout( uint32_t pPageCount, uint32_t pMaxArrayLayers, T3DRenderWorldVTBankLayout &rLayout );
bool t3d_render_world_compute_gpu_resource_plan(
        const T3DRenderWorldLimits &pLimits, const T3DRenderWorldGpuProfile &pProfile, T3DRenderWorldGpuResourcePlan &rPlan );

#endif // TERRAIN3D_RENDER_WORLD_RESOURCE_PLAN_H
