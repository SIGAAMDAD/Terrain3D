// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_GPU_REGION_CACHE_H
#define TERRAIN3D_GPU_REGION_CACHE_H

#include "Terrain3DConstants.h"
#include "assets/generated_texture.h"
#include "core/terrain_3d_compiler.h"
#include "data/terrain_3d_region.h"
#include "streaming/terrain_3d_gpu_cache_constants.h"
#include "streaming/terrain_3d_gpu_cache_policy.h"
#include "streaming/terrain_3d_residency_state.h"
#include "streaming/terrain_3d_runtime_profile.h"

#include <cstddef>
#include <cstdint>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>

class Terrain3DCompute;
class Terrain3DCpuRegionCache;
class Terrain3DMapEditService;
class Terrain3DRegion;
class Terrain3DRegionRepository;
class Terrain3DRuntimePageStore;
class Terrain3DSamplingStorage;
class Terrain3D;

typedef struct t3dGpuRegionCacheStats_s {
	bool m_bEnabled = false;
	bool m_bTexturesReady = false;
	bool m_bSelectionPending = true;
	bool m_bCameraMovedLastTick = false;
	int m_nBudgetMb = 0;
	int m_nCapacity = 0;
	int m_nPhysicalLayers = 0;
	int m_nSpareLayers = 0;
	int m_nResident = 0;
	int m_nRetired = 0;
	int m_nDesiredLastTick = 0;
	int m_nRequiredLastTick = 0;
	int m_nGuardLastTick = 0;
	int m_nPrefetchLastTick = 0;
	int m_nHysteresisLastTick = 0;
	int m_nMissingRequiredLastTick = 0;
	int m_nRequestedPages = 0;
	int m_nRequiredRadius = 0;
	int m_nUploadsLastTick = 0;
	int m_nEvictionsLastTick = 0;
	int m_nCookedHitsLastTick = 0;
	int m_nCookedMissesLastTick = 0;
	int m_nRuntimePreparesLastTick = 0;
	int m_nReusedLayersLastTick = 0;
	int m_nPageTableUpdatesLastTick = 0;
	int m_nHeightUpdatesLastTick = 0;
	int m_nControlUpdatesLastTick = 0;
	int m_nColorUpdatesLastTick = 0;
	int m_nDerivedUpdatesLastTick = 0;
	int m_nRuntimeColorMipsLastTick = 0;
	int m_nRuntimeDerivedBuildsLastTick = 0;
	int m_nPreparedPagesLastTick = 0;
	int m_nPrepareWorkersLastTick = 0;
	int m_nEffectiveUploadLimitLastTick = 0;
	uint32_t m_nStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	size_t m_nUploadBytesLastTick = 0u;
	size_t m_nHeightBytesLastTick = 0u;
	size_t m_nControlBytesLastTick = 0u;
	size_t m_nColorBytesLastTick = 0u;
	size_t m_nDerivedBytesLastTick = 0u;
	uint64_t m_nServiceUsecLast = 0u;
	uint64_t m_nPreparePublicationUsecLast = 0u;
	uint64_t m_nPrepareEndToEndUsecLast = 0u;
} t3dGpuRegionCacheStats_t;

typedef struct t3dGpuRegionCacheUpdateResult_s {
	bool m_bPageTableChanged = false;
} t3dGpuRegionCacheUpdateResult_t;

class Terrain3DGpuRegionCache {
	CLASS_NAME_STATIC( "Terrain3DGpuRegionCache" );

public:
	Terrain3DGpuRegionCache( Terrain3DRegionRepository &rRepository,
			Terrain3DSamplingStorage &rSamplingStorage,
			Terrain3DMapEditService &rMapEditService,
			Terrain3DRuntimePageStore &rRuntimePageStore,
			Terrain3DCpuRegionCache &rCpuRegionCache );

	EA_FORCE_INLINE void setTerrain( Terrain3D *pTerrain ) {
		m_pTerrain = pTerrain;
	}
	void setDefaultMaps( const Ref<Image> &rHeight,
			const Ref<Image> &rControl,
			const Ref<Image> &rColor,
			const Ref<Image> &rDerived,
			bool bControlImplicit,
			bool bColorImplicit );
	bool configure( bool bEnabled,
			int nBudgetMb,
			int nUploadLayers,
			int nUploadBudgetMb,
			int nSparePages,
			int nRetireFrames );
	void reset( bool bClearTextures );
	t3dGpuRegionCacheUpdateResult_t
	update( const t3dResidencyFrameState_t &rFrame, bool bForce, int nEffectiveUploadLayers, Terrain3DCompute *pCompute );
	void invalidate( const Vector2i &vLocation );
	bool republish( const Vector2i &vLocation, Terrain3DCompute *pCompute = nullptr );
	bool republishAllResidents( Terrain3DCompute *pCompute = nullptr );
	void queueRepublish( const Vector2i &vLocation );
	bool updateResidentLocation( const Vector2i &vLocation, t3dMapType_t eMapType, Terrain3DCompute *pCompute = nullptr );
	void armPendingRetirements( void );
	EA_FORCE_INLINE void markSelectionDirty( void ) {
		m_bGpuRegionCacheSelectionDirty = true;
	}

