// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_HLOD_STREAMER_H
#define TERRAIN3D_HLOD_STREAMER_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_owner.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "eastl_config.h"
#include "hlod/format/terrain_3d_hlod_manifest.h"
#include "hlod/format/terrain_3d_hlod_package.h"
#include "hlod/format/terrain_3d_hlod_instance_codec.h"
#include "hlod/format/terrain_3d_hlod_runtime_codec.h"
#include "instances/contracts/terrain_3d_instance_mutation.h"
#include "rendering/rd/instances/terrain_3d_rd_instance_renderer.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

typedef struct t3dHlodMaterialContext_s
{
    const Terrain3DHLODPackage *m_pPackage = NULL;
    uint32_t m_nSurfaceOffset = 0u;
} t3dHlodMaterialContext_t;

typedef struct t3dHlodPrototypeRuntime_s
{
    Ref< ArrayMesh > m_mesh;
    t3dHlodMaterialContext_t m_materialContext;
    uint32_t m_nFirstInstance = 0u;
    uint32_t m_nInstanceCount = 0u;
} t3dHlodPrototypeRuntime_t;

// Precomputed spatial culling cell within an instanced HLOD group.
// Instance ordering is finalized on the cold package load, not in the frame loop.
typedef struct t3dHlodSpatialCell_s
{
    uint32_t m_nPrototype = 0u;
    uint32_t m_nFirstInstance = 0u;
    uint32_t m_nInstanceCount = 0u;
    int16_t m_nBucketX = 0;
    int16_t m_nBucketZ = 0;
} t3dHlodSpatialCell_t;

typedef struct t3dEntry_s : public t3dHLODEntryDescriptor_t
{
	uint32_t						 m_nDesiredGeneration = 0u;
	bool							 m_bRequestStarted = false;
	bool							 m_bCancelPending = false;
	bool							 m_bLoadFailed = false;
	bool							 m_bActive = false;
	bool                             m_bGpuReady = false;
	uint64_t                         m_nAwaitedGpuGeneration = 0u;
	bool							 m_bRetiring = false;
	uint8_t							 m_nRetireReadyFrames = 0;
	int								 m_iActiveListPosition = -1;
	int								 m_iLoadingListPosition = -1;
	int								 m_iRetiringListPosition = -1;
	Ref< Terrain3DHLODPackage >		 m_ackage;
    t3dHlodMaterialContext_t m_singleMaterialContext;
    eastl::vector< t3dHLODGroupInstance_t > m_groupInstances;
    eastl::vector< t3dHlodPrototypeRuntime_t > m_groupPrototypes;
    eastl::vector< t3dHlodSpatialCell_t > m_groupCells;
	Terrain3DSharedMemoryReservation m_memory;
} t3dEntry_t;

class Terrain3DHLODStreamer : public Object
{
	GDCLASS( Terrain3DHLODStreamer, Object );
	CLASS_NAME();

	Terrain3DOwner											 *m_pTerrain = NULL;
	Ref< Terrain3DHLODManifest >							  m_manifest;
	eastl::vector< t3dEntry_t, Terrain3DEastlAllocator >	  m_entries;
	eastl::vector< int, Terrain3DEastlAllocator >			  m_roots;
	eastl::vector< int, Terrain3DEastlAllocator >			  m_activeEntries;
	eastl::vector< int, Terrain3DEastlAllocator >			  m_loadingEntries;
	eastl::vector< int, Terrain3DEastlAllocator >			  m_retiringEntries;
	eastl::vector< int, Terrain3DEastlAllocator >			  m_loadCandidates;
	eastl::vector< uint8_t, Terrain3DEastlAllocator >		  m_instancerMinLevels;
	eastl::vector< uint8_t, Terrain3DEastlAllocator > m_instancerSubcellMasks;
	eastl::vector< t3dNodePolicy_t, Terrain3DEastlAllocator > m_nodePolicies;
	uint32_t												  m_nodePolicyOffsets[T3D_REGION_SLOTS + 1]{};
    // Cold-built 64m owner groups. -1 denotes an empty quadrant.
    int32_t m_szSubcellEntries[T3D_REGION_SLOTS][4]{};
	uint8_t													  m_szRegionRepresentationLevel[T3D_REGION_SLOTS] = {};
	uint8_t							  m_szRegionResidentRepresentationLevel[T3D_REGION_SLOTS] = {};
	uint8_t							  m_szRegionRetiringRepresentationLevel[T3D_REGION_SLOTS] = {};
	uint8_t m_szRegionReadySubcells[T3D_REGION_SLOTS] = {};
	uint8_t m_szRegionResidentSubcells[T3D_REGION_SLOTS] = {};
	eastl::bitset< T3D_REGION_SLOTS > m_managedRegions;
	eastl::bitset< T3D_REGION_SLOTS > m_detailRequiredRegions;
	bool							  m_bEnabled = true;
	String							  m_manifestPath;
	real_t							  m_fTargetErrorPixels = 3.0f;
	real_t							  m_fHysteresis = 0.20f;
	int								  m_nMaxLevel = 6;
	int								  m_nLoadBudget = 2;
	int								  m_nUnloadBudget = 4;
	int								  m_nMemoryBudgetMb = 16;
	int								  m_nMaxShadowLevel = 3;
	int								  m_nForcedDebugLevel = -1;
	int64_t							  m_nResidentEstimatedBytes = 0;
	uint64_t						  m_nReservedBytes = 0u;
	size_t							  m_nLoadCandidateCursor = 0u;
	int								  m_nLoadsLastTick = 0;
	int								  m_nUnloadsLastTick = 0;
	int								  m_nRequestsLastTick = 0;
	int								  m_nFailuresLastTick = 0;
	int								  m_nCapacityDeferredLastTick = 0;
	int64_t							  m_nTransitionOvercommitBytesLastTick = 0;
	Vector3							  m_lastSelectionTarget = Vector3();
	real_t							  m_fLastSelectionProjectionValue = -1.0f;
	real_t							  m_fLastSelectionViewportHeight = -1.0f;
	int								  m_nLastSelectionProjection = -1;
	bool							  m_bHasSelectionContext = false;
	bool							  m_bSelectionDirty = true;
	uint64_t						  m_nRdPublicationGeneration = 1u;
	uint64_t						  m_nRepresentationGeneration = 1u;
	uint32_t						  m_nDesiredGeneration = 1u;
	uint32_t						  m_nTransitionEntriesExaminedLastTick = 0u;
	mutable uint64_t				  m_nCapacityMissCount = 0u;
	uint64_t						  m_nColdRebuildCount = 0u;
	uint64_t						  m_nInstanceMutationInvalidations = 0u;

