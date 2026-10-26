// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_HLOD_STREAMER_H
#define TERRAIN3D_HLOD_STREAMER_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_eastl_helpers.h"
#include "eastl_config.h"
#include "hlod/terrain_3d_hlod_manifest.h"
#include "hlod/terrain_3d_hlod_package.h"
#include "hlod/terrain_3d_hlod_runtime_codec.h"
#include "instances/terrain_3d_instance_mutation.h"
#include "rendering/rd/terrain_3d_rd_instance_renderer.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif
class Terrain3D;

typedef struct t3dEntry_s : public t3dHlodEntryDescriptor_t {
	uint32_t m_nDesiredGeneration = 0u;
	bool m_bRequestStarted = false;
	bool m_bLoadFailed = false;
	bool m_bActive = false;
	bool m_bRetiring = false;
	uint8_t m_nRetireReadyFrames = 0;
	int m_iActiveListPosition = -1;
	int m_iLoadingListPosition = -1;
	int m_iRetiringListPosition = -1;
	Ref<Terrain3DHLODPackage> m_ackage;
} t3dEntry_t;

typedef struct t3dNodePolicy_s {
	int64_t m_nSpawnId = 0;
	uint8_t m_nMinLevel = 0;
} t3dNodePolicy_t;

typedef struct t3dHlodInstancerRepresentationState_s {
	uint8_t m_nMinimumLevel = 0;
	uint8_t m_nActiveLevel = 0;
	uint8_t m_nResidentLevel = 0;
	uint8_t m_nRetiringLevel = 0;
} t3dHlodInstancerRepresentationState_t;

class Terrain3DHLODStreamer : public Object {
	GDCLASS( Terrain3DHLODStreamer, Object );
	CLASS_NAME();

	Terrain3D *m_pTerrain = NULL;
	Ref<Terrain3DHLODManifest> m_manifest;
	eastl::vector<t3dEntry_t, Terrain3DEastlAllocator> m_entries;
	eastl::vector<int, Terrain3DEastlAllocator> m_roots;
	eastl::vector<int, Terrain3DEastlAllocator> m_activeEntries;
	eastl::vector<int, Terrain3DEastlAllocator> m_loadingEntries;
	eastl::vector<int, Terrain3DEastlAllocator> m_retiringEntries;
	eastl::vector<int, Terrain3DEastlAllocator> m_loadCandidates;
	eastl::vector<uint8_t, Terrain3DEastlAllocator> m_instancerMinLevels;
	eastl::vector<t3dNodePolicy_t, Terrain3DEastlAllocator> m_nodePolicies;
	uint32_t m_nodePolicyOffsets[s_nRegionSlots + 1]{};
	uint8_t m_szRegionRepresentationLevel[s_nRegionSlots] = {};
	uint8_t m_szRegionResidentRepresentationLevel[s_nRegionSlots] = {};
	uint8_t m_szRegionRetiringRepresentationLevel[s_nRegionSlots] = {};
	eastl::bitset<s_nRegionSlots> m_managedRegions;
	eastl::bitset<s_nRegionSlots> m_detailRequiredRegions;
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
	uint64_t m_nRdPublicationGeneration = 1u;
	uint64_t m_nRepresentationGeneration = 1u;
	uint32_t m_nDesiredGeneration = 1u;
	uint32_t m_nTransitionEntriesExaminedLastTick = 0u;
	mutable uint64_t m_nCapacityMissCount = 0u;
	uint64_t m_nColdRebuildCount = 0u;
	uint64_t m_nInstanceMutationInvalidations = 0u;

