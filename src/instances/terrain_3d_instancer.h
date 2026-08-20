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
#include <EASTL/hash_map.h>
#include <EASTL/hash_set.h>
#include <EASTL/vector.h>

#include "Terrain3DConstants.h"
#include "instances/terrain_3d_instance_layout.h"
#include "core/terrain_3d_limits.h"
#include "data/terrain_3d_region.h"

class Terrain3D;
class Terrain3DAssets;
class T3DSampler;


class Terrain3DInstancer : public Object {
	GDCLASS( Terrain3DInstancer, Object );
	CLASS_NAME();
	friend Terrain3D;

public: // Constants
typedef enum t3dinstancerMode_e {
		DISABLED,
		// PLACEHOLDER,
		NORMAL,
	} t3dinstancerMode_t;

private:
	Terrain3D *m_pTerrain = nullptr;

	typedef struct t3dvector2iHash_s {
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
	} t3dvector2iHash_t;
	typedef struct t3drenderCellKey_s {
		Vector2i pRegion;
		Vector2i cell;
		int meshId = -1;
		int lod = 0;
		bool operator==( const t3drenderCellKey_s &rOther ) const {
			return pRegion == rOther.pRegion && cell == rOther.cell && meshId == rOther.meshId && lod == rOther.lod;
		}
	} t3drenderCellKey_t;
	typedef struct t3drenderCellKeyHash_s {
		size_t operator()( const t3drenderCellKey_t &rKey ) const {
			size_t h = t3dvector2iHash_t{}( rKey.pRegion );
			h ^= t3dvector2iHash_t{}( rKey.cell ) + ( size_t ) ( TERRAIN3D_HASH_GOLDEN_RATIO_32 ) + ( h << 6 ) + ( h >> 2 );
			h ^= ( size_t ) ( ( uint32_t ) ( rKey.meshId ) ) * ( size_t ) ( TERRAIN3D_HASH_MIX_32 );
			h ^= ( size_t ) ( ( uint32_t ) ( rKey.lod ) ) * ( size_t ) ( TERRAIN3D_HASH_MIX_32_SECONDARY );
			return h;
		}
	} t3drenderCellKeyHash_t;
	typedef struct t3dcellUpdateKey_s {
		Vector2i pRegion;
		Vector2i cell;
		int meshId = -1;
		bool operator==( const t3dcellUpdateKey_s &rOther ) const {
			return pRegion == rOther.pRegion && cell == rOther.cell && meshId == rOther.meshId;
		}
	} t3dcellUpdateKey_t;
	typedef struct t3dcellUpdateKeyHash_s {
		size_t operator()( const t3dcellUpdateKey_t &rKey ) const {
			t3drenderCellKey_t key{rKey.pRegion, rKey.cell, rKey.meshId, 0};
			return t3drenderCellKeyHash_t{}( key );
		}
	} t3dcellUpdateKeyHash_t;
	typedef struct t3dregionUpdateState_s {
		bool bAllMeshes = false;
		eastl::bitset<TERRAIN3D_MAX_MESH_ASSETS> meshes;
	} t3dregionUpdateState_t;
	typedef struct t3drenderCell_s {
		Vector2i pRegion;
		Vector2i cell;
		int meshId = -1;
		int lod = 0;
		RID mmi;
		RID multimesh;
		RID boundMultimesh;
		RID boundMesh;
		size_t iRegionListIndex = 0;
		int nUploadedCount = 0;
		uint32_t nUploadedRevision = 0;
		uint32_t nSettingsRevision = 0;
		uint32_t nAabbGeneration = 0;
		bool bUsesColors = false;
	} t3drenderCell_t;
	typedef struct t3dusableHeight_s {
		bool bValid = false;
		real_t height = 0.f;
		bool bRaycastHit = false;
		Vector3 normal = V3_UP;
	} t3dusableHeight_t;
	typedef struct t3dcellInterval_s {
		int first = 1;
		int last = 0;
		bool valid( void ) const {
			return first <= last;
		}
	} t3dcellInterval_t;
	typedef struct t3dtransitionBucketKey_s {
		Vector2i bucket;
		int meshId = -1;
		bool operator==( const t3dtransitionBucketKey_s &rOther ) const {
			return bucket == rOther.bucket && meshId == rOther.meshId;
		}
	} t3dtransitionBucketKey_t;
	typedef struct t3dtransitionBucketKeyHash_s {
		size_t operator()( const t3dtransitionBucketKey_t &rKey ) const {
			size_t h = t3dvector2iHash_t{}( rKey.bucket );
			h ^= ( size_t ) ( ( uint32_t ) ( rKey.meshId ) ) * ( size_t ) ( TERRAIN3D_HASH_MIX_32 );
			return h;
		}
	} t3dtransitionBucketKeyHash_t;

