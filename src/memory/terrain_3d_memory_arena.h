// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Fixed native memory image and deterministic low/high layout planner.
#pragma once

#ifndef TERRAIN3D_MEMORY_ARENA_H
#define TERRAIN3D_MEMORY_ARENA_H

#include "core/terrain_3d_compiler.h"
#include "memory/terrain_3d_memory_constants.h"

#include <cstddef>
#include <cstdint>
#include <limits>


typedef struct t3dMemoryArenaSlice_s {
	size_t offset = 0u;
	size_t size = 0u;
	size_t alignment = 1u;
	bool high = false;
	bool reserved = false;

	EA_FORCE_INLINE bool valid( void ) const { return reserved; }
} t3dMemoryArenaSlice_t;

typedef struct t3dMemoryArenaSpan_s {
	uint8_t *data = nullptr;
	size_t size = 0u;

	EA_FORCE_INLINE bool valid( void ) const { return data != nullptr || size == 0u; }
} t3dMemoryArenaSpan_t;

class T3DMemoryArenaLayout {
public:
	T3DMemoryArenaLayout( void ) = default;
	explicit T3DMemoryArenaLayout( size_t nCapacityBytes );

	bool configure( size_t nCapacityBytes );
	bool reserveLow( size_t nBytes, size_t nAlignment, t3dMemoryArenaSlice_t &rSlice );
	bool seal( void );

	EA_FORCE_INLINE size_t capacity( void ) const { return m_nCapacity; }
	EA_FORCE_INLINE size_t lowUsed( void ) const { return m_nLowCursor; }
	EA_FORCE_INLINE size_t highUsed( void ) const { return m_nCapacity - m_nHighCursor; }
	EA_FORCE_INLINE size_t remaining( void ) const { return m_nHighCursor >= m_nLowCursor ? m_nHighCursor - m_nLowCursor : 0u; }
	EA_FORCE_INLINE size_t maxAlignment( void ) const { return m_nMaxAlignment; }
	EA_FORCE_INLINE bool valid( void ) const { return m_bValid; }
	EA_FORCE_INLINE bool sealed( void ) const { return m_bSealed; }

private:
	size_t m_nCapacity = 0u;
	size_t m_nLowCursor = 0u;
	size_t m_nHighCursor = 0u;
	size_t m_nMaxAlignment = TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT;
	bool m_bValid = true;
	bool m_bSealed = false;

	static bool isPowerOfTwo( size_t nValue );
	static bool alignUp( size_t nValue, size_t nAlignment, size_t &rAligned );
	static size_t alignDown( size_t nValue, size_t nAlignment );
	bool reserve( bool bHigh, size_t nBytes, size_t nAlignment, t3dMemoryArenaSlice_t &rSlice );
};

class T3DMemoryArena {
public:
	T3DMemoryArena( void ) = default;
	~T3DMemoryArena( void );
	T3DMemoryArena( const T3DMemoryArena & ) = delete;
	T3DMemoryArena &operator=( const T3DMemoryArena & ) = delete;

	bool initialize( const T3DMemoryArenaLayout &rLayout );
	bool commit( void );
	bool seal( void );
	void release( void );

	t3dMemoryArenaSpan_t resolve( const t3dMemoryArenaSlice_t &rSlice );
	t3dMemoryArenaSpan_t resolve( const t3dMemoryArenaSlice_t &rSlice ) const;

	EA_FORCE_INLINE size_t capacity( void ) const { return m_nCapacity; }
	EA_FORCE_INLINE size_t baseAlignment( void ) const { return m_nBaseAlignment; }
	EA_FORCE_INLINE bool initialized( void ) const { return m_bInitialized; }
	EA_FORCE_INLINE bool committed( void ) const { return m_bCommitted; }
	EA_FORCE_INLINE bool sealed( void ) const { return m_bSealed; }

private:
	void *m_pRaw = nullptr;
	uint8_t *m_pBase = nullptr;
	size_t m_nCapacity = 0u;
	size_t m_nBaseAlignment = TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT;
	bool m_bInitialized = false;
	bool m_bCommitted = false;
	bool m_bSealed = false;

	bool sliceInBounds( const t3dMemoryArenaSlice_t &rSlice ) const;
};

#endif // TERRAIN3D_MEMORY_ARENA_H
