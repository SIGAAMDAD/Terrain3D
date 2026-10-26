// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_INSTANCER_CLASS_H
#define TERRAIN3D_INSTANCER_CLASS_H

#include "Terrain3DTypes.h"

#include "core/utility/terrain_3d_hash.h"
#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_owner.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "core/config/terrain_3d_limits.h"
#include "data/model/terrain_3d_region.h"
#include "eastl_config.h"
#include "instances/contracts/terrain_3d_instance_layout.h"
#include "instances/contracts/terrain_3d_instance_mutation.h"
#include "instances/storage/terrain_3d_instance_publication_builder.h"
#include "rendering/rd/instances/terrain_3d_rd_instance_renderer.h"

#include <EASTL/bitset.h>
#include <stdint.h>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include "assets/catalog/terrain_3d_assets.h"
#include "data/model/terrain_3d_data.h"
#include "data/sampling/terrain_3d_sampler.h"


// t3dInstancerMode_e centralized in Terrain3DTypes.h.

typedef struct t3dInstanceRegionAppend_s {
	Terrain3DRegion *m_pRegion = NULL;
	int m_nMeshId = -1;
	const Transform3D *m_pTransforms = NULL;
	size_t m_nTransformCount = 0u;
	const Color *m_pColors = NULL;
	size_t m_nColorCount = 0u;
	bool m_bGlobalSpace = false;
	bool m_bUpdate = true;
	bool m_bBackup = true;
	uint32_t m_nOwner = 0u;
	uint16_t m_nBehaviorFlags = T3D_INSTANCE_LEGACY_BEHAVIOR;
	const uint16_t *m_pBehaviorFlags = NULL;
	size_t m_nBehaviorCount = 0u;
} t3dInstanceRegionAppend_t;

typedef struct t3dRenderCellKey_s {

	Vector2i m_region;
	Vector2i m_cell;
	int m_nMeshId = -1;
	int m_nLod = 0;
	bool operator==( const t3dRenderCellKey_s &rOther ) const {
		return m_region == rOther.m_region && m_cell == rOther.m_cell && m_nMeshId == rOther.m_nMeshId && m_nLod == rOther.m_nLod;
	}
} t3dRenderCellKey_t;


typedef struct t3dRenderCellKeyHash_s {

	size_t operator()( const t3dRenderCellKey_t &rKey ) const {
		size_t h = t3dVector2iHash_t{}( rKey.m_region );
		h ^= t3dVector2iHash_t{}( rKey.m_cell ) + ( size_t )( T3D_HASH_GOLDEN_RATIO32 ) + ( h << 6 ) + ( h >> 2 );
		h ^= ( size_t )( ( uint32_t )( rKey.m_nMeshId ) ) * ( size_t )( T3D_HASH_MIX32 );
		h ^= ( size_t )( ( uint32_t )( rKey.m_nLod ) ) * ( size_t )( T3D_HASH_MIX32_SECONDARY );
		return h;
	}
} t3dRenderCellKeyHash_t;


typedef struct t3dRenderCellKeyLess_s {

	EA_FORCE_INLINE bool operator()( const t3dRenderCellKey_t &a, const t3dRenderCellKey_t &b ) const {
		if ( a.m_region.x != b.m_region.x ) {
			return a.m_region.x < b.m_region.x;
		}
		if ( a.m_region.y != b.m_region.y ) {
			return a.m_region.y < b.m_region.y;
		}
		if ( a.m_cell.x != b.m_cell.x ) {
			return a.m_cell.x < b.m_cell.x;
		}
		if ( a.m_cell.y != b.m_cell.y ) {
			return a.m_cell.y < b.m_cell.y;
		}
		if ( a.m_nMeshId != b.m_nMeshId ) {
			return a.m_nMeshId < b.m_nMeshId;
		}
		return a.m_nLod < b.m_nLod;
	}
} t3dRenderCellKeyLess_t;


typedef struct t3dCellUpdateKey_s {

	Vector2i m_region;
	Vector2i m_cell;
	int m_nMeshId = -1;
	bool operator==( const t3dCellUpdateKey_s &rOther ) const {
		return m_region == rOther.m_region && m_cell == rOther.m_cell && m_nMeshId == rOther.m_nMeshId;
	}
} t3dCellUpdateKey_t;


typedef struct t3dCellUpdateKeyHash_s {

	size_t operator()( const t3dCellUpdateKey_t &rKey ) const {
		t3dRenderCellKey_t key{ rKey.m_region, rKey.m_cell, rKey.m_nMeshId, 0 };
		return t3dRenderCellKeyHash_t{}( key );
	}
} t3dCellUpdateKeyHash_t;