	typedef struct t3dpendingPlacement_s {
		Terrain3DRegion *pRegion = nullptr;
		Vector2i cell;
		Transform3D transform;
		Color pColor = COLOR_WHITE;
		uint32_t owner = 0;
	} t3dpendingPlacement_t;
	typedef struct t3dlogicalCellState_s {
		uint32_t nCount = 0;
		size_t iRegionListIndex = 0;
	} t3dlogicalCellState_t;
	typedef enum t3dmultiMeshUpdateResult_e : uint8_t {
		UPDATED,
		EMPTY,
		MISSING_ASSET,
		MISSING_MESH,
	} t3dmultiMeshUpdateResult_t;

	// Dense render records provide cache-friendly iteration while the index gives
	// average O(1) lookup/removal. Erasure uses swap-and-pop, avoiding sorted-vector
	// middle moves when streaming cells enter or leave residency.
using RenderCellIndex = eastl::hash_map<t3drenderCellKey_t, size_t, t3drenderCellKeyHash_t>;
	using LogicalCellMap = eastl::hash_map<t3dcellUpdateKey_t, t3dlogicalCellState_t, t3dcellUpdateKeyHash_t>;
	using TransitionBucketMap = eastl::hash_map<t3dtransitionBucketKey_t, eastl::vector<t3dcellUpdateKey_t>, t3dtransitionBucketKeyHash_t>;

