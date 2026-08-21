// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_INSTANCER_CLASS_H
#define TERRAIN3D_INSTANCER_CLASS_H

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <array>
#include "eastl_config.h"
#include <EASTL/bitset.h>
#include <cstdint>
#include "core/terrain_3d_flat_hash.h"
#include <EASTL/vector.h>

#include "Terrain3DConstants.h"
#include "instances/terrain_3d_instance_layout.h"
#include "core/terrain_3d_limits.h"
#include "data/terrain_3d_region.h"

class T3DTerrain;
class T3DAssets;
class T3DSampler;


typedef enum t3dInstancerMode_e {
	INSTANCER_DISABLED,
	// PLACEHOLDER,
	NORMAL,
} t3dInstancerMode_t;

typedef struct t3dVector2iHash_s {
	size_t operator()( const Vector2i &rValue ) const {
		const uint64_t x = ( uint32_t ) ( rValue.x );
		const uint64_t y = ( uint32_t ) ( rValue.y );
		uint64_t h = ( x << 32 ) | y;
		h ^= h >> 33;
		h *= 0xff51afd7ed558ccdu;
		h ^= h >> 33;
		h *= 0xc4ceb9fe1a85ec53u;
		return ( size_t ) ( h ^ ( h >> 33 ) );
	}
} t3dVector2iHash_t;

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
		h ^= t3dVector2iHash_t{}( rKey.m_cell ) + ( size_t ) ( s_nHashGoldenRatio32 ) + ( h << 6 ) + ( h >> 2 );
		h ^= ( size_t ) ( ( uint32_t ) ( rKey.m_nMeshId ) ) * ( size_t ) ( s_nHashMix32 );
		h ^= ( size_t ) ( ( uint32_t ) ( rKey.m_nLod ) ) * ( size_t ) ( s_nHashMix32Secondary );
		return h;
	}
} t3dRenderCellKeyHash_t;

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
		t3dRenderCellKey_t key{rKey.m_region, rKey.m_cell, rKey.m_nMeshId, 0};
		return t3dRenderCellKeyHash_t{}( key );
	}
} t3dCellUpdateKeyHash_t;

typedef struct t3dRegionUpdateState_s {
	bool m_bAllMeshes = false;
	eastl::bitset<s_nMaxMeshAssets> m_meshes;
} t3dRegionUpdateState_t;

typedef struct t3dRenderCell_s {
	Vector2i m_region;
	Vector2i m_cell;
	int m_nMeshId = -1;
	int m_nLod = 0;
	RID m_hMmi;
	RID m_hMultimesh;
	RID m_hBoundMultimesh;
	RID m_hBoundMesh;
	uint32_t m_nRegionListNode = UINT32_MAX;
	int m_nUploadedCount = 0;
	uint32_t m_nUploadedRevision = 0;
	uint32_t m_nSettingsRevision = 0;
	uint32_t m_nAabbGeneration = 0;
	bool m_bUsesColors = false;
} t3dRenderCell_t;

typedef struct t3dUsableHeight_s {
	bool m_bValid = false;
	real_t m_fHeight = 0.f;
	bool m_bRaycastHit = false;
	Vector3 m_normal = V3_UP;
} t3dUsableHeight_t;

typedef struct t3dCellInterval_s {
	int m_nFirst = 1;
	int m_nLast = 0;
	EA_FORCE_INLINE bool valid( void ) const { return m_nFirst <= m_nLast; }
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
		h ^= ( size_t ) ( ( uint32_t ) ( rKey.m_nMeshId ) ) * ( size_t ) ( s_nHashMix32 );
		return h;
	}
} t3dTransitionBucketKeyHash_t;

typedef struct t3dPendingPlacement_s {
	T3DRegion *m_pRegion = nullptr;
	Vector2i m_cell;
	Transform3D m_transform;
	Color m_color = COLOR_WHITE;
	uint32_t m_nOwner = 0;
} t3dPendingPlacement_t;

