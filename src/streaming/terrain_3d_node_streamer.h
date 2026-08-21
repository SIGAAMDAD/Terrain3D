// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_NODE_STREAMER_H
#define TERRAIN3D_NODE_STREAMER_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_limits.h"
#include "data/terrain_3d_region.h"
#include "eastl_config.h"

#include <EASTL/bitset.h>
#include <EASTL/vector.h>
#include <cstdint>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class T3DTerrain;

typedef enum t3dStreamedNodeFlags_e {
	NODE_FLAG_NONE = 0,
	NODE_FLAG_DISABLED = 1 << 0,
	// Static streamed nodes never participate in transform-based ownership migration.
	// The default remains movable to preserve serialized behavior from older scenes.
	NODE_FLAG_STATIC = 1 << 1,
	// Static/render-only scene that may be replaced by an HLOD proxy.
	NODE_FLAG_HLOD_VISUAL = 1 << 2,
} t3dStreamedNodeFlags_t;

typedef enum t3dSpawnActivationResult_e {
	SPAWN_ACTIVATION_PENDING,
	SPAWN_ACTIVATION_CONSUMED,
	SPAWN_ACTIVATION_FAILED,
} t3dSpawnActivationResult_t;

typedef struct t3dActiveNode_s {
	int64_t m_nSpawnId = 0;
	ObjectID m_hObjectId;
	int m_nFlags = 0;
} t3dActiveNode_t;

typedef struct t3dRegionRuntime_s {
	Node3D *m_pRoot = nullptr;
	eastl::vector<t3dActiveNode_t> m_nodes;
	int m_nNextSpawn = 0;
	int m_iActiveListPosition = -1;
	bool m_bRequestsStarted = false;
	bool m_bLoading = false;
	bool m_bActive = false;
} t3dRegionRuntime_t;

typedef struct t3dMovableNodeRef_s {
	int m_iRegionIndex = -1;
	int64_t m_nSpawnId = 0;
	ObjectID m_hObjectId;
} t3dMovableNodeRef_t;

class T3DNodeStreamer : public Object {
	GDCLASS( T3DNodeStreamer, Object );
	CLASS_NAME();

public:
private:
	T3DTerrain *m_pTerrain = nullptr;
	Node3D *m_pHost = nullptr;
	t3dRegionRuntime_t m_szRegions[s_nMaxRegionSlots];
	eastl::bitset<s_nMaxRegionSlots> m_desiredRegions;
	uint16_t m_szActiveRegionIndices[s_nMaxRegionSlots] = {};
	uint16_t m_szDesiredRegionIndices[s_nMaxRegionSlots] = {};
	uint16_t m_nDesiredRegionCount = 0u;
	eastl::vector<t3dMovableNodeRef_t> m_movableNodes;
	bool m_bEnabled = true;
	int m_nRadiusRegions = 1;
	int m_nLoadRegionBudget = 2;
	int m_nRuntimeLoadRegionBudgetOverride = -1;
	int m_nLoadNodeBudget = 16;
	int m_nUnloadRegionBudget = 4;
	int m_nRuntimeUnloadRegionBudgetOverride = -1;
	int m_nActiveRegionCount = 0;
	int m_nActiveNodeCount = 0;
	int m_nLoadingRegionCount = 0;
	bool m_bHasPendingActivation = false;
	int m_nLoadsLastTick = 0;
	int m_nUnloadsLastTick = 0;
	int m_nSceneLoadFailuresLastTick = 0;
	int m_nNodesActivatedLastTick = 0;
	int m_nNodesDeactivatedLastTick = 0;
	int m_nSceneRequestsLastTick = 0;
	int m_nScenePendingPollsLastTick = 0;
	int m_nLoadRegionBudgetUsedLastTick = 0;
	int m_nLoadNodeBudgetUsedLastTick = 0;
	int m_nUnloadRegionBudgetUsedLastTick = 0;
	bool m_bLoadRegionBudgetLimitedLastTick = false;
	bool m_bUnloadRegionBudgetLimitedLastTick = false;
	bool m_bDeferredLastTick = false;
	int m_iFixedSlotsScannedLastTick = 0;
	int m_nActiveRegionsExaminedLastTick = 0;
	int m_nNodesTestedForMigrationLastTick = 0;
	int m_nGlobalTransformsReadLastTick = 0;
	int m_nMigrationsLastTick = 0;
	uint64_t m_nMigrationUsecLastTick = 0u;
	uint64_t m_nNextSpawnId = 1u;
	uint64_t m_nRuntimeGeneration = 1u;
	Vector2i m_center = s_v2iMax;

