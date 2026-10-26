#ifndef TERRAIN3D_RD_GPU_BUFFER_TYPES_H
#define TERRAIN3D_RD_GPU_BUFFER_TYPES_H

#include <godot_cpp/variant/rid.hpp>
#include <stdint.h>

using namespace godot;

enum Terrain3DGpuGrowthPolicy : uint8_t
{
	T3D_GPU_FIXED,
	T3D_GPU_EXACT,
	T3D_GPU_HIGH_WATER_1_5X,
	T3D_GPU_POWER_OF_TWO,
	T3D_GPU_FRAME_RING,
};

typedef struct t3dRdGpuBufferSlice_s
{
	RID m_hBuffer;
	uint32_t m_nOffsetBytes = 0u;
	uint32_t m_nSizeBytes = 0u;
	uint32_t m_nCapacityBytes = 0u;
	uint32_t m_nAlignmentBytes = 1u;
	uint32_t m_nGeneration = 0u;
} t3dRdGpuBufferSlice_t;

static_assert( sizeof( uint32_t ) == 4u, "GPU buffer offsets must be 32-bit" );

inline constexpr bool t3dRdGpuBufferAlignUp( const uint32_t nValue,
        const uint32_t nAlignment,
        uint32_t &rResult ) {
	if ( nAlignment == 0u || ( nAlignment & ( nAlignment - 1u ) ) != 0u
	        || nValue > UINT32_MAX - ( nAlignment - 1u ) ) {
		return false;
	}
	rResult = ( nValue + nAlignment - 1u ) & ~( nAlignment - 1u );
	return true;
}

inline constexpr bool t3dRdGpuBufferGrownCapacity( const uint32_t nCurrent,
        const uint32_t nRequired,
        const Terrain3DGpuGrowthPolicy policy,
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
			if ( policy == T3D_GPU_HIGH_WATER_1_5X ) {
				rCapacity = nRequired;
				return true;
			}
			return false;
		}
		next += step;
	}
	rCapacity = next;
	return true;
}

inline constexpr bool t3dRdGpuBufferReserveOffset( const uint32_t nCursor,
        const uint32_t nSize,
        const uint32_t nAlignment,
        const uint32_t nCapacity,
        uint32_t &rOffset,
        uint32_t &rNext ) {
	return t3dRdGpuBufferAlignUp( nCursor, nAlignment, rOffset )
	        && rOffset <= nCapacity
	        && nSize <= nCapacity - rOffset
	        ? ( rNext = rOffset + nSize, true )
	        : false;
}

inline constexpr uint32_t t3dRdGpuBufferNextGeneration( const uint32_t nGeneration ) {
	return nGeneration == UINT32_MAX ? 1u : nGeneration + 1u;
}

constexpr bool t3dRdGpuBufferArithmeticChecks( void ) {
	uint32_t value = 0u;
	if ( !t3dRdGpuBufferAlignUp( 5u, 4u, value ) || value != 8u ) return false;
	if ( t3dRdGpuBufferAlignUp( UINT32_MAX, 4u, value ) || t3dRdGpuBufferAlignUp( 1u, 3u, value ) ) return false;
	if ( !t3dRdGpuBufferGrownCapacity( 8u, 9u, T3D_GPU_HIGH_WATER_1_5X, value ) || value != 12u ) return false;
	if ( !t3dRdGpuBufferGrownCapacity( 0u, 9u, T3D_GPU_POWER_OF_TWO, value ) || value != 16u ) return false;
	if ( !t3dRdGpuBufferGrownCapacity( 3u, 4u, T3D_GPU_POWER_OF_TWO, value ) || value != 4u ) return false;
	uint32_t next = 0u;
	if ( !t3dRdGpuBufferReserveOffset( 5u, 4u, 4u, 16u, value, next ) || value != 8u || next != 12u ) return false;
	if ( t3dRdGpuBufferReserveOffset( 13u, 4u, 4u, 16u, value, next )
	        || t3dRdGpuBufferReserveOffset( UINT32_MAX, 1u, 4u, UINT32_MAX, value, next ) ) return false;
	if ( t3dRdGpuBufferNextGeneration( UINT32_MAX ) != 1u ) return false;
	return !t3dRdGpuBufferGrownCapacity( 8u, 9u, T3D_GPU_FIXED, value );
}
static_assert( t3dRdGpuBufferArithmeticChecks(), "GPU buffer alignment/growth contract failed" );

#endif // TERRAIN3D_RD_GPU_BUFFER_TYPES_H