typedef struct t3dLogicalCellState_s {
	uint32_t m_nCount = 0;
	uint32_t m_nRegionListNode = UINT32_MAX;
} t3dLogicalCellState_t;

typedef enum t3dMultiMeshUpdateResult_e : uint8_t {
	UPDATED,
	EMPTY,
	MISSING_ASSET,
	MISSING_MESH,
} t3dMultiMeshUpdateResult_t;

typedef struct t3dRegionList_s {
	uint32_t m_nHead = UINT32_MAX;
	uint32_t m_nCount = 0u;
} t3dRegionList_t;

typedef struct t3dRenderRegionNode_s {
	size_t m_iRenderCell = 0u;
	uint32_t m_nNext = UINT32_MAX;
	uint32_t m_nPrev = UINT32_MAX;
} t3dRenderRegionNode_t;

typedef struct t3dLogicalRegionNode_s {
	t3dCellUpdateKey_t m_key{};
	uint32_t m_nNext = UINT32_MAX;
	uint32_t m_nPrev = UINT32_MAX;
} t3dLogicalRegionNode_t;

typedef struct t3dTransitionBucketState_s {
	uint32_t m_nHead = UINT32_MAX;
	uint32_t m_nCount = 0u;
} t3dTransitionBucketState_t;

typedef struct t3dTransitionCellNode_s {
	t3dCellUpdateKey_t m_key{};
	uint32_t m_nNext = UINT32_MAX;
} t3dTransitionCellNode_t;

class T3DInstancer : public Object {
	GDCLASS( T3DInstancer, Object );
	CLASS_NAME();
	friend T3DTerrain;

public: // Constants


private:
	T3DTerrain *m_pTerrain = nullptr;

















	// Runtime indices use contiguous open-addressing tables. reserve() establishes a
	// high-water bucket array; ordinary streaming inserts/erases never allocate nodes.
	using RenderCellIndex = T3DFlatHashMap<t3dRenderCellKey_t, size_t, t3dRenderCellKeyHash_t>;
	using LogicalCellMap = T3DFlatHashMap<t3dCellUpdateKey_t, t3dLogicalCellState_t, t3dCellUpdateKeyHash_t>;





	using TransitionBucketMap = T3DFlatHashMap<t3dTransitionBucketKey_t, t3dTransitionBucketState_t, t3dTransitionBucketKeyHash_t>;
	using StreamingUpdateSet = T3DFlatHashSet<t3dCellUpdateKey_t, t3dCellUpdateKeyHash_t>;

