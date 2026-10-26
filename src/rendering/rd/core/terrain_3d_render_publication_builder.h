// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_PUBLICATION_BUILDER_H
#define TERRAIN3D_RENDER_PUBLICATION_BUILDER_H

#include "Terrain3DTypes.h"

#include "core/utility/terrain_3d_owner.h"
#include "rendering/rd/material/terrain_3d_material_state_builder.h"
#include "rendering/rd/world/runtime/terrain_3d_static_scene_publication.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_sync.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/instances/terrain_3d_rd_instance_renderer.h"
#include "rendering/rd/ocean/terrain_3d_rd_ocean_renderer.h"
#include "rendering/rd/world/runtime/terrain_3d_world_publication.h"
#include "eastl_config.h"
#include "memory/arena/terrain_3d_memory_zone.h"

typedef struct t3dRdFrameSnapshotSlot_s
{
	t3dRdFrameSnapshot_t   m_snapshot;
	t3dRdTerrainBindings_t m_bindings;
	uint32_t			   m_nRegionMapBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	uint32_t			   m_nInstanceMeshBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	uint32_t			   m_nInstanceSceneBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	uint32_t			   m_nOceanStripBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	uint32_t			   m_nWorldPublicationBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
} t3dRdFrameSnapshotSlot_t;

typedef struct t3dRdRegionMapBank_s
{
	uint64_t		m_nGeneration = 0u;
	PackedByteArray m_bytes;
} t3dRdRegionMapBank_t;

typedef struct t3dRdInstanceMeshBank_s
{
	uint64_t				  m_nGeneration = 0u;
	t3dRdMeshSourceSnapshot_t m_snapshot;
} t3dRdInstanceMeshBank_t;

typedef struct t3dRdInstanceSceneBank_s
{
	uint64_t					 m_nGeneration = 0u;
	t3dRdInstanceSceneSnapshot_t m_snapshot;
} t3dRdInstanceSceneBank_t;

typedef struct t3dWorldPublicationBank_s
{
	uint64_t				  m_nGeneration = 0u;
	Terrain3DWorldPublication m_publication;
} t3dWorldPublicationBank_t;

class Terrain3DPublicationHost
{
protected:
	~Terrain3DPublicationHost( void ) = default;

public:
	virtual void importPublicationMegaVTPersistence( void ) = 0;
	virtual void reportPublicationError( const String &rError ) = 0;
	virtual void failPublicationRenderer( void ) = 0;
};

// Owner-thread inputs. Banks owned by the builder remain private and immutable
// while acquired by a render reader.
struct t3dPublicationServices_t
{
	Terrain3DOwner					*m_pTerrain;
	Terrain3DTerrainMegaVT			&m_terrainMegaVT;
    Terrain3DVirtualTexture &m_virtualTexture;
	Terrain3DMaterialStateBuilder	&m_materialStateBuilder;
	Terrain3DStaticScenePublication &m_staticScene;
	t3dMaterialStateSources_t		 m_materialSources;
	Terrain3DPublicationHost		&m_host;
};

// Borrowed read-only diagnostics. Consume within the owner/lifecycle boundary.
struct t3dPublicationDiagnosticsView_t
{
	const bool						   &m_bColdCapacityServiceActive;
	const bool						   &m_bInstanceMeshBankCapacityPending;
	const bool						   &m_bInstancePublicationCapacityPending;
	const bool						   &m_bRegionMapHeightTextureReady;
	const t3dRdMeshSourceSnapshot_t	   &m_cachedInstanceMeshes;
	const t3dRdInstanceSceneSnapshot_t &m_cachedInstanceScene;
	const t3dRdTerrainBindings_t	   &m_lastPublishedResources;
	const uint64_t					   &m_nCapacityMissCount;
	const uint64_t					   &m_nColdCapacityRebuildCount;
	const t3dAtomicU64_t			   &m_nConsumedSnapshotGeneration;
	const uint64_t					   &m_nMeshSourceBuildCount;
	const uint64_t					   &m_nOceanStaticStateBuildCount;
	const uint64_t					   &m_nOwnerGrowthDeniedCount;
	const t3dAtomicU64_t			   &m_nPublishedSnapshotGeneration;
	const t3dAtomicU32_t			   &m_nPublishedSnapshotIndex;
	const uint64_t					   &m_nRegionLightingScanCount;
	const t3dAtomicU64_t			   &m_nSkippedSnapshotGenerations;
	const t3dAtomicU64_t			   &m_nSnapshotConsumes;
	const t3dAtomicU64_t			   &m_nSnapshotExchangeToken;
	const t3dAtomicU64_t			   &m_nSnapshotPublications;
	const t3dAtomicU64_t			   &m_nSnapshotReuses;
	const t3dAtomicU64_t			   &m_nStaleSnapshots;
};

