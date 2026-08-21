// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_REGION_REPOSITORY_CLASS_H
#define TERRAIN3D_REGION_REPOSITORY_CLASS_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_limits.h"
#include "data/terrain_3d_region.h"
#include "eastl_config.h"

#include <EASTL/bitset.h>
#include <EASTL/vector.h>
#include <cstddef>
#include <cstdint>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

typedef struct t3dRegionLocationReconcileResult_s {
	eastl::vector<Vector2i> m_removedLocations;
	eastl::vector<int> m_addedSlots;
	int m_nOldSlotCount = 0;
	bool m_bSlotsChanged = false;
	bool m_bCapacityExceeded = false;
} t3dRegionLocationReconcileResult_t;

typedef struct t3dRegionRepositoryView_s {
	const Vector2i *m_pSlots = nullptr;
	T3DRegion *const *m_ppRegionMap = nullptr;
	const uint16_t *m_pLogicalRegionMap = nullptr;
	int m_nSlotCount = 0;
	int m_nRegionSize = 0;
	int m_nRegionMapSize = 0;
	real_t m_fVertexSpacing = 1.0f;
	uint64_t m_nTopologyGeneration = 0u;
} t3dRegionRepositoryView_t;

class T3DRegionRepository {
private:
	int m_nRegionSize = 0;
	int m_nRegionMapSize = 0;
	Vector2i m_regionSizev = s_v2iZero;
	real_t m_fVertexSpacing = 1.0f;
	bool m_bLoaded = false;

	Ref<T3DRegion> m_szRegionRefs[s_nMaxRegionSlots];
	T3DRegion *m_pszRegionPtrMap[s_nMaxRegionSlots] = {};
	int m_nRegionResourceCount = 0;
	Vector2i m_szRegionLocations[s_nMaxRegionSlots] = {};
	int m_nRegionLocationCount = 0;
	uint16_t m_szFreeRegionSlots[s_nMaxRegionSlots] = {};
	int m_nFreeRegionSlotCount = 0;
	int m_nActiveRegionCount = 0;
	uint16_t m_szRegionMap[s_nMaxRegionSlots] = {};
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

	EA_FORCE_INLINE int getRegionSize( void ) const {
		return m_nRegionSize;
	}
	EA_FORCE_INLINE int getRegionMapSize( void ) const {
		return m_nRegionMapSize;
	}
	EA_FORCE_INLINE Vector2i getRegionSizeVector( void ) const {
		return m_regionSizev;
	}
	EA_FORCE_INLINE real_t getVertexSpacing( void ) const {
		return m_fVertexSpacing;
	}
	EA_FORCE_INLINE int getActiveRegionCount( void ) const {
		return m_nActiveRegionCount;
	}
	EA_FORCE_INLINE int getSlotCount( void ) const {
		return m_nRegionLocationCount;
	}
	EA_FORCE_INLINE bool isLoaded( void ) const {
		return m_bLoaded;
	}
	EA_FORCE_INLINE void setLoaded( bool bLoaded ) {
		m_bLoaded = bLoaded;
	}
	EA_FORCE_INLINE bool isRegionMapDirty( void ) const {
		return m_bRegionMapDirty;
	}
	EA_FORCE_INLINE void markRegionMapDirty( void ) {
		m_bRegionMapDirty = true;
	}
	EA_FORCE_INLINE uint64_t getTopologyGeneration( void ) const {
		return m_nTopologyGeneration;
	}

	EA_FORCE_INLINE const Vector2i *getSlotsData( void ) const {
		return m_szRegionLocations;
	}
	EA_FORCE_INLINE T3DRegion *const *getRegionPtrMapData( void ) const {
		return m_pszRegionPtrMap;
	}
	EA_FORCE_INLINE const uint16_t *getLogicalRegionMapData( void ) const {
		return m_szRegionMap;
	}
	Dictionary getRegions( void ) const;
	Array getRegionKeys( void ) const;
	EA_FORCE_INLINE bool isEmpty( void ) const {
		return m_nRegionResourceCount == 0;
	}

	bool isLocationValid( const Vector2i &vLocation ) const;
	Vector2i getRegionLocation( const Vector3 &rGlobalPosition ) const;
	int getRegionId( const Vector2i &rRegionLoc ) const;
	int findSlot( const Vector2i &rRegionLoc ) const;
	T3DRegion *getRegionPtr( const Vector2i &rRegionLoc ) const;
	Ref<T3DRegion> getRegion( const Vector2i &rRegionLoc ) const;

	TypedArray<Vector2i> getActiveLocations( void ) const;
	TypedArray<Vector2i> getSlots( void ) const;
	PackedInt32Array getLogicalRegionMapPacked( void ) const;
	t3dRegionRepositoryView_t getView( void ) const;

	bool storeRegion( const Vector2i &rLocation, const Ref<T3DRegion> &rRegion );
	void eraseRegionResource( const Vector2i &rLocation );
	int activateLocation( const Vector2i &rLocation, bool bPublishRegionMap );
	int deactivateLocation( const Vector2i &rLocation );
	t3dRegionLocationReconcileResult_t reconcileLocations( const TypedArray<Vector2i> &rLocations );

	void rebuildFreeSlots( void );
	bool ensureSlotReserve( int nDesiredFreeSlots );
	void rebuildRegionMapFromSlots( void );
	void replaceAll( const Dictionary &rRegions,
			T3DRegion *const *ppRegionMap,
			const Vector2i *pLocations,
			int nLocationCount,
			const uint16_t *pLogicalRegionMap );

	bool validateInvariants( void ) const;
};

#endif // TERRAIN3D_REGION_REPOSITORY_CLASS_H
