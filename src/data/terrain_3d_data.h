// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_DATA_CLASS_H
#define TERRAIN3D_DATA_CLASS_H

#include "Terrain3DConstants.h"

#include "assets/generated_texture.h"
#include "data/terrain_3d_sampling_storage.h"
#include "data/terrain_3d_map_edit_service.h"
#include "data/terrain_3d_region_persistence.h"
#include "rendering/terrain_3d_far_surface_atlas_cache.h"
#include "streaming/terrain_3d_cpu_region_cache.h"
#include "streaming/terrain_3d_gpu_region_cache.h"
#include "streaming/terrain_3d_instance_region_cache.h"
#include "streaming/terrain_3d_middle_height_region_cache.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_runtime_world_manifest.h"
#include "streaming/terrain_3d_runtime_profile.h"
#include "streaming/terrain_3d_runtime_page_store.h"
#include "streaming/terrain_3d_active_height_residency.h"
#include "streaming/terrain_3d_region_residency.h"

#include "eastl_config.h"
#include <EASTL/array.h>
#include <cstdint>
#include <EASTL/vector.h>

class T3DTerrain;
class T3DErosionJob;
class T3DErosionSettings;
class T3DCompute;

class T3DData : public Object {
	GDCLASS( T3DData, Object );
	CLASS_NAME();
	friend T3DTerrain;
	friend class T3DErosionJob;
	friend class T3DRegionResidency;

public:														 // Constants
	// Maximum logical grid. The active grid is TERRAIN3D_ACTIVE_WORLD_TEXELS / region_size.

	typedef enum t3dHeightFilter_e {
		HEIGHT_FILTER_NEAREST,
		HEIGHT_FILTER_MINIMUM
	} t3dHeightFilter_t;

	typedef enum t3dMapEditCommitFlag_e : uint32_t {
		MAP_EDIT_UPDATE_INSTANCES = 1u << 0,
		MAP_EDIT_UPDATE_COLLISION = 1u << 1,
		MAP_EDIT_SNAP = 1u << 2,
		MAP_EDIT_KEEP_EDITED = 1u << 3,
	} t3dMapEditCommitFlag_t;


private:
	T3DTerrain *m_pTerrain = nullptr;

	T3DRegionRepository m_regionRepository;
	T3DSamplingStorage m_samplingStorage;
	T3DMapEditService m_mapEditService;
	T3DRegionPersistence m_regionPersistence;
	T3DRuntimePageStore m_runtimePageStore;
	T3DRuntimeWorldManifest m_runtimeWorldManifest;
	T3DCpuRegionCache m_cpuRegionCache;
	T3DInstanceRegionCache m_instanceRegionCache;
	T3DMiddleHeightRegionCache m_middleHeightRegionCache;
	T3DGpuRegionCache m_gpuRegionCache;
	T3DFarSurfaceAtlasCache m_farSurfaceAtlasCache;
	T3DActiveHeightResidency m_activeHeightResidency;
	T3DRegionResidency m_regionResidency;
	bool m_bRuntimeActive = false;


	TypedArray<Image> m_heightMaps;
	TypedArray<Image> m_controlMaps;
	TypedArray<Image> m_colorMaps;
	TypedArray<Image> m_derivedMaps; // Runtime encoded normal XZ cache (RG8).

	// Shared implicit map payloads. Regions retain null control/color references until
	// a write materializes them, while GPU arrays still receive valid layers.
	Ref<Image> m_pDefaultHeightMap;
	Ref<Image> m_pDefaultControlMap;
	Ref<Image> m_pDefaultColorMap;
	Ref<Image> m_pDefaultDerivedMap;
	bool m_bControlMapsImplicit = true;
	bool m_bColorMapsImplicit = true;

	// Editing occurs on the Image arrays above, which are converted to Texture arrays
	// below for the shader.

	// Initial directory loading builds the CPU/GPU data once, before listeners are
	// initialized. Suppress redundant startup signals/snaps and defer spare layers.
	bool m_bStartupBulkLoad = false;
	bool m_bSkipSlotReserveOnce = false;

