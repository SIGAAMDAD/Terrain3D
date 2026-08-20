// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_DATA_CLASS_H
#define TERRAIN3D_DATA_CLASS_H

#include "Terrain3DConstants.h"

#include "assets/generated_texture.h"
#include "data/terrain_3d_sampling_storage.h"
#include "data/terrain_3d_map_edit_service.h"
#include "data/terrain_3d_region_persistence.h"
#include "streaming/terrain_3d_cpu_cache_policy.h"
#include "streaming/terrain_3d_gpu_cache_policy.h"
#include "instances/terrain_3d_instance_cache_policy.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_repository.h"
#include "streaming/terrain_3d_runtime_profile.h"
#include "streaming/terrain_3d_runtime_page_store.h"
#include "streaming/terrain_3d_active_height_residency.h"
#include "streaming/terrain_3d_region_residency.h"

#include "eastl_config.h"
#include <EASTL/array.h>
#include <EASTL/bitset.h>
#include <cstdint>
#include <EASTL/vector.h>

class T3DTerrain;
class T3DErosionJob;
class T3DErosionSettings;
class T3DCompute;
typedef struct t3dfarSurfaceBakeData_s t3dfarSurfaceBakeData_t;

class T3DData : public Object {
	using Terrain3DData = T3DData;
	GDCLASS( Terrain3DData, Object );
	CLASS_NAME();
	friend T3DTerrain;
	friend class T3DErosionJob;
	friend class T3DRegionResidency;

public:														 // Constants
	// Maximum logical grid. The active grid is TERRAIN3D_ACTIVE_WORLD_TEXELS / region_size.

	typedef enum t3dheightFilter_e {
		HEIGHT_FILTER_NEAREST,
		HEIGHT_FILTER_MINIMUM
	} t3dheightFilter_t;

	typedef enum t3dmapEditCommitFlag_e : uint32_t {
		MAP_EDIT_UPDATE_INSTANCES = 1u << 0,
		MAP_EDIT_UPDATE_COLLISION = 1u << 1,
		MAP_EDIT_SNAP = 1u << 2,
		MAP_EDIT_KEEP_EDITED = 1u << 3,
	} t3dmapEditCommitFlag_t;


private:
	T3DTerrain *m_pTerrain = nullptr;

	T3DRegionRepository m_regionRepository;
	T3DSamplingStorage m_samplingStorage;
	T3DMapEditService m_mapEditService;
	T3DRegionPersistence m_regionPersistence;
	T3DRuntimePageStore m_runtimePageStore;
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
	// Height-only residency for Middle geometry. This cache is independent of
	// full surface pages so control/color/derived eviction cannot move vertices.
	T3DGeneratedTexture m_generatedMiddleHeightMaps;
	T3DGeneratedTexture m_generatedMiddleHeightRegionMap;
	uint16_t m_szMiddleHeightRegionMap[TERRAIN3D_MAX_REGION_SLOTS] = {};
	Vector2i m_szMiddleHeightLocations[TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS] = {};
	uint64_t m_szMiddleHeightLastUseTick[TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS] = {};
	uint64_t m_szMiddleHeightRetireAfterFrame[TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS] = {};
	uint8_t m_szMiddleHeightSlotStates[TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS] = {};
	int m_nMiddleHeightPhysicalLayerCount = 0;
	int m_nMiddleHeightMappedCapacity = 0;
	int m_nMiddleHeightResidentCount = 0;
	int m_nMiddleHeightDesiredLastTick = 0;
	int m_nMiddleHeightMissingRequiredLastTick = 0;
	int m_nMiddleHeightUploadsLastTick = 0;
	int m_nMiddleHeightEvictionsLastTick = 0;
	uint64_t m_nMiddleHeightTick = 0u;
	Vector2i m_middleHeightCenter = V2I_MAX;
	bool m_bMiddleHeightTexturesReady = false;
	bool m_bMiddleHeightPageTableDirty = true;
	bool m_bMiddleHeightSelectionDirty = true;
	uint64_t m_nMiddleHeightStreamingGenerationLast = 0u;

	T3DGeneratedTexture m_generatedFarHeightMap;
	T3DGeneratedTexture m_generatedFarColorMap;
	Ref<Image> m_pGpuFarHeightImage;
	Ref<Image> m_pGpuFarColorImage;
	eastl::bitset<TERRAIN3D_MAX_REGION_SLOTS> m_gpuFarHeightDirtyRegions;
	eastl::bitset<TERRAIN3D_MAX_REGION_SLOTS> m_gpuFarColorDirtyRegions;
	bool m_bGpuFarHeightDirty = true;
	bool m_bGpuFarColorDirty = true;
	bool m_bGpuFarHeightDirtyAll = true;
	bool m_bGpuFarColorDirtyAll = true;
	bool m_bGpuFarCookedLoaded = false;
	bool m_bGpuFarCookedSaved = false;
	int m_nGpuFarHeightUpdatesLastTick = 0;
	int m_nGpuFarColorUpdatesLastTick = 0;
	int m_nGpuFarHeightRegionsLastTick = 0;
	int m_nGpuFarColorRegionsLastTick = 0;
	uint64_t m_nGpuFarHeightProcessingUsecLast = 0u;
	uint64_t m_nGpuFarColorProcessingUsecLast = 0u;
	uint64_t m_nGpuFarHeightDirtyTexelsLast = 0u;
	uint64_t m_nGpuFarColorDirtyTexelsLast = 0u;
	uint64_t m_nGpuFarHeightRebuiltBytesLast = 0u;
	uint64_t m_nGpuFarColorRebuiltBytesLast = 0u;
	uint64_t m_nGpuFarHeightUploadedBytesLast = 0u;
	uint64_t m_nGpuFarColorUploadedBytesLast = 0u;
	uint64_t m_nGpuFarHeightPublicationUsecLast = 0u;
	uint64_t m_nGpuFarColorPublicationUsecLast = 0u;
	uint64_t m_nGpuFarHeightPublicationCount = 0u;
	uint64_t m_nGpuFarColorPublicationCount = 0u;
	uint64_t m_nGpuFarHeightUploadedBytesTotal = 0u;
	uint64_t m_nGpuFarColorUploadedBytesTotal = 0u;
	uint64_t m_nGpuFarCookIoUsecLast = 0u;

