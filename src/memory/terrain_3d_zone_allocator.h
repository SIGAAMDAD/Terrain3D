// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_ZONE_ALLOCATOR_H
#define TERRAIN3D_ZONE_ALLOCATOR_H

#include "core/terrain_3d_compiler.h"
#include "memory/terrain_3d_memory_zone.h"

#include <atomic>
#include <cstdint>

typedef enum t3dAllocatorAllocationOrigin_e : uint8_t {
	TERRAIN3D_ALLOCATION_BOOTSTRAP = 0,
	TERRAIN3D_ALLOCATION_ZONE,
	TERRAIN3D_ALLOCATION_OVERFLOW,
} t3dAllocatorAllocationOrigin_t;

typedef enum t3dAllocatorOverflowPolicy_e : uint8_t {
	TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT = 0,
	TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP,
} t3dAllocatorOverflowPolicy_t;

class T3DZoneAllocatorRouter {
public:
	EA_FORCE_INLINE static void install( T3DMemoryZone *pZone ) {
		s_pZone.store( pZone, std::memory_order_release );
	}
	EA_FORCE_INLINE static T3DMemoryZone *zone( void ) {
		return s_pZone.load( std::memory_order_acquire );
	}
	EA_FORCE_INLINE static void setOverflowPolicy( const t3dAllocatorOverflowPolicy_t policy ) {
		s_nOverflowPolicy.store( ( uint8_t )policy, std::memory_order_release );
	}
	EA_FORCE_INLINE static t3dAllocatorOverflowPolicy_t overflowPolicy( void ) {
		return ( t3dAllocatorOverflowPolicy_t )s_nOverflowPolicy.load( std::memory_order_acquire );
	}

private:
	inline static std::atomic<T3DMemoryZone *> s_pZone{ nullptr };
	inline static std::atomic<uint8_t> s_nOverflowPolicy{ TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT };
};

#endif // TERRAIN3D_ZONE_ALLOCATOR_H
