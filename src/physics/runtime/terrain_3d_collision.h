// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_COLLISION_CLASS_H
#define TERRAIN3D_COLLISION_CLASS_H

#include "Terrain3DTypeSupport.h"
#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_owner.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "core/utility/terrain_3d_util.h"
#include "physics/build/terrain_3d_collision_surface_builder.h"

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/physics_material.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>


// t3dCollisionMode_e centralized in Terrain3DTypes.h.


typedef struct t3dCollisionShapeData_s {

	int m_nWidth = 0;
	int m_nDepth = 0;
	Transform3D m_transform = Transform3D( Basis(), s_v3Max );
	real_t m_fMinHeight = 0.0f;
	real_t m_fMaxHeight = 0.0f;
	bool m_bValid = false;
} t3dCollisionShapeData_t;


typedef struct t3dCollisionShapeRecord_s {

	CollisionShape3D *m_pNode = NULL;
	RID m_hRid;
	Transform3D m_transform = Transform3D( Basis(), s_v3Max );
	Vector2i m_gridPosition = s_v2iMax;
	t3dCollisionShapeData_t m_data;
	// Dynamic collision alternates retained pBuffers to avoid copy-on-write
	// allocation while shapes are updated every grid step. Full collision uses
	// one shared upload scratch buffer because pRegion shapes update rarely.
	PackedRealArray m_dynamicHeightBuffers[2];
	uint8_t m_nUploadedHeightBuffer = 0;
	uint8_t m_nPendingHeightBuffer = 1;
	bool m_bDisabled = true;
} t3dCollisionShapeRecord_t;


typedef struct t3dCollisionRegionCacheEntry_s {

	Vector2i m_location = s_v2iMax;
	PackedFloat32Array m_heights;
	int m_nWidth = 0;
	uint64_t m_nLastUseTick = 0u;
} t3dCollisionRegionCacheEntry_t;


class Terrain3DCollision : public Object {
	GDCLASS( Terrain3DCollision, Object );
	CLASS_NAME();

public: // Constants
private:
	Terrain3DCollisionHost *m_pTerrain = NULL;

	// Public settings
	t3dCollisionMode_t m_mode = DYNAMIC_GAME;
	uint16_t m_nShapeSize = 16;
	uint16_t m_nRadius = 64;
	uint32_t m_nLayer = 1;
	uint32_t m_nMask = 1;
	real_t m_fPriority = 1.f;
	Ref<PhysicsMaterial> m_physicsMaterial;

	// Work data. Shape records own all state so updates never read it back from
	// PhysicsServer. Height arrays retain caller-side capacity across grid moves;
	// submitting a changed heightmap remains linear in the sample count.
	RID m_hStaticBodyRid;               // Physics Server Static Body
	StaticBody3D *m_pStaticBody = NULL; // Editor mode StaticBody3D
	t3dCollisionShapeRecord_t m_szShapeRecords[T3D_MAX_COLLISION_SHAPE_RECORDS];
	int m_nShapeRecordCount = 0;
	bool m_bColdRebuildPending = false;
	bool m_bColdRebuildServiceActive = false;
	uint64_t m_nColdRebuildCount = 0u;
	uint64_t m_nColdRebuildFailures = 0u;
	int32_t m_gridScratch[T3D_MAX_COLLISION_SHAPE_RECORDS] = {};
	eastl::fixed_vector<int32_t, T3D_MAX_COLLISION_SHAPE_RECORDS, false> m_inactiveShapeIds;
	PackedRealArray m_fullHeightScratch;
	PackedFloat32Array m_fullBakedHeightScratch;
	PackedFloat32Array m_collisionSurfaceScratch;
	Terrain3DCollisionSurfaceBuilder m_collisionSurfaceBuilder;


	t3dCollisionRegionCacheEntry_t m_bakedRegionCache[T3D_BAKED_CACHE_CAPACITY];
	int16_t m_bakedRegionByMap[T3D_REGION_SLOTS] = {};
	int m_nBakedRegionCacheCount = 0;
	uint64_t m_nBakedRegionCacheTick = 0u;
	uint64_t m_nPreparationUsecLast = 0u;
	uint64_t m_nUploadUsecLast = 0u;
	int m_nBakedCacheHitsLastUpdate = 0;
	int m_nBakedCacheMissesLastUpdate = 0;

	bool m_bInitialized = false;
	bool m_bRegionMapUpdatePending = false;
	Vector2i m_lastSnappedPos = s_v2iMax;
	t3dRegionCoord_t m_szFullRegionSlots[T3D_REGION_SLOTS];
	int m_nFullRegionSlotCount = 0;
	uint64_t m_nStreamingGenerationLast = 0u;
	uint8_t m_szFullStreamingResident[T3D_REGION_SLOTS] = {};
	uint64_t m_nUpdateUsecLast = 0u;
	int m_nShapesRebuiltLastUpdate = 0;
	int m_nShapesReusedLastUpdate = 0;
	int m_nShapesDeactivatedLastUpdate = 0;
	int m_nHeightSamplesUploadedLastUpdate = 0;
	int m_nFullRegionsUpdatedLastUpdate = 0;
	bool m_bUpdateSkippedUnchangedLast = false;
	bool m_bRebuildRequestedLast = false;

