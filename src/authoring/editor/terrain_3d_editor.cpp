#include "memory/policy/terrain_3d_memory_account.h"
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypes.h"

#include "authoring/editor/terrain_3d_editor.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"

#include "Terrain3DConstants.h"
#include "core/utility/logger.h"
#include "core/utility/terrain_3d_util.h"
#include "data/model/terrain_3d_data.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/sampling/terrain_3d_sampler.h"
#include "eastl_config.h"
#include "rendering/compute/terrain_3d_compute.h"
#if defined( T3D_EDITOR_NATIVE )
#include "rendering/rd/core/terrain_3d_render_coordinator.h"
#endif
#include "terrain_3d.h"

#include <math.h>
#include <string.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

// t3dEditSnapshotKind_e centralized in Terrain3DTypes.h.

#define TERRAIN3DEDITORQUANTIZERGBA8( value )                                                                          \
	( uint32_t ) ( CLAMP( Math::round( ( value ) * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) )

// -----------------------------------------------------------------------------
// Private functions

bool Terrain3DEditor::isBrushSlopeAllowed(
	Terrain3DSampler &rSampler,
	const bool		  bUnrestricted,
	const Vector2	 &vSlopeRange,
	const Vector3	 &rPosition ) {
	if ( bUnrestricted ) {
		return true;
	}
	uint32_t control = UINT32_MAX;
	Vector3	 normal = s_v3Nan;
	if ( !rSampler.sampleControlAndSlopeNormal( rPosition, control, normal ) ) {
		return false;
	}
	const real_t fDegrees = Math::rad_to_deg( Math::acos( CLAMP( normal.dot( s_v3Up ), -1.0f, 1.0f ) ) );
	return vSlopeRange.x <= fDegrees && fDegrees <= vSlopeRange.y;
}

uint32_t Terrain3DEditor::packColorRgba8(
	const Color &rColor ) {
	return TERRAIN3DEDITORQUANTIZERGBA8( rColor.r ) | ( TERRAIN3DEDITORQUANTIZERGBA8( rColor.g ) << 8u ) |
		   ( TERRAIN3DEDITORQUANTIZERGBA8( rColor.b ) << 16u ) | ( TERRAIN3DEDITORQUANTIZERGBA8( rColor.a ) << 24u );
}

Color Terrain3DEditor::unpackColorRgba8(
	const uint32_t nValue ) {
	return Color( ( real_t ) ( nValue & 0xffu ) / s_fByteMaxFloat,
				  ( real_t ) ( ( nValue >> 8u ) & 0xffu ) / s_fByteMaxFloat,
				  ( real_t ) ( ( nValue >> 16u ) & 0xffu ) / s_fByteMaxFloat,
				  ( real_t ) ( ( nValue >> 24u ) & 0xffu ) / s_fByteMaxFloat );
}

uint32_t Terrain3DEditor::readU32(
	const uint8_t *pBytes,
	const size_t   iIndex ) {
	uint32_t value = T3D_REGION_DEFAULT_CONTROL;
	if ( pBytes ) {
		memcpy( &value, pBytes + iIndex * sizeof( uint32_t ), sizeof( uint32_t ) );
	}
	return value;
}

Color Terrain3DEditor::readMapPixel(
	const t3dRegionMapCache_t &rEntry,
	const size_t			   iIndex,
	const Vector2i			  &rPixel,
	const t3dMapType_t		   mapType ) {
	if ( !rEntry.m_bDirectAccess || !rEntry.m_pMapBytes ) {
		const Color raw = rEntry.m_pMap->get_pixelv( rPixel );
		return mapType == TYPE_HEIGHT
				   ? Color( t3dRegionCodecReadHeightPixel( rEntry.m_pMap, rPixel ), 0.0f, 0.0f, 1.0f )
				   : raw;
	}
	if ( mapType == TYPE_HEIGHT ) {
		uint16_t encoded = 0u;
		memcpy( &encoded, rEntry.m_pMapBytes + iIndex * sizeof( uint16_t ), sizeof( uint16_t ) );
		return Color( t3dRegionCodecDecodeHeightU16( encoded ), 0.0f, 0.0f, 1.0f );
	}
	if ( mapType == TYPE_CONTROL ) {
		return Color( t3dAsFloat( readU32( rEntry.m_pMapBytes, iIndex ) ), 0.0f, 0.0f, 1.0f );
	}
	const uint8_t *pRgba = rEntry.m_pMapBytes + iIndex * 4u;
	return Color( ( real_t ) ( pRgba[0] ) / s_fByteMaxFloat,
				  ( real_t ) ( pRgba[1] ) / s_fByteMaxFloat,
				  ( real_t ) ( pRgba[2] ) / s_fByteMaxFloat,
				  ( real_t ) ( pRgba[3] ) / s_fByteMaxFloat );
}

uint32_t Terrain3DEditor::readControlBits(
	const t3dRegionMapCache_t &rEntry,
	const size_t			   iIndex,
	const Vector2i			  &rPixel ) {
	if ( rEntry.m_pControlBytes ) {
		return readU32( rEntry.m_pControlBytes, iIndex );
	}
	return rEntry.m_pControlMap ? t3dAsUint( rEntry.m_pControlMap->get_pixelv( rPixel ).r )
								: T3D_REGION_DEFAULT_CONTROL;
}

void Terrain3DEditor::writeMapPixel(
	t3dRegionMapCache_t &rEntry,
	const size_t		 iIndex,
	const Vector2i		&rPixel,
	const Color			&clrValue,
	const t3dMapType_t	 mapType ) {
	if ( !rEntry.m_bDirectAccess || !rEntry.m_pMapBytes ) {
		if ( mapType == TYPE_HEIGHT ) {
			t3dRegionCodecWriteHeightPixel( rEntry.m_pMap, rPixel, clrValue.r );
		} else {
			rEntry.m_pMap->set_pixelv( rPixel, clrValue );
		}
		return;
	}
	if ( mapType == TYPE_HEIGHT ) {
		const uint16_t encoded = t3dRegionCodecEncodeHeightU16( clrValue.r );
		memcpy( rEntry.m_pMapBytes + iIndex * sizeof( uint16_t ), &encoded, sizeof( uint16_t ) );
		return;
	}
	if ( mapType == TYPE_CONTROL ) {
		const uint32_t bits = t3dAsUint( clrValue.r );
		memcpy( rEntry.m_pMapBytes + iIndex * sizeof( uint32_t ), &bits, sizeof( uint32_t ) );
		return;
	}
	uint8_t *pRgba = rEntry.m_pMapBytes + iIndex * 4u;
	pRgba[0] = ( uint8_t ) ( TERRAIN3DEDITORQUANTIZERGBA8( clrValue.r ) );
	pRgba[1] = ( uint8_t ) ( TERRAIN3DEDITORQUANTIZERGBA8( clrValue.g ) );
	pRgba[2] = ( uint8_t ) ( TERRAIN3DEDITORQUANTIZERGBA8( clrValue.b ) );
	pRgba[3] = ( uint8_t ) ( TERRAIN3DEDITORQUANTIZERGBA8( clrValue.a ) );
}

bool Terrain3DEditor::locationLess(
	const Vector2i &vA,
	const Vector2i &rB ) {
	return vA.x == rB.x ? vA.y < rB.y : vA.x < rB.x;
}

void Terrain3DEditor::updateBrushAlphaCache(
	Image *const	pBrushImage,
	const Vector2i &vImageSize,
	const real_t	fGamma ) {
	if ( m_pCachedBrushImage == pBrushImage && m_cachedBrushImageSize == vImageSize &&
		 Math::is_equal_approx( m_fCachedBrushGamma, fGamma ) &&
		 m_fBrushAlphaCache.size() == ( size_t ) ( vImageSize.x * vImageSize.y ) ) {
		return;
	}

	m_pCachedBrushImage = pBrushImage;
	m_cachedBrushImageSize = vImageSize;
	m_fCachedBrushGamma = fGamma;
	m_fBrushAlphaCache.resize( ( size_t ) ( vImageSize.x * vImageSize.y ) );

	for ( int32_t y = 0; y < vImageSize.y; ++y ) {
		for ( int32_t x = 0; x < vImageSize.x; ++x ) {
			real_t fAlpha = pBrushImage->get_pixel( x, y ).r;
			fAlpha = ( real_t ) ( Math::pow( ( double ) ( fAlpha ), ( double ) ( fGamma ) ) );
			m_fBrushAlphaCache[( size_t ) ( y * vImageSize.x + x )] = isnan( fAlpha ) ? 0.f : ( float ) ( fAlpha );
		}
	}
}

Dictionary Terrain3DEditor::captureEditSnapshot(
	const Ref< Terrain3DRegion > &rRegion,
	const Dictionary			 &rTemplate,
	const bool					  bMapWasImplicit ) const {
	Dictionary snapshot;
	if ( rRegion.is_null() ) {
		return snapshot;
	}

	const int kind = rTemplate.has( "kind" ) ? ( int ) ( rTemplate["kind"] )
											 : ( m_tool == INSTANCER ? EDIT_SNAPSHOT_INSTANCES : EDIT_SNAPSHOT_MAP );
	snapshot["kind"] = kind;
	snapshot["location"] = rRegion->getLocation();

	if ( kind == EDIT_SNAPSHOT_INSTANCES ) {
		snapshot["instances"] = rRegion->getInstances();
		return snapshot;
	}

	const t3dMapType_t map_type =
		rTemplate.has( "map_type" ) ? ( t3dMapType_t ) ( ( int ) ( rTemplate["map_type"] ) ) : getMapType();
	if ( map_type < TYPE_HEIGHT || map_type >= REGION_TYPE_MAX ) {
		return Dictionary();
	}
	snapshot["map_type"] = ( int ) ( map_type );
	if ( bMapWasImplicit && ( map_type == TYPE_CONTROL || map_type == TYPE_COLOR ) ) {
		snapshot["map_absent"] = true;
		return snapshot;
	}
	const Ref< Image > pMap = rRegion->getMap( map_type );
	if ( pMap.is_null() ) {
		return Dictionary();
	}

	// UndoRedo outlives the live Image and may execute after the region has been
	// compacted, streamed, or replaced. Store an immutable byte plane instead of
	// retaining a mutable Ref<Image> graph in the Variant payload. PackedByteArray
	// is copy-on-write, so the subsequent direct brush write detaches from this
	// snapshot before modifying any byte.
	snapshot["map_width"] = pMap->get_width();
	snapshot["map_height"] = pMap->get_height();
	snapshot["map_format"] = ( int ) ( pMap->get_format() );
	snapshot["map_mipmaps"] = pMap->has_mipmaps();
	snapshot["map_data"] = pMap->get_data();
	if ( map_type == TYPE_HEIGHT ) {
		snapshot["height_range"] = rRegion->getHeightRange();
	}
	return snapshot;
}

PackedByteArray Terrain3DEditor::captureUndoTile(
	Image *const pMap,
	const PackedByteArray *const pCachedBytes,
	const uint32_t nTileIndex,
	const Image::Format format ) {
	PackedByteArray tile;
	if ( !pMap || pMap->get_width() != T3D_REGION_SIZE || pMap->get_height() != T3D_REGION_SIZE ) {
		return tile;
	}
	const size_t pixel_bytes = format == Image::FORMAT_R16 ? 2u : 4u;
	const uint32_t tiles_per_axis = T3D_BRUSH_UNDO_TILES_PER_AXIS;
	if ( nTileIndex >= tiles_per_axis * tiles_per_axis ) {
		return tile;
	}
	const PackedByteArray source = pCachedBytes ? *pCachedBytes : pMap->get_data();
	const size_t minimum_bytes = ( size_t ) T3D_REGION_SIZE * T3D_REGION_SIZE * pixel_bytes;
	if ( ( size_t ) source.size() < minimum_bytes ) {
		return tile;
	}
	const size_t row_bytes = T3D_BRUSH_UNDO_TILE_SIZE * pixel_bytes;
	const size_t first_x = ( nTileIndex % tiles_per_axis ) * T3D_BRUSH_UNDO_TILE_SIZE;
	const size_t first_y = ( nTileIndex / tiles_per_axis ) * T3D_BRUSH_UNDO_TILE_SIZE;
	tile.resize( ( int64_t ) ( row_bytes * T3D_BRUSH_UNDO_TILE_SIZE ) );
	uint8_t *const destination = tile.ptrw();
	const uint8_t *const bytes = source.ptr();
	for ( size_t row = 0u; row < T3D_BRUSH_UNDO_TILE_SIZE; ++row ) {
		const size_t offset = ( ( first_y + row ) * T3D_REGION_SIZE + first_x ) * pixel_bytes;
		memcpy( destination + row * row_bytes, bytes + offset, row_bytes );
	}
	return tile;
}

void Terrain3DEditor::recordBrushUndoTile(
	t3dRegionMapCache_t &rEntry,
	const Vector2i &rPixel,
	const t3dMapType_t mapType ) {
	if ( !m_bIsOperating || rEntry.m_region.is_null() || !rEntry.m_pMap ||
		 m_addedRemovedLocations.find( rEntry.m_location ) >= 0 ) {
		return;
	}
	const Image::Format expected_format = mapType == TYPE_HEIGHT ? Image::FORMAT_R16 :
										  mapType == TYPE_CONTROL ? Image::FORMAT_RF : Image::FORMAT_RGBA8;
	if ( rEntry.m_pMap->get_format() != expected_format ) {
		return;
	}
	const uint32_t tile_index = ( ( uint32_t ) rPixel.y / T3D_BRUSH_UNDO_TILE_SIZE ) *
									T3D_BRUSH_UNDO_TILES_PER_AXIS +
								( uint32_t ) rPixel.x / T3D_BRUSH_UNDO_TILE_SIZE;
	const uint64_t tile_bit = 1ULL << tile_index;
	if ( rEntry.m_nUndoRegionIndex < 0 ) {
		for ( size_t i = 0u; i < m_brushUndoRegions.size(); ++i ) {
			if ( m_brushUndoRegions[i].m_location == rEntry.m_location &&
				 m_brushUndoRegions[i].m_mapType == mapType ) {
				rEntry.m_nUndoRegionIndex = ( int32_t ) i;
				break;
			}
		}
	}
	if ( rEntry.m_nUndoRegionIndex < 0 ) {
		rEntry.m_nUndoRegionIndex = ( int32_t ) m_brushUndoRegions.size();
		m_brushUndoRegions.push_back( t3dBrushUndoRegion_t() );
		t3dBrushUndoRegion_t *const pUndoRegion = &m_brushUndoRegions.back();
		pUndoRegion->m_location = rEntry.m_location;
		pUndoRegion->m_mapType = mapType;
		pUndoRegion->m_format = expected_format;
		pUndoRegion->m_bMipmaps = rEntry.m_pMap->has_mipmaps();
		pUndoRegion->m_bWasImplicit = rEntry.m_bMapWasImplicit;
		pUndoRegion->m_heightRange = rEntry.m_region->getHeightRange();
	}
	t3dBrushUndoRegion_t *const pUndoRegion = &m_brushUndoRegions[( size_t ) rEntry.m_nUndoRegionIndex];
	if ( pUndoRegion->m_nTouchedMask & tile_bit ) {
		return;
	}
	const PackedByteArray *const pCache = rEntry.m_bDirectAccess ? &rEntry.m_mapData : NULL;
	PackedByteArray before = captureUndoTile( rEntry.m_pMap, pCache, tile_index, expected_format );
	if ( before.is_empty() ) {
		return;
	}
	t3dBrushUndoTile_t tile;
	tile.m_nTileIndex = ( uint8_t ) tile_index;
	tile.m_before = before;
	pUndoRegion->m_tiles.push_back( tile );
	pUndoRegion->m_nTouchedMask |= tile_bit;
}

void Terrain3DEditor::finishBrushUndoTiles(
	void ) {
	Terrain3DData *const pData = m_pTerrain->dataObject();
	for ( size_t i = 0u; i < m_brushUndoRegions.size(); ++i ) {
		const t3dBrushUndoRegion_t &entry = m_brushUndoRegions[i];
		const Ref< Terrain3DRegion > region = pData->getRegion( entry.m_location );
		if ( region.is_null() || region->isDeleted() || entry.m_tiles.empty() ) {
			continue;
		}
		Ref< Image > current = region->getMap( entry.m_mapType );
		if ( current.is_null() || current->get_format() != entry.m_format ) {
			continue;
		}
		Dictionary undo;
		Dictionary redo;
		undo["kind"] = EDIT_SNAPSHOT_MAP;
		redo["kind"] = EDIT_SNAPSHOT_MAP;
		undo["location"] = entry.m_location;
		redo["location"] = entry.m_location;
		undo["map_type"] = ( int ) entry.m_mapType;
		redo["map_type"] = ( int ) entry.m_mapType;
		undo["map_width"] = T3D_REGION_SIZE;
		redo["map_width"] = T3D_REGION_SIZE;
		undo["map_height"] = T3D_REGION_SIZE;
		redo["map_height"] = T3D_REGION_SIZE;
		undo["map_format"] = ( int ) entry.m_format;
		redo["map_format"] = ( int ) entry.m_format;
		undo["map_mipmaps"] = entry.m_bMipmaps;
		redo["map_mipmaps"] = entry.m_bMipmaps;
		if ( entry.m_mapType == TYPE_HEIGHT ) {
			undo["height_range"] = entry.m_heightRange;
			redo["height_range"] = region->getHeightRange();
		}
		if ( entry.m_bWasImplicit ) {
			undo["map_absent"] = true;
		} else {
			Array before_tiles;
			for ( size_t j = 0u; j < entry.m_tiles.size(); ++j ) {
				Dictionary tile;
				tile["index"] = ( int ) entry.m_tiles[j].m_nTileIndex;
				tile["bytes"] = entry.m_tiles[j].m_before;
				before_tiles.push_back( tile );
			}
			undo["map_tiles"] = before_tiles;
		}
		const PackedByteArray current_bytes = current->get_data();
		Array after_tiles;
		for ( size_t j = 0u; j < entry.m_tiles.size(); ++j ) {
			Dictionary tile;
			const uint32_t tile_index = entry.m_tiles[j].m_nTileIndex;
			tile["index"] = ( int ) tile_index;
			tile["bytes"] = captureUndoTile( current.ptr(), &current_bytes, tile_index, entry.m_format );
			after_tiles.push_back( tile );
		}
		redo["map_tiles"] = after_tiles;
		m_originalEditSnapshots.push_back( undo );
		m_editedEditSnapshots.push_back( redo );
		region->setEdited( false );
	}
	m_brushUndoRegions.clear();
}

bool Terrain3DEditor::controlHoleBytesDiffer(
	const PackedByteArray &rOld,
	const PackedByteArray &rNew ) {
	const size_t count = ( size_t ) T3D_REGION_SIZE * T3D_REGION_SIZE;
	const uint8_t *const old_bytes = rOld.size() >= ( int64_t ) ( count * 4u ) ? rOld.ptr() : NULL;
	const uint8_t *const new_bytes = rNew.size() >= ( int64_t ) ( count * 4u ) ? rNew.ptr() : NULL;
	for ( size_t i = 0u; i < count; ++i ) {
		if ( t3dIsHole( readU32( old_bytes, i ) ) != t3dIsHole( readU32( new_bytes, i ) ) ) {
			return true;
		}
	}
	return false;
}

void Terrain3DEditor::applyEditSnapshot(
	const Dictionary	  &rSnapshot,
	t3dEditorMapUpdates_t &rMapUpdates,
	bool				  &bInstancesChanged,
	bool				  &bHoleChanged ) {
	if ( !rSnapshot.has( "kind" ) || !rSnapshot.has( "location" ) ) {
		return;
	}

	Terrain3DData *const   pData = m_pTerrain->dataObject();
	const Vector2i		   location = rSnapshot["location"];
	Ref< Terrain3DRegion > pRegion = pData->getRegion( location );
	if ( pRegion.is_null() || pRegion->isDeleted() ) {
		return;
	}

	const int kind = rSnapshot["kind"];
	if ( kind == EDIT_SNAPSHOT_INSTANCES ) {
		if ( rSnapshot.has( "instances" ) ) {
			const Dictionary instances = rSnapshot["instances"];
			pRegion->setInstances( instances );
			pRegion->setModified( true );
			bInstancesChanged = true;
		}
		return;
	}

	if ( !rSnapshot.has( "map_type" ) ) {
		return;
	}
	const t3dMapType_t map_type = ( t3dMapType_t ) ( ( int ) ( rSnapshot["map_type"] ) );
	if ( map_type < TYPE_HEIGHT || map_type >= REGION_TYPE_MAX ) {
		return;
	}
	if ( ( bool ) ( rSnapshot.get( "map_absent", false ) ) ) {
		if ( map_type == TYPE_CONTROL && pRegion->getMap( TYPE_CONTROL ).is_valid() ) {
			bHoleChanged |= controlHoleBytesDiffer( pRegion->getMap( TYPE_CONTROL )->get_data(), PackedByteArray() );
		}
		if ( map_type == TYPE_CONTROL || map_type == TYPE_COLOR ) {
			pRegion->setMap( map_type, Ref< Image >() );
			pRegion->setModified( true );
			rMapUpdates.m_locations[( size_t ) ( map_type )].push_back( location );
		}
		return;
	}
	if ( rSnapshot.has( "map_tiles" ) ) {
		const Image::Format format = map_type == TYPE_HEIGHT ? Image::FORMAT_R16 :
									 map_type == TYPE_CONTROL ? Image::FORMAT_RF : Image::FORMAT_RGBA8;
		Ref< Image > map = pRegion->getMap( map_type );
		if ( map.is_null() ) {
			pRegion->getMapPtrForWrite( map_type );
			map = pRegion->getMap( map_type );
		}
		if ( map.is_null() || map->get_format() != format ||
			 map->get_width() != T3D_REGION_SIZE || map->get_height() != T3D_REGION_SIZE ) {
			return;
		}
		PackedByteArray bytes = map->get_data();
		const size_t pixel_bytes = format == Image::FORMAT_R16 ? 2u : 4u;
		if ( ( size_t ) bytes.size() < ( size_t ) T3D_REGION_SIZE * T3D_REGION_SIZE * pixel_bytes ) {
			return;
		}
		const Array tiles = rSnapshot["map_tiles"];
		// Validate every record before mutating any bytes; corrupted history
		// must never leave a half-restored source map.
		for ( int i = 0; i < tiles.size(); ++i ) {
			const Dictionary tile = tiles[i];
			const int index = tile.get( "index", -1 );
			const PackedByteArray data = tile.get( "bytes", PackedByteArray() );
			if ( index < 0 || index >= T3D_BRUSH_UNDO_TILES_PER_AXIS * T3D_BRUSH_UNDO_TILES_PER_AXIS ||
				 ( size_t ) data.size() != T3D_BRUSH_UNDO_TILE_SIZE * T3D_BRUSH_UNDO_TILE_SIZE * pixel_bytes ) {
				LOG( ERROR, "Rejected invalid Terrain3D brush tile undo for region ", location );
				return;
			}
		}
		if ( map_type == TYPE_CONTROL ) {
			const uint8_t *const source = bytes.ptr();
			for ( int i = 0; i < tiles.size() && !bHoleChanged; ++i ) {
				const Dictionary tile = tiles[i];
				const int index = tile["index"];
				const PackedByteArray data = tile["bytes"];
				const size_t first_x = ( ( size_t ) index % T3D_BRUSH_UNDO_TILES_PER_AXIS ) * T3D_BRUSH_UNDO_TILE_SIZE;
				const size_t first_y = ( ( size_t ) index / T3D_BRUSH_UNDO_TILES_PER_AXIS ) * T3D_BRUSH_UNDO_TILE_SIZE;
				for ( size_t y = 0u; y < T3D_BRUSH_UNDO_TILE_SIZE && !bHoleChanged; ++y ) {
					for ( size_t x = 0u; x < T3D_BRUSH_UNDO_TILE_SIZE; ++x ) {
						const size_t old_i = ( ( first_y + y ) * T3D_REGION_SIZE + first_x + x );
						const size_t new_i = y * T3D_BRUSH_UNDO_TILE_SIZE + x;
						uint32_t next = 0u;
						memcpy( &next, data.ptr() + new_i * 4u, 4u );
						if ( t3dIsHole( readU32( source, old_i ) ) != t3dIsHole( next ) ) {
							bHoleChanged = true;
							break;
						}
					}
				}
			}
		}
		uint8_t *const destination = bytes.ptrw();
		for ( int i = 0; i < tiles.size(); ++i ) {
			const Dictionary tile = tiles[i];
			const int index = tile["index"];
			const PackedByteArray data = tile["bytes"];
			const size_t first_x = ( ( size_t ) index % T3D_BRUSH_UNDO_TILES_PER_AXIS ) * T3D_BRUSH_UNDO_TILE_SIZE;
			const size_t first_y = ( ( size_t ) index / T3D_BRUSH_UNDO_TILES_PER_AXIS ) * T3D_BRUSH_UNDO_TILE_SIZE;
			const size_t row_bytes = T3D_BRUSH_UNDO_TILE_SIZE * pixel_bytes;
			for ( size_t row = 0u; row < T3D_BRUSH_UNDO_TILE_SIZE; ++row ) {
				const size_t offset = ( ( first_y + row ) * T3D_REGION_SIZE + first_x ) * pixel_bytes;
				memcpy( destination + offset, data.ptr() + row * row_bytes, row_bytes );
			}
		}
		map->set_data( T3D_REGION_SIZE, T3D_REGION_SIZE, map->has_mipmaps(), format, bytes );
		if ( map_type == TYPE_HEIGHT && rSnapshot.has( "height_range" ) ) {
			pRegion->setHeightRange( rSnapshot["height_range"] );
		}
		pRegion->setModified( true );
		rMapUpdates.m_locations[( size_t ) map_type].push_back( location );
		return;
	}
	Ref< Image > pRestoredMap;
	if ( rSnapshot.has( "map_data" ) && rSnapshot.has( "map_width" ) && rSnapshot.has( "map_height" ) &&
		 rSnapshot.has( "map_format" ) ) {
		const int			  width = rSnapshot["map_width"];
		const int			  height = rSnapshot["map_height"];
		const int			  format_value = rSnapshot["map_format"];
		const bool			  mipmaps = ( bool ) ( rSnapshot.get( "map_mipmaps", false ) );
		const PackedByteArray bytes = rSnapshot["map_data"];
		Image::Format		  expected_format = Image::FORMAT_RGBA8;
		if ( map_type == TYPE_HEIGHT ) {
			expected_format = Image::FORMAT_R16;
		} else if ( map_type == TYPE_CONTROL ) {
			expected_format = Image::FORMAT_RF;
		}
		size_t		 expected_bytes = 0u;
		int			 mip_width = width;
		int			 mip_height = height;
		const size_t bytes_per_texel = expected_format == Image::FORMAT_R16 ? 2u : 4u;
		do {
			expected_bytes += ( size_t ) ( mip_width ) * ( size_t ) ( mip_height ) *bytes_per_texel;
			if ( !mipmaps || ( mip_width == 1 && mip_height == 1 ) ) {
				break;
			}
			mip_width = MAX( mip_width >> 1, 1 );
			mip_height = MAX( mip_height >> 1, 1 );
		} while ( true );
		if ( width != T3D_REGION_SIZE || height != T3D_REGION_SIZE || format_value != ( int ) ( expected_format ) ||
			 ( size_t ) ( bytes.size() ) != expected_bytes ) {
			LOG( ERROR, "Rejected invalid Terrain3D edit snapshot for region ", location );
			return;
		}
		if ( map_type == TYPE_CONTROL ) {
			const Ref< Image > old_map = pRegion->getMap( TYPE_CONTROL );
			bHoleChanged |= controlHoleBytesDiffer( old_map.is_valid() ? old_map->get_data() : PackedByteArray(), bytes );
		}
		pRestoredMap = Image::create_from_data( width, height, mipmaps, expected_format, bytes );
	} else if ( rSnapshot.has( "map" ) ) {
		// Backward compatibility for actions created before byte-plane snapshots.
		const Ref< Image > pSnapshotMap = rSnapshot["map"];
		if ( pSnapshotMap.is_valid() ) {
			if ( map_type == TYPE_CONTROL ) {
				const Ref< Image > old_map = pRegion->getMap( TYPE_CONTROL );
				bHoleChanged |= controlHoleBytesDiffer( old_map.is_valid() ? old_map->get_data() : PackedByteArray(), pSnapshotMap->get_data() );
			}
			pRestoredMap = pSnapshotMap->duplicate();
		}
	}
	if ( pRestoredMap.is_null() || pRestoredMap->is_empty() ) {
		return;
	}
	pRegion->setMap( map_type, pRestoredMap );
	if ( map_type == TYPE_HEIGHT && rSnapshot.has( "height_range" ) ) {
		pRegion->setHeightRange( rSnapshot["height_range"] );
	}
	pRegion->setModified( true );
	rMapUpdates.m_locations[( size_t ) ( map_type )].push_back( location );
}

// Sends the whole region aabb to edited_area
void Terrain3DEditor::sendRegionAabb(
	const Vector2i &rRegionLoc,
	const Vector2  &rHeightRange ) {
	const int region_size = T3D_REGION_SIZE;
	AABB	  edited_area;
	edited_area.position = Vector3( rRegionLoc.x * region_size, rHeightRange.x, rRegionLoc.y * region_size );
	edited_area.size = Vector3( region_size, rHeightRange.y - rHeightRange.x, region_size );
	edited_area.position *= m_pTerrain->getVertexSpacing();
	edited_area.size *= m_pTerrain->getVertexSpacing();
	m_pTerrain->dataObject()->addEditedArea( edited_area );
}

// Process location to add new region, mark as deleted, or just retrieve
Ref< Terrain3DRegion > Terrain3DEditor::operateRegion(
	const Vector2i &rRegionLoc ) {
	bool				 bChanged = false;
	Vector2				 height_range;
	Terrain3DData *const pData = m_pTerrain->dataObject();
	// Check if in bounds, limiting errors
	bool		   bCanPrint = false;
	const uint64_t ticks = Time::get_singleton()->get_ticks_msec();
	if ( ticks - m_nLastRegionBoundsError > 1000 ) {
		m_nLastRegionBoundsError = ticks;
		bCanPrint = true;
	}
	if ( !pData->isRegionLocationValidInternal( rRegionLoc ) ) {
		if ( bCanPrint ) {
			LOG( INFO,
				 "Location ",
				 rRegionLoc,
				 " out of bounds. Max: ",
				 -T3D_REGION_MAP_SIZE / 2,
				 " to ",
				 T3D_REGION_MAP_SIZE / 2 - 1 );
		}
		return Ref< Terrain3DRegion >();
	}

	// Get Region & dump data if debug
	Ref< Terrain3DRegion > pRegion = pData->getRegion( rRegionLoc );
	if ( pRegion.is_valid() && !pRegion->isDeleted() && !pData->ensureCpuRegionResident( rRegionLoc ) ) {
		LOG( ERROR, "Unable to page CPU terrain maps for editor region ", rRegionLoc );
		return Ref< Terrain3DRegion >();
	}
	if ( bCanPrint ) {
		LOG( DEBUG,
			 "Tool: ",
			 m_tool,
			 " Op: ",
			 m_nOperation,
			 " processing region ",
			 rRegionLoc,
			 ": ",
			 t3dPtrToStr( *pRegion ) );
	}

	// Create new region if location is null or deleted
	if ( pRegion.is_null() || ( pRegion.is_valid() && pRegion->isDeleted() ) ) {
		// And tool is Add Region, or Height + auto_regions
		if ( ( m_tool == REGION && m_nOperation == ADD ) ||
			 ( ( m_tool == SCULPT || m_tool == HEIGHT ) && m_brushData["auto_regions"] ) ) {
			if ( !m_undoData.has( "region_locations" ) ) {
				m_undoData["region_locations"] = pData->getRegionLocations().duplicate();
			}
			LOG( DEBUG, "Adding blank region at: ", rRegionLoc, ", ptr: ", t3dPtrToStr( *pRegion ) );
			pRegion = pData->addRegionBlank( rRegionLoc );
			if ( pRegion.is_null() ) {
				LOG( ERROR, "A new region cannot be created" );
				return pRegion;
			}
			m_editedRegions.push_back( pRegion ); // Ensure new region is added to the redo set
			bChanged = true;
		}
	}

	// If removing region
	else if ( pRegion.is_valid() && m_tool == REGION && m_nOperation == SUBTRACT ) {
		if ( !m_undoData.has( "region_locations" ) ) {
			m_undoData["region_locations"] = pData->getRegionLocations().duplicate();
		}
		LOG( DEBUG, "Removing region at: ", rRegionLoc, ", ptr: ", t3dPtrToStr( *pRegion ) );
		m_originalRegions.push_back( pRegion );
		height_range = pRegion->getHeightRange();
		m_pTerrain->dataObject()->removeRegion( pRegion );
		bChanged = true;
	}

	if ( bChanged ) {
		m_addedRemovedLocations.push_back( rRegionLoc );
		pRegion->setModified( true );
		sendRegionAabb( rRegionLoc, height_range );
	}
	return pRegion;
}

bool Terrain3DEditor::prepareMapOperationSamples(
	t3dMapOperationState_t &rState ) {
	const real_t fHalfBrushSize = rState.m_fBrushSize * .5f;
	const real_t fSampleCenterOffset = rState.m_fVertexSpacing * .5f;
	rState.m_nSampleCount = MAX( 1, ( int32_t ) Math::ceil( rState.m_fBrushSize / rState.m_fVertexSpacing ) );
	rState.m_nTotalSamples = ( int64_t ) ( rState.m_nSampleCount ) * ( int64_t ) ( rState.m_nSampleCount );
	const int64_t brush_pixel_count = ( int64_t ) ( rState.m_imageSize.x ) * ( int64_t ) ( rState.m_imageSize.y );
	rState.m_bUseAlphaCache = rState.m_nTotalSamples >= MAX( 4096, brush_pixel_count / 4 );
	if ( rState.m_bUseAlphaCache ) {
		updateBrushAlphaCache( rState.m_pBrushImage, rState.m_imageSize, rState.m_fGamma );
	}
	rState.m_clampedSlopeRange = CLAMP( rState.m_slopeRange, s_v2Zero, V2( 90.f ) );
	rState.m_bUnrestrictedSlope = rState.m_clampedSlopeRange.y - rState.m_clampedSlopeRange.x > 89.99f;
	const bool compute_supported =
		m_nOperation != AVERAGE &&
		( ( rState.m_mapType == TYPE_HEIGHT &&
			( m_nOperation == ADD || m_nOperation == SUBTRACT || m_nOperation == GRADIENT ) ) ||
		  ( rState.m_mapType == TYPE_CONTROL &&
			( m_tool == TEXTURE || m_tool == AUTOSHADER || m_tool == HOLES || m_tool == NAVIGATION ) &&
			( m_nOperation == ADD || m_nOperation == SUBTRACT || m_nOperation == REPLACE ) ) ||
		  ( rState.m_mapType == TYPE_COLOR && ( m_tool == COLOR || m_tool == ROUGHNESS ) &&
			( m_nOperation == ADD || m_nOperation == SUBTRACT ) ) );
	// The legacy brush compute path submits, synchronizes and reads the entire
	// batch for every dab. Keep the established buffered brush arithmetic and
	// immediate clipmap preview, but execute it on the CPU until the transient
	// renderer-owned GPU tile/preview path is available. This also avoids
	// constructing a local RenderingDevice on brush selection.
	rState.m_bUseBufferedBrush = compute_supported && rState.m_nTotalSamples >= 4096;
	rState.m_bUseDirectMapAccess =
		m_nOperation != AVERAGE && ( rState.m_bUseBufferedBrush || rState.m_nTotalSamples >= 1024 );

	m_xAxisScratch.resize( ( size_t ) ( rState.m_nSampleCount ) );
	m_zAxisScratch.resize( ( size_t ) ( rState.m_nSampleCount ) );
	for ( int32_t i = 0; i < rState.m_nSampleCount; ++i ) {
		const real_t  fDistance = ( real_t ) ( i ) *rState.m_fVertexSpacing;
		const bool	  single_sample = rState.m_nSampleCount == 1;
		const real_t  offset = single_sample ? 0.0f : fDistance - fHalfBrushSize + fSampleCenterOffset;
		const real_t  fGlobalX = rState.m_globalPosition.x + offset;
		const real_t  fGlobalZ = rState.m_globalPosition.z + offset;
		const real_t  fDescaledX = fGlobalX / rState.m_fVertexSpacing;
		const real_t  fDescaledZ = fGlobalZ / rState.m_fVertexSpacing;
		const int32_t region_x = ( int32_t ) Math::floor( fDescaledX / ( real_t ) ( rState.m_nRegionSize ) );
		const int32_t region_z = ( int32_t ) Math::floor( fDescaledZ / ( real_t ) ( rState.m_nRegionSize ) );
		m_xAxisScratch[i] = { offset,
							  fGlobalX,
							  region_x,
							  ( int32_t ) ( fDescaledX - ( real_t ) ( region_x * rState.m_nRegionSize ) ),
							  offset / rState.m_fBrushSize };
		m_zAxisScratch[i] = { offset,
							  fGlobalZ,
							  region_z,
							  ( int32_t ) ( fDescaledZ - ( real_t ) ( region_z * rState.m_nRegionSize ) ),
							  offset / rState.m_fBrushSize };
	}

	rState.m_fRotationCos = Math::cos( rState.m_fRotation );
	rState.m_fRotationSin = Math::sin( rState.m_fRotation );
	const uint32_t scale_align[8] = { 5, 6, 7, 0, 1, 2, 3, 4 };
	rState.m_nMinRegionX = m_xAxisScratch.front().m_nRegionLocation;
	rState.m_nMaxRegionX = m_xAxisScratch.back().m_nRegionLocation;
	rState.m_nMinRegionZ = m_zAxisScratch.front().m_nRegionLocation;
	rState.m_nMaxRegionZ = m_zAxisScratch.back().m_nRegionLocation;
	rState.m_nRegionCacheWidth = rState.m_nMaxRegionX - rState.m_nMinRegionX + 1;
	rState.m_nRegionCacheHeight = rState.m_nMaxRegionZ - rState.m_nMinRegionZ + 1;
	m_regionCacheScratch.clear();
	m_regionCacheScratch.resize( ( size_t ) ( rState.m_nRegionCacheWidth ) *
								 ( size_t ) ( rState.m_nRegionCacheHeight ) );
	for ( int32_t region_z = rState.m_nMinRegionZ; region_z <= rState.m_nMaxRegionZ; ++region_z ) {
		for ( int32_t region_x = rState.m_nMinRegionX; region_x <= rState.m_nMaxRegionX; ++region_x ) {
			t3dRegionMapCache_t &entry = m_regionCacheScratch[( size_t ) ( region_z - rState.m_nMinRegionZ ) *
																  ( size_t ) ( rState.m_nRegionCacheWidth ) +
															  ( size_t ) ( region_x - rState.m_nMinRegionX )];
			entry.m_location = Vector2i( region_x, region_z );
			entry.m_region = operateRegion( entry.m_location );
			entry.m_bMapWasImplicit =
				entry.m_region.is_valid() && entry.m_region->getMapPtr( rState.m_mapType ) == NULL;
			entry.m_pMap = entry.m_region.is_valid() ? entry.m_region->getMapPtrForWrite( rState.m_mapType ) : NULL;
			entry.m_pControlMap = entry.m_region.is_valid() ? entry.m_region->getMapPtr( TYPE_CONTROL ) : NULL;
			if ( !entry.m_pMap || !rState.m_bUseDirectMapAccess ) {
				continue;
			}
			entry.m_mapFormat = entry.m_pMap->get_format();
			entry.m_bMapHasMipmaps = entry.m_pMap->has_mipmaps();
			const Image::Format expected_format = rState.m_mapType == TYPE_HEIGHT	 ? Image::FORMAT_R16
												  : rState.m_mapType == TYPE_CONTROL ? Image::FORMAT_RF
																					 : Image::FORMAT_RGBA8;
			const size_t bytes_per_texel = rState.m_mapType == TYPE_HEIGHT ? sizeof( uint16_t ) : sizeof( uint32_t );
			const size_t expected_bytes =
				( size_t ) ( rState.m_nRegionSize ) * ( size_t ) ( rState.m_nRegionSize ) * bytes_per_texel;
			if ( entry.m_mapFormat != expected_format || entry.m_pMap->get_width() != rState.m_nRegionSize ||
				 entry.m_pMap->get_height() != rState.m_nRegionSize ) {
				continue;
			}
			entry.m_mapData = entry.m_pMap->get_data();
			++m_nBrushGetDataCalls;
			m_nBrushBytesRead += ( uint64_t ) entry.m_mapData.size();
			if ( ( size_t ) ( entry.m_mapData.size() ) < expected_bytes ) {
				entry.m_mapData.clear();
				continue;
			}
			entry.m_pMapBytes = entry.m_mapData.ptrw();
			entry.m_bDirectAccess = entry.m_pMapBytes != NULL;
			if ( !entry.m_bDirectAccess ) {
				continue;
			}
			if ( rState.m_mapType == TYPE_CONTROL ) {
				entry.m_pControlBytes = entry.m_pMapBytes;
			} else if ( entry.m_pControlMap && entry.m_pControlMap->get_format() == Image::FORMAT_RF &&
						entry.m_pControlMap->get_width() == rState.m_nRegionSize &&
						entry.m_pControlMap->get_height() == rState.m_nRegionSize ) {
				entry.m_controlData = entry.m_pControlMap->get_data();
				++m_nBrushGetDataCalls;
				m_nBrushBytesRead += ( uint64_t ) entry.m_controlData.size();
				if ( ( size_t ) ( entry.m_controlData.size() ) >=
					 ( size_t ) ( rState.m_nRegionSize ) * ( size_t ) ( rState.m_nRegionSize ) * sizeof( uint32_t ) ) {
					entry.m_pControlBytes = entry.m_controlData.ptr();
				}
			}
		}
	}
	m_computeElementScratch.clear();
	m_computeWriteScratch.clear();
	if ( rState.m_bUseBufferedBrush ) {
		m_computeElementScratch.reserve( ( size_t ) ( rState.m_nTotalSamples ) );
		m_computeWriteScratch.reserve( ( size_t ) ( rState.m_nTotalSamples ) );
	}
	real_t compute_angle = rState.m_fAngle;
	if ( rState.m_bDynamicAngle ) {
		compute_angle = Vector2( -m_operationMovement.x, m_operationMovement.z ).angle();
		compute_angle = ( real_t ) ( Math::fmod( Math::rad_to_deg( compute_angle ) + 450.f, ( real_t ) ( 360.f ) ) );
	}
	rState.m_nComputeUvRotation = ( uint32_t ) ( CLAMP( Math::round( compute_angle / 22.5f ), 0.f, 15.f ) );
	rState.m_nComputeUvScale =
		scale_align[( uint8_t ) ( CLAMP( Math::round( ( rState.m_fScale + 60.f ) / 20.f ), 0.f, 7.f ) )];
	return true;
}

bool Terrain3DEditor::applyHeightSample(
	t3dMapOperationState_t &rState,
	t3dBrushSample_t	   &rSample ) {
	const Vector3		   &rGlobalPosition = rState.m_globalPosition;
	Terrain3DData *const	pData = rState.m_pData;
	const real_t			strength = rState.m_fStrength;
	const real_t			fHeight = rState.m_fHeight;
	const bool				bModifierAlt = rState.m_bModifierAlt;
	PackedVector3Array	   &gradient_points = rState.m_gradientPoints;
	AABB				   &edited_area = rState.m_editedArea;
	Ref< Terrain3DRegion > &region = rSample.m_entry.m_region;
	const Vector2		   &brushOffset = rSample.m_offset;
	const Vector3		   &brushGlobalPosition = rSample.m_globalPosition;
	Vector3				   &edited_position = rSample.m_editedPosition;
	const Color			   &src = rSample.m_source;
	Color				   &dest = rSample.m_destination;
	const real_t			fBrushAlpha = rSample.m_fAlpha;
	real_t					fSrcf = src.r;
	// In case data in existing map has nan or inf saved, check, and reset to real number if required.
	fSrcf = isnan( fSrcf ) ? 0.f : fSrcf;
	edited_position.y = fSrcf;
	edited_area = edited_area.expand( edited_position );
	real_t fDestf = fSrcf;

	switch ( m_nOperation ) {
	case ADD: {
		if ( m_tool == HEIGHT ) {
			// Height
			fDestf = Math::lerp( fSrcf, fHeight, CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
		} else if ( bModifierAlt && !isnan( rGlobalPosition.y ) ) {
			// Lift troughs
			const real_t fBrushCenterY = rGlobalPosition.y + fBrushAlpha * strength;
			fDestf = Math::clamp( fBrushCenterY, fSrcf, fSrcf + fBrushAlpha * strength );
		} else {
			// Raise
			fDestf = fSrcf + ( fBrushAlpha * strength );
		}
		break;
	}
	case SUBTRACT: {
		if ( m_tool == HEIGHT ) {
			// Height, but GDScript has already picked height at cursor
			fDestf = Math::lerp( fSrcf, fHeight, CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
		} else if ( bModifierAlt && !isnan( rGlobalPosition.y ) ) {
			// Flatten peaks
			const real_t fBrushCenterY = rGlobalPosition.y - fBrushAlpha * strength;
			fDestf = Math::clamp( fBrushCenterY, fSrcf - fBrushAlpha * strength, fSrcf );
		} else {
			// Lower
			fDestf = fSrcf - ( fBrushAlpha * strength );
		}
		break;
	}
	case AVERAGE: {
		const real_t fAvgDefault = m_pTerrain->materialResource()->getWorldBackground() == 0u ? fSrcf : 0.f;
		const real_t fAvg = average( AVG_HEIGHT, brushGlobalPosition, fSrcf, fAvgDefault );
		fDestf = Math::lerp( fSrcf, fAvg, CLAMP( fBrushAlpha * strength * 2.f, 0.f, 1.f ) );
		break;
	}
	case GRADIENT: {
		if ( gradient_points.size() == 2 ) {
			const Vector3 point_1 = gradient_points[0];
			const Vector3 point_2 = gradient_points[1];
			const Vector2 point1Xz = Vector2( point_1.x, point_1.z );
			const Vector2 point_2_xz = Vector2( point_2.x, point_2.z );
			const Vector2 dir = point_2_xz - point1Xz;
			if ( dir.length_squared() < 0.01f ) {
				return false;
			}
			Vector2 brushXz = Vector2( brushGlobalPosition.x, brushGlobalPosition.z );

			if ( m_operationMovement.length_squared() > 0.f ) {
				// Ramp up/down only in the direction of movement, to avoid giving winding
				// paths one edge higher than the other.
				const Vector2 movement_xz = Vector2( m_operationMovement.x, m_operationMovement.z ).normalized();
				const Vector2 offset = movement_xz * Vector2( brushOffset ).dot( movement_xz );
				brushXz = Vector2( rGlobalPosition.x + offset.x, rGlobalPosition.z + offset.y );
			}

			real_t fWeight = dir.normalized().dot( brushXz - point1Xz ) / dir.length();
			fWeight = Math::clamp( fWeight, ( real_t ) 0.0f, ( real_t ) 1.0f );
			const real_t fGradientHeight = Math::lerp( point_1.y, point_2.y, fWeight );
			fDestf = Math::lerp( fSrcf, fGradientHeight, CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
		}
		break;
	}
	default:
		break;
	}
	dest = Color( fDestf, 0.f, 0.f, 1.f );
	edited_position.y = fDestf;
	edited_area = edited_area.expand( edited_position );

	return true;
}

bool Terrain3DEditor::applyControlSample(
	t3dMapOperationState_t &rState,
	t3dBrushSample_t	   &rSample,
	Terrain3DSampler	   &brushSampler,
	real_t				   &angle ) {
	const real_t   strength = rState.m_fStrength;
	const bool	   bModifierAlt = rState.m_bModifierAlt;
	const bool	   bEnableTexture = rState.m_bEnableTexture;
	const uint32_t asset_id_bits = rState.m_nAssetIdBits;
	const bool	   bEnableAngle = rState.m_bEnableAngle;
	const bool	   bDynamicAngle = rState.m_bDynamicAngle;
	const bool	   bEnableScale = rState.m_bEnableScale;
	const real_t   fScale = rState.m_fScale;
	const Vector2 &clamped_slope_range = rState.m_clampedSlopeRange;
	const bool	   bUnrestrictedSlope = rState.m_bUnrestrictedSlope;
	const uint32_t scale_align[8] = { 5, 6, 7, 0, 1, 2, 3, 4 };
	const Vector3 &brushGlobalPosition = rSample.m_globalPosition;
	const Color	  &src = rSample.m_source;
	Color		  &dest = rSample.m_destination;
	const real_t   fBrushAlpha = rSample.m_fAlpha;
	// Get current bit field from pixel
	uint32_t base_id = t3dGetBase( src.r );
	uint32_t overlay_id = t3dGetOverlay( src.r );
	real_t	 fBlend = ( real_t ) ( t3dGetBlend( src.r ) ) / 255.f;
	uint32_t uvrotation = t3dGetUvRotation( src.r );
	uint32_t uvscale = t3dGetUvScale( src.r );
	bool	 bHole = t3dIsHole( src.r );
	bool	 bNavigation = t3dIsNav( src.r );
	bool	 bAutoshader = t3dIsAuto( src.r );
	switch ( m_tool ) {
	case TEXTURE: {
		if ( !isBrushSlopeAllowed( brushSampler, bUnrestrictedSlope, clamped_slope_range, brushGlobalPosition ) ) {
			return false;
		}
		switch ( m_nOperation ) {
		// Base Paint
		case REPLACE: {
			if ( fBrushAlpha > 0.5f ) {
				if ( bEnableTexture ) {
					// Set base & overlay texture
					base_id = asset_id_bits;
					overlay_id = asset_id_bits;
					// Erase blend value
					fBlend = 0.f;
					bAutoshader = false;
				}
				// Set angle & scale
				if ( base_id == asset_id_bits && bEnableAngle && !bAutoshader ) {
					if ( bDynamicAngle ) {
						// Angle from mouse movement.
						angle = Vector2( -m_operationMovement.x, m_operationMovement.z ).angle();
						// Avoid negative, align texture "up" with mouse direction.
						angle = ( real_t ) ( Math::fmod( Math::rad_to_deg( angle ) + 450.f, ( real_t ) ( 360.f ) ) );
					}
					// Convert from degrees to 0 - 15 value range
					uvrotation = ( uint32_t ) ( CLAMP( Math::round( angle / 22.5f ), 0.f, 15.f ) );
				}
				if ( base_id == asset_id_bits && bEnableScale && !bAutoshader ) {
					// Offset negative and convert from percentage to 0 - 7 bit value range
					// Maintain 0 = 0, remap negatives to end.
					uvscale = scale_align[( uint8_t ) ( CLAMP( Math::round( ( fScale + 60.f ) / 20.f ), 0.f, 7.f ) )];
				}
			}
			break;
		}

		// Add asset id, and increase weighting
		case ADD: {
			const real_t fSprayStrength = CLAMP( strength * 0.05f, 0.004f, .25f );
			const real_t fBrushValue = CLAMP( fBrushAlpha * fSprayStrength, 0.f, 1.f );
			if ( bEnableTexture && fBrushAlpha * strength * 11.f > 0.1f ) {
				// Manual texture painting owns this texel immediately. Leaving
				// AutoShader enabled until the new texture becomes dominant makes
				// the shader ignore the freshly written base/overlay IDs and creates
				// an apparent paint-order dependency between texture slots.
				bAutoshader = false;
				// Pick lowest weighted id, and lower to zero before setting new asset id.
				if ( asset_id_bits != base_id && asset_id_bits != overlay_id ) {
					if ( bModifierAlt ) {
						if ( fBlend < 0.5f ) {
							overlay_id = asset_id_bits;
						} else {
							base_id = asset_id_bits;
						}
					} else {
						if ( fBlend >= 0.5f ) {
							fBlend = CLAMP( fBlend + fBrushValue, 0.f, 1.f );
						} else {
							fBlend = CLAMP( fBlend - fBrushValue, 0.f, 1.f );
						}
						if ( fBlend <= 1.0f / 254.f ) {
							overlay_id = asset_id_bits;
						} else if ( fBlend >= ( 1.f - 1.0f / 254.f ) ) {
							base_id = asset_id_bits;
						}
					}
				}

				if ( base_id == asset_id_bits ) {
					fBlend = CLAMP( fBlend - fBrushValue, 0.f, 1.f );
					if ( fBrushAlpha > 0.5f && fBlend < 0.5f ) {
						bAutoshader = false;
					}
				}
				if ( overlay_id == asset_id_bits ) {
					fBlend = CLAMP( fBlend + fBrushValue, 0.f, 1.f );
					if ( fBrushAlpha > 0.5f && fBlend >= 0.5f ) {
						bAutoshader = false;
					}
				}
			}

			if ( ( base_id == asset_id_bits && fBlend < 0.5f ) || ( overlay_id == asset_id_bits && fBlend >= 0.5f ) ) {
				// Set angle & scale
				if ( bEnableAngle && !bAutoshader && fBrushAlpha > 0.5f ) {
					if ( bDynamicAngle ) {
						// Angle from mouse movement.
						angle = Vector2( -m_operationMovement.x, m_operationMovement.z ).angle();
						// Avoid negative, align texture "up" with mouse direction.
						angle = ( real_t ) ( Math::fmod( Math::rad_to_deg( angle ) + 450.f, ( real_t ) ( 360.f ) ) );
					}
					// Convert from degrees to 0 - 15 value range
					uvrotation = ( uint32_t ) ( CLAMP( Math::round( angle / 22.5f ), 0.f, 15.f ) );
				}
				if ( bEnableScale && !bAutoshader && fBrushAlpha > 0.5f ) {
					// Offset negative and convert from percentage to 0 - 7 bit value range
					// Maintain 0 = 0, remap negatives to end.
					uvscale = scale_align[( uint8_t ) ( CLAMP( Math::round( ( fScale + 60.f ) / 20.f ), 0.f, 7.f ) )];
				}
			}
			break;
		}

		// Lower weight of current asset id
		case SUBTRACT: {
			const real_t fSprayStrength = CLAMP( strength * 0.05f, 0.004f, .25f );
			const real_t fBrushValue = CLAMP( fBrushAlpha * fSprayStrength, 0.f, 1.f );
			if ( base_id == asset_id_bits ) {
				fBlend = CLAMP( fBlend + fBrushValue, 0.f, 1.f );
			}
			if ( overlay_id == asset_id_bits ) {
				fBlend = CLAMP( fBlend - fBrushValue, 0.f, 1.f );
			}
			break;
		}

		case AVERAGE: {
			const real_t fAvg = average( AVG_BLEND, brushGlobalPosition, src.r, 0.f, bModifierAlt ) / 255.f;
			fBlend = Math::lerp( fBlend, fAvg, CLAMP( fBrushAlpha * strength * 2.f, 0.f, 1.f ) );
			break;
		}

		default: {
			break;
		}
		}
		break;
	}
	case AUTOSHADER: {
		if ( fBrushAlpha > 0.5f ) {
			bAutoshader = ( m_nOperation == ADD );
			uvscale = 0.f;
			uvrotation = 0.f;
		}
		break;
	}
	case HOLES: {
		if ( fBrushAlpha > 0.5f ) {
			bHole = ( m_nOperation == ADD );
		}
		break;
	}
	case NAVIGATION: {
		if ( fBrushAlpha > 0.5f ) {
			bNavigation = ( m_nOperation == ADD );
		}
		break;
	}
	default: {
		break;
	}
	}

	// Convert back to bitfield
	const uint32_t blend_int = ( uint32_t ) ( CLAMP( Math::round( fBlend * 255.f ), 0.f, 255.f ) );
	const uint32_t bits = t3dEncBase( base_id ) | t3dEncOverlay( overlay_id ) | t3dEncBlend( blend_int ) |
						  t3dEncUvRotation( uvrotation ) | t3dEncUvScale( uvscale ) | t3dEncHole( bHole ) |
						  t3dEncNav( bNavigation ) | t3dEncAuto( bAutoshader );

	// Write back to pixel in FORMAT_RF. Must be a 32-bit float
	dest = Color( t3dAsFloat( bits ), 0.f, 0.f, 1.f );

	return true;
}

bool Terrain3DEditor::applyColorSample(
	t3dMapOperationState_t &rState,
	t3dBrushSample_t	   &rSample,
	Terrain3DSampler	   &brushSampler ) {
	const real_t		 strength = rState.m_fStrength;
	const Color			&color = rState.m_color;
	const real_t		 roughness = rState.m_fRoughness;
	const bool			 bTextureFilter = rState.m_bTextureFilter;
	const int			 margin = rState.m_nMargin;
	const int			 asset_id = rState.m_nAssetId;
	const Vector2		&clamped_slope_range = rState.m_clampedSlopeRange;
	const bool			 bUnrestrictedSlope = rState.m_bUnrestrictedSlope;
	t3dRegionMapCache_t &region_entry = rSample.m_entry;
	const size_t		 map_index = rSample.m_nMapIndex;
	const Vector2i		&mapPixelPosition = rSample.m_pixel;
	const Vector3		&brushGlobalPosition = rSample.m_globalPosition;
	const Color			&src = rSample.m_source;
	Color				&dest = rSample.m_destination;
	const real_t		 fBrushAlpha = rSample.m_fAlpha;
	// Filter by visible texture
	if ( bTextureFilter ) {
		const float fSrcCtrl = t3dAsFloat( readControlBits( region_entry, map_index, mapPixelPosition ) );
		const int	tex_id =
			  ( t3dGetBlend( fSrcCtrl ) > 110 - margin ) ? t3dGetOverlay( fSrcCtrl ) : t3dGetBase( fSrcCtrl );
		if ( tex_id != asset_id ) {
			return false;
		}
	}
	if ( !isBrushSlopeAllowed( brushSampler, bUnrestrictedSlope, clamped_slope_range, brushGlobalPosition ) ) {
		return false;
	}
	switch ( m_tool ) {
	case COLOR:
		switch ( m_nOperation ) {
		case ADD: {
			dest = src.lerp( color, CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
			dest.a = src.a;
			break;
		}
		case SUBTRACT: {
			dest = src.lerp( s_colorWhite, CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
			dest.a = src.a;
			break;
		}
		case AVERAGE: {
			const Color avg_col = average( brushGlobalPosition, src );
			dest = src.lerp( avg_col, CLAMP( fBrushAlpha * strength * 2.f, 0.f, 1.f ) );
			dest.a = src.a;
			break;
		}
		default:
			break;
		}
		break;
	case ROUGHNESS:
		/* Roughness received from UI is -100 to 100. Changed to 0,1 before storing.
			 * To convert 0,1 back to -100,100 use: 200 * (color.a - 0.5)
			 * However Godot stores values as 8-bit ints. Roundtrip is = int(a*255)/255.0
			 * Roughness 0 is saved as 0.5, but retreived is 0.498, or -0.4 roughness
			 * We round the final amount in tool_settings.gd:_on_picked().
			 */
		switch ( m_nOperation ) {
		case ADD: {
			const real_t fTarget = .5f + .5f * roughness;
			dest.a = Math::lerp( ( real_t ) ( src.a ), fTarget, CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
			dest.a = ( float ) ( ( int ) ( dest.a * 255.f ) ) /
					 255.f; // Quantize explicitly so picked values match painted values
			break;
		}
		case SUBTRACT: {
			dest.a = Math::lerp( ( real_t ) ( src.a ), ( real_t ) ( .5f ), CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
			dest.a = ( float ) ( ( int ) ( dest.a * 255.f ) ) / 255.f;
			break;
		}
		case AVERAGE: {
			const real_t fAvg = average( AVG_ROUGHNESS, brushGlobalPosition, src.a, 0.5f );
			dest.a = Math::lerp( ( real_t ) ( dest.a ), fAvg, CLAMP( fBrushAlpha * strength * 2.f, .0f, 1.f ) );
			dest.a = ( float ) ( ( int ) ( dest.a * 255.f ) ) / 255.f;
			break;
		}
		default:
			break;
		}
		break;
	default:
		break;
	}

	return true;
}

bool Terrain3DEditor::processMapOperationSamples(
	t3dMapOperationState_t &rState ) {
	const Vector3						 &rGlobalPosition = rState.m_globalPosition;
	const t3dMapType_t					  map_type = rState.m_mapType;
	const int							  region_size = rState.m_nRegionSize;
	const Vector2i						  region_vsize = rState.m_regionVSize;
	Terrain3DData *const				  pData = rState.m_pData;
	Image *const						  pBrushImage = rState.m_pBrushImage;
	const Vector2i						  img_size = rState.m_imageSize;
	const real_t						  fBrushSize = rState.m_fBrushSize;
	const real_t						  strength = rState.m_fStrength;
	const bool							  bTextureFilter = rState.m_bTextureFilter;
	const int							  margin = rState.m_nMargin;
	const int							  asset_id = rState.m_nAssetId;
	real_t								  angle = rState.m_fAngle;
	const real_t						  fGamma = rState.m_fGamma;
	PackedVector3Array					 &gradient_points = rState.m_gradientPoints;
	AABB								 &edited_area = rState.m_editedArea;
	const real_t						  fVertexSpacing = rState.m_fVertexSpacing;
	const bool							  bUseAlphaCache = rState.m_bUseAlphaCache;
	Terrain3DSampler					  brushSampler( pData->getSamplingSourceInternal() );
	const Vector2						 &clamped_slope_range = rState.m_clampedSlopeRange;
	const bool							  bUnrestrictedSlope = rState.m_bUnrestrictedSlope;
	const bool							  bUseBufferedBrush = rState.m_bUseBufferedBrush;
	const real_t						  fRotCos = rState.m_fRotationCos;
	const real_t						  fRotSin = rState.m_fRotationSin;
	const int32_t						  min_region_x = rState.m_nMinRegionX;
	const int32_t						  min_region_z = rState.m_nMinRegionZ;
	const int32_t						  region_cache_width = rState.m_nRegionCacheWidth;
	eastl::vector< t3dAxisSample_t >	 &x_samples = m_xAxisScratch;
	eastl::vector< t3dAxisSample_t >	 &z_samples = m_zAxisScratch;
	eastl::vector< t3dRegionMapCache_t > &region_cache = m_regionCacheScratch;
	eastl::vector< t3dBrushElement_t >	 &compute_elements = m_computeElementScratch;
	eastl::vector< t3dComputeWrite_t >	 &compute_writes = m_computeWriteScratch;
	// Terrain image rows are X-contiguous. Keep X in the inner loop so ordinary
	// stamps stream through map cachelines rather than revisiting one cacheline per
	// column after walking the full brush height.
	const real_t fCenteredUvStep = fVertexSpacing / fBrushSize;
	const real_t fRotatedUStep = fCenteredUvStep * fRotCos;
	const real_t fRotatedVStep = fCenteredUvStep * fRotSin;
	for ( size_t iSampleZ = 0u; iSampleZ < ( size_t ) ( ( z_samples ).size() ); iSampleZ++ ) {
		const t3dAxisSample_t &z_sample = ( z_samples )[iSampleZ];
		real_t fRowU = x_samples.front().m_fCenteredUv * fRotCos - z_sample.m_fCenteredUv * fRotSin + .5f;
		real_t fRowV = x_samples.front().m_fCenteredUv * fRotSin + z_sample.m_fCenteredUv * fRotCos + .5f;
		for ( size_t iSampleX = 0u; iSampleX < ( size_t ) ( ( x_samples ).size() ); iSampleX++ ) {
			const t3dAxisSample_t &x_sample = ( x_samples )[iSampleX];
			const real_t		   fRotatedU = CLAMP( fRowU, 0.f, 1.f );
			const real_t		   fRotatedV = CLAMP( fRowV, 0.f, 1.f );
			fRowU += fRotatedUStep;
			fRowV += fRotatedVStep;
			const Vector2i brush_pixel_position = Vector2i( fRotatedU * img_size.x, fRotatedV * img_size.y );
			if ( !isInBounds( brush_pixel_position, img_size ) ) {
				continue;
			}

			real_t fBrushAlpha;
			if ( bUseAlphaCache ) {
				fBrushAlpha =
					m_fBrushAlphaCache[( size_t ) ( brush_pixel_position.y * img_size.x + brush_pixel_position.x )];
			} else {
				fBrushAlpha = pBrushImage->get_pixelv( brush_pixel_position ).r;
				fBrushAlpha = ( real_t ) ( Math::pow( ( double ) ( fBrushAlpha ), ( double ) ( fGamma ) ) );
				fBrushAlpha = isnan( fBrushAlpha ) ? 0.f : fBrushAlpha;
			}
			if ( fBrushAlpha <= CMP_EPSILON ) {
				continue;
			}

			const size_t region_cache_index =
				( size_t ) ( z_sample.m_nRegionLocation - min_region_z ) * ( size_t ) ( region_cache_width ) +
				( size_t ) ( x_sample.m_nRegionLocation - min_region_x );
			t3dRegionMapCache_t &region_entry = region_cache[region_cache_index];
			if ( !region_entry.m_pMap ) {
				continue;
			}
			Ref< Terrain3DRegion > &region = region_entry.m_region;

			const Vector2i mapPixelPosition( x_sample.m_nMapPixel, z_sample.m_nMapPixel );
			if ( !isInBounds( mapPixelPosition, region_vsize ) ) {
				continue;
			}

			const Vector2 brushOffset( x_sample.m_fOffset, z_sample.m_fOffset );
			const Vector3 brushGlobalPosition( x_sample.m_fGlobalPosition,
											   rGlobalPosition.y,
											   z_sample.m_fGlobalPosition );
			Vector3		  edited_position = brushGlobalPosition;
			if ( map_type != TYPE_HEIGHT ) {
				edited_position.y = brushSampler.sampleHeight( edited_position );
				edited_area = edited_area.expand( edited_position );
			}

			const size_t map_index =
				( size_t ) ( mapPixelPosition.y ) * ( size_t ) ( region_size ) + ( size_t ) ( mapPixelPosition.x );
			const Color src = readMapPixel( region_entry, map_index, mapPixelPosition, map_type );
			Color		dest = src;
			if ( bUseBufferedBrush ) {
				bool	 bEligible = true;
				uint32_t source_value = 0u;
				uint32_t target_value = 0u;
				if ( map_type == TYPE_HEIGHT ) {
					const float fSourceHeight = isfinite( src.r ) ? src.r : 0.0f;
					source_value = t3dAsUint( fSourceHeight );
					edited_position.y = fSourceHeight;
					edited_area = edited_area.expand( edited_position );
					if ( m_nOperation == GRADIENT ) {
						if ( gradient_points.size() != 2 ) {
							bEligible = false;
						} else {
							const Vector3 point_1 = gradient_points[0];
							const Vector3 point_2 = gradient_points[1];
							const Vector2 point1Xz( point_1.x, point_1.z );
							const Vector2 direction = Vector2( point_2.x, point_2.z ) - point1Xz;
							if ( direction.length_squared() < 0.01f ) {
								bEligible = false;
							} else {
								Vector2 brushXz( brushGlobalPosition.x, brushGlobalPosition.z );
								if ( m_operationMovement.length_squared() > 0.f ) {
									const Vector2 movement_xz =
										Vector2( m_operationMovement.x, m_operationMovement.z ).normalized();
									const Vector2 offset = movement_xz * brushOffset.dot( movement_xz );
									brushXz = Vector2( rGlobalPosition.x + offset.x, rGlobalPosition.z + offset.y );
								}
								const real_t fWeight =
									CLAMP( direction.normalized().dot( brushXz - point1Xz ) / direction.length(),
										   0.0f,
										   1.0f );
								target_value = t3dAsUint( ( float ) ( Math::lerp( point_1.y, point_2.y, fWeight ) ) );
							}
						}
					}
				} else if ( map_type == TYPE_CONTROL ) {
					source_value = t3dAsUint( src.r );
					if ( m_tool == TEXTURE ) {
						bEligible = isBrushSlopeAllowed( brushSampler,
														 bUnrestrictedSlope,
														 clamped_slope_range,
														 brushGlobalPosition );
					}
				} else {
					source_value = packColorRgba8( src );
					if ( bTextureFilter ) {
						const float fControl =
							t3dAsFloat( readControlBits( region_entry, map_index, mapPixelPosition ) );
						const int texture_id = ( t3dGetBlend( fControl ) > 110 - margin ) ? t3dGetOverlay( fControl )
																						  : t3dGetBase( fControl );
						bEligible = texture_id == asset_id;
					}
					if ( bEligible ) {
						bEligible = isBrushSlopeAllowed( brushSampler,
														 bUnrestrictedSlope,
														 clamped_slope_range,
														 brushGlobalPosition );
					}
				}
				if ( bEligible ) {
					t3dBrushElement_t element;
					element.m_nValue = source_value;
					element.m_nAlpha = t3dAsUint( ( float ) ( fBrushAlpha ) );
					element.m_nTarget = target_value;
					element.m_nFlags = 1u;
					compute_elements.push_back( element );
					compute_writes.push_back( { region_cache_index, mapPixelPosition, source_value, edited_position } );
				}
				continue;
			}
			t3dBrushSample_t sample{ region_entry,
									 map_index,
									 mapPixelPosition,
									 brushOffset,
									 brushGlobalPosition,
									 edited_position,
									 src,
									 dest,
									 fBrushAlpha };
			if ( map_type == TYPE_HEIGHT ) {
				if ( !applyHeightSample( rState, sample ) ) {
					return false;
				}
			} else if ( map_type == TYPE_CONTROL ) {
				if ( !applyControlSample( rState, sample, brushSampler, angle ) ) {
					continue;
				}
			} else if ( map_type == TYPE_COLOR ) {
				if ( !applyColorSample( rState, sample, brushSampler ) ) {
					continue;
				}
			}
			if ( dest == src ) {
				continue;
			}
			backupRegion( region, region_entry.m_bMapWasImplicit );
			recordBrushUndoTile( region_entry, mapPixelPosition, map_type );
			if ( map_type == TYPE_CONTROL && t3dIsHole( src.r ) != t3dIsHole( dest.r ) ) {
				rState.m_bHoleChanged = true;
			}
			writeMapPixel( region_entry, map_index, mapPixelPosition, dest, map_type );
			if ( map_type == TYPE_HEIGHT ) {
				region->updateHeight( dest.r );
				pData->updateMasterHeight( dest.r );
			}
			region_entry.m_bModified = true;
		}
	}

	return true;
}

void Terrain3DEditor::applyComputedMapOperationSamples(
	t3dMapOperationState_t &rState ) {
	const Vector3						 &rGlobalPosition = rState.m_globalPosition;
	const t3dMapType_t					  map_type = rState.m_mapType;
	const int							  region_size = rState.m_nRegionSize;
	Terrain3DData *const				  pData = rState.m_pData;
	const real_t						  strength = rState.m_fStrength;
	const real_t						  fHeight = rState.m_fHeight;
	const Color							 &color = rState.m_color;
	const real_t						  roughness = rState.m_fRoughness;
	const bool							  bModifierAlt = rState.m_bModifierAlt;
	const bool							  bEnableTexture = rState.m_bEnableTexture;
	const bool							  bEnableAngle = rState.m_bEnableAngle;
	const bool							  bEnableScale = rState.m_bEnableScale;
	const uint32_t						  asset_id_bits = rState.m_nAssetIdBits;
	const bool							  bUseBufferedBrush = rState.m_bUseBufferedBrush;

	const uint32_t						  compute_uv_rotation = rState.m_nComputeUvRotation;
	const uint32_t						  compute_uv_scale = rState.m_nComputeUvScale;
	AABB								 &edited_area = rState.m_editedArea;
	eastl::vector< t3dBrushElement_t >	 &compute_elements = m_computeElementScratch;
	eastl::vector< t3dComputeWrite_t >	 &compute_writes = m_computeWriteScratch;
	eastl::vector< t3dRegionMapCache_t > &region_cache = m_regionCacheScratch;
	if ( bUseBufferedBrush && !compute_elements.empty() ) {
		t3dBrushParams_t params;
		params.m_nMapType = ( uint32_t ) ( map_type );
		params.m_nTool = ( uint32_t ) ( m_tool );
		params.m_nOperation = ( uint32_t ) ( m_nOperation );
		params.m_fStrength = ( float ) ( strength );
		params.m_fSelectedHeight = ( float ) ( fHeight );
		params.m_fBrushCenterHeight = ( float ) ( rGlobalPosition.y );
		params.m_fRoughness = ( float ) ( roughness );
		params.m_nColorRgba8 = packColorRgba8( color );
		params.m_nAssetId = asset_id_bits;
		params.m_nUvRotation = compute_uv_rotation;
		params.m_nUvScale = compute_uv_scale;
		params.m_nFeatureFlags = ( bModifierAlt ? 1u : 0u ) | ( isfinite( rGlobalPosition.y ) ? 2u : 0u ) |
								 ( bEnableTexture ? 4u : 0u ) | ( bEnableAngle ? 8u : 0u ) |
								 ( bEnableScale ? 16u : 0u );
		// Reused CPU brush-element scratch preserves immediate clipmap updates.
		// This is the explicit fallback until a renderer-owned GPU preview and
		// tile completion path is available; no local GPU device is submitted here.
		Terrain3DCompute::processBrushCpu( compute_elements, params );
		for ( size_t i = 0; i < compute_elements.size(); ++i ) {
			const t3dBrushElement_t &element = compute_elements[i];
			const t3dComputeWrite_t &write = compute_writes[i];
			if ( element.m_nValue == write.m_nSourceValue || write.m_iRegionIndex >= region_cache.size() ) {
				continue;
			}
			t3dRegionMapCache_t &entry = region_cache[write.m_iRegionIndex];
			if ( !entry.m_pMap || entry.m_region.is_null() ) {
				continue;
			}
			backupRegion( entry.m_region, entry.m_bMapWasImplicit );
			recordBrushUndoTile( entry, write.m_mapPixel, map_type );
			const size_t map_index =
				( size_t ) ( write.m_mapPixel.y ) * ( size_t ) ( region_size ) + ( size_t ) ( write.m_mapPixel.x );
			if ( map_type == TYPE_HEIGHT ) {
				const float fResult = t3dAsFloat( element.m_nValue );
				writeMapPixel( entry, map_index, write.m_mapPixel, Color( fResult, 0.f, 0.f, 1.f ), map_type );
				entry.m_region->updateHeight( fResult );
				pData->updateMasterHeight( fResult );
				Vector3 result_position = write.m_worldPosition;
				result_position.y = fResult;
				edited_area = edited_area.expand( result_position );
			} else if ( map_type == TYPE_CONTROL ) {
				if ( t3dIsHole( write.m_nSourceValue ) != t3dIsHole( element.m_nValue ) ) {
					rState.m_bHoleChanged = true;
				}
				writeMapPixel( entry,
							   map_index,
							   write.m_mapPixel,
							   Color( t3dAsFloat( element.m_nValue ), 0.f, 0.f, 1.f ),
							   map_type );
			} else {
				writeMapPixel( entry, map_index, write.m_mapPixel, unpackColorRgba8( element.m_nValue ), map_type );
			}
			entry.m_bModified = true;
		}
	}
}

void Terrain3DEditor::commitMapOperationSamples(
	t3dMapOperationState_t &rState ) {
	const t3dMapType_t					  map_type = rState.m_mapType;
	const int							  region_size = rState.m_nRegionSize;
	Terrain3DData *const				  pData = rState.m_pData;
	AABB								 &edited_area = rState.m_editedArea;
	eastl::vector< t3dRegionMapCache_t > &region_cache = m_regionCacheScratch;
	// Publish detached row-major byte planes once per modified region. This keeps
	// the hot brush loop on raw contiguous storage while preserving Image resource
	// identity for the renderer, undo snapshots and external references.
	for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( region_cache ).size() ); iRegion++ ) {
		t3dRegionMapCache_t &entry = ( region_cache )[iRegion];
		if ( entry.m_bModified && entry.m_bDirectAccess && entry.m_pMap && !entry.m_mapData.is_empty() ) {
			++m_nBrushSetDataCalls;
			m_nBrushBytesPublished += ( uint64_t ) entry.m_mapData.size();
			entry.m_pMap->set_data( region_size,
									region_size,
									entry.m_bMapHasMipmaps,
									entry.m_mapFormat,
									entry.m_mapData );
		}
		if ( entry.m_bMapWasImplicit && !entry.m_bModified && entry.m_region.is_valid() &&
			 ( map_type == TYPE_CONTROL || map_type == TYPE_COLOR ) ) {
			// Region coverage is resolved before slope/texture filters are evaluated.
			// Drop transient default maps for covered regions that received no write.
			entry.m_region->compactMapIfDefault( map_type, false );
		}
	}

	m_modifiedRegionScratch.clear();
	m_modifiedRegionScratch.reserve( region_cache.size() );
	eastl::vector< Vector2i > &modified_region_locations = m_modifiedRegionScratch;
	for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( region_cache ).size() ); iRegion++ ) {
		t3dRegionMapCache_t &entry = ( region_cache )[iRegion];
		if ( !entry.m_bModified || entry.m_region.is_null() ) {
			continue;
		}
		modified_region_locations.push_back( entry.m_location );
	}

	uint32_t commit_flags = MAP_EDIT_KEEP_EDITED | MAP_EDIT_INTERACTIVE;
	if ( m_tool == HOLES || m_tool == HEIGHT || m_tool == SCULPT ) {
		commit_flags |= MAP_EDIT_UPDATE_INSTANCES;
	}
	if ( map_type == TYPE_HEIGHT || ( map_type == TYPE_CONTROL && rState.m_bHoleChanged ) ) {
		commit_flags |= MAP_EDIT_UPDATE_COLLISION;
	}
	if ( m_tool == HEIGHT || m_tool == SCULPT || m_tool == TEXTURE || m_tool == AUTOSHADER ) {
		commit_flags |= MAP_EDIT_SNAP;
	}
	// One typed commit owns persistence metadata, height ranges, texture-array
	// uploads, instance/collision invalidation and coalesced notifications.
	pData->commitMapEdits( map_type, modified_region_locations, edited_area, commit_flags );
}

#if defined( T3D_EDITOR_NATIVE )
// BRP-07: prevalidated source-owned packed tile publication. The GPU
// producer must keep the stroke open until the async readback is ready;
// ordinary CPU painting continues through commitMapOperationSamples().
typedef struct t3dEditorBrushCommitStage_s
{
	Vector2i m_location = s_v2iZero;
	Ref< Terrain3DRegion > m_region;
	Ref< Image > m_image;
	PackedByteArray m_before;
	PackedByteArray m_after;
	bool m_bWasImplicit = false;
	bool m_bChanged = false;
	bool m_bHoleChanged = false;
} t3dEditorBrushCommitStage_t;

static bool t3dStageEditorBrushTilesCold(
	Terrain3DData *const pData,
	const Terrain3DEditorBrushTransaction &rTransaction,
	const PackedByteArray &rPayload,
	const t3dMapType_t mapType,
	eastl::vector< t3dEditorBrushCommitStage_t > &rStages )
{
	const uint32_t nTileCount = rTransaction.tileCount();
	const Image::Format format = mapType == TYPE_HEIGHT ? Image::FORMAT_R16 :
		mapType == TYPE_CONTROL ? Image::FORMAT_RF : Image::FORMAT_RGBA8;
	const size_t nPixelSize = Terrain3DEditorBrushTransaction::bytesPerTexel( ( uint8_t ) mapType );
	const size_t nMapBytes = ( size_t ) T3D_REGION_SIZE * T3D_REGION_SIZE * nPixelSize;
	const uint8_t *const pPayload = rPayload.ptr();
	// Validate every tile and stage complete region byte planes before touching
	// authoritative Images or allocating an implicit writable map.
	for ( uint32_t i = 0u; i < nTileCount; ++i )
	{
		const t3dEditorBrushTile_t &tile = rTransaction.tile( i );
		if ( tile.m_nEncoding != ( uint8_t ) mapType || !tile.m_nWidth || !tile.m_nHeight ||
			 tile.m_nMinX + tile.m_nWidth > T3D_REGION_SIZE ||
			 tile.m_nMinY + tile.m_nHeight > T3D_REGION_SIZE ||
			 tile.m_nRowStrideBytes != tile.m_nWidth * nPixelSize ||
			 tile.m_nByteOffset > ( uint32_t ) rPayload.size() ||
			 ( size_t ) tile.m_nRowStrideBytes * tile.m_nHeight >
			 ( size_t ) rPayload.size() - tile.m_nByteOffset ) return false;
		const Vector2i location( tile.m_nRegionX, tile.m_nRegionY );
		int32_t nStage = -1;
		for ( size_t j = 0u; j < rStages.size(); ++j )
		{
			if ( rStages[j].m_location == location ) { nStage = ( int32_t ) j; break; }
		}
		if ( nStage < 0 )
		{
			const Ref< Terrain3DRegion > region = pData->getRegion( location );
			if ( region.is_null() || region->isDeleted() || pData->getRegionId( location ) < 0 ||
				 region->getAuthoringSourceRevision() != tile.m_nSourceRevision ) return false;
			t3dEditorBrushCommitStage_t stage;
			stage.m_location = location;
			stage.m_region = region;
			stage.m_image = region->getMap( mapType );
			stage.m_bWasImplicit = stage.m_image.is_null();
			if ( stage.m_bWasImplicit )
			{
				if ( mapType == TYPE_HEIGHT ) return false;
				stage.m_image = Terrain3DUtil::getFilledImage( V2I( T3D_REGION_SIZE ),
					 mapType == TYPE_CONTROL ? COLOR_CONTROL : COLOR_ROUGHNESS,
					 false, format );
			}
			if ( stage.m_image.is_null() || stage.m_image->get_width() != T3D_REGION_SIZE ||
				 stage.m_image->get_height() != T3D_REGION_SIZE || stage.m_image->get_format() != format ||
				 stage.m_image->has_mipmaps() ) return false;
			stage.m_before = stage.m_image->get_data();
			if ( ( size_t ) stage.m_before.size() != nMapBytes ) return false;
			stage.m_after = stage.m_before;
			rStages.push_back( stage );
			nStage = ( int32_t ) rStages.size() - 1;
		}
		else if ( rStages[( size_t ) nStage].m_region->getAuthoringSourceRevision() != tile.m_nSourceRevision ) return false;
		t3dEditorBrushCommitStage_t &stage = rStages[( size_t ) nStage];
		if ( !Terrain3DEditorBrushTransaction::stageTileBytes( tile, pPayload,
			( uint32_t ) rPayload.size(), stage.m_before.ptr(), stage.m_after.ptrw(),
			( uint32_t ) nMapBytes, stage.m_bChanged, stage.m_bHoleChanged ) ) return false;
	}

	return true;
}

static bool t3dMaterializeEditorBrushTilesCold(
	eastl::vector< t3dEditorBrushCommitStage_t > &rStages,
	const t3dMapType_t mapType )
{
	const Image::Format format = mapType == TYPE_HEIGHT ? Image::FORMAT_R16 :
		mapType == TYPE_CONTROL ? Image::FORMAT_RF : Image::FORMAT_RGBA8;
	// The writable maps are materialized only after the complete payload and
	// every source revision have passed validation.
	for ( size_t i = 0u; i < rStages.size(); ++i )
	{
		t3dEditorBrushCommitStage_t &stage = rStages[i];
		if ( !stage.m_bChanged ) continue;
		Image *const pWritable = stage.m_region->getMapPtrForWrite( mapType );
		if ( !pWritable || pWritable->get_format() != format ||
			 pWritable->get_width() != T3D_REGION_SIZE || pWritable->get_height() != T3D_REGION_SIZE )
		{
			// This failure occurs before any source byte plane is published.
			for ( size_t j = 0u; j < rStages.size(); ++j )
			{
				if ( rStages[j].m_bWasImplicit ) rStages[j].m_region->compactMapIfDefault( mapType, false );
			}
			return false;
		}
	}
	return true;
}

bool Terrain3DEditor::publishEditorBrushReadbackCold( const uint64_t nStrokeId )
{
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) return false;
	Terrain3D *const pTerrain = static_cast< Terrain3D * >( m_pTerrain );
	Terrain3DRenderCoordinator *const pRenderer = pTerrain ? pTerrain->getRenderCoordinatorInternal() : NULL;
	if ( !pRenderer ) return false;
	Terrain3DEditorBrushTransaction transaction;
	PackedByteArray payload;
	if ( !pRenderer->takeEditorBrushReadbackCold( nStrokeId, transaction, payload ) ) return false;
	if ( !m_bIsOperating || transaction.ownerGeneration() != m_nEditorBrushOwnerGeneration ||
		 transaction.state() != T3D_EDITOR_BRUSH_CPU_COMMITTING ||
		 payload.size() != ( int64_t ) transaction.payloadBytes() )
	{
		pRenderer->cancelEditorBrushStrokeCold( nStrokeId );
		return false;
	}

	Terrain3DData *const pData = m_pTerrain->dataObject();
	const uint32_t nTileCount = transaction.tileCount();
	if ( nTileCount == 0u )
	{
		pRenderer->cancelEditorBrushStrokeCold( nStrokeId );
		return false;
	}
	const t3dMapType_t mapType = ( t3dMapType_t ) transaction.tile( 0u ).m_nEncoding;
	if ( mapType < TYPE_HEIGHT || mapType >= REGION_TYPE_MAX || mapType != getMapType() )
	{
		pRenderer->cancelEditorBrushStrokeCold( nStrokeId );
		return false;
	}
	const Image::Format format = mapType == TYPE_HEIGHT ? Image::FORMAT_R16 :
		 mapType == TYPE_CONTROL ? Image::FORMAT_RF : Image::FORMAT_RGBA8;
	eastl::vector< t3dEditorBrushCommitStage_t > stages;
	stages.reserve( nTileCount );

	if ( !t3dStageEditorBrushTilesCold( pData, transaction, payload, mapType, stages ) ||
		 !t3dMaterializeEditorBrushTilesCold( stages, mapType ) )
	{
		pRenderer->cancelEditorBrushStrokeCold( nStrokeId );
		return false;
	}

	AABB editedArea;
	bool bHasArea = false;
	bool bHoleChanged = false;
	eastl::vector< Vector2i > changedLocations;
	const real_t spacing = m_pTerrain->getVertexSpacing();
	const real_t side = spacing * T3D_REGION_SIZE;
	for ( size_t i = 0u; i < stages.size(); ++i )
	{
		t3dEditorBrushCommitStage_t &stage = stages[i];
		if ( !stage.m_bChanged ) continue;
		t3dRegionMapCache_t undoEntry;
		undoEntry.m_region = stage.m_region;
		undoEntry.m_pMap = stage.m_region->getMapPtrForWrite( mapType );
		undoEntry.m_bMapWasImplicit = stage.m_bWasImplicit;
		undoEntry.m_location = stage.m_location;
		for ( uint32_t t = 0u; t < nTileCount; ++t )
		{
			const t3dEditorBrushTile_t &tile = transaction.tile( t );
			if ( tile.m_nRegionX != stage.m_location.x || tile.m_nRegionY != stage.m_location.y ) continue;
			for ( uint32_t y = ( tile.m_nMinY / T3D_BRUSH_UNDO_TILE_SIZE ) * T3D_BRUSH_UNDO_TILE_SIZE;
				 y < tile.m_nMinY + tile.m_nHeight; y += T3D_BRUSH_UNDO_TILE_SIZE )
			{
				for ( uint32_t x = ( tile.m_nMinX / T3D_BRUSH_UNDO_TILE_SIZE ) * T3D_BRUSH_UNDO_TILE_SIZE;
					 x < tile.m_nMinX + tile.m_nWidth; x += T3D_BRUSH_UNDO_TILE_SIZE )
				{
					recordBrushUndoTile( undoEntry, Vector2i( x, y ), mapType );
				}
			}
			const Vector3 minPoint( stage.m_location.x * side + tile.m_nMinX * spacing,
				 -1.f, stage.m_location.y * side + tile.m_nMinY * spacing );
			const Vector3 maxPoint( minPoint.x + tile.m_nWidth * spacing, 1.f,
				 minPoint.z + tile.m_nHeight * spacing );
			if ( !bHasArea ) { editedArea = AABB( minPoint, maxPoint - minPoint ); bHasArea = true; }
			else { editedArea = editedArea.expand( minPoint ).expand( maxPoint ); }
		}
		++m_nBrushSetDataCalls;
		m_nBrushBytesPublished += ( uint64_t ) stage.m_after.size();
		undoEntry.m_pMap->set_data( T3D_REGION_SIZE, T3D_REGION_SIZE, false, format, stage.m_after );
		changedLocations.push_back( stage.m_location );
		bHoleChanged |= stage.m_bHoleChanged;
	}
	if ( !changedLocations.empty() )
	{
		uint32_t flags = MAP_EDIT_KEEP_EDITED | MAP_EDIT_INTERACTIVE;
		if ( mapType == TYPE_HEIGHT || bHoleChanged ) flags |= MAP_EDIT_UPDATE_COLLISION;
		if ( mapType == TYPE_HEIGHT || bHoleChanged ) flags |= MAP_EDIT_UPDATE_INSTANCES;
		if ( m_tool == HEIGHT || m_tool == SCULPT || m_tool == TEXTURE || m_tool == AUTOSHADER )
			flags |= MAP_EDIT_SNAP;
		// Control/color may be compacted to an implicit map during commit.
		// Capture their redo tiles while the authored Image still exists.
		if ( mapType != TYPE_HEIGHT ) finishBrushUndoTiles();
		pData->addEditedArea( editedArea );
		pData->commitMapEdits( mapType, changedLocations, editedArea, flags );
		if ( mapType != TYPE_HEIGHT )
		{
			for ( int32_t nSnapshot = 0; nSnapshot < m_editedEditSnapshots.size(); ++nSnapshot )
			{
				Dictionary redo = m_editedEditSnapshots[nSnapshot];
				if ( !redo.has( "location" ) || ( int ) redo.get( "map_type", -1 ) != ( int ) mapType ) continue;
				const Vector2i location = redo["location"];
				const Ref< Terrain3DRegion > region = pData->getRegion( location );
				if ( region.is_valid() && region->getMap( mapType ).is_null() )
				{
					redo.erase( "map_tiles" );
					redo["map_absent"] = true;
					m_editedEditSnapshots[nSnapshot] = redo;
				}
			}
		}
	}
	// Always finalize the undo history after map publication, even if the
	// renderer was lost between publication and its lifecycle acknowledgment.
	const bool bAcknowledged = pRenderer->acknowledgeEditorBrushCommitCold( nStrokeId );
	if ( !bAcknowledged ) {
		// Source maps were already published. A device teardown may invalidate
		// only the renderer acknowledgement; CPU authority and undo still win.
		LOG( ERROR, "Editor brush source commit outlived renderer acknowledgment" );
	}
	stopOperation();
	return true;
}
// BRP-06: associate an initialized renderer-owned transaction with this
// editor stroke. A second stroke cannot overtake a pending authoritative map
// publication. The producer calls this only after beginEditorBrushStrokeCold.
bool Terrain3DEditor::acceptEditorBrushStrokeCold( const uint64_t nStrokeId )
{
    if ( !nStrokeId || !m_bIsOperating || m_nPendingEditorBrushStrokeId || !m_pTerrain ) return false;
    Terrain3DRenderCoordinator *const pRenderer = static_cast< Terrain3D * >( m_pTerrain )->getRenderCoordinatorInternal();
    if ( !pRenderer || pRenderer->getEditorBrushStrokeStateCold( nStrokeId ) != T3D_EDITOR_BRUSH_ACTIVE ) return false;
    m_nPendingEditorBrushStrokeId = nStrokeId;
    m_bPendingEditorBrushSealed = false;
    return true;
}

void Terrain3DEditor::pollEditorBrushCommitCold( void )
{
    const uint64_t nStrokeId = m_nPendingEditorBrushStrokeId;
    if ( !nStrokeId || !m_bPendingEditorBrushSealed || !m_pTerrain ) return;
    Terrain3DRenderCoordinator *const pRenderer = static_cast< Terrain3D * >( m_pTerrain )->getRenderCoordinatorInternal();
    if ( !pRenderer )
    {
        m_nPendingEditorBrushStrokeId = 0u;
        m_bPendingEditorBrushSealed = false;
        cancelOperation();
        return;
    }
    switch ( pRenderer->getEditorBrushStrokeStateCold( nStrokeId ) )
    {
        case T3D_EDITOR_BRUSH_READBACK_READY:
        {
            // Clear the pending token before the owner-side publication calls
            // stopOperation(). The claim-once transition prevents duplicate commits.
            m_nPendingEditorBrushStrokeId = 0u;
            m_bPendingEditorBrushSealed = false;
            if ( !publishEditorBrushReadbackCold( nStrokeId ) ) cancelOperation();
            return;
        }
        case T3D_EDITOR_BRUSH_FAILED:
        case T3D_EDITOR_BRUSH_CANCELED:
        case T3D_EDITOR_BRUSH_IDLE:
        {
            pRenderer->cancelEditorBrushStrokeCold( nStrokeId );
            m_nPendingEditorBrushStrokeId = 0u;
            m_bPendingEditorBrushSealed = false;
            cancelOperation();
            return;
        }
        default: return; // Submit/dispatch/readback remains asynchronous.
    }
}
#endif

#if defined( T3D_EDITOR_NATIVE )
// BRP-05: control occupancy edits use a bounded transient GPU byte plane.
// All source tiles are snapshotted once and the canonical Image is left untouched
// until seal/readback on the editor owner thread. Other tools use the CPU path.
bool Terrain3DEditor::operateGpuTilesCold( const t3dMapOperationState_t &rState )
{
    const bool control = rState.m_mapType == TYPE_CONTROL &&
        ( m_tool == HOLES || m_tool == NAVIGATION || m_tool == AUTOSHADER );
    const bool height = rState.m_mapType == TYPE_HEIGHT && ( m_tool == SCULPT || m_tool == HEIGHT );
    const bool texture = rState.m_mapType == TYPE_CONTROL && m_tool == TEXTURE &&
        rState.m_slopeRange.y - rState.m_slopeRange.x > 89.99f && !rState.m_bDynamicAngle;
    const bool color = rState.m_mapType == TYPE_COLOR && ( m_tool == COLOR || m_tool == ROUGHNESS ) &&
        !rState.m_bTextureFilter && rState.m_slopeRange.y - rState.m_slopeRange.x > 89.99f;
    bool gradientValid = false;
    if ( height && m_nOperation == GRADIENT && rState.m_gradientPoints.size() == 2 ) {
        const Vector3 direction = rState.m_gradientPoints[1] - rState.m_gradientPoints[0];
        gradientValid = direction.x * direction.x + direction.z * direction.z >= .01f;
    }
    if ( ( !control && !height && !color && !texture ) ||
         ( m_nOperation != ADD && m_nOperation != SUBTRACT &&
           !( texture && m_nOperation == REPLACE ) && !gradientValid ) ||
         m_bEditorBrushGpuRejected || rState.m_bModifierAlt ||
         ( m_nBrushDabs && !m_nPendingEditorBrushStrokeId ) ) return false;
    if ( m_nPendingEditorBrushStrokeId &&
         ( m_nEditorBrushGpuMapType != ( int32_t ) rState.m_mapType ||
           m_editorBrushGpuMaskSize != rState.m_imageSize ||
           m_pEditorBrushGpuMaskImage != rState.m_pBrushImage ||
           !Math::is_equal_approx( m_fEditorBrushGpuMaskGamma, ( float ) rState.m_fGamma ) ) ) return false;
    if ( rState.m_fBrushSize < rState.m_fVertexSpacing * 16.f ) return false;
    Terrain3D *const pTerrain = static_cast< Terrain3D * >( m_pTerrain );
    Terrain3DRenderCoordinator *const pRenderer = pTerrain->getRenderCoordinatorInternal();
    if ( !pRenderer || !Terrain3DRdContext::getShared().isReady() ||
         rState.m_imageSize.x <= 0 || rState.m_imageSize.y <= 0 ||
         uint64_t( rState.m_imageSize.x ) * rState.m_imageSize.y > 1024u * 1024u ) return false;
    const uint32_t pixelSize = height ? 2u : 4u;
    const float spacing = ( float ) rState.m_fVertexSpacing;
    const float half = ( float ) rState.m_fBrushSize * .5f;
    const int32_t minX = ( int32_t ) Math::floor( ( rState.m_globalPosition.x - half ) / spacing ) - 1;
    const int32_t maxX = ( int32_t ) Math::ceil( ( rState.m_globalPosition.x + half ) / spacing ) + 1;
    const int32_t minY = ( int32_t ) Math::floor( ( rState.m_globalPosition.z - half ) / spacing ) - 1;
    const int32_t maxY = ( int32_t ) Math::ceil( ( rState.m_globalPosition.z + half ) / spacing ) + 1;
    const int32_t firstRegionX = ( int32_t ) Math::floor( ( float ) minX / T3D_REGION_SIZE );
    const int32_t lastRegionX = ( int32_t ) Math::floor( ( float ) maxX / T3D_REGION_SIZE );
    const int32_t firstRegionY = ( int32_t ) Math::floor( ( float ) minY / T3D_REGION_SIZE );
    const int32_t lastRegionY = ( int32_t ) Math::floor( ( float ) maxY / T3D_REGION_SIZE );
    if ( lastRegionX - firstRegionX > 2 || lastRegionY - firstRegionY > 2 ) return false;

    eastl::vector< t3dEditorBrushTile_t > fresh;
    eastl::vector< PackedByteArray > freshBytes;
    uint32_t nOffset = 0u;
    for ( uint32_t i = 0u; i < m_editorBrushGpuTiles.size(); ++i )
        nOffset += m_editorBrushGpuTiles[i].m_nRowStrideBytes * m_editorBrushGpuTiles[i].m_nHeight;
    Terrain3DData *const pData = pTerrain->dataObject();
    for ( int32_t regionY = firstRegionY; regionY <= lastRegionY; ++regionY )
    for ( int32_t regionX = firstRegionX; regionX <= lastRegionX; ++regionX )
    {
        const Vector2i location( regionX, regionY );
        const Ref< Terrain3DRegion > region = pData->getRegion( location );
        if ( region.is_null() || region->isDeleted() || !pData->ensureCpuRegionResident( location ) )
        {
            if ( m_nPendingEditorBrushStrokeId ) { cancelOperation(); return true; }
            return false; // Existing CPU path handles automatic regions.
        }
        int32_t sourceIndex = -1;
        for ( uint32_t i = 0u; i < m_editorBrushGpuRegions.size(); ++i )
            if ( m_editorBrushGpuRegions[i].m_location == location ) { sourceIndex = ( int32_t ) i; break; }
        if ( sourceIndex < 0 )
        {
            t3dEditorBrushSourceRegion_t entry;
            entry.m_location = location;
            entry.m_nRevision = region->getAuthoringSourceRevision();
            Ref< Image > map = region->getMap( rState.m_mapType );
            if ( map.is_null() && !height ) {
                map = Terrain3DUtil::getFilledImage( V2I( T3D_REGION_SIZE ),
                     control ? COLOR_CONTROL : COLOR_ROUGHNESS, false,
                     control ? Image::FORMAT_RF : Image::FORMAT_RGBA8 );
            }
            if ( map.is_null() ) return false;
            entry.m_source = map->get_data();
            if ( entry.m_source.size() != ( int64_t ) ( T3D_REGION_SIZE * T3D_REGION_SIZE * pixelSize ) ) return false;
            m_editorBrushGpuRegions.push_back( entry );
            sourceIndex = ( int32_t ) m_editorBrushGpuRegions.size() - 1;
            ++m_nBrushGetDataCalls;
            m_nBrushBytesRead += entry.m_source.size();
        }
        const t3dEditorBrushSourceRegion_t &source = m_editorBrushGpuRegions[( size_t ) sourceIndex];
        const int32_t localMinX = MAX( minX - regionX * T3D_REGION_SIZE, 0 );
        const int32_t localMaxX = MIN( maxX - regionX * T3D_REGION_SIZE, T3D_REGION_SIZE - 1 );
        const int32_t localMinY = MAX( minY - regionY * T3D_REGION_SIZE, 0 );
        const int32_t localMaxY = MIN( maxY - regionY * T3D_REGION_SIZE, T3D_REGION_SIZE - 1 );
        if ( localMaxX < localMinX || localMaxY < localMinY ) continue;
        for ( int32_t ty = localMinY / 16; ty <= localMaxY / 16; ++ty )
        for ( int32_t tx = localMinX / 16; tx <= localMaxX / 16; ++tx )
        {
            const uint16_t tileX = ( uint16_t ) ( tx * 16 );
            const uint16_t tileY = ( uint16_t ) ( ty * 16 );
            bool exists = false;
            for ( const t3dEditorBrushTile_t &tile : m_editorBrushGpuTiles )
                if ( tile.m_nRegionX == regionX && tile.m_nRegionY == regionY &&
                     tile.m_nMinX == tileX && tile.m_nMinY == tileY ) { exists = true; break; }
            if ( exists ) continue;
            if ( m_editorBrushGpuTiles.size() + fresh.size() >= T3D_EDITOR_BRUSH_MAX_TILES ||
                 nOffset + 16u * 16u * pixelSize > T3D_EDITOR_BRUSH_MAX_TRANSFER_BYTES )
            {
                if ( m_nPendingEditorBrushStrokeId ) { cancelOperation(); return true; }
                m_bEditorBrushGpuRejected = true;
                return false;
            }
            t3dEditorBrushTile_t tile;
            tile.m_nRegionX = regionX;
            tile.m_nRegionY = regionY;
            tile.m_nSourceRevision = source.m_nRevision;
            tile.m_nEncoding = ( uint8_t ) rState.m_mapType;
            tile.m_nByteOffset = nOffset;
            tile.m_nMinX = tileX;
            tile.m_nMinY = tileY;
            tile.m_nWidth = 16u;
            tile.m_nHeight = 16u;
            tile.m_nRowStrideBytes = ( uint16_t ) ( 16u * pixelSize );
            PackedByteArray bytes;
            const uint32_t length = 16u * tile.m_nRowStrideBytes;
            bytes.resize( length );
            for ( uint32_t y = 0u; y < 16u; ++y )
                memcpy( bytes.ptrw() + y * tile.m_nRowStrideBytes,
                        source.m_source.ptr() + ( ( tileY + y ) * T3D_REGION_SIZE + tileX ) * pixelSize,
                        tile.m_nRowStrideBytes );
            fresh.push_back( tile );
            freshBytes.push_back( bytes );
            nOffset += length;
        }
    }
    if ( fresh.empty() && !m_nPendingEditorBrushStrokeId ) return false;

    const uint64_t nStrokeId = m_nPendingEditorBrushStrokeId ? m_nPendingEditorBrushStrokeId :
        ( m_nEditorBrushNextStrokeId = MAX( m_nEditorBrushNextStrokeId + 1u,
             Time::get_singleton()->get_ticks_usec() ) );
    if ( !m_nPendingEditorBrushStrokeId )
    {
        // Stroke-invariant mask: same gamma-adjusted source texels as the CPU sampler.
        updateBrushAlphaCache( rState.m_pBrushImage, rState.m_imageSize, rState.m_fGamma );
        PackedByteArray mask;
        mask.resize( ( int64_t ) m_fBrushAlphaCache.size() * sizeof( float ) );
        memcpy( mask.ptrw(), m_fBrushAlphaCache.data(), ( size_t ) mask.size() );
        Terrain3DEditorBrushTransaction transaction;
        if ( !transaction.begin( nStrokeId, m_nEditorBrushOwnerGeneration,
                                Terrain3DRdContext::getShared().getDeviceGeneration() ) ) return false;
        PackedByteArray source;
        source.resize( nOffset );
        for ( uint32_t i = 0u; i < fresh.size(); ++i )
        {
            if ( !transaction.appendTile( fresh[i] ) ) return false;
            memcpy( source.ptrw() + fresh[i].m_nByteOffset, freshBytes[i].ptr(),
                    ( size_t ) freshBytes[i].size() );
        }
        m_nEditorBrushGpuMapType = ( int32_t ) rState.m_mapType;
        m_editorBrushGpuMaskSize = rState.m_imageSize;
        m_pEditorBrushGpuMaskImage = rState.m_pBrushImage;
        m_fEditorBrushGpuMaskGamma = rState.m_fGamma;
        if ( !pRenderer->beginEditorBrushStrokeCold( transaction, source, mask,
             ( uint32_t ) rState.m_imageSize.x, ( uint32_t ) rState.m_imageSize.y ) ||
             !acceptEditorBrushStrokeCold( nStrokeId ) )
        {
            pRenderer->cancelEditorBrushStrokeCold( nStrokeId );
            m_bEditorBrushGpuRejected = true;
            return false;
        }
    }
    else
    {
        for ( uint32_t i = 0u; i < fresh.size(); ++i )
            if ( !pRenderer->appendEditorBrushTileCold( nStrokeId, fresh[i], freshBytes[i] ) )
            {
                cancelOperation();
                return true;
            }
    }
    for ( const t3dEditorBrushTile_t &tile : fresh ) m_editorBrushGpuTiles.push_back( tile );
    t3dEditorBrushCommand_t cmd;
    cmd.m_nStrokeId = nStrokeId;
    cmd.m_nOwnerGeneration = m_nEditorBrushOwnerGeneration;
    cmd.m_nDeviceGeneration = Terrain3DRdContext::getShared().getDeviceGeneration();
    cmd.m_nSequence = ++m_nEditorBrushGpuSequence;
    cmd.m_nRegionX = m_editorBrushGpuTiles.front().m_nRegionX;
    cmd.m_nRegionY = m_editorBrushGpuTiles.front().m_nRegionY;
    cmd.m_nMapType = ( uint32_t ) rState.m_mapType;
    cmd.m_fSelectedHeight = rState.m_fHeight;
    cmd.m_nPackedColor = packColorRgba8( rState.m_color );
    cmd.m_fRoughness = rState.m_fRoughness;
    cmd.m_fCenterHeight = rState.m_globalPosition.y;
    if ( gradientValid ) {
        const Vector3 first = rState.m_gradientPoints[0];
        const Vector3 second = rState.m_gradientPoints[1];
        cmd.m_fGradientX1 = first.x;
        cmd.m_fGradientY1 = first.y;
        cmd.m_fGradientZ1 = first.z;
        cmd.m_fGradientX2 = second.x;
        cmd.m_fGradientY2 = second.y;
        cmd.m_fGradientZ2 = second.z;
        cmd.m_fMotionX = m_operationMovement.x;
        cmd.m_fMotionZ = m_operationMovement.z;
    }
    cmd.m_nTileCountAtEnqueue = ( uint32_t ) m_editorBrushGpuTiles.size();
    cmd.m_nTool = ( uint32_t ) m_tool;
    cmd.m_nOperation = ( uint32_t ) m_nOperation;
    cmd.m_nFlags = ( rState.m_bEnableTexture ? 4u : 0u ) |
        ( rState.m_bEnableAngle ? 8u : 0u ) | ( rState.m_bEnableScale ? 16u : 0u );
    cmd.m_nAssetId = rState.m_nAssetIdBits;
    cmd.m_nUvRotation = ( uint32_t ) CLAMP( Math::round( rState.m_fAngle / 22.5f ), 0.f, 15.f );
    const uint32_t scaleAlign[8] = {5u, 6u, 7u, 0u, 1u, 2u, 3u, 4u};
    cmd.m_nUvScale = scaleAlign[( uint8_t ) CLAMP( Math::round( ( rState.m_fScale + 60.f ) / 20.f ), 0.f, 7.f )];
    cmd.m_fCenterX = rState.m_globalPosition.x;
    cmd.m_fCenterZ = rState.m_globalPosition.z;
    cmd.m_fRotation = rState.m_fRotation;
    cmd.m_fRadius = rState.m_fBrushSize * .5f;
    cmd.m_fStrength = rState.m_fStrength;
    cmd.m_fVertexSpacing = rState.m_fVertexSpacing;
    cmd.m_nMaskWidth = rState.m_imageSize.x;
    cmd.m_nMaskHeight = rState.m_imageSize.y;
    if ( !pRenderer->enqueueEditorBrushCommandCold( nStrokeId, cmd ) )
    {
        cancelOperation();
        return true;
    }
    ++m_nBrushDabs;
    return true;
}
#endif

void Terrain3DEditor::operateMap(
	const Vector3 &rGlobalPosition,
	const real_t   fCameraDirection ) {
	LOG( EXTREME, "Operating at ", rGlobalPosition, " tool type ", m_tool, " op ", m_nOperation );

	const t3dMapType_t map_type = getMapType();
	if ( map_type == REGION_TYPE_MAX ) {
		LOG( ERROR, "Invalid tool selected" );
		return;
	}

	const int	   region_size = T3D_REGION_SIZE;
	const Vector2i region_vsize = V2I( region_size );

	// If no region and can't add one, skip whole function. Checked again later
	Terrain3DData *const pData = m_pTerrain->dataObject();
	if ( !pData->hasRegionp( rGlobalPosition ) &&
		 ( !m_brushData["auto_regions"] || ( m_tool != SCULPT && m_tool != HEIGHT ) ) ) {
		return;
	}

	const bool bModifierAlt = m_brushData["modifier_alt"];
	// bool modifier_shift = _brush_data["modifier_shift"];

	Image *const pBrushImage = cast_to< Image >( m_brushData["brush_image"] );
	if ( !pBrushImage ) {
		LOG( ERROR, "Invalid brush image. Returning" );
		return;
	}
	const Vector2i img_size = m_brushData["brush_image_size"];
	const real_t   fBrushSize = CLAMP( ( real_t ) ( m_brushData.get( "size", 10.f ) ), 0.1f, 4096.f ); // Meters
	// Typicall we multiply mouse pressure & strength setting, but
	// * Mouse movement w/ button down has a pressure of 1
	// * Mouse clicks always have pressure of 0
	// * Pen movement pressure varies, sometimes lifting or clicking has a pressure of 0
	// If we're operating with a pressure of 0.001-.999 it's a pen
	// So if there's a 0 pressure operation >100ms after a pen operation, we assume it's
	// a mouse click. This occasionally catches a pen click, but avoids most pen lifts.
	real_t fMousePressure = CLAMP( ( real_t ) ( m_brushData.get( "mouse_pressure", 0.f ) ), 0.f, 1.f );
	if ( fMousePressure > CMP_EPSILON && fMousePressure < 1.f ) {
		m_nLastPenTick = Time::get_singleton()->get_ticks_msec();
	}
	const uint64_t ticks = Time::get_singleton()->get_ticks_msec();
	if ( fMousePressure < CMP_EPSILON && ticks - m_nLastPenTick >= 100 ) {
		fMousePressure = 1.f;
	}
	const real_t strength = fMousePressure * ( real_t ) m_brushData["strength"];

	const real_t fHeight = m_brushData["height"];
	const Color	 color = m_brushData["color"];
	const real_t roughness = m_brushData["roughness"];

	const bool					 bEnableTexture = m_brushData["enable_texture"];
	const bool					 bTextureFilter = m_brushData["texture_filter"];
	const int					 margin = m_brushData["margin"];
	const int					 asset_id = m_brushData["asset_id"];
	const Ref< Terrain3DAssets > pAssets = m_pTerrain->assetsResource();
	if ( m_tool == TEXTURE && ( pAssets.is_null() || pAssets->getTextureAsset( asset_id ).is_null() ) ) {
		return;
	}
	if ( ( m_tool == COLOR || m_tool == ROUGHNESS ) && bTextureFilter &&
		 ( pAssets.is_null() || pAssets->getTextureAsset( asset_id ).is_null() ) ) {
		return;
	}
	if ( m_tool == INSTANCER && ( pAssets.is_null() || pAssets->getMeshAsset( asset_id ).is_null() ) ) {
		return;
	}
	const uint32_t asset_id_bits = ( uint32_t ) ( asset_id );

	const Vector2 slope_range = m_brushData["slope"];
	const bool	  bEnableAngle = m_brushData["enable_angle"];
	const bool	  bDynamicAngle = m_brushData["dynamic_angle"];
	real_t		  angle = m_brushData["angle"];

	const bool	 bEnableScale = m_brushData["enable_scale"];
	const real_t fScale = m_brushData["scale"];

	const real_t	   fGamma = m_brushData["gamma"];
	PackedVector3Array gradient_points = m_brushData["gradient_points"];

	const real_t randf = UtilityFunctions::randf();
	real_t		 fRot = randf * Math::PI * ( real_t ) ( m_brushData["brush_spin_speed"] );
	if ( m_brushData["align_to_view"] ) {
		fRot += fCameraDirection;
	}
	// Rotate the decal to align with the brush
	if ( m_pTerrain->getPlugin() ) {
		Node *const pNode = cast_to< Node >( m_pTerrain->getPlugin()->get( "ui" ) );
		if ( pNode->has_method( "set_decal_rotation" ) ) {
			pNode->call( "set_decal_rotation", fRot );
		}
	}
	AABB edited_area;
	edited_area.position = rGlobalPosition - Vector3( fBrushSize, 0.f, fBrushSize ) * .5f;
	edited_area.size = Vector3( fBrushSize, 0.f, fBrushSize );

	if ( m_tool == INSTANCER ) {
		if ( m_nOperation == SUBTRACT ) {
			m_pTerrain->instancerObject()->removeInstances( rGlobalPosition, m_brushData );
		} else if ( m_nOperation == ADD ) {
			m_pTerrain->instancerObject()->addInstances( rGlobalPosition, m_brushData );
		}
		return;
	}

	t3dMapOperationState_t state;
	state.m_globalPosition = rGlobalPosition;
	state.m_mapType = map_type;
	state.m_nRegionSize = region_size;
	state.m_regionVSize = region_vsize;
	state.m_pData = pData;
	state.m_pBrushImage = pBrushImage;
	state.m_imageSize = img_size;
	state.m_fBrushSize = fBrushSize;
	state.m_fStrength = strength;
	state.m_fHeight = fHeight;
	state.m_color = color;
	state.m_fRoughness = roughness;
	state.m_bModifierAlt = bModifierAlt;
	state.m_bEnableTexture = bEnableTexture;
	state.m_bTextureFilter = bTextureFilter;
	state.m_nMargin = margin;
	state.m_nAssetId = asset_id;
	state.m_nAssetIdBits = asset_id_bits;
	state.m_slopeRange = slope_range;
	state.m_bEnableAngle = bEnableAngle;
	state.m_bDynamicAngle = bDynamicAngle;
	state.m_fAngle = angle;
	state.m_bEnableScale = bEnableScale;
	state.m_fScale = fScale;
	state.m_fGamma = fGamma;
	state.m_gradientPoints = gradient_points;
	state.m_fRotation = fRot;
	state.m_editedArea = edited_area;
	state.m_fVertexSpacing = m_pTerrain->getVertexSpacing();
#if defined( T3D_EDITOR_NATIVE )
    if ( operateGpuTilesCold( state ) ) return;
    if ( m_nPendingEditorBrushStrokeId ) { cancelOperation(); return; }
#endif
	++m_nBrushDabs;
	uint64_t started = Time::get_singleton()->get_ticks_usec();
	const bool prepared = prepareMapOperationSamples( state );
	m_nBrushPrepareUsec += Time::get_singleton()->get_ticks_usec() - started;
	if ( !prepared ) {
		return;
	}
	started = Time::get_singleton()->get_ticks_usec();
	const bool processed = processMapOperationSamples( state );
	m_nBrushCpuSampleUsec += Time::get_singleton()->get_ticks_usec() - started;
	if ( !processed ) {
		return;
	}
	started = Time::get_singleton()->get_ticks_usec();
	applyComputedMapOperationSamples( state );
	m_nBrushCpuComputeUsec += Time::get_singleton()->get_ticks_usec() - started;
	started = Time::get_singleton()->get_ticks_usec();
	commitMapOperationSamples( state );
	m_nBrushCommitUsec += Time::get_singleton()->get_ticks_usec() - started;
}

void Terrain3DEditor::storeUndo(
	void ) {
	IS_INIT_COND_MESG( !m_pTerrain->getPlugin(), "_terrain isn't initialized, returning", VOID );
	const bool bHasValidTool = m_tool >= 0 && m_tool < TOOL_MAX;
	if ( !bHasValidTool && m_originalRegions.is_empty() && m_editedRegions.is_empty() ) {
		return;
	}
	LOG( DEBUG, "Finalize undo & redo snapshots" );
	Dictionary redo_data;
	// Region locations are captured lazily only when the operation actually
	// adds or removes a region. Ordinary painting avoids copying the full active
	// location list on mouse-down.
	if ( m_undoData.has( "region_locations" ) ) {
		redo_data["region_locations"] = m_pTerrain->dataObject()->getRegionLocations().duplicate();
	}
	// Store original and current backups of edited regions
	if ( !m_originalRegions.is_empty() ) {
		m_undoData["edited_regions"] = m_originalRegions;
	}
	if ( !m_editedRegions.is_empty() ) {
		redo_data["edited_regions"] = m_editedRegions;
	}
	if ( !m_originalEditSnapshots.is_empty() ) {
		// Duplicate the containers so resetting the working queues after commit
		// cannot clear the arrays retained by UndoRedo. Images remain shared with
		// the immutable snapshots and are duplicated again when applied.
		m_undoData["edit_snapshots"] = m_originalEditSnapshots.duplicate();
		redo_data["edit_snapshots"] = m_editedEditSnapshots.duplicate();
	}

	if ( Terrain3DLogPolicy::getLevel() >= DEBUG ) {
		LOG( DEBUG, "Storing Original Regions:" );
		for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( m_originalRegions ).size() ); iRegion++ ) {
			const Ref< Terrain3DRegion > pRegion = ( m_originalRegions )[iRegion];
			if ( pRegion.is_valid() ) {
				pRegion->dump();
			}
		}
		LOG( DEBUG, "Storing Edited Regions:" );
		for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( m_editedRegions ).size() ); iRegion++ ) {
			const Ref< Terrain3DRegion > pRegion = ( m_editedRegions )[iRegion];
			if ( pRegion.is_valid() ) {
				pRegion->dump();
			}
		}
		LOG( DEBUG,
			 "Targeted edit snapshots: undo=",
			 m_originalEditSnapshots.size(),
			 ", redo=",
			 m_editedEditSnapshots.size() );
	}

	// Store regions that were removed or added
	if ( m_addedRemovedLocations.size() > 0 ) {
		if ( m_tool == REGION && m_nOperation == SUBTRACT ) {
			m_undoData["removed_regions"] = m_addedRemovedLocations;
			redo_data["added_regions"] = m_addedRemovedLocations;
			LOG( DEBUG, "Removed regions: ", m_addedRemovedLocations );
		} else {
			m_undoData["added_regions"] = m_addedRemovedLocations;
			redo_data["removed_regions"] = m_addedRemovedLocations;
			LOG( DEBUG, "Added regions: ", m_addedRemovedLocations );
		}
	}

	if ( m_pTerrain->dataObject()->getEditedArea().has_volume() ) {
		m_undoData["edited_area"] = m_pTerrain->dataObject()->getEditedArea();
		redo_data["edited_area"] = m_pTerrain->dataObject()->getEditedArea();
		LOG( DEBUG, "Adding edited area to snapshots: ", m_undoData["edited_area"] );
	}

	// Request the plugin store the undo/redo data.
	if ( m_pTerrain->getPlugin()->has_method( "create_undo_action" ) ) {
		LOG( INFO, "Storing undo snapshot" );
		String action_name = "Terrain3D Edit";
		if ( bHasValidTool && m_nOperation >= 0 && m_nOperation < OP_MAX ) {
			action_name = String( "Terrain3D " ) + s_pszEditorOperationNames[m_nOperation] + String( " " ) +
						  s_pszEditorToolNames[m_tool];
		}
		LOG( DEBUG, "Creating undo action: '", action_name, "'" );
		m_pTerrain->getPlugin()->call( "create_undo_action", action_name );

		LOG( DEBUG, "Storing undo snapshot: " );
		Terrain3DUtil::printDict( "_undo_data snapshot", m_undoData, EXTREME );
		m_pTerrain->getPlugin()->call( "add_undo_method",
									   Callable( this, "apply_undo" ).bind( m_undoData.duplicate() ) );

		LOG( DEBUG, "Storing redo snapshot: " );
		Terrain3DUtil::printDict( "redo_data snapshot", redo_data, EXTREME );
		m_pTerrain->getPlugin()->call( "add_do_method", Callable( this, "apply_undo" ).bind( redo_data ) );

		LOG( DEBUG, "Committing undo action" );
		m_pTerrain->getPlugin()->call( "commit_action", false );
	}
}

