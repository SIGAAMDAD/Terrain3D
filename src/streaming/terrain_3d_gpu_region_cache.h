// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_GPU_REGION_CACHE_H
#define TERRAIN3D_GPU_REGION_CACHE_H

#include "core/terrain_3d_compiler.h"
#include "Terrain3DConstants.h"
#include "assets/generated_texture.h"
#include "streaming/terrain_3d_gpu_cache_constants.h"
#include "streaming/terrain_3d_gpu_cache_policy.h"
#include "streaming/terrain_3d_residency_state.h"
#include "streaming/terrain_3d_runtime_profile.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstddef>
#include <cstdint>

class T3DCompute;
class T3DCpuRegionCache;
class T3DMapEditService;
class T3DRegion;
class T3DRegionRepository;
class T3DRuntimePageStore;
class T3DSamplingStorage;
class T3DTerrain;

struct T3DGpuRegionCacheStats {
	bool bEnabled = false;
	bool bTexturesReady = false;
	bool bSelectionPending = true;
	bool bCameraMovedLastTick = false;
	int nBudgetMb = 0;
	int nCapacity = 0;
	int nPhysicalLayers = 0;
	int nSpareLayers = 0;
	int nResident = 0;
	int nRetired = 0;
	int nDesiredLastTick = 0;
	int nRequiredLastTick = 0;
	int nGuardLastTick = 0;
	int nPrefetchLastTick = 0;
	int nHysteresisLastTick = 0;
	int nMissingRequiredLastTick = 0;
	int nRequestedPages = 0;
	int nRequiredRadius = 0;
	int nUploadsLastTick = 0;
	int nEvictionsLastTick = 0;
	int nCookedHitsLastTick = 0;
	int nCookedMissesLastTick = 0;
	int nRuntimePreparesLastTick = 0;
	int nReusedLayersLastTick = 0;
	int nPageTableUpdatesLastTick = 0;
	int nHeightUpdatesLastTick = 0;
	int nControlUpdatesLastTick = 0;
	int nColorUpdatesLastTick = 0;
	int nDerivedUpdatesLastTick = 0;
	int nRuntimeColorMipsLastTick = 0;
	int nRuntimeDerivedBuildsLastTick = 0;
	int nPreparedPagesLastTick = 0;
	int nPrepareWorkersLastTick = 0;
	int nEffectiveUploadLimitLastTick = 0;
	uint32_t nStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	size_t nUploadBytesLastTick = 0u;
	size_t nHeightBytesLastTick = 0u;
	size_t nControlBytesLastTick = 0u;
	size_t nColorBytesLastTick = 0u;
	size_t nDerivedBytesLastTick = 0u;
	uint64_t nServiceUsecLast = 0u;
	uint64_t nPreparePublicationUsecLast = 0u;
	uint64_t nPrepareEndToEndUsecLast = 0u;
};

struct T3DGpuRegionCacheUpdateResult {
	bool bPageTableChanged = false;
};

class T3DGpuRegionCache {
public:
	T3DGpuRegionCache(
		T3DRegionRepository &rRepository,
		T3DSamplingStorage &rSamplingStorage,
		T3DMapEditService &rMapEditService,
		T3DRuntimePageStore &rRuntimePageStore,
		T3DCpuRegionCache &rCpuRegionCache
	);

