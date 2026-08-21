// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Godot-independent native memory constants shared by allocator tests and runtime code.
#pragma once

#ifndef TERRAIN3D_MEMORY_CONSTANTS_H
#define TERRAIN3D_MEMORY_CONSTANTS_H

#include <cstddef>
#include <cstdint>

static constexpr size_t TERRAIN3D_ARENA_CACHELINE_ALIGNMENT = 64u;
static constexpr size_t TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT = 64u;
static constexpr uint32_t TERRAIN3D_RUNTIME_ALLOCATOR_BUDGET_DIVISOR = 8u;
static constexpr size_t TERRAIN3D_RUNTIME_ALLOCATOR_MIN_BYTES = 4u * 1024u * 1024u;
static constexpr size_t TERRAIN3D_RUNTIME_ALLOCATOR_MAX_BYTES = 64u * 1024u * 1024u;

#endif // TERRAIN3D_MEMORY_CONSTANTS_H
