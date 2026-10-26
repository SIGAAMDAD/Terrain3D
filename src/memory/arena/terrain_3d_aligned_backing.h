#ifndef TERRAIN3D_ALIGNED_BACKING_H
#define TERRAIN3D_ALIGNED_BACKING_H

#include "memory/core/terrain_3d_memory.h"
#include "core/utility/terrain_3d_checked_math.h"
#include <EASTL/utility.h>

// Raw bootstrap backing: charged through the native allocator, not a memory
// zone. Alignment padding is retained and charged; allocation does not zero it.
class Terrain3DAlignedBacking
{
	void	*m_pRaw = NULL;
	uint8_t *m_pBase = NULL;
	size_t	 m_nCapacity = 0u;
	size_t	 m_nAlignment = 0u;

public:
	Terrain3DAlignedBacking() = default;
	~Terrain3DAlignedBacking() {
		release();
	}
	Terrain3DAlignedBacking( const Terrain3DAlignedBacking & ) = delete;
	Terrain3DAlignedBacking &operator=( const Terrain3DAlignedBacking & ) = delete;

	bool allocate(
		size_t nBytes,
		size_t nAlignment ) {
		release();
		if ( nBytes == 0u ) {
			return true;
		}
		size_t allocated = 0u;
		if ( !t3dIsPowerOfTwo( nAlignment ) || !t3dCheckedAdd( nBytes, nAlignment - 1u, allocated ) ) {
			return false;
		}
		m_pRaw = t3dMemoryAllocate( allocated );
		if ( !m_pRaw ) {
			return false;
		}
		const uintptr_t mask = ( uintptr_t ) ( nAlignment - 1u );
		m_pBase = ( uint8_t * ) ( ( ( uintptr_t ) m_pRaw + mask ) & ~mask );
		m_nCapacity = nBytes;
		m_nAlignment = nAlignment;
		return true;
	}

	void release() {
		t3dMemoryRelease( m_pRaw );
		m_pRaw = NULL;
		m_pBase = NULL;
		m_nCapacity = 0u;
		m_nAlignment = 0u;
	}
	void swap(
		Terrain3DAlignedBacking &rOther ) noexcept {
		eastl::swap( m_pRaw, rOther.m_pRaw );
		eastl::swap( m_pBase, rOther.m_pBase );
		eastl::swap( m_nCapacity, rOther.m_nCapacity );
		eastl::swap( m_nAlignment, rOther.m_nAlignment );
	}
	uint8_t *data() const {
		return m_pBase;
	}
	size_t capacity() const {
		return m_nCapacity;
	}
	size_t alignment() const {
		return m_nAlignment;
	}
};

#endif // TERRAIN3D_ALIGNED_BACKING_H