typedef struct t3dCellUpdateKeyLess_s {

	EA_FORCE_INLINE bool operator()( const t3dCellUpdateKey_t &a, const t3dCellUpdateKey_t &b ) const {
		if ( a.m_region.x != b.m_region.x ) {
			return a.m_region.x < b.m_region.x;
		}
		if ( a.m_region.y != b.m_region.y ) {
			return a.m_region.y < b.m_region.y;
		}
		if ( a.m_cell.x != b.m_cell.x ) {
			return a.m_cell.x < b.m_cell.x;
		}
		if ( a.m_cell.y != b.m_cell.y ) {
			return a.m_cell.y < b.m_cell.y;
		}
		return a.m_nMeshId < b.m_nMeshId;
	}
} t3dCellUpdateKeyLess_t;


typedef struct t3dRegionUpdateState_s {

	bool m_bAllMeshes = false;
	eastl::bitset<T3D_MAX_MESH_ASSETS> m_meshes;
} t3dRegionUpdateState_t;


// t3dRenderCell_s centralized in Terrain3DTypes.h.


// t3dUsableHeight_s centralized in Terrain3DTypes.h.


typedef struct t3dCellInterval_s {

	int m_nFirst = 1;
	int m_nLast = 0;
	EA_FORCE_INLINE bool valid( void ) const {
		return m_nFirst <= m_nLast;
	}
} t3dCellInterval_t;


typedef struct t3dTransitionBucketKey_s {

	Vector2i m_bucket;
	int m_nMeshId = -1;
	bool operator==( const t3dTransitionBucketKey_s &rOther ) const {
		return m_bucket == rOther.m_bucket && m_nMeshId == rOther.m_nMeshId;
	}
} t3dTransitionBucketKey_t;


typedef struct t3dTransitionBucketKeyHash_s {

	size_t operator()( const t3dTransitionBucketKey_t &rKey ) const {
		size_t h = t3dVector2iHash_t{}( rKey.m_bucket );
		h ^= ( size_t )( ( uint32_t )( rKey.m_nMeshId ) ) * ( size_t )( T3D_HASH_MIX32 );
		return h;
	}
} t3dTransitionBucketKeyHash_t;


typedef struct t3dTransitionBucketKeyLess_s {

	EA_FORCE_INLINE bool operator()( const t3dTransitionBucketKey_t &a, const t3dTransitionBucketKey_t &b ) const {
		if ( a.m_bucket.x != b.m_bucket.x ) {
			return a.m_bucket.x < b.m_bucket.x;
		}
		if ( a.m_bucket.y != b.m_bucket.y ) {
			return a.m_bucket.y < b.m_bucket.y;
		}
		return a.m_nMeshId < b.m_nMeshId;
	}
} t3dTransitionBucketKeyLess_t;


typedef struct t3dPendingPlacement_s {
	Terrain3DRegion *m_pRegion = NULL;
	Vector2i m_cell;
	Transform3D m_transform;
	Color m_color = Color( 1.0f, 1.0f, 1.0f, 1.0f );
	uint32_t m_nOwner = 0;
	uint16_t m_nBehaviorFlags = ( uint16_t )( FOLLOW_TERRAIN_HEIGHT | MANUAL_ORIENTATION );
} t3dPendingPlacement_t;


// t3dLogicalCellState_s centralized in Terrain3DTypes.h.


// t3dRdCellRange_s centralized in Terrain3DTypes.h.



// t3dRegionList_s centralized in Terrain3DTypes.h.


// t3dRenderRegionNode_s centralized in Terrain3DTypes.h.


typedef struct t3dLogicalRegionNode_s {

	t3dCellUpdateKey_t m_key{};
	uint32_t m_nNext = UINT32_MAX;
	uint32_t m_nPrev = UINT32_MAX;
} t3dLogicalRegionNode_t;


// t3dTransitionBucketState_s centralized in Terrain3DTypes.h.


typedef struct t3dTransitionCellNode_s {

	t3dCellUpdateKey_t m_key{};
	uint32_t m_nNext = UINT32_MAX;
} t3dTransitionCellNode_t;


typedef eastl::vector_map<t3dRenderCellKey_t, size_t, t3dRenderCellKeyLess_t, Terrain3DEastlAllocator> t3dRenderCellIndex_t;
typedef eastl::vector_map<t3dCellUpdateKey_t, t3dLogicalCellState_t, t3dCellUpdateKeyLess_t, Terrain3DEastlAllocator> t3dLogicalCellMap_t;
typedef eastl::vector_map<t3dTransitionBucketKey_t, t3dTransitionBucketState_t, t3dTransitionBucketKeyLess_t, Terrain3DEastlAllocator>
        t3dTransitionBucketMap_t;
typedef eastl::vector_set<t3dCellUpdateKey_t, t3dCellUpdateKeyLess_t, Terrain3DEastlAllocator> t3dCellUpdateSet_t;
typedef eastl::vector_map<t3dCellUpdateKey_t, t3dRdCellRange_t, t3dCellUpdateKeyLess_t, Terrain3DEastlAllocator> t3dRdSnapshotRangeMap_t;

