// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_MIDDLE_HEIGHT_REGION_CACHE_H
#define TERRAIN3D_MIDDLE_HEIGHT_REGION_CACHE_H

#include "Terrain3DConstants.h"
#include "assets/generated_texture.h"
#include "core/terrain_3d_compiler.h"
#include "streaming/terrain_3d_gpu_cache_constants.h"
#include "streaming/terrain_3d_residency_state.h"

#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/variant/dictionary.hpp>

class Terrain3DRegionRepository;
class Terrain3DRuntimePageStore;
class Terrain3DSamplingStorage;

typedef struct t3dMiddleHeightRegionCacheConfig_s {
	bool m_bAuthoring = false;
	int m_nHotRadius = 0;
	int m_nResolutionDivisor = 1;
	int m_nRetireFrames = s_nGpuCacheDefaultRetireFrames;
} t3dMiddleHeightRegionCacheConfig_t;

typedef struct t3dMiddleHeightRegionCacheStats_s {
	int m_nResident = 0;
	int m_nMappedCapacity = 0;
	int m_nPhysicalLayers = 0;
	int m_nDesiredLastTick = 0;
	int m_nMissingRequiredLastTick = 0;
	int m_nUploadsLastTick = 0;
	int m_nEvictionsLastTick = 0;
	int m_nResolutionDivisor = 1;
	size_t m_nReservedBytes = 0u;
	bool m_bPageTableValid = false;
	bool m_bHeightArrayValid = false;
} t3dMiddleHeightRegionCacheStats_t;

class Terrain3DMiddleHeightRegionCache {
public:
	Terrain3DMiddleHeightRegionCache( Terrain3DRegionRepository *pRepository,
	        Terrain3DSamplingStorage *pSamplingStorage,
	        Terrain3DRuntimePageStore *pRuntimePageStore );

	void reset( void );
	bool configure( const t3dMiddleHeightRegionCacheConfig_t &rConfig );
	void update( const t3dResidencyFrameState_t &rFrame,
	        bool bForce,
	        int nEffectiveUploadLayers,
	        const t3dMiddleHeightRegionCacheConfig_t &rConfig );
	void invalidate( const Vector2i &vLocation );
	EA_FORCE_INLINE void markSelectionDirty( void ) {
		m_bSelectionDirty = true;
	}

	EA_FORCE_INLINE RID getHeightMapsRid( void ) const {
		return m_generatedHeightMaps.getRid();
	}
	EA_FORCE_INLINE RID getRegionMapRid( void ) const {
		return m_generatedRegionMap.getRid();
	}
	EA_FORCE_INLINE int getResidentCount( void ) const {
		return m_nResidentCount;
	}
	EA_FORCE_INLINE int getMissingRequiredLastTick( void ) const {
		return m_nMissingRequiredLastTick;
	}
	EA_FORCE_INLINE bool isResident( const Vector2i &vLocation ) const {
		return findSlot( vLocation ) >= 0;
	}
	EA_FORCE_INLINE bool isReady( void ) const {
		return m_bTexturesReady;
	}
	EA_FORCE_INLINE bool isSelectionDirty( void ) const {
		return m_bSelectionDirty;
	}
	Ref<Image> makeHeightImage( const Vector2i &vLocation, bool bRuntimeResolution = false ) const;
	t3dMiddleHeightRegionCacheStats_t getStats( void ) const;
	Dictionary getStatsDictionary( void ) const;
	size_t getReservedBytes( void ) const;

private:
	Terrain3DRegionRepository *m_pRepository = NULL;
	Terrain3DSamplingStorage *m_pSamplingStorage = NULL;
	Terrain3DRuntimePageStore *m_pRuntimePageStore = NULL;
	t3dMiddleHeightRegionCacheConfig_t m_config;

	Terrain3DGeneratedTexture m_generatedHeightMaps;
	Terrain3DGeneratedTexture m_generatedRegionMap;
	uint16_t m_szRegionMap[s_nMaxRegionSlots] = {};
	Vector2i m_szLocations[s_nGpuHeightCacheMaxLayers] = {};
	uint64_t m_szLastUseTick[s_nGpuHeightCacheMaxLayers] = {};
	uint64_t m_szRetireAfterFrame[s_nGpuHeightCacheMaxLayers] = {};
	uint8_t m_szSlotStates[s_nGpuHeightCacheMaxLayers] = {};
	int m_nPhysicalLayerCount = 0;
	int m_nMappedCapacity = 0;
	int m_nResidentCount = 0;
	int m_nDesiredLastTick = 0;
	int m_nMissingRequiredLastTick = 0;
	int m_nUploadsLastTick = 0;
	int m_nEvictionsLastTick = 0;
	uint64_t m_nTick = 0u;
	Vector2i m_center = s_v2iMax;
	bool m_bTexturesReady = false;
	bool m_bPageTableDirty = true;
	bool m_bSelectionDirty = true;
	uint64_t m_nStreamingGenerationLast = 0u;

	int findSlot( const Vector2i &vLocation ) const;
	bool uploadLayer( int nLayer, const Vector2i &vLocation, const Ref<Image> &rPreparedHeight = Ref<Image>() );
	int collectDesired( const t3dResidencyFrameState_t &rFrame,
	        const Vector2i &rCenter,
	        Vector2i *pLocations,
	        int nCapacity,
	        int &rRequiredCount ) const;
	bool updatePageTableTexture( void );
};

#endif // TERRAIN3D_MIDDLE_HEIGHT_REGION_CACHE_H