void Terrain3DEditor::applyUndo(
	const Dictionary &rData ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	IS_INIT_COND_MESG( !m_pTerrain->getPlugin(), "_terrain isn't initialized, returning", VOID );
	LOG( INFO, "Applying Undo/Redo data" );

	Terrain3DData *const	  pData = m_pTerrain->dataObject();
	eastl::vector< Vector2i > restored_region_locations;
	t3dEditorMapUpdates_t	  restored_map_locations;
	bool					  bInstancesChanged = false;
	bool					  bHoleChanged = false;

	if ( rData.has( "edit_snapshots" ) ) {
		const Array snapshots = rData["edit_snapshots"];
		for ( int i = 0; i < snapshots.size(); ++i ) {
			const Dictionary snapshot = snapshots[i];
			applyEditSnapshot( snapshot, restored_map_locations, bInstancesChanged, bHoleChanged );
		}
	}

	if ( rData.has( "edited_regions" ) ) {
		Terrain3DUtil::printArr( "Edited regions", rData["edited_regions"], EXTREME );
		const TypedArray< Terrain3DRegion > undo_regions = rData["edited_regions"];
		LOG( DEBUG, "Backup has ", undo_regions.size(), " edited regions" );
		for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( undo_regions ).size() ); iRegion++ ) {
			Ref< Terrain3DRegion > pRegion = ( undo_regions )[iRegion];
			if ( pRegion.is_null() ) {
				LOG( ERROR, "Null region saved in undo data. Please report this error." );
				continue;
			}
			pRegion->sanitizeMaps(); // Live data may not have some maps so must be sanitized
			// Replace through the repository transaction so the Dictionary, native
			// pointer map, active slot and deferred renderer reconciliation agree.
			const Error replace_error = pData->addRegion( pRegion, false );
			if ( replace_error != OK ) {
				LOG( ERROR, "Unable to restore undo region at ", pRegion->getLocation(), ": ", replace_error );
				continue;
			}
			pRegion->setModified( true ); // Marks the region for per-layer map updates.
			pRegion->setEdited( true );	  // Includes the region in the next map update.
			pRegion->setDeleted( false ); // Ensure region not marked for deletion
			restored_region_locations.push_back( pRegion->getLocation() );
			if ( Terrain3DLogPolicy::getLevel() >= DEBUG ) {
				LOG( DEBUG, "Restoring region:" );
				pRegion->dump();
			}
		}
	}

	if ( rData.has( "edited_area" ) ) {
		LOG( DEBUG, "Edited area: ", rData["edited_area"] );
		pData->addEditedArea( rData["edited_area"] );
	}

	if ( rData.has( "added_regions" ) ) {
		LOG( DEBUG, "Added regions: ", rData["added_regions"] );
		const TypedArray< Vector2i > region_locs = rData["added_regions"];
		for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( region_locs ).size() ); iRegion++ ) {
			const Vector2i		   region_loc = ( region_locs )[iRegion];
			Ref< Terrain3DRegion > pRegion = pData->getRegion( region_loc );
			if ( pRegion.is_valid() ) {
				LOG( DEBUG, "Marking region: ", region_loc, " +deleted, +modified, ", t3dPtrToStr( *pRegion ) );
				pRegion->setDeleted( true );
				pRegion->setModified( true );
			}
		}
	}
	if ( rData.has( "removed_regions" ) ) {
		LOG( DEBUG, "Removed regions: ", rData["removed_regions"] );
		const TypedArray< Vector2i > region_locs = rData["removed_regions"];
		for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( region_locs ).size() ); iRegion++ ) {
			const Vector2i		   region_loc = ( region_locs )[iRegion];
			Ref< Terrain3DRegion > pRegion = pData->getRegion( region_loc );
			if ( pRegion.is_valid() ) {
				LOG( DEBUG, "Marking region: ", region_loc, " -deleted, +modified, ", t3dPtrToStr( *pRegion ) );
				pRegion->setDeleted( false );
				pRegion->setModified( true );
				sendRegionAabb( region_loc, pRegion->getHeightRange() );
			}
		}
	}

	bool bRestoredRegionLayout = false;
	// After all regions are in place, reconcile the active locations. The data
	// layer preserves stable texture slots and uploads only newly reactivated
	// layers, so do not follow this with another full array rebuild.
	if ( rData.has( "region_locations" ) ) {
		// Load w/ duplicate or it gets a bit wonky undoing removed regions w/ saves
		const TypedArray< Vector2i > locations = rData["region_locations"];
		m_pTerrain->dataObject()->setRegionLocations( locations.duplicate() );
		bRestoredRegionLayout = true;
		LOG( DEBUG, "Locations(", locations.size(), "): ", locations );
	}
	// Legacy/external undo payloads may omit region_locations. Only those require
	// the old full rebuild fallback.
	if ( ( rData.has( "added_regions" ) || rData.has( "removed_regions" ) ) && !bRestoredRegionLayout ) {
		pData->updateMaps( REGION_TYPE_MAX, true, false );
	}
	const AABB restored_area = rData.has( "edited_area" ) ? AABB( rData["edited_area"] ) : AABB();
	for ( int map_type = TYPE_HEIGHT; map_type < REGION_TYPE_MAX; ++map_type ) {
		eastl::vector< Vector2i > commit_locations = restored_map_locations.m_locations[( size_t ) ( map_type )];
		commit_locations.insert( commit_locations.end(),
								 restored_region_locations.begin(),
								 restored_region_locations.end() );
		eastl::sort( commit_locations.begin(), commit_locations.end(), locationLess );
		commit_locations.erase( eastl::unique( commit_locations.begin(), commit_locations.end() ),
								commit_locations.end() );
		if ( commit_locations.empty() ) {
			continue;
		}
		uint32_t commit_flags = MAP_EDIT_INTERACTIVE;
		if ( map_type == TYPE_HEIGHT || map_type == TYPE_CONTROL ) {
			commit_flags |= MAP_EDIT_UPDATE_INSTANCES | MAP_EDIT_SNAP;
		}
		if ( map_type == TYPE_HEIGHT || ( map_type == TYPE_CONTROL &&
			( bHoleChanged || rData.has( "edited_regions" ) ) ) ) {
			commit_flags |= MAP_EDIT_UPDATE_COLLISION;
		}
		pData->commitMapEdits( ( t3dMapType_t ) ( map_type ), commit_locations, restored_area, commit_flags );
	}
	// After TextureArray updates clear edited regions flag.
	if ( rData.has( "edited_regions" ) ) {
		const TypedArray< Terrain3DRegion > undo_regions = rData["edited_regions"];
		for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( undo_regions ).size() ); iRegion++ ) {
			Ref< Terrain3DRegion > pRegion = ( undo_regions )[iRegion];
			if ( pRegion.is_valid() ) {
				pRegion->setEdited( false );
			}
		}
	}
	if ( bInstancesChanged || rData.has( "edited_regions" ) || rData.has( "added_regions" ) ||
		 rData.has( "removed_regions" ) ) {
		m_pTerrain->instancerObject()->updateMmis( -1, s_v2iMax, true );
	}
}