class Terrain3DRenderPublicationBuilder
{
	CLASS_NAME_STATIC( "Terrain3DRenderPublicationBuilder" );

private:
    t3dEnvironmentRegionClimate_t m_szEnvironmentRegionClimate[T3D_REGION_SLOTS]{};
    uint64_t m_nEnvironmentRegionClimateEpoch = 1u;
    uint64_t m_nEnvironmentRegionOverrideEpoch = 0u;
    t3dEnvironmentForcing_t m_environmentForcing{};
    t3dEnvironmentImpulse_t m_szEnvironmentPending[T3D_ENVIRONMENT_MAX_IMPULSES]{};
    uint32_t m_nEnvironmentPendingCount = 0u;
    uint64_t m_nEnvironmentImpulseEpoch = 0u;
    uint64_t m_nEnvironmentImpulseDropped = 0u;
	t3dRdFrameSnapshotSlot_t  m_snapshotSlots[T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT]{};
	t3dRdRegionMapBank_t	  m_regionMapBanks[T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT]{};
	t3dRdInstanceMeshBank_t	  m_instanceMeshBanks[T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT]{};
	t3dRdInstanceSceneBank_t  m_instanceSceneBanks[T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT]{};
	t3dRdOceanStripBank_t	  m_oceanStripBanks[T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT]{};
	t3dWorldPublicationBank_t m_worldPublicationBanks[T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT]{};
	t3dAtomicU32_t			  m_nRegionMapReaderBank{ T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX };
	t3dAtomicU32_t			  m_nInstanceMeshReaderBank{ T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX };
	t3dAtomicU32_t			  m_nInstanceSceneReaderBank{ T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX };
	t3dAtomicU32_t			  m_nOceanStripReaderBank{ T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX };
	t3dAtomicU32_t			  m_nWorldPublicationReaderBank{ T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX };
	uint32_t				  m_nCurrentRegionMapBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	uint32_t				  m_nCurrentInstanceMeshBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	uint32_t				  m_nCurrentInstanceSceneBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	uint32_t				  m_nCurrentOceanStripBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	uint32_t				  m_nCurrentWorldPublicationBank = T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX;
	t3dAtomicU64_t			  m_nSnapshotExchangeToken{ t3dRdMakeFrameSnapshotToken( 1u, 0u, false ) };
	uint32_t				  m_nSnapshotWriteIndex = 2u;
	uint32_t				  m_nSnapshotRenderIndex = 0u;
	uint64_t				  m_nSnapshotRenderGeneration = 0u;
	bool					  m_bSnapshotRenderValid = false;
	uint64_t				  m_nNextSnapshotGeneration = 1u;
	t3dAtomicU32_t			  m_nPublishedSnapshotIndex{ T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX };
	t3dAtomicU64_t			  m_nPublishedSnapshotGeneration{ 0u };
	t3dAtomicU64_t			  m_nConsumedSnapshotGeneration{ 0u };
	t3dAtomicU64_t			  m_nSnapshotPublications{ 0u };
	t3dAtomicU64_t			  m_nSnapshotConsumes{ 0u };
	t3dAtomicU64_t			  m_nSnapshotReuses{ 0u };
	t3dAtomicU64_t			  m_nSkippedSnapshotGenerations{ 0u };
	t3dAtomicU64_t			  m_nStaleSnapshots{ 0u };
	t3dAtomicU64_t			  m_nLastStaleSnapshotToken{ 0u };

