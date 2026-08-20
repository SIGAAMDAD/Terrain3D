// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_MIDDLE_HEIGHT_REGION_CACHE_H
#define TERRAIN3D_MIDDLE_HEIGHT_REGION_CACHE_H

#include "Terrain3DConstants.h"
#include "assets/generated_texture.h"
#include "streaming/terrain_3d_gpu_cache_constants.h"
#include "streaming/terrain_3d_residency_state.h"

#include <godot_cpp/variant/dictionary.hpp>

#include <cstddef>
#include <cstdint>

class T3DRegionRepository;
class T3DRuntimePageStore;
class T3DSamplingStorage;

struct T3DMiddleHeightRegionCacheConfig {
	bool bAuthoring = false;
	int nHotRadius = 0;
	int nResolutionDivisor = 1;
	int nRetireFrames = TERRAIN3D_GPU_CACHE_DEFAULT_RETIRE_FRAMES;
};

struct T3DMiddleHeightRegionCacheStats {
	int nResident = 0;
	int nMappedCapacity = 0;
	int nPhysicalLayers = 0;
	int nDesiredLastTick = 0;
	int nMissingRequiredLastTick = 0;
	int nUploadsLastTick = 0;
	int nEvictionsLastTick = 0;
	int nResolutionDivisor = 1;
	size_t nReservedBytes = 0u;
	bool bPageTableValid = false;
	bool bHeightArrayValid = false;
};

class T3DMiddleHeightRegionCache {
public:
	T3DMiddleHeightRegionCache(
		T3DRegionRepository *pRepository,
		T3DSamplingStorage *pSamplingStorage,
		T3DRuntimePageStore *pRuntimePageStore
	);

	void reset( void );
	bool configure( const T3DMiddleHeightRegionCacheConfig &rConfig );
	void update( const T3DResidencyFrameState &rFrame, bool bForce, int nEffectiveUploadLayers,
		const T3DMiddleHeightRegionCacheConfig &rConfig );
	void invalidate( const Vector2i &vLocation );
	void markSelectionDirty( void ) { m_bSelectionDirty = true; }

	RID getHeightMapsRid( void ) const { return m_generatedHeightMaps.getRid(); }
	RID getRegionMapRid( void ) const { return m_generatedRegionMap.getRid(); }
	int getResidentCount( void ) const { return m_nResidentCount; }
	int getMissingRequiredLastTick( void ) const { return m_nMissingRequiredLastTick; }
	bool isResident( const Vector2i &vLocation ) const { return findSlot( vLocation ) >= 0; }
	bool isReady( void ) const { return m_bTexturesReady; }
	bool isSelectionDirty( void ) const { return m_bSelectionDirty; }
	Ref<Image> makeHeightImage( const Vector2i &vLocation, bool bRuntimeResolution = false ) const;
	T3DMiddleHeightRegionCacheStats getStats( void ) const;
	Dictionary getStatsDictionary( void ) const;
	size_t getReservedBytes( void ) const;

private:
	T3DRegionRepository *m_pRepository = nullptr;
	T3DSamplingStorage *m_pSamplingStorage = nullptr;
	T3DRuntimePageStore *m_pRuntimePageStore = nullptr;
	T3DMiddleHeightRegionCacheConfig m_config;

	T3DGeneratedTexture m_generatedHeightMaps;
	T3DGeneratedTexture m_generatedRegionMap;
	uint16_t m_szRegionMap[TERRAIN3D_MAX_REGION_SLOTS] = {};
	Vector2i m_szLocations[TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS] = {};
	uint64_t m_szLastUseTick[TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS] = {};
	uint64_t m_szRetireAfterFrame[TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS] = {};
	uint8_t m_szSlotStates[TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS] = {};
	int m_nPhysicalLayerCount = 0;
	int m_nMappedCapacity = 0;
	int m_nResidentCount = 0;
	int m_nDesiredLastTick = 0;
	int m_nMissingRequiredLastTick = 0;
	int m_nUploadsLastTick = 0;
	int m_nEvictionsLastTick = 0;
	uint64_t m_nTick = 0u;
	Vector2i m_center = V2I_MAX;
	bool m_bTexturesReady = false;
	bool m_bPageTableDirty = true;
	bool m_bSelectionDirty = true;
	uint64_t m_nStreamingGenerationLast = 0u;

	int findSlot( const Vector2i &vLocation ) const;
	bool uploadLayer( int nLayer, const Vector2i &vLocation, const Ref<Image> &rPreparedHeight = Ref<Image>() );
	int collectDesired( const T3DResidencyFrameState &rFrame, const Vector2i &rCenter,
		Vector2i *pLocations, int nCapacity, int &rRequiredCount ) const;
	bool updatePageTableTexture( void );
};

#endif // TERRAIN3D_MIDDLE_HEIGHT_REGION_CACHE_H