// Authored records belong to Terrain3DInstanceStore. The instancer privately
// owns operational indices, queues and policy; immutable publication decisions
// and diagnostics belong to this encapsulated builder.
class Terrain3DInstanceSnapshotBuilder {
	CLASS_NAME_STATIC( "Terrain3DInstanceSnapshotBuilder" );
private:
	uint64_t m_nRdPublicationGeneration = 1u;
	uint64_t m_nHlodRepresentationRequiredGeneration = 0u;
	mutable uint64_t m_nHlodRepresentationBuiltGeneration = 0u;
	mutable uint64_t m_nLastAssetSourceFailureSignature = 0u;
	mutable t3dInstancePublicationDiagnostics_t m_rdPublicationDiagnostics;
public:
	void markRdPublicationDirty( void );
	bool build( const t3dInstancePublicationBuildContext_t &rContext, t3dRdInstanceSceneSnapshot_t &rSnapshot ) const;
	uint64_t generation( void ) const { return m_nRdPublicationGeneration; }
	const t3dInstancePublicationDiagnostics_t &diagnostics( void ) const { return m_rdPublicationDiagnostics; }
	void resetSourceFailure( void ) { m_nLastAssetSourceFailureSignature = 0u; }
	void requireHlodRepresentation( uint64_t nGeneration ) {
		if ( nGeneration > m_nHlodRepresentationRequiredGeneration ) { m_nHlodRepresentationRequiredGeneration = nGeneration; }
		markRdPublicationDirty();
	}
	bool isHlodRepresentationReady( void ) const { return m_nHlodRepresentationBuiltGeneration >= m_nHlodRepresentationRequiredGeneration; }
	uint64_t requiredHlodGeneration( void ) const { return m_nHlodRepresentationRequiredGeneration; }
	uint64_t builtHlodGeneration( void ) const { return m_nHlodRepresentationBuiltGeneration; }
};



struct t3dInstanceInitialPolicy_t {
	int m_nCellSize;
	bool m_bCellStreaming;
	bool m_bSingleLodBuffers;
	real_t m_fMargin;
	real_t m_fUpdateDistance;
	uint32_t m_nUpdateBudget;
};

class Terrain3DInstancer : public Object {
	GDCLASS( Terrain3DInstancer, Object );
	CLASS_NAME();

private:
	Terrain3DOwner *m_pTerrain = NULL;

	// Runtime indices use EASTL contiguous sorted containers. Capacity is established
	// during cold rebuilds; mutating runtime paths reject inserts once that capacity
	// is exhausted instead of asking EASTL to grow implicitly.

