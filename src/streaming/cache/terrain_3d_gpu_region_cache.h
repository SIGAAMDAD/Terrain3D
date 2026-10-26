// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_GPU_REGION_CACHE_H
#define TERRAIN3D_GPU_REGION_CACHE_H

#include "Terrain3DTypeSupport.h"
#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_owner.h"
#include "assets/resources/generated_texture.h"
#include "core/config/terrain_3d_compiler.h"
#include "data/model/terrain_3d_region.h"
#include "streaming/policy/terrain_3d_gpu_cache_constants.h"
#include "streaming/policy/terrain_3d_gpu_cache_policy.h"
#include "streaming/residency/terrain_3d_residency_state.h"
#include "streaming/policy/terrain_3d_runtime_profile.h"

#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include "data/editing/terrain_3d_map_edit_service.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "streaming/cache/terrain_3d_cpu_region_cache.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"


// t3dGpuRegionCacheStats_s centralized in Terrain3DTypes.h.


// t3dGpuRegionCacheUpdateResult_s centralized in Terrain3DTypes.h.


// t3dGpuRegionPendingPublication_e centralized in Terrain3DTypes.h.


typedef struct t3dGpuRegionPendingUpload_s {

	uint64_t m_nGeneration = 0u;
	uint64_t m_nLastUseTick = 0u;
	uint64_t m_nLoadedTick = 0u;
	uint64_t m_nVictimLastUseTick = 0u;
	t3dRegionCoord_t m_location = t3dInvalidRegionCoord();
	t3dRegionCoord_t m_victimLocation = t3dInvalidRegionCoord();
	EA_FORCE_INLINE Vector2i getLocation( void ) const {
		return t3dUnpackRegionCoord( m_location );
	}
	EA_FORCE_INLINE Vector2i getVictimLocation( void ) const {
		return t3dUnpackRegionCoord( m_victimLocation );
	}
	EA_FORCE_INLINE void setLocation( const Vector2i &rLocation ) {
		m_location = t3dPackRegionCoord( rLocation );
	}
	EA_FORCE_INLINE void setVictimLocation( const Vector2i &rLocation ) {
		m_victimLocation = t3dPackRegionCoord( rLocation );
	}
	int m_nVictimLayer = -1;
	uint8_t m_nPublication = TERRAIN3D_GPU_PENDING_PUBLICATION_NONE;
	uint8_t m_nPrefetchOrigin = 0u;
} t3dGpuRegionPendingUpload_t;


class Terrain3DGpuRegionCache {
	CLASS_NAME_STATIC( "Terrain3DGpuRegionCache" );

public:
	Terrain3DGpuRegionCache( Terrain3DRegionRepository &rRepository,
	        Terrain3DSamplingStorage &rSamplingStorage,
	        Terrain3DMapEditService &rMapEditService,
	        Terrain3DRuntimePageStore &rRuntimePageStore,
	        Terrain3DCpuRegionCache &rCpuRegionCache );