	// These contain the TextureArray RIDs from the RenderingServer. In bounded
	// residency mode they hold physical cache layers rather than repository slots.
	T3DGeneratedTexture m_generatedHeightMaps;
	T3DGeneratedTexture m_generatedControlMaps;
	T3DGeneratedTexture m_generatedColorMaps;
	T3DGeneratedTexture m_generatedDerivedMaps;
	T3DGeneratedTexture m_generatedMiddleCompositeMaps;
	// Optional compute backend. It is lazy so Compatibility/headless runtimes
	// retain the existing CPU path without constructing RenderingDevice state.
	T3DCompute *m_pCompute = nullptr;
	// Functions
	void clear( void );
	EA_FORCE_INLINE void invalidateSamplingCache( void ) { m_samplingStorage.invalidateAllSamplers(); }
	EA_FORCE_INLINE void publishRegionSamplingGeneration( const Vector2i &vLocation ) { m_samplingStorage.publishRegionGeneration( vLocation ); }
	void ensureDefaultMaps( void );
	bool setRegionSizeInternal( int nRegionSize );
	EA_FORCE_INLINE bool bindActiveStorageInternal( int nRegionSize, void *pBacking, size_t nBackingBytes ) { return m_samplingStorage.bindActive( nRegionSize, pBacking, nBackingBytes ); }
	EA_FORCE_INLINE bool isActiveStorageOwnerInternal( void ) const { return m_samplingStorage.isActiveOwner(); }
	EA_FORCE_INLINE void resetResidentSamplingPoolInternal( void ) { m_samplingStorage.resetResidentPool(); }
	Error rehydrateActiveStorage( void );
	bool syncActiveRegion( const Vector2i &vLocation, const T3DRegion *pRegion );
	Ref<Image> getRegionMapOrDefault( const T3DRegion *pRegion, t3dMapType_t mapType ) const;
	Ref<Image> makeGpuColorImage( const Ref<Image> &rImage ) const;
	Ref<Image> makeGpuDerivedImage( const Ref<Image> &rImage ) const;
	Ref<Image> makeGpuRenderControlImage( const Vector2i &vLocation ) const;
	Ref<Image> makeMiddleCompositeImage( const Vector2i &rLocation,
		const Ref<Image> &rControl, const Ref<Image> &rColor ) const;
	T3DRuntimePageConfig getRuntimePageConfig( void ) const;
	static bool isRuntimePageWanted( void *pContext, const Vector2i &rLocation );
	uint64_t computeGpuRuntimePageSourceSignature( const T3DRegion *pRegion ) const;
	bool readGpuRuntimePageSourceSignature( const String &rPagePath, uint64_t &nSignature ) const;
	String getGpuRuntimePagePath( const T3DRegion *pRegion ) const;
	bool collectGpuRegionPageSources(
		String &rDataDirectory, PackedStringArray &rSourceFiles,
		eastl::vector<Vector2i> &rLocations, PackedStringArray &rErrors
	) const;
	void pollGpuRuntimePagePrepare( bool bWait );
	void requestGpuRuntimePage( const Vector2i &vLocation, uint16_t nConsumers, uint8_t nPriority );
	void serviceGpuRuntimePagePrepare( const T3DResidencyFrameState &rFrame );
	bool isGpuRuntimePagePreparePending( const Vector2i &vLocation ) const;
	bool takePreparedRuntimeHeight(
		const Vector2i &vLocation, uint16_t nConsumer, Ref<Image> &rHeight, bool bMiddle,
		PackedByteArray *pNativeHeightBytes = nullptr
	);
	bool takePreparedRuntimeInstance( const Vector2i &vLocation, T3DInstanceStore &rStore );
	bool takePreparedGpuRuntimePage(
		const Vector2i &vLocation, Ref<Image> &rHeight,
		Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived, Ref<Image> &rMiddleComposite,
		uint32_t &rMaterialMask, bool &bAttempted, uint64_t *pRequestedUsec = nullptr
	);
	bool writeGpuRuntimePage( T3DRegion *pRegion, String *pError = nullptr );
	bool hasMaterializedMap( t3dMapType_t mapType ) const;
	bool refreshImplicitMapState( void );
	int getRegionSlotReserve( void ) const;
	bool canUpdateRegionSlotsIncrementally( void ) const;
	void syncCacheDefaultMaps( void );
	T3DFarSurfaceAtlasConfig getFarSurfaceAtlasConfig( void ) const;
	T3DMiddleHeightRegionCacheConfig getMiddleHeightCacheConfig( void ) const;
	void applyInstanceRegionCacheChanges( const T3DInstanceRegionCacheChanges &rChanges );
	void resetGpuRegionCache( bool bClearTextures );
	void armPendingGpuRegionRetirements( void );
	void invalidateGpuRegionLocation( const Vector2i &vLocation );
	bool republishGpuRegionLocation( const Vector2i &vLocation );
	void queueGpuRegionRepublish( const Vector2i &vLocation );
	bool isFarAtlasRequired( void ) const;
	bool writeCookedGpuFarAtlases( const String &rDirectory = String() );
	void markAllGpuFarRegionsDirty( t3dMapType_t mapType );
	void markGpuFarRegionDirty( const Vector2i &vLocation, t3dMapType_t mapType );
	void resetMiddleHeightCache( void );
	bool configureMiddleHeightCache( void );
	void updateMiddleHeightResidencyInternal( const T3DResidencyFrameState &rFrame, bool bForce );
	void resetCpuRegionCache( bool bReleaseCleanMaps );
	void invalidateCpuRegionLocation( const Vector2i &vLocation );
	bool requestCpuRegionSourceAsync( const Vector2i &vLocation );
	bool ensureCpuRegionResidentInternal( const Vector2i &vLocation, bool bDemandLoad );
	bool ensureCpuSampleArea( const Vector3 &rWorldPosition, int nRadiusTexels );
	void resetInstanceRegionCache( bool bRestoreAll );
	void invalidateInstanceRegionLocation( const Vector2i &vLocation );
	bool ensureInstanceRegionResidentInternal( const Vector2i &vLocation, bool bDemandLoad );
	void updateMapsBounded( const t3dMapType_t mapType, const bool bAllRegions );
	void emitGpuRegionMapChanged( void );
	void emitRegionLayoutChanged( const bool bTextureLayersChanged );
	TypedArray<Image> getActiveMaps( const t3dMapType_t mapType ) const;
	void copyPasteDfr(
		const T3DRegion *pSrcRegion, const Rect2i &rSrcRect, const Rect2i &rDstRect,
		T3DRegion *pDstRegion
	);
	Error addRegionInternal( const Ref<T3DRegion> &rRegion, const bool bUpdate, const bool bSanitize );
	void processRegionSaveResult( const T3DRegionSaveResult &rResult );
	static bool ensurePersistenceRegionResident( void *pContext, const Vector2i &rLocation );
	T3DResidencyFrameState makeResidencyFrameState( const Vector3 &rWorldPosition ) const;
	void updateCpuRegionResidencyInternal( const T3DResidencyFrameState &rFrame, bool bForce );
	void updateInstanceRegionResidencyInternal( const T3DResidencyFrameState &rFrame, bool bForce );
	void updateGpuRegionResidencyInternal( const T3DResidencyFrameState &rFrame, bool bForce );

public:
	T3DData( void );
	Error initialize( T3DTerrain *pTerrain );
	void deactivateRuntimeStorage( T3DTerrain *pTerrain );
	~T3DData( void );
	EA_FORCE_INLINE bool isInitializedFor( const T3DTerrain *pTerrain ) const { return m_bRuntimeActive && m_pTerrain == pTerrain; }
	EA_FORCE_INLINE bool isRuntimeReady( void ) const { return m_bRuntimeActive && m_samplingStorage.isActiveOwner() && m_regionRepository.getRegionMapSize() > 0; }
	EA_FORCE_INLINE bool isRepositoryLoaded( void ) const { return m_regionRepository.isLoaded(); }
	EA_FORCE_INLINE uint32_t getActiveStorageGeneration( void ) const { return isRuntimeReady() ? m_samplingStorage.getActiveGeneration() : 0u; }
	Dictionary getRuntimeState( void ) const;