	EA_FORCE_INLINE void setTerrain( T3DTerrain *pTerrain ) { m_pTerrain = pTerrain; }
	void setDefaultMaps(
		const Ref<Image> &rHeight, const Ref<Image> &rControl,
		const Ref<Image> &rColor, const Ref<Image> &rDerived,
		bool bControlImplicit, bool bColorImplicit
	);
	bool configure(
		bool bEnabled, int nBudgetMb, int nUploadLayers,
		int nUploadBudgetMb, int nSparePages, int nRetireFrames
	);
	void reset( bool bClearTextures );
	T3DGpuRegionCacheUpdateResult update(
		const T3DResidencyFrameState &rFrame, bool bForce,
		int nEffectiveUploadLayers, T3DCompute *pCompute
	);
	void invalidate( const Vector2i &vLocation );
	bool republish( const Vector2i &vLocation, T3DCompute *pCompute = nullptr );
	bool republishAllResidents( T3DCompute *pCompute = nullptr );
	void queueRepublish( const Vector2i &vLocation );
	bool updateResidentLocation(
		const Vector2i &vLocation, t3dmapType_t eMapType,
		T3DCompute *pCompute = nullptr
	);
	void armPendingRetirements( void );
	EA_FORCE_INLINE void markSelectionDirty( void ) { m_bGpuRegionCacheSelectionDirty = true; }

	EA_FORCE_INLINE bool isEnabled( void ) const { return m_bGpuRegionCacheEnabled; }
	EA_FORCE_INLINE bool isTexturesReady( void ) const { return m_bGpuRegionCacheTexturesReady; }
	EA_FORCE_INLINE bool isSelectionDirty( void ) const { return m_bGpuRegionCacheSelectionDirty; }
	EA_FORCE_INLINE bool isResident( const Vector2i &vLocation ) const { return findGpuRegionSlot( vLocation ) >= 0; }
	EA_FORCE_INLINE int getLayerForLocation( const Vector2i &vLocation ) const { return findGpuRegionSlot( vLocation ); }
	EA_FORCE_INLINE int getCapacity( void ) const { return m_nGpuRegionCacheCapacity; }
	EA_FORCE_INLINE int getPhysicalLayerCount( void ) const { return m_nGpuRegionPhysicalLayerCount; }
	EA_FORCE_INLINE int getResidentCount( void ) const { return m_nGpuRegionResidentCount; }
	EA_FORCE_INLINE int getRequiredRadius( void ) const { return calculateGpuRegionRequiredRadius(); }
	EA_FORCE_INLINE int getAuthoringBudgetMb( void ) const { return calculateAuthoringGpuRegionBudgetMb(); }
	EA_FORCE_INLINE int getUploadBudgetLayers( void ) const { return m_nGpuRegionUploadBudgetLayers; }
	EA_FORCE_INLINE uint32_t getStallFlagsLastTick( void ) const { return m_nGpuRegionStallFlagsLastTick; }
	bool isColorChannelEnabled( void ) const;
	bool isDerivedChannelEnabled( void ) const;
	int calculateRequiredCorePagesForLods( int nDetailLods ) const;
	int calculateCapacityForLods( int nDetailLods ) const;
	bool isAuthoringFullResidencyPossible( void ) const;
	bool isAuthoringReadbackSafe( void ) const;
	int fillMaterialPriorityIds( uint8_t *pIds, int nCapacity ) const;

	EA_FORCE_INLINE RID getHeightMapsRid( void ) const { return m_generatedHeightMaps.getRid(); }
	EA_FORCE_INLINE RID getControlMapsRid( void ) const { return m_generatedControlMaps.getRid(); }
	EA_FORCE_INLINE RID getColorMapsRid( void ) const { return m_generatedColorMaps.getRid(); }
	EA_FORCE_INLINE RID getDerivedMapsRid( void ) const { return m_generatedDerivedMaps.getRid(); }
	EA_FORCE_INLINE RID getMiddleCompositeMapsRid( void ) const { return m_generatedMiddleCompositeMaps.getRid(); }
	TypedArray<Vector2i> getRenderRegionSlots( void ) const;
	int getSlotStateForLocation( const Vector2i &vLocation ) const;
	EA_FORCE_INLINE const uint16_t *getRegionMapData( void ) const { return m_szGpuRegionMap; }