	uint64_t			   m_nRegionTopologyGeneration = 0u;
	uint64_t			   m_nRegionLightingSignature = 0u;
	uint64_t			   m_nRegionMapGeneration = 0u;
	uint64_t			   m_nTerrainMegaVTPublicationGeneration = 0u;
    uint64_t m_nFarVTPublicationGeneration = UINT64_MAX;
    uint64_t m_nFarAvtSourcePublicationRevision = UINT64_MAX;
	uint64_t			   m_nTerrainMegaVTPersistenceTopologyGeneration = UINT64_MAX;
	uint64_t			   m_nPhysicalRegionMapGeneration = 0u;
	uint64_t			   m_nLastRegionCacheSourceGeneration = 0u;
	uint64_t			   m_nRegionCacheGeneration = 0u;
	uint64_t			   m_nLastPatchFingerprint = 0u;
	uint64_t			   m_nPatchGeneration = 0u;
	t3dRdPatch_t		   m_previousPatches[T3D_MAX_CLIPMAP_LEVELS]{};
	uint32_t			   m_nPreviousPatchCount = 0u;
	uint64_t			   m_nTerrainTextureGeneration = 0u;
	uint64_t			   m_nMaterialTextureGeneration = 0u;
	t3dRdTerrainBindings_t m_lastPublishedResources;
	bool				   m_bRegionMapInitialized = false;
	bool				   m_bRegionMapHeightTextureReady = false;
	bool				   m_bTerrainMegaVTReady = false;
	bool				   m_bRegionCacheInitialized = false;
	bool				   m_bPatchStateInitialized = false;
	uint64_t			   m_nRegionLightingSourceGeneration = 0u;
	uint64_t			   m_nRegionLightingScanCount = 0u;
	uint64_t			   m_nMeshSourceBuildCount = 0u;
	uint64_t			   m_nOceanStaticStateBuildCount = 0u;
	bool				   m_bPublishedResourcesInitialized = false;

	t3dRdOceanStripSnapshot_t											m_cachedOceanStrip;
	bool																m_bCachedOceanStripValid = false;
	uint64_t															m_nOceanStripGeneration = 0u;
	t3dRdMeshSourceSnapshot_t											m_cachedInstanceMeshes;
	t3dRdInstanceSceneSnapshot_t										m_cachedRegularInstanceScene;
	t3dRdInstanceSceneSnapshot_t										m_cachedHlodInstanceScene;
	t3dRdInstanceSceneSnapshot_t										m_cachedInstanceScene;
	bool																m_bCachedInstanceMeshesValid = false;
	bool																m_bCachedRegularInstanceSceneValid = false;
	bool																m_bCachedHlodInstanceSceneValid = false;
	bool																m_bCachedInstanceSceneValid = false;
	eastl::vector< t3dRdExternalMeshSource_t, Terrain3DEastlAllocator > m_cachedHlodMeshSources;
	uint64_t															m_nInstanceMeshSourceGeneration = 0u;
	uint64_t															m_nInstanceMeshGeneration = 0u;
	uint64_t															m_nInstanceSceneGeneration = 0u;
	uint64_t															m_nInstanceOcclusionGeneration = 0u;
	uint64_t															m_nHlodSceneGeneration = 0u;
	uint64_t															m_nCombinedInstanceSceneGeneration = 0u;
	t3dRdOceanState_t													m_cachedOceanStaticState{};
	uint64_t															m_nOceanStaticSourceGeneration = 0u;
	bool																m_bOceanStaticStateInitialized = false;
	bool																m_bColdCapacityServiceActive = false;
	bool																m_bInstancePublicationCapacityPending = true;
	bool																m_bInstanceMeshBankCapacityPending = true;
	bool																m_bWorldPublicationCapacityPending = true;
	t3dWorldPublicationCapacity_t										m_worldPublicationRequired{};
	uint64_t															m_nWorldPublicationGeneration = 0u;
	uint64_t															m_nWorldMeshPayloadReuseCount = 0u;
	uint64_t															m_nWorldGeometryGeneration = 0u;
	uint64_t															m_nWorldMaterialGeneration = 0u;
	uint64_t															m_nWorldStaticSignature = 0u;
	uint64_t															m_nWorldTerrainSignature = 0u;
	uint64_t															m_nColdCapacityRebuildCount = 0u;
	uint64_t															m_nCapacityMissCount = 0u;
	uint64_t															m_nOwnerGrowthDeniedCount = 0u;