	// Regions

	EA_FORCE_INLINE int getRegionCount( void ) const { return m_regionRepository.getActiveRegionCount(); }
	void setRegionLocations( const TypedArray<Vector2i> &vLocations );
	TypedArray<Vector2i> getRegionLocations( void ) const;
	// Internal layer-ordered slots, including V2I_MAX spare entries. Custom
	// shaders require this exact ordering to match the texture arrays.
	TypedArray<Vector2i> getRegionSlots( void ) const;
	TypedArray<Vector2i> getRenderRegionSlots( void ) const;
	EA_FORCE_INLINE const Vector2i *getRegionSlotsInternal( void ) const { return m_regionRepository.getSlotsData(); }
	EA_FORCE_INLINE int getRegionSlotCountInternal( void ) const { return m_regionRepository.getSlotCount(); }
	EA_FORCE_INLINE T3DRegion *const *getRegionPtrMapInternal( void ) const { return m_regionRepository.getRegionPtrMapData(); }
	EA_FORCE_INLINE int getRegionSizeInternal( void ) const { return m_regionRepository.getRegionSize(); }
	EA_FORCE_INLINE int getRegionMapSizeInternal( void ) const { return m_regionRepository.getRegionMapSize(); }
	EA_FORCE_INLINE bool matchesRegionLayoutInternal( const int nRegionSize ) const { return m_regionRepository.getRegionSize() == nRegionSize && m_regionRepository.getRegionSizeVector() == V2I( nRegionSize ); }
	EA_FORCE_INLINE T3DRegionRepositoryView getRegionRepositoryViewInternal( void ) const { return m_regionRepository.getView(); }
	EA_FORCE_INLINE uint64_t getRegionTopologyGenerationInternal( void ) const { return m_regionRepository.getTopologyGeneration(); }
	EA_FORCE_INLINE void setVertexSpacingInternal( const real_t fVertexSpacing ) { m_regionRepository.setVertexSpacing( fVertexSpacing ); }
	EA_FORCE_INLINE bool isRegionLocationValidInternal( const Vector2i &vLocation ) const { return m_regionRepository.isLocationValid( vLocation ); }
	EA_FORCE_INLINE real_t getVertexSpacingInternal( void ) const { return m_regionRepository.getVertexSpacing(); }
	T3DSamplingSource getSamplingSourceInternal( void ) const {
		return T3DSamplingSource{ &m_regionRepository, &m_samplingStorage };
	}
	TypedArray<T3DRegion> getRegionsActive( const bool bCopy = false, const bool bDeep = false ) const;
	EA_FORCE_INLINE Dictionary getRegionsAll( void ) const { return m_regionRepository.getRegions(); }
	PackedInt32Array getRegionMap( void ) const;
	PackedInt32Array getLogicalRegionMap( void ) const;
	EA_FORCE_INLINE int getRegionMapSize( void ) const { return m_regionRepository.getRegionMapSize(); }
	static int getRegionMapIndex( const Vector2i &rRegionLoc );

	void doForRegions( const Rect2i &rArea, const Callable &rCallback );
	void changeRegionSize( int region_size );

	Vector2i getRegionLocation( const Vector3 &rGlobalPosition ) const;
	int getRegionId( const Vector2i &rRegionLoc ) const;
	int getRegionIdp( const Vector3 &rGlobalPosition ) const;

	EA_FORCE_INLINE bool hasRegion( const Vector2i &rRegionLoc ) const { return getRegionId( rRegionLoc ) != -1; }
	EA_FORCE_INLINE bool hasRegionp( const Vector3 &rGlobalPosition ) const { return getRegionIdp( rGlobalPosition ) != -1; }
	Ref<T3DRegion> getRegion( const Vector2i &rRegionLoc ) const;
	T3DRegion *getRegionPtr( const Vector2i &rRegionLoc ) const;
	template <typename T> // Catch invalid types. See note below in implementation.
	T3DRegion *getRegionPtr( const T &rRegionLoc ) const = delete;
	Ref<T3DRegion> getRegionp( const Vector3 &rGlobalPosition ) const;

