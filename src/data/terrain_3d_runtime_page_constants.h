// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_RUNTIME_PAGE_CONSTANTS_H
#define TERRAIN3D_RUNTIME_PAGE_CONSTANTS_H

#include <cstdint>

static constexpr uint32_t TERRAIN3D_RUNTIME_PAGE_ALIGNMENT = 16u;
static constexpr uint32_t TERRAIN3D_RUNTIME_PAGE_BLEND_RECIPE_VERSION = 4u;
static constexpr uint32_t TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT = 16u;
static constexpr uint32_t TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES = 864u;
static constexpr uint32_t TERRAIN3D_RUNTIME_PAGE_MAGIC = 0x47503354u;
static constexpr uint32_t TERRAIN3D_RUNTIME_PAGE_MAX_BYTES = 256u * 1024u * 1024u;
static constexpr uint32_t TERRAIN3D_RUNTIME_PAGE_VERSION = 5u;

#endif // TERRAIN3D_RUNTIME_PAGE_CONSTANTS_H