	uint32_t selectWritablePublicationBank( uint32_t nReaderBank, uint32_t nPendingBank ) const;
	bool	 hasInstancePublicationCapacity( const t3dPublicationServices_t &rServices ) const;
	bool	 hasInstanceMeshBankCapacity( void ) const;
	bool	 ensureInstancePublicationStorage( const t3dPublicationServices_t &rServices );
	bool	 ensureInstanceMeshBankStorage( void );
	bool	 hasWorldPublicationCapacity( const t3dWorldPublicationCapacity_t &rRequired ) const;
	bool	 ensureWorldPublicationStorage( void );
	void	 requestWorldPublicationCapacity( const t3dWorldPublicationCapacity_t &rRequired );
	bool	 publishWorldState( const t3dPublicationServices_t &rServices,
								t3dRdFrameSnapshotSlot_t	   &rPublication,
								const t3dRdFrameSnapshot_t	   &rSnapshot,
								const t3dRdFrameSnapshotSlot_t *pPending );
	void	 recordCapacityMiss( const char *pszOwner, size_t nRequired, size_t nCapacity, bool bMeshBank );
	void	 publishPatchState( const t3dPublicationServices_t &rServices, t3dRdFrameSnapshot_t &rSnapshot );
	void	 publishLightingState( const t3dPublicationServices_t &rServices, t3dRdFrameSnapshot_t &rSnapshot ) const;
	bool	 publishOceanState( const t3dPublicationServices_t &rServices,
								t3dRdFrameSnapshotSlot_t	   &rPublication,
								t3dRdFrameSnapshot_t		   &rSnapshot,
								const t3dRdFrameSnapshotSlot_t *pPending );
	bool	 publishRegionState( const t3dPublicationServices_t &rServices,
								 t3dRdFrameSnapshotSlot_t		&rPublication,
								 t3dRdFrameSnapshot_t			&rSnapshot,
								 const t3dRdFrameSnapshotSlot_t *pPending );
	bool	 publishInstanceState( const t3dPublicationServices_t &rServices,
								   t3dRdFrameSnapshotSlot_t		  &rPublication,
								   const t3dRdFrameSnapshotSlot_t *pPending );
	void	 commitSnapshotPublication( const t3dPublicationServices_t &rServices, t3dRdFrameSnapshot_t &rSnapshot );
	void	 recordStaleSnapshot( uint64_t nToken );

public:
	t3dPublicationDiagnosticsView_t getDiagnosticsView(
		void ) const {
		return { m_bColdCapacityServiceActive,
				 m_bInstanceMeshBankCapacityPending,
				 m_bInstancePublicationCapacityPending,
				 m_bRegionMapHeightTextureReady,
				 m_cachedInstanceMeshes,
				 m_cachedInstanceScene,
				 m_lastPublishedResources,
				 m_nCapacityMissCount,
				 m_nColdCapacityRebuildCount,
				 m_nConsumedSnapshotGeneration,
				 m_nMeshSourceBuildCount,
				 m_nOceanStaticStateBuildCount,
				 m_nOwnerGrowthDeniedCount,
				 m_nPublishedSnapshotGeneration,
				 m_nPublishedSnapshotIndex,
				 m_nRegionLightingScanCount,
				 m_nSkippedSnapshotGenerations,
				 m_nSnapshotConsumes,
				 m_nSnapshotExchangeToken,
				 m_nSnapshotPublications,
				 m_nSnapshotReuses,
				 m_nStaleSnapshots };
	}

	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	bool initializeFixedStorageCold( void );
	void serviceColdCapacity( const t3dPublicationServices_t &rServices );
    void setEnvironmentForcingOwner( const t3dEnvironmentForcing_t &rForcing );
    void clearEnvironmentRegionClimateOwner();
    bool setEnvironmentRegionClimateOwner( const Vector2i &rRegion, const Vector3 &rClimate );
    Vector3 getEnvironmentRegionClimateOwner( const Vector2i &rRegion ) const;
    PackedByteArray exportEnvironmentRegionClimateOwner() const;
    bool importEnvironmentRegionClimateOwner( const PackedByteArray &rData );
    uint64_t getEnvironmentRegionClimateEpochOwner() const { return m_nEnvironmentRegionClimateEpoch; }
    uint32_t submitEnvironmentImpulsesOwner( const t3dEnvironmentImpulse_t *pEvents, uint32_t nCount );
    uint64_t getEnvironmentDroppedCountOwner() const { return m_nEnvironmentImpulseDropped; }
	void publish( const t3dPublicationServices_t &rServices );
	const t3dRdFrameSnapshotSlot_t *acquireOnRenderThread( void );
	void							initializeOcean( const t3dPublicationServices_t &rServices );
	void							clearCachedSources( void );
	void							reset( void );