	static int floorDiv( int value, int divisor );
	static int regionIndex( const Vector2i &rRegion );
	static Vector2i regionFromIndex( int index );
	int findEntry( int level, const Vector2i &rCluster ) const;
	void clearRuntime( bool bNotify, bool bReleaseStorage = false );
	bool compileManifest( const Ref<Terrain3DHLODManifest> &rManifest );
	bool compilePackedManifest( const Ref<Terrain3DHLODManifest> &rManifest );
	bool compileLegacyNodePolicies( const Ref<Terrain3DHLODManifest> &rManifest );
	bool buildHierarchy( void );
	bool packageAcceptable( const t3dEntry_t &rEntry, const Vector3 &rLocalTarget ) const;
	EA_FORCE_INLINE bool isDesired( const t3dEntry_t &rEntry ) const {
		return rEntry.m_nDesiredGeneration == m_nDesiredGeneration;
	}
	bool prepareTransitionStorageCold( void );
	void beginSelectionGeneration( void );
	void markDesired( int index );
	void addActiveEntry( int index );
	void removeActiveEntry( int index );
	void addLoadingEntry( int index );
	void removeLoadingEntry( int index );
	void addRetiringEntry( int index );
	void removeRetiringEntry( int index );
	void refreshRetiringFromActive( void );
	void selectRecursive( int index, const Vector3 &rLocalTarget );
	void selectFallbackChildren( int index, const Vector3 &rLocalTarget );
	void clearDesiredDescendants( int index );
	bool hasActiveDescendant( int index ) const;
	bool replacementReady( const t3dEntry_t &rEntry ) const;
	bool requestOrActivate( int index );
	void deactivate( int index );
	void publishRepresentationMap( void );
	void notifyRepresentationChanges( const uint8_t *pOldLevels );
	void markRdPublicationDirty( void );
	static uint64_t rdMeshSourceKey( int nEntryIndex );

public:
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	Terrain3DHLODStreamer( void ) = default;
	EA_FORCE_INLINE ~Terrain3DHLODStreamer( void ) {
		destroy();
	}
	void initialize( Terrain3D *pTerrain );
	void destroy( void );
	void suspend( void );
	bool reloadManifest( void );
	void updateResidency( const Vector3 &rTargetPosition, bool bForce = false );
	void onInstanceMutation( const t3dInstanceMutationEvent_t &rEvent );

	EA_FORCE_INLINE void setEnabled( bool bEnabled ) {
		if ( m_bEnabled == bEnabled ) {
			return;
		}
		m_bEnabled = bEnabled;
		if ( !bEnabled ) {
			suspend();
		} else if ( m_manifest.is_null() || m_entries.empty() ) {
			reloadManifest();
		}
	}
	EA_FORCE_INLINE bool isEnabled( void ) const {
		return m_bEnabled;
	}
	EA_FORCE_INLINE void setManifestPath( const String &rPath ) {
		const String path = rPath.strip_edges();
		if ( path == m_manifestPath ) {
			return;
		}
		m_manifestPath = path;
		if ( m_pTerrain ) {
			reloadManifest();
		}
	}
	EA_FORCE_INLINE String getManifestPath( void ) const {
		return m_manifestPath;
	}
	EA_FORCE_INLINE void setTargetErrorPixels( real_t fPixels ) {
		const real_t value = CLAMP( fPixels, 0.25f, 64.0f );
		if ( Math::is_equal_approx( m_fTargetErrorPixels, value ) ) {
			return;
		}
		m_fTargetErrorPixels = value;
		m_bSelectionDirty = true;
	}
	EA_FORCE_INLINE real_t getTargetErrorPixels( void ) const {
		return m_fTargetErrorPixels;
	}
	EA_FORCE_INLINE void setHysteresis( real_t fRatio ) {
		const real_t value = CLAMP( fRatio, 0.0f, 0.75f );
		if ( Math::is_equal_approx( m_fHysteresis, value ) ) {
			return;
		}
		m_fHysteresis = value;
		m_bSelectionDirty = true;
	}
	EA_FORCE_INLINE real_t getHysteresis( void ) const {
		return m_fHysteresis;
	}
	EA_FORCE_INLINE void setMaxLevel( int nLevel ) {
		const int value = CLAMP( nLevel, 1, 6 );
		if ( m_nMaxLevel == value ) {
			return;
		}
		m_nMaxLevel = value;
		m_bSelectionDirty = true;
	}
	EA_FORCE_INLINE int getMaxLevel( void ) const {
		return m_nMaxLevel;
	}
	EA_FORCE_INLINE void setLoadBudget( int nBudget ) {
		m_nLoadBudget = CLAMP( nBudget, 0, 64 );
	}
	EA_FORCE_INLINE int getLoadBudget( void ) const {
		return m_nLoadBudget;
	}
	EA_FORCE_INLINE void setUnloadBudget( int nBudget ) {
		m_nUnloadBudget = CLAMP( nBudget, 0, 64 );
	}
	EA_FORCE_INLINE int getUnloadBudget( void ) const {
		return m_nUnloadBudget;
	}
	EA_FORCE_INLINE void setMemoryBudgetMb( int nMb ) {
		m_nMemoryBudgetMb = CLAMP( nMb, 1, 4096 );
	}
	EA_FORCE_INLINE int getMemoryBudgetMb( void ) const {
		return m_nMemoryBudgetMb;
	}
	EA_FORCE_INLINE void setMaxShadowLevel( int nLevel ) {
		const int value = CLAMP( nLevel, 0, 6 );
		if ( m_nMaxShadowLevel == value ) {
			return;
		}
		m_nMaxShadowLevel = value;
		markRdPublicationDirty();
	}
	EA_FORCE_INLINE int getMaxShadowLevel( void ) const {
		return m_nMaxShadowLevel;
	}
	EA_FORCE_INLINE void setForcedDebugLevel( int nLevel ) {
		const int value = CLAMP( nLevel, -1, 6 );
		if ( m_nForcedDebugLevel == value ) {
			return;
		}
		m_nForcedDebugLevel = value;
		m_bSelectionDirty = true;
	}
	EA_FORCE_INLINE int getForcedDebugLevel( void ) const {
		return m_nForcedDebugLevel;
	}

