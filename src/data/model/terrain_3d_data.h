// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_DATA_CLASS_H
#define TERRAIN3D_DATA_CLASS_H

#include "Terrain3DTypes.h"

#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "core/config/terrain_3d_startup_state.h"
#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_owner.h"
#include "assets/resources/generated_texture.h"
#include "data/model/terrain_3d_data_access.h"
#include "data/runtime/terrain_3d_data_residency_adapter.h"
#include "data/editing/terrain_3d_map_edit_service.h"
#include "data/model/terrain_3d_region.h"
#include "data/persistence/terrain_3d_region_persistence.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/runtime/terrain_3d_runtime_artifact_service.h"
#include "data/storage/terrain_3d_raster_tile_store.h"
#include "data/cooking/terrain_3d_raster_hierarchy_builder.h"
#include "data/persistence/terrain_3d_raster_bundle.h"
#include "data/persistence/terrain_3d_raster_hierarchy_bundle.h"
#include "streaming/residency/terrain_3d_tile_residency_controller.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "eastl_config.h"
#include "streaming/policy/terrain_3d_runtime_profile.h"

#include <EASTL/array.h>
#include <EASTL/vector.h>
#include <stdint.h>
#include "rendering/compute/terrain_3d_compute.h"

class Terrain3DErosionSettings;
class Terrain3DErosionJob;

class Terrain3DData : public Object, public Terrain3DDataAccess
{
	GDCLASS( Terrain3DData, Object );
	CLASS_NAME();
	friend class Terrain3D;
	friend class Terrain3DRegionResidency;

public: // Constants
		// Maximum logical grid. The active grid is T3D_ACTIVE_WORLD_TEXELS / region_size.
private:
	Terrain3DOwner				*m_pTerrain = NULL;
	Terrain3DMemoryZone			*m_pRuntimeAllocatorZone = NULL;
	t3dAllocatorOverflowPolicy_t m_eRuntimeAllocatorOverflowPolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT;

	Terrain3DRegionRepository		m_regionRepository;
	Terrain3DSamplingStorage		m_samplingStorage;
	Terrain3DMapEditService			m_mapEditService;
	Terrain3DRegionPersistence		m_regionPersistence;
	Terrain3DRuntimeArtifactService m_runtimeArtifacts;
	// Derived sparse source pixels; authored Image/region ABI remains authoritative.
	Terrain3DRasterTileStore				 m_sparseRasterTiles;
	Terrain3DRasterHierarchyBuilder			 m_sparseRasterHierarchy;
	Terrain3DRasterBundleIndex				 m_sparseRasterIndex;
	Terrain3DRasterHierarchyIndex			 m_sparseHierarchyIndex;
	mutable Terrain3DTileResidencyController m_sparseTileResidency;
	eastl::bitset< T3D_REGION_SLOTS >		 m_sparseRegionInitialized;
	// An edit may invalidate one saved leaf without invalidating every other
	// immutable tile in the cooked index. Ancestor fallback is withheld until
	// the next coherent directory save because parents aggregate many leaves.
	eastl::bitset< T3D_REGION_SLOTS > m_sparseDirtySavedRegions;
	bool							  m_bSparseHierarchyStale = false;
	bool							  m_bSparseBundleLoaded = false;
	Terrain3DDataResidencyAdapter	  m_residency;
	bool m_bRuntimeActive = false;
	uint64_t m_nMapEditCommitCalls = 0u;
	uint64_t m_nMapEditDerivedUsecLast = 0u;
	uint64_t m_nMapEditPublicationUsecLast = 0u;
	uint64_t m_nMapEditCollisionQueueUsecLast = 0u;
	bool m_bMapEditInteractiveLast = false;
	bool m_bMapEditComputeBackendRequestedLast = false;
	bool m_bMapEditCollisionRequestedLast = false;

	TypedArray< Image > m_heightMaps;
	TypedArray< Image > m_controlMaps;
	TypedArray< Image > m_colorMaps;
	TypedArray< Image > m_derivedMaps; // Runtime encoded normal XZ cache (RG8).

	// Shared implicit map payloads. Regions retain null control/color references until
	// a write materializes them, while GPU arrays still receive valid layers.
	Ref< Image > m_defaultHeightMap;
	Ref< Image > m_defaultControlMap;
	Ref< Image > m_defaultColorMap;
	Ref< Image > m_defaultDerivedMap;
	bool		 m_bControlMapsImplicit = true;
	bool		 m_bColorMapsImplicit = true;

	// Editing occurs on the Image arrays above, which are converted to Texture arrays
	// below for the shader.

	// Initial directory loading builds the CPU/GPU data once, before listeners are
	// initialized. Suppress redundant startup signals/snaps and defer spare layers.
	bool						  m_bStartupBulkLoad = false;
	bool						  m_bSkipSlotReserveOnce = false;
	uint32_t					  m_nStartupGenerationLast = 0u;
	t3dStartupDomainState_t		  m_startupState;
	uint64_t					  m_nStartupInitializeCount = 0u;
	uint64_t					  m_nStartupInitializeUsecLast = 0u;
	uint64_t					  m_nStartupDirectoryLoadCount = 0u;
	uint64_t					  m_nStartupDirectoryLoadUsecLast = 0u;
	uint64_t					  m_nStartupRehydrateCount = 0u;
	uint64_t					  m_nStartupRehydrateUsecLast = 0u;
	uint64_t					  m_nStartupBytesReadLast = 0u;
	uint32_t					  m_nStartupRegionCountLast = 0u;
	t3dStagedRepository_s		  m_startupPreparedRepository;
	Terrain3DRuntimeWorldManifest m_startupPreparedWorldManifest;
	String						  m_startupPreparedDirectory;
	bool						  m_bStartupPreparationValid = false;
	bool						  m_bStartupInitialDirectoryLoad = false;
	bool						  m_bStartupPreparedDirectory = false;
	bool						  m_bStartupPreparedBoundedCpuLoad = false;
	bool						  m_bStartupPreparedCookedManifest = false;
	bool						  m_bStartupNeedsRehydrate = false;
	bool						  m_bStartupDerivedPending = false;