	Vector2i snapToGrid( const Vector2i &rPos ) const;
	Vector2i snapToGrid( const Vector3 &rPos ) const;
	int findBakedRegionCache( const Vector2i &rLocation ) const;
	void clearBakedRegionCache( bool bReleasePayloads );
	void eraseBakedRegionCacheAt( int nIndex );
	void refreshBakedRegionCache( const Vector2i &rCenter );
	bool copyBakedTile( const Vector2i &rPosition, int nSize, t3dCollisionShapeRecord_t &rRecord, PackedRealArray &rHeights );
	bool canUseBakedDynamicCollision( void ) const;
	bool getShapeData( const Vector2i &rPosition, const int nSize, t3dCollisionShapeRecord_t &rRecord, PackedRealArray &rHeights );

	void shapeSetDisabled( const int nShapeId, const bool bDisabled );
	void shapeSetTransform( const int nShapeId, const Transform3D &rXform );
	Vector3 shapeGetPosition( const int nShapeId ) const;
	void shapeSetData( const int nShapeId, const PackedRealArray &rHeights );
	void deactivateShape( const int nShapeId );

	void reloadPhysicsMaterial( void );
	void processRegionMapUpdate( void );
	void updateFullShape( const int nShapeId, const Vector2i &rRegionLoc );
	bool isAuthoringFullResidencyMode( void ) const;
	bool usesEditorBackend( void ) const;
	bool usesDynamicShapePool( void ) const;
	bool regionAllowsCollision( const Vector2i &rRegionLoc ) const;
	bool shapeAllowsCollision( const Vector2i &rPosition, const int nSize ) const;

public:
	EA_FORCE_INLINE Terrain3DCollision( void ) {
		for ( int i = 0; i < T3D_REGION_SLOTS; ++i ) {
			m_bakedRegionByMap[i] = ( int16_t )( -1 );
			m_szFullRegionSlots[i] = t3dInvalidRegionCoord();
		}
	}
	EA_FORCE_INLINE ~Terrain3DCollision( void ) {
		destroy();
	}
	void serviceColdRebuild( void );
	void servicePendingRegionMapUpdate( void );
	void initialize( Terrain3DCollisionHost *pTerrain );

	void build( void );
	void requestColdRebuild( void );
	void onRegionMapChanged( void );
	EA_FORCE_INLINE void resetTargetPosition( void ) {
		m_lastSnappedPos = s_v2iMax;
	}
	void update( const Vector2i &rRegionLoc = s_v2iMax, const bool bRebuild = false );
	void destroy( void );

	void setMode( const t3dCollisionMode_t mode );
	EA_FORCE_INLINE t3dCollisionMode_t getMode( void ) const {
		return m_mode;
	}
	EA_FORCE_INLINE bool isEnabled( void ) const {
		return m_mode > COLLISION_DISABLED;
	}
	EA_FORCE_INLINE bool isEditorMode( void ) const {
		return m_mode == DYNAMIC_EDITOR || m_mode == FULL_EDITOR;
	}
	EA_FORCE_INLINE bool isDynamicMode( void ) const {
		return m_mode == DYNAMIC_GAME || m_mode == DYNAMIC_EDITOR;
	}

	void setShapeSize( const uint16_t nSize );
	EA_FORCE_INLINE uint16_t getShapeSize( void ) const {
		return m_nShapeSize;
	}
	void setRadius( const uint16_t nRadius );
	EA_FORCE_INLINE uint16_t getRadius( void ) const {
		return m_nRadius;
	}
	void setLayer( const uint32_t nLayers );
	EA_FORCE_INLINE uint32_t getLayer( void ) const {
		return m_nLayer;
	}
	void setMask( const uint32_t nMask );
	EA_FORCE_INLINE uint32_t getMask( void ) const {
		return m_nMask;
	}
	void setPriority( const real_t fPriority );
	EA_FORCE_INLINE real_t getPriority( void ) const {
		return m_fPriority;
	}
	void setPhysicsMaterial( const Ref<PhysicsMaterial> &rMat );
	EA_FORCE_INLINE Ref<PhysicsMaterial> getPhysicsMaterial( void ) const {
		return m_physicsMaterial;
	}
	Dictionary getStats( void ) const;
	size_t getRetainedBufferBytes( void ) const;
	RID getRid( void ) const;

protected:
	static void _bind_methods( void );
};
VARIANT_ENUM_CAST( t3dCollisionMode_t );

EA_FORCE_INLINE Vector2i Terrain3DCollision::snapToGrid( const Vector2i &rPos ) const {
	return Vector2i( t3dIntRoundMult( rPos.x, ( int32_t )( m_nShapeSize ) ), t3dIntRoundMult( rPos.y, ( int32_t )( m_nShapeSize ) ) );
}

EA_FORCE_INLINE Vector2i Terrain3DCollision::snapToGrid( const Vector3 &rPos ) const {
	return Vector2i( Math::floor( rPos.x / ( real_t )( m_nShapeSize ) + 0.5f ), Math::floor( rPos.z / ( real_t )( m_nShapeSize ) + 0.5f ) )
	        * m_nShapeSize;
}
#endif // TERRAIN3D_COLLISION_CLASS_H