	eastl::vector<t3drenderCell_t> m_renderCells;
	RenderCellIndex m_iRenderCellIndex;
	// Region coordinates are bounded to the terrain's fixed maximum map. Direct tables
	// avoid one hash node and bucket entry per active region. Empty vectors retain
	// no heap allocation; masks identify the populated slots.
	eastl::vector<size_t> m_szRegionRenderIndices[TERRAIN3D_INSTANCE_REGION_TABLE_CAPACITY];
	eastl::bitset<TERRAIN3D_INSTANCE_REGION_TABLE_CAPACITY> m_regionRenderMask;
	// One entry per persistent logical instance cell, independent of renderer
	// residency. Asset counts describe authored terrain contents; streaming only
	// creates or removes t3drenderCell_t records and must never mutate these totals.
	LogicalCellMap m_logicalCellCounts;
	TransitionBucketMap m_singleLodTransitionBuckets;
	eastl::vector<t3dcellUpdateKey_t> m_szRegionLogicalCells[TERRAIN3D_INSTANCE_REGION_TABLE_CAPACITY];
	eastl::bitset<TERRAIN3D_INSTANCE_REGION_TABLE_CAPACITY> m_regionLogicalMask;
	uint64_t m_szLogicalMeshInstanceTotals[TERRAIN3D_MAX_MESH_ASSETS] = {};
	eastl::bitset<TERRAIN3D_MAX_MESH_ASSETS> m_dirtyLogicalMeshCounts;
	bool m_bPublishingLogicalMeshCounts = false;
t3dregionUpdateState_t m_szRegionUpdates[TERRAIN3D_INSTANCE_REGION_UPDATE_CAPACITY] = {};
	eastl::bitset<TERRAIN3D_INSTANCE_REGION_UPDATE_CAPACITY> m_regionUpdateMask;
	uint16_t m_szRegionUpdateIndices[TERRAIN3D_INSTANCE_REGION_UPDATE_CAPACITY] = {};
	int m_nRegionUpdateCount = 0;
	eastl::hash_set<t3dcellUpdateKey_t, t3dcellUpdateKeyHash_t> m_streamingUpdates;
	// Dirty region topology is fixed by the maximum terrain domain. Only active
	// indices are copied into the callback-safe snapshot between batches.
	t3dregionUpdateState_t m_szActiveRegionUpdates[TERRAIN3D_INSTANCE_REGION_UPDATE_CAPACITY] = {};
	eastl::bitset<TERRAIN3D_INSTANCE_REGION_UPDATE_CAPACITY> m_activeRegionUpdateMask;
	uint16_t m_szActiveRegionUpdateIndices[TERRAIN3D_INSTANCE_REGION_UPDATE_CAPACITY] = {};
	int m_nActiveRegionUpdateCount = 0;
	eastl::hash_set<t3dcellUpdateKey_t, t3dcellUpdateKeyHash_t> m_activeStreamingUpdates;
	// Dirty requests are converted into stable cell keys before touching the
	// RenderingServer. Processing a bounded number of keys per pre-draw frame
	// prevents global asset or region changes from becoming one unbounded hitch.
	eastl::vector<t3dcellUpdateKey_t> m_pendingCellUpdates;
	eastl::vector<t3dcellUpdateKey_t> m_buildingCellUpdates;
	eastl::bitset<TERRAIN3D_INSTANCE_REGION_TABLE_CAPACITY> m_pendingRegionWorkMask;
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
	eastl::bitset<TERRAIN3D_MAX_MESH_ASSETS> m_globalMeshUpdates;
	bool m_bUpdateAll = false;
	bool m_bRebuildAll = false;
	uint32_t m_szMeshSettingsRevision[TERRAIN3D_MAX_MESH_ASSETS] = {};
	real_t m_szMeshStreamingRadius[TERRAIN3D_MAX_MESH_ASSETS] = {};
	static_assert( TERRAIN3D_MAX_MESH_ASSETS <= UINT16_MAX, "Mesh asset IDs must fit in uint16_t" );
	uint16_t m_szFiniteStreamingMeshIds[TERRAIN3D_MAX_MESH_ASSETS] = {};
	int m_nFiniteStreamingMeshCount = 0;
	real_t m_fMaxStreamingRadius = 0.0f;
	bool m_bStreamingCacheDirty = true;
	mutable PackedFloat32Array m_uploadBuffer;
	eastl::vector<Transform3D> m_placementTransforms;
	eastl::vector<Color> m_placementColors;
	eastl::vector<t3dpendingPlacement_t> m_placementPending;
	eastl::vector<Terrain3DRegion *> m_pTouchedRegions;