	size_t estimateLayerBytes( void ) const;
	size_t estimateSharedBytes( void ) const;
	size_t getReservedBytes( void ) const;
	size_t getMinimumBytes( void ) const;
	size_t getHardCapBytes( void ) const;
	T3DGpuRegionCacheStats getStats( void ) const;
	Dictionary getStatsDictionary( bool bDetailed ) const;
	EA_FORCE_INLINE void recordPageTableUpdate( void ) { ++m_nGpuRegionPageTableUpdatesLastTick; }

private:
	T3DRegionRepository &m_regionRepository;
	T3DSamplingStorage &m_samplingStorage;
	T3DMapEditService &m_mapEditService;
	T3DRuntimePageStore &m_runtimePageStore;
	T3DCpuRegionCache &m_cpuRegionCache;
	T3DTerrain *m_pTerrain = nullptr;
	T3DCompute *m_pCompute = nullptr;

	Ref<Image> m_pDefaultHeightMap;
	Ref<Image> m_pDefaultControlMap;
	Ref<Image> m_pDefaultColorMap;
	Ref<Image> m_pDefaultDerivedMap;
	bool m_bControlMapsImplicit = true;
	bool m_bColorMapsImplicit = true;

	T3DGeneratedTexture m_generatedHeightMaps;
	T3DGeneratedTexture m_generatedControlMaps;
	T3DGeneratedTexture m_generatedColorMaps;
	T3DGeneratedTexture m_generatedDerivedMaps;
	T3DGeneratedTexture m_generatedMiddleCompositeMaps;

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
	uint64_t m_nGpuRegionServiceUsecLast = 0u;
	uint64_t m_nGpuPreparePublicationUsecLast = 0u;
	uint64_t m_nGpuPrepareEndToEndUsecLast = 0u;
	bool m_bPageTableChangedThisTick = false;

	int calculateGpuRegionCacheCapacity( void ) const;
	int calculateGpuRegionRequiredRadius( void ) const;
	int calculateGpuRegionRequestedPages( void ) const;
	int calculateGpuRegionPhysicalLayers( int nMappedCapacity ) const;
	int calculateAuthoringGpuRegionBudgetMb( void ) const;
	int estimateGpuRegionUploadCalls( const T3DRegion *pRegion, int nLayer ) const;
	size_t estimateGpuRegionUploadBytes( const T3DRegion *pRegion, int nLayer ) const;
	int getGpuRegionTextureUpdateCallsLastTick( void ) const;
	bool rebuildTextures( void );
	int findGpuRegionSlot( const Vector2i &vLocation ) const;
	int findFreeGpuRegionSlot( void ) const;
	void recordGpuRegionDeparture( int nLayer, bool bCountUnusedPrefetch );
	void retireGpuRegionSlot( int nLayer );
	void reclaimRetiredGpuRegionSlots( void );
	bool uploadGpuRegionLayer( int nLayer, const Vector2i &vLocation, t3dmapType_t eMapType );
	int processPendingGpuRegionRepublishes( int nLimit );
	int collectDesiredGpuRegions(
		const T3DResidencyFrameState &rFrame, const Vector2i &rCenter,
		Vector2i *pLocations, int nCapacity, t3dgpuCacheSelectionStats_t &rStats
	);
	Ref<Image> getRegionMapOrDefault( const T3DRegion *pRegion, t3dmapType_t eMapType ) const;
	Ref<Image> makeGpuColorImage( const Ref<Image> &rImage ) const;
	Ref<Image> makeGpuDerivedImage( const Ref<Image> &rImage ) const;
	Ref<Image> makeGpuRenderControlImage( const Vector2i &vLocation ) const;
	Ref<Image> makeMiddleCompositeImage(
		const Vector2i &rLocation, const Ref<Image> &rControl,
		const Ref<Image> &rColor
	) const;
	bool serviceRuntimePages( const T3DResidencyFrameState &rFrame );
	static bool isRuntimePageWanted( void *pContext, const Vector2i &rLocation );
	EA_FORCE_INLINE void notePageTableChanged( void ) { m_bPageTableChangedThisTick = true; }
};

#endif // TERRAIN3D_GPU_REGION_CACHE_H
