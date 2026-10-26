// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_TERRAIN_MEGAVT_H
#define TERRAIN3D_TERRAIN_MEGAVT_H

#include "Terrain3DConstants.h"
#include "rendering/rd/vt/terrain_3d_vt_contract.h"
#include "rendering/rd/vt/terrain_3d_virtual_texture.h"

#include <stdint.h>

#define T3D_TERRAIN_MEGAVT_MIN_TEXELS_PER_METER ( 2u )
#define T3D_TERRAIN_MEGAVT_MAX_TEXELS_PER_METER ( 4u )
#define T3D_TERRAIN_MEGAVT_DEFAULT_TEXELS_PER_METER ( 2u )
#define T3D_TERRAIN_MEGAVT_MAX_MIP0_PAGES ( 4u )
#define T3D_TERRAIN_MEGAVT_MAX_PAGES_PER_REGION ( 21u )
#define T3D_TERRAIN_MEGAVT_MAX_MIP0_PAGE_COUNT ( 16u )
#define T3D_TERRAIN_MEGAVT_ALL_MIP0_PAGES_MASK ( 0xffffu )

typedef enum t3dTerrainMegaVTRegionFlags_e : uint8_t
{
	T3D_TERRAIN_MEGAVT_REGION_PRESENT = 1u << 0u,
	T3D_TERRAIN_MEGAVT_REGION_COOKED = 1u << 1u,
	T3D_TERRAIN_MEGAVT_REGION_EDITOR_DYNAMIC = 1u << 2u,
} t3dTerrainMegaVTRegionFlags_t;

typedef enum t3dTerrainMegaVTCookFlags_e : uint16_t
{
	T3D_TERRAIN_MEGAVT_COOK_ALL_CHANNELS = ( 1u << T3D_VT_CHANNEL_COUNT ) - 1u,
	T3D_TERRAIN_MEGAVT_COOK_NEIGHBOR_GUTTERS = 1u << 8u,
} t3dTerrainMegaVTCookFlags_t;

// Fixed CPU-side identity record. Allocation IDs are stable for a logical
// 32x32 world slot and survive region residency/unload transitions.
typedef struct t3dTerrainMegaVTRegion_s
{
	uint32_t m_nAllocationId;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint32_t m_nFlags;
	float m_vWorldToUv[ 4 ]; // scale X/Z, bias X/Z
	uint16_t m_nMip0WidthPages;
	uint16_t m_nMip0HeightPages;
	uint16_t m_nMipCount;
	uint16_t m_nTexelsPerMeter;
	uint32_t m_nPageCount;
	uint32_t m_nReserved;
} t3dTerrainMegaVTRegion_t;

// P25 -> common P19 page-builder contract. P25 defines what terrain needs;
// the common cooker owns tiling, gutters, compression and package emission.
typedef struct t3dTerrainMegaVTCookRequest_s
{
	uint32_t m_nAllocationId;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint32_t m_nAllocationFlags;
	int32_t m_nRegionX;
	int32_t m_nRegionZ;
	uint16_t m_nMip0WidthPages;
	uint16_t m_nMip0HeightPages;
	uint16_t m_nMipCount;
	uint16_t m_nTexelsPerMeter;
	uint32_t m_nPageCount;
	uint32_t m_nCookFlags;
	uint32_t m_nPageInterior;
	uint32_t m_nPageGutter;
	uint32_t m_nPhysicalTile;
	float m_vWorldToUv[ 4 ];
} t3dTerrainMegaVTCookRequest_t;

typedef enum t3dTerrainMegaVTIncrementalFlags_e : uint8_t
{
	T3D_TERRAIN_MEGAVT_INCREMENTAL_EDITOR = 1u << 0u,
	T3D_TERRAIN_MEGAVT_INCREMENTAL_ANCESTORS = 1u << 1u,
	T3D_TERRAIN_MEGAVT_INCREMENTAL_GUTTER_NEIGHBORS = 1u << 2u,
} t3dTerrainMegaVTIncrementalFlags_t;

typedef struct t3dTerrainMegaVTEditBounds_s
{
	float m_fMinWorldX;
	float m_fMinWorldZ;
	float m_fMaxWorldX;
	float m_fMaxWorldZ;
} t3dTerrainMegaVTEditBounds_t;

typedef struct t3dTerrainMegaVTEditorState_s
{
	uint32_t m_nDirtyMip0Mask;
	uint32_t m_nDirtyPageMask;
	uint32_t m_nInFlightGeneration;
	uint32_t m_nLastPublishedGeneration;
} t3dTerrainMegaVTEditorState_t;

typedef struct t3dTerrainMegaVTIncrementalCookRequest_s
{
	t3dTerrainMegaVTCookRequest_t m_base;
	uint32_t m_nDirtyMip0Mask;
	uint32_t m_nDirtyPageMask;
	uint32_t m_nSourceGeneration;
	uint32_t m_nIncrementalFlags;
} t3dTerrainMegaVTIncrementalCookRequest_t;

typedef struct t3dTerrainMegaVTCookTicket_s
{
	uint32_t m_nAllocationId;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint32_t m_nPageMask;
} t3dTerrainMegaVTCookTicket_t;

