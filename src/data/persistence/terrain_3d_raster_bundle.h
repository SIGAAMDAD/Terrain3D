#ifndef TERRAIN3D_RASTER_BUNDLE_H
#define TERRAIN3D_RASTER_BUNDLE_H

#include "data/storage/terrain_3d_raster_tile_store.h"
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <EASTL/hash_map.h>
#include <EASTL/array.h>
#include "core/utility/terrain_3d_sync.h"

// Derived source raster bundle; the existing region resources and V7/V1 formats
// remain authoritative and readable. No automatic VT cook is performed.
class Terrain3DRasterBundle
{
public:
	static godot::String getPath( const godot::String &rDirectory );
	static godot::Error	 save( const godot::String &rDirectory, const Terrain3DRasterTileStore &rStore );
	static godot::Error	 load( const godot::String &rDirectory, Terrain3DRasterTileStore &rStore );
};

// Index-only runtime access: source raster bytes remain on disk until requested.
// This avoids retaining all authored tiles alongside the bounded V7 CPU cache.
typedef struct t3dRasterBundleIndexEntry_s
{
	uint64_t m_nOffset = 0u;
	uint64_t m_nHash = 0u;
	uint32_t m_nBytes = 0u;
	uint32_t m_nRevision = 0u;
	uint8_t	 m_nEncoding = 0u;
} t3dRasterBundleIndexEntry_t;

class Terrain3DRasterBundleIndex
{
public:
	void					   clear( void );
	godot::Error			   open( const godot::String &rDirectory );
	godot::Ref< godot::Image > reconstructRegion( const godot::Vector2i &rRegion, t3dMapType_t eChannel ) const;
	bool					   hasRegionChannel( const godot::Vector2i &rRegion, t3dMapType_t eChannel ) const;
	bool					   hasTile( uint16_t nId ) const;
	// A single cold file lease can serve all sixteen tiles of one raster channel.
	godot::Ref< godot::FileAccess > openColdReader( void ) const;
	// A caller-owned cold reader opens on the first source cache miss only.
	bool readTile( uint16_t nId, godot::PackedByteArray &rRaw, godot::Ref< godot::FileAccess > &rReader ) const;
	bool readTile(
		uint16_t				nId,
		godot::PackedByteArray &rRaw ) const {
		godot::Ref< godot::FileAccess > reader;
		return readTile( nId, rRaw, reader );
	}
	int getEntryCount(
		void ) const {
		return ( int ) m_entries.size();
	}
	// Cooked source bytes only: no decoded Image or tile data retained here.
	godot::Dictionary getSourceIoStats( void ) const;

private:
	struct SourceEntry
	{
		uint64_t			   m_nOffset = 0u;
		uint64_t			   m_nHash = 0u;
		uint64_t			   m_nUse = 0u;
		uint32_t			   m_nBytes = 0u;
		bool				   m_bReading = false;
		bool				   m_bReady = false;
		bool				   m_bFailed = false;
		uint64_t			   m_nRetryAfter = 0u;
		godot::PackedByteArray m_stored;
	};
	bool													 readStored( const t3dRasterBundleIndexEntry_t &rEntry,
																		 godot::PackedByteArray			   &rStored,
																		 godot::Ref< godot::FileAccess >   &rReader ) const;
	mutable Terrain3DMutex									 m_sourceMutex;
	mutable Terrain3DCondition								 m_sourceReady;
	mutable eastl::array< SourceEntry, 128 >				 m_sourceEntries{};
	mutable uint64_t										 m_nSourceClock = 1u;
	mutable uint64_t										 m_nSourceEpoch = 1u;
	mutable uint64_t										 m_nSourceResidentBytes = 0u;
	mutable uint64_t										 m_nSourceHits = 0u;
	mutable uint64_t										 m_nSourceJoins = 0u;
	mutable uint64_t										 m_nSourceReads = 0u;
	mutable uint64_t										 m_nSourceBytes = 0u;
	mutable uint64_t										 m_nSourceEvictions = 0u;
	uint64_t												 m_nSourceModified = 0u;
	uint64_t												 m_nSourceFileBytes = 0u;
	godot::String											 m_path;
	eastl::hash_map< uint16_t, t3dRasterBundleIndexEntry_t > m_entries;
};

#endif
