// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_NODE_STREAMER_H
#define TERRAIN3D_NODE_STREAMER_H

#include "eastl_config.h"
#include <EASTL/bitset.h>
#include <cstdint>

#include <EASTL/vector.h>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "Terrain3DConstants.h"
#include "terrain_3d_limits.h"
#include "terrain_3d_region.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class T3DTerrain;

class T3DNodeStreamer : public Object {
	using Terrain3DNodeStreamer = T3DNodeStreamer;
	GDCLASS( Terrain3DNodeStreamer, Object );
	CLASS_NAME();

public:
	typedef enum t3dstreamedNodeFlags_e {
		NODE_FLAG_NONE = 0,
		NODE_FLAG_DISABLED = 1 << 0,
		// Static streamed nodes never participate in transform-based ownership migration.
		// The default remains movable to preserve serialized behavior from older scenes.
		NODE_FLAG_STATIC = 1 << 1,
	} t3dstreamedNodeFlags_t;

private:
	typedef enum t3dspawnActivationResult_e {
		SPAWN_ACTIVATION_PENDING,
		SPAWN_ACTIVATION_CONSUMED,
		SPAWN_ACTIVATION_FAILED,
	} t3dspawnActivationResult_t;

	typedef struct t3dactiveNode_s {
		int64_t spawnId = 0;
		ObjectID objectId;
		int flags = 0;
	} t3dactiveNode_t;

	typedef struct t3dregionRuntime_s {
		Node3D *pRoot = nullptr;
		eastl::vector<t3dactiveNode_t> nodes;
		int nextSpawn = 0;
		int iActiveListPosition = -1;
		bool bRequestsStarted = false;
		bool bLoading = false;
		bool bActive = false;
	} t3dregionRuntime_t;

	typedef struct t3dmovableNodeRef_s {
		int iRegionIndex = -1;
		int64_t spawnId = 0;
		ObjectID objectId;
	} t3dmovableNodeRef_t;

	T3DTerrain *m_pTerrain = nullptr;
	Node3D *m_pHost = nullptr;
	t3dregionRuntime_t m_szRegions[TERRAIN3D_MAX_REGION_SLOTS];
	eastl::bitset<TERRAIN3D_MAX_REGION_SLOTS> m_desiredRegions;
	eastl::vector<int> m_activeRegionIndices;
	eastl::vector<int> m_desiredRegionIndices;
	eastl::vector<t3dmovableNodeRef_t> m_movableNodes;
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
	Vector2i m_center = V2I_MAX;

	static Vector2i locationFromIndex( int iIndex );
	static int indexFromLocation( const Vector2i &rLocation );
	Node *getActiveNode( const t3dactiveNode_t &rActive ) const;
	bool runtimeHasSpawn( const t3dregionRuntime_t &rRuntime, int64_t nSpawnId ) const;
	int findActiveNodeIndex( const t3dregionRuntime_t &rRuntime, int64_t nSpawnId ) const;
	void trackActiveRegion( int iIndex );
	void untrackActiveRegion( int iIndex );
	void setRegionLoading( t3dregionRuntime_t &rRuntime, bool bLoading );
	void trackMovableNode( int iRegionIndex, const t3dactiveNode_t &rActive );
	void untrackMovableNode( int64_t nSpawnId );
	void removeMovableNodeAt( int iIndex );
	void eraseActiveNode( t3dregionRuntime_t &rRuntime, int iNodeIndex );
	Node3D *ensureRegionRoot( int iIndex, T3DRegion *pRegion );
	bool migrateActiveNodes( void );
	void ensureHost( void );
	void destroyHost( void );
	void captureNodeState( T3DRegion *pRegion, Node3D *pRegionRoot, const t3dactiveNode_t &rActive );
	void deactivateRegion( int iIndex, bool bCaptureState );
	bool activateRegion( int iIndex, int &nNodeBudget );
	t3dspawnActivationResult_t activateSpawn(
		T3DRegion *pRegion, t3dregionRuntime_t &rRuntime,
		const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &rSpawn
	);
	void requestRegionScenes( const eastl::vector<T3DRegion::t3dstreamedNodeRuntimeDescriptor_t> &rSpawns );
	int64_t addStreamedSceneInternal(
		const Ref<PackedScene> &rScene, const String &rScenePath,
		const Transform3D &rGlobalTransform, const Dictionary &rState, int nFlags
	);
	void buildDesiredSet( const Vector2i &rCenter );
	bool spawnIdExists( uint64_t nId ) const;
	int64_t allocateSpawnId( void );
	void refreshSpawnId( void );

public:
	T3DNodeStreamer( void ) {}
	~T3DNodeStreamer( void );