	eastl::vector<t3dRenderCell_t> m_renderCells;
	RenderCellIndex m_iRenderCellIndex;
	t3dRegionList_t m_szRegionRenderLists[s_nInstanceRegionTableCapacity] = {};
	eastl::vector<t3dRenderRegionNode_t> m_renderRegionNodes;
	uint32_t m_nRenderRegionFreeHead = UINT32_MAX;
	eastl::bitset<s_nInstanceRegionTableCapacity> m_regionRenderMask;
	// One entry per persistent logical instance cell, independent of renderer
	// residency. Asset counts describe authored terrain contents; streaming only
	// creates or removes t3dRenderCell_t records and must never mutate these totals.
	LogicalCellMap m_logicalCellCounts;
	TransitionBucketMap m_singleLodTransitionBuckets;
	eastl::vector<t3dTransitionCellNode_t> m_transitionCellNodes;
	uint32_t m_nTransitionCellFreeHead = UINT32_MAX;
	t3dRegionList_t m_szRegionLogicalLists[s_nInstanceRegionTableCapacity] = {};
	eastl::vector<t3dLogicalRegionNode_t> m_logicalRegionNodes;
	uint32_t m_nLogicalRegionFreeHead = UINT32_MAX;
	eastl::bitset<s_nInstanceRegionTableCapacity> m_regionLogicalMask;
	uint64_t m_szLogicalMeshInstanceTotals[s_nMaxMeshAssets] = {};
	eastl::bitset<s_nMaxMeshAssets> m_dirtyLogicalMeshCounts;
	bool m_bPublishingLogicalMeshCounts = false;
t3dRegionUpdateState_t m_szRegionUpdates[s_nInstanceRegionUpdateCapacity] = {};
	eastl::bitset<s_nInstanceRegionUpdateCapacity> m_regionUpdateMask;
	uint16_t m_szRegionUpdateIndices[s_nInstanceRegionUpdateCapacity] = {};
	int m_nRegionUpdateCount = 0;
	StreamingUpdateSet m_streamingUpdates;
	// Dirty region topology is s_nFixed by the maximum terrain domain. Only active
	// indices are copied into the callback-safe snapshot between batches.
	t3dRegionUpdateState_t m_szActiveRegionUpdates[s_nInstanceRegionUpdateCapacity] = {};
	eastl::bitset<s_nInstanceRegionUpdateCapacity> m_activeRegionUpdateMask;
	uint16_t m_szActiveRegionUpdateIndices[s_nInstanceRegionUpdateCapacity] = {};
	int m_nActiveRegionUpdateCount = 0;
	StreamingUpdateSet m_activeStreamingUpdates;
	// Dirty requests are converted into stable cell keys before touching the
	// RenderingServer. Processing a bounded number of keys per pre-draw frame
	// prevents global asset or region changes from becoming one unbounded hitch.
	eastl::vector<t3dCellUpdateKey_t> m_pendingCellUpdates;
	eastl::vector<t3dCellUpdateKey_t> m_buildingCellUpdates;
	eastl::bitset<s_nInstanceRegionTableCapacity> m_pendingRegionWorkMask;
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
	uint64_t m_nTransitionCellsExaminedLastScan = 0u;
	uint64_t m_nTransitionLodEvaluationsLastScan = 0u;
	uint64_t m_nTransitionsQueuedLastScan = 0u;
	uint64_t m_nTransitionScanUsecLastScan = 0u;
	uint64_t m_nTransitionBucketsExaminedLastScan = 0u;
	uint64_t m_nTransitionCandidateBucketsLastScan = 0u;
	uint64_t m_nTransitionFullRescanFallbackCount = 0u;
	eastl::bitset<s_nMaxMeshAssets> m_globalMeshUpdates;
	bool m_bUpdateAll = false;
	bool m_bRebuildAll = false;
	uint32_t m_szMeshSettingsRevision[s_nMaxMeshAssets] = {};
	real_t m_szMeshStreamingRadius[s_nMaxMeshAssets] = {};
	static_assert( s_nMaxMeshAssets <= UINT16_MAX, "Mesh asset IDs must fit in uint16_t" );
	uint16_t m_szFiniteStreamingMeshIds[s_nMaxMeshAssets] = {};
	int m_nFiniteStreamingMeshCount = 0;
	real_t m_fMaxStreamingRadius = 0.0f;
	bool m_bStreamingCacheDirty = true;
	mutable PackedFloat32Array m_uploadBuffer;
	eastl::vector<Transform3D> m_placementTransforms;
	eastl::vector<Color> m_placementColors;
	eastl::vector<t3dPendingPlacement_t> m_placementPending;
	T3DRegion *m_pTouchedRegions[s_nMaxRegionSlots] = {};
	uint16_t m_nTouchedRegionCount = 0u;