	static t3drenderCellKey_t makeRenderKey( const t3drenderCell_t &rCell );
	static int getRegionTableIndex( const Vector2i &vRegion );
	eastl::vector<size_t> *getRegionRenderList( const Vector2i &vRegion );
	const eastl::vector<size_t> *findRegionRenderList( const Vector2i &vRegion ) const;
	eastl::vector<t3dcellUpdateKey_t> *getRegionLogicalList( const Vector2i &vRegion );
	const eastl::vector<t3dcellUpdateKey_t> *findRegionLogicalList( const Vector2i &vRegion ) const;
	void clearRegionRenderLists( void );
	void clearRegionLogicalLists( void );
	t3drenderCell_t *findRenderCell( const Vector2i &region, int nMeshId, int nLod, const Vector2i &cell );
	t3drenderCell_t *getOrCreateRenderCell( const Vector2i &region, int nMeshId, int nLod, const Vector2i &cell );
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
	void swapLogicalMeshIds( int nSrcId, int nDstId );
	bool hasPendingUpdates( void ) const;
	bool hasPendingUpdatesForRegion( const Vector2i &rRegion ) const;
	void markPendingRegionWork( const Vector2i &rRegion );
	void rebuildPendingRegionWorkMask( void );
	void clearRegionUpdates( bool bActive );
	void snapshotRegionUpdates( void );
	t3dregionUpdateState_t *getRegionUpdate( const Vector2i &vRegion );
	const t3dregionUpdateState_t *findActiveRegionUpdate( const Vector2i &vRegion ) const;
	static Vector2i regionLocationFromUpdateIndex( uint16_t iIndex );
	void scheduleUpdateProcessing( void );
	void cancelPendingCellUpdates( void );
	void materializePendingUpdates( void );
	void enqueueRegionCellUpdates( Terrain3DRegion *pRegion, int nMeshId );
	void enqueueRegionMeshUpdates( Terrain3DRegion *pRegion, int nMeshId );
	void enqueueAllRegions( Terrain3DData *pData, int nMeshId );
	void processPendingCellUpdate( const t3dcellUpdateKey_t &rUpdate );
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
		const Ref<Terrain3DMeshAsset> &rAsset, const Vector3 &rTargetPosition
	) const;
	Vector2i getGlobalCell( const Vector2i &vRegion, const Vector2i &vCell ) const;
	t3dtransitionBucketKey_t getTransitionBucketKey( const t3dcellUpdateKey_t &rKey ) const;
	void trackSingleLodCell( const t3dcellUpdateKey_t &rKey );
	void untrackSingleLodCell( const t3dcellUpdateKey_t &rKey );
	void clearSingleLodTransitionBuckets( void );
	bool transitionBucketMayCrossBoundary(
		const t3dtransitionBucketKey_t &rBucket,
		const Ref<Terrain3DMeshAsset> &rAsset, const Vector3 &rPreviousPosition,
		const Vector3 &rTargetPosition
	) const;
	const t3drenderCell_t *findSingleLodRenderCell( const t3dcellUpdateKey_t &rKey ) const;
	void queueSingleLodTransitions( const Vector3 &rPreviousPosition, const Vector3 &rTargetPosition );
	void appendGlobalNative(
		int nMeshId, const eastl::vector<Transform3D> &xforms,
		const eastl::vector<Color> &colors, bool bUpdate, uint32_t owner = 0,
		bool bBackup = true
	);
	void appendRegionNative(
		Terrain3DRegion *pRegion, int nMeshId, const eastl::vector<Transform3D> &rXforms,
		const eastl::vector<Color> &rColors, bool bGlobalSpace, bool bUpdate,
		bool bBackup = true, uint32_t nOwner = 0
	);

	t3dinstancerMode_t m_mode = NORMAL;
	uint32_t m_nDensityCounter = 0;
	int m_nCellSize = TERRAIN3D_INSTANCE_DEFAULT_CELL_SIZE;
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
		const Terrain3DRegion *pRegion, const t3dinstanceLayout_t &rLayout,
		T3DInstanceStore &rStore
	) const;
	bool repartitionRegionStore( Terrain3DRegion *pRegion, const t3dinstanceLayout_t &rLayout );
	bool repartitionResidentRegions( const t3dinstanceLayout_t &rLayout );
	uint32_t getDensityCount( const real_t fDensity );
	int getMasterLod( const Ref<Terrain3DMeshAsset> &rMa ) const;
	void processUpdates( void );
	void updateMmiByRegion( Terrain3DRegion *pRegion, const int nMeshId );
	void updateMmiCell(
		Terrain3DRegion *pRegion, const int nMeshId, t3dinstanceCell_t &rCell,
		const Ref<Terrain3DMeshAsset> &rAsset
	);
	void ensureUploadBuffer(
		t3dinstanceCell_t &rCell, const Ref<Mesh> &rMasterMesh,
		const AABB &aabbSourceMeshAabb, bool bRebuildAabb, bool &bReady
	);
	void setMmiLodRanges( RID mmi, const Ref<Terrain3DMeshAsset> &rMa, const int nLod );
	void updateVertexSpacing( const real_t fVertexSpacing );
	// Renderer teardown never mutates persistent content accounting. Callers that
	// change T3DInstanceStore must update logical counts explicitly.
	void destroyMmisByRegion( const Vector2i &rRegionLoc );
	void destroyMmiByLocation( const Vector2i &rRegionLoc, int nMeshId );
	void destroyMmiByCell( const Vector2i &rRegionLoc, int nMeshId, Vector2i vCell, int nLod = INT32_MAX );
	void eraseMmiLod( const Vector2i &rRegionLoc, int nMeshId, const Vector2i &vCell, int nLod );
	static bool pendingPlacementRegionCellLess( const t3dpendingPlacement_t &rA, const t3dpendingPlacement_t &rB );
	static bool pendingPlacementCellLess( const t3dpendingPlacement_t &rA, const t3dpendingPlacement_t &rB );
	static int floorDivCell( int nValue, int nDivisor );
	t3dcellInterval_t streamingRowInterval(
		const Vector3 &rPosition, real_t fRadius, int iGlobalZ,
		real_t fCellWorldSize, int nMinGlobalCell, int nMaxGlobalCell
	) const;
	void queueStreamingCell(
		Terrain3DData *pData, int nMeshId, int iGlobalX, int iGlobalZ,
		int nCellsPerRegion
	);
	void queueStreamingInterval(
		Terrain3DData *pData, int nMeshId, int iGlobalZ, int nFirst, int nLast,
		int nCellsPerRegion
	);
	void queueStreamingDifference(
		Terrain3DData *pData, int nMeshId, int iGlobalZ,
		const t3dcellInterval_t &rSource, const t3dcellInterval_t &rOther, int nCellsPerRegion
	);
	void backupRegion( const Ref<Terrain3DRegion> &rRegion );
	t3dmultiMeshUpdateResult_t updateMultimesh(
		const int nMeshId, const int nLod, const t3dinstanceCell_t &rCell,
		t3drenderCell_t &rRender, bool bUseColors,
		const PackedFloat32Array &rUploadBuffer
	);
	void fillMultimeshBuffer(
		const t3dinstanceCell_t &rCell, PackedFloat32Array &rBuffer,
		const AABB *pSourceMeshAabb = nullptr, AABB *pCellAabb = nullptr
	) const;
	const PackedFloat32Array &buildUploadBuffer( const t3dinstanceCell_t &rCell ) const;
	Vector2i getCell( const Vector3 &rGlobalPosition, const int nRegionSize ) const;
	t3dusableHeight_t getUsableHeight(
		T3DSampler &rSampler, const Vector3 &rGlobalPosition,
		const Vector2 &rSlopeRange, bool bOnCollision, real_t fRaycastStart,
		bool bSampleNormal = false
	) const;