// 32-byte region draw record mirrored in terrain clipmap GLSL. This replaces
// the old scalar region-map entry while retaining its physical/shadow flags.
typedef struct t3dRdTerrainRegionGpu_s
{
	uint32_t m_nPackedPhysicalFlags;
	uint32_t m_nMegaVTAllocation;
	uint32_t m_nMegaVTGeneration;
	uint32_t m_nMegaVTFlags;
	float m_vMegaVTWorldToUv[ 4 ];
} t3dRdTerrainRegionGpu_t;

class Terrain3DTerrainMegaVT
{
public:
	Terrain3DTerrainMegaVT();

	bool attachVirtualTextureCold( const Terrain3DVirtualTexture *pVirtualTexture );
	void resetCold();
	bool syncRegionPresenceHot( const uint16_t *pLogicalRegionMap, uint64_t nTopologyGeneration );
	bool buildCookRequest( uint32_t nRegionMapIndex, t3dTerrainMegaVTCookRequest_t& rRequest ) const;
	bool setTexelsPerMeterCold( uint32_t nTexelsPerMeter );

	bool markEditorDirtyBoundsHot( const t3dTerrainMegaVTEditBounds_t& rBounds );
	bool importPersistentEditorStateHot( uint32_t nRegionMapIndex, uint32_t nSourceGeneration, uint32_t nDirtyMip0Mask );
	bool getPersistentEditorState( uint32_t nRegionMapIndex, uint32_t& rSourceGeneration, uint32_t& rDirtyMip0Mask ) const;
	bool claimIncrementalCookHot(
	    uint32_t nRegionMapIndex, t3dTerrainMegaVTIncrementalCookRequest_t& rRequest, t3dTerrainMegaVTCookTicket_t& rTicket );
	bool validateIncrementalCookCompletionHot( const t3dTerrainMegaVTCookTicket_t& rTicket );
	bool acceptIncrementalCookCompletionHot( const t3dTerrainMegaVTCookTicket_t& rTicket );

	uint64_t getEditorDirtyGeneration() const;
	uint64_t getStaleCompletionRejectCount() const;
	uint32_t getDirtyRegionCount() const;

	const t3dTerrainMegaVTRegion_t *getRegion( uint32_t nRegionMapIndex ) const;
	uint64_t getPublicationGeneration() const;
	uint64_t getTopologyGeneration() const;
	uint32_t getTexelsPerMeter() const;
	bool isReady() const;

	static bool computeFootprint(
	    uint32_t nTexelsPerMeter, uint16_t& rMip0WidthPages, uint16_t& rMip0HeightPages, uint16_t& rMipCount, uint32_t& rPageCount );
	static void regionIndexToLocation( uint32_t nRegionMapIndex, int32_t& rX, int32_t& rZ );
	static bool locationToRegionIndex( int32_t nRegionX, int32_t nRegionZ, uint32_t& rRegionMapIndex );
	static uint32_t computeDirtyPageClosure(
	    uint16_t nMip0WidthPages, uint16_t nMip0HeightPages, uint16_t nMipCount, uint32_t nDirtyMip0Mask );

private:
	static uint32_t makeContentId( int32_t nRegionX, int32_t nRegionZ, uint32_t nTexelsPerMeter );
	void initializeRegionIdentity( uint32_t nRegionMapIndex, t3dTerrainMegaVTRegion_t& rRegion );
	uint32_t computeRegionDirtyMip0Mask( const t3dTerrainMegaVTRegion_t& rRegion,
	                                     int32_t nRegionX,
	                                     int32_t nRegionZ,
	                                     const t3dTerrainMegaVTEditBounds_t& rBounds ) const;
	bool isIncrementalCookCompletionCurrent( const t3dTerrainMegaVTCookTicket_t& rTicket ) const;
	void advancePublicationGeneration();

	const Terrain3DVirtualTexture *m_pVirtualTexture;
	t3dTerrainMegaVTRegion_t m_szRegions[ s_nRegionSlots ];
	t3dTerrainMegaVTEditorState_t m_szEditorState[ s_nRegionSlots ];
	uint64_t m_nPublicationGeneration;
	uint64_t m_nTopologyGeneration;
	uint32_t m_nTexelsPerMeter;
	uint32_t m_nDirtyRegionCount;
	uint64_t m_nEditorDirtyGeneration;
	uint64_t m_nStaleCompletionRejectCount;
	bool m_bAttached;
};

static_assert( s_nRegionWorldSizeMeters == 128, "P25 MegaVT requires the fork's 128 m region specialization" );
static_assert( s_nRegionSlots == 1024, "P25 MegaVT stable allocation domain requires 1024 logical region slots" );
static_assert( sizeof( t3dTerrainMegaVTRegion_t ) == 48u, "Terrain MegaVT CPU region ABI changed" );
static_assert( sizeof( t3dTerrainMegaVTEditorState_t ) == 16u, "Terrain MegaVT editor state ABI changed" );
static_assert( sizeof( t3dTerrainMegaVTCookTicket_t ) == 16u, "Terrain MegaVT cook ticket ABI changed" );
static_assert( sizeof( t3dRdTerrainRegionGpu_t ) == 32u, "Terrain MegaVT draw-region ABI changed" );

#endif // TERRAIN3D_TERRAIN_MEGAVT_H
