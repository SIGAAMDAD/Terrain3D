#ifndef TERRAIN_3D_HOT_RANGE_ABI_H
#define TERRAIN_3D_HOT_RANGE_ABI_H

#include <stdint.h>
#include <stddef.h>

// Copy this POD contract into the matching Godot engine fork. Neither the
// callback nor the returned spans may be retained after the completion callback.
struct t3dHotRangeInfo_t
{
	uint32_t abi, size;
	uint64_t render_frame, device_frame;
	uint32_t frame_slot, frame_slot_count, byte_count, reserved;
};
struct t3dHotRange_t
{
	uint32_t	   offset_bytes; // Relative to the logical HOT frame, NOT the whole ring.
	uint32_t	   size_bytes;	 // Multiple of 16, with source aligned to this range.
	const uint8_t *source;		 // Stable until the completion callback.
};
struct t3dHotRangeBatch_t
{
	uint32_t			 abi, size, frame_slot, range_count;
	const t3dHotRange_t *ranges;
};

typedef const t3dHotRangeBatch_t *( *t3dHotRangePrepareFn )( void *, const t3dHotRangeInfo_t * ) noexcept;
typedef bool ( *t3dHotRangeCompleteFn )( void *, const t3dHotRangeInfo_t *, bool ) noexcept;

static_assert( sizeof( t3dHotRangeInfo_t ) == 40u, "HOT frame ABI" );
static_assert( sizeof( t3dHotRange_t ) == 16u, "HOT range ABI (64-bit pointers)" );
static_assert( sizeof( t3dHotRangeBatch_t ) == 24u, "HOT range batch ABI (64-bit pointers)" );

#endif