	void setRegionModified( const Vector2i &rRegionLoc, const bool bModified = true );
	bool isRegionModified( const Vector2i &rRegionLoc ) const;
	void setRegionDeleted( const Vector2i &rRegionLoc, const bool bDeleted = true );
	bool isRegionDeleted( const Vector2i &rRegionLoc ) const;

	Ref<T3DRegion> addRegionBlankp( const Vector3 &rGlobalPosition, const bool bUpdate = true );
	Ref<T3DRegion> addRegionBlank( const Vector2i &rRegionLoc, const bool bUpdate = true );
	Error addRegion( const Ref<T3DRegion> &rRegion, const bool bUpdate = true );
	// Applies a complete or partial snapshot through an authoritative region
	// replacement transaction. This is the safe script-facing alternative to
	// mutating T3DRegion::setData() on an attached resource.
	Error replaceRegionData( const Vector2i &rRegionLoc, const Dictionary &rData );
	void removeRegionp( const Vector3 &rGlobalPosition, const bool bUpdate = true );
	void removeRegionl( const Vector2i &rRegionLoc, const bool bUpdate = true );
	void removeRegion( const Ref<T3DRegion> &rRegion, const bool bUpdate = true );

	// File I/O
	void saveDirectory( const String &rDir );
	void saveRegion( const Vector2i &rRegionLoc, const String &rDir, const bool b16Bit = false );
	Error loadDirectory( const String &rDir );
	void loadRegion( const Vector2i &rRegionLoc, const String &rDir, const bool bUpdate = true );

	// Maps
	TypedArray<Image> getHeightMaps( void ) const;
	TypedArray<Image> getControlMaps( void ) const;
	TypedArray<Image> getColorMaps( void ) const;
	TypedArray<Image> getMaps( const t3dMapType_t mapType ) const;
	void updateMaps(
		const t3dMapType_t mapType = T3DRegion::TYPE_MAX, const bool bAllRegions = true,
		const bool bGenerateMipmaps = false
	);
	void updateMapRegions( const t3dMapType_t mapType, const eastl::vector<Vector2i> &rRegionLocations );
	void commitMapEdits(
		const t3dMapType_t mapType, const eastl::vector<Vector2i> &rRegionLocations,
		const AABB &rEditedArea, const uint32_t nFlags
	);
	void commitMapEdit(
		const t3dMapType_t mapType, const Vector2i &rRegionLocation,
		const Rect2i &rDirtyRect = Rect2i()
	);
	T3DCompute *getComputeBackend( void );
	EA_FORCE_INLINE T3DCompute *getComputeBackendIfCreated( void ) const { return m_pCompute; }
	void configureResidentPool( int nCapacity = 64 );
	void configureGpuRegionCache(
		bool bEnabled, int nBudgetMb = 16, int nUploadLayers = 4,
		int nUploadBudgetMb = 2, int nSparePages = TERRAIN3D_GPU_CACHE_DEFAULT_SPARE_LAYERS,
		int nRetireFrames = TERRAIN3D_GPU_CACHE_DEFAULT_RETIRE_FRAMES
	);
	void configureCpuRegionCache(
		bool bEnabled, int nBudgetMb = 16, int nLoadLayers = 4,
		int nLoadBudgetMb = 2
	);
	void configureInstanceRegionCache(
		bool bEnabled, int nBudgetMb = 16, int nLoadPages = 4,
		int nLoadBudgetMb = 2
	);
	void setRuntimePageBudgetOverrides(
		int nGpuUploadLayers, int nCpuLoadLayers,
		int nInstanceLoadPages
	);
	void clearRuntimePageBudgetOverrides( void );
	int getEffectiveGpuRegionUploadBudgetLayers( void ) const;
	int getEffectiveCpuRegionLoadBudgetLayers( void ) const;
	int getEffectiveInstanceRegionLoadBudgetPages( void ) const;
	EA_FORCE_INLINE uint32_t getGpuRegionStallFlagsLastTick( void ) const { return m_gpuRegionCache.getStallFlagsLastTick(); }
	EA_FORCE_INLINE uint32_t getCpuRegionStallFlagsLastTick( void ) const { return m_cpuRegionCache.getStallFlagsLastTick(); }
	EA_FORCE_INLINE uint32_t getInstanceRegionStallFlagsLastTick( void ) const { return m_instanceRegionCache.getStallFlagsLastTick(); }
	bool ensureCpuRegionResident( const Vector2i &vLocation );

	EA_FORCE_INLINE uint64_t getCookedWorldMaterialSignatureInternal( void ) const { return m_runtimeWorldManifest.isLoaded() ? m_runtimeWorldManifest.getMaterialSignature() : 0u; }