// Returns average of height, blend (as real_t(0-255)), or roughness. Overloaded version handles average color
float Terrain3DEditor::average(
	const t3dAverageMode_t mode,
	const Vector3		  &rGlobalPosition,
	const float			   fBase,
	const float			   fNanVal,
	const bool			   bAlt ) const {
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return NAN;
	}
	Terrain3DData *const pData = m_pTerrain->dataObject();
	const real_t		 fVertexSpacing = m_pTerrain->getVertexSpacing();
	const Vector3		 left_position = rGlobalPosition - Vector3( fVertexSpacing, 0.f, 0.f );
	const Vector3		 right_position = rGlobalPosition + Vector3( fVertexSpacing, 0.f, 0.f );
	const Vector3		 down_position = rGlobalPosition - Vector3( 0.f, 0.f, fVertexSpacing );
	const Vector3		 up_position = rGlobalPosition + Vector3( 0.f, 0.f, fVertexSpacing );

	t3dMapType_t map_type;
	int			 index;
	switch ( mode ) {
	case AVG_HEIGHT:
		map_type = TYPE_HEIGHT;
		index = 0; // Red
		break;
	case AVG_BLEND:
		map_type = TYPE_CONTROL;
		index = 0; // Red
		break;
	case AVG_ROUGHNESS:
		map_type = TYPE_COLOR;
		index = 3; // Alpha
		break;
	default:
		break;
	}

	Color pixel;
	float fLeft, right, up, down;
	pixel = pData->getPixel( map_type, left_position );
	fLeft = isnan( pixel.r ) ? fNanVal : pixel[index];
	pixel = pData->getPixel( map_type, right_position );
	right = isnan( pixel.r ) ? fNanVal : pixel[index];
	pixel = pData->getPixel( map_type, up_position );
	up = isnan( pixel.r ) ? fNanVal : pixel[index];
	pixel = pData->getPixel( map_type, down_position );
	down = isnan( pixel.r ) ? fNanVal : pixel[index];

	if ( mode == AVG_BLEND ) {
		if ( bAlt ) {
			return ( t3dGetBlend( fBase ) + t3dGetBlend( fLeft ) + t3dGetBlend( right ) + t3dGetBlend( up ) +
					 t3dGetBlend( down ) ) *
				   0.2f;
		} else {
			return Math::lerp( ( float ) ( t3dGetBlend( fBase ) ), 128.f, .1f );
		}
	} else {
		return ( fBase + fLeft + right + up + down ) * 0.2f;
	}
}