	const t3dRdRegionMapBank_t *getRegionMapBank(
		uint32_t nBank ) const {
		return nBank < T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ? &m_regionMapBanks[nBank] : NULL;
	}
	const t3dRdInstanceMeshBank_t *getInstanceMeshBank(
		uint32_t nBank ) const {
		return nBank < T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ? &m_instanceMeshBanks[nBank] : NULL;
	}
	const t3dRdInstanceSceneBank_t *getInstanceSceneBank(
		uint32_t nBank ) const {
		return nBank < T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ? &m_instanceSceneBanks[nBank] : NULL;
	}
	const t3dRdOceanStripBank_t *getOceanStripBank(
		uint32_t nBank ) const {
		return nBank < T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ? &m_oceanStripBanks[nBank] : NULL;
	}
	const t3dWorldPublicationBank_t *getWorldPublicationBank(
		uint32_t nBank ) const {
		return nBank < T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT ? &m_worldPublicationBanks[nBank] : NULL;
	}
	bool hasRequiredInstancePublicationCapacity(
		const t3dPublicationServices_t &rServices ) const {
		return hasInstancePublicationCapacity( rServices );
	}
	bool hasRequiredInstanceMeshBankCapacity(
		void ) const {
		return hasInstanceMeshBankCapacity();
	}
	bool isInstancePublicationCapacityPending(
		void ) const {
		return m_bInstancePublicationCapacityPending;
	}
	bool isInstanceMeshBankCapacityPending(
		void ) const {
		return m_bInstanceMeshBankCapacityPending;
	}
	bool isColdCapacityServiceActive(
		void ) const {
		return m_bColdCapacityServiceActive;
	}
	void invalidateTerrainMegaVTPersistence(
		void ) {
		m_nTerrainMegaVTPersistenceTopologyGeneration = UINT64_MAX;
	}
	uint64_t getTerrainTextureGeneration(
		void ) const {
		return m_nTerrainTextureGeneration;
	}
	uint64_t getMaterialTextureGeneration(
		void ) const {
		return m_nMaterialTextureGeneration;
	}
	bool isRegionMapHeightTextureReady(
		void ) const {
		return m_bRegionMapHeightTextureReady;
	}
	bool isPublishedHeightTextureValid(
		void ) const {
		return m_lastPublishedResources.m_hHeightTextureRd.is_valid();
	}
	uint64_t getRegionLightingScanCount(
		void ) const {
		return m_nRegionLightingScanCount;
	}
	uint64_t getMeshSourceBuildCount(
		void ) const {
		return m_nMeshSourceBuildCount;
	}
	uint64_t getOceanStaticStateBuildCount(
		void ) const {
		return m_nOceanStaticStateBuildCount;
	}
	const t3dRdInstanceSceneSnapshot_t &getCachedInstanceScene(
		void ) const {
		return m_cachedInstanceScene;
	}
	const t3dRdMeshSourceSnapshot_t &getCachedInstanceMeshes(
		void ) const {
		return m_cachedInstanceMeshes;
	}
	uint64_t getColdCapacityRebuildCount(
		void ) const {
		return m_nColdCapacityRebuildCount;
	}
	uint64_t getCapacityMissCount(
		void ) const {
		return m_nCapacityMissCount;
	}
	uint64_t getOwnerGrowthDeniedCount(
		void ) const {
		return m_nOwnerGrowthDeniedCount;
	}
	uint64_t getSnapshotToken(
		void ) const {
		return m_nSnapshotExchangeToken.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	bool isSnapshotExchangeLockFree(
		void ) const {
		return m_nSnapshotExchangeToken.is_lock_free();
	}
	uint32_t getPublishedSnapshotIndex(
		void ) const {
		return m_nPublishedSnapshotIndex.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	uint64_t getPublishedSnapshotGeneration(
		void ) const {
		return m_nPublishedSnapshotGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	uint64_t getConsumedSnapshotGeneration(
		void ) const {
		return m_nConsumedSnapshotGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	uint64_t getSnapshotPublicationCount(
		void ) const {
		return m_nSnapshotPublications.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	uint64_t getSnapshotConsumeCount(
		void ) const {
		return m_nSnapshotConsumes.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	uint64_t getSnapshotReuseCount(
		void ) const {
		return m_nSnapshotReuses.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	uint64_t getSkippedSnapshotGenerationCount(
		void ) const {
		return m_nSkippedSnapshotGenerations.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	uint64_t getStaleSnapshotCount(
		void ) const {
		return m_nStaleSnapshots.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	uint64_t getWorldMeshPayloadReuseCount(
		void ) const {
		return m_nWorldMeshPayloadReuseCount;
	}
	size_t getRetainedCpuBytes( void ) const;
};

#endif // TERRAIN3D_RENDER_PUBLICATION_BUILDER_H