	bool ensureCookedNodeDescriptors( const Vector2i &vLocation );
	PackedStringArray getCookedSceneDependencies( const Vector2i &vLocation ) const;
	bool takeCookedCollisionTile( const Vector2i &vLocation, PackedByteArray &rHeight, PackedByteArray &rBounds, PackedByteArray &rHoleMask );
	bool takeCookedNavigationTile( const Vector2i &vLocation, PackedByteArray &rNavigation );
	bool takeCookedTerrainShadowMask( const Vector2i &vLocation, Ref<Image> &rShadowMask );
	bool loadCookedCollisionTileNow( const Vector2i &vLocation, PackedByteArray &rHeight,
		PackedByteArray &rBounds, PackedByteArray &rHoleMask ) const;
	bool loadCookedNavigationTileNow( const Vector2i &vLocation, PackedByteArray &rNavigation,
		PackedByteArray *pCollisionHeight = nullptr, PackedByteArray *pMinMaxHeight = nullptr ) const;
	bool loadCookedLightingReceiversNow( const Vector2i &vLocation, PackedByteArray &rReceivers ) const;
	void requestCookedCollisionTile( const Vector2i &vLocation, uint8_t nPriority = 0u );
	void requestCookedNavigationTile( const Vector2i &vLocation, uint8_t nPriority = 0u );
	void requestCookedTerrainShadowMask( const Vector2i &vLocation, uint8_t nPriority = 0u );
	void updateCpuRegionResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void updateInstanceRegionResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void updateGpuRegionResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void updateRegionResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void resetRuntimeStreamingCaches( void );
	void appendRuntimeProfileSample( t3dRuntimeProfileSample_t &rSample ) const;
	EA_FORCE_INLINE bool isGpuRegionCacheEnabled( void ) const { return m_gpuRegionCache.isEnabled(); }
	EA_FORCE_INLINE int getGpuRegionCacheCapacity( void ) const { return m_gpuRegionCache.getCapacity(); }
	EA_FORCE_INLINE int getAuthoringGpuRegionBudgetMb( void ) const { return m_gpuRegionCache.getAuthoringBudgetMb(); }
	bool isAuthoringGpuFullResidencyPossible( void ) const;
	bool isAuthoringGpuReadbackSafe( void ) const;
	EA_FORCE_INLINE int getGpuRegionRequiredRadius( void ) const { return m_gpuRegionCache.getRequiredRadius(); }
	EA_FORCE_INLINE int getGpuRegionRequiredCorePages( void ) const { const int radius = m_gpuRegionCache.getRequiredRadius(); const int diameter = radius * 2 + 1; return MIN( diameter * diameter, m_regionRepository.getRegionMapSize() * m_regionRepository.getRegionMapSize() ); }
	int calculateGpuRegionRequiredCorePagesForLods( int nDetailLods ) const;
	int calculateGpuRegionCapacityForLods( int nDetailLods ) const;
	Dictionary getGpuRegionCacheStats( void ) const;
	Dictionary getGpuRegionCacheStatsFast( void ) const;
	Dictionary getStartupLoadStats( void ) const;
	size_t getGpuRegionCacheReservedBytes( void ) const;
	size_t getGpuRegionCacheMinimumBytes( void ) const;
	size_t getGpuRegionCacheHardCapBytes( void ) const;
	size_t getCpuRegionCacheReservedBytes( void ) const;
	size_t getCpuRegionCacheMinimumBytes( void ) const;
	size_t getCpuRegionCacheHardCapBytes( void ) const;
	EA_FORCE_INLINE size_t getInstanceRegionCacheReservedBytes( void ) const { return m_instanceRegionCache.getReservedBytes(); }
	size_t getMemoryTransientStagingBytes( void ) const;
	size_t getMemoryFixedRepositoryBytes( void ) const;
	Dictionary getRuntimeResidencyGrid( int nRadius = 6 ) const;
	void invalidateGpuFarSurface( void );
	void invalidateGpuFarAtlasLayout( void );
	bool ensureFarAtlases( void );
	Dictionary inspectGpuRegionPages( void );
	Dictionary cookGpuRegionPages( void );
	Dictionary getCpuRegionCacheStats( void ) const;
	Dictionary getCpuRegionCacheStatsFast( void ) const;
	Dictionary getInstanceRegionCacheStats( void ) const;
	Dictionary getInstanceRegionCacheStatsFast( void ) const;
	EA_FORCE_INLINE bool isInstanceRegionCacheEnabled( void ) const { return m_instanceRegionCache.isEnabled(); }
	EA_FORCE_INLINE bool isInstanceRegionResident( const Vector2i &vLocation ) const { return m_instanceRegionCache.isResident( vLocation ); }
	EA_FORCE_INLINE bool ensureInstanceRegionResident( const Vector2i &vLocation ) { return ensureInstanceRegionResidentInternal( vLocation, true ); }
	bool evictInstanceRegion( const Vector2i &vLocation );
	EA_FORCE_INLINE bool isCpuRegionCacheEnabled( void ) const { return m_cpuRegionCache.isEnabled(); }
	EA_FORCE_INLINE int getCpuRegionCacheCapacity( void ) const { return m_cpuRegionCache.getCapacity(); }
	void regenerateDerivedMaps( const eastl::vector<Vector2i> &rRegionLocations, const AABB &rEditedArea = AABB() );
	void publishDerivedMapUpdates( const eastl::vector<T3DDerivedMapUpdate> &rUpdates );
	Ref<T3DErosionJob> createErosionJob( const Ref<T3DErosionSettings> &rSettings );
	EA_FORCE_INLINE RID getHeightMapsRid( void ) const { return m_gpuRegionCache.isEnabled() ? m_gpuRegionCache.getHeightMapsRid() : m_generatedHeightMaps.getRid(); }
	EA_FORCE_INLINE RID getDerivedMapsRid( void ) const { return m_gpuRegionCache.isEnabled() ? m_gpuRegionCache.getDerivedMapsRid() : m_generatedDerivedMaps.getRid(); }
	EA_FORCE_INLINE RID getControlMapsRid( void ) const { return m_gpuRegionCache.isEnabled() ? m_gpuRegionCache.getControlMapsRid() : m_generatedControlMaps.getRid(); }
	EA_FORCE_INLINE RID getColorMapsRid( void ) const { return m_gpuRegionCache.isEnabled() ? m_gpuRegionCache.getColorMapsRid() : m_generatedColorMaps.getRid(); }
	EA_FORCE_INLINE RID getMiddleCompositeMapsRid( void ) const { return m_gpuRegionCache.isEnabled() ? m_gpuRegionCache.getMiddleCompositeMapsRid() : m_generatedMiddleCompositeMaps.getRid(); }
	EA_FORCE_INLINE RID getMiddleHeightMapsRid( void ) const { return m_middleHeightRegionCache.getHeightMapsRid(); }
	EA_FORCE_INLINE RID getMiddleHeightRegionMapRid( void ) const { return m_middleHeightRegionCache.getRegionMapRid(); }
	EA_FORCE_INLINE int getMiddleHeightResidentCount( void ) const { return m_middleHeightRegionCache.getResidentCount(); }
	EA_FORCE_INLINE int getMiddleHeightMissingRequiredLastTick( void ) const { return m_middleHeightRegionCache.getMissingRequiredLastTick(); }
	EA_FORCE_INLINE bool isMiddleHeightRegionResident( const Vector2i &vLocation ) const { return m_middleHeightRegionCache.isResident( vLocation ); }
	EA_FORCE_INLINE bool isGpuSurfaceRegionResident( const Vector2i &vLocation ) const { return m_gpuRegionCache.isResident( vLocation ); }
	Dictionary getMiddleHeightCacheStats( void ) const;
	size_t getMiddleHeightCacheReservedBytes( void ) const;
	int fillGpuMaterialPriorityIds( uint8_t *pIds, int nCapacity ) const;
	EA_FORCE_INLINE RID getFarHeightMapRid( void ) const { return m_farSurfaceAtlasCache.getHeightMapRid(); }
	EA_FORCE_INLINE RID getFarColorMapRid( void ) const { return m_farSurfaceAtlasCache.getColorMapRid(); }
	EA_FORCE_INLINE bool areControlMapsImplicit( void ) const { return m_bControlMapsImplicit; }
	EA_FORCE_INLINE bool areColorMapsImplicit( void ) const { return m_bColorMapsImplicit; }
	bool isGpuRegionColorChannelEnabled( void ) const;
	bool isGpuRegionDerivedChannelEnabled( void ) const;