	// These contain direct RenderingDevice Texture2DArray RIDs. In bounded
	// residency mode they hold physical cache layers rather than repository slots.
	Terrain3DGeneratedTexture m_generatedHeightMaps;
	Terrain3DGeneratedTexture m_generatedControlMaps;
	Terrain3DGeneratedTexture m_generatedColorMaps;
	Terrain3DGeneratedTexture m_generatedDerivedMaps;
	Terrain3DGeneratedTexture m_generatedMiddleCompositeMaps;
	Terrain3DGeneratedTexture m_generatedTerrainShadowMasks;
	// Optional compute backend. It is lazy so Compatibility/headless runtimes
	// retain the existing CPU path without constructing RenderingDevice state.
	Terrain3DCompute *m_pCompute = NULL;
	// Functions
	static bool	 ensureHeightRangeRegionResident( void *pContext, const Vector2i &rLocation );
	static Error prepareImportRegion( void *pContext, const Vector2i &rLocation, Ref< Terrain3DRegion > &rRegion );
	static bool	 ensureImageExportRegionResident( void *pContext, const Vector2i &rLocation );

	void				 clear( void );
	bool				 rebuildSparseRasterSnapshotCold( void );
	void				 publishSparseRasterEdit( t3dMapType_t eType, const Vector2i &rLocation, const Rect2i &rDirty );
	EA_FORCE_INLINE void invalidateSamplingCache(
		void ) {
		m_samplingStorage.invalidateAllSamplers();
	}
	EA_FORCE_INLINE void publishRegionSamplingGeneration(
		const Vector2i &vLocation ) {
		m_samplingStorage.publishRegionGeneration( vLocation );
	}

