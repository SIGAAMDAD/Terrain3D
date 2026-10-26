// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_MIDDLE_HEIGHT_REGION_CACHE_H
#define TERRAIN3D_MIDDLE_HEIGHT_REGION_CACHE_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "assets/resources/generated_texture.h"
#include "core/config/terrain_3d_compiler.h"
#include "streaming/residency/terrain_3d_residency_state.h"

#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/variant/dictionary.hpp>
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"

class Terrain3DMiddleHeightRegionCache
{
public:
	Terrain3DMiddleHeightRegionCache( Terrain3DRegionRepository *pRepository,
									  Terrain3DSamplingStorage	*pSamplingStorage,
									  Terrain3DRuntimePageStore *pRuntimePageStore );

	void				 reset( void );
	bool				 configure( const t3dMiddleHeightRegionCacheConfig_t &rConfig );
	void				 update( const t3dResidencyFrameState_s			  &rFrame,
								 bool									   bForce,
								 int									   nEffectiveUploadLayers,
								 const t3dMiddleHeightRegionCacheConfig_t &rConfig );
	void				 invalidate( const Vector2i &vLocation );
	EA_FORCE_INLINE void markSelectionDirty(
		void ) {
		m_bSelectionDirty = true;
	}

	EA_FORCE_INLINE RID getHeightMapsRid(
		void ) const {
		return m_generatedHeightMaps.getRid();
	}
	EA_FORCE_INLINE RID getRegionMapRid(
		void ) const {
		return m_generatedRegionMap.getRid();
	}
	EA_FORCE_INLINE int getResidentCount(
		void ) const {
		return m_nResidentCount;
	}
	EA_FORCE_INLINE int getMissingRequiredLastTick(
		void ) const {
		return m_nMissingRequiredLastTick;
	}
	EA_FORCE_INLINE bool isResident(
		const Vector2i &vLocation ) const {
		return findSlot( vLocation ) >= 0;
	}
	EA_FORCE_INLINE bool isReady(
		void ) const {
		return m_bTexturesReady;
	}
	EA_FORCE_INLINE bool isSelectionDirty(
		void ) const {
		return m_bSelectionDirty;
	}
	Ref< Image > makeHeightImage( const Vector2i &vLocation, bool bRuntimeResolution = false ) const;
	Ref< Image > makeHeightImageForDivisor( const Vector2i &vLocation, int nResolutionDivisor ) const;
	t3dMiddleHeightRegionCacheStats_t getStats( void ) const;
	Dictionary						  getStatsDictionary( void ) const;
	size_t							  getReservedBytes( void ) const;

private:
	Terrain3DRegionRepository		  *m_pRepository = NULL;
	Terrain3DSamplingStorage		  *m_pSamplingStorage = NULL;
	Terrain3DRuntimePageStore		  *m_pRuntimePageStore = NULL;
	t3dMiddleHeightRegionCacheConfig_t m_config;

	Terrain3DGeneratedTexture m_generatedHeightMaps;
	Terrain3DGeneratedTexture m_generatedRegionMap;
	uint16_t				  m_szRegionMap[T3D_REGION_SLOTS] = {};
	t3dRegionCoord_t		  m_szLocations[T3D_GPU_HEIGHT_CACHE_MAX_LAYERS] = {};
	uint64_t				  m_szLastUseTick[T3D_GPU_HEIGHT_CACHE_MAX_LAYERS] = {};
	uint64_t				  m_szRetireAfterFrame[T3D_GPU_HEIGHT_CACHE_MAX_LAYERS] = {};
	uint8_t					  m_szSlotStates[T3D_GPU_HEIGHT_CACHE_MAX_LAYERS] = {};
	int						  m_nPhysicalLayerCount = 0;
	int						  m_nMappedCapacity = 0;
	int						  m_nResidentCount = 0;
	int						  m_nDesiredLastTick = 0;
	int						  m_nMissingRequiredLastTick = 0;
	int						  m_nUploadsLastTick = 0;
	int						  m_nEvictionsLastTick = 0;
	uint64_t				  m_nTick = 0u;
	Vector2i				  m_center = s_v2iMax;
	bool					  m_bTexturesReady = false;
	bool					  m_bPageTableDirty = true;
	bool					  m_bSelectionDirty = true;
	uint64_t				  m_nStreamingGenerationLast = 0u;

	int	 findSlot( const Vector2i &vLocation ) const;
	bool uploadLayer( int nLayer, const Vector2i &vLocation, const Ref< Image > &rPreparedHeight = Ref< Image >() );
	int	 collectDesired( const t3dResidencyFrameState_s &rFrame,
						 const Vector2i					&rCenter,
						 Vector2i						*pLocations,
						 int							 nCapacity,
						 int							&rRequiredCount ) const;
	bool updatePageTableTexture( void );
};

#endif // TERRAIN3D_MIDDLE_HEIGHT_REGION_CACHE_H