	// Operational indices and queues are owned privately by the instancer.
	t3dLogicalCellMap_t m_logicalCellCounts;
	t3dRegionList_t m_szRegionLogicalLists[T3D_INSTANCE_REGION_TABLE_CAPACITY] = {};
	eastl::vector<t3dLogicalRegionNode_t, Terrain3DEastlAllocator> m_logicalRegionNodes;
	uint32_t m_nLogicalRegionFreeHead = UINT32_MAX;
	eastl::bitset<T3D_INSTANCE_REGION_TABLE_CAPACITY> m_regionLogicalMask;
	uint64_t m_szLogicalMeshInstanceTotals[T3D_MAX_MESH_ASSETS] = {};
	eastl::bitset<T3D_MAX_MESH_ASSETS> m_dirtyLogicalMeshCounts;
	bool m_bPublishingLogicalMeshCounts = false;
	eastl::vector<Transform3D, Terrain3DEastlAllocator> m_placementTransforms;
	eastl::vector<Color, Terrain3DEastlAllocator> m_placementColors;
	eastl::vector<uint16_t, Terrain3DEastlAllocator> m_placementBehaviorFlags;
	eastl::vector<t3dPendingPlacement_t, Terrain3DEastlAllocator> m_placementPending;
	eastl::fixed_vector<Terrain3DRegion *, T3D_REGION_SLOTS, false> m_touchedRegions;
	t3dRegionUpdateState_t m_szRegionUpdates[T3D_INSTANCE_REGION_UPDATE_CAPACITY] = {};
	eastl::bitset<T3D_INSTANCE_REGION_UPDATE_CAPACITY> m_regionUpdateMask;
	eastl::fixed_vector<uint16_t, T3D_INSTANCE_REGION_UPDATE_CAPACITY, false> m_regionUpdateIndices;
	t3dCellUpdateSet_t m_streamingUpdates;
	t3dRegionUpdateState_t m_szActiveRegionUpdates[T3D_INSTANCE_REGION_UPDATE_CAPACITY] = {};
	eastl::bitset<T3D_INSTANCE_REGION_UPDATE_CAPACITY> m_activeRegionUpdateMask;
	eastl::fixed_vector<uint16_t, T3D_INSTANCE_REGION_UPDATE_CAPACITY, false> m_activeRegionUpdateIndices;
	t3dCellUpdateSet_t m_activeStreamingUpdates;
	eastl::vector<t3dCellUpdateKey_t, Terrain3DEastlAllocator> m_pendingCellUpdates;
	eastl::vector<t3dCellUpdateKey_t, Terrain3DEastlAllocator> m_buildingCellUpdates;
	eastl::bitset<T3D_INSTANCE_REGION_TABLE_CAPACITY> m_pendingRegionWorkMask;
	size_t m_iPendingCellUpdateCursor = 0;
	bool m_bMaterializingUpdates = false;
	bool m_bProcessingPendingUpdates = false;
	bool m_bPendingCancelRequested = false;
	uint64_t m_nCellUpdateUsecLastFrame = 0u;
	uint64_t m_nMaterializeUsecLastFrame = 0u;
	uint64_t m_nMultimeshUploadBytesLastFrame = 0u;
	uint64_t m_nInstanceRecordsUploadedLastFrame = 0u;
	uint32_t m_nQueueBeforeLastFrame = 0u;
	uint32_t m_nQueueAfterLastFrame = 0u;
	uint32_t m_nProcessedCellUpdatesLastFrame = 0u;
	uint32_t m_nSkippedCellUpdatesLastFrame = 0u;
	uint32_t m_nMultimeshUploadsLastFrame = 0u;
	uint32_t m_nNewRenderInstancesLastFrame = 0u;
	bool m_bStreamingSelectionDeferredLastFrame = false;
	eastl::bitset<T3D_MAX_MESH_ASSETS> m_globalMeshUpdates;
	bool m_bUpdateAll = false;
	bool m_bRebuildAll = false;
	uint64_t m_nDerivedMutationGeneration = 1u;
	eastl::vector<t3dRenderCell_t, Terrain3DEastlAllocator> m_renderCells;
	t3dRenderCellIndex_t m_iRenderCellIndex;
	t3dRegionList_t m_szRegionRenderLists[T3D_INSTANCE_REGION_TABLE_CAPACITY] = {};
	eastl::vector<t3dRenderRegionNode_t, Terrain3DEastlAllocator> m_renderRegionNodes;
	uint32_t m_nRenderRegionFreeHead = UINT32_MAX;
	eastl::bitset<T3D_INSTANCE_REGION_TABLE_CAPACITY> m_regionRenderMask;
	t3dTransitionBucketMap_t m_singleLodTransitionBuckets;
	eastl::vector<t3dTransitionCellNode_t, Terrain3DEastlAllocator> m_transitionCellNodes;
	uint32_t m_nTransitionCellFreeHead = UINT32_MAX;
	uint64_t m_nTransitionCellsExaminedLastScan = 0u;
	uint64_t m_nTransitionLodEvaluationsLastScan = 0u;
	uint64_t m_nTransitionsQueuedLastScan = 0u;
	uint64_t m_nTransitionScanUsecLastScan = 0u;
	uint64_t m_nTransitionBucketsExaminedLastScan = 0u;
	uint64_t m_nTransitionCandidateBucketsLastScan = 0u;
	uint64_t m_nTransitionFullRescanFallbackCount = 0u;
	uint32_t m_szMeshSettingsRevision[T3D_MAX_MESH_ASSETS] = {};
	real_t m_szMeshStreamingRadius[T3D_MAX_MESH_ASSETS] = {};
	eastl::fixed_vector<uint16_t, T3D_MAX_MESH_ASSETS, false> m_finiteStreamingMeshIds;
	real_t m_fMaxStreamingRadius = 0.0f;
	bool m_bStreamingCacheDirty = true;
	size_t m_nRenderCellCapacity = 0u;
	size_t m_nLogicalCellCapacity = 0u;
	size_t m_nTransitionBucketCapacity = 0u;
	size_t m_nCellUpdateQueueCapacity = 0u;
	size_t m_nRequestedMutationLogicalCapacity = 0u;
	bool m_bRuntimeStorageRebuildPending = false;
	uint64_t m_nCapacityMissCount = 0u;
	uint64_t m_nColdRebuildCount = 0u;
	uint64_t m_nOwnerGrowthDeniedCount = 0u;
	t3dInstancerMode_t m_mode = NORMAL;
	uint32_t m_nDensityCounter = 0;
	int m_nCellSize = T3D_INSTANCE_DEFAULT_CELL_SIZE;
	uint8_t m_nCellShift = 5u;
	bool m_bCellStreamingEnabled = true;
	bool m_bSingleLodInstanceBuffers = true;
	real_t m_fCellStreamingMargin = 64.f;
	real_t m_fCellStreamingStep = 16.f;
	uint32_t m_nUpdateCellBudget = 32;
	int m_nRuntimeUpdateCellBudgetOverride = -1;
	Vector3 m_lastStreamPosition = s_v3Zero;
	bool m_bStreamPositionValid = false;
	Terrain3DInstanceSnapshotBuilder m_snapshotBuilder;