	void				 invalidateBakedWorldManifest( const String &rDataDirectory );
	void				 ensureDefaultMaps( void );
	bool				 setRegionSizeInternal( void );
	EA_FORCE_INLINE bool bindActiveStorageInternal(
		void  *pBacking,
		size_t nBackingBytes ) {
		return m_samplingStorage.bindActive( pBacking, nBackingBytes );
	}
	EA_FORCE_INLINE bool isActiveStorageOwnerInternal(
		void ) const {
		return m_samplingStorage.isActiveOwner();
	}
	EA_FORCE_INLINE void resetResidentSamplingPoolInternal(
		void ) {
		m_samplingStorage.resetResidentPool();
	}
	Error				   rehydrateActiveStorage( void );
	void				   resetStartupPreparationCold( void );
	Error				   prepareDirectoryCandidateCold( const String			&rDir,
														  t3dStagedRepository_s &rStaged,
														  bool					&rBoundedCpuLoad,
														  bool					&rCookedRuntimeManifest );
	Error				   commitDirectoryCandidateCold( const String		   &rDir,
														 t3dStagedRepository_s &rStaged,
														 bool					bBoundedCpuLoad,
														 bool					bCookedRuntimeManifest,
														 bool					bFinalizeDerived );
	void				   finalizeDirectoryCandidateCold( bool bCookedRuntimeManifest );
	bool				   syncActiveRegion( const Vector2i &vLocation, const Terrain3DRegion *pRegion );
	Ref< Image >		   getRegionMapOrDefault( const Terrain3DRegion *pRegion, t3dMapType_t mapType ) const;
	Ref< Image >		   makeGpuColorImage( const Ref< Image > &rImage ) const;
	Ref< Image >		   makeGpuDerivedImage( const Ref< Image > &rImage ) const;
	Ref< Image >		   makeTerrainShadowMaskImage( const Terrain3DRegion *pRegion ) const;
	Ref< Image >		   makeGpuRenderControlImage( const Vector2i &vLocation ) const;
	Ref< Image >		   makeMiddleCompositeImage( const Vector2i		&rLocation,
													 const Ref< Image > &rControl,
													 const Ref< Image > &rColor ) const;
	t3dRuntimePageConfig_s getRuntimePageConfig( void ) const;
	uint64_t			   computeGpuRuntimePageSourceSignature( const Terrain3DRegion *pRegion ) const;
	bool				   readGpuRuntimePageSourceSignature( const String &rPagePath, uint64_t &nSignature ) const;
	String				   getGpuRuntimePagePath( const Terrain3DRegion *pRegion ) const;
	bool				   collectGpuRegionPageSources( String											   &rDataDirectory,
														PackedStringArray								   &rSourceFiles,
														eastl::vector< Vector2i, Terrain3DEastlAllocator > &rLocations,
														PackedStringArray								   &rErrors ) const;
	void				   pollGpuRuntimePagePrepare( bool bWait );
	void				   requestGpuRuntimePage( const Vector2i &vLocation, uint16_t nConsumers, uint8_t nPriority );
	void				   serviceGpuRuntimePagePrepare( const t3dResidencyFrameState_s &rFrame );
	bool				   isGpuRuntimePagePreparePending( const Vector2i &vLocation ) const;
	bool				   takePreparedRuntimeHeight( const Vector2i  &vLocation,
													  uint16_t		   nConsumer,
													  Ref< Image >	  &rHeight,
													  bool			   bMiddle,
													  PackedByteArray *pNativeHeightBytes = NULL );
	bool				   takePreparedRuntimeInstance( const Vector2i &vLocation, Terrain3DInstanceStore &rStore );
	bool				   takePreparedGpuRuntimePage( const Vector2i &rLocation, t3dRuntimeGpuPageResult_t &rResult );
#if defined( T3D_EDITOR_NATIVE )
	bool encodeGpuRuntimePage( Terrain3DRegion *pRegion, PackedByteArray &rBytes, String *pError = NULL );
#endif
	bool							   hasMaterializedMap( t3dMapType_t mapType ) const;
	bool							   refreshImplicitMapState( void );
	int								   getRegionSlotReserve( void ) const;
	bool							   canUpdateRegionSlotsIncrementally( void ) const;
	void							   syncCacheDefaultMaps( void );
	t3dMiddleHeightRegionCacheConfig_t getMiddleHeightCacheConfig( void ) const;
	void		applyInstanceRegionCacheChanges( const t3dInstanceRegionCacheChanges_s &rChanges );
	static void residencyApplyInstanceChanges( void *pContext, const t3dInstanceRegionCacheChanges_s &rChanges );
	static void residencyGpuPageTableChanged( void *pContext );
	void		resetGpuRegionCache( bool bClearTextures );
	void		armPendingGpuRegionRetirements( void );
	void		invalidateGpuRegionLocation( const Vector2i &vLocation );
	bool		republishGpuRegionLocation( const Vector2i &vLocation );
	void		queueGpuRegionRepublish( const Vector2i &vLocation );
	void		resetMiddleHeightCache( void );
	bool		configureMiddleHeightCache( void );
	void		updateMiddleHeightResidencyInternal( const t3dResidencyFrameState_s &rFrame, bool bForce );
	void		resetCpuRegionCache( bool bReleaseCleanMaps );
	void		invalidateCpuRegionLocation( const Vector2i &vLocation );
	bool		requestCpuRegionSourceAsync( const Vector2i &vLocation );
	bool		ensureCpuRegionResidentInternal( const Vector2i &vLocation, bool bDemandLoad );
	void		resetInstanceRegionCache( bool bRestoreAll );
	void		invalidateInstanceRegionLocation( const Vector2i &vLocation );
	bool		ensureInstanceRegionResidentInternal( const Vector2i &vLocation, bool bDemandLoad );
	void		updateMapsBounded( const t3dMapType_t mapType, const bool bAllRegions );
	void		emitGpuRegionMapChanged( void );
	void		emitRegionLayoutChanged( const bool bTextureLayersChanged );
	TypedArray< Image > getActiveMaps( const t3dMapType_t mapType ) const;
	void				copyPasteDfr( const Terrain3DRegion *pSrcRegion,
									  const Rect2i			&rSrcRect,
									  const Rect2i			&rDstRect,
									  Terrain3DRegion		*pDstRegion );
	Error		addRegionInternal( const Ref< Terrain3DRegion > &rRegion, const bool bUpdate, const bool bSanitize );
	static bool ensurePersistenceRegionResident( void *pContext, const Vector2i &rLocation );
	t3dResidencyFrameState_s makeResidencyFrameState( const Vector3 &rWorldPosition ) const;
	void					 updateCpuRegionResidencyInternal( const t3dResidencyFrameState_s &rFrame, bool bForce );
	void updateInstanceRegionResidencyInternal( const t3dResidencyFrameState_s &rFrame, bool bForce );
	void serviceColdInstanceResidencyInternal( const t3dResidencyFrameState_s &rFrame, bool bForce );
	void updateGpuRegionResidencyInternal( const t3dResidencyFrameState_s &rFrame, bool bForce );
	bool releaseRuntimeAllocatorStorageInternal( void );
	bool bindRuntimeAllocatorInternal( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );

public:
	Terrain3DData( void );
	Error beginStartupCold( Terrain3DOwner *pTerrain, uint32_t nStartupGeneration );
	Error prepareStartupCold( uint32_t nStartupGeneration );
	Error commitStartupCold( uint32_t nStartupGeneration );
	void  finalizeStartupDerivedCold( uint32_t nStartupGeneration );
	void  cancelStartupCold( uint32_t nStartupGeneration );
	bool  isStartupCpuReady( const uint32_t nStartupGeneration ) const;
	bool  isStartupCommitted( const uint32_t nStartupGeneration ) const;
	void  deactivateRuntimeStorage( Terrain3DOwner *pTerrain );
	~Terrain3DData( void );
	EA_FORCE_INLINE bool isInitializedFor(
		const Terrain3DOwner *pTerrain ) const {
		return m_bRuntimeActive && m_pTerrain == pTerrain;
	}
	EA_FORCE_INLINE t3dStartupDomainSnapshot_t getStartupSnapshot(
		void ) const {
		return t3dStartupDomainSnapshot( m_startupState );
	}
	EA_FORCE_INLINE bool isRuntimeReady(
		void ) const {
		return m_bRuntimeActive && m_samplingStorage.isActiveOwner() && m_regionRepository.getRegionMapSize() > 0;
	}
	EA_FORCE_INLINE bool isRepositoryLoaded(
		void ) const {
		return m_regionRepository.isLoaded();
	}
	EA_FORCE_INLINE uint32_t getActiveStorageGeneration(
		void ) const {
		return isRuntimeReady() ? m_samplingStorage.getActiveGeneration() : 0u;
	}
	Dictionary getRuntimeState( void ) const;

	// Regions