	static t3dRenderCellKey_t makeRenderKey( const t3dRenderCell_t &rCell );
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
	void reconcileRegionRenderCells( T3DRegion *pRegion, int nMeshId );
	void markLogicalMeshCountDirty( int nMeshId );
	void setLogicalCellCount( const Vector2i &vRegion, int nMeshId, const Vector2i &vCell, size_t nCount );
	void publishLogicalMeshCount( int nMeshId );
	void flushLogicalMeshCounts( void );
	void eraseLogicalCellCount( const Vector2i &vRegion, int nMeshId, const Vector2i &vCell );
	void eraseLogicalCellsByRegion( const Vector2i &vRegion );
	void eraseLogicalCellsByLocation( const Vector2i &vRegion, int nMeshId );
	void clearLogicalCellCounts( void );
	void rebuildLogicalCountsFromStore( void );
	void reconcileLogicalRegion( T3DRegion *pRegion, int nMeshId );
	void swapLogicalMeshIds( int nSrcId, int nDstId );
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
	void enqueueRegionCellUpdates( T3DRegion *pRegion, int nMeshId );
	void enqueueRegionMeshUpdates( T3DRegion *pRegion, int nMeshId );
	void enqueueAllRegions( T3DData *pData, int nMeshId );
	void processPendingCellUpdate( const t3dCellUpdateKey_t &rUpdate );
#ifdef DEBUG_ENABLED
	static bool reportInvariantFailure( const String &rMessage );
	bool validateInternalState( void ) const;
#endif
	void refreshStreamingCache( void );
	bool cellShouldBeResident( const Vector2i &region, const Vector2i &cell, int nMeshId ) const;
	bool cellShouldBeResidentAt(
		const Vector2i &region, const Vector2i &cell, int nMeshId,
		const Vector3 &rTargetPosition
	) const;
	int selectCellLod(
		const Vector2i &vRegion, const Vector2i &vCell,
		const Ref<T3DMeshAsset> &rAsset, const Vector3 &rTargetPosition
	) const;
	Vector2i getGlobalCell( const Vector2i &vRegion, const Vector2i &vCell ) const;
	t3dTransitionBucketKey_t getTransitionBucketKey( const t3dCellUpdateKey_t &rKey ) const;
	void trackSingleLodCell( const t3dCellUpdateKey_t &rKey );
	void untrackSingleLodCell( const t3dCellUpdateKey_t &rKey );
	void clearSingleLodTransitionBuckets( void );
	bool transitionBucketMayCrossBoundary(
		const t3dTransitionBucketKey_t &rBucket,
		const Ref<T3DMeshAsset> &rAsset, const Vector3 &rPreviousPosition,
		const Vector3 &rTargetPosition
	) const;
	const t3dRenderCell_t *findSingleLodRenderCell( const t3dCellUpdateKey_t &rKey ) const;
	void queueSingleLodTransitions( const Vector3 &rPreviousPosition, const Vector3 &rTargetPosition );
	void appendGlobalNative(
		int nMeshId, const eastl::vector<Transform3D> &xforms,
		const eastl::vector<Color> &colors, bool bUpdate, uint32_t owner = 0,
		bool bBackup = true
	);
	void appendRegionNative(
		T3DRegion *pRegion, int nMeshId, const eastl::vector<Transform3D> &rXforms,
		const eastl::vector<Color> &rColors, bool bGlobalSpace, bool bUpdate,
		bool bBackup = true, uint32_t nOwner = 0
	);

	t3dInstancerMode_t m_mode = NORMAL;
	uint32_t m_nDensityCounter = 0;
	int m_nCellSize = s_nInstanceDefaultCellSize;
	bool m_bCellStreamingEnabled = true;
	// Keeps one visible MultiMesh transform allocation per logical cell. The
	// legacy path retains one full transform buffer per LOD to support renderer
	// cross-fades.
	bool m_bSingleLodInstanceBuffers = true;
	real_t m_fCellStreamingMargin = 64.f;
	real_t m_fCellStreamingStep = 16.f;
	uint32_t m_nUpdateCellBudget = 32;
	int m_nRuntimeUpdateCellBudgetOverride = -1;
	Vector3 m_lastStreamPosition = V3_ZERO;
	bool m_bStreamPositionValid = false;

