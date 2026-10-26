// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_REGION_REPOSITORY_CLASS_H
#define TERRAIN3D_REGION_REPOSITORY_CLASS_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "core/config/terrain_3d_limits.h"
#include "data/model/terrain_3d_region.h"
#include "eastl_config.h"

#include <EASTL/bitset.h>
#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

typedef struct t3dRegionLocationReconcileResult_s {

	eastl::fixed_vector<Vector2i, T3D_REGION_SLOTS, false> m_removedLocations;
	eastl::fixed_vector<int, T3D_REGION_SLOTS, false> m_addedSlots;
	int m_nOldSlotCount = 0;
	bool m_bSlotsChanged = false;
	bool m_bCapacityExceeded = false;
} t3dRegionLocationReconcileResult_t;


typedef struct t3dRegionRepositoryView_s {
	const t3dRegionCoord_t *m_pSlots = NULL;
	Terrain3DRegion *const *m_ppRegionMap = NULL;
	const uint16_t *m_pLogicalRegionMap = NULL;
	int m_nSlotCount = 0;
	uint64_t m_nTopologyGeneration = 0u;
} t3dRegionRepositoryView_t;


EA_FORCE_INLINE bool t3dRegionLocationValid( const Vector2i &rLocation, const int nMapSize ) {
	if ( nMapSize <= 0 ) {
		return false;
	}
	const int half = nMapSize / 2;
	return rLocation.x >= -half && rLocation.x < half && rLocation.y >= -half && rLocation.y < half;
}

static_assert( sizeof( t3dRegionCoord_t ) * T3D_REGION_SLOTS == 2048u, "Packed repository slot table must remain 2 KiB" );

class Terrain3DRegionRepository {
private:
	bool m_bLoaded = false;

	Ref<Terrain3DRegion> m_szRegionRefs[T3D_REGION_SLOTS];
	Terrain3DRegion *m_pszRegionPtrMap[T3D_REGION_SLOTS] = {};
	int m_nRegionResourceCount = 0;
	t3dRegionCoord_t m_szRegionLocations[T3D_REGION_SLOTS] = {};
	int m_nRegionLocationCount = 0;
	eastl::fixed_vector<uint16_t, T3D_REGION_SLOTS, false> m_freeRegionSlots;
	int m_nActiveRegionCount = 0;
	uint16_t m_szRegionMap[T3D_REGION_SLOTS] = {};
	bool m_bRegionMapDirty = true;
	uint64_t m_nTopologyGeneration = 1u;

	void bumpTopologyGeneration( void );
	int activateLocationInternal( const Vector2i &rLocation, bool bPublishRegionMap, bool bBumpGeneration );

public:
	Terrain3DRegionRepository( void );

	static int getRegionMapIndex( const Vector2i &rRegionLoc );

	void clear( void );
	bool releaseRuntimeAllocatorStorage( void );

	EA_FORCE_INLINE int getRegionSize( void ) const {
		return T3D_REGION_SIZE;
	}
	EA_FORCE_INLINE int getRegionMapSize( void ) const {
		return T3D_REGION_MAP_SIZE;
	}
	EA_FORCE_INLINE Vector2i getRegionSizeVector( void ) const {
		return V2I( T3D_REGION_SIZE );
	}
	EA_FORCE_INLINE real_t getVertexSpacing( void ) const {
		return ( real_t )( s_fRegionVertexSpacing );
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

	EA_FORCE_INLINE const t3dRegionCoord_t *getSlotsData( void ) const {
		return m_szRegionLocations;
	}
	EA_FORCE_INLINE Terrain3DRegion *const *getRegionPtrMapData( void ) const {
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
	Terrain3DRegion *getRegionPtr( const Vector2i &rRegionLoc ) const;
	Ref<Terrain3DRegion> getRegion( const Vector2i &rRegionLoc ) const;

	TypedArray<Vector2i> getActiveLocations( void ) const;
	TypedArray<Vector2i> getSlots( void ) const;
	PackedInt32Array getLogicalRegionMapPacked( void ) const;
	t3dRegionRepositoryView_t getView( void ) const;

	bool storeRegion( const Vector2i &rLocation, const Ref<Terrain3DRegion> &rRegion );
	void eraseRegionResource( const Vector2i &rLocation );
	int activateLocation( const Vector2i &rLocation, bool bPublishRegionMap );
	int deactivateLocation( const Vector2i &rLocation );
	t3dRegionLocationReconcileResult_t reconcileLocations( const TypedArray<Vector2i> &rLocations );

	void rebuildFreeSlots( void );
	bool ensureSlotReserve( int nDesiredFreeSlots );
	void rebuildRegionMapFromSlots( void );
	void replaceAll( const Dictionary &rRegions,
	        Terrain3DRegion *const *ppRegionMap,
	        const t3dRegionCoord_t *pLocations,
	        int nLocationCount,
	        const uint16_t *pLogicalRegionMap );

	bool validateInvariants( void ) const;
};

#endif // TERRAIN3D_REGION_REPOSITORY_CLASS_H