	EA_FORCE_INLINE int getRegionCount(
		void ) const override {
		return m_regionRepository.getActiveRegionCount();
	}
	void				   setRegionLocations( const TypedArray< Vector2i > &vLocations );
	TypedArray< Vector2i > getRegionLocations( void ) const override;
	// Internal layer-ordered slots, including s_v2iMax spare entries. Custom
	// shaders require this exact ordering to match the texture arrays.
	TypedArray< Vector2i >					getRegionSlots( void ) const;
	TypedArray< Vector2i >					getRenderRegionSlots( void ) const;
	EA_FORCE_INLINE const t3dRegionCoord_t *getRegionSlotsInternal(
		void ) const {
		return m_regionRepository.getSlotsData();
	}
	EA_FORCE_INLINE int getRegionSlotCountInternal(
		void ) const {
		return m_regionRepository.getSlotCount();
	}
	EA_FORCE_INLINE Terrain3DRegion *const *getRegionPtrMapInternal(
		void ) const {
		return m_regionRepository.getRegionPtrMapData();
	}
	EA_FORCE_INLINE int getRegionSizeInternal(
		void ) const {
		return T3D_REGION_SIZE;
	}
	EA_FORCE_INLINE int getRegionMapSizeInternal(
		void ) const {
		return T3D_REGION_MAP_SIZE;
	}
	EA_FORCE_INLINE bool matchesRegionLayoutInternal(
		const int nRegionSize ) const {
		return nRegionSize == T3D_REGION_SIZE;
	}
	EA_FORCE_INLINE t3dRegionRepositoryView_t getRegionRepositoryViewInternal(
		void ) const {
		return m_regionRepository.getView();
	}
	EA_FORCE_INLINE uint64_t getRegionTopologyGenerationInternal(
		void ) const {
		return m_regionRepository.getTopologyGeneration();
	}
	EA_FORCE_INLINE uint64_t getPhysicalRegionMapGenerationInternal(
		void ) const {
		return m_residency.gpuRegionCache().getPhysicalPageTableGeneration();
	}
	EA_FORCE_INLINE const uint16_t *getPhysicalRegionMapDataInternal(
		void ) const {
		return m_residency.gpuRegionCache().isEnabled() ? m_residency.gpuRegionCache().getRegionMapData()
														: m_regionRepository.getLogicalRegionMapData();
	}
	EA_FORCE_INLINE const uint16_t *getLogicalRegionMapDataInternal(
		void ) const {
		return m_regionRepository.getLogicalRegionMapData();
	}
	EA_FORCE_INLINE bool isRegionLocationValidInternal(
		const Vector2i &vLocation ) const {
		return m_regionRepository.isLocationValid( vLocation );
	}
	EA_FORCE_INLINE real_t getVertexSpacingInternal(
		void ) const {
		return m_regionRepository.getVertexSpacing();
	}
	t3dSamplingSource_t getSamplingSourceInternal(
		void ) const {
		return t3dSamplingSource_t{ &m_regionRepository, &m_samplingStorage };
	}
	TypedArray< Terrain3DRegion > getRegionsActive( const bool bCopy = false, const bool bDeep = false ) const;
	EA_FORCE_INLINE Dictionary	  getRegionsAll(
		   void ) const {
		return m_regionRepository.getRegions();
	}
	PackedInt32Array	getRegionMap( void ) const;
	PackedInt32Array	getLogicalRegionMap( void ) const;
	EA_FORCE_INLINE int getRegionMapSize(
		void ) const {
		return T3D_REGION_MAP_SIZE;
	}
	static int getRegionMapIndex( const Vector2i &rRegionLoc );

	void doForRegions( const Rect2i &rArea, const Callable &rCallback );

	Vector2i getRegionLocation( const Vector3 &rGlobalPosition ) const;
	int		 getRegionId( const Vector2i &rRegionLoc ) const;
	int		 getRegionIdp( const Vector3 &rGlobalPosition ) const;

	EA_FORCE_INLINE bool hasRegion(
		const Vector2i &rRegionLoc ) const {
		return getRegionId( rRegionLoc ) != -1;
	}
	EA_FORCE_INLINE bool hasRegionp(
		const Vector3 &rGlobalPosition ) const {
		return getRegionIdp( rGlobalPosition ) != -1;
	}
	Ref< Terrain3DRegion > getRegion( const Vector2i &rRegionLoc ) const override;
	Terrain3DRegion		  *getRegionPtr( const Vector2i &rRegionLoc ) const;
	template< typename T > // Catch invalid types. See note below in implementation.
	Terrain3DRegion		  *getRegionPtr( const T &rRegionLoc ) const = delete;
	Ref< Terrain3DRegion > getRegionp( const Vector3 &rGlobalPosition ) const;

	void setRegionModified( const Vector2i &rRegionLoc, const bool bModified = true );
	bool isRegionModified( const Vector2i &rRegionLoc ) const;
	void setRegionDeleted( const Vector2i &rRegionLoc, const bool bDeleted = true );
	bool isRegionDeleted( const Vector2i &rRegionLoc ) const;

	Ref< Terrain3DRegion > addRegionBlankp( const Vector3 &rGlobalPosition, const bool bUpdate = true );
	Ref< Terrain3DRegion > addRegionBlank( const Vector2i &rRegionLoc, const bool bUpdate = true );
	Error				   addRegion( const Ref< Terrain3DRegion > &rRegion, const bool bUpdate = true );
	// Applies a complete or partial snapshot through an authoritative region
	// replacement transaction. This is the safe script-facing alternative to
	// mutating Terrain3DRegion::setData() on an attached resource.
	Error replaceRegionData( const Vector2i &rRegionLoc, const Dictionary &rData );
	void  removeRegionp( const Vector3 &rGlobalPosition, const bool bUpdate = true );
	void  removeRegionl( const Vector2i &rRegionLoc, const bool bUpdate = true );
	void  removeRegion( const Ref< Terrain3DRegion > &rRegion, const bool bUpdate = true );

	// File I/O
	void  saveDirectory( const String &rDir );
	void  saveRegion( const Vector2i &rRegionLoc, const String &rDir, const bool b16Bit = false );
	Error loadDirectory( const String &rDir );
	void  loadRegion( const Vector2i &rRegionLoc, const String &rDir, const bool bUpdate = true );