	void setPixel( const t3dMapType_t mapType, const Vector3 &rGlobalPosition, const Color &rPixel );
	Color getPixel( const t3dMapType_t mapType, const Vector3 &rGlobalPosition ) const;
	void setHeight( const Vector3 &rGlobalPosition, const real_t fHeight );
	real_t getHeight( const Vector3 &rGlobalPosition ) const;
	// Bulk sampling resolves and maps each touched region once for the batch.
	// Missing regions and holes return NAN heights/normals and -1 controls.
	PackedFloat32Array getHeights( const PackedVector3Array &rGlobalPositions ) const;
	PackedVector3Array getNormals( const PackedVector3Array &rGlobalPositions ) const;
	PackedInt32Array getControls( const PackedVector3Array &rGlobalPositions ) const;
	void setColor( const Vector3 &rGlobalPosition, const Color &rColor );
	Color getColor( const Vector3 &rGlobalPosition ) const;
	void setControl( const Vector3 &rGlobalPosition, const uint32_t nControl );
	uint32_t getControl( const Vector3 &rGlobalPosition ) const;
	void setRoughness( const Vector3 &rGlobalPosition, const real_t fRoughness );
	real_t getRoughness( const Vector3 &rGlobalPosition ) const;

	// Control Map
	void setControlBaseId( const Vector3 &rGlobalPosition, const uint8_t nBase );
	uint32_t getControlBaseId( const Vector3 &rGlobalPosition ) const;
	void setControlOverlayId( const Vector3 &rGlobalPosition, const uint8_t nOverlay );
	uint32_t getControlOverlayId( const Vector3 &rGlobalPosition ) const;
	void setControlBlend( const Vector3 &rGlobalPosition, const real_t fBlend );
	real_t getControlBlend( const Vector3 &rGlobalPosition ) const;
	void setControlAngle( const Vector3 &rGlobalPosition, const real_t fAngle );
	real_t getControlAngle( const Vector3 &rGlobalPosition ) const;
	void setControlScale( const Vector3 &rGlobalPosition, const real_t fScale );
	real_t getControlScale( const Vector3 &rGlobalPosition ) const;
	void setControlHole( const Vector3 &rGlobalPosition, const bool bHole );
	bool getControlHole( const Vector3 &rGlobalPosition ) const;
	void setControlNavigation( const Vector3 &rGlobalPosition, const bool bNavigation );
	bool getControlNavigation( const Vector3 &rGlobalPosition ) const;
	void setControlAuto( const Vector3 &rGlobalPosition, const bool bAuto );
	bool getControlAuto( const Vector3 &rGlobalPosition ) const;

