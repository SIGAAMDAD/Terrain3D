// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_COLLISION_CLASS_H
#define TERRAIN3D_COLLISION_CLASS_H

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/physics_material.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include "eastl_config.h"
#include <EASTL/array.h>
#include <EASTL/vector.h>

#include "Terrain3DConstants.h"
#include "core/terrain_3d_util.h"
#include "physics/terrain_3d_collision_surface_builder.h"

class T3DTerrain;


class T3DCollision : public Object {
	GDCLASS( T3DCollision, Object );
	CLASS_NAME();

public: // Constants
	typedef enum t3dcollisionMode_e {
		DISABLED,
		DYNAMIC_GAME,
		DYNAMIC_EDITOR,
		FULL_GAME,
		FULL_EDITOR,
	} t3dcollisionMode_t;

private:
	T3DTerrain *m_pTerrain = nullptr;

	// Public settings
	t3dcollisionMode_t m_mode = DYNAMIC_GAME;
	uint16_t m_nShapeSize = 16;
	uint16_t m_nRadius = 64;
	uint32_t m_layer = 1;
	uint32_t m_nMask = 1;
	real_t m_fPriority = 1.f;
	Ref<PhysicsMaterial> m_pPhysicsMaterial;

	typedef struct t3dcollisionShapeData_s {
		int nWidth = 0;
		int depth = 0;
		Transform3D transform = Transform3D( Basis(), V3_MAX );
		real_t fMinHeight = 0.0f;
		real_t fMaxHeight = 0.0f;
		bool bValid = false;
	} t3dcollisionShapeData_t;

	typedef struct t3dcollisionShapeRecord_s {
		CollisionShape3D *pNode = nullptr;
		RID rid;
		Transform3D transform = Transform3D( Basis(), V3_MAX );
		Vector2i gridPosition = V2I_MAX;
		t3dcollisionShapeData_t data;
		// Dynamic collision alternates retained pBuffers to avoid copy-on-write
		// allocation while shapes are updated every grid step. Full collision uses
		// one shared upload scratch buffer because pRegion shapes update rarely.
		eastl::array<PackedRealArray, 2> szDynamicHeightBuffers;
		uint8_t uploadedHeightBuffer = 0;
		uint8_t pendingHeightBuffer = 1;
		bool bDisabled = true;
	} t3dcollisionShapeRecord_t;

	// Work data. Shape records own all state so updates never read it back from
	// PhysicsServer. Height arrays retain caller-side capacity across grid moves;
	// submitting a changed heightmap remains linear in the sample count.
	RID m_staticBodyRid;				  // Physics Server Static Body
	StaticBody3D *m_pStaticBody = nullptr; // Editor mode StaticBody3D
	eastl::vector<t3dcollisionShapeRecord_t> m_shapeRecords;
	eastl::array<int32_t, TERRAIN3D_MAX_REGION_SLOTS> m_gridScratch{};
	eastl::array<int32_t, TERRAIN3D_MAX_REGION_SLOTS> m_inactiveShapeIds{};
	int m_nInactiveShapeCount = 0;
	PackedRealArray m_fullHeightScratch;
	PackedFloat32Array m_collisionSurfaceScratch;
	T3DCollisionSurfaceBuilder m_collisionSurfaceBuilder;

	typedef struct t3dcollisionRegionCacheEntry_s {
		Vector2i location = V2I_MAX;
		PackedFloat32Array heights;
		int nWidth = 0;
		uint64_t nLastUseTick = 0u;
	} t3dcollisionRegionCacheEntry_t;
	eastl::vector<t3dcollisionRegionCacheEntry_t> m_bakedRegionCache;
	uint64_t m_nBakedRegionCacheTick = 0u;
	uint64_t m_nPreparationUsecLast = 0u;
	uint64_t m_nUploadUsecLast = 0u;
	int m_nBakedCacheHitsLastUpdate = 0;
	int m_nBakedCacheMissesLastUpdate = 0;