	// Xbox 360-style physical GPU residency. Logical terrain ownership remains in
	// the repository plus sparse direct-indexed active height pages; these fixed
	// tables map a bounded set of nearby logical regions into physical texture layers.
	bool m_bGpuRegionCacheEnabled = false;
	bool m_bGpuRegionCacheTexturesReady = false;
	bool m_bGpuRegionCacheSelectionDirty = true;
	int m_nGpuRegionCacheBudgetMb = 16;
	int m_nGpuRegionUploadBudgetLayers = 4;
	int m_nGpuRegionUploadBudgetMb = 2;
	int m_nGpuRegionSparePages = TERRAIN3D_GPU_CACHE_DEFAULT_SPARE_LAYERS;
	int m_nGpuRegionRetireFrames = TERRAIN3D_GPU_CACHE_DEFAULT_RETIRE_FRAMES;
	int m_nGpuRegionCacheCapacity = 0;
	int m_nGpuRegionPhysicalLayerCount = 0;
	int m_nGpuRegionSpareLayerCount = 0;
	int m_nGpuRegionResidentCount = 0;
	int m_nGpuRegionRetiredCount = 0;
	uint8_t m_szGpuRegionRepublishPending[TERRAIN3D_MAX_REGION_SLOTS] = {};
	int m_nGpuRegionRepublishPendingCount = 0;
	int m_nGpuRegionRepublishCursor = 0;
	int m_nGpuRegionDesiredLastTick = 0;
	int m_nGpuRegionRequiredLastTick = 0;
	int m_nGpuRegionGuardLastTick = 0;
	int m_nGpuRegionPrefetchLastTick = 0;
	int m_nGpuRegionHysteresisLastTick = 0;
	int m_nGpuRegionMissingRequiredLastTick = 0;
	uint32_t m_nGpuRegionStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	int m_nGpuRegionRequestedPages = 0;
	int m_nGpuRegionRequiredRadius = 0;
	int m_nGpuRegionColorDivisor = 1;
	int m_nGpuRegionDerivedDivisor = 1;
	bool m_bGpuRegionColorChannelAllocated = true;
	bool m_bGpuRegionDerivedChannelAllocated = true;
	int m_nGpuRegionUploadsLastTick = 0;
	uint64_t m_nGpuRegionUploadsSinceReset = 0u;
	int m_nGpuRegionEvictionsLastTick = 0;
	int m_nGpuRegionCookedHitsLastTick = 0;
	int m_nGpuRegionCookedMissesLastTick = 0;
	int m_nGpuRegionRuntimePreparesLastTick = 0;
	int m_nGpuRegionReusedLayersLastTick = 0;
	int m_nGpuRegionPageTableUpdatesLastTick = 0;
	int m_nGpuRegionHeightUpdatesLastTick = 0;
	int m_nGpuRegionControlUpdatesLastTick = 0;
	int m_nGpuRegionColorUpdatesLastTick = 0;
	int m_nGpuRegionDerivedUpdatesLastTick = 0;
	int m_nGpuRegionRuntimeColorMipsLastTick = 0;
	int m_nGpuRegionRuntimeDerivedBuildsLastTick = 0;
	int m_nGpuRegionPreparedPagesLastTick = 0;
	int m_nGpuRegionPrepareWorkersLastTick = 0;
	int m_nGpuRegionEffectiveUploadLimitLastTick = 0;
	bool m_bGpuRegionCameraMovedLastTick = false;
	size_t m_nGpuRegionUploadBytesLastTick = 0u;
	size_t m_nGpuRegionHeightBytesLastTick = 0u;
	size_t m_nGpuRegionControlBytesLastTick = 0u;
	size_t m_nGpuRegionColorBytesLastTick = 0u;
	size_t m_nGpuRegionDerivedBytesLastTick = 0u;
	uint64_t m_nGpuRegionTick = 0u;
	uint64_t m_nGpuRegionLastPublicationTick = 0u;
	Vector2i m_gpuRegionCacheCenter = V2I_MAX;
	Vector2i m_gpuRegionPredictionDirection = V2I_ZERO;
	uint64_t m_nGpuRegionStreamingGenerationLast = 0u;
	Vector2i m_szGpuRegionPriorityLocations[TERRAIN3D_GPU_CACHE_MAX_LAYERS] = {};
	uint8_t m_szGpuRegionPriorityClasses[TERRAIN3D_GPU_CACHE_MAX_LAYERS] = {};
	int m_nGpuRegionPriorityCount = 0;
	Vector2i m_szGpuRegionLocations[TERRAIN3D_GPU_CACHE_MAX_LAYERS] = {};
	uint64_t m_szGpuRegionLastUseTick[TERRAIN3D_GPU_CACHE_MAX_LAYERS] = {};
	uint64_t m_szGpuRegionLoadedTick[TERRAIN3D_GPU_CACHE_MAX_LAYERS] = {};
	uint64_t m_szGpuRegionRetireAfterFrame[TERRAIN3D_GPU_CACHE_MAX_LAYERS] = {};
	uint8_t m_szGpuRegionSlotStates[TERRAIN3D_GPU_CACHE_MAX_LAYERS] = {};
	uint8_t m_szGpuRegionPrefetchOrigin[TERRAIN3D_GPU_CACHE_MAX_LAYERS] = {};
	// Tracks which optional payloads currently occupy each physical layer. A
	// sparse region can reuse an already-default layer without issuing a redundant
	// control or color texture update.
	uint8_t m_szGpuRegionLayerChannelFlags[TERRAIN3D_GPU_CACHE_MAX_LAYERS] = {};
	uint32_t m_szGpuRegionMaterialMasks[TERRAIN3D_GPU_CACHE_MAX_LAYERS] = {};
	uint64_t m_nGpuRegionPrefetchUsedTotal = 0u;
	uint64_t m_nGpuRegionPrefetchDiscardedTotal = 0u;
	uint64_t m_nGpuRegionResidentTicksTotal = 0u;
	uint64_t m_nGpuRegionDepartedPagesTotal = 0u;
	uint16_t m_szGpuRegionMap[TERRAIN3D_MAX_REGION_SLOTS] = {};
	TypedArray<Image> m_gpuHeightMaps;
	TypedArray<Image> m_gpuControlMaps;
	TypedArray<Image> m_gpuColorMaps;
	TypedArray<Image> m_gpuDerivedMaps;
	TypedArray<Image> m_gpuMiddleCompositeMaps;