Color Terrain3DEditor::average(
	const Vector3 &rGlobalPosition,
	const Color	  &clrBase ) const {
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return s_colorNan;
	}
	Terrain3DData *const pData = m_pTerrain->dataObject();
	const real_t		 fVertexSpacing = m_pTerrain->getVertexSpacing();
	const Vector3		 left_position = rGlobalPosition - Vector3( fVertexSpacing, 0.f, 0.f );
	const Vector3		 right_position = rGlobalPosition + Vector3( fVertexSpacing, 0.f, 0.f );
	const Vector3		 down_position = rGlobalPosition - Vector3( 0.f, 0.f, fVertexSpacing );
	const Vector3		 up_position = rGlobalPosition + Vector3( 0.f, 0.f, fVertexSpacing );

	Color left = pData->getPixel( TYPE_COLOR, left_position ).srgb_to_linear();
	if ( isnan( left.r ) ) {
		left = s_colorWhite;
	}
	Color right = pData->getPixel( TYPE_COLOR, right_position ).srgb_to_linear();
	if ( isnan( right.r ) ) {
		right = s_colorWhite;
	}
	Color up = pData->getPixel( TYPE_COLOR, up_position ).srgb_to_linear();
	if ( isnan( up.r ) ) {
		up = s_colorWhite;
	}
	Color down = pData->getPixel( TYPE_COLOR, down_position ).srgb_to_linear();
	if ( isnan( down.r ) ) {
		down = s_colorWhite;
	}
	Color base = clrBase.srgb_to_linear();
	return Color( ( base.r + left.r + right.r + up.r + down.r ) * 0.2f,
				  ( base.g + left.g + right.g + up.g + down.g ) * 0.2f,
				  ( base.b + left.b + right.b + up.b + down.b ) * 0.2f,
				  1.f )
		.linear_to_srgb();
}

