// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_FAR_SURFACE_ATLAS_CACHE_H
#define TERRAIN3D_FAR_SURFACE_ATLAS_CACHE_H

#include "Terrain3DConstants.h"
#include "assets/generated_texture.h"
#include "streaming/terrain_3d_gpu_cache_constants.h"

#include "eastl_config.h"
#include <EASTL/bitset.h>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstddef>
#include <cstdint>

class Terrain3DAssets;
class T3DCpuRegionCache;
class Terrain3DMaterial;
class Terrain3DRegion;
class T3DRegionRepository;
class T3DSamplingStorage;

struct T3DFarSurfaceAtlasConfig {
	bool bRequired = false;
	bool bStrictCooked = false;
	int nHeightAtlasSide = TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE;
	int nSurfaceAtlasSide = TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE;
	String dataDirectory;
	Ref<Image> pDefaultHeightMap;
	Ref<Image> pDefaultControlMap;
	Ref<Image> pDefaultColorMap;
	Ref<Terrain3DMaterial> pMaterial;
	Ref<Terrain3DAssets> pAssets;
};

struct T3DFarSurfaceAtlasStats {
	int nHeightUpdatesLastTick = 0;
	int nColorUpdatesLastTick = 0;
	int nHeightRegionsLastTick = 0;
	int nColorRegionsLastTick = 0;
	int nHeightDirtyRegions = 0;
	int nColorDirtyRegions = 0;
	uint64_t nHeightProcessingUsecLast = 0u;
	uint64_t nColorProcessingUsecLast = 0u;
	uint64_t nHeightDirtyTexelsLast = 0u;
	uint64_t nColorDirtyTexelsLast = 0u;
	uint64_t nHeightRebuiltBytesLast = 0u;
	uint64_t nColorRebuiltBytesLast = 0u;
	uint64_t nHeightUploadedBytesLast = 0u;
	uint64_t nColorUploadedBytesLast = 0u;
	uint64_t nHeightPublicationUsecLast = 0u;
	uint64_t nColorPublicationUsecLast = 0u;
	uint64_t nHeightPublicationCount = 0u;
	uint64_t nColorPublicationCount = 0u;
	uint64_t nHeightUploadedBytesTotal = 0u;
	uint64_t nColorUploadedBytesTotal = 0u;
	uint64_t nCookIoUsecLast = 0u;
	bool bHeightDirtyAll = true;
	bool bColorDirtyAll = true;
	bool bCookedLoaded = false;
	bool bCookedSaved = false;
};

class T3DFarSurfaceAtlasCache {
public:
	T3DFarSurfaceAtlasCache(
		T3DRegionRepository *pRepository,
		T3DSamplingStorage *pSamplingStorage,
		T3DCpuRegionCache *pCpuRegionCache
	);

	void setConfig( const T3DFarSurfaceAtlasConfig &rConfig );
	void reset( void );
	void resetFrameStats( void );
	void invalidateSurface( void );
	void invalidateLayout( void );
	void markAllDirty( t3dmapType_t eMapType );
	void markRegionDirty( const Vector2i &vLocation, t3dmapType_t eMapType );
	bool ensure( void );
	bool writeCooked( const String &rDirectory = String() );

	RID getHeightMapRid( void ) const { return m_generatedHeightMap.getRid(); }
	RID getColorMapRid( void ) const { return m_generatedColorMap.getRid(); }
	bool isReady( void ) const { return m_generatedHeightMap.getRid().is_valid() && m_generatedColorMap.getRid().is_valid(); }
	bool isRegionHeightDirty( const Vector2i &vLocation ) const;
	bool isRegionColorDirty( const Vector2i &vLocation ) const;
	int getDirtyRegionCount( void ) const;
	size_t getImageBytes( void ) const;
	T3DFarSurfaceAtlasStats getStats( void ) const;
	Dictionary getStatsDictionary( void ) const;

private:
	T3DRegionRepository *m_pRepository = nullptr;
	T3DSamplingStorage *m_pSamplingStorage = nullptr;
	T3DCpuRegionCache *m_pCpuRegionCache = nullptr;
	T3DFarSurfaceAtlasConfig m_config;

	T3DGeneratedTexture m_generatedHeightMap;
	T3DGeneratedTexture m_generatedColorMap;
	Ref<Image> m_pHeightImage;
	Ref<Image> m_pColorImage;
	eastl::bitset<TERRAIN3D_MAX_REGION_SLOTS> m_heightDirtyRegions;
	eastl::bitset<TERRAIN3D_MAX_REGION_SLOTS> m_colorDirtyRegions;
	bool m_bHeightDirty = true;
	bool m_bColorDirty = true;
	bool m_bHeightDirtyAll = true;
	bool m_bColorDirtyAll = true;
	bool m_bCookedLoaded = false;
	bool m_bCookedSaved = false;
	int m_nHeightUpdatesLastTick = 0;
	int m_nColorUpdatesLastTick = 0;
	int m_nHeightRegionsLastTick = 0;
	int m_nColorRegionsLastTick = 0;
	uint64_t m_nHeightProcessingUsecLast = 0u;
	uint64_t m_nColorProcessingUsecLast = 0u;
	uint64_t m_nHeightDirtyTexelsLast = 0u;
	uint64_t m_nColorDirtyTexelsLast = 0u;
	uint64_t m_nHeightRebuiltBytesLast = 0u;
	uint64_t m_nColorRebuiltBytesLast = 0u;
	uint64_t m_nHeightUploadedBytesLast = 0u;
	uint64_t m_nColorUploadedBytesLast = 0u;
	uint64_t m_nHeightPublicationUsecLast = 0u;
	uint64_t m_nColorPublicationUsecLast = 0u;
	uint64_t m_nHeightPublicationCount = 0u;
	uint64_t m_nColorPublicationCount = 0u;
	uint64_t m_nHeightUploadedBytesTotal = 0u;
	uint64_t m_nColorUploadedBytesTotal = 0u;
	uint64_t m_nCookIoUsecLast = 0u;

	String getCookedPath( const String &rDirectory = String() ) const;
	uint64_t computeSourceSignature( void ) const;
	bool initializeImages( void );
	bool loadCooked( void );
	bool patchHeightRegion( const Vector2i &vLocation, PackedByteArray &rAtlasBytes );
	bool patchColorRegion( const Vector2i &vLocation, PackedByteArray &rAtlasBytes );
	bool rebuildHeightAtlas( void );
	bool rebuildColorAtlas( void );
	Ref<Image> getRegionMapOrDefault( const Terrain3DRegion *pRegion, t3dmapType_t eMapType ) const;
};

#endif // TERRAIN3D_FAR_SURFACE_ATLAS_CACHE_H
