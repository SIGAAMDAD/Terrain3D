// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_ACTIVE_STORAGE_H
#define TERRAIN3D_ACTIVE_STORAGE_H

#include "core/terrain_3d_compiler.h"
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


// Height hot storage is a s_nFixed 32-page, cache-aligned R16 bank. Logical region
// descriptors publish immutable physical pages atomically; leases pin a page while
// readers use it. Writers stage into an unpinned page and never mutate published
// bytes in place. Control, color, and derived maps remain authoritative in
// T3DRegion.
typedef struct t3dActiveMapView_s {
	const uint16_t *m_pHeight = nullptr;
	uint32_t m_nOwnerGeneration = 0u;
	uint32_t m_nRegionGeneration = 0u;
	int m_nRegionSize = 0;
	int m_iSlot = -1;

	EA_FORCE_INLINE bool valid( void ) const { return m_pHeight != nullptr && m_iSlot >= 0; }
} t3dActiveMapView_t;

class T3DActiveMapLease {
public:
	T3DActiveMapLease( void ) = default;
	~T3DActiveMapLease( void );
	T3DActiveMapLease( const T3DActiveMapLease & ) = delete;
	T3DActiveMapLease &operator=( const T3DActiveMapLease & ) = delete;
	T3DActiveMapLease( T3DActiveMapLease &&rOther ) noexcept;
	T3DActiveMapLease &operator=( T3DActiveMapLease &&rOther ) noexcept;

	EA_FORCE_INLINE bool valid( void ) const { return m_view.valid(); }
	EA_FORCE_INLINE const t3dActiveMapView_t &view( void ) const { return m_view; }
	void reset( void );

private:
	friend T3DActiveMapLease t3dActiveStorageAcquire( const void *, const Vector2i & );
	int m_iSlot = -1;
	t3dActiveMapView_t m_view;

	T3DActiveMapLease( int iSlot, const t3dActiveMapView_t &rView );
};

bool t3dActiveStorageClaim( const void *pOwner, int nRegionSize );
size_t t3dActiveStorageRequiredBytes( int nRegionSize );
bool t3dActiveStorageBind( const void *pOwner, int nRegionSize, void *pBacking, size_t nBackingBytes );
bool t3dActiveStorageReconfigure( const void *pOwner, int nRegionSize );
void t3dActiveStorageRelease( const void *pOwner );
void t3dActiveStorageReset( const void *pOwner );
bool t3dActiveStorageIsOwner( const void *pOwner );
int t3dActiveStorageGetRegionMapSize( const void *pOwner );
uint32_t t3dActiveStorageGetOwnerGeneration( const void *pOwner );
size_t t3dActiveStorageGetReservedBytes( void );
int t3dActiveStorageGetPhysicalCapacity( void );
uint64_t t3dActiveStorageGetReaderRetryCount( void );
uint64_t t3dActiveStorageGetReaderMutexCount( void );
uint64_t t3dActiveStorageGetPublicationCount( void );
bool t3dActiveStorageContains( const void *pOwner, const Vector2i &rLocation );

T3DActiveMapLease t3dActiveStorageAcquire( const void *pOwner, const Vector2i &rLocation );
bool t3dActiveStorageSyncRegion(
	const void *pOwner, const Vector2i &rLocation,
	const T3DRegion *pRegion
);
bool t3dActiveStorageSyncHeightPage(
	const void *pOwner, const Vector2i &rLocation,
	const uint8_t *pHeightBytes, size_t nHeightBytes
);
bool t3dActiveStorageReplaceHeightPages(
	const void *pOwner, int nRegionSize,
	const uint8_t *const *pHeightPages, const int *pSlots, size_t nPageCount
);
bool t3dActiveStorageSyncMap(
	const void *pOwner, const Vector2i &rLocation,
	const T3DRegion *pRegion, t3dMapType_t type, const Rect2i &rDirtyRect = Rect2i()
);
bool t3dActiveStorageSyncDerived(
	const void *pOwner, const Vector2i &rLocation,
	const T3DRegion *pRegion, const Rect2i &rDirtyRect = Rect2i()
);
void t3dActiveStorageInvalidate( const void *pOwner, const Vector2i &rLocation );

#endif // TERRAIN3D_ACTIVE_STORAGE_H