	static Vector2i locationFromIndex( int iIndex );
	static int indexFromLocation( const Vector2i &rLocation );
	Node *getActiveNode( const t3dActiveNode_t &rActive ) const;
	bool runtimeHasSpawn( const t3dRegionRuntime_t &rRuntime, int64_t nSpawnId ) const;
	int findActiveNodeIndex( const t3dRegionRuntime_t &rRuntime, int64_t nSpawnId ) const;
	void trackActiveRegion( int iIndex );
	void untrackActiveRegion( int iIndex );
	void setRegionLoading( t3dRegionRuntime_t &rRuntime, bool bLoading );
	void trackMovableNode( int iRegionIndex, const t3dActiveNode_t &rActive );
	void untrackMovableNode( int64_t nSpawnId );
	void removeMovableNodeAt( int iIndex );
	void eraseActiveNode( t3dRegionRuntime_t &rRuntime, int iNodeIndex );
	Node3D *ensureRegionRoot( int iIndex, T3DRegion *pRegion );
	bool migrateActiveNodes( void );
	void ensureHost( void );
	void destroyHost( void );
	void captureNodeState( T3DRegion *pRegion, Node3D *pRegionRoot, const t3dActiveNode_t &rActive );
	void deactivateRegion( int iIndex, bool bCaptureState );
	bool activateRegion( int iIndex, int &nNodeBudget );
	t3dSpawnActivationResult_t
	activateSpawn( T3DRegion *pRegion, t3dRegionRuntime_t &rRuntime, const t3dStreamedNodeRuntimeDescriptor_t &rSpawn );
	void requestRegionScenes( const Vector2i &rOwnerLocation,
			const eastl::vector<t3dStreamedNodeRuntimeDescriptor_t> &rSpawns );
	bool isHlodVisualSuppressed( const Vector2i &rOwnerLocation,
			const t3dStreamedNodeRuntimeDescriptor_t &rSpawn ) const;
	bool streamedSpawnCoverageIntersects( const Vector2i &rOwnerLocation,
			const t3dStreamedNodeRuntimeDescriptor_t &rSpawn,
			const Vector2i &rTargetMin,
			const Vector2i &rTargetMax ) const;
	int64_t addStreamedSceneInternal( const Ref<PackedScene> &rScene,
			const String &rScenePath,
			const Transform3D &rGlobalTransform,
			const Dictionary &rState,
			int nFlags,
			int nHlodMinLevel );
	void buildDesiredSet( const Vector2i &rCenter );
	bool usesFullAuthoringResidency( void ) const;
	int getEffectiveRadiusRegions( void ) const;
	bool spawnIdExists( uint64_t nId ) const;
	int64_t allocateSpawnId( void );
	void refreshSpawnId( void );

public:
	EA_FORCE_INLINE T3DNodeStreamer( void ) {}
	~T3DNodeStreamer( void );

	void initialize( T3DTerrain *pTerrain );
	void destroy( bool bCaptureState = true );
	void updateResidency( const Vector3 &rTargetPosition, bool bForce = false );
	void deferUpdateForFrame( void );
	void suspend( void );
	EA_FORCE_INLINE void invalidateResidency( void ) {
		m_center = s_v2iMax;
	}