// -----------------------------------------------------------------------------
// Public functions

// Sanitizes incoming brush data and applies editor defaults and clamps.
// Only editor-owned fields are sanitized here; instancer-specific fields remain untouched.
void Terrain3DEditor::setBrushData(
	const Dictionary &rData ) {
	// Asset-selection notifications can arrive before the editor brush UI is ready.
	// Preserve the last valid mask instead of replacing it with partial settings.
	if ( !rData.has( "brush" ) || rData.get( "brush", Variant() ).get_type() != Variant::ARRAY ) {
		return;
	}
	const Array brush_values = rData.get( "brush", Array() );
	if ( brush_values.size() != 2 ) {
		return;
	}
	const Ref< Image >	   image = brush_values[0];
	const Ref< Texture2D > texture = brush_values[1];
	if ( !image.is_valid() || image->is_empty() || !texture.is_valid() || texture->get_width() <= 0 ||
		 texture->get_height() <= 0 ) {
		return;
	}
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	m_brushData = rData; // Same instance. Anything could be inserted after this, eg mouse_pressure
	m_pCachedBrushImage = NULL;
	m_cachedBrushImageSize = s_v2iZero;
	m_fCachedBrushGamma = -1.f;
	m_fBrushAlphaCache.clear();

	// Sanitize image and textures
	Array brush_images = brush_values;
	if ( brush_images.size() == 2 ) {
		const Ref< Image > pImg = brush_images[0];
		if ( pImg.is_valid() && !pImg->is_empty() ) {
			m_brushData["brush_image"] = pImg;
			m_brushData["brush_image_size"] = pImg->get_size();
		} else {
			LOG( ERROR, "Brush data doesn't contain a valid image" );
		}
		const Ref< Texture2D > pTex = brush_images[1];
		if ( pTex.is_valid() && pTex->get_width() > 0 && pTex->get_height() > 0 ) {
			m_brushData["brush_texture"] = pTex;
		} else {
			LOG( ERROR, "Brush data doesn't contain a valid texture" );
		}
	} else {
		LOG( ERROR, "Brush data doesn't contain an image and texture" );
	}

	// Sanitize scalar brush settings.
	// Size is also constrained by operateMap() and Terrain3DInstancer::addTransforms().
	m_brushData["size"] = CLAMP( ( real_t ) ( rData.get( "size", 10.f ) ), 0.1f, 4096.f ); // Diameter in meters
	m_brushData["strength"] =
		CLAMP( ( real_t ) ( rData.get( "strength", .1f ) ) * .01f, .01f, 1000.f ); // 1-100k% (max of 1000m per click)
	// mouse_pressure is injected by editor.gd and sanitized by operateMap().
	Vector2 slope = rData.get( "slope", Vector2( 0.f, 90.f ) );
	slope.x = CLAMP( slope.x, 0.f, 90.f );
	slope.y = CLAMP( slope.y, 0.f, 90.f );
	m_brushData["slope"] = slope;																   // 0-90 (degrees)
	m_brushData["height"] = CLAMP( ( real_t ) ( rData.get( "height", 0.f ) ), -65536.f, 65536.f ); // Meters
	Color col = rData.get( "color", COLOR_ROUGHNESS );
	col.r = CLAMP( col.r, 0.f, 5.f );
	col.g = CLAMP( col.g, 0.f, 5.f );
	col.b = CLAMP( col.b, 0.f, 5.f );
	col.a = CLAMP( col.a, 0.f, 1.f );
	m_brushData["color"] = col;
	m_brushData["roughness"] =
		CLAMP( ( real_t ) ( rData.get( "roughness", 0.f ) ), -100.f, 100.f ) * .01f; // Percentage

	m_brushData["enable_texture"] = rData.get( "enable_texture", true );
	m_brushData["texture_filter"] = rData.get( "texture_filter", false );
	const int requested_asset_id = ( int ) ( rData.get( "asset_id", -1 ) );
	const int maximum_asset_id = MAX( T3D_MAX_MESH_ASSETS, T3D_MAX_TEXTURE_ASSETS ) - 1;
	m_brushData["asset_id"] = requested_asset_id < 0 ? -1 : CLAMP( requested_asset_id, 0, maximum_asset_id );
	m_brushData["margin"] = CLAMP( ( int ) ( rData.get( "margin", 0 ) ), -100, 100 );

	m_brushData["enable_angle"] = rData.get( "enable_angle", true );
	m_brushData["dynamic_angle"] = rData.get( "dynamic_angle", false );
	m_brushData["angle"] = CLAMP( ( real_t ) ( rData.get( "angle", 0.f ) ), 0.f, 337.5f );

	m_brushData["enable_scale"] = rData.get( "enable_scale", true );
	m_brushData["scale"] = CLAMP( ( real_t ) ( rData.get( "scale", 0.f ) ), -60.f, 80.f );

	m_brushData["auto_regions"] = ( bool ) ( rData.get( "auto_regions", true ) );
	m_brushData["align_to_view"] = ( bool ) ( rData.get( "align_to_view", true ) );
	m_brushData["gamma"] = CLAMP( ( real_t ) ( rData.get( "gamma", 1.f ) ), 0.1f, 2.f );
	m_brushData["brush_spin_speed"] = CLAMP( ( real_t ) ( rData.get( "brush_spin_speed", 0.f ) ), 0.f, 1.f );
	m_brushData["gradient_points"] = rData.get( "gradient_points", PackedVector3Array() );

	Terrain3DUtil::printDict( "setBrushData() sanitized brush data:", m_brushData, EXTREME );
}

