// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once
#ifndef TERRAIN3D_HLOD_STREAMER_H
#define TERRAIN3D_HLOD_STREAMER_H

#include "eastl_config.h"
#include <EASTL/bitset.h>
#include <EASTL/vector.h>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include "Terrain3DConstants.h"
#include "hlod/terrain_3d_hlod_manifest.h"
#include "hlod/terrain_3d_hlod_package.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif
class T3DTerrain;

class T3DHLODStreamer : public Object {
	using Terrain3DHLODStreamer = T3DHLODStreamer;
	GDCLASS( Terrain3DHLODStreamer, Object );
	CLASS_NAME();

	typedef struct t3dentry_s {
		int level = 1;
		Vector2i cluster = V2I_ZERO;
		Vector2i regionMin = V2I_ZERO;
		Vector2i regionMax = V2I_ZERO;
		AABB localBounds;
		String packagePath;
		real_t geometricError = 1.0f;
		int64_t sourceSignature = 0;
		int64_t buildSignature = 0;
		int64_t estimatedGpuBytes = 0;
		int parent = -1;
		int children[4] = {-1, -1, -1, -1};
		bool desired = false;
		bool requestStarted = false;
		bool loadFailed = false;
		bool active = false;
		bool retiring = false;
		uint8_t retireReadyFrames = 0;
		Ref<T3DHLODPackage> package;
		RID instance;
	} t3dentry_t;

	typedef struct t3dnodePolicy_s {
		int64_t spawnId = 0;
		uint8_t minLevel = 0;
	} t3dnodePolicy_t;

	T3DTerrain *m_pTerrain = nullptr;
	Ref<T3DHLODManifest> m_manifest;
	eastl::vector<t3dentry_t> m_entries;
	eastl::vector<int> m_roots;
	eastl::vector<uint8_t> m_instancerMinLevels;
	eastl::vector<t3dnodePolicy_t> m_nodePolicies[TERRAIN3D_MAX_REGION_SLOTS];
	uint8_t m_regionRepresentationLevel[TERRAIN3D_MAX_REGION_SLOTS] = {};
	eastl::bitset<TERRAIN3D_MAX_REGION_SLOTS> m_managedRegions;
	eastl::bitset<TERRAIN3D_MAX_REGION_SLOTS> m_detailRequiredRegions;
	bool m_bEnabled = true;
	String m_manifestPath;
	real_t m_fTargetErrorPixels = 3.0f;
	real_t m_fHysteresis = 0.20f;
	int m_nMaxLevel = 6;
	int m_nLoadBudget = 2;
	int m_nUnloadBudget = 4;
	int m_nMemoryBudgetMb = 16;
	int m_nMaxShadowLevel = 3;
	int m_nForcedDebugLevel = -1;
	int64_t m_nResidentEstimatedBytes = 0;
	int m_nLoadsLastTick = 0;
	int m_nUnloadsLastTick = 0;
	int m_nRequestsLastTick = 0;
	int m_nFailuresLastTick = 0;
	int m_nCapacityDeferredLastTick = 0;
	int64_t m_nTransitionOvercommitBytesLastTick = 0;
	Vector3 m_lastSelectionTarget = Vector3();
	real_t m_fLastSelectionProjectionValue = -1.0f;
	real_t m_fLastSelectionViewportHeight = -1.0f;
	int m_nLastSelectionProjection = -1;
	bool m_bHasSelectionContext = false;
	bool m_bSelectionDirty = true;

	static int floorDiv( int value, int divisor );
	static int regionIndex( const Vector2i &rRegion );
	static Vector2i regionFromIndex( int index );
	int findEntry( int level, const Vector2i &rCluster ) const;
	void clearRuntime( bool bNotify );
	bool compileManifest( const Ref<T3DHLODManifest> &rManifest );
	void buildHierarchy( void );
	bool packageAcceptable( const t3dentry_t &rEntry, const Vector3 &rLocalTarget ) const;
	void selectRecursive( int index, const Vector3 &rLocalTarget );
	void selectFallbackChildren( int index, const Vector3 &rLocalTarget );
	void clearDesiredDescendants( int index );
	bool hasActiveDescendant( int index ) const;
	bool replacementReady( const t3dentry_t &rEntry ) const;
	bool requestOrActivate( int index );
	void deactivate( int index );
	void publishRepresentationMap( void );
	void notifyRepresentationChanges( const uint8_t *pOldLevels );

public:
	T3DHLODStreamer( void ) = default;
	~T3DHLODStreamer( void );
	void initialize( T3DTerrain *pTerrain );
	void destroy( void );
	void suspend( void );
	bool reloadManifest( void );
	void updateResidency( const Vector3 &rTargetPosition, bool bForce = false );

	void setEnabled( bool bEnabled ); bool isEnabled( void ) const { return m_bEnabled; }
	void setManifestPath( const String &rPath ); String getManifestPath( void ) const { return m_manifestPath; }
	void setTargetErrorPixels( real_t fPixels ); real_t getTargetErrorPixels( void ) const { return m_fTargetErrorPixels; }
	void setHysteresis( real_t fRatio ); real_t getHysteresis( void ) const { return m_fHysteresis; }
	void setMaxLevel( int nLevel ); int getMaxLevel( void ) const { return m_nMaxLevel; }
	void setLoadBudget( int nBudget ) { m_nLoadBudget = CLAMP( nBudget, 0, 64 ); }
	int getLoadBudget( void ) const { return m_nLoadBudget; }
	void setUnloadBudget( int nBudget ) { m_nUnloadBudget = CLAMP( nBudget, 0, 64 ); }
	int getUnloadBudget( void ) const { return m_nUnloadBudget; }
	void setMemoryBudgetMb( int nMb ) { m_nMemoryBudgetMb = CLAMP( nMb, 1, 4096 ); }
	int getMemoryBudgetMb( void ) const { return m_nMemoryBudgetMb; }
	void setMaxShadowLevel( int nLevel ) { m_nMaxShadowLevel = CLAMP( nLevel, 0, 6 ); }
	int getMaxShadowLevel( void ) const { return m_nMaxShadowLevel; }
	void setForcedDebugLevel( int nLevel ); int getForcedDebugLevel( void ) const { return m_nForcedDebugLevel; }

	bool isRegionManaged( const Vector2i &rRegion ) const;
	bool isRegionDetailRequired( const Vector2i &rRegion ) const;
	bool isRegionSuppressed( const Vector2i &rRegion, int nMinimumLevel = 1 ) const;
	uint8_t getInstancerMinimumLevel( const Vector2i &rRegion, int nMeshId ) const;
	uint8_t getNodeMinimumLevel( const Vector2i &rRegion, int64_t nSpawnId ) const;
	uint8_t getRegionRepresentationLevel( const Vector2i &rRegion ) const;
	int64_t getResidentEstimatedBytes( void ) const { return m_nResidentEstimatedBytes; }
	bool isResidencyIdle( void ) const { return m_nLoadsLastTick == 0 && m_nRequestsLastTick == 0 && m_nUnloadsLastTick == 0; }
	Dictionary getStats( void ) const;
	Array getDebugEntries( void ) const;

protected:
	static void _bind_methods( void );
};
using Terrain3DHLODStreamer = T3DHLODStreamer;
#endif