	Vector3 getNormal( const Vector3 &rGlobalPosition ) const;
	bool isInSlope(
		const Vector3 &rGlobalPosition, const Vector2 &rSlopeRange,
		const Vector3 &rNormal = V3_ZERO
	) const;
	Vector3 getTextureId( const Vector3 &rGlobalPosition ) const;
	Vector3 getMeshVertex( const int32_t nLod, const t3dHeightFilter_t filter, const Vector3 &rGlobalPosition ) const;

	void addEditedArea( const AABB &rArea );
	EA_FORCE_INLINE void clearEditedArea( void ) { m_mapEditService.clearEditedArea(); }
	EA_FORCE_INLINE AABB getEditedArea( void ) const { return m_mapEditService.getEditedArea(); }

	EA_FORCE_INLINE Vector2 getHeightRange( void ) const { return m_mapEditService.getHeightRange(); }
	void updateMasterHeight( const real_t fHeight );
	void updateMasterHeights( const Vector2 &rLowHigh );
	void calcHeightRange( const bool bRecursive = false );

	void importImages(
		const TypedArray<Image> &rImages, const Vector3 &rGlobalPosition = V3_ZERO,
		const real_t fOffset = 0.f, const real_t fScale = 1.f
	);
	Error exportImage( const String &rFileName, const t3dMapType_t mapType = T3DRegion::TYPE_HEIGHT ) const;
	Ref<Image> layeredToImage( const t3dMapType_t mapType ) const;