void Terrain3DEditor::setTool(
	const t3dTool_t tool ) {
	const t3dTool_t new_tool = CLAMP( tool, ( t3dTool_t ) ( 0 ), TOOL_MAX );
	if ( new_tool == m_tool ) {
		return;
	}
	// Tool changes end the existing transaction before map type switches.
	if ( m_bIsOperating ) {
#if defined( T3D_EDITOR_NATIVE )
		if ( m_nPendingEditorBrushStrokeId ) cancelOperation();
		else
#endif
		stopOperation();
	}
	const t3dTool_t old_tool = m_tool;
	m_tool = new_tool;
	++m_nBrushToolSelections;
	if ( m_pTerrain && ( m_tool == t3dTool_t::NAVIGATION || old_tool == t3dTool_t::NAVIGATION ||
						 m_tool == t3dTool_t::REGION || old_tool == t3dTool_t::REGION ) ) {
		m_pTerrain->materialResource()->updateEditorOverlay();
	}
}

void Terrain3DEditor::setOperation(
	const t3dOperation_t operation ) {
	if ( m_bIsOperating && m_nOperation != operation ) {
#if defined( T3D_EDITOR_NATIVE )
		if ( m_nPendingEditorBrushStrokeId ) cancelOperation();
		else
#endif
		stopOperation();
	}
	SET_IF_DIFF( m_nOperation, CLAMP( operation, ( t3dOperation_t ) ( 0 ), OP_MAX ) );
}

