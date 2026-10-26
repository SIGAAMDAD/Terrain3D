#ifndef TERRAIN3D_TILE_RESIDENCY_CONTROLLER_H
#define TERRAIN3D_TILE_RESIDENCY_CONTROLLER_H

#include "data/persistence/terrain_3d_raster_bundle.h"
#include "streaming/residency/terrain_3d_residency_state.h"
#include <EASTL/vector.h>
#include <godot_cpp/variant/dictionary.hpp>

// Bounded decoded source-tile residency. Uses only already cooked raster data;
// collision's exact R16 sources remain owned by the existing active-height cache.
// No Godot Images are kept by this cache, and no GPU buffers are allocated.
typedef struct t3dRasterTileLease_s
{
	uint16_t m_nSlot = UINT16_MAX;
	uint32_t m_nGeneration = 0u;
} t3dRasterTileLease_t;

typedef struct t3dRasterTileResident_s
{
	uint16_t			   m_nId = UINT16_MAX;
	uint16_t			   m_nPins = 0u;
	uint32_t			   m_nGeneration = 1u;
	uint64_t			   m_nLastUse = 0u;
	godot::PackedByteArray m_bytes;
} t3dRasterTileResident_t;

class Terrain3DTileResidencyController
{
public:
	bool					   configureCold( uint16_t nSlots );
	void					   setFrameDemand( const t3dResidencyFrameState_s &rFrame );
	void					   reset( void );
	bool					   acquire( uint16_t						  nId,
										const Terrain3DRasterBundleIndex &rIndex,
										t3dRasterTileLease_t			 &rLease,
										godot::PackedByteArray			 &rData,
										godot::Ref< godot::FileAccess >	 &rReader );
	void					   release( t3dRasterTileLease_t &rLease );
	godot::Ref< godot::Image > reconstructRegion( const godot::Vector2i			   &rRegion,
												  t3dMapType_t						eChannel,
												  const Terrain3DRasterBundleIndex &rIndex );
	godot::Dictionary		   getStats( void ) const;

private:
	eastl::vector< t3dRasterTileResident_t > m_slots;
	uint32_t								 m_szDemandPriority[T3D_REGION_SLOTS]{};
	uint64_t								 m_nClock = 1u;
	uint64_t								 m_nHits = 0u;
	uint64_t								 m_nMisses = 0u;
	uint64_t								 m_nEvictions = 0u;
	uint64_t								 m_nResidentBytes = 0u;
	uint64_t								 m_nHighWaterBytes = 0u;
};
#endif