	// Runtime-page worker state and merged consumer queues are owned by
	// T3DRuntimePageStore. Publication timing remains here because GPU upload
	// itself is a renderer/cache concern outside the page store.
	uint64_t m_nGpuPreparePublicationUsecLast = 0u;
	uint64_t m_nGpuPrepareEndToEndUsecLast = 0u;
	uint64_t m_nGpuRegionServiceUsecLast = 0u;

	// Experimental bounded CPU map residency. Region resources retain metadata
	// and instances, while clean map payloads outside this fixed window are
	// released and reloaded from their external resource path on demand.
	bool m_bCpuRegionCacheEnabled = false;
	bool m_bCpuRegionCacheSelectionDirty = true;
	int m_nCpuRegionCacheBudgetMb = 16;
	int m_nCpuRegionLoadBudgetLayers = 4;
	int m_nCpuRegionLoadBudgetMb = 2;
	int m_nCpuRegionCacheCapacity = 0;
	int m_nCpuRegionResidentCount = 0;
	int m_nCpuRegionDesiredLastTick = 0;
	int m_nCpuRegionLoadsLastTick = 0;
	uint64_t m_nCpuRegionLoadsSinceReset = 0u;
	uint32_t m_nCpuRegionStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	int m_nCpuRegionEvictionsLastTick = 0;
	int m_nCpuRegionPinnedLastTick = 0;
	int m_nCpuRegionDemandLoads = 0;
	size_t m_nCpuRegionLoadBytesLastTick = 0u;
	uint64_t m_nCpuRegionServiceUsecLast = 0u;
	uint64_t m_nCpuRegionPrefetchStreamingGenerationLast = 0u;
	int m_nCpuRegionAsyncRequestsLastTick = 0;
	int m_nCpuRegionAsyncReadyLastTick = 0;
	int m_nCpuRegionAsyncPendingLastTick = 0;
	int m_nCpuRegionAsyncOutstanding = 0;
	uint8_t m_szCpuRegionAsyncState[TERRAIN3D_MAX_REGION_SLOTS] = {};
	Vector2i m_cpuRegionCacheCenter = V2I_MAX;
	Vector2i m_szCpuRegionLocations[TERRAIN3D_CPU_CACHE_MAX_LAYERS] = {};
	uint8_t m_szCpuRegionMap[TERRAIN3D_MAX_REGION_SLOTS] = {};

	// Bounded expansion cache for serialized static-instance pages. Every region
	// retains its compact page, while only nearby regions own decoded transform
	// arrays. The render-cell streamer consumes this same physical residency.
	bool m_bInstanceRegionCacheEnabled = false;
	bool m_bInstanceRegionCacheSelectionDirty = true;
	int m_nInstanceRegionCacheBudgetMb = 16;
	int m_nInstanceRegionLoadBudgetPages = 4;
	int m_nInstanceRegionLoadBudgetMb = 2;
	int m_nInstanceRegionCacheCapacity = TERRAIN3D_INSTANCE_CACHE_MAX_PAGES;
	int m_nInstanceRegionResidentCount = 0;
	int m_nInstanceRegionDesiredLastTick = 0;
	int m_nInstanceRegionLoadsLastTick = 0;
	uint32_t m_nInstanceRegionStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	int m_nInstanceRegionEvictionsLastTick = 0;
	int m_nInstanceRegionDemandLoads = 0;
	size_t m_nInstanceRegionResidentBytes = 0u;
	size_t m_nInstanceRegionLoadBytesLastTick = 0u;
	uint64_t m_nInstanceRegionServiceUsecLast = 0u;
	uint64_t m_nInstancePrefetchStreamingGenerationLast = 0u;
	Vector2i m_instanceRegionCacheCenter = V2I_MAX;
	Vector2i m_szInstanceRegionLocations[TERRAIN3D_INSTANCE_CACHE_MAX_PAGES] = {};
	uint8_t m_szInstanceRegionMap[TERRAIN3D_MAX_REGION_SLOTS] = {};

