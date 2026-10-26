#ifndef TERRAIN3D_RASTER_HIERARCHY_BUILDER_H
#define TERRAIN3D_RASTER_HIERARCHY_BUILDER_H

#include "data/storage/terrain_3d_raster_tile_store.h"
#include <EASTL/hash_map.h>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

// Six-level sparse region quadtree. Only nodes touched by authored regions
// are materialized; parent height ranges conservatively enclose children.
typedef struct t3dRasterHierarchyNode_s
{
	float	 m_fMinHeight = 0.0f;
	float	 m_fMaxHeight = 0.0f;
	uint32_t m_nControl = T3D_REGION_DEFAULT_CONTROL;
	uint8_t	 m_szColor[4] = { 255u, 255u, 255u, 128u };
	uint32_t m_nLeafCount = 0u;
	uint32_t m_nRevision = 1u;
	uint8_t  m_nChildMask = 0u;
	uint8_t  m_nCoverageMask = 7u; // height, control, color
	float    m_fMaxHeightErrorM = 0.0f; // conservative nearest-texel error against exact source
} t3dRasterHierarchyNode_t;

// A 32x32 raster at every occupied quadtree node. All source encodings are
// exactly representable in these arrays; only parent LOD resampling is lossy.
// Authoritative leaf R16/control/RGBA8 maps are unchanged.
typedef struct t3dRasterHierarchyPayload_s
{
	uint16_t m_szHeight[32u * 32u] = {};
	uint32_t m_szControl[32u * 32u] = {};
	uint8_t	 m_szColor[32u * 32u * 4u] = {};
} t3dRasterHierarchyPayload_t;
static_assert( sizeof( t3dRasterHierarchyPayload_t ) == 10240u, "Compact hierarchy raster ABI" );

class Terrain3DRasterHierarchyBuilder
{
public:
	void clear( void );
	void updateRegion( const godot::Vector2i &rRegion, const Terrain3DRasterTileStore &rTiles );
	void removeRegion( const godot::Vector2i &rRegion );
	const t3dRasterHierarchyNode_t	  *getNode( uint8_t nLevel, uint8_t nX, uint8_t nY ) const;
	const t3dRasterHierarchyPayload_t *getPayload( uint8_t nLevel, uint8_t nX, uint8_t nY ) const;
	const eastl::hash_map< uint16_t, t3dRasterHierarchyPayload_t > &getPayloads(
		void ) const {
		return m_payloads;
	}
	const t3dRasterHierarchyNode_t *getNodeById( uint16_t nId ) const;
	uint32_t getGeneration( void ) const { return m_nGeneration; }
	static uint16_t	  nodeId( uint8_t nLevel, uint8_t nX, uint8_t nY );
	godot::Dictionary getStats( void ) const;

private:
	eastl::hash_map< uint16_t, t3dRasterHierarchyNode_t >	 m_nodes;
	eastl::hash_map< uint16_t, t3dRasterHierarchyPayload_t > m_payloads;
	uint32_t												 m_nGeneration = 1u;
	void													 rebuildParents( uint8_t nX, uint8_t nY );
};

#endif