public:
	Terrain3DInstancer( void ) {}
	~Terrain3DInstancer( void ) {
		destroy();
	}

	void initialize( Terrain3D *pTerrain );
	void destroy( void );
	void suspendRendering( void );

	void clearByMesh( const int nMeshId );
	void clearByLocation( const Vector2i &rRegionLoc, const int nMeshId );
	void clearByRegion( const Ref<Terrain3DRegion> &rRegion, const int nMeshId );
	// Frees rendered MMIs for one region without modifying its stored instance data.
	// Used when a terrain region is temporarily removed and may later be restored by undo.
	void destroyRegionMmis( const Vector2i &rRegionLoc );

	void setMode( const t3dinstancerMode_t mode );
	t3dinstancerMode_t getMode( void ) const {
		return m_mode;
	}
	bool isEnabled( void ) const {
		return m_mode != DISABLED;
	}

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
		const Ref<Terrain3DRegion> &rRegion, const int nMeshId,
		const TypedArray<Transform3D> &rXforms, const PackedColorArray &rColors,
		const bool bUpdate = true
	);
	void updateTransforms( const AABB &rAabb );
	void updateStreaming( const Vector3 &rTargetPosition );
	void deferStreamingSelectionForFrame( void ) { m_bStreamingSelectionDeferredLastFrame = true; }
	void resetStreamingResidency( void ) {
		suspendRendering();
	}
	int getRenderCellCount( void ) const {
		return ( int ) ( m_renderCells.size() );
	}
	int getPendingStreamingUpdateCount( void ) const {
		const size_t pending_count = m_pendingCellUpdates.size() > m_iPendingCellUpdateCursor ?
			m_pendingCellUpdates.size() - m_iPendingCellUpdateCursor : 0u;
		return ( int ) ( m_streamingUpdates.size() + m_activeStreamingUpdates.size() + pending_count );
	}
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
	int getCellSize( void ) const {
		return m_nCellSize;
	}
	int getCellsPerRegionAxis( void ) const;
	bool ensureRegionLayout( Terrain3DRegion *pRegion );
	bool refreshResidentRegionLayouts( void );
	void setCellStreamingEnabled( bool bEnabled );
	bool isCellStreamingEnabled( void ) const {
		return m_bCellStreamingEnabled;
	}
	void setSingleLodInstanceBuffers( bool bEnabled );
	bool isSingleLodInstanceBuffers( void ) const {
		return m_bSingleLodInstanceBuffers;
	}
	void setCellStreamingMargin( real_t fMargin );
	real_t getCellStreamingMargin( void ) const {
		return m_fCellStreamingMargin;
	}
	void setCellStreamingStep( const real_t fStep ) {
		m_fCellStreamingStep = CLAMP( fStep, 1.f, 256.f );
	}
	real_t getCellStreamingStep( void ) const {
		return m_fCellStreamingStep;
	}
	void setUpdateCellBudget( const int nBudget ) {
		m_nUpdateCellBudget = ( uint32_t ) ( CLAMP( nBudget, 0, 65536 ) );
	}
	uint32_t getUpdateCellBudget( void ) const {
		return m_nUpdateCellBudget;
	}
	void setRuntimeUpdateCellBudgetOverride( const int nBudget ) {
		m_nRuntimeUpdateCellBudgetOverride = nBudget < 0 ? -1 : CLAMP( nBudget, 0, 65536 );
	}
	int getEffectiveUpdateCellBudget( void ) const {
		if ( m_nRuntimeUpdateCellBudgetOverride < 0 ) {
			return ( int ) ( m_nUpdateCellBudget );
		}
		return m_nUpdateCellBudget == 0u ? m_nRuntimeUpdateCellBudgetOverride :
			MIN( ( int ) ( m_nUpdateCellBudget ), m_nRuntimeUpdateCellBudgetOverride );
	}
	bool isUpdateBudgetLimitedLastFrame( void ) const {
		const int effective_budget = getEffectiveUpdateCellBudget();
		return effective_budget > 0 && m_nProcessedCellUpdatesLastFrame >= ( uint32_t ) ( effective_budget ) &&
			m_nQueueAfterLastFrame > 0u;
	}
	int getClosestMeshId( const Vector3 &rGlobalPosition ) const;
	void copyPasteDfr(
		const Terrain3DRegion *pSrcRegion, const Rect2i &rSrcRect,
		const Terrain3DRegion *pDstRegion
	);

	void swapIds( const int nSrcId, const int nDstId );
	void updateMmis( const int nMeshId = -1, const Vector2i &rRegionLoc = V2I_MAX, const bool bRebuild = false );

	// Builds the exact packed RenderingServer buffer used by Terrain3D's
	// MultiMeshes: 12 transform floats, plus RGBA only when a cell uses non-white colors.
	PackedFloat32Array buildMultimeshBuffer(
		const TypedArray<Transform3D> &rXforms,
		const PackedColorArray &rColors = PackedColorArray()
	) const;

	void resetDensityCounter( void ) {
		m_nDensityCounter = 0;
	}

protected:
	static void _bind_methods( void );
};

using t3dinstancerMode_t = Terrain3DInstancer::t3dinstancerMode_t;
VARIANT_ENUM_CAST( Terrain3DInstancer::t3dinstancerMode_t );

// Allows us to instance every X function calls for sparse placement
// Modifies _density_counter, not const!
inline uint32_t Terrain3DInstancer::getDensityCount( const real_t fDensity ) {
	uint32_t count = 0;
	if ( fDensity < 1.f && m_nDensityCounter++ % ( uint32_t ) ( 1.f / fDensity ) == 0 ) {
		count = 1;
	} else if ( fDensity >= 1.f ) {
		count = ( uint32_t ) ( fDensity );
	}
	return count;
}

// Use lod0 to track instance counter and set AABB, but in shadows_only lod0 doesn't exist
inline int Terrain3DInstancer::getMasterLod( const Ref<Terrain3DMeshAsset> &rMa ) const {
	if ( rMa.is_valid() && rMa->getCastShadows() == TERRAIN3D_SHADOW_CASTING_ONLY ) {
		return rMa->getShadowImpostor();
	}
	return 0;
}
#endif // TERRAIN3D_INSTANCER_CLASS_H