	static int			 regionIndex( const Vector2i &rRegion );
	static Vector2i		 regionFromIndex( int index );
	void				 clearRuntime( bool bNotify, bool bReleaseStorage = false );
	bool				 compileManifest( const Ref< Terrain3DHLODManifest > &rManifest );
	bool				 compilePackedManifest( const Ref< Terrain3DHLODManifest > &rManifest );
	bool				 compileLegacyNodePolicies( const Ref< Terrain3DHLODManifest > &rManifest );
	bool				 buildHierarchy( void );
    bool buildSubcellLookupCold( void );
	bool				 packageAcceptable( const t3dEntry_t &rEntry, const Vector3 &rLocalTarget ) const;
	EA_FORCE_INLINE bool isDesired(
		const t3dEntry_t &rEntry ) const {
		return rEntry.m_nDesiredGeneration == m_nDesiredGeneration;
	}
	bool			prepareTransitionStorageCold( void );
	void			beginSelectionGeneration( void );
	void			markDesired( int index );
	void			addActiveEntry( int index );
	void			removeActiveEntry( int index );
	void			addLoadingEntry( int index );
	void			removeLoadingEntry( int index );
	void			addRetiringEntry( int index );
	void			removeRetiringEntry( int index );
	void			refreshRetiringFromActive( void );
	void			selectRecursive( int index, const Vector3 &rLocalTarget );
	void			selectFallbackChildren( int index, const Vector3 &rLocalTarget );
	bool            refreshGpuReadiness( void );
	void			clearDesiredDescendants( int index );
	bool			hasActiveDescendant( int index ) const;
	bool			replacementReady( const t3dEntry_t &rEntry ) const;
	bool			requestOrActivate( int index );
	void			deactivate( int index, bool bWait = false );
	void			publishRepresentationMap( void );
	void			notifyRepresentationChanges( const uint8_t *pOldLevels, const uint8_t *pOldSubcells = NULL );
	void			markRdPublicationDirty( void );
	static uint64_t rdMeshSourceKey( int nEntryIndex, uint32_t nPrototype = 0u );

public:
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	Terrain3DHLODStreamer( void ) = default;
	EA_FORCE_INLINE ~Terrain3DHLODStreamer(
		void ) {
		destroy();
	}
	void initialize( Terrain3DOwner *pTerrain );
	void destroy( void );
	void suspend( void );
	bool reloadManifest( void );
	void updateResidency( const Vector3 &rTargetPosition, bool bForce = false );
	void onInstanceMutation( const t3dInstanceMutationEvent_t &rEvent );

