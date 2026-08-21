// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_FAR_SURFACE_ATLAS_CACHE_H
#define TERRAIN3D_FAR_SURFACE_ATLAS_CACHE_H

#include "Terrain3DConstants.h"
#include "assets/generated_texture.h"
#include "eastl_config.h"
#include "streaming/terrain_3d_gpu_cache_constants.h"

#include <EASTL/bitset.h>
#include <cstddef>
#include <cstdint>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

class T3DAssets;
class T3DCpuRegionCache;
class T3DMaterial;
class T3DRegion;
class T3DRegionRepository;
class T3DSamplingStorage;

typedef struct t3dFarSurfaceAtlasConfig_s {
	bool m_bRequired = false;
	bool m_bStrictCooked = false;
	int m_nHeightAtlasSide = s_nGpuCacheFarHeightSide;
	int m_nSurfaceAtlasSide = s_nGpuCacheFarHeightSide;
	String m_dataDirectory;
	Ref<Image> m_defaultHeightMap;
	Ref<Image> m_defaultControlMap;
	Ref<Image> m_defaultColorMap;
	Ref<T3DMaterial> m_material;
	Ref<T3DAssets> m_assets;
} t3dFarSurfaceAtlasConfig_t;

typedef struct t3dFarSurfaceAtlasStats_s {
	int m_nHeightUpdatesLastTick = 0;
	int m_nColorUpdatesLastTick = 0;
	int m_nHeightRegionsLastTick = 0;
	int m_nColorRegionsLastTick = 0;
	int m_nHeightDirtyRegions = 0;
	int m_nColorDirtyRegions = 0;
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
	bool m_bHeightDirtyAll = true;
	bool m_bColorDirtyAll = true;
	bool m_bCookedLoaded = false;
	bool m_bCookedSaved = false;
} t3dFarSurfaceAtlasStats_t;

class T3DFarSurfaceAtlasCache {
public:
	T3DFarSurfaceAtlasCache( T3DRegionRepository *pRepository,
			T3DSamplingStorage *pSamplingStorage,
			T3DCpuRegionCache *pCpuRegionCache );

	void setConfig( const t3dFarSurfaceAtlasConfig_t &rConfig );
	void reset( void );
	void resetFrameStats( void );
	void invalidateSurface( void );
	void invalidateLayout( void );
	void markAllDirty( t3dMapType_t eMapType );
	void markRegionDirty( const Vector2i &vLocation, t3dMapType_t eMapType );
	bool ensure( void );
	bool writeCooked( const String &rDirectory = String() );

	EA_FORCE_INLINE RID getHeightMapRid( void ) const {
		return m_generatedHeightMap.getRid();
	}
	EA_FORCE_INLINE RID getColorMapRid( void ) const {
		return m_generatedColorMap.getRid();
	}
	EA_FORCE_INLINE bool isReady( void ) const {
		return m_generatedHeightMap.getRid().is_valid() && m_generatedColorMap.getRid().is_valid();
	}
	bool isRegionHeightDirty( const Vector2i &vLocation ) const;
	bool isRegionColorDirty( const Vector2i &vLocation ) const;
	int getDirtyRegionCount( void ) const;
	size_t getImageBytes( void ) const;
	t3dFarSurfaceAtlasStats_t getStats( void ) const;
	Dictionary getStatsDictionary( void ) const;

private:
	T3DRegionRepository *m_pRepository = nullptr;
	T3DSamplingStorage *m_pSamplingStorage = nullptr;
	T3DCpuRegionCache *m_pCpuRegionCache = nullptr;
	t3dFarSurfaceAtlasConfig_t m_config;

	T3DGeneratedTexture m_generatedHeightMap;
	T3DGeneratedTexture m_generatedColorMap;
	Ref<Image> m_heightImage;
	Ref<Image> m_colorImage;
	eastl::bitset<s_nMaxRegionSlots> m_heightDirtyRegions;
	eastl::bitset<s_nMaxRegionSlots> m_colorDirtyRegions;
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
	Ref<Image> getRegionMapOrDefault( const T3DRegion *pRegion, t3dMapType_t eMapType ) const;
};

#endif // TERRAIN3D_FAR_SURFACE_ATLAS_CACHE_H