	bool m_bInitialized = false;
	bool m_bRegionMapUpdatePending = false;
	Vector2i m_lastSnappedPos = V2I_MAX;
	TypedArray<Vector2i> m_fullRegionSlots;
	uint64_t m_nStreamingGenerationLast = 0u;
	eastl::array<uint8_t, TERRAIN3D_MAX_REGION_SLOTS> m_szFullStreamingResident{};
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
	void refreshBakedRegionCache( const Vector2i &rCenter );
	bool copyBakedTile( const Vector2i &rPosition, int nSize, t3dcollisionShapeRecord_t &rRecord, PackedRealArray &rHeights );
	bool canUseBakedDynamicCollision( void ) const;
	bool getShapeData(
		const Vector2i &rPosition, const int nSize, t3dcollisionShapeRecord_t &rRecord,
		PackedRealArray &rHeights
	);

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
	EA_FORCE_INLINE T3DCollision( void ) {}
	EA_FORCE_INLINE ~T3DCollision( void ) { destroy(); }
	void initialize( T3DTerrain *pTerrain );

	void build( void );
	void onRegionMapChanged( void );
	EA_FORCE_INLINE void resetTargetPosition( void ) { m_lastSnappedPos = V2I_MAX; }
	void update( const Vector2i &rRegionLoc = V2I_MAX, const bool bRebuild = false );
	void destroy( void );

	void setMode( const t3dcollisionMode_t mode );
	EA_FORCE_INLINE t3dcollisionMode_t getMode( void ) const { return m_mode; }
	EA_FORCE_INLINE bool isEnabled( void ) const { return m_mode > DISABLED; }
	EA_FORCE_INLINE bool isEditorMode( void ) const { return m_mode == DYNAMIC_EDITOR || m_mode == FULL_EDITOR; }
	EA_FORCE_INLINE bool isDynamicMode( void ) const { return m_mode == DYNAMIC_GAME || m_mode == DYNAMIC_EDITOR; }

	void setShapeSize( const uint16_t nSize );
	EA_FORCE_INLINE uint16_t getShapeSize( void ) const { return m_nShapeSize; }
	void setRadius( const uint16_t nRadius );
	EA_FORCE_INLINE uint16_t getRadius( void ) const { return m_nRadius; }
	void setLayer( const uint32_t nLayers );
	EA_FORCE_INLINE uint32_t getLayer( void ) const { return m_layer; }
	void setMask( const uint32_t nMask );
	EA_FORCE_INLINE uint32_t getMask( void ) const { return m_nMask; }
	void setPriority( const real_t fPriority );
	EA_FORCE_INLINE real_t getPriority( void ) const { return m_fPriority; }
	void setPhysicsMaterial( const Ref<PhysicsMaterial> &rMat );
	EA_FORCE_INLINE Ref<PhysicsMaterial> getPhysicsMaterial( void ) const { return m_pPhysicsMaterial; }
	Dictionary getStats( void ) const;
	size_t getRetainedBufferBytes( void ) const;
	RID getRid( void ) const;

protected:
	static void _bind_methods( void );
};

using t3dcollisionMode_t = T3DCollision::t3dcollisionMode_t;
VARIANT_ENUM_CAST( T3DCollision::t3dcollisionMode_t );

EA_FORCE_INLINE Vector2i T3DCollision::snapToGrid( const Vector2i &rPos ) const { return Vector2i( intRoundMult( rPos.x, ( int32_t ) ( m_nShapeSize ) ), intRoundMult( rPos.y, ( int32_t ) ( m_nShapeSize ) ) ); }

EA_FORCE_INLINE Vector2i T3DCollision::snapToGrid( const Vector3 &rPos ) const { return Vector2i( Math::floor( rPos.x / ( real_t ) ( m_nShapeSize ) + 0.5f ), Math::floor( rPos.z / ( real_t ) ( m_nShapeSize ) + 0.5f ) ) * m_nShapeSize; }
#endif // TERRAIN3D_COLLISION_CLASS_H