	static t3dRenderCellKey_t makeRenderKey( const t3dRenderCell_t &rCell );
	void markRdPublicationDirty( void );
	void emitDerivedMutation( const Vector2i &rRegion, const Vector2i &rCell, int nMeshId, uint32_t nFlags, uint32_t nSourceRevision = 0u );
	bool requestRuntimeCapacityForMutation( size_t nAdditionalRecords );
	bool rebuildRuntimeStorageCapacities( void );
	bool appendBuildingCellUpdate( const t3dCellUpdateKey_t &rKey );
	bool insertStreamingUpdate( const t3dCellUpdateKey_t &rKey );
	bool reportRuntimeCapacityExceeded( const char *pszStorage, size_t nRequired, size_t nCapacity );
	bool buildAuthoritativeRdInstanceSnapshot( t3dRdInstanceSceneSnapshot_t &rSnapshot ) const;
	static int getRegionTableIndex( const Vector2i &vRegion );
	uint32_t allocateRenderRegionNode( size_t iRenderCell, uint16_t nRegionIndex );
	void releaseRenderRegionNode( uint32_t nNode, uint16_t nRegionIndex );
	uint32_t allocateLogicalRegionNode( const t3dCellUpdateKey_t &rKey, uint16_t nRegionIndex );
	void releaseLogicalRegionNode( uint32_t nNode, uint16_t nRegionIndex );
	uint32_t allocateTransitionCellNode( const t3dCellUpdateKey_t &rKey, uint32_t nNext );
	void releaseTransitionCellNode( uint32_t nNode );
	void clearRegionRenderLists( void );
	void clearRegionLogicalLists( void );
	t3dRenderCell_t *findRenderCell( const Vector2i &region, int nMeshId, int nLod, const Vector2i &cell );
	t3dRenderCell_t *getOrCreateRenderCell( const Vector2i &region, int nMeshId, int nLod, const Vector2i &cell );
	void eraseRenderCell( const Vector2i &region, int nMeshId, int nLod, const Vector2i &cell );
	void eraseRenderCellAt( size_t iIndex );
	void destroyRenderCells( void );
	void resetRuntimeState( bool bClearLogicalCounts );
	void reconcileRegionRenderCells( Terrain3DRegion *pRegion, int nMeshId );
	void markLogicalMeshCountDirty( int nMeshId );
	void setLogicalCellCount( const Vector2i &vRegion, int nMeshId, const Vector2i &vCell, size_t nCount );
	void publishLogicalMeshCount( int nMeshId );
	void flushLogicalMeshCounts( void );
	void eraseLogicalCellCount( const Vector2i &vRegion, int nMeshId, const Vector2i &vCell );
	void eraseLogicalCellsByRegion( const Vector2i &vRegion );
	void eraseLogicalCellsByLocation( const Vector2i &vRegion, int nMeshId );
	void clearLogicalCellCounts( void );
	void rebuildLogicalCountsFromStore( void );
	void reconcileLogicalRegion( Terrain3DRegion *pRegion, int nMeshId );
	bool swapLogicalMeshIds( int nSrcId, int nDstId );
	bool hasPendingUpdates( void ) const;
	bool hasPendingUpdatesForRegion( const Vector2i &rRegion ) const;
	void markPendingRegionWork( const Vector2i &rRegion );
	void rebuildPendingRegionWorkMask( void );
	void clearRegionUpdates( bool bActive );
	void snapshotRegionUpdates( void );
	t3dRegionUpdateState_t *getRegionUpdate( const Vector2i &vRegion );
	const t3dRegionUpdateState_t *findActiveRegionUpdate( const Vector2i &vRegion ) const;
	static Vector2i regionLocationFromUpdateIndex( uint16_t iIndex );
	void scheduleUpdateProcessing( void );
	void cancelPendingCellUpdates( void );
	void materializePendingUpdates( void );
	void enqueueRegionCellUpdates( Terrain3DRegion *pRegion, int nMeshId );
	void enqueueRegionMeshUpdates( Terrain3DRegion *pRegion, int nMeshId );
	void enqueueAllRegions( Terrain3DData *pData, int nMeshId );
	void processPendingCellUpdate( const t3dCellUpdateKey_t &rUpdate );
#ifdef DEBUG_ENABLED
	static bool reportInvariantFailure( const String &rMessage );
	bool validateInternalState( void ) const;
#endif
	void refreshStreamingCache( void );
	bool cellShouldBeResident( const Vector2i &region, const Vector2i &cell, int nMeshId ) const;
	bool cellShouldBeResidentAt( const Vector2i &region, const Vector2i &cell, int nMeshId, const Vector3 &rTargetPosition ) const;
	int selectCellLod(
	        const Vector2i &vRegion, const Vector2i &vCell, const Ref<Terrain3DMeshAsset> &rAsset, const Vector3 &rTargetPosition ) const;
	Vector2i getGlobalCell( const Vector2i &vRegion, const Vector2i &vCell ) const;
	t3dTransitionBucketKey_t getTransitionBucketKey( const t3dCellUpdateKey_t &rKey ) const;
	void trackSingleLodCell( const t3dCellUpdateKey_t &rKey );
	void untrackSingleLodCell( const t3dCellUpdateKey_t &rKey );
	void clearSingleLodTransitionBuckets( void );
	bool transitionBucketMayCrossBoundary( const t3dTransitionBucketKey_t &rBucket,
	        const Ref<Terrain3DMeshAsset> &rAsset,
	        const Vector3 &rPreviousPosition,
	        const Vector3 &rTargetPosition ) const;
	const t3dRenderCell_t *findSingleLodRenderCell( const t3dCellUpdateKey_t &rKey ) const;
	void queueSingleLodTransitions( const Vector3 &rPreviousPosition, const Vector3 &rTargetPosition );
	bool appendGlobalNative( int nMeshId,
	        const Transform3D *pTransforms,
	        size_t nTransformCount,
	        const Color *pColors,
	        size_t nColorCount,
	        bool bUpdate,
	        uint32_t owner = 0,
	        bool bBackup = true,
	        uint16_t nBehaviorFlags = T3D_INSTANCE_LEGACY_BEHAVIOR,
	        const uint16_t *pBehaviorFlags = NULL,
	        size_t nBehaviorCount = 0u );
	bool appendRegionNative( const t3dInstanceRegionAppend_t &rRequest );