	// Optional compute backend. It is lazy so Compatibility/headless runtimes
	// retain the existing CPU path without constructing RenderingDevice state.
	T3DCompute *m_pCompute = nullptr;
	// Functions
	void clear( void );
	void invalidateSamplingCache( void ) { m_samplingStorage.invalidateAllSamplers(); }
	void publishRegionSamplingGeneration( const Vector2i &vLocation ) { m_samplingStorage.publishRegionGeneration( vLocation ); }
	void ensureDefaultMaps( void );
	bool setRegionSizeInternal( int nRegionSize );
	bool bindActiveStorageInternal( int nRegionSize, void *pBacking, size_t nBackingBytes ) {
		return m_samplingStorage.bindActive( nRegionSize, pBacking, nBackingBytes );
	}
	bool isActiveStorageOwnerInternal( void ) const { return m_samplingStorage.isActiveOwner(); }
	void resetResidentSamplingPoolInternal( void ) { m_samplingStorage.resetResidentPool(); }
	Error rehydrateActiveStorage( void );
	bool syncActiveRegion( const Vector2i &vLocation, const T3DRegion *pRegion );
	Ref<Image> getRegionMapOrDefault( const T3DRegion *pRegion, t3dmapType_t mapType ) const;
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
	void requestGpuRuntimePage( const Vector2i &vLocation, uint8_t nConsumers, uint8_t nPriority );
	void serviceGpuRuntimePagePrepare( const T3DResidencyFrameState &rFrame );
	bool isGpuRuntimePagePreparePending( const Vector2i &vLocation ) const;
	bool takePreparedRuntimeHeight(
		const Vector2i &vLocation, uint8_t nConsumer, Ref<Image> &rHeight, bool bMiddle,
		PackedByteArray *pNativeHeightBytes = nullptr
	);
	bool takePreparedRuntimeInstance( const Vector2i &vLocation, T3DInstanceStore &rStore );
	bool takePreparedGpuRuntimePage(
		const Vector2i &vLocation, Ref<Image> &rHeight,
		Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived, Ref<Image> &rMiddleComposite,
		uint32_t &rMaterialMask, bool &bAttempted, uint64_t *pRequestedUsec = nullptr
	);
	bool writeGpuRuntimePage( T3DRegion *pRegion, String *pError = nullptr );
	bool hasMaterializedMap( t3dmapType_t mapType ) const;
	bool refreshImplicitMapState( void );
	int getRegionSlotReserve( void ) const;
	bool canUpdateRegionSlotsIncrementally( void ) const;
	int calculateAuthoringGpuRegionBudgetMb( void ) const;
	int calculateGpuRegionCacheCapacity( void ) const;
	int calculateGpuRegionRequiredRadius( void ) const;
	int calculateGpuRegionRequestedPages( void ) const;
	int calculateGpuRegionPhysicalLayers( const int nMappedCapacity ) const;
	size_t estimateGpuRegionLayerBytes( void ) const;
	size_t estimateGpuRegionSharedBytes( void ) const;
	int getGpuFarHeightAtlasSide( void ) const;
	int getGpuFarSurfaceAtlasSide( void ) const;
	size_t getGpuFarHeightBytes( void ) const;
	size_t getGpuFarColorBytes( void ) const;
	int estimateGpuRegionUploadCalls( const T3DRegion *pRegion, int nLayer ) const;
	size_t estimateGpuRegionUploadBytes( const T3DRegion *pRegion, int nLayer ) const;
	int getGpuRegionTextureUpdateCallsLastTick( void ) const;
	void resetGpuRegionCache( const bool bClearTextures );
	bool rebuildGpuRegionCacheTextures( void );
	String getGpuFarAtlasPath( const String &rDirectory = String() ) const;
	uint64_t computeGpuFarAtlasSourceSignature( void ) const;
	bool isFarAtlasRequired( void ) const;
	bool initializeGpuFarAtlasImages( void );
	bool loadCookedGpuFarAtlases( void );
	bool writeCookedGpuFarAtlases( const String &rDirectory = String() );
	void markAllGpuFarRegionsDirty( t3dmapType_t mapType );
	void markGpuFarRegionDirty( const Vector2i &vLocation, t3dmapType_t mapType );
	bool patchGpuFarHeightRegion( const Vector2i &vLocation, PackedByteArray &nAtlasBytes );
	bool patchGpuFarColorRegion(
		const Vector2i &vLocation, PackedByteArray &nAtlasBytes,
		const t3dfarSurfaceBakeData_t &rBakeData
	);
	bool rebuildGpuFarHeightAtlas( void );
	bool rebuildGpuFarColorAtlas( void );
	int findGpuRegionSlot( const Vector2i &vLocation ) const;
	int findFreeGpuRegionSlot( void ) const;
	void recordGpuRegionDeparture( int nLayer, bool bCountUnusedPrefetch );
	void retireGpuRegionSlot( int nLayer );
	void reclaimRetiredGpuRegionSlots( void );
	void armPendingGpuRegionRetirements( void );
	void invalidateGpuRegionLocation( const Vector2i &vLocation );
	bool uploadGpuRegionLayer( const int nLayer, const Vector2i &vLocation, const t3dmapType_t mapType );
	bool republishGpuRegionLocation( const Vector2i &vLocation );
	void queueGpuRegionRepublish( const Vector2i &vLocation );
	int processPendingGpuRegionRepublishes( int nLimit );
	int collectDesiredGpuRegions(
		const T3DResidencyFrameState &rFrame, const Vector2i &rCenter, Vector2i *pLocations, int nCapacity,
		t3dgpuCacheSelectionStats_t &rStats
	);
	void resetMiddleHeightCache( void );
	bool configureMiddleHeightCache( void );
	void updateMiddleHeightResidencyInternal( const T3DResidencyFrameState &rFrame, bool bForce );
	int collectDesiredMiddleHeightRegions( const T3DResidencyFrameState &rFrame, const Vector2i &rCenter, Vector2i *pLocations, int nCapacity, int &rRequiredCount ) const;
	int findMiddleHeightSlot( const Vector2i &vLocation ) const;
	bool uploadMiddleHeightLayer( int nLayer, const Vector2i &vLocation, const Ref<Image> &rPreparedHeight = Ref<Image>() );
	bool updateMiddleHeightPageTableTexture( void );
	Ref<Image> makeMiddleHeightImage( const Vector2i &vLocation, bool bRuntimeResolution = false ) const;
	int calculateCpuRegionCacheCapacity( void ) const;
	size_t estimateCpuRegionLayerBytes( void ) const;
	void resetCpuRegionCache( bool bReleaseCleanMaps );
	int findCpuRegionSlot( const Vector2i &vLocation ) const;
	void invalidateCpuRegionLocation( const Vector2i &vLocation );
	bool loadCpuRegionSource( const Vector2i &vLocation, Ref<T3DRegion> &rSource ) const;
	bool requestCpuRegionSourceAsync( const Vector2i &vLocation );
	bool takeCpuRegionSourceAsync( const Vector2i &vLocation, Ref<T3DRegion> &rSource );
	void prewarmCpuRegionSources( const T3DResidencyFrameState &rFrame );
	void reapCpuRegionSourceAsync( const T3DResidencyFrameState &rFrame );
	bool publishCpuRegionSlot( int iSlot, const Vector2i &vLocation, const Ref<T3DRegion> &rSource );
	bool evictCpuRegionSlot( int iSlot );
	bool ensureCpuRegionResidentInternal( const Vector2i &vLocation, bool bDemandLoad );
	void releaseUntrackedCpuMaps( void );
	bool restoreAllCpuMaps( void );
	int collectDesiredCpuRegions( const T3DResidencyFrameState &rFrame, const Vector2i &rCenter, Vector2i *pLocations, int nCapacity ) const;
	bool ensureCpuSampleArea( const Vector3 &rWorldPosition, int nRadiusTexels );
	void resetInstanceRegionCache( bool bRestoreAll );
	int findInstanceRegionSlot( const Vector2i &vLocation ) const;
	void invalidateInstanceRegionLocation( const Vector2i &vLocation );
	bool evictInstanceRegionSlot( int iSlot );
	bool ensureInstanceRegionResidentInternal( const Vector2i &vLocation, bool bDemandLoad );
	int collectDesiredInstanceRegions( const T3DResidencyFrameState &rFrame, const Vector2i &rCenter, Vector2i *pLocations, int nCapacity ) const;
	void updateMapsBounded( const t3dmapType_t mapType, const bool bAllRegions );
	void emitGpuRegionMapChanged( void );
	void emitRegionLayoutChanged( const bool bTextureLayersChanged );
	TypedArray<Image> getActiveMaps( const t3dmapType_t mapType ) const;
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
	bool isInitializedFor( const T3DTerrain *pTerrain ) const {
		return m_bRuntimeActive && m_pTerrain == pTerrain;
	}
	bool isRuntimeReady( void ) const {
		return m_bRuntimeActive && m_samplingStorage.isActiveOwner() && m_regionRepository.getRegionMapSize() > 0;
	}
	bool isRepositoryLoaded( void ) const {
		return m_regionRepository.isLoaded();
	}
	uint32_t getActiveStorageGeneration( void ) const {
		return isRuntimeReady() ? m_samplingStorage.getActiveGeneration() : 0u;
	}
	Dictionary getRuntimeState( void ) const;

