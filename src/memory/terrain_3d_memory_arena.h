// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Fixed native memory image and deterministic low/high layout planner.
#ifndef TERRAIN3D_MEMORY_ARENA_H
#define TERRAIN3D_MEMORY_ARENA_H

#include "core/terrain_3d_compiler.h"
#include "memory/terrain_3d_memory_constants.h"

#include <stddef.h>
#include <stdint.h>

typedef struct t3dMemoryArenaSlice_s {
	size_t m_iOffset = 0u;
	size_t m_nSize = 0u;
	size_t m_nAlignment = 1u;
	bool m_bReserved = false;

	EA_FORCE_INLINE bool valid( void ) const {
		return m_bReserved;
	}
} t3dMemoryArenaSlice_t;

typedef struct t3dMemoryArenaSpan_s {
	uint8_t *m_pData = NULL;
	size_t m_nSize = 0u;

	EA_FORCE_INLINE bool valid( void ) const {
		return m_pData != NULL || m_nSize == 0u;
	}
} t3dMemoryArenaSpan_t;

class Terrain3DMemoryArenaLayout {
public:
	Terrain3DMemoryArenaLayout( void ) = default;
	explicit Terrain3DMemoryArenaLayout( size_t nCapacityBytes );

	bool configure( size_t nCapacityBytes );
	bool reserveLow( size_t nBytes, size_t nAlignment, t3dMemoryArenaSlice_t &rSlice );
	bool seal( void );

	EA_FORCE_INLINE size_t capacity( void ) const {
		return m_nCapacity;
	}
	EA_FORCE_INLINE size_t lowUsed( void ) const {
		return m_nLowCursor;
	}
	EA_FORCE_INLINE size_t remaining( void ) const {
		return m_nCapacity >= m_nLowCursor ? m_nCapacity - m_nLowCursor : 0u;
	}
	EA_FORCE_INLINE size_t maxAlignment( void ) const {
		return m_nMaxAlignment;
	}
	EA_FORCE_INLINE bool valid( void ) const {
		return m_bValid;
	}
	EA_FORCE_INLINE bool sealed( void ) const {
		return m_bSealed;
	}

private:
	size_t m_nCapacity = 0u;
	size_t m_nLowCursor = 0u;
	size_t m_nMaxAlignment = s_nArenaDefaultBaseAlignment;
	bool m_bValid = true;
	bool m_bSealed = false;

	static bool isPowerOfTwo( size_t nValue );
	static bool alignUp( size_t nValue, size_t nAlignment, size_t &rAligned );
};

class Terrain3DMemoryArena {
public:
	Terrain3DMemoryArena( void ) = default;
	~Terrain3DMemoryArena( void );
	Terrain3DMemoryArena( const Terrain3DMemoryArena & ) = delete;
	Terrain3DMemoryArena &operator=( const Terrain3DMemoryArena & ) = delete;
	Terrain3DMemoryArena( Terrain3DMemoryArena &&rOther ) noexcept;
	Terrain3DMemoryArena &operator=( Terrain3DMemoryArena &&rOther ) noexcept;

	bool initialize( const Terrain3DMemoryArenaLayout &rLayout );
	bool commit( void );
	bool seal( void );
	void release( void );
	void swap( Terrain3DMemoryArena &rOther ) noexcept;

	t3dMemoryArenaSpan_t resolve( const t3dMemoryArenaSlice_t &rSlice );
	t3dMemoryArenaSpan_t resolve( const t3dMemoryArenaSlice_t &rSlice ) const;

	EA_FORCE_INLINE size_t capacity( void ) const {
		return m_nCapacity;
	}
	EA_FORCE_INLINE size_t baseAlignment( void ) const {
		return m_nBaseAlignment;
	}
	EA_FORCE_INLINE bool initialized( void ) const {
		return m_bInitialized;
	}
	EA_FORCE_INLINE bool committed( void ) const {
		return m_bCommitted;
	}
	EA_FORCE_INLINE bool sealed( void ) const {
		return m_bSealed;
	}

private:
	void *m_pRaw = NULL;
	uint8_t *m_pBase = NULL;
	size_t m_nCapacity = 0u;
	size_t m_nBaseAlignment = s_nArenaDefaultBaseAlignment;
	bool m_bInitialized = false;
	bool m_bCommitted = false;
	bool m_bSealed = false;

	bool sliceInBounds( const t3dMemoryArenaSlice_t &rSlice ) const;
};

#endif // TERRAIN3D_MEMORY_ARENA_H