	// Utility
	void dump( const bool bVerbose = false ) const;

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( T3DData::t3dHeightFilter_t );

// Inline Region Functions

// Returns an index in the maximum 64x64 process-static region table.
// Repository operations additionally enforce the active map size, which is
// derived from the fixed 4096x4096 world resolution and current region size.
EA_FORCE_INLINE int T3DData::getRegionMapIndex( const Vector2i &rRegionLoc ) { return T3DRegionRepository::getRegionMapIndex( rRegionLoc ); }

// Returns a region location given a global position. No bounds checking nor data access.
EA_FORCE_INLINE Vector2i T3DData::getRegionLocation( const Vector3 &rGlobalPosition ) const { return m_regionRepository.getRegionLocation( rGlobalPosition ); }

EA_FORCE_INLINE int T3DData::getRegionId( const Vector2i &rRegionLoc ) const { return m_regionRepository.getRegionId( rRegionLoc ); }

EA_FORCE_INLINE int T3DData::getRegionIdp( const Vector3 &rGlobalPosition ) const { return m_regionRepository.getRegionId( m_regionRepository.getRegionLocation( rGlobalPosition ) ); }

EA_FORCE_INLINE Ref<T3DRegion> T3DData::getRegion( const Vector2i &rRegionLoc ) const { return m_regionRepository.getRegion( rRegionLoc ); }

EA_FORCE_INLINE T3DRegion *T3DData::getRegionPtr( const Vector2i &rRegionLoc ) const { return m_regionRepository.getRegionPtr( rRegionLoc ); }

EA_FORCE_INLINE Ref<T3DRegion> T3DData::getRegionp( const Vector3 &rGlobalPosition ) const { return m_regionRepository.getRegion( m_regionRepository.getRegionLocation( rGlobalPosition ) ); }

// Inline Map Functions

EA_FORCE_INLINE void T3DData::setHeight( const Vector3 &rGlobalPosition, const real_t fHeight ) { setPixel( T3DRegion::TYPE_HEIGHT, rGlobalPosition, Color( fHeight, 0.f, 0.f, 1.f ) ); }

EA_FORCE_INLINE void T3DData::setColor( const Vector3 &rGlobalPosition, const Color &rColor ) { Color clr = rColor; clr.a = getRoughness( rGlobalPosition ); setPixel( T3DRegion::TYPE_COLOR, rGlobalPosition, clr ); }

EA_FORCE_INLINE Color T3DData::getColor( const Vector3 &rGlobalPosition ) const { Color clr = getPixel( T3DRegion::TYPE_COLOR, rGlobalPosition ); clr.a = 1.0f; return clr; }

EA_FORCE_INLINE void T3DData::setControl( const Vector3 &rGlobalPosition, const uint32_t nControl ) { setPixel( T3DRegion::TYPE_CONTROL, rGlobalPosition, Color( asFloat( nControl ), 0.f, 0.f, 1.f ) ); }

EA_FORCE_INLINE void T3DData::setControlBaseId( const Vector3 &rGlobalPosition, const uint8_t nBase ) { const uint32_t control = getControl( rGlobalPosition ); const uint8_t base = CLAMP( nBase, ( uint8_t ) ( 0 ), ( uint8_t ) ( 31 ) ); setControl( rGlobalPosition, ( control & ~( 0x1F << 27 ) ) | encBase( base ) ); }

EA_FORCE_INLINE uint32_t T3DData::getControlBaseId( const Vector3 &rGlobalPosition ) const { const uint32_t control = getControl( rGlobalPosition ); return control == UINT32_MAX ? UINT32_MAX : getBase( control ); }

EA_FORCE_INLINE void T3DData::setControlOverlayId( const Vector3 &rGlobalPosition, const uint8_t nOverlay ) { const uint32_t control = getControl( rGlobalPosition ); const uint8_t overlay = CLAMP( nOverlay, ( uint8_t ) ( 0 ), ( uint8_t ) ( 31 ) ); setControl( rGlobalPosition, ( control & ~( 0x1F << 22 ) ) | encOverlay( overlay ) ); }

EA_FORCE_INLINE uint32_t T3DData::getControlOverlayId( const Vector3 &rGlobalPosition ) const { const uint32_t control = getControl( rGlobalPosition ); return control == UINT32_MAX ? UINT32_MAX : getOverlay( control ); }

// Expects 0.0 to 1.0 range
EA_FORCE_INLINE void T3DData::setControlBlend( const Vector3 &rGlobalPosition, const real_t fBlend ) { const uint32_t control = getControl( rGlobalPosition ); const uint8_t blend = ( uint8_t ) ( CLAMP( Math::round( fBlend * 255.f ), 0.f, 255.f ) ); setControl( rGlobalPosition, ( control & ~( 0xFF << 14 ) ) | encBlend( blend ) ); }

EA_FORCE_INLINE real_t T3DData::getControlBlend( const Vector3 &rGlobalPosition ) const { const uint32_t control = getControl( rGlobalPosition ); return control == UINT32_MAX ? NAN : ( real_t ) ( getBlend( control ) ) / 255.f; }

// Expects angle in degrees
EA_FORCE_INLINE void T3DData::setControlAngle( const Vector3 &rGlobalPosition, const real_t fAngle ) { const uint32_t control = getControl( rGlobalPosition ); const uint8_t uvrotation = ( uint8_t ) ( CLAMP( Math::round( fAngle / 22.5f ), 0.f, 15.f ) ); setControl( rGlobalPosition, ( control & ~( 0xF << 10 ) ) | encUvRotation( uvrotation ) ); }

// returns angle in degrees
EA_FORCE_INLINE real_t T3DData::getControlAngle( const Vector3 &rGlobalPosition ) const { const uint32_t control = getControl( rGlobalPosition ); const real_t fAngle = ( real_t ) ( getUvRotation( control ) ) * 22.5f; return control == UINT32_MAX ? NAN : fAngle; }

// Expects scale as a percentage modifier
inline void T3DData::setControlScale( const Vector3 &rGlobalPosition, const real_t fScale ) {
	const uint32_t control = getControl( rGlobalPosition );
	eastl::array<uint32_t, 8> scale_align = {5, 6, 7, 0, 1, 2, 3, 4};
	const uint8_t uvscale = scale_align[( uint8_t ) ( CLAMP( Math::round( ( fScale + 60.f ) / 20.f ), 0.f, 7.f ) )];
	setControl( rGlobalPosition, ( control & ~( 0x7 << 7 ) ) | encUvScale( uvscale ) );
}

inline real_t T3DData::getControlScale( const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	eastl::array<real_t, 8> scale_values = {0.0f, 20.0f, 40.0f, 60.0f, 80.0f, -60.0f, -40.0f, -20.0f};
	const real_t fScale = scale_values[getUvScale( control )]; // select from array UI return values
	return control == UINT32_MAX ? NAN : fScale;
}

EA_FORCE_INLINE void T3DData::setControlHole( const Vector3 &rGlobalPosition, const bool bHole ) { const uint32_t control = getControl( rGlobalPosition ); setControl( rGlobalPosition, ( control & ~( 0x1 << 2 ) ) | encHole( bHole ) ); }

EA_FORCE_INLINE bool T3DData::getControlHole( const Vector3 &rGlobalPosition ) const { const uint32_t control = getControl( rGlobalPosition ); return control == UINT32_MAX ? false : isHole( control ); }

EA_FORCE_INLINE void T3DData::setControlNavigation( const Vector3 &rGlobalPosition, const bool bNavigation ) { const uint32_t control = getControl( rGlobalPosition ); setControl( rGlobalPosition, ( control & ~( 0x1 << 1 ) ) | encNav( bNavigation ) ); }

EA_FORCE_INLINE bool T3DData::getControlNavigation( const Vector3 &rGlobalPosition ) const { const uint32_t control = getControl( rGlobalPosition ); return control == UINT32_MAX ? false : isNav( control ); }

EA_FORCE_INLINE void T3DData::setControlAuto( const Vector3 &rGlobalPosition, const bool bAuto ) { const uint32_t control = getControl( rGlobalPosition ); setControl( rGlobalPosition, ( control & ~( 0x1 ) ) | encAuto( bAuto ) ); }

EA_FORCE_INLINE bool T3DData::getControlAuto( const Vector3 &rGlobalPosition ) const { const uint32_t control = getControl( rGlobalPosition ); return control == UINT32_MAX ? false : isAuto( control ); }

EA_FORCE_INLINE void T3DData::setRoughness( const Vector3 &rGlobalPosition, const real_t fRoughness ) { Color clr = getPixel( T3DRegion::TYPE_COLOR, rGlobalPosition ); clr.a = fRoughness; setPixel( T3DRegion::TYPE_COLOR, rGlobalPosition, clr ); }

EA_FORCE_INLINE real_t T3DData::getRoughness( const Vector3 &rGlobalPosition ) const { return getPixel( T3DRegion::TYPE_COLOR, rGlobalPosition ).a; }

EA_FORCE_INLINE void T3DData::updateMasterHeight( const real_t fHeight ) { m_mapEditService.updateMasterHeight( fHeight ); }

EA_FORCE_INLINE void T3DData::updateMasterHeights( const Vector2 &rLowHigh ) { m_mapEditService.updateMasterHeights( rLowHigh ); }
#endif // TERRAIN3D_DATA_CLASS_H