	EA_FORCE_INLINE bool isEnabled( void ) const {
		return m_bGpuRegionCacheEnabled;
	}
	EA_FORCE_INLINE bool isTexturesReady( void ) const {
		return m_bGpuRegionCacheTexturesReady;
	}
	EA_FORCE_INLINE bool isSelectionDirty( void ) const {
		return m_bGpuRegionCacheSelectionDirty;
	}
	EA_FORCE_INLINE bool isResident( const Vector2i &vLocation ) const {
		return findGpuRegionSlot( vLocation ) >= 0;
	}
	EA_FORCE_INLINE int getLayerForLocation( const Vector2i &vLocation ) const {
		return findGpuRegionSlot( vLocation );
	}
	EA_FORCE_INLINE int getCapacity( void ) const {
		return m_nGpuRegionCacheCapacity;
	}
	EA_FORCE_INLINE int getPhysicalLayerCount( void ) const {
		return m_nGpuRegionPhysicalLayerCount;
	}
	EA_FORCE_INLINE int getResidentCount( void ) const {
		return m_nGpuRegionResidentCount;
	}
	EA_FORCE_INLINE int getRequiredRadius( void ) const {
		return calculateGpuRegionRequiredRadius();
	}
	EA_FORCE_INLINE int getAuthoringBudgetMb( void ) const {
		return calculateAuthoringGpuRegionBudgetMb();
	}
	EA_FORCE_INLINE int getUploadBudgetLayers( void ) const {
		return m_nGpuRegionUploadBudgetLayers;
	}
	EA_FORCE_INLINE uint32_t getStallFlagsLastTick( void ) const {
		return m_nGpuRegionStallFlagsLastTick;
	}
	bool isColorChannelEnabled( void ) const;
	bool isDerivedChannelEnabled( void ) const;
	int calculateRequiredCorePagesForLods( int nDetailLods ) const;
	int calculateCapacityForLods( int nDetailLods ) const;
	bool isAuthoringFullResidencyPossible( void ) const;
	bool isAuthoringReadbackSafe( void ) const;
	int fillMaterialPriorityIds( uint8_t *pIds, int nCapacity ) const;

	EA_FORCE_INLINE RID getHeightMapsRid( void ) const {
		return m_generatedHeightMaps.getRid();
	}
	EA_FORCE_INLINE RID getControlMapsRid( void ) const {
		return m_generatedControlMaps.getRid();
	}
	EA_FORCE_INLINE RID getColorMapsRid( void ) const {
		return m_generatedColorMaps.getRid();
	}
	EA_FORCE_INLINE RID getDerivedMapsRid( void ) const {
		return m_generatedDerivedMaps.getRid();
	}
	EA_FORCE_INLINE RID getMiddleCompositeMapsRid( void ) const {
		return m_generatedMiddleCompositeMaps.getRid();
	}
	TypedArray<Vector2i> getRenderRegionSlots( void ) const;
	int getSlotStateForLocation( const Vector2i &vLocation ) const;
	EA_FORCE_INLINE const uint16_t *getRegionMapData( void ) const {
		return m_szGpuRegionMap;
	}

	size_t estimateLayerBytes( void ) const;
	size_t estimateSharedBytes( void ) const;
	size_t getReservedBytes( void ) const;
	size_t getMinimumBytes( void ) const;
	size_t getHardCapBytes( void ) const;
	t3dGpuRegionCacheStats_t getStats( void ) const;
	Dictionary getStatsDictionary( bool bDetailed ) const;
	EA_FORCE_INLINE void recordPageTableUpdate( void ) {
		++m_nGpuRegionPageTableUpdatesLastTick;
	}

private:
	Terrain3DRegionRepository &m_regionRepository;
	Terrain3DSamplingStorage &m_samplingStorage;
	Terrain3DMapEditService &m_mapEditService;
	Terrain3DRuntimePageStore &m_runtimePageStore;
	Terrain3DCpuRegionCache &m_cpuRegionCache;
	Terrain3D *m_pTerrain = nullptr;
	Terrain3DCompute *m_pCompute = nullptr;

	Ref<Image> m_defaultHeightMap;
	Ref<Image> m_defaultControlMap;
	Ref<Image> m_defaultColorMap;
	Ref<Image> m_defaultDerivedMap;
	bool m_bControlMapsImplicit = true;
	bool m_bColorMapsImplicit = true;

	Terrain3DGeneratedTexture m_generatedHeightMaps;
	Terrain3DGeneratedTexture m_generatedControlMaps;
	Terrain3DGeneratedTexture m_generatedColorMaps;
	Terrain3DGeneratedTexture m_generatedDerivedMaps;
	Terrain3DGeneratedTexture m_generatedMiddleCompositeMaps;