	// Regions

	int getRegionCount( void ) const {
		return m_regionRepository.getActiveRegionCount();
	}
	void setRegionLocations( const TypedArray<Vector2i> &vLocations );
	TypedArray<Vector2i> getRegionLocations( void ) const;
	// Internal layer-ordered slots, including V2I_MAX spare entries. Custom
	// shaders require this exact ordering to match the texture arrays.
	TypedArray<Vector2i> getRegionSlots( void ) const;
	TypedArray<Vector2i> getRenderRegionSlots( void ) const;
	const Vector2i *getRegionSlotsInternal( void ) const {
		return m_regionRepository.getSlotsData();
	}
	int getRegionSlotCountInternal( void ) const {
		return m_regionRepository.getSlotCount();
	}
	T3DRegion *const *getRegionPtrMapInternal( void ) const {
		return m_regionRepository.getRegionPtrMapData();
	}
	int getRegionSizeInternal( void ) const {
		return m_regionRepository.getRegionSize();
	}
	int getRegionMapSizeInternal( void ) const {
		return m_regionRepository.getRegionMapSize();
	}
	bool matchesRegionLayoutInternal( const int nRegionSize ) const {
		return m_regionRepository.getRegionSize() == nRegionSize &&
			m_regionRepository.getRegionSizeVector() == V2I( nRegionSize );
	}
	T3DRegionRepositoryView getRegionRepositoryViewInternal( void ) const {
		return m_regionRepository.getView();
	}
	uint64_t getRegionTopologyGenerationInternal( void ) const {
		return m_regionRepository.getTopologyGeneration();
	}
	void setVertexSpacingInternal( const real_t fVertexSpacing ) {
		m_regionRepository.setVertexSpacing( fVertexSpacing );
	}
	bool isRegionLocationValidInternal( const Vector2i &vLocation ) const {
		return m_regionRepository.isLocationValid( vLocation );
	}
	real_t getVertexSpacingInternal( void ) const {
		return m_regionRepository.getVertexSpacing();
	}
	T3DSamplingSource getSamplingSourceInternal( void ) const {
		return T3DSamplingSource{ &m_regionRepository, &m_samplingStorage };
	}
	TypedArray<T3DRegion> getRegionsActive( const bool bCopy = false, const bool bDeep = false ) const;
	Dictionary getRegionsAll( void ) const {
		return m_regionRepository.getRegions();
	}
	PackedInt32Array getRegionMap( void ) const;
	PackedInt32Array getLogicalRegionMap( void ) const;
	int getRegionMapSize( void ) const {
		return m_regionRepository.getRegionMapSize();
	}
	static int getRegionMapIndex( const Vector2i &rRegionLoc );

