// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_ACTIVE_STORAGE_H
#define TERRAIN3D_ACTIVE_STORAGE_H

#include "Terrain3DConstants.h"

#include <cstddef>
#include <cstdint>

#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "core/terrain_3d_limits.h"
#include "data/terrain_3d_region.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif


// Height hot storage is a fixed 32-page, cache-aligned R16 bank. Logical region
// descriptors publish immutable physical pages atomically; leases pin a page while
// readers use it. Writers stage into an unpinned page and never mutate published
// bytes in place. Control, color, and derived maps remain authoritative in
// Terrain3DRegion.
typedef struct t3dactiveMapView_s {
	const uint16_t *pHeight = nullptr;
	uint32_t nOwnerGeneration = 0u;
	uint32_t nRegionGeneration = 0u;
	int nRegionSize = 0;
	int iSlot = -1;

	bool valid( void ) const {
		return pHeight != nullptr && iSlot >= 0;
	}
} t3dactiveMapView_t;

class T3DActiveMapLease {
public:
	T3DActiveMapLease( void ) = default;
	~T3DActiveMapLease( void );
	T3DActiveMapLease( const T3DActiveMapLease & ) = delete;
	T3DActiveMapLease &operator=( const T3DActiveMapLease & ) = delete;
	T3DActiveMapLease( T3DActiveMapLease &&rOther ) noexcept;
	T3DActiveMapLease &operator=( T3DActiveMapLease &&rOther ) noexcept;

	bool valid( void ) const {
		return m_view.valid();
	}
	const t3dactiveMapView_t &view( void ) const {
		return m_view;
	}
	void reset( void );

private:
	friend T3DActiveMapLease terrain3dActiveStorageAcquire( const void *, const Vector2i & );
	int m_iSlot = -1;
	t3dactiveMapView_t m_view;

	T3DActiveMapLease( int iSlot, const t3dactiveMapView_t &rView );
};

bool terrain3dActiveStorageClaim( const void *pOwner, int nRegionSize );
size_t terrain3dActiveStorageRequiredBytes( int nRegionSize );
bool terrain3dActiveStorageBind( const void *pOwner, int nRegionSize, void *pBacking, size_t nBackingBytes );
bool terrain3dActiveStorageReconfigure( const void *pOwner, int nRegionSize );
void terrain3dActiveStorageRelease( const void *pOwner );
void terrain3dActiveStorageReset( const void *pOwner );
bool terrain3dActiveStorageIsOwner( const void *pOwner );
int terrain3dActiveStorageGetRegionMapSize( const void *pOwner );
uint32_t terrain3dActiveStorageGetOwnerGeneration( const void *pOwner );
size_t terrain3dActiveStorageGetReservedBytes( void );
int terrain3dActiveStorageGetPhysicalCapacity( void );
uint64_t terrain3dActiveStorageGetReaderRetryCount( void );
uint64_t terrain3dActiveStorageGetReaderMutexCount( void );
uint64_t terrain3dActiveStorageGetPublicationCount( void );
bool terrain3dActiveStorageContains( const void *pOwner, const Vector2i &rLocation );

T3DActiveMapLease terrain3dActiveStorageAcquire( const void *pOwner, const Vector2i &rLocation );
bool terrain3dActiveStorageSyncRegion(
	const void *pOwner, const Vector2i &rLocation,
	const T3DRegion *pRegion
);
bool terrain3dActiveStorageSyncHeightPage(
	const void *pOwner, const Vector2i &rLocation,
	const uint8_t *pHeightBytes, size_t nHeightBytes
);
bool terrain3dActiveStorageReplaceHeightPages(
	const void *pOwner, int nRegionSize,
	const uint8_t *const *pHeightPages, const int *pSlots, size_t nPageCount
);
bool terrain3dActiveStorageSyncMap(
	const void *pOwner, const Vector2i &rLocation,
	const T3DRegion *pRegion, t3dmapType_t type, const Rect2i &rDirtyRect = Rect2i()
);
bool terrain3dActiveStorageSyncDerived(
	const void *pOwner, const Vector2i &rLocation,
	const T3DRegion *pRegion, const Rect2i &rDirtyRect = Rect2i()
);
void terrain3dActiveStorageInvalidate( const void *pOwner, const Vector2i &rLocation );

#endif // TERRAIN3D_ACTIVE_STORAGE_H