	EA_FORCE_INLINE bool isRegionManaged( const Vector2i &rRegion ) const {
		const int index = regionIndex( rRegion );
		return index >= 0 && m_managedRegions.test( ( size_t )index );
	}
	EA_FORCE_INLINE bool isRegionDetailRequired( const Vector2i &rRegion ) const {
		const int index = regionIndex( rRegion );
		return index < 0 || !m_managedRegions.test( ( size_t )index ) || m_detailRequiredRegions.test( ( size_t )index );
	}
	EA_FORCE_INLINE bool isRegionSuppressed( const Vector2i &rRegion, int nMinimumLevel = 1 ) const {
		return getRegionRepresentationLevel( rRegion ) >= CLAMP( nMinimumLevel, 1, 6 );
	}
	EA_FORCE_INLINE uint8_t getInstancerMinimumLevel( const Vector2i &rRegion, int nMeshId ) const {
		const int region = regionIndex( rRegion );
		if ( region < 0 || nMeshId < 0 || nMeshId >= s_nMaxMeshAssets ) {
			return 0;
		}
		const size_t offset = ( size_t )region * s_nMaxMeshAssets + ( size_t )nMeshId;
		return offset < m_instancerMinLevels.size() ? m_instancerMinLevels[offset] : 0;
	}
	EA_FORCE_INLINE t3dHlodInstancerRepresentationState_t getInstancerRepresentationState(
	        const Vector2i &rRegion, const int nMeshId ) const {
		t3dHlodInstancerRepresentationState_t state;
		const int region = regionIndex( rRegion );
		if ( region < 0 || nMeshId < 0 || nMeshId >= s_nMaxMeshAssets ) {
			return state;
		}
		const size_t offset = ( size_t )region * s_nMaxMeshAssets + ( size_t )nMeshId;
		state.m_nMinimumLevel = offset < m_instancerMinLevels.size() ? m_instancerMinLevels[offset] : 0;
		state.m_nActiveLevel = m_szRegionRepresentationLevel[region];
		state.m_nResidentLevel = m_szRegionResidentRepresentationLevel[region];
		state.m_nRetiringLevel = m_szRegionRetiringRepresentationLevel[region];
		return state;
	}
	uint8_t getNodeMinimumLevel( const Vector2i &rRegion, int64_t nSpawnId ) const;
	EA_FORCE_INLINE uint8_t getRegionRepresentationLevel( const Vector2i &rRegion ) const {
		const int index = regionIndex( rRegion );
		return index >= 0 ? m_szRegionRepresentationLevel[index] : 0;
	}
	EA_FORCE_INLINE int64_t getResidentEstimatedBytes( void ) const {
		return m_nResidentEstimatedBytes;
	}
	EA_FORCE_INLINE bool isResidencyIdle( void ) const {
		return m_nLoadsLastTick == 0 && m_nRequestsLastTick == 0 && m_nUnloadsLastTick == 0;
	}
	EA_FORCE_INLINE uint64_t getRdPublicationGeneration( void ) const {
		return m_nRdPublicationGeneration;
	}
	EA_FORCE_INLINE uint64_t getRepresentationGeneration( void ) const {
		return m_nRepresentationGeneration;
	}
	bool fillRdMeshSources( eastl::vector<t3dRdExternalMeshSource_t, Terrain3DEastlAllocator> &rSources );
	bool buildRdInstanceSnapshot( t3dRdInstanceSceneSnapshot_t &rSnapshot ) const;
	EA_FORCE_INLINE size_t getRdPublicationCapacity( void ) const {
		return m_entries.size();
	}
	Dictionary getStats( void ) const;
	Array getDebugEntries( void ) const;

protected:
	static void _bind_methods( void );
};
#endif // TERRAIN3D_HLOD_STREAMER_H