	void doForRegions( const Rect2i &rArea, const Callable &rCallback );
	void changeRegionSize( int region_size );

	Vector2i getRegionLocation( const Vector3 &rGlobalPosition ) const;
	int getRegionId( const Vector2i &rRegionLoc ) const;
	int getRegionIdp( const Vector3 &rGlobalPosition ) const;

	bool hasRegion( const Vector2i &rRegionLoc ) const {
		return getRegionId( rRegionLoc ) != -1;
	}
	bool hasRegionp( const Vector3 &rGlobalPosition ) const {
		return getRegionIdp( rGlobalPosition ) != -1;
	}
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
	// mutating Terrain3DRegion::setData() on an attached resource.
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
	TypedArray<Image> getMaps( const t3dmapType_t mapType ) const;
	void updateMaps(
		const t3dmapType_t mapType = T3DRegion::TYPE_MAX, const bool bAllRegions = true,
		const bool bGenerateMipmaps = false
	);
	void updateMapRegions( const t3dmapType_t mapType, const eastl::vector<Vector2i> &rRegionLocations );
	void commitMapEdits(
		const t3dmapType_t mapType, const eastl::vector<Vector2i> &rRegionLocations,
		const AABB &rEditedArea, const uint32_t nFlags
	);
	void commitMapEdit(
		const t3dmapType_t mapType, const Vector2i &rRegionLocation,
		const Rect2i &rDirtyRect = Rect2i()
	);
	T3DCompute *getComputeBackend( void );
	T3DCompute *getComputeBackendIfCreated( void ) const {
		return m_pCompute;
	}
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
	uint32_t getGpuRegionStallFlagsLastTick( void ) const {
		return m_nGpuRegionStallFlagsLastTick;
	}
	uint32_t getCpuRegionStallFlagsLastTick( void ) const {
		return m_nCpuRegionStallFlagsLastTick;
	}
	uint32_t getInstanceRegionStallFlagsLastTick( void ) const {
		return m_nInstanceRegionStallFlagsLastTick;
	}
	bool ensureCpuRegionResident( const Vector2i &vLocation );
	void updateCpuRegionResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void updateInstanceRegionResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void updateGpuRegionResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void updateRegionResidency( const Vector3 &rWorldPosition, bool bForce = false );
	void resetRuntimeStreamingCaches( void );
	void appendRuntimeProfileSample( t3druntimeProfileSample_t &rSample ) const;
	bool isGpuRegionCacheEnabled( void ) const {
		return m_bGpuRegionCacheEnabled;
	}
	int getGpuRegionCacheCapacity( void ) const {
		return m_nGpuRegionCacheCapacity;
	}
	int getAuthoringGpuRegionBudgetMb( void ) const {
		return calculateAuthoringGpuRegionBudgetMb();
	}
	bool isAuthoringGpuFullResidencyPossible( void ) const;
	bool isAuthoringGpuReadbackSafe( void ) const;
	int getGpuRegionRequiredRadius( void ) const {
		return calculateGpuRegionRequiredRadius();
	}
	int getGpuRegionRequiredCorePages( void ) const {
		const int radius = calculateGpuRegionRequiredRadius();
		const int diameter = radius * 2 + 1;
		return MIN( diameter * diameter, m_regionRepository.getRegionMapSize() * m_regionRepository.getRegionMapSize() );
	}
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
	size_t getInstanceRegionCacheReservedBytes( void ) const { return m_nInstanceRegionResidentBytes; }
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
	bool isInstanceRegionCacheEnabled( void ) const {
		return m_bInstanceRegionCacheEnabled;
	}
	bool isInstanceRegionResident( const Vector2i &vLocation ) const {
		return !m_bInstanceRegionCacheEnabled || findInstanceRegionSlot( vLocation ) >= 0;
	}
	bool ensureInstanceRegionResident( const Vector2i &vLocation ) {
		return ensureInstanceRegionResidentInternal( vLocation, true );
	}
	bool evictInstanceRegion( const Vector2i &vLocation ) {
		const int slot = findInstanceRegionSlot( vLocation );
		if ( slot >= 0 ) {
			return evictInstanceRegionSlot( slot );
		}
		T3DRegion *region = getRegionPtr( vLocation );
		return !region || !region->isInstanceStoreResident() || region->evictInstanceStore();
	}
	bool isCpuRegionCacheEnabled( void ) const {
		return m_bCpuRegionCacheEnabled;
	}
	int getCpuRegionCacheCapacity( void ) const {
		return m_nCpuRegionCacheCapacity;
	}
	void regenerateDerivedMaps( const eastl::vector<Vector2i> &rRegionLocations, const AABB &rEditedArea = AABB() );
	void publishDerivedMapUpdates( const eastl::vector<T3DDerivedMapUpdate> &rUpdates );
	Ref<T3DErosionJob> createErosionJob( const Ref<T3DErosionSettings> &rSettings );
	RID getHeightMapsRid( void ) const {
		return m_generatedHeightMaps.getRid();
	}
	RID getDerivedMapsRid( void ) const {
		return m_generatedDerivedMaps.getRid();
	}
	RID getControlMapsRid( void ) const {
		return m_generatedControlMaps.getRid();
	}
	RID getColorMapsRid( void ) const {
		return m_generatedColorMaps.getRid();
	}
	RID getMiddleCompositeMapsRid( void ) const { return m_generatedMiddleCompositeMaps.getRid(); }
	RID getMiddleHeightMapsRid( void ) const { return m_generatedMiddleHeightMaps.getRid(); }
	RID getMiddleHeightRegionMapRid( void ) const { return m_generatedMiddleHeightRegionMap.getRid(); }
	int getMiddleHeightResidentCount( void ) const { return m_nMiddleHeightResidentCount; }
	int getMiddleHeightMissingRequiredLastTick( void ) const { return m_nMiddleHeightMissingRequiredLastTick; }
	bool isMiddleHeightRegionResident( const Vector2i &vLocation ) const { return findMiddleHeightSlot( vLocation ) >= 0; }
	bool isGpuSurfaceRegionResident( const Vector2i &vLocation ) const { return findGpuRegionSlot( vLocation ) >= 0; }
	Dictionary getMiddleHeightCacheStats( void ) const;
	size_t getMiddleHeightCacheReservedBytes( void ) const;
	int fillGpuMaterialPriorityIds( uint8_t *pIds, int nCapacity ) const;
	RID getFarHeightMapRid( void ) const {
		return m_generatedFarHeightMap.getRid();
	}
	RID getFarColorMapRid( void ) const {
		return m_generatedFarColorMap.getRid();
	}
	bool areControlMapsImplicit( void ) const {
		return m_bControlMapsImplicit;
	}
	bool areColorMapsImplicit( void ) const {
		return m_bColorMapsImplicit;
	}
	bool isGpuRegionColorChannelEnabled( void ) const;
	bool isGpuRegionDerivedChannelEnabled( void ) const;

