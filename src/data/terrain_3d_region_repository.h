// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_REGION_REPOSITORY_CLASS_H
#define TERRAIN3D_REGION_REPOSITORY_CLASS_H

#include "Terrain3DConstants.h"
#include "data/terrain_3d_region.h"
#include "core/terrain_3d_limits.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <EASTL/bitset.h>
#include <EASTL/vector.h>
#include <cstddef>
#include <cstdint>

struct T3DRegionLocationReconcileResult {
	eastl::vector<Vector2i> removedLocations;
	eastl::vector<int> addedSlots;
	int nOldSlotCount = 0;
	bool bSlotsChanged = false;
	bool bCapacityExceeded = false;
};

struct T3DRegionRepositoryView {
	const Vector2i *pSlots = nullptr;
	Terrain3DRegion *const *ppRegionMap = nullptr;
	const uint16_t *pLogicalRegionMap = nullptr;
	int nSlotCount = 0;
	int nRegionSize = 0;
	int nRegionMapSize = 0;
	real_t fVertexSpacing = 1.0f;
	uint64_t nTopologyGeneration = 0u;
};

class T3DRegionRepository {
private:
	int m_nRegionSize = 0;
	int m_nRegionMapSize = 0;
	Vector2i m_regionSizev = V2I_ZERO;
	real_t m_fVertexSpacing = 1.0f;
	bool m_bLoaded = false;

	Dictionary m_regions;
	Terrain3DRegion *m_szRegionPtrMap[TERRAIN3D_MAX_REGION_SLOTS] = {};
	Vector2i m_szRegionLocations[TERRAIN3D_MAX_REGION_SLOTS] = {};
	int m_nRegionLocationCount = 0;
	uint16_t m_szFreeRegionSlots[TERRAIN3D_MAX_REGION_SLOTS] = {};
	int m_nFreeRegionSlotCount = 0;
	int m_nActiveRegionCount = 0;
	uint16_t m_szRegionMap[TERRAIN3D_MAX_REGION_SLOTS] = {};
	bool m_bRegionMapDirty = true;
	uint64_t m_nTopologyGeneration = 1u;

	void bumpTopologyGeneration( void );
	int activateLocationInternal( const Vector2i &rLocation, bool bPublishRegionMap, bool bBumpGeneration );

public:
	T3DRegionRepository( void );

	static int getRegionMapIndex( const Vector2i &rRegionLoc );

	void clear( void );
	void configureGeometry( int nRegionSize, real_t fVertexSpacing );
	void setVertexSpacing( real_t fVertexSpacing );

	int getRegionSize( void ) const { return m_nRegionSize; }
	int getRegionMapSize( void ) const { return m_nRegionMapSize; }
	Vector2i getRegionSizeVector( void ) const { return m_regionSizev; }
	real_t getVertexSpacing( void ) const { return m_fVertexSpacing; }
	int getActiveRegionCount( void ) const { return m_nActiveRegionCount; }
	int getSlotCount( void ) const { return m_nRegionLocationCount; }
	bool isLoaded( void ) const { return m_bLoaded; }
	void setLoaded( bool bLoaded ) { m_bLoaded = bLoaded; }
	bool isRegionMapDirty( void ) const { return m_bRegionMapDirty; }
	void markRegionMapDirty( void ) { m_bRegionMapDirty = true; }
	uint64_t getTopologyGeneration( void ) const { return m_nTopologyGeneration; }

	const Vector2i *getSlotsData( void ) const { return m_szRegionLocations; }
	Terrain3DRegion *const *getRegionPtrMapData( void ) const { return m_szRegionPtrMap; }
	const uint16_t *getLogicalRegionMapData( void ) const { return m_szRegionMap; }
	const Dictionary &getRegions( void ) const { return m_regions; }
	Array getRegionKeys( void ) const { return m_regions.keys(); }
	bool isEmpty( void ) const { return m_regions.is_empty(); }

	bool isLocationValid( const Vector2i &vLocation ) const;
	Vector2i getRegionLocation( const Vector3 &rGlobalPosition ) const;
	int getRegionId( const Vector2i &rRegionLoc ) const;
	int findSlot( const Vector2i &rRegionLoc ) const;
	Terrain3DRegion *getRegionPtr( const Vector2i &rRegionLoc ) const;
	Ref<Terrain3DRegion> getRegion( const Vector2i &rRegionLoc ) const;

	TypedArray<Vector2i> getActiveLocations( void ) const;
	TypedArray<Vector2i> getSlots( void ) const;
	PackedInt32Array getLogicalRegionMapPacked( void ) const;
	T3DRegionRepositoryView getView( void ) const;

	bool storeRegion( const Vector2i &rLocation, const Ref<Terrain3DRegion> &rRegion );
	void eraseRegionResource( const Vector2i &rLocation );
	int activateLocation( const Vector2i &rLocation, bool bPublishRegionMap );
	int deactivateLocation( const Vector2i &rLocation );
	T3DRegionLocationReconcileResult reconcileLocations( const TypedArray<Vector2i> &rLocations );

	void rebuildFreeSlots( void );
	bool ensureSlotReserve( int nDesiredFreeSlots );
	void rebuildRegionMapFromSlots( void );
	void replaceAll( const Dictionary &rRegions, Terrain3DRegion *const *ppRegionMap,
		const Vector2i *pLocations, int nLocationCount, const uint16_t *pLogicalRegionMap );

	bool validateInvariants( void ) const;
};

#endif // TERRAIN3D_REGION_REPOSITORY_CLASS_H