	Vector2i getCellForLocalPosition(
		const Vector3 &rLocalPosition, int nRegionSize,
		int nCellSize
	) const;
	bool buildRepartitionedStore(
		const T3DRegion *pRegion, const t3dInstanceLayout_t &rLayout,
		T3DInstanceStore &rStore
	) const;
	bool repartitionRegionStore( T3DRegion *pRegion, const t3dInstanceLayout_t &rLayout );
	bool repartitionResidentRegions( const t3dInstanceLayout_t &rLayout );
	uint32_t getDensityCount( const real_t fDensity );
	int getMasterLod( const Ref<T3DMeshAsset> &rMa ) const;
	void processUpdates( void );
	void updateMmiByRegion( T3DRegion *pRegion, const int nMeshId );
	void updateMmiCell(
		T3DRegion *pRegion, const int nMeshId, t3dInstanceCell_t &rCell,
		const Ref<T3DMeshAsset> &rAsset
	);
	void ensureUploadBuffer(
		t3dInstanceCell_t &rCell, const Ref<Mesh> &rMasterMesh,
		const AABB &aabbSourceMeshAabb, bool bRebuildAabb, bool &bReady
	);
	void setMmiLodRanges( RID mmi, const Ref<T3DMeshAsset> &rMa, const int nLod );
	void updateVertexSpacing( const real_t fVertexSpacing );
	// Renderer teardown never mutates persistent content accounting. Callers that
	// change T3DInstanceStore must update logical counts explicitly.
	void destroyMmisByRegion( const Vector2i &rRegionLoc );
	void destroyMmiByLocation( const Vector2i &rRegionLoc, int nMeshId );
	void destroyMmiByCell( const Vector2i &rRegionLoc, int nMeshId, Vector2i vCell, int nLod = INT32_MAX );
	void eraseMmiLod( const Vector2i &rRegionLoc, int nMeshId, const Vector2i &vCell, int nLod );
	static bool pendingPlacementRegionCellLess( const t3dPendingPlacement_t &rA, const t3dPendingPlacement_t &rB );
	static bool pendingPlacementCellLess( const t3dPendingPlacement_t &rA, const t3dPendingPlacement_t &rB );
	static int floorDivCell( int nValue, int nDivisor );
	t3dCellInterval_t streamingRowInterval(
		const Vector3 &rPosition, real_t fRadius, int iGlobalZ,
		real_t fCellWorldSize, int nMinGlobalCell, int nMaxGlobalCell
	) const;
	void queueStreamingCell(
		T3DData *pData, int nMeshId, int iGlobalX, int iGlobalZ,
		int nCellsPerRegion
	);
	void queueStreamingInterval(
		T3DData *pData, int nMeshId, int iGlobalZ, int nFirst, int nLast,
		int nCellsPerRegion
	);
	void queueStreamingDifference(
		T3DData *pData, int nMeshId, int iGlobalZ,
		const t3dCellInterval_t &rSource, const t3dCellInterval_t &rOther, int nCellsPerRegion
	);
	void backupRegion( const Ref<T3DRegion> &rRegion );
	t3dMultiMeshUpdateResult_t updateMultimesh(
		const int nMeshId, const int nLod, const t3dInstanceCell_t &rCell,
		t3dRenderCell_t &rRender, bool bUseColors,
		const PackedFloat32Array &rUploadBuffer
	);
	void fillMultimeshBuffer(
		const t3dInstanceCell_t &rCell, PackedFloat32Array &rBuffer,
		const AABB *pSourceMeshAabb = nullptr, AABB *pCellAabb = nullptr
	) const;
	const PackedFloat32Array &buildUploadBuffer( const t3dInstanceCell_t &rCell ) const;
	Vector2i getCell( const Vector3 &rGlobalPosition, const int nRegionSize ) const;
	t3dUsableHeight_t getUsableHeight(
		T3DSampler &rSampler, const Vector3 &rGlobalPosition,
		const Vector2 &rSlopeRange, bool bOnCollision, real_t fRaycastStart,
		bool bSampleNormal = false
	) const;

public:
	EA_FORCE_INLINE T3DInstancer( void ) {}
	EA_FORCE_INLINE ~T3DInstancer( void ) { destroy(); }

	void initialize( T3DTerrain *pTerrain );
	void destroy( void );
	void suspendRendering( void );