	void setPixel( const t3dmapType_t mapType, const Vector3 &rGlobalPosition, const Color &rPixel );
	Color getPixel( const t3dmapType_t mapType, const Vector3 &rGlobalPosition ) const;
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
	Vector3 getMeshVertex( const int32_t nLod, const t3dheightFilter_t filter, const Vector3 &rGlobalPosition ) const;

	void addEditedArea( const AABB &rArea );
	void clearEditedArea( void ) {
		m_mapEditService.clearEditedArea();
	}
	AABB getEditedArea( void ) const {
		return m_mapEditService.getEditedArea();
	}

	Vector2 getHeightRange( void ) const {
		return m_mapEditService.getHeightRange();
	}
	void updateMasterHeight( const real_t fHeight );
	void updateMasterHeights( const Vector2 &rLowHigh );
	void calcHeightRange( const bool bRecursive = false );

	void importImages(
		const TypedArray<Image> &rImages, const Vector3 &rGlobalPosition = V3_ZERO,
		const real_t fOffset = 0.f, const real_t fScale = 1.f
	);
	Error exportImage( const String &rFileName, const t3dmapType_t mapType = T3DRegion::TYPE_HEIGHT ) const;
	Ref<Image> layeredToImage( const t3dmapType_t mapType ) const;

	// Utility
	void dump( const bool bVerbose = false ) const;

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( T3DData::t3dheightFilter_t );

// Inline Region Functions

// Returns an index in the maximum 64x64 process-static region table.
// Repository operations additionally enforce the active map size, which is
// derived from the fixed 4096x4096 world resolution and current region size.
inline int T3DData::getRegionMapIndex( const Vector2i &rRegionLoc ) {
	return T3DRegionRepository::getRegionMapIndex( rRegionLoc );
}

// Returns a region location given a global position. No bounds checking nor data access.
inline Vector2i T3DData::getRegionLocation( const Vector3 &rGlobalPosition ) const {
	return m_regionRepository.getRegionLocation( rGlobalPosition );
}

inline int T3DData::getRegionId( const Vector2i &rRegionLoc ) const {
	return m_regionRepository.getRegionId( rRegionLoc );
}

inline int T3DData::getRegionIdp( const Vector3 &rGlobalPosition ) const {
	return m_regionRepository.getRegionId( m_regionRepository.getRegionLocation( rGlobalPosition ) );
}

inline Ref<T3DRegion> T3DData::getRegion( const Vector2i &rRegionLoc ) const {
	return m_regionRepository.getRegion( rRegionLoc );
}

inline T3DRegion *T3DData::getRegionPtr( const Vector2i &rRegionLoc ) const {
	return m_regionRepository.getRegionPtr( rRegionLoc );
}

inline Ref<T3DRegion> T3DData::getRegionp( const Vector3 &rGlobalPosition ) const {
	return m_regionRepository.getRegion( m_regionRepository.getRegionLocation( rGlobalPosition ) );
}

// Inline Map Functions

inline void T3DData::setHeight( const Vector3 &rGlobalPosition, const real_t fHeight ) {
	setPixel( T3DRegion::TYPE_HEIGHT, rGlobalPosition, Color( fHeight, 0.f, 0.f, 1.f ) );
}

inline void T3DData::setColor( const Vector3 &rGlobalPosition, const Color &rColor ) {
	Color clr = rColor;
	clr.a = getRoughness( rGlobalPosition );
	setPixel( T3DRegion::TYPE_COLOR, rGlobalPosition, clr );
}

inline Color T3DData::getColor( const Vector3 &rGlobalPosition ) const {
	Color clr = getPixel( T3DRegion::TYPE_COLOR, rGlobalPosition );
	clr.a = 1.0f;
	return clr;
}

inline void T3DData::setControl( const Vector3 &rGlobalPosition, const uint32_t nControl ) {
	setPixel( T3DRegion::TYPE_CONTROL, rGlobalPosition, Color( asFloat( nControl ), 0.f, 0.f, 1.f ) );
}

inline void T3DData::setControlBaseId( const Vector3 &rGlobalPosition, const uint8_t nBase ) {
	const uint32_t control = getControl( rGlobalPosition );
	const uint8_t base = CLAMP( nBase, ( uint8_t ) ( 0 ), ( uint8_t ) ( 31 ) );
	setControl( rGlobalPosition, ( control & ~( 0x1F << 27 ) ) | encBase( base ) );
}

inline uint32_t T3DData::getControlBaseId( const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	return control == UINT32_MAX ? UINT32_MAX : getBase( control );
}

inline void T3DData::setControlOverlayId( const Vector3 &rGlobalPosition, const uint8_t nOverlay ) {
	const uint32_t control = getControl( rGlobalPosition );
	const uint8_t overlay = CLAMP( nOverlay, ( uint8_t ) ( 0 ), ( uint8_t ) ( 31 ) );
	setControl( rGlobalPosition, ( control & ~( 0x1F << 22 ) ) | encOverlay( overlay ) );
}

inline uint32_t T3DData::getControlOverlayId( const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	return control == UINT32_MAX ? UINT32_MAX : getOverlay( control );
}

// Expects 0.0 to 1.0 range
inline void T3DData::setControlBlend( const Vector3 &rGlobalPosition, const real_t fBlend ) {
	const uint32_t control = getControl( rGlobalPosition );
	const uint8_t blend = ( uint8_t ) ( CLAMP( Math::round( fBlend * 255.f ), 0.f, 255.f ) );
	setControl( rGlobalPosition, ( control & ~( 0xFF << 14 ) ) | encBlend( blend ) );
}

inline real_t T3DData::getControlBlend( const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	return control == UINT32_MAX ? NAN : ( real_t ) ( getBlend( control ) ) / 255.f;
}

// Expects angle in degrees
inline void T3DData::setControlAngle( const Vector3 &rGlobalPosition, const real_t fAngle ) {
	const uint32_t control = getControl( rGlobalPosition );
	const uint8_t uvrotation = ( uint8_t ) ( CLAMP( Math::round( fAngle / 22.5f ), 0.f, 15.f ) );
	setControl( rGlobalPosition, ( control & ~( 0xF << 10 ) ) | encUvRotation( uvrotation ) );
}

// returns angle in degrees
inline real_t T3DData::getControlAngle( const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	const real_t fAngle = ( real_t ) ( getUvRotation( control ) ) * 22.5f;
	return control == UINT32_MAX ? NAN : fAngle;
}

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

inline void T3DData::setControlHole( const Vector3 &rGlobalPosition, const bool bHole ) {
	const uint32_t control = getControl( rGlobalPosition );
	setControl( rGlobalPosition, ( control & ~( 0x1 << 2 ) ) | encHole( bHole ) );
}

inline bool T3DData::getControlHole( const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	return control == UINT32_MAX ? false : isHole( control );
}

inline void T3DData::setControlNavigation( const Vector3 &rGlobalPosition, const bool bNavigation ) {
	const uint32_t control = getControl( rGlobalPosition );
	setControl( rGlobalPosition, ( control & ~( 0x1 << 1 ) ) | encNav( bNavigation ) );
}

inline bool T3DData::getControlNavigation( const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	return control == UINT32_MAX ? false : isNav( control );
}

inline void T3DData::setControlAuto( const Vector3 &rGlobalPosition, const bool bAuto ) {
	const uint32_t control = getControl( rGlobalPosition );
	setControl( rGlobalPosition, ( control & ~( 0x1 ) ) | encAuto( bAuto ) );
}

inline bool T3DData::getControlAuto( const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	return control == UINT32_MAX ? false : isAuto( control );
}

inline void T3DData::setRoughness( const Vector3 &rGlobalPosition, const real_t fRoughness ) {
	Color clr = getPixel( T3DRegion::TYPE_COLOR, rGlobalPosition );
	clr.a = fRoughness;
	setPixel( T3DRegion::TYPE_COLOR, rGlobalPosition, clr );
}

inline real_t T3DData::getRoughness( const Vector3 &rGlobalPosition ) const {
	return getPixel( T3DRegion::TYPE_COLOR, rGlobalPosition ).a;
}

inline void T3DData::updateMasterHeight( const real_t fHeight ) {
	m_mapEditService.updateMasterHeight( fHeight );
}

inline void T3DData::updateMasterHeights( const Vector2 &rLowHigh ) {
	m_mapEditService.updateMasterHeights( rLowHigh );
}

using Terrain3DData = T3DData;

#endif // TERRAIN3D_DATA_CLASS_H