	// Maps
	TypedArray< Image > getHeightMaps( void ) const;
	TypedArray< Image > getControlMaps( void ) const;
	TypedArray< Image > getColorMaps( void ) const;
	TypedArray< Image > getMaps( const t3dMapType_t mapType ) const;
	void				updateMaps( const t3dMapType_t mapType = REGION_TYPE_MAX,
									const bool		   bAllRegions = true,
									const bool		   bGenerateMipmaps = false );
	void				updateMapRegions( const t3dMapType_t										mapType,
										  const eastl::vector< Vector2i, Terrain3DEastlAllocator > &rRegionLocations );
	Dictionary getMapEditPerformanceStats( void ) const;
	void				commitMapEdits( const t3dMapType_t										  mapType,
										const eastl::vector< Vector2i, Terrain3DEastlAllocator > &rRegionLocations,
										const AABB												 &rEditedArea,
										const uint32_t											  nFlags );
	// Set bUpdateGeometryDependents=false only for material-only control edits.
	void
	commitMapEdit( const t3dMapType_t mapType,
				   const Vector2i &rRegionLocation,
				   const Rect2i &rDirtyRect = Rect2i(),
				   const bool bUpdateGeometryDependents = true );
	Terrain3DCompute				 *getComputeBackend( void );
	EA_FORCE_INLINE Terrain3DCompute *getComputeBackendIfCreated(
		void ) const {
		return m_pCompute;
	}
	void configureResidentPool( int nCapacity = 64 );
	void configureGpuRegionCache( bool	   bEnabled,
								  uint64_t nBudgetBytes = 16u * 1024u * 1024u,
								  int	   nUploadLayers = 4,
								  int	   nUploadBudgetMb = 2,
								  int	   nSparePages = T3D_GPU_CACHE_DEFAULT_SPARE_LAYERS,
								  int	   nRetireFrames = T3D_GPU_CACHE_DEFAULT_RETIRE_FRAMES );
	void configureCpuRegionCache( bool bEnabled, int nBudgetMb = 16, int nLoadLayers = 4, int nLoadBudgetMb = 2 );
	void configureInstanceRegionCache( bool bEnabled, int nBudgetMb = 16, int nLoadPages = 4, int nLoadBudgetMb = 2 );
	void setRuntimePageBudgetOverrides( int nGpuUploadLayers, int nCpuLoadLayers, int nInstanceLoadPages );
	void clearRuntimePageBudgetOverrides( void );
	int	 getEffectiveGpuRegionUploadBudgetLayers( void ) const;
	int	 getEffectiveCpuRegionLoadBudgetLayers( void ) const;
	int	 getEffectiveInstanceRegionLoadBudgetPages( void ) const;
	EA_FORCE_INLINE uint32_t getGpuRegionStallFlagsLastTick(
		void ) const {
		return m_residency.gpuRegionCache().getStallFlagsLastTick();
	}
	EA_FORCE_INLINE uint32_t getCpuRegionStallFlagsLastTick(
		void ) const {
		return m_residency.cpuRegionCache().getStallFlagsLastTick();
	}
	EA_FORCE_INLINE uint32_t getInstanceRegionStallFlagsLastTick(
		void ) const {
		return m_residency.instanceRegionCache().getStallFlagsLastTick();
	}
	bool					 ensureCpuRegionResident( const Vector2i &vLocation ) override;
	Ref< Terrain3DAssets >	 getErosionAssets( void ) const override;
	Ref< Terrain3DMaterial > getErosionMaterial( void ) const override;
	// Explicit demand loading for cold/editor callers. Sampling getters only
	// read currently published planes and return their missing-data sentinel.
	bool prepareSamplingArea( const Vector3 &rWorldPosition, int nRadiusTexels = 1 );
	bool tryPrepareSamplingArea( const Vector3 &rWorldPosition, int nRadiusTexels );

	EA_FORCE_INLINE uint64_t getCookedWorldMaterialSignatureInternal(
		void ) const {
		return m_runtimeArtifacts.worldManifest().isLoaded() ? m_runtimeArtifacts.worldManifest().getMaterialSignature()
															 : 0u;
	}