	Vector2i getCellForLocalPosition( const Vector3 &rLocalPosition, uint8_t nCellShift ) const;
	bool buildRepartitionedStore(
	        const Terrain3DRegion *pRegion, const t3dInstanceLayout_t &rLayout, Terrain3DInstanceStore &rStore ) const;
	bool repartitionRegionStore( Terrain3DRegion *pRegion, const t3dInstanceLayout_t &rLayout );
	bool repartitionResidentRegions( const t3dInstanceLayout_t &rLayout );
	uint32_t getDensityCount( const real_t fDensity );
	int getMasterLod( const Ref<Terrain3DMeshAsset> &rMa ) const;
	void processUpdates( void );
	void updateMmiByRegion( Terrain3DRegion *pRegion, const int nMeshId );
	void updateMmiCell( Terrain3DRegion *pRegion, const int nMeshId, t3dInstanceCell_s &rCell, const Ref<Terrain3DMeshAsset> &rAsset );
	void ensureCellAabb( t3dInstanceCell_s &rCell, const AABB &rSourceMeshAabb );
	// Renderer teardown never mutates persistent content accounting. Callers that
	// change Terrain3DInstanceStore must update logical counts explicitly.
	void destroyMmisByRegion( const Vector2i &rRegionLoc );
	void destroyMmiByLocation( const Vector2i &rRegionLoc, int nMeshId );
	void destroyMmiByCell( const Vector2i &rRegionLoc, int nMeshId, Vector2i vCell, int nLod = INT32_MAX );
	void eraseMmiLod( const Vector2i &rRegionLoc, int nMeshId, const Vector2i &vCell, int nLod );
	static bool pendingPlacementRegionCellLess( const t3dPendingPlacement_t &rA, const t3dPendingPlacement_t &rB );
	static bool pendingPlacementCellLess( const t3dPendingPlacement_t &rA, const t3dPendingPlacement_t &rB );
	static int floorDivCell( int nValue, int nDivisor );
	t3dCellInterval_t streamingRowInterval(
	        const Vector3 &rPosition, real_t fRadius, int iGlobalZ, real_t fCellWorldSize, int nMinGlobalCell, int nMaxGlobalCell ) const;
	void queueStreamingCell( Terrain3DData *pData, int nMeshId, int iGlobalX, int iGlobalZ, int nCellsPerRegion );
	void queueStreamingInterval( Terrain3DData *pData, int nMeshId, int iGlobalZ, int nFirst, int nLast, int nCellsPerRegion );
	void queueStreamingDifference( Terrain3DData *pData,
	        int nMeshId,
	        int iGlobalZ,
	        const t3dCellInterval_t &rSource,
	        const t3dCellInterval_t &rOther,
	        int nCellsPerRegion );
	void backupRegion( const Ref<Terrain3DRegion> &rRegion );
	bool fillMultimeshBuffer(
	        const t3dInstanceCell_s &rCell, PackedFloat32Array &rBuffer, const AABB *pSourceMeshAabb = NULL, AABB *pCellAabb = NULL ) const;
	Vector2i getCell( const Vector3 &rGlobalPosition ) const;
	t3dUsableHeight_t getUsableHeight( Terrain3DSampler &rSampler,
	        const Vector3 &rGlobalPosition,
	        const Vector2 &rSlopeRange,
	        bool bOnCollision,
	        real_t fRaycastStart,
	        bool bSampleNormal = false ) const;
	int resnapInstancesToTerrain( const AABB &rAabb, bool bUseEditorCollision );

public:
	void configureInitialPolicy( const t3dInstanceInitialPolicy_t &rPolicy ) {
		m_nCellSize = rPolicy.m_nCellSize;
		m_bCellStreamingEnabled = rPolicy.m_bCellStreaming;
		m_bSingleLodInstanceBuffers = rPolicy.m_bSingleLodBuffers;
		m_fCellStreamingMargin = rPolicy.m_fMargin;
		m_fCellStreamingStep = rPolicy.m_fUpdateDistance;
		m_nUpdateCellBudget = rPolicy.m_nUpdateBudget;
	}
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	EA_FORCE_INLINE Terrain3DInstancer( void ) {
	}
	EA_FORCE_INLINE ~Terrain3DInstancer( void ) {
		destroy();
	}