	void setEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isEnabled( void ) const {
		return m_bEnabled;
	}
	void setRadiusRegions( const int nRadius ) {
		const int radius = CLAMP( nRadius, 0, 16 );
		if ( m_nRadiusRegions != radius ) {
			m_nRadiusRegions = radius;
			m_center = s_v2iMax;
		}
	}
	EA_FORCE_INLINE int getRadiusRegions( void ) const {
		return m_nRadiusRegions;
	}
	EA_FORCE_INLINE void setLoadRegionBudget( const int nBudget ) {
		m_nLoadRegionBudget = CLAMP( nBudget, 0, 64 );
	}
	EA_FORCE_INLINE int getLoadRegionBudget( void ) const {
		return m_nLoadRegionBudget;
	}
	EA_FORCE_INLINE void setRuntimeLoadRegionBudgetOverride( const int nBudget ) {
		m_nRuntimeLoadRegionBudgetOverride = nBudget < 0 ? -1 : CLAMP( nBudget, 0, 64 );
	}
	EA_FORCE_INLINE int getEffectiveLoadRegionBudget( void ) const {
		return m_nRuntimeLoadRegionBudgetOverride < 0 ? m_nLoadRegionBudget
													  : MIN( m_nLoadRegionBudget, m_nRuntimeLoadRegionBudgetOverride );
	}
	EA_FORCE_INLINE void setLoadNodeBudget( const int nBudget ) {
		m_nLoadNodeBudget = CLAMP( nBudget, 0, 4096 );
	}
	EA_FORCE_INLINE int getLoadNodeBudget( void ) const {
		return m_nLoadNodeBudget;
	}
	EA_FORCE_INLINE void setUnloadRegionBudget( const int nBudget ) {
		m_nUnloadRegionBudget = CLAMP( nBudget, 0, 64 );
	}
	EA_FORCE_INLINE int getUnloadRegionBudget( void ) const {
		return m_nUnloadRegionBudget;
	}
	EA_FORCE_INLINE void setRuntimeUnloadRegionBudgetOverride( const int nBudget ) {
		m_nRuntimeUnloadRegionBudgetOverride = nBudget < 0 ? -1 : CLAMP( nBudget, 0, 64 );
	}
	EA_FORCE_INLINE int getEffectiveUnloadRegionBudget( void ) const {
		return m_nRuntimeUnloadRegionBudgetOverride < 0
				? m_nUnloadRegionBudget
				: MIN( m_nUnloadRegionBudget, m_nRuntimeUnloadRegionBudgetOverride );
	}
	EA_FORCE_INLINE bool isLoadRegionBudgetLimitedLastTick( void ) const {
		return m_bLoadRegionBudgetLimitedLastTick;
	}
	EA_FORCE_INLINE bool isUnloadRegionBudgetLimitedLastTick( void ) const {
		return m_bUnloadRegionBudgetLimitedLastTick;
	}

	int64_t addStreamedScene( const Ref<PackedScene> &rScene,
			const Transform3D &rGlobalTransform,
			const Dictionary &rState = Dictionary(),
			int nFlags = NODE_FLAG_NONE,
			int nHlodMinLevel = 1 );
	int64_t addStreamedScenePath( const String &rScenePath,
			const Transform3D &rGlobalTransform,
			const Dictionary &rState = Dictionary(),
			int nFlags = NODE_FLAG_NONE,
			int nHlodMinLevel = 1 );
	void onHlodRepresentationChanged( const Vector2i &rRegion );
	bool areHlodVisualRegionsReady( const Vector2i &rRegionMin, const Vector2i &rRegionMax ) const;
	bool removeStreamedScene( int64_t nSpawnId );
	Dictionary getStats( void ) const;
	size_t getRetainedCapacityBytes( void ) const;
	EA_FORCE_INLINE int getActiveRegionCount( void ) const {
		return m_nActiveRegionCount;
	}
	EA_FORCE_INLINE int getActiveNodeCount( void ) const {
		return m_nActiveNodeCount;
	}
	EA_FORCE_INLINE int getLoadingRegionCount( void ) const {
		return m_nLoadingRegionCount;
	}
	EA_FORCE_INLINE bool isResidencyIdle( void ) const {
		return m_nLoadingRegionCount == 0 && m_nLoadsLastTick == 0 && m_nNodesActivatedLastTick == 0;
	}
	EA_FORCE_INLINE int getMovableNodeCount( void ) const {
		return ( int )( m_movableNodes.size() );
	}
	EA_FORCE_INLINE int getLoadsLastTick( void ) const {
		return m_nLoadsLastTick;
	}
	EA_FORCE_INLINE int getUnloadsLastTick( void ) const {
		return m_nUnloadsLastTick;
	}
	EA_FORCE_INLINE int getSceneLoadFailuresLastTick( void ) const {
		return m_nSceneLoadFailuresLastTick;
	}
	EA_FORCE_INLINE uint64_t getRuntimeGeneration( void ) const {
		return m_nRuntimeGeneration;
	}
	bool isRegionDesired( const Vector2i &rLocation ) const;

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dStreamedNodeFlags_t );
#endif // TERRAIN3D_NODE_STREAMER_H
