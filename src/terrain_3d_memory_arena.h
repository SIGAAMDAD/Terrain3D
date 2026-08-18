// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Fixed native memory image and deterministic low/high layout planner.
#pragma once

#ifndef TERRAIN3D_MEMORY_ARENA_H
#define TERRAIN3D_MEMORY_ARENA_H

#include "terrain_3d_memory_constants.h"

#include <cstddef>
#include <cstdint>
#include <limits>


typedef struct t3dmemoryArenaSlice_s {
	size_t offset = 0u;
	size_t size = 0u;
	size_t alignment = 1u;
	bool high = false;
	bool reserved = false;

	bool valid( void ) const {
		return reserved;
	}
} t3dmemoryArenaSlice_t;

typedef struct t3dmemoryArenaSpan_s {
	uint8_t *data = nullptr;
	size_t size = 0u;

	bool valid( void ) const {
		return data != nullptr || size == 0u;
	}
} t3dmemoryArenaSpan_t;

class T3DMemoryArenaLayout {
public:
	T3DMemoryArenaLayout( void ) = default;
	explicit T3DMemoryArenaLayout( size_t nCapacityBytes );

	bool configure( size_t nCapacityBytes );
	bool reserveLow( size_t nBytes, size_t nAlignment, t3dmemoryArenaSlice_t &rSlice );
	bool reserveHigh( size_t nBytes, size_t nAlignment, t3dmemoryArenaSlice_t &rSlice );
	bool seal( void );

	size_t capacity( void ) const {
		return m_nCapacity;
	}
	size_t lowUsed( void ) const {
		return m_nLowCursor;
	}
	size_t highUsed( void ) const {
		return m_nCapacity - m_nHighCursor;
	}
	size_t remaining( void ) const {
		return m_nHighCursor >= m_nLowCursor ? m_nHighCursor - m_nLowCursor : 0u;
	}
	size_t maxAlignment( void ) const {
		return m_nMaxAlignment;
	}
	bool valid( void ) const {
		return m_bValid;
	}
	bool sealed( void ) const {
		return m_bSealed;
	}
	bool contains( const t3dmemoryArenaSlice_t &rSlice ) const;

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
	bool reserve( bool bHigh, size_t nBytes, size_t nAlignment, t3dmemoryArenaSlice_t &rSlice );
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

	t3dmemoryArenaSpan_t resolve( const t3dmemoryArenaSlice_t &rSlice );
	t3dmemoryArenaSpan_t resolve( const t3dmemoryArenaSlice_t &rSlice ) const;

	size_t capacity( void ) const {
		return m_nCapacity;
	}
	size_t baseAlignment( void ) const {
		return m_nBaseAlignment;
	}
	bool initialized( void ) const {
		return m_bInitialized;
	}
	bool committed( void ) const {
		return m_bCommitted;
	}
	bool sealed( void ) const {
		return m_bSealed;
	}

private:
	void *m_pRaw = nullptr;
	uint8_t *m_pBase = nullptr;
	size_t m_nCapacity = 0u;
	size_t m_nBaseAlignment = TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT;
	bool m_bInitialized = false;
	bool m_bCommitted = false;
	bool m_bSealed = false;

	bool sliceInBounds( const t3dmemoryArenaSlice_t &rSlice ) const;
};

#endif // TERRAIN3D_MEMORY_ARENA_H