	void serviceColdCapacity( bool bForce = false );
	void servicePendingUpdates( void );
	void initialize( Terrain3DOwner *pTerrain );
	void destroy( void );
	void suspendRendering( void );

	void clearByMesh( const int nMeshId );
	void clearByLocation( const Vector2i &rRegionLoc, const int nMeshId );
	void clearByRegion( const Ref<Terrain3DRegion> &rRegion, const int nMeshId );
	// Frees resident render-cell descriptors for one region without modifying stored instance data.
	// Used when a terrain region is temporarily removed and may later be restored by undo.
	void destroyRegionMmis( const Vector2i &rRegionLoc );

	void setMode( const t3dInstancerMode_t mode );
	EA_FORCE_INLINE t3dInstancerMode_t getMode( void ) const {
		return m_mode;
	}
	EA_FORCE_INLINE bool isEnabled( void ) const {
		return m_mode != INSTANCER_DISABLED;
	}

	void addInstances( const Vector3 &rGlobalPosition, const Dictionary &rParams );
	void removeInstances( const Vector3 &rGlobalPosition, const Dictionary &rParams );
	void addMultimesh(
	        const int nMeshId, const Ref<MultiMesh> &rMultimesh, const Transform3D &rXform = Transform3D(), const bool bUpdate = true );
	void addTransforms( const int nMeshId,
	        const TypedArray<Transform3D> &rXforms,
	        const PackedColorArray &rColors = PackedColorArray(),
	        const bool bUpdate = true );
	void addOwnedTransforms( const int nMeshId,
	        const TypedArray<Transform3D> &rXforms,
	        const int64_t nOwnerId,
	        const PackedColorArray &rColors = PackedColorArray(),
	        const bool bUpdate = true,
	        const bool bBackup = true );
	int removeOwnedInstances( const int64_t nOwnerId, const bool bUpdate = true, const bool bBackup = true );
	void appendLocation( const Vector2i &rRegionLoc,
	        const int nMeshId,
	        const TypedArray<Transform3D> &rXforms,
	        const PackedColorArray &rColors,
	        const bool bUpdate = true );
	void appendRegion( const Ref<Terrain3DRegion> &rRegion,
	        const int nMeshId,
	        const TypedArray<Transform3D> &rXforms,
	        const PackedColorArray &rColors,
	        const bool bUpdate = true );
	void updateTransforms( const AABB &rAabb );
	int snapInstancesToTerrain( const AABB &rAabb );
	int setInstanceFollowPolicy( const AABB &rAabb, bool bFollowHeight, bool bFollowNormal, bool bManualOrientation, int nMeshId = -1 );
	void updateStreaming( const Vector3 &rTargetPosition );
	void synchronizeStreamingTarget( const Vector3 &rTargetPosition );
	EA_FORCE_INLINE void deferStreamingSelectionForFrame( void ) {
		m_bStreamingSelectionDeferredLastFrame = true;
	}
	EA_FORCE_INLINE void resetStreamingResidency( void ) {
		suspendRendering();
	}
	EA_FORCE_INLINE int getRenderCellCount( void ) const {
		return ( int )( m_renderCells.size() );
	}
	EA_FORCE_INLINE int getPendingStreamingUpdateCount( void ) const {
		const size_t pending_count = m_pendingCellUpdates.size() > m_iPendingCellUpdateCursor
		        ? m_pendingCellUpdates.size() - m_iPendingCellUpdateCursor
		        : 0u;
		return ( int )( m_streamingUpdates.size() + m_activeStreamingUpdates.size() + pending_count );
	}
	void onHlodRepresentationChanged( const Vector2i &rRegion, uint64_t nRepresentationGeneration );
	void onAssetPolicyChanged( int nMeshId );
	bool areHlodDetailRegionsReady( const Vector2i &rRegionMin, const Vector2i &rRegionMax ) const;
	Dictionary getStats( void ) const;
	Dictionary debugValidateSingleLodTransitionCandidates( const Vector3 &rPreviousPosition, const Vector3 &rTargetPosition ) const;
	Dictionary debugValidateSingleLodState( const Vector3 &rTargetPosition ) const;
	size_t getRetainedContainerBytes( void ) const;
	size_t getActiveMultimeshBytes( void ) const;
	void setCellSize( int nCellSize );
	EA_FORCE_INLINE int getCellSize( void ) const {
		return m_nCellSize;
	}
	int getCellsPerRegionAxis( void ) const;
	bool ensureRegionLayout( Terrain3DRegion *pRegion );
	bool refreshResidentRegionLayouts( void );
	void setCellStreamingEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isCellStreamingEnabled( void ) const {
		return m_bCellStreamingEnabled;
	}
	void setSingleLodInstanceBuffers( bool bEnabled );
	EA_FORCE_INLINE bool isSingleLodInstanceBuffers( void ) const {
		return m_bSingleLodInstanceBuffers;
	}
	void setCellStreamingMargin( real_t fMargin );
	EA_FORCE_INLINE real_t getCellStreamingMargin( void ) const {
		return m_fCellStreamingMargin;
	}
	EA_FORCE_INLINE void setCellStreamingStep( const real_t fStep ) {
		m_fCellStreamingStep = CLAMP( fStep, 1.f, 256.f );
	}
	EA_FORCE_INLINE real_t getCellStreamingStep( void ) const {
		return m_fCellStreamingStep;
	}
	void setUpdateCellBudget( int nBudget );
	EA_FORCE_INLINE uint32_t getUpdateCellBudget( void ) const {
		return m_nUpdateCellBudget;
	}
	EA_FORCE_INLINE void setRuntimeUpdateCellBudgetOverride( const int nBudget ) {
		m_nRuntimeUpdateCellBudgetOverride = nBudget < 0 ? -1 : CLAMP( nBudget, 0, 65536 );
	}
	int getEffectiveUpdateCellBudget( void ) const {
		if ( m_nRuntimeUpdateCellBudgetOverride < 0 ) {
			return ( int )( m_nUpdateCellBudget );
		}
		return m_nUpdateCellBudget == 0u
		        ? m_nRuntimeUpdateCellBudgetOverride
		        : MIN( ( int )( m_nUpdateCellBudget ), m_nRuntimeUpdateCellBudgetOverride );
	}
	EA_FORCE_INLINE bool isUpdateBudgetLimitedLastFrame( void ) const {
		const int effective_budget = getEffectiveUpdateCellBudget();
		return effective_budget > 0 && m_nProcessedCellUpdatesLastFrame >= ( uint32_t )( effective_budget )
		        && m_nQueueAfterLastFrame > 0u;
	}
	int getClosestMeshId( const Vector3 &rGlobalPosition ) const;
	Dictionary pickEditableInstance( const Vector3 &rRayOrigin, const Vector3 &rRayDirection, real_t fMaxDistance = 4096.0f ) const;
	Dictionary getEditableInstance( const Dictionary &rHandle ) const;
	Dictionary setEditableInstanceTransform( const Dictionary &rHandle, const Transform3D &rGlobalTransform, bool bManualize = false );
	void copyPasteDfr( const Terrain3DRegion *pSrcRegion, const Rect2i &rSrcRect, const Terrain3DRegion *pDstRegion );

