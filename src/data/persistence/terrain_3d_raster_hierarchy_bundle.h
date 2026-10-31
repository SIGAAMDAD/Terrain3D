#ifndef TERRAIN3D_RASTER_HIERARCHY_BUNDLE_H
#define TERRAIN3D_RASTER_HIERARCHY_BUNDLE_H

#include "data/cooking/terrain_3d_raster_hierarchy_builder.h"
#include "streaming/hierarchy/terrain_3d_hierarchy_metadata.h"
#include <godot_cpp/variant/string.hpp>

#define T3D_RASTER_HIERARCHY_MAGIC	 ( 0x48523354u ) // T3RH, fixed little-endian
#define T3D_RASTER_HIERARCHY_VERSION ( 1u )
#define T3D_RASTER_HIERARCHY_STREAM_FORMAT_VERSION ( 2u )
#define T3D_RASTER_HIERARCHY_STREAM_HEADER_BYTES ( 64u )
#define T3D_RASTER_HIERARCHY_STREAM_ENTRY_BYTES ( 112u )
#define T3D_RASTER_HIERARCHY_STREAM_HEIGHT_BYTES ( 34u * 34u * 2u )
#define T3D_RASTER_HIERARCHY_STREAM_CONTROL_BYTES ( 32u * 32u * 4u )
#define T3D_RASTER_HIERARCHY_STREAM_COLOR_BYTES ( 32u * 32u * 4u )

typedef struct t3dRasterHierarchyIndexEntry_s
{
	uint64_t m_nOffset = 0u;
	uint64_t m_nHash = 0u;
	uint32_t m_nRevision = 0u;
} t3dRasterHierarchyIndexEntry_t;

class Terrain3DRasterHierarchyBundle
{
public:
	static godot::String getPath( const godot::String &rDirectory );
	static godot::String getPathV2( const godot::String &rDirectory );
	static godot::Error saveV2( const godot::String &rDirectory, const Terrain3DRasterHierarchyBuilder &rBuilder );
	static godot::Error	 save( const godot::String &rDirectory, const Terrain3DRasterHierarchyBuilder &rBuilder );
};

// Index-only. Missing fine nodes fall back to the closest existing ancestor;
// this is strictly a visual raster fallback, never collision height.
class Terrain3DRasterHierarchyIndex
{
public:
	void		 clear( void );
	godot::Error open( const godot::String &rDirectory );
	bool		 read( uint8_t nLevel, uint8_t nX, uint8_t nY, t3dRasterHierarchyPayload_t &rPayload ) const;
	bool		 readNearestAncestor( uint8_t					   nLevel,
									  uint8_t					   nX,
									  uint8_t					   nY,
									  t3dRasterHierarchyPayload_t &rPayload,
									  uint8_t					  &rFoundLevel ) const;
	uint32_t	 getEntryCount(
			void ) const {
		return ( uint32_t ) m_entries.size();
	}

private:
	godot::String												m_path;
	eastl::hash_map< uint16_t, t3dRasterHierarchyIndexEntry_t > m_entries;
};

// F01: v2 is a separate, index-only offline/cold representation. V1 remains
// the active visual fallback until the streaming tile store is integrated.
// F02: worker-safe cold payload reader. Each call opens an independent file
// handle and verifies all three co-located channels before publishing bytes.
// pTarget owns T3D_RASTER_HIERARCHY_STREAM_TILE_BYTES writable bytes.
#define T3D_RASTER_HIERARCHY_STREAM_TILE_BYTES ( T3D_RASTER_HIERARCHY_STREAM_HEIGHT_BYTES + T3D_RASTER_HIERARCHY_STREAM_CONTROL_BYTES + T3D_RASTER_HIERARCHY_STREAM_COLOR_BYTES )
bool t3dRasterHierarchyReadStreamTile( const godot::String &rPath,
                                   const t3dRasterHierarchyStreamNode_t &rNode,
                                   uint8_t *pTarget, uint32_t nCapacity );

class Terrain3DRasterHierarchyStreamIndex
{
public:
	void clear( void );
	godot::Error open( const godot::String &rDirectory );
	const t3dRasterHierarchyStreamNode_t *getNode( uint16_t nId ) const;
	bool read( uint16_t nId, t3dRasterHierarchyPayload_t &rPayload ) const; // cold reads only
	uint32_t getEntryCount( void ) const { return ( uint32_t ) m_entries.size(); }
	uint32_t getGeneration( void ) const { return m_nGeneration; }
    const godot::String &getPath( void ) const { return m_path; }

private:
	godot::String m_path;
	eastl::hash_map< uint16_t, t3dRasterHierarchyStreamNode_t > m_entries;
	uint32_t m_nGeneration = 0u;
};

#endif