	EA_FORCE_INLINE void setTerrain( Terrain3DOwner *pTerrain ) {
		m_pTerrain = pTerrain;
	}
	void setDefaultMaps( const Ref<Image> &rHeight,
	        const Ref<Image> &rControl,
	        const Ref<Image> &rColor,
	        const Ref<Image> &rDerived,
	        bool bControlImplicit,
	        bool bColorImplicit );
	bool configure( bool bEnabled, uint64_t nBudgetBytes, int nUploadLayers, int nUploadBudgetMb, int nSparePages, int nRetireFrames );
	void reset( bool bClearTextures );
	t3dGpuRegionCacheUpdateResult_t update(
	        const t3dResidencyFrameState_s &rFrame, bool bForce, int nEffectiveUploadLayers, Terrain3DCompute *pCompute );
	void invalidate( const Vector2i &vLocation );
	bool republish( const Vector2i &vLocation, Terrain3DCompute *pCompute = NULL );
	bool republishAllResidents( Terrain3DCompute *pCompute = NULL );
	bool refreshTerrainShadowMasks( void );
	void queueRepublish( const Vector2i &vLocation );
	bool updateResidentLocation( const Vector2i &vLocation, t3dMapType_t eMapType, Terrain3DCompute *pCompute = NULL );
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
		return ( int )( calculateAuthoringGpuRegionBudgetBytes() / ( uint64_t )( T3D_GPU_CACHE_MIB ) );
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
	EA_FORCE_INLINE RID getTerrainShadowMasksRid( void ) const {
		return m_generatedTerrainShadowMasks.getRid();
	}
	TypedArray<Vector2i> getRenderRegionSlots( void ) const;
	int getSlotStateForLocation( const Vector2i &vLocation ) const;
	EA_FORCE_INLINE const uint16_t *getRegionMapData( void ) const {
		return m_szGpuRegionMap;
	}
	EA_FORCE_INLINE uint64_t getPhysicalPageTableGeneration( void ) const {
		return m_nPhysicalPageTableGeneration;
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
	class t3dGpuRegionLayerUploadState_t {
public:

		int m_nLayer = -1;
		Vector2i m_location;
		t3dMapType_t m_eMapType = REGION_TYPE_MAX;
		bool m_bAllowCookedPage = true;
		Terrain3DRegion *m_pRegion = NULL;
		uint8_t m_nSourceChannelFlags = 0u;
		uint8_t m_nPreviousChannelFlags = 0u;
		uint8_t m_nPreviousState = TERRAIN3D_GPU_SLOT_FREE;
		uint8_t m_nPublishedFlags = 0u;
		bool m_bUploadHeight = false;
		bool m_bUploadControl = false;
		bool m_bUploadColor = false;
		bool m_bCookedPage = false;
		bool m_bDerivedChannelEnabled = false;
		bool m_bMiddleCompositeAllocated = false;
		bool m_bTerrainShadowAllocated = false;
		uint32_t m_nCookedMaterialMask = 1u;
		uint32_t m_nMaterialMask = 1u;
		Ref<Image> m_pHeight;
		Ref<Image> m_pDerived;
		Ref<Image> m_pControl;
		Ref<Image> m_pColor;
		Ref<Image> m_pMiddleComposite;
		Ref<Image> m_pTerrainShadowMask;
	};
	Terrain3DRegionRepository &m_regionRepository;
	Terrain3DSamplingStorage &m_samplingStorage;
	Terrain3DMapEditService &m_mapEditService;
	Terrain3DRuntimePageStore &m_runtimePageStore;
	Terrain3DCpuRegionCache &m_cpuRegionCache;
	Terrain3DOwner *m_pTerrain = NULL;
	Terrain3DCompute *m_pCompute = NULL;

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
	Terrain3DGeneratedTexture m_generatedTerrainShadowMasks;

	bool m_bGpuRegionCacheEnabled = false;
	bool m_bGpuRegionCacheTexturesReady = false;
	bool m_bGpuRegionCacheSelectionDirty = true;
	uint64_t m_nGpuRegionCacheBudgetBytes = 16u * ( uint64_t )( T3D_GPU_CACHE_MIB );
	int m_nGpuRegionUploadBudgetLayers = 4;
	int m_nGpuRegionUploadBudgetMb = 2;
	uint64_t m_nGpuRegionUploadByteTokens = 0u;
	uint64_t m_nGpuRegionUploadCallTokens = 0u;
	uint64_t m_nGpuRegionUploadTokenRefillUsec = 0u;
	int m_nGpuRegionSparePages = T3D_GPU_CACHE_DEFAULT_SPARE_LAYERS;
	int m_nGpuRegionRetireFrames = T3D_GPU_CACHE_DEFAULT_RETIRE_FRAMES;
	int m_nGpuRegionCacheCapacity = 0;
	int m_nGpuRegionPhysicalLayerCount = 0;
	int m_nGpuRegionSpareLayerCount = 0;
	int m_nGpuRegionResidentCount = 0;
	int m_nGpuRegionRetiredCount = 0;
	uint8_t m_szGpuRegionRepublishPending[T3D_REGION_SLOTS] = {};
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
	int m_nGpuRegionMiddleCompositeUpdatesLastTick = 0;
	int m_nGpuRegionTerrainShadowUpdatesLastTick = 0;
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
	size_t m_nGpuRegionMiddleCompositeBytesLastTick = 0u;
	size_t m_nGpuRegionTerrainShadowBytesLastTick = 0u;
	uint64_t m_nGpuRegionTick = 0u;
	uint64_t m_nGpuRegionLastPublicationTick = 0u;
	Vector2i m_gpuRegionCacheCenter = s_v2iMax;
	Vector2i m_gpuRegionPredictionDirection = s_v2iZero;
	uint64_t m_nGpuRegionStreamingGenerationLast = 0u;
	t3dRegionCoord_t m_szGpuRegionPriorityLocations[T3D_GPU_CACHE_MAX_LAYERS] = {};
	uint8_t m_szGpuRegionPriorityClasses[T3D_GPU_CACHE_MAX_LAYERS] = {};
	int m_nGpuRegionPriorityCount = 0;
	t3dRegionCoord_t m_szGpuRegionLocations[T3D_GPU_CACHE_MAX_LAYERS] = {};
	uint64_t m_szGpuRegionLastUseTick[T3D_GPU_CACHE_MAX_LAYERS] = {};
	uint64_t m_szGpuRegionLoadedTick[T3D_GPU_CACHE_MAX_LAYERS] = {};
	uint64_t m_szGpuRegionRetireAfterFrame[T3D_GPU_CACHE_MAX_LAYERS] = {};
	uint8_t m_szGpuRegionSlotStates[T3D_GPU_CACHE_MAX_LAYERS] = {};
	uint8_t m_szGpuRegionPrefetchOrigin[T3D_GPU_CACHE_MAX_LAYERS] = {};
	uint8_t m_szGpuRegionLayerChannelFlags[T3D_GPU_CACHE_MAX_LAYERS] = {};
	uint32_t m_szGpuRegionMaterialMasks[T3D_GPU_CACHE_MAX_LAYERS] = {};
	t3dGpuRegionPendingUpload_t m_szGpuRegionPendingUploads[T3D_GPU_CACHE_MAX_LAYERS];
	uint8_t m_szGpuRegionVictimReserved[T3D_GPU_CACHE_MAX_LAYERS] = {};
	int m_nGpuRegionPendingUnmappedReservations = 0;
	uint64_t m_nGpuRegionPrefetchUsedTotal = 0u;
	uint64_t m_nGpuRegionPrefetchDiscardedTotal = 0u;
	uint64_t m_nGpuRegionResidentTicksTotal = 0u;
	uint64_t m_nGpuRegionDepartedPagesTotal = 0u;
	uint16_t m_szGpuRegionMap[T3D_REGION_SLOTS] = {};
	uint64_t m_nPhysicalPageTableGeneration = 0u;
	TypedArray<Image> m_gpuHeightMaps;
	TypedArray<Image> m_gpuControlMaps;
	TypedArray<Image> m_gpuColorMaps;
	TypedArray<Image> m_gpuDerivedMaps;
	TypedArray<Image> m_gpuMiddleCompositeMaps;
	TypedArray<Image> m_gpuTerrainShadowMasks;
	uint64_t m_nMiddleCompositeMaterialSignature = UINT64_MAX;
	PackedColorArray m_middleCompositeMaterialColors;
	PackedFloat32Array m_middleCompositeMaterialScales;
	Ref<Image> m_middleCompositeMaterialImages[T3D_MAX_TEXTURE_ASSETS];
	PackedByteArray m_middleCompositeScratch[T3D_GPU_CACHE_MAX_LAYERS];
	int m_nMiddleCompositeScratchBytes = 0;
	int m_nMiddleCompositeScratchLayerCount = 0;
	uint64_t m_nGpuRegionServiceUsecLast = 0u;
	uint64_t m_nGpuPreparePublicationUsecLast = 0u;
	uint64_t m_nGpuPrepareEndToEndUsecLast = 0u;
	bool m_bPageTableChangedThisTick = false;

	int calculateGpuRegionCacheCapacity( void ) const;
	int calculateGpuRegionRequiredRadius( void ) const;
	int calculateGpuRegionRequestedPages( void ) const;
	int calculateGpuRegionPhysicalLayers( int nMappedCapacity ) const;
	uint64_t calculateAuthoringGpuRegionBudgetBytes( void ) const;
	int estimateGpuRegionUploadCalls( const Terrain3DRegion *pRegion, int nLayer ) const;
	size_t estimateGpuRegionUploadBytes( const Terrain3DRegion *pRegion, int nLayer ) const;
	int getGpuRegionTextureUpdateCallsLastTick( void ) const;
	bool rebuildTextures( void );
	int findGpuRegionSlot( const Vector2i &vLocation ) const;
	int findPendingGpuRegionUpload( const Vector2i &vLocation ) const;
	int findFreeGpuRegionSlot( void ) const;
	void recordGpuRegionDeparture( int nLayer, bool bCountUnusedPrefetch );
	void retireGpuRegionSlot( int nLayer );
	void reclaimRetiredGpuRegionSlots( void );
	void clearPendingGpuRegionUpload( int nLayer );
	void finalizePendingGpuRegionUploads( void );
	bool beginGpuRegionLayerUpload( t3dGpuRegionLayerUploadState_t &rState, uint64_t &rPreparedRequestUsec );
	bool resolveGpuRegionLayerImages( t3dGpuRegionLayerUploadState_t &rState );
	void resolveGpuRegionLayerMetadata( t3dGpuRegionLayerUploadState_t &rState ) const;
	bool submitGpuRegionLayerTrackedBatch( t3dGpuRegionLayerUploadState_t &rState );
	bool uploadGpuRegionLayerDirect( t3dGpuRegionLayerUploadState_t &rState );
	bool uploadGpuRegionLayer( int nLayer, const Vector2i &vLocation, t3dMapType_t eMapType, bool bAllowCookedPage = true );
	void refillGpuUploadTokens( int nCallBurst );
	int processPendingGpuRegionRepublishes( int nLimit );
	int collectDesiredGpuRegions( const t3dResidencyFrameState_s &rFrame,
	        const Vector2i &rCenter,
	        Vector2i *pLocations,
	        int nCapacity,
	        t3dGpuCacheSelectionStats_t &rStats );
	Ref<Image> getRegionMapOrDefault( const Terrain3DRegion *pRegion, t3dMapType_t eMapType ) const;
	Ref<Image> makeGpuColorImage( const Ref<Image> &rImage ) const;
	Ref<Image> makeGpuDerivedImage( const Ref<Image> &rImage ) const;
	Ref<Image> makeTerrainShadowMaskImage( const Terrain3DRegion *pRegion ) const;
	Ref<Image> makeGpuRenderControlImage( const Vector2i &vLocation ) const;
	bool refreshMiddleCompositeSources( uint64_t nMaterialSignature, int nWidth, int nHeight );
	Ref<Image> makeMiddleCompositeImage(
	        const Vector2i &rLocation, const Ref<Image> &rControl, const Ref<Image> &rColor, int nScratchSlot );
	bool serviceRuntimePages( const t3dResidencyFrameState_s &rFrame );
	bool validatePublishedMappings( void ) const;
	void notePageTableChanged( void );
};

#endif // TERRAIN3D_GPU_REGION_CACHE_H