	void				 setEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isEnabled(
		void ) const {
		return m_bEnabled;
	}
	void				   setManifestPath( const String &rPath );
	EA_FORCE_INLINE String getManifestPath(
		void ) const {
		return m_manifestPath;
	}
	void				   setTargetErrorPixels( real_t fPixels );
	EA_FORCE_INLINE real_t getTargetErrorPixels(
		void ) const {
		return m_fTargetErrorPixels;
	}
	void				   setHysteresis( real_t fRatio );
	EA_FORCE_INLINE real_t getHysteresis(
		void ) const {
		return m_fHysteresis;
	}
	void				setMaxLevel( int nLevel );
	EA_FORCE_INLINE int getMaxLevel(
		void ) const {
		return m_nMaxLevel;
	}
	EA_FORCE_INLINE void setLoadBudget(
		int nBudget ) {
		m_nLoadBudget = CLAMP( nBudget, 0, 64 );
	}
	EA_FORCE_INLINE int getLoadBudget(
		void ) const {
		return m_nLoadBudget;
	}
	EA_FORCE_INLINE void setUnloadBudget(
		int nBudget ) {
		m_nUnloadBudget = CLAMP( nBudget, 0, 64 );
	}
	EA_FORCE_INLINE int getUnloadBudget(
		void ) const {
		return m_nUnloadBudget;
	}
	void				setMemoryBudgetMb( int nMb );
	EA_FORCE_INLINE int getMemoryBudgetMb(
		void ) const {
		return m_nMemoryBudgetMb;
	}
	void				setMaxShadowLevel( int nLevel );
	EA_FORCE_INLINE int getMaxShadowLevel(
		void ) const {
		return m_nMaxShadowLevel;
	}
	void				setForcedDebugLevel( int nLevel );
	EA_FORCE_INLINE int getForcedDebugLevel(
		void ) const {
		return m_nForcedDebugLevel;
	}

	bool				 isRegionManaged( const Vector2i &rRegion ) const;
	bool				 isRegionDetailRequired( const Vector2i &rRegion ) const;
	EA_FORCE_INLINE bool isRegionSuppressed(
		const Vector2i &rRegion,
		int				nMinimumLevel = 1 ) const {
		return getRegionRepresentationLevel( rRegion ) >= CLAMP( nMinimumLevel, 1, 6 );
	}
	uint8_t								  getInstancerMinimumLevel( const Vector2i &rRegion, int nMeshId ) const;
	t3dHLODInstancerRepresentationState_t getInstancerRepresentationState( const Vector2i &rRegion,
																		   const int	   nMeshId ) const;
	uint8_t getNodeMinimumLevel( const Vector2i &rRegion, int64_t nSpawnId ) const;
    bool isNodeSubcellSuppressed( const Vector2i &rRegion, int64_t nSpawnId, int nMinimumLevel ) const;
	uint8_t								  getRegionRepresentationLevel( const Vector2i &rRegion ) const;
	EA_FORCE_INLINE int64_t				  getResidentEstimatedBytes(
					  void ) const {
		return m_nResidentEstimatedBytes;
	}
	EA_FORCE_INLINE bool isResidencyIdle(
		void ) const {
		return m_nLoadsLastTick == 0 && m_nRequestsLastTick == 0 && m_nUnloadsLastTick == 0;
	}
	EA_FORCE_INLINE uint64_t getRdPublicationGeneration(
		void ) const {
		return m_nRdPublicationGeneration;
	}
	EA_FORCE_INLINE uint64_t getRepresentationGeneration(
		void ) const {
		return m_nRepresentationGeneration;
	}
	bool fillRdMeshSources( eastl::vector< t3dRdExternalMeshSource_t, Terrain3DEastlAllocator > &rSources );
	bool buildRdInstanceSnapshot( t3dRdInstanceSceneSnapshot_t &rSnapshot ) const;
    EA_FORCE_INLINE size_t getRdPublicationInstanceCapacity( void ) const {
        size_t capacity = m_entries.size();
        for ( size_t i = 0u; i < m_entries.size(); ++i ) {
            const t3dEntry_t &entry = m_entries[i];
            if ( entry.m_bActive && entry.m_groupInstances.size() > 1u ) {
                capacity += entry.m_groupInstances.size() - 1u;
            }
        }
        return capacity;
    }
    EA_FORCE_INLINE size_t getRdPublicationCellCapacity( void ) const {
        size_t capacity = m_entries.size();
        for ( size_t i = 0u; i < m_entries.size(); ++i ) {
            const t3dEntry_t &entry = m_entries[i];
            if ( !entry.m_bActive ) { continue; }
            const size_t count = entry.m_groupCells.empty() ? 1u : entry.m_groupCells.size();
            if ( count > 1u ) { capacity += count - 1u; }
        }
        return capacity;
    }
    EA_FORCE_INLINE size_t getRdPublicationSourceCapacity( void ) const {
        size_t capacity = m_entries.size();
        for ( size_t i = 0u; i < m_entries.size(); ++i ) {
            const t3dEntry_t &entry = m_entries[i];
            if ( entry.m_bActive && entry.m_groupPrototypes.size() > 1u ) {
                capacity += entry.m_groupPrototypes.size() - 1u;
            }
        }
        return capacity;
    }
    EA_FORCE_INLINE size_t getRdPublicationCapacity( void ) const {
        return getRdPublicationInstanceCapacity();
    }

	Dictionary getStats( void ) const;
	Array	   getDebugEntries( void ) const;

protected:
	static void _bind_methods( void );
};
#endif // TERRAIN3D_HLOD_STREAMER_H