	bool			  ensureCookedNodeDescriptors( const Vector2i &vLocation );
	int				  getStreamedNodeCapacityHint( const Vector2i &vLocation ) const;
	bool			  getCookedSceneDependencySpan( const Vector2i	&vLocation,
													const uint16_t *&pDependencies,
													uint16_t		&nDependencyCount ) const;
	const String	 &getCookedSceneDependencyPath( uint16_t nDependency ) const;
	PackedStringArray getCookedSceneDependencies( const Vector2i &vLocation ) const;
	bool			  takeCookedCollisionTile( const Vector2i  &vLocation,
											   PackedByteArray &rHeight,
											   PackedByteArray &rBounds,
											   PackedByteArray &rHoleMask );
	bool			  takeCookedNavigationTile( const Vector2i &vLocation, PackedByteArray &rNavigation );
	bool			  takeCookedTerrainShadowMask( const Vector2i &vLocation, Ref< Image > &rShadowMask );
	bool			  loadCookedCollisionTileNow( const Vector2i  &vLocation,
												  PackedByteArray &rHeight,
												  PackedByteArray &rBounds,
												  PackedByteArray &rHoleMask ) const;
	bool			  loadCookedNavigationTileNow( const Vector2i  &vLocation,
												   PackedByteArray &rNavigation,
												   PackedByteArray *pCollisionHeight = NULL,
												   PackedByteArray *pMinMaxHeight = NULL ) const;
	bool			  loadCookedLightingReceiversNow( const Vector2i &vLocation, PackedByteArray &rReceivers ) const;
	void			  requestCookedCollisionTile( const Vector2i &vLocation, uint8_t nPriority = 0u );
	void			  requestCookedNavigationTile( const Vector2i &vLocation, uint8_t nPriority = 0u );
	void			  requestCookedTerrainShadowMask( const Vector2i &vLocation, uint8_t nPriority = 0u );
	void			  updateCpuRegionResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void			  serviceColdInstanceResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void			  updateInstanceRegionResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void			  updateGpuRegionResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void			  updateRegionResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void			  setGpuAuthoritativePages(
					 bool enabled ) {
		m_residency.regionResidency().setGpuAuthoritativePages( enabled );
	}
	bool getGpuAuthoritativePages() const {
		return m_residency.regionResidency().gpuAuthoritativePages();
	}
	Dictionary getSingleCopyBaselineStats() const {
		return m_residency.regionResidency().getSingleCopyBaselineStats();
	}
	void				 resetRuntimeStreamingCaches( void );
	void				 appendRuntimeProfileSample( t3dRuntimeProfileSample_t &rSample ) const;
	EA_FORCE_INLINE bool isGpuRegionCacheEnabled(
		void ) const {
		return m_residency.gpuRegionCache().isEnabled();
	}
    EA_FORCE_INLINE bool applyGpuRegionGrantWithoutReconfigure( uint64_t nBudgetBytes ) {
        return m_residency.gpuRegionCache().applyBudgetGrantWithoutReconfigure( nBudgetBytes );
    }
	EA_FORCE_INLINE int getGpuRegionCacheCapacity(
		void ) const {
		return m_residency.gpuRegionCache().getCapacity();
	}
	EA_FORCE_INLINE int getAuthoringGpuRegionBudgetMb(
		void ) const {
		return m_residency.gpuRegionCache().getAuthoringBudgetMb();
	}
	bool				isAuthoringGpuFullResidencyPossible( void ) const;
	bool				isAuthoringGpuReadbackSafe( void ) const;
	EA_FORCE_INLINE int getGpuRegionRequiredRadius(
		void ) const {
		return m_residency.gpuRegionCache().getRequiredRadius();
	}
	int					   getGpuRegionRequiredCorePages( void ) const;
	int					   calculateGpuRegionRequiredCorePagesForLods( int nDetailLods ) const;
	int					   calculateGpuRegionCapacityForLods( int nDetailLods ) const;
	Dictionary			   getGpuRegionCacheStats( void ) const;
	Dictionary			   getGpuRegionCacheStatsFast( void ) const;
	Dictionary			   getStartupLoadStats( void ) const;
	size_t				   getGpuRegionCacheReservedBytes( void ) const;
	size_t				   getGpuRegionCacheMinimumBytes( void ) const;
	size_t				   getGpuRegionCacheHardCapBytes( void ) const;
	size_t				   getCpuRegionCacheReservedBytes( void ) const;
	size_t				   getCpuRegionCacheMinimumBytes( void ) const;
	size_t				   getCpuRegionCacheHardCapBytes( void ) const;
	EA_FORCE_INLINE size_t getInstanceRegionCacheReservedBytes(
		void ) const {
		return m_residency.instanceRegionCache().getReservedBytes();
	}
	void	   setRuntimePagePreparedByteBudget( uint64_t nBytes );
	size_t	   getMemoryRuntimePagePreparedBytes( void ) const;
	size_t	   getMemoryNonPageTransientStagingBytes( void ) const;
	size_t	   getMemoryTransientStagingBytes( void ) const;
	size_t	   getMemoryFixedRepositoryBytes( void ) const;
	size_t	   getMemoryAllocatorContainedRepositoryBytes( void ) const;
	uint64_t   getMemoryComputeLocalGpuBytes( void ) const;
	Dictionary getComputeLocalGpuTelemetry( void ) const;
	Dictionary getRuntimeResidencyGrid( int nRadius = 6 ) const;
	void	   invalidateGpuFarSurface( void );
	void	   invalidateFarVtSourceLayout( void );
	Dictionary inspectGpuRegionPages( void );
#if defined( T3D_EDITOR_NATIVE )
	Dictionary cookGpuRegionPages( void );
	bool	   publishRuntimeVirtualTexturesCold( String				 &rOutputPath,
												  uint32_t				 &rGeneration,
												  uint32_t				 &rMaterialCount,
												  String				 &rError,
												  PackedByteArray		 *pStagedPackage = NULL,
												  Terrain3DMemoryAccount *pAuthoringAccount = NULL );
#endif
	Dictionary			 getCpuRegionCacheStats( void ) const;
	Dictionary			 getCpuRegionCacheStatsFast( void ) const;
	Dictionary			 getInstanceRegionCacheStats( void ) const;
	Dictionary			 getInstanceRegionCacheStatsFast( void ) const;
	EA_FORCE_INLINE bool isInstanceRegionCacheEnabled(
		void ) const {
		return m_residency.instanceRegionCache().isEnabled();
	}
	EA_FORCE_INLINE bool isInstanceRegionResident(
		const Vector2i &vLocation ) const {
		return m_residency.instanceRegionCache().isResident( vLocation );
	}
	EA_FORCE_INLINE bool ensureInstanceRegionResident(
		const Vector2i &vLocation ) {
		return ensureInstanceRegionResidentInternal( vLocation, true );
	}
	bool				 evictInstanceRegion( const Vector2i &vLocation );
	EA_FORCE_INLINE bool isCpuRegionCacheEnabled(
		void ) const {
		return m_residency.cpuRegionCache().isEnabled();
	}
	EA_FORCE_INLINE int getCpuRegionCacheCapacity(
		void ) const {
		return m_residency.cpuRegionCache().getCapacity();
	}
	void regenerateDerivedMaps( const eastl::vector< Vector2i, Terrain3DEastlAllocator > &rRegionLocations,
								const AABB												 &rEditedArea = AABB() );
	void publishDerivedMapUpdates( const eastl::vector< t3dDerivedMapUpdate_t, Terrain3DEastlAllocator > &rUpdates );
#if defined( T3D_EDITOR_NATIVE )
	Ref< Terrain3DErosionJob > createErosionJob( const Ref< Terrain3DErosionSettings > &rSettings );
#endif
	EA_FORCE_INLINE RID getHeightMapsRid(
		void ) const {
		return m_residency.gpuRegionCache().isEnabled() ? m_residency.gpuRegionCache().getHeightMapsRid()
														: m_generatedHeightMaps.getRid();
	}
	EA_FORCE_INLINE RID getDerivedMapsRid(
		void ) const {
		return m_residency.gpuRegionCache().isEnabled() ? m_residency.gpuRegionCache().getDerivedMapsRid()
														: m_generatedDerivedMaps.getRid();
	}
	EA_FORCE_INLINE RID getControlMapsRid(
		void ) const {
		return m_residency.gpuRegionCache().isEnabled() ? m_residency.gpuRegionCache().getControlMapsRid()
														: m_generatedControlMaps.getRid();
	}
	EA_FORCE_INLINE RID getColorMapsRid(
		void ) const {
		return m_residency.gpuRegionCache().isEnabled() ? m_residency.gpuRegionCache().getColorMapsRid()
														: m_generatedColorMaps.getRid();
	}
	EA_FORCE_INLINE RID getMiddleCompositeMapsRid(
		void ) const {
		return m_residency.gpuRegionCache().isEnabled() ? m_residency.gpuRegionCache().getMiddleCompositeMapsRid()
														: m_generatedMiddleCompositeMaps.getRid();
	}
	bool				refreshTerrainShadowMasksInternal( void );
	EA_FORCE_INLINE RID getTerrainShadowMasksRid(
		void ) const {
		return m_residency.gpuRegionCache().isEnabled() ? m_residency.gpuRegionCache().getTerrainShadowMasksRid()
														: m_generatedTerrainShadowMasks.getRid();
	}
	EA_FORCE_INLINE RID getMiddleHeightMapsRid(
		void ) const {
		return m_residency.middleHeightRegionCache().getHeightMapsRid();
	}
	EA_FORCE_INLINE RID getMiddleHeightRegionMapRid(
		void ) const {
		return m_residency.middleHeightRegionCache().getRegionMapRid();
	}
	EA_FORCE_INLINE int getMiddleHeightResidentCount(
		void ) const {
		return m_residency.middleHeightRegionCache().getResidentCount();
	}
	EA_FORCE_INLINE int getMiddleHeightMissingRequiredLastTick(
		void ) const {
		return m_residency.middleHeightRegionCache().getMissingRequiredLastTick();
	}
	EA_FORCE_INLINE bool isMiddleHeightRegionResident(
		const Vector2i &vLocation ) const {
		return m_residency.middleHeightRegionCache().isResident( vLocation );
	}
	EA_FORCE_INLINE bool isGpuSurfaceRegionResident(
		const Vector2i &vLocation ) const {
		return m_residency.gpuRegionCache().isResident( vLocation );
	}
	Dictionary			getMiddleHeightCacheStats( void ) const;
	size_t				getMiddleHeightCacheReservedBytes( void ) const;
	int					fillGpuMaterialPriorityIds( uint8_t *pIds, int nCapacity ) const;
	EA_FORCE_INLINE bool areControlMapsImplicit(
		void ) const {
		return m_bControlMapsImplicit;
	}
	EA_FORCE_INLINE bool areColorMapsImplicit(
		void ) const {
		return m_bColorMapsImplicit;
	}
	bool isGpuRegionColorChannelEnabled( void ) const;
	bool isGpuRegionDerivedChannelEnabled( void ) const;