	void swapIds( const int nSrcId, const int nDstId );
	void updateMmis( const int nMeshId = -1, const Vector2i &rRegionLoc = s_v2iMax, const bool bRebuild = false );

	// Compatibility helper for public authoring/import APIs that still accept a
	// MultiMesh buffer. The runtime renderer does not consume this representation.
	PackedFloat32Array buildMultimeshBuffer(
	        const TypedArray<Transform3D> &rXforms, const PackedColorArray &rColors = PackedColorArray() ) const;

	bool buildRdInstanceSnapshot( t3dRdInstanceSceneSnapshot_t &rSnapshot ) const;
	EA_FORCE_INLINE uint64_t getRdPublicationGeneration( void ) const {
		return m_snapshotBuilder.generation();
	}
	EA_FORCE_INLINE size_t getRdSnapshotInstanceCapacity( void ) const {
		return m_nLogicalCellCapacity;
	}
	EA_FORCE_INLINE size_t getRdSnapshotCellCapacity( void ) const {
		return m_nRenderCellCapacity;
	}

	EA_FORCE_INLINE void resetDensityCounter( void ) {
		m_nDensityCounter = 0;
	}

#if defined( TERRAIN3D_INSTANCER_TRANSACTION_TEST_ACCESS )
	static bool testPrepareStoreAppend( const Terrain3DInstanceStore &rSource,
	        int nMeshId,
	        const t3dPendingPlacement_t *pPlacements,
	        size_t nPlacementCount,
	        Terrain3DInstanceStore &rReplacement );
	static bool testPrepareStoreEraseMesh( const Terrain3DInstanceStore &rSource, int nMeshId, Terrain3DInstanceStore &rReplacement );
#endif

protected:
	static void _bind_methods( void );
};
VARIANT_ENUM_CAST( t3dInstancerMode_t );

// Allows us to instance every X function calls for sparse placement
// Modifies _density_counter, not const!
inline uint32_t Terrain3DInstancer::getDensityCount( const real_t fDensity ) {
	uint32_t count = 0;
	if ( fDensity < 1.f && m_nDensityCounter++ % ( uint32_t )( 1.f / fDensity ) == 0 ) {
		count = 1;
	} else if ( fDensity >= 1.f ) {
		count = ( uint32_t )( fDensity );
	}
	return count;
}

// Use lod0 to track instance counter and set AABB, but in shadows_only lod0 doesn't exist
inline int Terrain3DInstancer::getMasterLod( const Ref<Terrain3DMeshAsset> &rMa ) const {
	if ( rMa.is_valid() && rMa->getCastShadows() == s_shadowCastingOnly ) {
		return rMa->getShadowImpostor();
	}
	return 0;
}
#endif // TERRAIN3D_INSTANCER_CLASS_H
