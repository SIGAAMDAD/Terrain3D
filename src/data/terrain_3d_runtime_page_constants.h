// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_RUNTIME_PAGE_CONSTANTS_H
#define TERRAIN3D_RUNTIME_PAGE_CONSTANTS_H

#include <cstdint>

static constexpr uint32_t s_nRuntimePageAlignment = 16u;
static constexpr uint32_t s_nRuntimePageBlendRecipeVersion = 4u;
static constexpr uint32_t s_nRuntimePageChannelCount = 24u;
static constexpr uint32_t s_nRuntimePageHeaderBytes = 1248u;
static constexpr uint32_t s_nRuntimePageMagic = 0x47503354u;
static constexpr uint32_t s_nRuntimePageMaxBytes = 256u * 1024u * 1024u;
static constexpr uint32_t s_nRuntimePageVersion = 6u;

#endif // TERRAIN3D_RUNTIME_PAGE_CONSTANTS_H