	void clearByMesh( const int nMeshId );
	void clearByLocation( const Vector2i &rRegionLoc, const int nMeshId );
	void clearByRegion( const Ref<T3DRegion> &rRegion, const int nMeshId );
	// Frees rendered MMIs for one region without modifying its stored instance data.
	// Used when a terrain region is temporarily removed and may later be restored by undo.
	void destroyRegionMmis( const Vector2i &rRegionLoc );

	void setMode( const t3dInstancerMode_t mode );
	EA_FORCE_INLINE t3dInstancerMode_t getMode( void ) const { return m_mode; }
	EA_FORCE_INLINE bool isEnabled( void ) const { return m_mode != INSTANCER_DISABLED; }

	void addInstances( const Vector3 &rGlobalPosition, const Dictionary &rParams );
	void removeInstances( const Vector3 &rGlobalPosition, const Dictionary &rParams );
	void addMultimesh(
		const int nMeshId, const Ref<MultiMesh> &rMultimesh,
		const Transform3D &rXform = Transform3D(), const bool bUpdate = true
	);
	void addTransforms(
		const int nMeshId, const TypedArray<Transform3D> &rXforms,
		const PackedColorArray &rColors = PackedColorArray(), const bool bUpdate = true
	);
	void addOwnedTransforms(
		const int nMeshId, const TypedArray<Transform3D> &rXforms, const int64_t nOwnerId,
		const PackedColorArray &rColors = PackedColorArray(), const bool bUpdate = true,
		const bool bBackup = true
	);
	int removeOwnedInstances( const int64_t nOwnerId, const bool bUpdate = true, const bool bBackup = true );
	void appendLocation(
		const Vector2i &rRegionLoc, const int nMeshId, const TypedArray<Transform3D> &rXforms,
		const PackedColorArray &rColors, const bool bUpdate = true
	);
	void appendRegion(
		const Ref<T3DRegion> &rRegion, const int nMeshId,
		const TypedArray<Transform3D> &rXforms, const PackedColorArray &rColors,
		const bool bUpdate = true
	);
	void updateTransforms( const AABB &rAabb );
	void updateStreaming( const Vector3 &rTargetPosition );
	EA_FORCE_INLINE void deferStreamingSelectionForFrame( void ) { m_bStreamingSelectionDeferredLastFrame = true; }
	EA_FORCE_INLINE void resetStreamingResidency( void ) { suspendRendering(); }
	EA_FORCE_INLINE int getRenderCellCount( void ) const { return ( int ) ( m_renderCells.size() ); }
	EA_FORCE_INLINE int getPendingStreamingUpdateCount( void ) const { const size_t pending_count = m_pendingCellUpdates.size() > m_iPendingCellUpdateCursor ? m_pendingCellUpdates.size() - m_iPendingCellUpdateCursor : 0u; return ( int ) ( m_streamingUpdates.size() + m_activeStreamingUpdates.size() + pending_count ); }
	bool areHlodDetailRegionsReady( const Vector2i &rRegionMin, const Vector2i &rRegionMax ) const {
		for ( int z = rRegionMin.y; z <= rRegionMax.y; ++z ) {
			for ( int x = rRegionMin.x; x <= rRegionMax.x; ++x ) {
				if ( hasPendingUpdatesForRegion( Vector2i( x, z ) ) ) { return false; }
			}
		}
		return true;
	}
	Dictionary getStats( void ) const;
	Dictionary debugValidateSingleLodTransitionCandidates(
		const Vector3 &rPreviousPosition,
		const Vector3 &rTargetPosition
	) const;
	Dictionary debugValidateSingleLodState( const Vector3 &rTargetPosition ) const;
	size_t getRetainedContainerBytes( void ) const;
	size_t getActiveMultimeshBytes( void ) const;
	void setCellSize( int nCellSize );
	EA_FORCE_INLINE int getCellSize( void ) const { return m_nCellSize; }
	int getCellsPerRegionAxis( void ) const;
	bool ensureRegionLayout( T3DRegion *pRegion );
	bool refreshResidentRegionLayouts( void );
	void setCellStreamingEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isCellStreamingEnabled( void ) const { return m_bCellStreamingEnabled; }
	void setSingleLodInstanceBuffers( bool bEnabled );
	EA_FORCE_INLINE bool isSingleLodInstanceBuffers( void ) const { return m_bSingleLodInstanceBuffers; }
	void setCellStreamingMargin( real_t fMargin );
	EA_FORCE_INLINE real_t getCellStreamingMargin( void ) const { return m_fCellStreamingMargin; }
	EA_FORCE_INLINE void setCellStreamingStep( const real_t fStep ) { m_fCellStreamingStep = CLAMP( fStep, 1.f, 256.f ); }
	EA_FORCE_INLINE real_t getCellStreamingStep( void ) const { return m_fCellStreamingStep; }
	EA_FORCE_INLINE void setUpdateCellBudget( const int nBudget ) { m_nUpdateCellBudget = ( uint32_t ) ( CLAMP( nBudget, 0, 65536 ) ); }
	EA_FORCE_INLINE uint32_t getUpdateCellBudget( void ) const { return m_nUpdateCellBudget; }
	EA_FORCE_INLINE void setRuntimeUpdateCellBudgetOverride( const int nBudget ) { m_nRuntimeUpdateCellBudgetOverride = nBudget < 0 ? -1 : CLAMP( nBudget, 0, 65536 ); }
	int getEffectiveUpdateCellBudget( void ) const {
		if ( m_nRuntimeUpdateCellBudgetOverride < 0 ) {
			return ( int ) ( m_nUpdateCellBudget );
		}
		return m_nUpdateCellBudget == 0u ? m_nRuntimeUpdateCellBudgetOverride :
			MIN( ( int ) ( m_nUpdateCellBudget ), m_nRuntimeUpdateCellBudgetOverride );
	}
	EA_FORCE_INLINE bool isUpdateBudgetLimitedLastFrame( void ) const { const int effective_budget = getEffectiveUpdateCellBudget(); return effective_budget > 0 && m_nProcessedCellUpdatesLastFrame >= ( uint32_t ) ( effective_budget ) && m_nQueueAfterLastFrame > 0u; }
	int getClosestMeshId( const Vector3 &rGlobalPosition ) const;
	void copyPasteDfr(
		const T3DRegion *pSrcRegion, const Rect2i &rSrcRect,
		const T3DRegion *pDstRegion
	);