	Dictionary getSparseRasterStats( void ) const;
	// Resolve clean cooked source rasters for the GPU cache without retaining a
	// second long-lived Godot Image copy. Explicit authoring images win.
	Ref< Image > getCookedSparseRegionChannel( const Terrain3DRegion *pRegion, t3dMapType_t eChannel ) const;
	// Visual-only raster LOD fallback. Never supplies collision/authoring height.
	Ref< Image > getCookedSparseVisualFallback( const Terrain3DRegion *pRegion, t3dMapType_t eChannel ) const;
	void		 setPixel( const t3dMapType_t mapType, const Vector3 &rGlobalPosition, const Color &rPixel );
	Color		 getPixel( const t3dMapType_t mapType, const Vector3 &rGlobalPosition ) const;
	void		 setHeight( const Vector3 &rGlobalPosition, const real_t fHeight );
	real_t		 getHeight( const Vector3 &rGlobalPosition ) const;
	bool		 intersectHeightfieldRay( const Vector3 &rOrigin,
										  const Vector3 &rDirection,
										  const real_t	 fMaxDistance,
										  Vector3		&rHitPosition ) const;
	// Bulk sampling resolves and maps each touched region once for the batch.
	// Missing regions and holes return NAN heights/normals and -1 controls.
	PackedFloat32Array getHeights( const PackedVector3Array &rGlobalPositions ) const;
	PackedVector3Array getNormals( const PackedVector3Array &rGlobalPositions ) const;
	PackedInt32Array   getControls( const PackedVector3Array &rGlobalPositions ) const;
	void			   setColor( const Vector3 &rGlobalPosition, const Color &rColor );
	Color			   getColor( const Vector3 &rGlobalPosition ) const;
	void			   setControl( const Vector3 &rGlobalPosition, const uint32_t nControl );
	uint32_t		   getControl( const Vector3 &rGlobalPosition ) const;
	void			   setRoughness( const Vector3 &rGlobalPosition, const real_t fRoughness );
	real_t			   getRoughness( const Vector3 &rGlobalPosition ) const;

