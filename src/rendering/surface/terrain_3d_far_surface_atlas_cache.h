// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_FAR_SURFACE_ATLAS_CACHE_H
#define TERRAIN3D_FAR_SURFACE_ATLAS_CACHE_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "assets/resources/generated_texture.h"
#include "data/model/terrain_3d_region.h"
#include "eastl_config.h"
#include "streaming/policy/terrain_3d_gpu_cache_constants.h"

#include <EASTL/bitset.h>
#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include "assets/catalog/terrain_3d_assets.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "rendering/material/terrain_3d_material.h"
#include "streaming/cache/terrain_3d_cpu_region_cache.h"


typedef struct t3dFarSurfaceAtlasConfig_s {

	bool m_bRequired = false;
	bool m_bStrictCooked = false;
	int m_nHeightAtlasSide = T3D_GPU_CACHE_FAR_HEIGHT_SIDE;
	int m_nSurfaceAtlasSide = T3D_GPU_CACHE_FAR_HEIGHT_SIDE;
	String m_dataDirectory;
	Ref<Image> m_defaultHeightMap;
	Ref<Image> m_defaultControlMap;
	Ref<Image> m_defaultColorMap;
	Ref<Terrain3DMaterial> m_material;
	Ref<Terrain3DAssets> m_assets;
} t3dFarSurfaceAtlasConfig_t;


// t3dFarSurfaceAtlasStats_s centralized in Terrain3DTypes.h.


class Terrain3DFarSurfaceAtlasCache {
public:
	Terrain3DFarSurfaceAtlasCache(
	        Terrain3DRegionRepository *pRepository, Terrain3DSamplingStorage *pSamplingStorage, Terrain3DCpuRegionCache *pCpuRegionCache );

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
	Terrain3DRegionRepository *m_pRepository = NULL;
	Terrain3DSamplingStorage *m_pSamplingStorage = NULL;
	Terrain3DCpuRegionCache *m_pCpuRegionCache = NULL;
	t3dFarSurfaceAtlasConfig_t m_config;

	Terrain3DGeneratedTexture m_generatedHeightMap;
	Terrain3DGeneratedTexture m_generatedColorMap;
	Ref<Image> m_heightImage;
	Ref<Image> m_colorImage;
	eastl::bitset<T3D_REGION_SLOTS> m_heightDirtyRegions;
	eastl::bitset<T3D_REGION_SLOTS> m_colorDirtyRegions;
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
	// CPU image rebuild and GPU publication are separate so an on-disk cook never
	// depends on RenderingDevice state or on the render tiering needing the atlas.
	bool rebuildHeightImage( void );
	bool rebuildColorImage( void );
	bool publishHeightAtlas( void );
	bool publishColorAtlas( void );
	bool rebuildHeightAtlas( void );
	bool rebuildColorAtlas( void );
	bool cookAtlases( void );
	Ref<Image> getRegionMapOrDefault( const Terrain3DRegion *pRegion, t3dMapType_t eMapType ) const;
};

#endif // TERRAIN3D_FAR_SURFACE_ATLAS_CACHE_H
