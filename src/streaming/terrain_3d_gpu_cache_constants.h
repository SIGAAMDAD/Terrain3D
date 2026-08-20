// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_GPU_CACHE_CONSTANTS_H
#define TERRAIN3D_GPU_CACHE_CONSTANTS_H

#include <cstddef>
#include <cstdint>

typedef enum t3dgpuRegionSlotState_e : uint8_t {
	TERRAIN3D_GPU_SLOT_FREE = 0u,
	TERRAIN3D_GPU_SLOT_STAGING = 1u,
	TERRAIN3D_GPU_SLOT_READY = 2u,
	TERRAIN3D_GPU_SLOT_MAPPED = 3u,
	TERRAIN3D_GPU_SLOT_RETIRED = 4u,
} t3dgpuRegionSlotState_t;


static constexpr int TERRAIN3D_GPU_CACHE_DEFAULT_GUARD_REGIONS = 1;
static constexpr int TERRAIN3D_GPU_CACHE_DEFAULT_HYSTERESIS_RESERVE = 4;
static constexpr int TERRAIN3D_GPU_CACHE_DEFAULT_HYSTERESIS_TICKS = 30;
static constexpr int TERRAIN3D_GPU_CACHE_DEFAULT_PREFETCH_ROWS = 1;
static constexpr int TERRAIN3D_GPU_CACHE_DEFAULT_RETIRE_FRAMES = 4;
static constexpr int TERRAIN3D_GPU_CACHE_DEFAULT_SPARE_LAYERS = 12;
static constexpr int TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE = 512;
static constexpr int TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS = 64;
static constexpr int TERRAIN3D_GPU_CACHE_EDITOR_MAX_LAYERS = 256;
// Storage ceiling. Runtime remains capped at 64; Authoring may pin larger supplied worlds.
static constexpr int TERRAIN3D_GPU_CACHE_MAX_LAYERS = TERRAIN3D_GPU_CACHE_EDITOR_MAX_LAYERS;
static constexpr size_t TERRAIN3D_GPU_CACHE_MAX_PAGE_TABLE_BYTES = 64u * 64u * 3u;
static constexpr size_t TERRAIN3D_GPU_CACHE_MIB = 1024u * 1024u;
static constexpr int TERRAIN3D_GPU_CACHE_MIN_SPARE_LAYERS = 2;

#endif // TERRAIN3D_GPU_CACHE_CONSTANTS_H