	// Control Map
	void	 setControlBaseId( const Vector3 &rGlobalPosition, const uint8_t nBase );
	uint32_t getControlBaseId( const Vector3 &rGlobalPosition ) const;
	void	 setControlOverlayId( const Vector3 &rGlobalPosition, const uint8_t nOverlay );
	uint32_t getControlOverlayId( const Vector3 &rGlobalPosition ) const;
	void	 setControlBlend( const Vector3 &rGlobalPosition, const real_t fBlend );
	real_t	 getControlBlend( const Vector3 &rGlobalPosition ) const;
	void	 setControlAngle( const Vector3 &rGlobalPosition, const real_t fAngle );
	real_t	 getControlAngle( const Vector3 &rGlobalPosition ) const;
	void	 setControlScale( const Vector3 &rGlobalPosition, const real_t fScale );
	real_t	 getControlScale( const Vector3 &rGlobalPosition ) const;
	void	 setControlHole( const Vector3 &rGlobalPosition, const bool bHole );
	bool	 getControlHole( const Vector3 &rGlobalPosition ) const;
	void	 setControlNavigation( const Vector3 &rGlobalPosition, const bool bNavigation );
	bool	 getControlNavigation( const Vector3 &rGlobalPosition ) const;
	void	 setControlAuto( const Vector3 &rGlobalPosition, const bool bAuto );
	bool	 getControlAuto( const Vector3 &rGlobalPosition ) const;

	Vector3 getNormal( const Vector3 &rGlobalPosition ) const;
	bool
	isInSlope( const Vector3 &rGlobalPosition, const Vector2 &rSlopeRange, const Vector3 &rNormal = s_v3Zero ) const;
	Vector3 getTextureId( const Vector3 &rGlobalPosition ) const;
	Vector3 getMeshVertex( const int32_t nLod, const t3dHeightFilter_t filter, const Vector3 &rGlobalPosition ) const;

	void				 addEditedArea( const AABB &rArea );
	EA_FORCE_INLINE void clearEditedArea(
		void ) {
		m_mapEditService.clearEditedArea();
	}
	EA_FORCE_INLINE AABB getEditedArea(
		void ) const {
		return m_mapEditService.getEditedArea();
	}

	EA_FORCE_INLINE Vector2 getHeightRange(
		void ) const {
		return m_mapEditService.getHeightRange();
	}
	void updateMasterHeight( const real_t fHeight );
	void updateMasterHeights( const Vector2 &rLowHigh );
	void calcHeightRange( const bool bRecursive = false );

	void		 importImages( const TypedArray< Image > &rImages,
							   const Vector3			 &rGlobalPosition = s_v3Zero,
							   const real_t				  fOffset = 0.f,
							   const real_t				  fScale = 1.f );
	Error		 exportImage( const String &rFileName, const t3dMapType_t mapType = TYPE_HEIGHT ) const;
	Ref< Image > layeredToImage( const t3dMapType_t mapType ) const;

	// Utility
	void dump( const bool bVerbose = false ) const;

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dHeightFilter_t );

// Inline Region Functions

// Returns an index in the fixed logical region table.
// Repository operations additionally enforce the active map size, which is
// derived from the fixed 4096x4096 world resolution and current region size.
EA_FORCE_INLINE int Terrain3DData::getRegionMapIndex(
	const Vector2i &rRegionLoc ) {
	return Terrain3DRegionRepository::getRegionMapIndex( rRegionLoc );
}

// Returns a region location given a global position. No bounds checking nor data access.
EA_FORCE_INLINE Vector2i Terrain3DData::getRegionLocation(
	const Vector3 &rGlobalPosition ) const {
	return m_regionRepository.getRegionLocation( rGlobalPosition );
}

EA_FORCE_INLINE int Terrain3DData::getRegionId(
	const Vector2i &rRegionLoc ) const {
	return m_regionRepository.getRegionId( rRegionLoc );
}

EA_FORCE_INLINE int Terrain3DData::getRegionIdp(
	const Vector3 &rGlobalPosition ) const {
	return m_regionRepository.getRegionId( m_regionRepository.getRegionLocation( rGlobalPosition ) );
}

EA_FORCE_INLINE Ref< Terrain3DRegion > Terrain3DData::getRegion(
	const Vector2i &rRegionLoc ) const {
	return m_regionRepository.getRegion( rRegionLoc );
}

EA_FORCE_INLINE Terrain3DRegion *Terrain3DData::getRegionPtr(
	const Vector2i &rRegionLoc ) const {
	return m_regionRepository.getRegionPtr( rRegionLoc );
}

EA_FORCE_INLINE Ref< Terrain3DRegion > Terrain3DData::getRegionp(
	const Vector3 &rGlobalPosition ) const {
	return m_regionRepository.getRegion( m_regionRepository.getRegionLocation( rGlobalPosition ) );
}

// Inline Map Functions

EA_FORCE_INLINE void Terrain3DData::setHeight(
	const Vector3 &rGlobalPosition,
	const real_t   fHeight ) {
	setPixel( TYPE_HEIGHT, rGlobalPosition, Color( fHeight, 0.f, 0.f, 1.f ) );
}

EA_FORCE_INLINE void Terrain3DData::setControl(
	const Vector3 &rGlobalPosition,
	const uint32_t nControl ) {
	setPixel( TYPE_CONTROL, rGlobalPosition, Color( t3dAsFloat( nControl ), 0.f, 0.f, 1.f ) );
}

// Expects 0.0 to 1.0 range

// Expects angle in degrees

// returns angle in degrees

// Expects scale as a percentage modifier

EA_FORCE_INLINE real_t Terrain3DData::getRoughness(
	const Vector3 &rGlobalPosition ) const {
	return getPixel( TYPE_COLOR, rGlobalPosition ).a;
}

EA_FORCE_INLINE void Terrain3DData::updateMasterHeight(
	const real_t fHeight ) {
	m_mapEditService.updateMasterHeight( fHeight );
}

EA_FORCE_INLINE void Terrain3DData::updateMasterHeights(
	const Vector2 &rLowHigh ) {
	m_mapEditService.updateMasterHeights( rLowHigh );
}
#endif // TERRAIN3D_DATA_CLASS_H
