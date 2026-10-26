// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "memory/terrain_3d_memory_arena.h"

#include "memory/terrain_3d_memory.h"

#include <EASTL/numeric_limits.h>
#include <string.h>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

Terrain3DMemoryArenaLayout::Terrain3DMemoryArenaLayout( const size_t nCapacityBytes )
{
	configure( nCapacityBytes );
}

bool Terrain3DMemoryArenaLayout::isPowerOfTwo( const size_t nValue )
{
	return nValue != 0u && ( nValue & ( nValue - 1u ) ) == 0u;
}

bool Terrain3DMemoryArenaLayout::alignUp( const size_t nValue, const size_t nAlignment, size_t& rAligned )
{
	if ( !isPowerOfTwo( nAlignment ) ) {
		return false;
	}
	const size_t mask = nAlignment - 1u;
	if ( nValue > eastl::numeric_limits<size_t>::max() - mask ) {
		return false;
	}
	rAligned = ( nValue + mask ) & ~mask;
	return true;
}

bool Terrain3DMemoryArenaLayout::configure( const size_t nCapacityBytes )
{
	m_nCapacity = nCapacityBytes;
	m_nLowCursor = 0u;
	m_nMaxAlignment = s_nArenaDefaultBaseAlignment;
	m_bValid = true;
	m_bSealed = false;
	return true;
}

bool Terrain3DMemoryArenaLayout::reserveLow( const size_t nBytes, const size_t nAlignment, t3dMemoryArenaSlice_t& rSlice )
{
	if ( m_bSealed || !m_bValid ) {
		return false;
	}
	if ( !isPowerOfTwo( nAlignment ) ) {
		m_bValid = false;
		return false;
	}
	m_nMaxAlignment = MAX( m_nMaxAlignment, nAlignment );

	size_t aligned = 0u;
	if ( !alignUp( m_nLowCursor, nAlignment, aligned ) || aligned > m_nCapacity || nBytes > m_nCapacity - aligned ) {
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

Terrain3DMemoryArena::~Terrain3DMemoryArena( void )
{
	release();
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
	eastl::swap( m_pRaw, rOther.m_pRaw );
	eastl::swap( m_pBase, rOther.m_pBase );
	eastl::swap( m_nCapacity, rOther.m_nCapacity );
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
	m_nCapacity = rLayout.capacity();
	m_nBaseAlignment = MAX( rLayout.maxAlignment(), s_nArenaDefaultBaseAlignment );
	if ( m_nCapacity == 0u ) {
		m_bInitialized = true;
		return true;
	}
	if ( m_nCapacity > eastl::numeric_limits<size_t>::max() - ( m_nBaseAlignment - 1u ) ) {
		release();
		return false;
	}
	m_pRaw = t3dMemoryAllocate( m_nCapacity + m_nBaseAlignment - 1u );
	if ( !m_pRaw ) {
		release();
		return false;
	}
	const uintptr_t raw_address = (uintptr_t)( m_pRaw );
	const uintptr_t mask = (uintptr_t)( m_nBaseAlignment - 1u );
	m_pBase = (uint8_t *)( ( raw_address + mask ) & ~mask );
	m_bInitialized = true;
	return true;
}

bool Terrain3DMemoryArena::commit( void )
{
	if ( !initialized() || m_bSealed ) {
		return false;
	}
	if ( m_nCapacity > 0u ) {
		memset( m_pBase, 0, m_nCapacity );
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
	t3dMemoryRelease( m_pRaw );
	m_pRaw = NULL;
	m_pBase = NULL;
	m_nCapacity = 0u;
	m_nBaseAlignment = s_nArenaDefaultBaseAlignment;
	m_bInitialized = false;
	m_bCommitted = false;
	m_bSealed = false;
}

bool Terrain3DMemoryArena::sliceInBounds( const t3dMemoryArenaSlice_t& rSlice ) const
{
	if ( !rSlice.m_bReserved || rSlice.m_iOffset > m_nCapacity || rSlice.m_nAlignment == 0u || ( rSlice.m_nAlignment & ( rSlice.m_nAlignment - 1u ) ) != 0u || rSlice.m_iOffset % rSlice.m_nAlignment != 0u ) {
		return false;
	}
	return rSlice.m_nSize <= m_nCapacity - rSlice.m_iOffset;
}

t3dMemoryArenaSpan_t Terrain3DMemoryArena::resolve( const t3dMemoryArenaSlice_t& rSlice )
{
	if ( !initialized() || !sliceInBounds( rSlice ) ) {
		return {};
	}
	return { m_pBase ? m_pBase + rSlice.m_iOffset : NULL, rSlice.m_nSize };
}

t3dMemoryArenaSpan_t Terrain3DMemoryArena::resolve( const t3dMemoryArenaSlice_t& rSlice ) const
{
	if ( !initialized() || !sliceInBounds( rSlice ) ) {
		return {};
	}
	return { m_pBase ? m_pBase + rSlice.m_iOffset : NULL, rSlice.m_nSize };
}
