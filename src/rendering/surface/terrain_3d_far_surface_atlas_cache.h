// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_FAR_SURFACE_ATLAS_CACHE_H
#define TERRAIN3D_FAR_SURFACE_ATLAS_CACHE_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "assets/resources/generated_texture.h"
#include "data/model/terrain_3d_region.h"
#include "eastl_config.h"

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
#include "streaming/residency/terrain_3d_residency_state.h"

class Terrain3DData;

typedef struct t3dFarSurfaceAtlasConfig_s
{
	bool					 m_bRequired = false;
	bool					 m_bStrictCooked = false;
	int						 m_nHeightAtlasSide = T3D_GPU_CACHE_FAR_HEIGHT_SIDE;
	int						 m_nSurfaceAtlasSide = T3D_GPU_CACHE_FAR_HEIGHT_SIDE;
	int						 m_nFinePageLimit = 64;
	int						 m_nFineUploadBudget = 4;
	String					 m_dataDirectory;
	Ref< Image >			 m_defaultHeightMap;
	Ref< Image >			 m_defaultControlMap;
	Ref< Image >			 m_defaultColorMap;
	const Terrain3DData		*m_pData = NULL;
	Ref< Terrain3DMaterial > m_material;
	Ref< Terrain3DAssets >	 m_assets;
} t3dFarSurfaceAtlasConfig_t;

class Terrain3DFarSurfaceAtlasCache
{
public:
	Terrain3DFarSurfaceAtlasCache( Terrain3DRegionRepository *pRepository,
								   Terrain3DSamplingStorage	 *pSamplingStorage,
								   Terrain3DCpuRegionCache	 *pCpuRegionCache );

	void setConfig( const t3dFarSurfaceAtlasConfig_t &rConfig );
	void reset( void );
	void resetFrameStats( void );
	void invalidateSurface( void );
	void invalidateLayout( void );
	void markAllDirty( t3dMapType_t eMapType );
	void markRegionDirty( const Vector2i &vLocation, t3dMapType_t eMapType );
	void setFrameDemand( const t3dResidencyFrameState_s &rFrame );
	bool ensure( void );
	bool writeCooked( const String &rDirectory = String() );

	EA_FORCE_INLINE RID getHeightMapRid(
		void ) const {
		return m_generatedHeightMap.getRid();
	}
	EA_FORCE_INLINE RID getColorMapRid(
		void ) const {
		return m_generatedColorMap.getRid();
	}
	EA_FORCE_INLINE bool isReady(
		void ) const {
		return m_generatedHeightMap.getRid().is_valid() && m_generatedColorMap.getRid().is_valid();
	}
	bool isRegionHeightDirty( const Vector2i &vLocation ) const;
	bool isRegionColorDirty( const Vector2i &vLocation ) const;
	int	 getDirtyRegionCount( void ) const;
	// 1-based physical slot, packed in bits 18..29 of TerrainRegionRecord.
	// Slot zero is absent. This does not alter the 128x128 CPU region contract.
	uint16_t getPackedColorSlot( const Vector2i &rLocation ) const;
	uint64_t getPackedColorLayoutGeneration(
		void ) const {
		return m_nPackedColorLayoutGeneration;
	}
	int getPackedColorAtlasSide(
		void ) const {
		return m_nPackedColorAtlasSide;
	}
	int getPackedColorAtlasHeight(
		void ) const {
		return m_nPackedColorAtlasHeight;
	}
	size_t					  getImageBytes( void ) const;
	t3dFarSurfaceAtlasStats_t getStats( void ) const;
	Dictionary				  getStatsDictionary( void ) const;

private:
	Terrain3DRegionRepository *m_pRepository = NULL;
	Terrain3DSamplingStorage  *m_pSamplingStorage = NULL;
	Terrain3DCpuRegionCache	  *m_pCpuRegionCache = NULL;
	t3dFarSurfaceAtlasConfig_t m_config;

	Terrain3DGeneratedTexture		  m_generatedHeightMap;
	Terrain3DGeneratedTexture		  m_generatedColorMap;
	Ref< Image >					  m_heightImage;
	Ref< Image >					  m_colorImage;
	eastl::bitset< T3D_REGION_SLOTS > m_heightDirtyRegions;
	eastl::bitset< T3D_REGION_SLOTS > m_colorDirtyRegions;
	bool							  m_bHeightDirty = true;
	bool							  m_bColorDirty = true;
	bool							  m_bColorCookDirty = true;
	// m_szPackedColorSlots is the last successfully published mapping. Pending
	// changes become visible only after the matching texture uploads succeed.
	uint16_t				  m_szPackedColorSlots[T3D_REGION_SLOTS]{};
	uint16_t				  m_szDesiredColorSlots[T3D_REGION_SLOTS]{};
	uint16_t				  m_szFineSlotOwners[128]{}; // map index + 1, zero = free/retiring
	uint64_t				  m_szFineSlotRetireTick[128]{};
	uint32_t				  m_szDemandPriority[T3D_REGION_SLOTS]{};
	uint64_t				  m_nDemandTick = 0u;
	uint64_t				  m_nLastDemandServiceTick = 0u;
	uint64_t				  m_nFineAdmissions = 0u;
	uint64_t				  m_nFineEvictions = 0u;
	bool					  m_bFrameDemandAvailable = false;
	uint16_t				  m_nPackedColorCount = 0u;
	int						  m_nPackedColorAtlasSide = 0;
	int						  m_nPackedColorAtlasHeight = 0;
	uint64_t				  m_nPackedColorLayoutGeneration = 1u;
	uint64_t				  m_nPackedTopologyGeneration = 0u;
	t3dFarSurfaceAtlasStats_t m_stats;

	String	 getCookedPath( const String &rDirectory = String() ) const;
	uint64_t computeSourceSignature( void ) const;
	bool	 initializeHeightImage( void );
	bool	 initializeColorImage( void );
	bool	 refreshPackedColorLayout( void );
	void	 serviceFineResidency( void );
	bool
	writeCoarseRegion( const PackedByteArray &rFine, PackedByteArray &rAtlas, int nAtlasWidth, int nX, int nY ) const;
	bool rebuildPackedColorAtlas( void );
	bool patchPackedColorRegion( const Vector2i &rLocation, PackedByteArray &rBytes, int nSide, int nX, int nY );
	bool copyCookedColorRegion( const Vector2i &rLocation, PackedByteArray &rBytes, int nSide, int nX, int nY ) const;
	bool loadCooked( void );
	bool patchHeightRegion( const Vector2i &vLocation, PackedByteArray &rAtlasBytes );
	bool patchColorRegion( const Vector2i &vLocation, PackedByteArray &rAtlasBytes );
	bool patchColorRegionAt( const Vector2i	 &vLocation,
							 PackedByteArray &rAtlasBytes,
							 int			  nDestinationSide,
							 int			  nDestinationX,
							 int			  nDestinationY );
	// CPU image rebuild and GPU publication are separate so an on-disk cook never
	// depends on RenderingDevice state or on the render tiering needing the atlas.
	bool		 rebuildHeightImage( void );
	bool		 rebuildColorImage( void );
	bool		 publishHeightAtlas( void );
	bool		 rebuildHeightAtlas( void );
	bool		 rebuildColorAtlas( void );
	bool		 cookAtlases( void );
	Ref< Image > getRegionMapOrDefault( const Terrain3DRegion *pRegion, t3dMapType_t eMapType ) const;
};

#endif // TERRAIN3D_FAR_SURFACE_ATLAS_CACHE_H