	bool m_bGpuRegionCacheEnabled = false;
	bool m_bGpuRegionCacheTexturesReady = false;
	bool m_bGpuRegionCacheSelectionDirty = true;
	int m_nGpuRegionCacheBudgetMb = 16;
	int m_nGpuRegionUploadBudgetLayers = 4;
	int m_nGpuRegionUploadBudgetMb = 2;
	int m_nGpuRegionSparePages = s_nGpuCacheDefaultSpareLayers;
	int m_nGpuRegionRetireFrames = s_nGpuCacheDefaultRetireFrames;
	int m_nGpuRegionCacheCapacity = 0;
	int m_nGpuRegionPhysicalLayerCount = 0;
	int m_nGpuRegionSpareLayerCount = 0;
	int m_nGpuRegionResidentCount = 0;
	int m_nGpuRegionRetiredCount = 0;
	uint8_t m_szGpuRegionRepublishPending[s_nMaxRegionSlots] = {};
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
	Vector2i m_gpuRegionCacheCenter = s_v2iMax;
	Vector2i m_gpuRegionPredictionDirection = s_v2iZero;
	uint64_t m_nGpuRegionStreamingGenerationLast = 0u;
	Vector2i m_szGpuRegionPriorityLocations[s_nGpuCacheMaxLayers] = {};
	uint8_t m_szGpuRegionPriorityClasses[s_nGpuCacheMaxLayers] = {};
	int m_nGpuRegionPriorityCount = 0;
	Vector2i m_szGpuRegionLocations[s_nGpuCacheMaxLayers] = {};
	uint64_t m_szGpuRegionLastUseTick[s_nGpuCacheMaxLayers] = {};
	uint64_t m_szGpuRegionLoadedTick[s_nGpuCacheMaxLayers] = {};
	uint64_t m_szGpuRegionRetireAfterFrame[s_nGpuCacheMaxLayers] = {};
	uint8_t m_szGpuRegionSlotStates[s_nGpuCacheMaxLayers] = {};
	uint8_t m_szGpuRegionPrefetchOrigin[s_nGpuCacheMaxLayers] = {};
	uint8_t m_szGpuRegionLayerChannelFlags[s_nGpuCacheMaxLayers] = {};
	uint32_t m_szGpuRegionMaterialMasks[s_nGpuCacheMaxLayers] = {};
	uint64_t m_nGpuRegionPrefetchUsedTotal = 0u;
	uint64_t m_nGpuRegionPrefetchDiscardedTotal = 0u;
	uint64_t m_nGpuRegionResidentTicksTotal = 0u;
	uint64_t m_nGpuRegionDepartedPagesTotal = 0u;
	uint16_t m_szGpuRegionMap[s_nMaxRegionSlots] = {};
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
	int estimateGpuRegionUploadCalls( const Terrain3DRegion *pRegion, int nLayer ) const;
	size_t estimateGpuRegionUploadBytes( const Terrain3DRegion *pRegion, int nLayer ) const;
	int getGpuRegionTextureUpdateCallsLastTick( void ) const;
	bool rebuildTextures( void );
	int findGpuRegionSlot( const Vector2i &vLocation ) const;
	int findFreeGpuRegionSlot( void ) const;
	void recordGpuRegionDeparture( int nLayer, bool bCountUnusedPrefetch );
	void retireGpuRegionSlot( int nLayer );
	void reclaimRetiredGpuRegionSlots( void );
	bool uploadGpuRegionLayer( int nLayer, const Vector2i &vLocation, t3dMapType_t eMapType );
	int processPendingGpuRegionRepublishes( int nLimit );
	int collectDesiredGpuRegions( const t3dResidencyFrameState_t &rFrame,
			const Vector2i &rCenter,
			Vector2i *pLocations,
			int nCapacity,
			t3dGpuCacheSelectionStats_t &rStats );
	Ref<Image> getRegionMapOrDefault( const Terrain3DRegion *pRegion, t3dMapType_t eMapType ) const;
	Ref<Image> makeGpuColorImage( const Ref<Image> &rImage ) const;
	Ref<Image> makeGpuDerivedImage( const Ref<Image> &rImage ) const;
	Ref<Image> makeGpuRenderControlImage( const Vector2i &vLocation ) const;
	Ref<Image>
	makeMiddleCompositeImage( const Vector2i &rLocation, const Ref<Image> &rControl, const Ref<Image> &rColor ) const;
	bool serviceRuntimePages( const t3dResidencyFrameState_t &rFrame );
	static bool isRuntimePageWanted( void *pContext, const Vector2i &rLocation );
	EA_FORCE_INLINE void notePageTableChanged( void ) {
		m_bPageTableChangedThisTick = true;
	}
};

#endif // TERRAIN3D_GPU_REGION_CACHE_H
