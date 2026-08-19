// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_ZONE_ALLOCATOR_H
#define TERRAIN3D_ZONE_ALLOCATOR_H

#include "memory/terrain_3d_memory.h"
#include "memory/terrain_3d_memory_zone.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <limits>

enum t3dzoneAllocationOrigin_t : uint8_t { TERRAIN3D_ALLOCATION_BOOTSTRAP, TERRAIN3D_ALLOCATION_ZONE };

struct alignas( 16 ) t3dzoneOriginHeader_t {
	uint64_t magic = 0x4F524947334454ULL;
	void *raw = nullptr;
	T3DMemoryZone *zone = nullptr;
	uint8_t origin = TERRAIN3D_ALLOCATION_BOOTSTRAP;
	uint8_t reserved[7] = {};
};

class T3DZoneAllocatorRouter {
public:
	static void install( T3DMemoryZone *pZone ) { s_pZone.store( pZone, std::memory_order_release ); }
	static T3DMemoryZone *zone() { return s_pZone.load( std::memory_order_acquire ); }
	static void *allocate( size_t nBytes, size_t nAlignment ) {
		if ( nBytes == 0u || nAlignment == 0u || ( nAlignment & ( nAlignment - 1u ) ) != 0u ||
			nBytes > std::numeric_limits<size_t>::max() - sizeof( t3dzoneOriginHeader_t ) - nAlignment ) return nullptr;
		T3DMemoryZone *const pZone = zone();
		const size_t total = nBytes + sizeof( t3dzoneOriginHeader_t ) + nAlignment - 1u;
		void *raw = pZone ? pZone->allocate( total, nAlignment ) : terrain3d_memory::allocate( total );
		if ( !raw ) return nullptr;
		const uintptr_t payload = ( ( uintptr_t ) raw + sizeof( t3dzoneOriginHeader_t ) + nAlignment - 1u ) & ~( uintptr_t ) ( nAlignment - 1u );
		t3dzoneOriginHeader_t *const header = reinterpret_cast<t3dzoneOriginHeader_t *>( payload - sizeof( t3dzoneOriginHeader_t ) );
		*header = {};
		header->magic = 0x4F524947334454ULL;
		header->raw = raw;
		header->zone = pZone;
		header->origin = pZone && pZone->owns( raw ) ? TERRAIN3D_ALLOCATION_ZONE : TERRAIN3D_ALLOCATION_BOOTSTRAP;
		return reinterpret_cast<void *>( payload );
	}
	static void deallocate( void *pMemory ) {
		if ( !pMemory ) return;
		t3dzoneOriginHeader_t *const header = reinterpret_cast<t3dzoneOriginHeader_t *>( static_cast<uint8_t *>( pMemory ) - sizeof( t3dzoneOriginHeader_t ) );
		if ( header->magic != 0x4F524947334454ULL ) return;
		void *const raw = header->raw;
		T3DMemoryZone *const pZone = header->zone;
		const uint8_t origin = header->origin;
		header->magic = 0u;
		if ( origin == TERRAIN3D_ALLOCATION_ZONE && pZone ) pZone->deallocate( raw );
		else terrain3d_memory::release( raw );
	}
private:
	inline static std::atomic<T3DMemoryZone *> s_pZone{nullptr};
};

template <typename T>
class T3DZoneAllocator {
public:
	using value_type = T;
	T3DZoneAllocator() = default;
	template <typename U> T3DZoneAllocator( const T3DZoneAllocator<U> & ) {}
	T *allocate( const size_t nCount ) {
		if ( nCount > std::numeric_limits<size_t>::max() / sizeof( T ) ) return nullptr;
		return static_cast<T *>( T3DZoneAllocatorRouter::allocate( nCount * sizeof( T ), alignof( T ) ) );
	}
	void deallocate( T *pMemory, size_t ) { T3DZoneAllocatorRouter::deallocate( pMemory ); }
	template <typename U> bool operator==( const T3DZoneAllocator<U> & ) const { return true; }
	template <typename U> bool operator!=( const T3DZoneAllocator<U> & ) const { return false; }
};

#endif