	void swapIds( const int nSrcId, const int nDstId );
	void updateMmis( const int nMeshId = -1, const Vector2i &rRegionLoc = V2I_MAX, const bool bRebuild = false );

	// Builds the exact packed RenderingServer buffer used by Terrain3D's
	// MultiMeshes: 12 transform floats, plus RGBA only when a cell uses non-white colors.
	PackedFloat32Array buildMultimeshBuffer(
		const TypedArray<Transform3D> &rXforms,
		const PackedColorArray &rColors = PackedColorArray()
	) const;

	EA_FORCE_INLINE void resetDensityCounter( void ) { m_nDensityCounter = 0; }

protected:
	static void _bind_methods( void );
};

using t3dInstancerMode_t = t3dInstancerMode_t;
VARIANT_ENUM_CAST( t3dInstancerMode_t );

// Allows us to instance every X function calls for sparse placement
// Modifies _density_counter, not const!
inline uint32_t T3DInstancer::getDensityCount( const real_t fDensity ) {
	uint32_t count = 0;
	if ( fDensity < 1.f && m_nDensityCounter++ % ( uint32_t ) ( 1.f / fDensity ) == 0 ) {
		count = 1;
	} else if ( fDensity >= 1.f ) {
		count = ( uint32_t ) ( fDensity );
	}
	return count;
}

// Use lod0 to track instance counter and set AABB, but in shadows_only lod0 doesn't exist
inline int T3DInstancer::getMasterLod( const Ref<T3DMeshAsset> &rMa ) const {
	if ( rMa.is_valid() && rMa->getCastShadows() == s_shadowCastingOnly ) {
		return rMa->getShadowImpostor();
	}
	return 0;
}
#endif // TERRAIN3D_INSTANCER_CLASS_H