// Called on mouse click
void Terrain3DEditor::startOperation(
	const Vector3 &rGlobalPosition ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		LOG( ERROR, "Terrain isn't initialized" );
		return;
	}
	// The pending GPU result must publish (or be canceled) before a new stroke
	// may mutate the same region/channel; never overtake its source revision.
#if defined( T3D_EDITOR_NATIVE )
	if ( m_nPendingEditorBrushStrokeId ) return;
#endif
	if ( m_bIsOperating ) stopOperation();
	LOG( INFO, "Setting up undo snapshot" );
	m_undoData.clear();
	m_bIsOperating = true;
	if ( ++m_nEditorBrushOwnerGeneration == 0u ) m_nEditorBrushOwnerGeneration = 1u;
	m_originalRegions = TypedArray< Terrain3DRegion >(); // New pointers instead of clear
	m_editedRegions = TypedArray< Terrain3DRegion >();
	m_originalEditSnapshots = Array();
	m_editedEditSnapshots = Array();
	m_addedRemovedLocations = TypedArray< Vector2i >();
	m_brushUndoRegions.clear();
#if defined( T3D_EDITOR_NATIVE )
    m_editorBrushGpuRegions.clear();
    m_editorBrushGpuTiles.clear();
    m_nEditorBrushGpuSequence = 0u;
    m_bEditorBrushGpuRejected = false;
    m_nEditorBrushGpuMapType = -1;
    m_pEditorBrushGpuMaskImage = NULL;
#endif
	m_nBrushDabs = 0u;
	m_nBrushGetDataCalls = 0u;
	m_nBrushSetDataCalls = 0u;
	m_nBrushBytesRead = 0u;
	m_nBrushBytesPublished = 0u;
	m_nBrushPrepareUsec = 0u;
	m_nBrushCpuSampleUsec = 0u;
	m_nBrushCpuComputeUsec = 0u;
	m_nBrushCommitUsec = 0u;
	// Reset counter at start to ensure first click places an instance
	m_pTerrain->instancerObject()->resetDensityCounter();
	m_pTerrain->dataObject()->clearEditedArea();
	m_operationPosition = rGlobalPosition;
	m_operationMovement = s_v3Zero;
}

