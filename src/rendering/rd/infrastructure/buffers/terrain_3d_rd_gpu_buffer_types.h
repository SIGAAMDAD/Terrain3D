#ifndef TERRAIN3D_RD_GPU_BUFFER_TYPES_H
#define TERRAIN3D_RD_GPU_BUFFER_TYPES_H

#include "Terrain3DTypes.h"
#include "core/utility/terrain_3d_checked_math.h"
#include "core/utility/terrain_3d_generation.h"

#include <godot_cpp/variant/rid.hpp>
#include <stdint.h>

using namespace godot;

typedef struct t3dRdGpuBufferSlice_s
{
	RID		 m_hBuffer;
	uint32_t m_nOffsetBytes = 0u;
	uint32_t m_nSizeBytes = 0u;
	uint32_t m_nCapacityBytes = 0u;
	uint32_t m_nAlignmentBytes = 1u;
	uint32_t m_nGeneration = 0u;
} t3dRdGpuBufferSlice_t;

static_assert( sizeof( uint32_t ) == 4u, "GPU buffer offsets must be 32-bit" );

inline constexpr bool t3dRdGpuBufferAlignUp(
	const uint32_t nValue,
	const uint32_t nAlignment,
	uint32_t	  &rResult ) {
	return t3dCheckedAlignUp( nValue, nAlignment, rResult );
}

inline constexpr bool t3dRdGpuBufferGrownCapacity(
	const uint32_t				   nCurrent,
	const uint32_t				   nRequired,
	const Terrain3DGpuGrowthPolicy policy,
	uint32_t					  &rCapacity ) {
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

inline constexpr bool t3dRdGpuBufferReserveOffset(
	const uint32_t nCursor,
	const uint32_t nSize,
	const uint32_t nAlignment,
	const uint32_t nCapacity,
	uint32_t	  &rOffset,
	uint32_t	  &rNext ) {
	return t3dRdGpuBufferAlignUp( nCursor, nAlignment, rOffset ) && rOffset <= nCapacity && nSize <= nCapacity - rOffset
			   ? ( rNext = rOffset + nSize, true )
			   : false;
}

inline constexpr uint32_t t3dRdGpuBufferNextGeneration(
	const uint32_t nGeneration ) {
	return t3dNextGeneration( nGeneration );
}

constexpr bool t3dRdGpuBufferArithmeticChecks(
	void ) {
	uint32_t value = 0u;
	if ( !t3dRdGpuBufferAlignUp( 5u, 4u, value ) || value != 8u ) {
		return false;
	}
	if ( t3dRdGpuBufferAlignUp( UINT32_MAX, 4u, value ) || t3dRdGpuBufferAlignUp( 1u, 3u, value ) ) {
		return false;
	}
	if ( !t3dRdGpuBufferGrownCapacity( 8u, 9u, T3D_GPU_HIGH_WATER_1_5X, value ) || value != 12u ) {
		return false;
	}
	if ( !t3dRdGpuBufferGrownCapacity( 0u, 9u, T3D_GPU_POWER_OF_TWO, value ) || value != 16u ) {
		return false;
	}
	if ( !t3dRdGpuBufferGrownCapacity( 3u, 4u, T3D_GPU_POWER_OF_TWO, value ) || value != 4u ) {
		return false;
	}
	uint32_t next = 0u;
	if ( !t3dRdGpuBufferReserveOffset( 5u, 4u, 4u, 16u, value, next ) || value != 8u || next != 12u ) {
		return false;
	}
	if ( t3dRdGpuBufferReserveOffset( 13u, 4u, 4u, 16u, value, next ) ||
		 t3dRdGpuBufferReserveOffset( UINT32_MAX, 1u, 4u, UINT32_MAX, value, next ) )
		return false;
	if ( t3dRdGpuBufferNextGeneration( UINT32_MAX ) != 1u ) {
		return false;
	}
	return !t3dRdGpuBufferGrownCapacity( 8u, 9u, T3D_GPU_FIXED, value );
}
static_assert( t3dRdGpuBufferArithmeticChecks(), "GPU buffer alignment/growth contract failed" );

// Align relative to a typed-array origin, including strides such as 48 bytes.
// The resulting absolute offset must also satisfy the SSBO record alignment.
inline constexpr bool t3dRdGpuBufferSection(
	uint32_t	  &cursor,
	const uint32_t size,
	const uint32_t alignment,
	const uint32_t stride,
	const uint32_t origin,
	uint32_t	  &offset ) {
	if ( !alignment || ( alignment & ( alignment - 1u ) ) || !stride || cursor < origin || origin % alignment ) {
		return false;
	}
	uint32_t a = alignment, b = stride;
	while ( b ) {
		const uint32_t remainder = a % b;
		a = b;
		b = remainder;
	}
	const uint64_t multiple = uint64_t( alignment / a ) * stride;
	const uint64_t aligned = ( ( uint64_t( cursor - origin ) + multiple - 1u ) / multiple ) * multiple + origin;
	if ( aligned > UINT32_MAX || size > UINT32_MAX - aligned ) {
		return false;
	}
	offset = uint32_t( aligned );
	cursor = offset + size;
	return true;
}
constexpr bool t3dRdGpuBufferSectionChecks() {
	uint32_t cursor = 96u, offset = 0u;
	if ( !t3dRdGpuBufferSection( cursor, 48u, 16u, 48u, 80u, offset ) || offset != 128u || cursor != 176u ) {
		return false;
	}
	return !t3dRdGpuBufferSection( cursor, 1u, 3u, 48u, 80u, offset ) &&
		   !t3dRdGpuBufferSection( cursor, UINT32_MAX, 16u, 48u, 80u, offset );
}
static_assert( t3dRdGpuBufferSectionChecks(), "Typed arena layout overflow/alignment failed" );

#endif // TERRAIN3D_RD_GPU_BUFFER_TYPES_H
