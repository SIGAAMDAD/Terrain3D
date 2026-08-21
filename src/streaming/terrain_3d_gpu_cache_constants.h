// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_GPU_CACHE_CONSTANTS_H
#define TERRAIN3D_GPU_CACHE_CONSTANTS_H

#include <cstddef>
#include <cstdint>

typedef enum t3dGpuRegionSlotState_e : uint8_t {
	TERRAIN3D_GPU_SLOT_FREE = 0u,
	TERRAIN3D_GPU_SLOT_STAGING = 1u,
	TERRAIN3D_GPU_SLOT_READY = 2u,
	TERRAIN3D_GPU_SLOT_MAPPED = 3u,
	TERRAIN3D_GPU_SLOT_RETIRED = 4u,
} t3dGpuRegionSlotState_t;

static constexpr int s_nGpuCacheDefaultGuardRegions = 1;
static constexpr int s_nGpuCacheDefaultHysteresisReserve = 4;
static constexpr int s_nGpuCacheDefaultHysteresisTicks = 30;
static constexpr int s_nGpuCacheDefaultPrefetchRows = 1;
static constexpr int s_nGpuCacheDefaultRetireFrames = 4;
static constexpr int s_nGpuCacheDefaultSpareLayers = 12;
static constexpr int s_nGpuCacheFarHeightSide = 512;
static constexpr int s_nGpuCacheRuntimeMaxLayers = 64;
static constexpr int s_nGpuCacheEditorMaxLayers = 256;
// Storage ceiling. Runtime remains capped at 64; Authoring may pin larger supplied worlds.
static constexpr int s_nGpuCacheMaxLayers = s_nGpuCacheEditorMaxLayers;
static constexpr size_t s_nGpuCacheMaxPageTableBytes = 64u * 64u * 3u;
static constexpr size_t s_nGpuCacheMib = 1024u * 1024u;
static constexpr int s_nGpuCacheMinSpareLayers = 2;

#endif // TERRAIN3D_GPU_CACHE_CONSTANTS_H
