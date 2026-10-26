// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "memory/arena/terrain_3d_memory_arena.h"

#include "memory/core/terrain_3d_memory.h"
#include "core/utility/terrain_3d_checked_math.h"

#include <EASTL/numeric_limits.h>
#include <string.h>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

Terrain3DMemoryArenaLayout::Terrain3DMemoryArenaLayout( const size_t nCapacityBytes )
{
	configure( nCapacityBytes );
}

bool Terrain3DMemoryArenaLayout::configure( const size_t nCapacityBytes )
{
	m_nCapacity = nCapacityBytes;
	m_nLowCursor = 0u;
	m_nMaxAlignment = T3D_ARENA_DEFAULT_BASE_ALIGNMENT;
	m_bValid = true;
	m_bSealed = false;
	return true;
}

bool Terrain3DMemoryArenaLayout::reserveLow( const size_t nBytes, const size_t nAlignment, t3dMemoryArenaSlice_t& rSlice )
{
	if ( m_bSealed || !m_bValid ) {
		return false;
	}
	if ( !t3dIsPowerOfTwo<size_t>( nAlignment ) ) {
		m_bValid = false;
		return false;
	}
	m_nMaxAlignment = MAX( m_nMaxAlignment, nAlignment );

	size_t aligned = 0u;
	if ( !t3dCheckedAlignUp<size_t>( m_nLowCursor, nAlignment, aligned ) || aligned > m_nCapacity || nBytes > m_nCapacity - aligned ) {
		m_bValid = false;
		return false;
	}
	rSlice = { aligned, nBytes, nAlignment, true };
	m_nLowCursor = aligned + nBytes;
	return true;
}

bool Terrain3DMemoryArenaLayout::seal( void )
{
	if ( !m_bValid ) {
		return false;
	}
	m_bSealed = true;
	return true;
}

Terrain3DMemoryArena::Terrain3DMemoryArena( Terrain3DMemoryArena&& rOther ) noexcept
{
	swap( rOther );
}

Terrain3DMemoryArena& Terrain3DMemoryArena::operator=( Terrain3DMemoryArena&& rOther ) noexcept
{
	if ( this != &rOther ) {
		release();
		swap( rOther );
	}
	return *this;
}

void Terrain3DMemoryArena::swap( Terrain3DMemoryArena& rOther ) noexcept
{
	m_backing.swap( rOther.m_backing );
	eastl::swap( m_nBaseAlignment, rOther.m_nBaseAlignment );
	eastl::swap( m_bInitialized, rOther.m_bInitialized );
	eastl::swap( m_bCommitted, rOther.m_bCommitted );
	eastl::swap( m_bSealed, rOther.m_bSealed );
}

bool Terrain3DMemoryArena::initialize( const Terrain3DMemoryArenaLayout& rLayout )
{
	release();
	if ( !rLayout.valid() || !rLayout.sealed() ) {
		return false;
	}
	m_nBaseAlignment = MAX( rLayout.maxAlignment(), T3D_ARENA_DEFAULT_BASE_ALIGNMENT );
	if ( !m_backing.allocate( rLayout.capacity(), m_nBaseAlignment ) ) {
		release();
		return false;
	}
	m_bInitialized = true;
	return true;
}

bool Terrain3DMemoryArena::commit( void )
{
	if ( !initialized() || m_bSealed ) {
		return false;
	}
	if ( m_backing.capacity() > 0u ) {
		memset( m_backing.data(), 0, m_backing.capacity() );
	}
	m_bCommitted = true;
	return true;
}

bool Terrain3DMemoryArena::seal( void )
{
	if ( !initialized() || !m_bCommitted ) {
		return false;
	}
	m_bSealed = true;
	return true;
}

void Terrain3DMemoryArena::release( void )
{
	m_backing.release();
	m_nBaseAlignment = T3D_ARENA_DEFAULT_BASE_ALIGNMENT;
	m_bInitialized = false;
	m_bCommitted = false;
	m_bSealed = false;
}

bool Terrain3DMemoryArena::sliceInBounds( const t3dMemoryArenaSlice_t& rSlice ) const
{
	if ( !rSlice.m_bReserved || rSlice.m_iOffset > m_backing.capacity() || rSlice.m_nAlignment == 0u || ( rSlice.m_nAlignment & ( rSlice.m_nAlignment - 1u ) ) != 0u || rSlice.m_iOffset % rSlice.m_nAlignment != 0u ) {
		return false;
	}
	return rSlice.m_nSize <= m_backing.capacity() - rSlice.m_iOffset;
}

t3dMemoryArenaSpan_t Terrain3DMemoryArena::resolve( const t3dMemoryArenaSlice_t& rSlice )
{
	if ( !initialized() || !sliceInBounds( rSlice ) ) {
		return {};
	}
	return { m_backing.data() ? m_backing.data() + rSlice.m_iOffset : NULL, rSlice.m_nSize };
}

t3dMemoryArenaSpan_t Terrain3DMemoryArena::resolve( const t3dMemoryArenaSlice_t& rSlice ) const
{
	if ( !initialized() || !sliceInBounds( rSlice ) ) {
		return {};
	}
	return { m_backing.data() ? m_backing.data() + rSlice.m_iOffset : NULL, rSlice.m_nSize };
}
