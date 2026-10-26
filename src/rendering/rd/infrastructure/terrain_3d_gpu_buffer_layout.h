#ifndef TERRAIN3D_GPU_BUFFER_LAYOUT_H
#define TERRAIN3D_GPU_BUFFER_LAYOUT_H

#include <godot_cpp/variant/rid.hpp>
#include <stdint.h>

enum Terrain3DGpuBufferPolicy : uint8_t
{
	T3D_GPU_PUBLICATION,
	T3D_GPU_DIRTY_UPLOAD,
	T3D_GPU_HOT_FRAME,
	T3D_GPU_SCRATCH,
	T3D_GPU_HISTORY,
	T3D_GPU_INDIRECT,
	T3D_GPU_NEUTRAL,
	T3D_GPU_POLICY_COUNT,
};

enum Terrain3DGpuGrowthPolicy : uint8_t
{
	T3D_GPU_FIXED,
	T3D_GPU_EXACT,
	T3D_GPU_HIGH_WATER_1_5X,
	T3D_GPU_POWER_OF_TWO,
	T3D_GPU_FRAME_RING,
};

typedef struct t3dGpuBufferSlice_s
{
	godot::RID m_hBuffer;
	uint32_t m_nOffsetBytes = 0u;
	uint32_t m_nSizeBytes = 0u;
	uint32_t m_nCapacityBytes = 0u;
	uint32_t m_nAlignmentBytes = 1u;
	uint32_t m_nGeneration = 0u;
} t3dGpuBufferSlice_t;

// Arithmetic only. Callers rebuild layouts on cold/warm publication paths and
// create the physical RID separately; reserve() never allocates or grows it.
class Terrain3DGpuBufferLayout
{
	godot::RID m_hBuffer;
	uint32_t m_nCapacityBytes = 0u;
	uint32_t m_nCursorBytes = 0u;
	uint32_t m_nGeneration = 1u;

public:
	static constexpr bool alignUp( uint32_t nValue, uint32_t nAlignment, uint32_t &rResult ) {
		if ( nAlignment == 0u || ( nAlignment & ( nAlignment - 1u ) ) != 0u
		        || nValue > UINT32_MAX - ( nAlignment - 1u ) ) {
			return false;
		}
		rResult = ( nValue + nAlignment - 1u ) & ~( nAlignment - 1u );
		return true;
	}

	static constexpr bool grownCapacity( uint32_t nCurrent,
	        uint32_t nRequired,
	        Terrain3DGpuGrowthPolicy policy,
	        uint32_t &rCapacity ) {
		if ( nRequired <= nCurrent ) {
			rCapacity = nCurrent;
			return true;
		}
		if ( policy == T3D_GPU_FIXED ) {
			return false;
		}
		if ( policy == T3D_GPU_EXACT || policy == T3D_GPU_FRAME_RING ) {
			rCapacity = nRequired;
			return true;
		}
		uint32_t next = policy == T3D_GPU_POWER_OF_TWO ? 1u : ( nCurrent > 0u ? nCurrent : 1u );
		while ( next < nRequired ) {
			const uint32_t step = policy == T3D_GPU_POWER_OF_TWO ? next : ( next + 1u ) / 2u;
			if ( step > UINT32_MAX - next ) {
				return false;
			}
			next += step;
		}
		rCapacity = next;
		return true;
	}

	static constexpr bool reserveOffset( uint32_t nCursor,
	        uint32_t nSize,
	        uint32_t nAlignment,
	        uint32_t nCapacity,
	        uint32_t &rOffset,
	        uint32_t &rNext ) {
		return alignUp( nCursor, nAlignment, rOffset ) && rOffset <= nCapacity && nSize <= nCapacity - rOffset
		        ? ( rNext = rOffset + nSize, true )
		        : false;
	}

	static constexpr uint32_t nextGeneration( uint32_t nGeneration ) {
		return nGeneration == UINT32_MAX ? 1u : nGeneration + 1u;
	}

	void beginLayout( const godot::RID &rBuffer, uint32_t nCapacityBytes ) {
		m_hBuffer = rBuffer;
		m_nCapacityBytes = nCapacityBytes;
		m_nCursorBytes = 0u;
		m_nGeneration = nextGeneration( m_nGeneration );
	}

	bool reserve( uint32_t nSizeBytes, uint32_t nAlignmentBytes, t3dGpuBufferSlice_t &rSlice ) {
		uint32_t offset = 0u;
		uint32_t next = 0u;
		if ( !reserveOffset( m_nCursorBytes, nSizeBytes, nAlignmentBytes, m_nCapacityBytes, offset, next ) ) {
			return false;
		}
		rSlice.m_hBuffer = m_hBuffer;
		rSlice.m_nOffsetBytes = offset;
		rSlice.m_nSizeBytes = nSizeBytes;
		rSlice.m_nCapacityBytes = nSizeBytes;
		rSlice.m_nAlignmentBytes = nAlignmentBytes;
		rSlice.m_nGeneration = m_nGeneration;
		m_nCursorBytes = next;
		return true;
	}

	uint32_t getGeneration() const { return m_nGeneration; }
	uint32_t getUsedBytes() const { return m_nCursorBytes; }
};

static_assert( sizeof( uint32_t ) == 4u, "GPU arena offsets must be 32-bit" );

constexpr bool t3dGpuBufferLayoutArithmeticChecks() {
	uint32_t value = 0u;
	if ( !Terrain3DGpuBufferLayout::alignUp( 5u, 4u, value ) || value != 8u ) {
		return false;
	}
	if ( Terrain3DGpuBufferLayout::alignUp( UINT32_MAX, 4u, value )
	        || Terrain3DGpuBufferLayout::alignUp( 1u, 3u, value ) ) {
		return false;
	}
	if ( !Terrain3DGpuBufferLayout::grownCapacity( 8u, 9u, T3D_GPU_HIGH_WATER_1_5X, value ) || value != 12u ) {
		return false;
	}
	if ( !Terrain3DGpuBufferLayout::grownCapacity( 0u, 9u, T3D_GPU_POWER_OF_TWO, value ) || value != 16u ) {
		return false;
	}
	if ( !Terrain3DGpuBufferLayout::grownCapacity( 3u, 4u, T3D_GPU_POWER_OF_TWO, value ) || value != 4u ) {
		return false;
	}
	uint32_t next = 0u;
	if ( !Terrain3DGpuBufferLayout::reserveOffset( 5u, 4u, 4u, 16u, value, next ) || value != 8u || next != 12u ) {
		return false;
	}
	if ( Terrain3DGpuBufferLayout::reserveOffset( 13u, 4u, 4u, 16u, value, next )
	        || Terrain3DGpuBufferLayout::reserveOffset( UINT32_MAX, 1u, 4u, UINT32_MAX, value, next ) ) {
		return false;
	}
	if ( Terrain3DGpuBufferLayout::nextGeneration( UINT32_MAX ) != 1u ) {
		return false;
	}
	return !Terrain3DGpuBufferLayout::grownCapacity( 8u, 9u, T3D_GPU_FIXED, value );
}
static_assert( t3dGpuBufferLayoutArithmeticChecks(), "GPU arena alignment/growth contract failed" );

#endif // TERRAIN3D_GPU_BUFFER_LAYOUT_H
