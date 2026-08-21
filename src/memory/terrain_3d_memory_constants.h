// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Godot-independent native memory constants shared by allocator tests and runtime code.
#pragma once

#ifndef TERRAIN3D_MEMORY_CONSTANTS_H
#define TERRAIN3D_MEMORY_CONSTANTS_H

#include <cstddef>
#include <cstdint>

static constexpr size_t s_nArenaCachelineAlignment = 64u;
static constexpr size_t s_nArenaDefaultBaseAlignment = 64u;
static constexpr uint32_t s_nRuntimeAllocatorBudgetDivisor = 8u;
static constexpr size_t s_nRuntimeAllocatorMinBytes = 4u * 1024u * 1024u;
static constexpr size_t s_nRuntimeAllocatorMaxBytes = 64u * 1024u * 1024u;

#endif // TERRAIN3D_MEMORY_CONSTANTS_H