	void initialize( T3DTerrain *pTerrain );
	void destroy( bool bCaptureState = true );
	void updateResidency( const Vector3 &rTargetPosition, bool bForce = false );
	void deferUpdateForFrame( void );
	void suspend( void );

	void setEnabled( bool bEnabled );
	bool isEnabled( void ) const {
		return m_bEnabled;
	}
	void setRadiusRegions( const int nRadius ) {
		const int radius = CLAMP( nRadius, 0, 16 );
		if ( m_nRadiusRegions != radius ) {
			m_nRadiusRegions = radius;
			m_center = V2I_MAX;
		}
	}
	int getRadiusRegions( void ) const {
		return m_nRadiusRegions;
	}
	void setLoadRegionBudget( const int nBudget ) {
		m_nLoadRegionBudget = CLAMP( nBudget, 0, 64 );
	}
	int getLoadRegionBudget( void ) const {
		return m_nLoadRegionBudget;
	}
	void setRuntimeLoadRegionBudgetOverride( const int nBudget ) {
		m_nRuntimeLoadRegionBudgetOverride = nBudget < 0 ? -1 : CLAMP( nBudget, 0, 64 );
	}
	int getEffectiveLoadRegionBudget( void ) const {
		return m_nRuntimeLoadRegionBudgetOverride < 0 ? m_nLoadRegionBudget :
			MIN( m_nLoadRegionBudget, m_nRuntimeLoadRegionBudgetOverride );
	}
	void setLoadNodeBudget( const int nBudget ) {
		m_nLoadNodeBudget = CLAMP( nBudget, 0, 4096 );
	}
	int getLoadNodeBudget( void ) const {
		return m_nLoadNodeBudget;
	}
	void setUnloadRegionBudget( const int nBudget ) {
		m_nUnloadRegionBudget = CLAMP( nBudget, 0, 64 );
	}
	int getUnloadRegionBudget( void ) const {
		return m_nUnloadRegionBudget;
	}
	void setRuntimeUnloadRegionBudgetOverride( const int nBudget ) {
		m_nRuntimeUnloadRegionBudgetOverride = nBudget < 0 ? -1 : CLAMP( nBudget, 0, 64 );
	}
	int getEffectiveUnloadRegionBudget( void ) const {
		return m_nRuntimeUnloadRegionBudgetOverride < 0 ? m_nUnloadRegionBudget :
			MIN( m_nUnloadRegionBudget, m_nRuntimeUnloadRegionBudgetOverride );
	}
	bool isLoadRegionBudgetLimitedLastTick( void ) const {
		return m_bLoadRegionBudgetLimitedLastTick;
	}
	bool isUnloadRegionBudgetLimitedLastTick( void ) const {
		return m_bUnloadRegionBudgetLimitedLastTick;
	}

	int64_t addStreamedScene(
		const Ref<PackedScene> &rScene, const Transform3D &rGlobalTransform,
		const Dictionary &rState = Dictionary(), int nFlags = NODE_FLAG_NONE
	);
	int64_t addStreamedScenePath(
		const String &rScenePath, const Transform3D &rGlobalTransform,
		const Dictionary &rState = Dictionary(), int nFlags = NODE_FLAG_NONE
	);
	bool removeStreamedScene( int64_t nSpawnId );
	Dictionary getStats( void ) const;
	size_t getRetainedCapacityBytes( void ) const;
	int getActiveRegionCount( void ) const {
		return m_nActiveRegionCount;
	}
	int getActiveNodeCount( void ) const {
		return m_nActiveNodeCount;
	}
	int getLoadingRegionCount( void ) const {
		return m_nLoadingRegionCount;
	}
	bool isResidencyIdle( void ) const {
		return m_nLoadingRegionCount == 0 && m_nLoadsLastTick == 0 &&
			m_nNodesActivatedLastTick == 0;
	}
	int getMovableNodeCount( void ) const {
		return ( int ) ( m_movableNodes.size() );
	}
	int getLoadsLastTick( void ) const {
		return m_nLoadsLastTick;
	}
	int getUnloadsLastTick( void ) const {
		return m_nUnloadsLastTick;
	}
	int getSceneLoadFailuresLastTick( void ) const {
		return m_nSceneLoadFailuresLastTick;
	}
	uint64_t getRuntimeGeneration( void ) const {
		return m_nRuntimeGeneration;
	}
	bool isRegionDesired( const Vector2i &rLocation ) const;

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( T3DNodeStreamer::t3dstreamedNodeFlags_t );

using Terrain3DNodeStreamer = T3DNodeStreamer;

#endif // TERRAIN3D_NODE_STREAMER_H