// Called on mouse movement with left mouse button down
void Terrain3DEditor::operate(
	const Vector3 &rGlobalPosition,
	const real_t   fCameraDirection ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		LOG( ERROR, "Terrain isn't initialized" );
		return;
	}
	if ( !m_bIsOperating ) {
		LOG( ERROR, "Run start_operation() before operating" );
		return;
	}
#if defined( T3D_EDITOR_NATIVE )
	if ( m_bPendingEditorBrushSealed ) return;
#endif
	m_operationMovement = rGlobalPosition - m_operationPosition;
	m_operationPosition = rGlobalPosition;

	// Convolve the last 8 movement events, we dont clear on mouse release
	// so as to make repeated mouse strokes in the same direction consistent
	m_operationMovementHistory.push_back( m_operationMovement );
	if ( m_operationMovementHistory.size() > 8 ) {
		m_operationMovementHistory.pop_front();
	}
	// size -1, dont add the last appended entry
	for ( int i = 0; i < m_operationMovementHistory.size() - 1; i++ ) {
		m_operationMovement += m_operationMovementHistory[i];
	}
	m_operationMovement *= 0.125f; // 1/8th

	if ( m_tool == REGION ) {
		operateRegion( m_pTerrain->dataObject()->getRegionLocation( rGlobalPosition ) );
	} else if ( m_tool >= 0 && m_tool < TOOL_MAX ) {
		operateMap( rGlobalPosition, fCameraDirection );
	}
}

void Terrain3DEditor::backupRegion(
	const Ref< Terrain3DRegion > &rRegion,
	const bool					  bMapWasImplicit ) {
	// Backup region once at the start of an operation. Once Edited is set, this is skipped
	if ( m_bIsOperating && rRegion.is_valid() && !rRegion->isEdited() ) {
		LOG( DEBUG, "Storing original copy of region: ", rRegion->getLocation() );
		// A newly auto-created region is already represented by the add/remove
		// snapshot and the redo region. Capturing another map snapshot is redundant.
		if ( m_addedRemovedLocations.find( rRegion->getLocation() ) < 0 ) {
			if ( m_tool == REGION ) {
				const Ref< Terrain3DRegion > pOrigRegion = rRegion->duplicate( true );
				m_originalRegions.push_back( pOrigRegion );
				m_editedRegions.push_back( rRegion );
			} else if ( m_tool == INSTANCER ) {
				const Dictionary snapshot = captureEditSnapshot( rRegion, Dictionary(), bMapWasImplicit );
				if ( !snapshot.is_empty() ) {
					m_originalEditSnapshots.push_back( snapshot );
				}
			} else {
				const Ref< Image > map = rRegion->getMap( getMapType() );
				const Image::Format format = getMapType() == TYPE_HEIGHT ? Image::FORMAT_R16 :
											 getMapType() == TYPE_CONTROL ? Image::FORMAT_RF : Image::FORMAT_RGBA8;
				if ( map.is_null() || map->get_format() != format ) {
					const Dictionary snapshot = captureEditSnapshot( rRegion, Dictionary(), bMapWasImplicit );
					if ( !snapshot.is_empty() ) {
						m_originalEditSnapshots.push_back( snapshot );
					}
				}
				// Native-format map pixels use 16x16 first-write deltas.
			}
		}
		rRegion->setEdited( true );
		rRegion->setModified( true );
		if ( Terrain3DLogPolicy::getLevel() >= DEBUG ) {
			LOG( DEBUG, "Backed up region edit state" );
			rRegion->dump();
		}
	}
}

void Terrain3DEditor::backupRegionFull(
	const Ref< Terrain3DRegion > &rRegion ) {
	// External editor jobs such as erosion can update height, control and color in
	// one transaction. Preserve the complete region independently of the active
	// brush tool so undo restores every channel changed by the job.
	if ( !m_bIsOperating || rRegion.is_null() || rRegion->isEdited() ) {
		return;
	}

	LOG( DEBUG, "Storing full original copy of region: ", rRegion->getLocation() );
	if ( m_addedRemovedLocations.find( rRegion->getLocation() ) < 0 ) {
		const Ref< Terrain3DRegion > pOrigRegion = rRegion->duplicate( true );
		m_originalRegions.push_back( pOrigRegion );
		m_editedRegions.push_back( rRegion );
	}
	rRegion->setEdited( true );
	rRegion->setModified( true );
}

void Terrain3DEditor::releaseRuntimeAllocatorStorage(
	void ) {
	t3dVectorRelease( m_fBrushAlphaCache );
	t3dVectorRelease( m_xAxisScratch );
	t3dVectorRelease( m_zAxisScratch );
	t3dVectorRelease( m_regionCacheScratch );
	t3dVectorRelease( m_computeElementScratch );
	t3dVectorRelease( m_computeWriteScratch );
	t3dVectorRelease( m_modifiedRegionScratch );
	t3dVectorRelease( m_brushUndoRegions );
	m_pCachedBrushImage = NULL;
	m_cachedBrushImageSize = s_v2iZero;
	m_fCachedBrushGamma = -1.f;
}

void Terrain3DEditor::trimBrushScratch(
	void ) {
	m_xAxisScratch.clear();
	m_zAxisScratch.clear();
	m_regionCacheScratch.clear();
	m_computeElementScratch.clear();
	m_computeWriteScratch.clear();
	m_modifiedRegionScratch.clear();

	const size_t compute_bytes = m_computeElementScratch.capacity() * sizeof( t3dBrushElement_t ) +
								 m_computeWriteScratch.capacity() * sizeof( t3dComputeWrite_t );
	if ( compute_bytes > T3D_COMPUTE_SCRATCH_RETAIN_LIMIT ) {
		t3dVectorRelease( m_computeElementScratch );
		t3dVectorRelease( m_computeWriteScratch );
	}

	// Retain no more region-cache entries than the active logical terrain can use.
	int region_map_size = T3D_REGION_MAP_SIZE;
	if ( m_pTerrain && m_pTerrain->dataObject() ) {
		region_map_size = T3D_REGION_MAP_SIZE;
	}
	const size_t region_cache_limit = ( size_t ) ( region_map_size ) * ( size_t ) ( region_map_size );
	if ( m_regionCacheScratch.capacity() > region_cache_limit ) {
		t3dVectorRelease( m_regionCacheScratch );
	}
}

// Called on left mouse button released
void Terrain3DEditor::stopOperation(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		LOG( ERROR, "Terrain isn't initialized" );
		return;
	}
#if defined( T3D_EDITOR_NATIVE )
	if ( m_nPendingEditorBrushStrokeId ) {
		if ( m_bPendingEditorBrushSealed ) return;
		Terrain3DRenderCoordinator *const pRenderer = static_cast< Terrain3D * >( m_pTerrain )->getRenderCoordinatorInternal();
		if ( pRenderer && pRenderer->sealEditorBrushStrokeCold( m_nPendingEditorBrushStrokeId ) ) {
			m_bPendingEditorBrushSealed = true;
			return; // Deferred CPU publication and undo after async readback.
		}
		cancelOperation();
		return;
	}
#endif
	// Assemble immutable before/after touched-tile records exactly once.
	finishBrushUndoTiles();
	// If undo was created and terrain actually modified, store it
	LOG( DEBUG,
		 "Backed up regions: ",
		 m_originalRegions.size(),
		 ", Edited regions: ",
		 m_editedRegions.size(),
		 ", Targeted snapshots: ",
		 m_originalEditSnapshots.size(),
		 ", Added/Removed regions: ",
		 m_addedRemovedLocations.size() );
	if ( m_bIsOperating && ( !m_addedRemovedLocations.is_empty() || !m_editedRegions.is_empty() ||
							 !m_originalEditSnapshots.is_empty() ) ) {
		for ( int i = 0; i < m_editedRegions.size(); i++ ) {
			Ref< Terrain3DRegion > pRegion = m_editedRegions[i];
			pRegion->setEdited( false );
			// Make duplicate for redo, necessary or redos won't work
			const Ref< Terrain3DRegion > pRedoRegion = pRegion->duplicate( true );
			m_editedRegions[i] = pRedoRegion;
			if ( Terrain3DLogPolicy::getLevel() >= DEBUG ) {
				LOG( DEBUG, "Edited region:" );
				pRegion->dump();
				LOG( DEBUG, "Redo region:" );
				pRedoRegion->dump();
			}
		}
		for ( int i = 0; i < m_originalEditSnapshots.size(); ++i ) {
			const Dictionary original_snapshot = m_originalEditSnapshots[i];
			if ( !original_snapshot.has( "location" ) ) {
				continue;
			}
			const Vector2i		   location = original_snapshot["location"];
			Ref< Terrain3DRegion > pRegion = m_pTerrain->dataObject()->getRegion( location );
			if ( pRegion.is_valid() ) {
				// Tile deltas already carry the corresponding immutable redo
				// bytes. Recapturing here would turn the delta back into a
				// full-region map snapshot.
				if ( original_snapshot.has( "map_tiles" ) || original_snapshot.has( "map_absent" ) ) {
					pRegion->setEdited( false );
					continue;
				}
				const Dictionary redo_snapshot = captureEditSnapshot( pRegion, original_snapshot );
				if ( !redo_snapshot.is_empty() ) {
					m_editedEditSnapshots.push_back( redo_snapshot );
				}
				pRegion->setEdited( false );
			}
		}
		storeUndo();
	}
	m_undoData.clear();
	m_originalRegions = TypedArray< Terrain3DRegion >(); // New pointers instead of clear
	m_editedRegions = TypedArray< Terrain3DRegion >();
	m_originalEditSnapshots = Array();
	m_editedEditSnapshots = Array();
	m_addedRemovedLocations = TypedArray< Vector2i >();
	m_pTerrain->dataObject()->clearEditedArea();
	m_bIsOperating = false;
	trimBrushScratch();
}

// Discard an interrupted stroke without publishing an UndoRedo action. The
// existing source-map transaction owns all restoration and invalidation.
void Terrain3DEditor::cancelOperation(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
#if defined( T3D_EDITOR_NATIVE )
	if ( m_nPendingEditorBrushStrokeId ) {
		if ( m_pTerrain ) {
			Terrain3DRenderCoordinator *const pRenderer = static_cast< Terrain3D * >( m_pTerrain )->getRenderCoordinatorInternal();
			if ( pRenderer ) pRenderer->cancelEditorBrushStrokeCold( m_nPendingEditorBrushStrokeId );
		}
		m_nPendingEditorBrushStrokeId = 0u;
		m_bPendingEditorBrushSealed = false;
        m_editorBrushGpuRegions.clear();
        m_editorBrushGpuTiles.clear();
        m_nEditorBrushGpuSequence = 0u;
		if ( ++m_nEditorBrushOwnerGeneration == 0u ) m_nEditorBrushOwnerGeneration = 1u;
	}
#endif
	if ( !m_bIsOperating || !m_pTerrain || !m_pTerrain->dataObject() ) return;
	finishBrushUndoTiles();
	Dictionary rollback = m_undoData.duplicate();
	if ( !m_originalRegions.is_empty() ) {
		rollback["edited_regions"] = m_originalRegions;
	}
	if ( !m_originalEditSnapshots.is_empty() ) {
		rollback["edit_snapshots"] = m_originalEditSnapshots.duplicate();
	}
	if ( !m_addedRemovedLocations.is_empty() ) {
		if ( m_tool == REGION && m_nOperation == SUBTRACT ) {
			rollback["removed_regions"] = m_addedRemovedLocations;
		} else {
			rollback["added_regions"] = m_addedRemovedLocations;
		}
	}
	rollback["edited_area"] = m_pTerrain->dataObject()->getEditedArea();
	applyUndo( rollback );
	m_undoData.clear();
	m_originalRegions = TypedArray< Terrain3DRegion >();
	m_editedRegions = TypedArray< Terrain3DRegion >();
	m_originalEditSnapshots = Array();
	m_editedEditSnapshots = Array();
	m_addedRemovedLocations = TypedArray< Vector2i >();
	m_brushUndoRegions.clear();
	m_pTerrain->dataObject()->clearEditedArea();
	m_bIsOperating = false;
	trimBrushScratch();
}

// M41 extracted authoring/editor header implementations.

t3dMapType_t Terrain3DEditor::getMapType(
	void ) const {
	switch ( m_tool ) {
	case SCULPT:
	case HEIGHT:
	case INSTANCER:
	case LANDFORM:
		return TYPE_HEIGHT;
	case TEXTURE:
	case AUTOSHADER:
	case HOLES:
	case NAVIGATION:
	case ANGLE:
	case SCALE:
		return TYPE_CONTROL;
	case COLOR:
	case ROUGHNESS:
		return TYPE_COLOR;
	default:
		return REGION_TYPE_MAX;
	}
}

bool Terrain3DEditor::isInBounds(
	const Point2i &rPixel,
	const Point2i &rSize ) const {
	const bool bPositive = rPixel.x >= 0 && rPixel.y >= 0;
	const bool bLessThanMax = rPixel.x < rSize.x && rPixel.y < rSize.y;
	return bPositive && bLessThanMax;
}

Vector2 Terrain3DEditor::getRotatedUv(
	const Vector2 &rUv,
	const real_t   fAngle ) const {
	const Vector2 rotation_offset = V2( 0.5f );
	Vector2		  uv = ( rUv - rotation_offset ).rotated( fAngle ) + rotation_offset;
	return uv.clamp( s_v2Zero, V2( 1.f ) );
}

// -----------------------------------------------------------------------------
// Protected functions

void Terrain3DEditor::setTerrain(
	Object *const pTerrain ) {
	Terrain3D *const pConcrete = Object::cast_to< Terrain3D >( pTerrain );
	if ( m_pTerrain == pConcrete ) {
		return;
	}
	// Restore an interrupted transaction while the old terrain owner still exists.
	if ( m_bIsOperating && m_pTerrain ) {
		cancelOperation();
	}
#if defined( T3D_EDITOR_NATIVE )
	if ( m_pTerrain ) {
		Terrain3D *const pPrevious = static_cast< Terrain3D * >( m_pTerrain );
		pPrevious->detachAuthoringHooks( this );
		if ( pPrevious->getEditor() == this ) {
			pPrevious->setEditor( NULL );
		}
	}
#endif
	m_pTerrain = pConcrete;
#if defined( T3D_EDITOR_NATIVE )
	if ( pConcrete ) {
		pConcrete->attachAuthoringHooks( this );
	}
#endif
}

void Terrain3DEditor::onTerrainOwnerDetached(
	void ) {
	// The owner has invalidated this non-owning attachment. Never invoke
	// its methods during editor teardown or after scene deletion.
	m_brushUndoRegions.clear();
	m_nPendingEditorBrushStrokeId = 0u;
	m_bPendingEditorBrushSealed = false;
	if ( ++m_nEditorBrushOwnerGeneration == 0u ) m_nEditorBrushOwnerGeneration = 1u;
	m_pTerrain = NULL;
	m_bIsOperating = false;
}

Object *Terrain3DEditor::getTerrain(
	void ) const {
	return m_pTerrain ? &m_pTerrain->sceneNode() : NULL;
}

Dictionary Terrain3DEditor::getBrushPerformanceStats(
	void ) const {
	Dictionary stats;
	stats["tool_selection_changes"] = ( int64_t ) m_nBrushToolSelections;
	stats["stroke_active"] = m_bIsOperating;
	stats["gpu_stroke_pending"] = m_nPendingEditorBrushStrokeId != 0u;
	stats["gpu_stroke_sealed"] = m_bPendingEditorBrushSealed;
	stats["dabs_this_stroke"] = ( int64_t ) m_nBrushDabs;
	stats["map_get_data_calls"] = ( int64_t ) m_nBrushGetDataCalls;
	stats["map_set_data_calls"] = ( int64_t ) m_nBrushSetDataCalls;
	stats["map_bytes_read"] = ( int64_t ) m_nBrushBytesRead;
	stats["map_bytes_published"] = ( int64_t ) m_nBrushBytesPublished;
	stats["prepare_usec"] = ( int64_t ) m_nBrushPrepareUsec;
	stats["cpu_sample_usec"] = ( int64_t ) m_nBrushCpuSampleUsec;
	stats["cpu_compute_usec"] = ( int64_t ) m_nBrushCpuComputeUsec;
	stats["commit_usec"] = ( int64_t ) m_nBrushCommitUsec;
	if ( m_pTerrain && m_pTerrain->dataObject() ) {
		stats["data_commit"] = m_pTerrain->dataObject()->getMapEditPerformanceStats();
	}
	if ( m_pTerrain && m_pTerrain->collisionObject() ) {
		stats["collision"] = m_pTerrain->collisionObject()->getStats();
	}
	return stats;
}

void Terrain3DEditor::_bind_methods(
	void ) {
	BIND_ENUM_CONSTANT( ADD );
	BIND_ENUM_CONSTANT( SUBTRACT );
	BIND_ENUM_CONSTANT( REPLACE );
	BIND_ENUM_CONSTANT( AVERAGE );
	BIND_ENUM_CONSTANT( GRADIENT );
	BIND_ENUM_CONSTANT( OP_MAX );

	BIND_ENUM_CONSTANT( SCULPT );
	BIND_ENUM_CONSTANT( HEIGHT );
	BIND_ENUM_CONSTANT( TEXTURE );
	BIND_ENUM_CONSTANT( COLOR );
	BIND_ENUM_CONSTANT( ROUGHNESS );
	BIND_ENUM_CONSTANT( ANGLE );
	BIND_ENUM_CONSTANT( SCALE );
	BIND_ENUM_CONSTANT( AUTOSHADER );
	BIND_ENUM_CONSTANT( HOLES );
	BIND_ENUM_CONSTANT( NAVIGATION );
	BIND_ENUM_CONSTANT( INSTANCER );
	BIND_ENUM_CONSTANT( LANDFORM );
	BIND_ENUM_CONSTANT( REGION );
	BIND_ENUM_CONSTANT( TOOL_MAX );

	ClassDB::bind_method( D_METHOD( "set_terrain", "terrain" ), &Terrain3DEditor::setTerrain );
	ClassDB::bind_method( D_METHOD( "get_terrain" ), &Terrain3DEditor::getTerrain );

	ClassDB::bind_method( D_METHOD( "set_brush_data", "data" ), &Terrain3DEditor::setBrushData );
	ClassDB::bind_method( D_METHOD( "set_tool", "tool" ), &Terrain3DEditor::setTool );
	ClassDB::bind_method( D_METHOD( "get_tool" ), &Terrain3DEditor::getTool );
	ClassDB::bind_method( D_METHOD( "get_brush_performance_stats" ), &Terrain3DEditor::getBrushPerformanceStats );
	ClassDB::bind_method( D_METHOD( "set_operation", "operation" ), &Terrain3DEditor::setOperation );
	ClassDB::bind_method( D_METHOD( "get_operation" ), &Terrain3DEditor::getOperation );
	ClassDB::bind_method( D_METHOD( "start_operation", "position" ), &Terrain3DEditor::startOperation );
	ClassDB::bind_method( D_METHOD( "is_operating" ), &Terrain3DEditor::isOperating );
	ClassDB::bind_method( D_METHOD( "operate", "position", "camera_direction" ), &Terrain3DEditor::operate );
	ClassDB::bind_method( D_METHOD( "backup_region", "region", "map_was_implicit" ),
						  &Terrain3DEditor::backupRegion,
						  DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "backup_region_full", "region" ), &Terrain3DEditor::backupRegionFull );
	ClassDB::bind_method( D_METHOD( "stop_operation" ), &Terrain3DEditor::stopOperation );
#if defined( T3D_EDITOR_NATIVE )
	ClassDB::bind_method( D_METHOD( "poll_editor_brush_commit" ), &Terrain3DEditor::pollEditorBrushCommitCold );
#endif
	ClassDB::bind_method( D_METHOD( "cancel_operation" ), &Terrain3DEditor::cancelOperation );

	ClassDB::bind_method( D_METHOD( "apply_undo", "data" ), &Terrain3DEditor::applyUndo );
}
