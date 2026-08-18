// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "memory/terrain_3d_memory_arena.h"

#include "memory/terrain_3d_memory.h"

#include <algorithm>
#include <cstring>

T3DMemoryArenaLayout::T3DMemoryArenaLayout( const size_t nCapacityBytes ) {
	configure( nCapacityBytes );
}

bool T3DMemoryArenaLayout::isPowerOfTwo( const size_t nValue ) {
	return nValue != 0u && ( nValue & ( nValue - 1u ) ) == 0u;
}

bool T3DMemoryArenaLayout::alignUp( const size_t nValue, const size_t nAlignment, size_t &rAligned ) {
	if ( !isPowerOfTwo( nAlignment ) ) {
		return false;
	}
	const size_t mask = nAlignment - 1u;
	if ( nValue > std::numeric_limits<size_t>::max() - mask ) {
		return false;
	}
	rAligned = ( nValue + mask ) & ~mask;
	return true;
}

size_t T3DMemoryArenaLayout::alignDown( const size_t nValue, const size_t nAlignment ) {
	return nValue & ~( nAlignment - 1u );
}

bool T3DMemoryArenaLayout::configure( const size_t nCapacityBytes ) {
	m_nCapacity = nCapacityBytes;
	m_nLowCursor = 0u;
	m_nHighCursor = nCapacityBytes;
	m_nMaxAlignment = TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT;
	m_bValid = true;
	m_bSealed = false;
	return true;
}

bool T3DMemoryArenaLayout::reserveLow(
	const size_t nBytes, const size_t nAlignment,
	t3dmemoryArenaSlice_t &rSlice
) {
	return reserve( false, nBytes, nAlignment, rSlice );
}

bool T3DMemoryArenaLayout::reserveHigh(
	const size_t nBytes, const size_t nAlignment,
	t3dmemoryArenaSlice_t &rSlice
) {
	return reserve( true, nBytes, nAlignment, rSlice );
}

bool T3DMemoryArenaLayout::reserve(
	const bool bHigh, const size_t nBytes, const size_t nAlignment,
	t3dmemoryArenaSlice_t &rSlice
) {
	if ( m_bSealed || !m_bValid ) {
		return false;
	}
	if ( !isPowerOfTwo( nAlignment ) ) {
		m_bValid = false;
		return false;
	}
	m_nMaxAlignment = std::max( m_nMaxAlignment, nAlignment );

	if ( !bHigh ) {
		size_t aligned = 0u;
		if ( !alignUp( m_nLowCursor, nAlignment, aligned ) || aligned > m_nHighCursor ||
			nBytes > m_nHighCursor - aligned ) {
			m_bValid = false;
			return false;
		}
		const t3dmemoryArenaSlice_t slice = {aligned, nBytes, nAlignment, false, true};
		rSlice = slice;
		m_nLowCursor = aligned + nBytes;
		return true;
	}

	if ( nBytes > m_nHighCursor ) {
		m_bValid = false;
		return false;
	}
	const size_t candidate = alignDown( m_nHighCursor - nBytes, nAlignment );
	if ( candidate < m_nLowCursor || nBytes > m_nHighCursor - candidate ) {
		m_bValid = false;
		return false;
	}
	const t3dmemoryArenaSlice_t slice = {candidate, nBytes, nAlignment, true, true};
	rSlice = slice;
	m_nHighCursor = candidate;
	return true;
}

bool T3DMemoryArenaLayout::seal( void ) {
	if ( !m_bValid ) {
		return false;
	}
	m_bSealed = true;
	return true;
}

bool T3DMemoryArenaLayout::contains( const t3dmemoryArenaSlice_t &rSlice ) const {
	if ( !rSlice.reserved || !isPowerOfTwo( rSlice.alignment ) || rSlice.offset % rSlice.alignment != 0u ||
		rSlice.offset > m_nCapacity ) {
		return false;
	}
	return rSlice.size <= m_nCapacity - rSlice.offset;
}

T3DMemoryArena::~T3DMemoryArena( void ) {
	release();
}

bool T3DMemoryArena::initialize( const T3DMemoryArenaLayout &rLayout ) {
	release();
	if ( !rLayout.valid() || !rLayout.sealed() ) {
		return false;
	}
	m_nCapacity = rLayout.capacity();
	m_nBaseAlignment = std::max( rLayout.maxAlignment(), TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT );
	if ( m_nCapacity == 0u ) {
		m_bInitialized = true;
		return true;
	}
	if ( m_nCapacity > std::numeric_limits<size_t>::max() - ( m_nBaseAlignment - 1u ) ) {
		release();
		return false;
	}
	m_pRaw = terrain3d_memory::allocate( m_nCapacity + m_nBaseAlignment - 1u );
	if ( !m_pRaw ) {
		release();
		return false;
	}
	const uintptr_t raw_address = ( uintptr_t ) ( m_pRaw );
	const uintptr_t mask = ( uintptr_t ) ( m_nBaseAlignment - 1u );
	m_pBase = ( uint8_t * ) ( ( raw_address + mask ) & ~mask );
	m_bInitialized = true;
	return true;
}

bool T3DMemoryArena::commit( void ) {
	if ( !initialized() || m_bSealed ) {
		return false;
	}
	if ( m_nCapacity > 0u ) {
		std::memset( m_pBase, 0, m_nCapacity );
	}
	m_bCommitted = true;
	return true;
}

bool T3DMemoryArena::seal( void ) {
	if ( !initialized() || !m_bCommitted ) {
		return false;
	}
	m_bSealed = true;
	return true;
}

void T3DMemoryArena::release( void ) {
	terrain3d_memory::release( m_pRaw );
	m_pRaw = nullptr;
	m_pBase = nullptr;
	m_nCapacity = 0u;
	m_nBaseAlignment = TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT;
	m_bInitialized = false;
	m_bCommitted = false;
	m_bSealed = false;
}

bool T3DMemoryArena::sliceInBounds( const t3dmemoryArenaSlice_t &rSlice ) const {
	if ( !rSlice.reserved || rSlice.offset > m_nCapacity || rSlice.alignment == 0u ||
		( rSlice.alignment & ( rSlice.alignment - 1u ) ) != 0u ||
		rSlice.offset % rSlice.alignment != 0u ) {
		return false;
	}
	return rSlice.size <= m_nCapacity - rSlice.offset;
}

t3dmemoryArenaSpan_t T3DMemoryArena::resolve( const t3dmemoryArenaSlice_t &rSlice ) {
	if ( !initialized() || !sliceInBounds( rSlice ) ) {
		return {};
	}
	return {m_pBase ? m_pBase + rSlice.offset : nullptr, rSlice.size};
}

t3dmemoryArenaSpan_t T3DMemoryArena::resolve( const t3dmemoryArenaSlice_t &rSlice ) const {
	if ( !initialized() || !sliceInBounds( rSlice ) ) {
		return {};
	}
	return {m_pBase ? m_pBase + rSlice.offset : nullptr, rSlice.size};
}
